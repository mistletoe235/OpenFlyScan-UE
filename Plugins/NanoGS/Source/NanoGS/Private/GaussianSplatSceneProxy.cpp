// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatRenderData.h"
#include "Paged/NanoGSPagedRenderData.h"
#include "Paged/NanoGSCaptureReadiness.h"
#include "Paged/NanoGSPagedSourceAsset.h"
#include "Paged/NanoGSSpatialLODSourceAsset.h"
#include "Paged/NanoGSTreeSourceAsset.h"
#include "GaussianSplatViewExtension.h"
#include "NanoGSRHICompat.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "SceneView.h"
#include "SceneManagement.h"
#include "DynamicMeshBuilder.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Async/Async.h"

#include <limits>

namespace
{
	void RadixSortPackedSplatRefs(TArray<uint64>& Values)
	{
		if (Values.Num() < 2)
		{
			return;
		}

		TArray<uint64> Scratch;
		Scratch.SetNumUninitialized(Values.Num());
		TArray<uint64>* Source = &Values;
		TArray<uint64>* Destination = &Scratch;
		for (uint32 Pass = 0; Pass < sizeof(uint64); ++Pass)
		{
			uint32 Counts[256];
			uint32 Offsets[256];
			FMemory::Memzero(Counts, sizeof(Counts));
			const uint32 Shift = Pass * 8u;
			for (const uint64 Value : *Source)
			{
				++Counts[(Value >> Shift) & 0xffu];
			}
			uint32 Offset = 0;
			for (uint32 Bucket = 0; Bucket < UE_ARRAY_COUNT(Counts); ++Bucket)
			{
				Offsets[Bucket] = Offset;
				Offset += Counts[Bucket];
			}
			for (const uint64 Value : *Source)
			{
				(*Destination)[Offsets[(Value >> Shift) & 0xffu]++] = Value;
			}
			Swap(Source, Destination);
		}
	}


	bool BuildSortedPackedSplatRefs(
		const NanoGS::Tree::FManifest& Manifest,
		TConstArrayView<uint32> NodeIndices,
		TArray<uint64>& OutPackedRefs)
	{
		if (Manifest.Header.PagePoints == 0 || Manifest.Nodes.Num() <= 0)
		{
			return false;
		}

		TBitArray<> SelectedPageOutputs(false, Manifest.Nodes.Num());
		for (const uint32 NodeIndex : NodeIndices)
		{
			if (!Manifest.Nodes.IsValidIndex(static_cast<int32>(NodeIndex)))
			{
				return false;
			}
			const NanoGS::Tree::FNode& Node = Manifest.Nodes[NodeIndex];
			const uint64 PageOutputIndex =
				static_cast<uint64>(Node.PageId) * Manifest.Header.PagePoints + Node.PageLocal;
			if (PageOutputIndex >= static_cast<uint64>(Manifest.Nodes.Num()))
			{
				return false;
			}
			SelectedPageOutputs[static_cast<int32>(PageOutputIndex)] = true;
		}

		OutPackedRefs.Reset(NodeIndices.Num());
		OutPackedRefs.Reserve(NodeIndices.Num());
		for (TConstSetBitIterator<> It(SelectedPageOutputs); It; ++It)
		{
			const uint32 PageOutputIndex = static_cast<uint32>(It.GetIndex());
			const uint32 PageId = PageOutputIndex / Manifest.Header.PagePoints;
			const uint32 PageLocal = PageOutputIndex % Manifest.Header.PagePoints;
			OutPackedRefs.Add((static_cast<uint64>(PageId) << 32u) | PageLocal);
		}
		return OutPackedRefs.Num() == NodeIndices.Num();
	}


	void SortUniquePageIds(TArray<uint64>& PageIds)
	{
		PageIds.Sort();
		int32 WriteIndex = 0;
		for (const uint64 PageId : PageIds)
		{
			if (WriteIndex == 0 || PageIds[WriteIndex - 1] != PageId)
			{
				PageIds[WriteIndex++] = PageId;
			}
		}
		PageIds.SetNum(WriteIndex, EAllowShrinking::No);
	}

	bool BuildExplicitSplatFrontier(
		const NanoGS::Tree::FManifest& Manifest,
		TConstArrayView<uint32> NodeIndices,
		TArray<NanoGS::Paged::FExplicitSplatRef>& OutRefs,
		TArray<uint64>& OutPageIds,
		TArray<NanoGS::Paged::FExplicitSplatRun>& OutRuns,
		const TBitArray<>* PrecomputedPageOutputs = nullptr)
	{
		if (Manifest.Header.PagePoints == 0 || Manifest.Nodes.Num() <= 0)
		{
			return false;
		}

		TBitArray<> LocalPageOutputs;
		const TBitArray<>* SelectedPageOutputs = PrecomputedPageOutputs;
		if (SelectedPageOutputs == nullptr || SelectedPageOutputs->Num() != Manifest.Nodes.Num())
		{
			LocalPageOutputs.Init(false, Manifest.Nodes.Num());
			for (const uint32 NodeIndex : NodeIndices)
			{
				if (!Manifest.Nodes.IsValidIndex(static_cast<int32>(NodeIndex)))
				{
					return false;
				}
				const NanoGS::Tree::FNode& Node = Manifest.Nodes[NodeIndex];
				const uint64 PageOutputIndex =
					static_cast<uint64>(Node.PageId) * Manifest.Header.PagePoints + Node.PageLocal;
				if (PageOutputIndex >= static_cast<uint64>(Manifest.Nodes.Num()))
				{
					return false;
				}
				LocalPageOutputs[static_cast<int32>(PageOutputIndex)] = true;
			}
			SelectedPageOutputs = &LocalPageOutputs;
		}

		OutRefs.Reset(NodeIndices.Num());
		OutPageIds.Reset();
		OutRuns.Reset();
		OutRefs.Reserve(NodeIndices.Num());
		uint64 PreviousPageId = MAX_uint64;
		for (TConstSetBitIterator<> It(*SelectedPageOutputs); It; ++It)
		{
			const uint32 PageOutputIndex = static_cast<uint32>(It.GetIndex());
			const uint64 PageId = PageOutputIndex / Manifest.Header.PagePoints;
			const uint32 PageLocal = PageOutputIndex % Manifest.Header.PagePoints;
			OutRefs.Add({PageId, PageLocal});
			if (PageId != PreviousPageId)
			{
				OutPageIds.Add(PageId);
				PreviousPageId = PageId;
			}
			if (!OutRuns.IsEmpty() && OutRuns.Last().PageId == PageId &&
				OutRuns.Last().PageLocal + OutRuns.Last().Count == PageLocal)
			{
				++OutRuns.Last().Count;
			}
			else
			{
				OutRuns.Add({PageId, PageLocal, 1u});
			}
		}
		return OutRefs.Num() == NodeIndices.Num();
	}

	bool BuildTransitionRequestPages(
		const NanoGS::Tree::FManifest& Manifest,
		TConstArrayView<uint32> ParentIndices,
		TConstArrayView<uint32> TargetNodes,
		TConstArrayView<uint64> ResidentPageIds,
		const uint32 PageCapacity,
		TArray<uint64>& OutRequestedPageIds)
	{
		OutRequestedPageIds.Reset();
		if (TargetNodes.IsEmpty() || ParentIndices.Num() != Manifest.Nodes.Num() || PageCapacity == 0 ||
			Manifest.Header.RootNode >= static_cast<uint64>(Manifest.Nodes.Num()))
		{
			return false;
		}

		TSet<uint64> TargetPages;
		TMap<uint64, uint16> MinimumDepthByPage;
		TBitArray<> Visited(false, Manifest.Nodes.Num());
		for (const uint32 TargetNode : TargetNodes)
		{
			if (!Manifest.Nodes.IsValidIndex(static_cast<int32>(TargetNode)))
			{
				return false;
			}
			TargetPages.Add(Manifest.Nodes[TargetNode].PageId);
			uint32 NodeIndex = TargetNode;
			while (NodeIndex != MAX_uint32 && !Visited[NodeIndex])
			{
				Visited[NodeIndex] = true;
				const NanoGS::Tree::FNode& Node = Manifest.Nodes[NodeIndex];
				uint16& MinimumDepth = MinimumDepthByPage.FindOrAdd(Node.PageId, MAX_uint16);
				MinimumDepth = FMath::Min(MinimumDepth, Node.Depth);
				NodeIndex = ParentIndices[NodeIndex];
			}
		}
		if (TargetPages.Num() > static_cast<int32>(PageCapacity))
		{
			OutRequestedPageIds = TargetPages.Array();
			SortUniquePageIds(OutRequestedPageIds);
			return false;
		}

		const uint64 RootPageId = Manifest.Nodes[static_cast<uint32>(Manifest.Header.RootNode)].PageId;
		TSet<uint64> SelectedPages = TargetPages;
		if (!SelectedPages.Contains(RootPageId))
		{
			if (SelectedPages.Num() >= static_cast<int32>(PageCapacity))
			{
				OutRequestedPageIds = TargetPages.Array();
				SortUniquePageIds(OutRequestedPageIds);
				return false;
			}
			SelectedPages.Add(RootPageId);
		}

		TSet<uint64> ResidentPages;
		ResidentPages.Reserve(ResidentPageIds.Num());
		for (const uint64 PageId : ResidentPageIds)
		{
			ResidentPages.Add(PageId);
		}
		struct FPagePriority
		{
			uint64 PageId = 0;
			uint16 MinimumDepth = MAX_uint16;
			bool bTarget = false;
			bool bResident = false;
		};
		TArray<FPagePriority> Candidates;
		Candidates.Reserve(MinimumDepthByPage.Num());
		for (const TPair<uint64, uint16>& Pair : MinimumDepthByPage)
		{
			Candidates.Add({Pair.Key, Pair.Value, TargetPages.Contains(Pair.Key), ResidentPages.Contains(Pair.Key)});
		}
		Candidates.Sort([RootPageId](const FPagePriority& A, const FPagePriority& B)
		{
			if ((A.PageId == RootPageId) != (B.PageId == RootPageId))
			{
				return A.PageId == RootPageId;
			}
			if (A.bResident != B.bResident)
			{
				return A.bResident;
			}
			if (A.MinimumDepth != B.MinimumDepth)
			{
				return A.MinimumDepth < B.MinimumDepth;
			}
			return A.PageId < B.PageId;
		});

		for (const FPagePriority& Candidate : Candidates)
		{
			if (!SelectedPages.Contains(Candidate.PageId) &&
				SelectedPages.Num() < static_cast<int32>(PageCapacity))
			{
				SelectedPages.Add(Candidate.PageId);
			}
		}
		OutRequestedPageIds.Reserve(SelectedPages.Num());
		for (const FPagePriority& Candidate : Candidates)
		{
			if (SelectedPages.Contains(Candidate.PageId))
			{
				OutRequestedPageIds.Add(Candidate.PageId);
			}
		}
		if (OutRequestedPageIds.Num() != SelectedPages.Num())
		{
			TArray<uint64> MissingPages = SelectedPages.Array();
			SortUniquePageIds(MissingPages);
			for (const uint64 PageId : MissingPages)
			{
				if (!OutRequestedPageIds.Contains(PageId))
				{
					OutRequestedPageIds.Add(PageId);
				}
			}
		}
		return true;
	}

	class FRefinedResidentFrontierResolver
	{
	public:
		FRefinedResidentFrontierResolver(
			const NanoGS::Tree::FManifest& InManifest,
			const TBitArray<>& InRefinedNodes,
			const TBitArray<>& InDemandedNodes,
			const TBitArray<>& InRenderablePages,
			TArray<uint32>& InOutRenderNodes)
			: Manifest(InManifest)
			, RefinedNodes(InRefinedNodes)
			, DemandedNodes(InDemandedNodes)
			, RenderablePages(InRenderablePages)
			, OutRenderNodes(InOutRenderNodes)
		{
		}

		bool Resolve(const uint32 NodeIndex)
		{
			if (!DemandedNodes[NodeIndex])
			{
				return true;
			}
			if (!RefinedNodes[NodeIndex])
			{
				if (!IsResidentAndProtected(NodeIndex))
				{
					return false;
				}
				OutRenderNodes.Add(NodeIndex);
				return true;
			}

			const NanoGS::Tree::FNode& Node = Manifest.Nodes[NodeIndex];
			const int32 PreviousOutputCount = OutRenderNodes.Num();
			for (uint32 Offset = 0; Offset < Node.ChildCount; ++Offset)
			{
				const uint32 ChildIndex = Node.FirstChild + Offset;
				if (!DemandedNodes[ChildIndex])
				{
					continue;
				}
				if (!Resolve(ChildIndex))
				{
					OutRenderNodes.SetNum(PreviousOutputCount, EAllowShrinking::No);
					if (!IsResidentAndProtected(NodeIndex))
					{
						return false;
					}
					OutRenderNodes.Add(NodeIndex);
					return true;
				}
			}
			return true;
		}

	private:
		bool IsResidentAndProtected(const uint32 NodeIndex) const
		{
			const uint32 PageId = Manifest.Nodes[NodeIndex].PageId;
			return RenderablePages.IsValidIndex(static_cast<int32>(PageId)) && RenderablePages[PageId];
		}

		const NanoGS::Tree::FManifest& Manifest;
		const TBitArray<>& RefinedNodes;
		const TBitArray<>& DemandedNodes;
		const TBitArray<>& RenderablePages;
		TArray<uint32>& OutRenderNodes;
	};

	bool ResolveRefinedResidentFrontier(
		const NanoGS::Tree::FManifest& Manifest,
		const TBitArray<>& RefinedNodes,
		const TBitArray<>& DemandedNodes,
		TConstArrayView<uint64> ResidentPageIds,
		TConstArrayView<uint64> ProtectedPageIds,
		TArray<uint32>& OutRenderNodes)
	{
		OutRenderNodes.Reset();
		if (Manifest.Nodes.IsEmpty() ||
			RefinedNodes.Num() != Manifest.Nodes.Num() ||
			DemandedNodes.Num() != Manifest.Nodes.Num() ||
			Manifest.Header.RootNode >= static_cast<uint64>(Manifest.Nodes.Num()))
		{
			return false;
		}

		uint32 MaxPageId = 0;
		for (const uint64 PageId : ProtectedPageIds)
		{
			if (PageId <= MAX_uint32)
			{
				MaxPageId = FMath::Max(MaxPageId, static_cast<uint32>(PageId));
			}
		}
		TBitArray<> ProtectedPages(false, static_cast<int32>(MaxPageId) + 1);
		for (const uint64 PageId : ProtectedPageIds)
		{
			if (PageId <= MaxPageId)
			{
				ProtectedPages[static_cast<int32>(PageId)] = true;
			}
		}
		TBitArray<> RenderablePages(false, ProtectedPages.Num());
		for (const uint64 PageId : ResidentPageIds)
		{
			if (PageId < static_cast<uint64>(RenderablePages.Num()) &&
				ProtectedPages[static_cast<int32>(PageId)])
			{
				RenderablePages[static_cast<int32>(PageId)] = true;
			}
		}

		FRefinedResidentFrontierResolver Resolver(
			Manifest, RefinedNodes, DemandedNodes, RenderablePages, OutRenderNodes);
		return Resolver.Resolve(static_cast<uint32>(Manifest.Header.RootNode));
	}
}

static TAutoConsoleVariable<int32> CVarNanoGSSpatialLODGlobalBudget(
	TEXT("gs.SpatialLODGlobalBudget"),
	0,
	TEXT("Soft global SpatialLOD splat budget. 0 preserves the legacy selector. Positive values only release certified-safe hysteresis redundancy; quality safety may exceed the budget."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSSpatialLODSafeTransitions(
	TEXT("gs.SpatialLODSafeTransitions"),
	0,
	TEXT("Enable two-phase lossless SpatialLOD switching. Mixed changes publish required refinements first, then coarsen while captures keep using the old finer frontier."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeLOD(
	TEXT("gs.TreeLOD"),
	0,
	TEXT("Enable experimental Spark-style Tree v2 runtime selection. Default 0 keeps every legacy source path unchanged."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNanoGSTreeLODSplitPixels(
	TEXT("gs.TreeLODSplitPixels"),
	0.5f,
	TEXT("Spark-compatible projected feature size in pixels above which a Tree v2 parent is refined (default 0.5 for near-LOD0 quality)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeLODMaxActive(
	TEXT("gs.TreeLODMaxActive"),
	8000000,
	TEXT("Maximum regular-view Tree v2 frontier nodes. Default 8M avoids the visibly blurry legacy 1M cap; captures ignore this budget and require Exact Leaves."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeLODFrustumCull(
	TEXT("gs.TreeLODFrustumCull"),
	0,
	TEXT("Cull Tree v2 nodes during LoD traversal. Default 0 matches Spark, whose LoD heap is distance-only."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNanoGSTreeLODUpdateInterval(
	TEXT("gs.TreeLODUpdateInterval"),
	0.15f,
	TEXT("Minimum seconds between Tree v2 view-dependent selections (default 0.15)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNanoGSTreeSelectorCancelMinAge(
	TEXT("gs.TreeSelectorCancelMinAge"),
	0.15f,
	TEXT("Minimum age in seconds before camera motion may cancel an in-flight Tree selector.\n")
	TEXT("Coalesces rapid view updates without changing the selected frontier or final quality.\n")
	TEXT("Projection/configuration changes still cancel immediately. 0 restores legacy behavior."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNanoGSTreeLODMovementThreshold(
	TEXT("gs.TreeLODMovementThreshold"),
	100.0f,
	TEXT("Camera movement in centimeters that triggers a Tree v2 selection (default 100 cm)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNanoGSTreeLODDirectionDegrees(
	TEXT("gs.TreeLODDirectionDegrees"),
	0.5f,
	TEXT("Camera direction change in degrees that triggers a Tree v2 selection (default 0.5)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeGPUExclusiveSelector(
	TEXT("gs.TreeGPUExclusiveSelector"),
	1,
	TEXT("When GPU Tree v3 resources are active, keep one CPU bootstrap/fallback but do not run the regular-view CPU selector in parallel (default 1)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeGPUExperimentalSelector(
	TEXT("gs.TreeGPUExperimentalSelector"),
	0,
	TEXT("Allow the experimental multi-pass Tree v3 GPU selector to own regular views. Default 0 uses the exact one-pass CPU selector; 1 is diagnostic only."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeSelectorHierarchicalHeap(
	TEXT("gs.TreeSelectorHierarchicalHeap"),
	1,
	TEXT("Use the exact full-float hierarchical priority heap for the CPU Tree selector. 0 restores the reference global binary heap."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeProgressiveFirstBudget(
	TEXT("gs.TreeProgressiveFirstBudget"),
	-1,
	TEXT("Optional global override for the component's Moving Progressive Budget. -1 uses the Details value; 0 disables it."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeResidentProgressiveMinMissingPages(
	TEXT("gs.TreeResidentProgressiveMinMissingPages"),
	64,
	TEXT("Minimum still-missing target pages required for another resident-safe local publication (default 64)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeResidentProgressiveFullTopology(
	TEXT("gs.TreeResidentProgressiveFullTopology"),
	0,
	TEXT("Experimental repeated resident-safe publication topology. 0 tracks only the fast first checkpoint; 1 retains full split topology (default 0)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSTreeResidentProgressiveMinSplatGain(
	TEXT("gs.TreeResidentProgressiveMinSplatGain"),
	250000,
	TEXT("Minimum resident-safe splat increase over the displayed frontier before local publication (default 250,000)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNanoGSTreeGPUExclusiveBootstrapTimeout(
	TEXT("gs.TreeGPUExclusiveBootstrapTimeout"),
	0.0f,
	TEXT("Optional seconds before allowing another CPU fallback while the first GPU frontier is still accepted. 0 waits indefinitely and falls back only when GPU advancement fails (default 0)."),
	ECVF_RenderThreadSafe);

//////////////////////////////////////////////////////////////////////////
// FGaussianSplatGPUResources

FGaussianSplatGPUResources::FGaussianSplatGPUResources()
{
}

FGaussianSplatGPUResources::~FGaussianSplatGPUResources()
{
}

void FGaussianSplatGPUResources::Initialize(UGaussianSplatAsset* Asset)
{
	if (!Asset || !Asset->IsValid())
	{
		return;
	}

	// Get or create shared render data (CPU-side cached data, loaded once per asset)
	SharedData = Asset->GetOrCreateRenderData();
	if (!SharedData.IsValid() || !SharedData->IsInitialized())
	{
		return;
	}

	// Copy metadata from shared render data
	SplatCount = SharedData->SplatCount;
	PositionFormat = SharedData->PositionFormat;
	PackedPositionOrigin = SharedData->PackedPositionOrigin;
	bHasFloat32PositionData = SharedData->bHasFloat32PositionData;
	bHasFloat32RotationScaleData = SharedData->bHasFloat32RotationScaleData;
	bUsesDedicatedColorOpacityBuffer = SharedData->bUsesDedicatedColorOpacityBuffer;
	bHasPackedGeometryData = SharedData->bHasPackedGeometryData;
	SHBands = SharedData->SHBands;
	bEnableNanite = SharedData->bEnableNanite;
	bHasClusterData = SharedData->bHasClusterData;
	ClusterCount = SharedData->ClusterCount;
	LeafClusterCount = SharedData->LeafClusterCount;
	LODSplatCount = SharedData->LODSplatCount;
	bHasLODSplats = SharedData->bHasLODSplats;

	// Initialize render resource
	if (!bInitialized)
	{
		InitResource(FRHICommandListImmediate::Get());
		bInitialized = true;
	}
}

void FGaussianSplatGPUResources::Initialize(
	const TSharedPtr<NanoGS::Paged::FPageRenderData, ESPMode::ThreadSafe>& InPagedData)
{
	if (!InPagedData.IsValid() || !InPagedData->IsInitialized())
	{
		return;
	}

	PagedSharedData = InPagedData;
	// Renderer allocation/indexing follows the fixed physical source pool. In
	// bounded complete-frontier mode this is intentionally smaller than asset total.
	SplatCount = static_cast<int32>(PagedSharedData->GetSourceCapacity());
	PositionFormat = EGaussianPositionFormat::Float32;
	PackedPositionOrigin = FVector3f::ZeroVector;
	bHasFloat32PositionData = true;
	bHasFloat32RotationScaleData = true;
	bUsesDedicatedColorOpacityBuffer = true;
	bHasPackedGeometryData = false;
	SHBands = 3;
	bEnableNanite = false;
	bHasClusterData = false;
	ClusterCount = 0;
	LeafClusterCount = 0;
	LODSplatCount = 0;
	bHasLODSplats = false;
	bIsPagedSource = true;

	if (!bInitialized)
	{
		InitResource(FRHICommandListImmediate::Get());
		bInitialized = true;
	}
}

uint32 FGaussianSplatGPUResources::GetActiveSplatCount(const bool bCaptureFrontier) const
{
	return PagedSharedData.IsValid()
		? PagedSharedData->GetActiveSplatCount(bCaptureFrontier)
		: static_cast<uint32>(FMath::Max(SplatCount, 0));
}

uint32 FGaussianSplatGPUResources::GetSourceSplatCapacity() const
{
	return PagedSharedData.IsValid()
		? PagedSharedData->GetSourceCapacity()
		: static_cast<uint32>(FMath::Max(SplatCount - LODSplatCount, 0));
}

uint32 FGaussianSplatGPUResources::GetResidentGeneration(const bool bCaptureFrontier) const
{
	return PagedSharedData.IsValid() ? PagedSharedData->GetResidentGeneration(bCaptureFrontier) : 0u;
}

bool FGaussianSplatGPUResources::IsPagedUploadComplete() const
{
	return !PagedSharedData.IsValid() || PagedSharedData->IsUploadComplete();
}

bool FGaussianSplatGPUResources::IsBoundedPagedStreamingEnabled() const
{
	return PagedSharedData.IsValid() && PagedSharedData->IsBoundedStreamingEnabled();
}

void FGaussianSplatGPUResources::RefreshPagedFrontierBinding_RenderThread(const bool bCaptureFrontier)
{
	check(IsInRenderingThread());
	if (!PagedSharedData.IsValid() || !PagedSharedData->AreGPUBuffersCreated())
	{
		return;
	}
	CompactedSplatIndicesBuffer =
		PagedSharedData->GetPublishedActivePhysicalIndexBuffer_RenderThread(bCaptureFrontier);
	CompactedSplatIndicesBufferSRV =
		PagedSharedData->GetPublishedActivePhysicalIndexBufferSRV_RenderThread(bCaptureFrontier);
}

void FGaussianSplatGPUResources::NotifyPagedFrontierDrawn_RenderThread(
	FRHICommandListImmediate& RHICmdList, const bool bCaptureFrontier)
{
	if (PagedSharedData.IsValid())
	{
		PagedSharedData->NotifyFrontierDrawn_RenderThread(RHICmdList, bCaptureFrontier);
	}
}

void FGaussianSplatGPUResources::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (SplatCount <= 0 || (!SharedData.IsValid() && !PagedSharedData.IsValid()))
	{
		return;
	}

	if (PagedSharedData.IsValid())
	{
		FString Error;
		if (!PagedSharedData->CreateGPUBuffers(RHICmdList, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("NanoGS Page GPU pool initialization failed: %s"), *Error);
			return;
		}

		PackedSplatBuffer = PagedSharedData->PackedSplatBuffer;
		PackedSplatBufferSRV = PagedSharedData->PackedSplatBufferSRV;
		ColorOpacityBuffer = PagedSharedData->ColorOpacityBuffer;
		ColorOpacityBufferSRV = PagedSharedData->ColorOpacityBufferSRV;
		PositionBuffer = PagedSharedData->PositionBuffer;
		PositionBufferSRV = PagedSharedData->PositionBufferSRV;
		OtherDataBuffer = PagedSharedData->OtherDataBuffer;
		OtherDataBufferSRV = PagedSharedData->OtherDataBufferSRV;
		SHBuffer = PagedSharedData->SHBuffer;
		SHBufferSRV = PagedSharedData->SHBufferSRV;
		IndexBuffer = PagedSharedData->IndexBuffer;
		SplatClusterIndexBuffer = PagedSharedData->SplatClusterIndexBuffer;
		SplatClusterIndexBufferSRV = PagedSharedData->SplatClusterIndexBufferSRV;

		TotalSplatCount = static_cast<int32>(PagedSharedData->GetSourceCapacity());
		CreatePerInstanceBuffers(RHICmdList);

		// The no-cluster setup above creates mandatory shader-binding dummies.
		// Replace only the compaction input with the residency-authored active list.
		CompactedSplatIndicesBuffer.SafeRelease();
		CompactedSplatIndicesBufferUAV.SafeRelease();
		CompactedSplatIndicesBufferSRV.SafeRelease();
		RefreshPagedFrontierBinding_RenderThread();
		bHasPrecompactedActiveList = true;

		UE_LOG(LogTemp, Log,
			TEXT("NanoGS Page GPU source pool ready: %u physical-splat capacity, Exact-HQ Float32/SH3, active pages publish asynchronously"),
			PagedSharedData->GetSourceCapacity());
		return;
	}

	// Create shared GPU buffers (only runs once per asset, thread-safe)
	SharedData->CreateGPUBuffers(RHICmdList);

	// Copy shared GPU buffer refs (just ref-count increments, no GPU allocation)
	PackedSplatBuffer = SharedData->PackedSplatBuffer;
	PackedSplatBufferSRV = SharedData->PackedSplatBufferSRV;
	ColorOpacityBuffer = SharedData->ColorOpacityBuffer;
	ColorOpacityBufferSRV = SharedData->ColorOpacityBufferSRV;
	PositionBuffer = SharedData->PositionBuffer;
	PositionBufferSRV = SharedData->PositionBufferSRV;
	OtherDataBuffer = SharedData->OtherDataBuffer;
	OtherDataBufferSRV = SharedData->OtherDataBufferSRV;
	SHBuffer = SharedData->SHBuffer;
	SHBufferSRV = SharedData->SHBufferSRV;
	ChunkBuffer = SharedData->ChunkBuffer;
	ChunkBufferSRV = SharedData->ChunkBufferSRV;
	IndexBuffer = SharedData->IndexBuffer;
	ClusterBuffer = SharedData->ClusterBuffer;
	ClusterBufferSRV = SharedData->ClusterBufferSRV;
	SplatClusterIndexBuffer = SharedData->SplatClusterIndexBuffer;
	SplatClusterIndexBufferSRV = SharedData->SplatClusterIndexBufferSRV;

	// TotalSplatCount is needed by per-instance buffer sizing
	TotalSplatCount = SplatCount;

	// Create per-instance buffers (cluster visibility, compaction, sort args, etc.)
	CreatePerInstanceBuffers(RHICmdList);

	int32 PerInstanceBufferCount = 0;
	if (bSupportsCompaction) PerInstanceBufferCount += 15; // cluster/compaction/sort buffers
	if (bSupportsIndirectDraw) PerInstanceBufferCount += 1;
	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created %d per-instance buffers (shared data: %s)"),
		PerInstanceBufferCount, *SharedData->GetAssetName());
}

void FGaussianSplatGPUResources::ReleaseRHI()
{
	// Release shared buffer refs (just ref-count decrements, actual GPU memory
	// is freed when SharedData releases its refs)
	PackedSplatBuffer.SafeRelease();
	PackedSplatBufferSRV.SafeRelease();
	ColorOpacityBuffer.SafeRelease();
	ColorOpacityBufferSRV.SafeRelease();
	SHBuffer.SafeRelease();
	SHBufferSRV.SafeRelease();
	ChunkBuffer.SafeRelease();
	ChunkBufferSRV.SafeRelease();
	IndexBuffer.SafeRelease();
	ClusterBuffer.SafeRelease();
	ClusterBufferSRV.SafeRelease();
	SplatClusterIndexBuffer.SafeRelease();
	SplatClusterIndexBufferSRV.SafeRelease();

	// Release shared high-precision geometry buffer refs.
	PositionBuffer.SafeRelease();
	PositionBufferSRV.SafeRelease();
	OtherDataBuffer.SafeRelease();
	OtherDataBufferSRV.SafeRelease();

	// Release per-instance buffers
	ViewDataBuffer.SafeRelease();
	ViewDataBufferUAV.SafeRelease();
	ViewDataBufferSRV.SafeRelease();
	SortDistanceBuffer.SafeRelease();
	SortDistanceBufferUAV.SafeRelease();
	SortDistanceBufferSRV.SafeRelease();
	SortKeysBuffer.SafeRelease();
	SortKeysBufferUAV.SafeRelease();
	SortKeysBufferSRV.SafeRelease();
	SortKeysBufferAlt.SafeRelease();
	SortKeysBufferAltUAV.SafeRelease();
	SortKeysBufferAltSRV.SafeRelease();
	SortDistanceBufferAlt.SafeRelease();
	SortDistanceBufferAltUAV.SafeRelease();
	RadixHistogramBuffer.SafeRelease();
	RadixHistogramBufferUAV.SafeRelease();
	RadixDigitOffsetBuffer.SafeRelease();
	RadixDigitOffsetBufferUAV.SafeRelease();
	SortIndirectArgsBuffer.SafeRelease();
	SortIndirectArgsBufferUAV.SafeRelease();
	SortParamsBuffer.SafeRelease();
	SortParamsBufferUAV.SafeRelease();
	SortParamsBufferSRV.SafeRelease();
	VisibleClusterBuffer.SafeRelease();
	VisibleClusterBufferUAV.SafeRelease();
	VisibleClusterBufferSRV.SafeRelease();
	VisibleClusterCountBuffer.SafeRelease();
	VisibleClusterCountBufferUAV.SafeRelease();
	VisibleClusterCountBufferSRV.SafeRelease();

	// Release LOD splat buffers
	LODSplatBuffer.SafeRelease();
	LODSplatBufferSRV.SafeRelease();

	// Release indirect draw buffers
	IndirectDrawArgsBuffer.SafeRelease();
	IndirectDrawArgsBufferUAV.SafeRelease();

	// Release cluster visibility integration buffers
	ClusterVisibilityBitmap.SafeRelease();
	ClusterVisibilityBitmapUAV.SafeRelease();
	ClusterVisibilityBitmapSRV.SafeRelease();
	SelectedClusterBuffer.SafeRelease();
	SelectedClusterBufferUAV.SafeRelease();
	SelectedClusterBufferSRV.SafeRelease();

	// Release LOD cluster tracking buffers
	LODClusterBuffer.SafeRelease();
	LODClusterBufferUAV.SafeRelease();
	LODClusterBufferSRV.SafeRelease();
	LODClusterCountBuffer.SafeRelease();
	LODClusterCountBufferUAV.SafeRelease();
	LODClusterCountBufferSRV.SafeRelease();
	LODClusterSelectedBitmap.SafeRelease();
	LODClusterSelectedBitmapUAV.SafeRelease();
	LODClusterSelectedBitmapSRV.SafeRelease();
	LODSplatTotalBuffer.SafeRelease();
	LODSplatTotalBufferUAV.SafeRelease();
	LODSplatTotalBufferSRV.SafeRelease();

	// Release GPU-driven LOD rendering buffers
	LODSplatClusterIndexBuffer.SafeRelease();
	LODSplatClusterIndexBufferSRV.SafeRelease();
	LODSplatOutputCountBuffer.SafeRelease();
	LODSplatOutputCountBufferUAV.SafeRelease();
	LODSplatOutputCountBufferSRV.SafeRelease();

	// Release splat compaction buffers
	CompactedSplatIndicesBuffer.SafeRelease();
	CompactedSplatIndicesBufferUAV.SafeRelease();
	CompactedSplatIndicesBufferSRV.SafeRelease();
	VisibleSplatCountBuffer.SafeRelease();
	VisibleSplatCountBufferUAV.SafeRelease();
	VisibleSplatCountBufferSRV.SafeRelease();
	IndirectDispatchArgsBuffer.SafeRelease();
	IndirectDispatchArgsBufferUAV.SafeRelease();

	// Release shared data reference
	SharedData.Reset();
	PagedSharedData.Reset();

	bInitialized = false;
	bHasPrecompactedActiveList = false;
	bIsPagedSource = false;
}

void FGaussianSplatGPUResources::CreatePerInstanceBuffers(FRHICommandListBase& RHICmdList)
{
	// When Nanite is disabled (no cluster data), create dummy buffers for shader binding
	if (!bHasClusterData)
	{
		// Dummy ClusterVisibilityBitmap (1 uint)
		{
			const uint32 BufferSize = sizeof(uint32);
			ClusterVisibilityBitmap = NanoGS::RHICompat::CreateBuffer(
				RHICmdList,
				TEXT("GaussianClusterVisibilityBitmapDummy"),
				BufferSize,
				sizeof(uint32),
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
				ERHIAccess::SRVMask);

			uint32 Zero = 0;
			void* Data = RHICmdList.LockBuffer(ClusterVisibilityBitmap, 0, BufferSize, RLM_WriteOnly);
			FMemory::Memcpy(Data, &Zero, BufferSize);
			RHICmdList.UnlockBuffer(ClusterVisibilityBitmap);

			ClusterVisibilityBitmapSRV = RHICmdList.CreateShaderResourceView(
				ClusterVisibilityBitmap, FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(uint32)));
		}

		// Dummy LODClusterSelectedBitmap (1 uint)
		{
			const uint32 BufferSize = sizeof(uint32);
			LODClusterSelectedBitmap = NanoGS::RHICompat::CreateBuffer(
				RHICmdList,
				TEXT("GaussianLODClusterSelectedBitmapDummy"),
				BufferSize,
				sizeof(uint32),
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
				ERHIAccess::SRVMask);

			uint32 Zero = 0;
			void* Data = RHICmdList.LockBuffer(LODClusterSelectedBitmap, 0, BufferSize, RLM_WriteOnly);
			FMemory::Memcpy(Data, &Zero, BufferSize);
			RHICmdList.UnlockBuffer(LODClusterSelectedBitmap);

			LODClusterSelectedBitmapSRV = RHICmdList.CreateShaderResourceView(
				LODClusterSelectedBitmap, FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(uint32)));
		}

		// Dummy SelectedClusterBuffer (1 uint)
		{
			const uint32 BufferSize = sizeof(uint32);
			SelectedClusterBuffer = NanoGS::RHICompat::CreateBuffer(
				RHICmdList,
				TEXT("GaussianSelectedClusterBufferDummy"),
				BufferSize,
				sizeof(uint32),
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
				ERHIAccess::SRVMask);

			uint32 Zero = 0;
			void* Data = RHICmdList.LockBuffer(SelectedClusterBuffer, 0, BufferSize, RLM_WriteOnly);
			FMemory::Memcpy(Data, &Zero, BufferSize);
			RHICmdList.UnlockBuffer(SelectedClusterBuffer);

			SelectedClusterBufferSRV = RHICmdList.CreateShaderResourceView(
				SelectedClusterBuffer, FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(uint32)));
		}

		// Dummy CompactedSplatIndicesBuffer (1 uint) - needed even when UseCompaction=0
		{
			const uint32 BufferSize = sizeof(uint32);
			CompactedSplatIndicesBuffer = NanoGS::RHICompat::CreateBuffer(
				RHICmdList,
				TEXT("GaussianCompactedSplatIndicesBufferDummy"),
				BufferSize,
				sizeof(uint32),
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
				ERHIAccess::SRVMask);

			uint32 Zero = 0;
			void* Data = RHICmdList.LockBuffer(CompactedSplatIndicesBuffer, 0, BufferSize, RLM_WriteOnly);
			FMemory::Memcpy(Data, &Zero, BufferSize);
			RHICmdList.UnlockBuffer(CompactedSplatIndicesBuffer);

			CompactedSplatIndicesBufferSRV = RHICmdList.CreateShaderResourceView(
				CompactedSplatIndicesBuffer, FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(uint32)));
		}

		return;
	}

	// Create visible cluster buffer (dynamic, written by culling shader)
	// Size = max possible visible clusters (all clusters could be visible)
	{
		const uint32 BufferSize = ClusterCount * sizeof(uint32);
		VisibleClusterBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianVisibleClusterBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		VisibleClusterBufferUAV = RHICmdList.CreateUnorderedAccessView(
			VisibleClusterBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		VisibleClusterBufferSRV = RHICmdList.CreateShaderResourceView(
			VisibleClusterBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// Create visible cluster count buffer (single uint, atomic counter)
	{
		const uint32 BufferSize = sizeof(uint32);
		VisibleClusterCountBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianVisibleClusterCountBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		VisibleClusterCountBufferUAV = RHICmdList.CreateUnorderedAccessView(
			VisibleClusterCountBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		VisibleClusterCountBufferSRV = RHICmdList.CreateShaderResourceView(
			VisibleClusterCountBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// Create indirect draw argument buffer for GPU-driven rendering
	// Structure: IndexCountPerInstance(4), InstanceCount(4), StartIndexLocation(4), BaseVertexLocation(4), StartInstanceLocation(4)
	// Total: 20 bytes, but we use 32 bytes for alignment
	{
		const uint32 BufferSize = 32;  // 5 uints + padding for alignment
		IndirectDrawArgsBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianIndirectDrawArgsBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer,
			ERHIAccess::IndirectArgs);

		// Initialize with default values
		// IndexCountPerInstance = 6 (2 triangles per quad)
		// InstanceCount = SplatCount (will be updated by culling shader)
		// StartIndexLocation = 0
		// BaseVertexLocation = 0
		// StartInstanceLocation = 0
		uint32 InitData[8] = { 6, (uint32)SplatCount, 0, 0, 0, 0, 0, 0 };
		void* Data = RHICmdList.LockBuffer(IndirectDrawArgsBuffer, 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, InitData, BufferSize);
		RHICmdList.UnlockBuffer(IndirectDrawArgsBuffer);

		IndirectDrawArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			IndirectDrawArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));

		bSupportsIndirectDraw = true;
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created indirect draw buffer"));
	}

	// Create cluster visibility bitmap buffer
	// One bit per cluster, rounded up to uint32 boundary
	{
		uint32 BitmapSize = FMath::DivideAndRoundUp(ClusterCount, 32) * sizeof(uint32);
		BitmapSize = FMath::Max(BitmapSize, (uint32)sizeof(uint32));  // At least one uint32

		ClusterVisibilityBitmap = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianClusterVisibilityBitmap"),
			BitmapSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		ClusterVisibilityBitmapUAV = RHICmdList.CreateUnorderedAccessView(
			ClusterVisibilityBitmap, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		ClusterVisibilityBitmapSRV = RHICmdList.CreateShaderResourceView(
			ClusterVisibilityBitmap, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));

		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created cluster visibility bitmap (%d bytes for %d clusters)"), BitmapSize, ClusterCount);
	}

	// Create selected cluster buffer for Nanite-style debug visualization
	// One entry per leaf cluster, stores which cluster ID is selected based on LOD
	{
		uint32 BufferSize = LeafClusterCount * sizeof(uint32);
		BufferSize = FMath::Max(BufferSize, (uint32)sizeof(uint32));  // At least one uint32

		SelectedClusterBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianSelectedClusterBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		SelectedClusterBufferUAV = RHICmdList.CreateUnorderedAccessView(
			SelectedClusterBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SelectedClusterBufferSRV = RHICmdList.CreateShaderResourceView(
			SelectedClusterBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));

		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created selected cluster buffer (%d bytes for %d leaf clusters)"), BufferSize, LeafClusterCount);
	}

	// Create LOD cluster tracking buffers for LOD rendering
	// LOD cluster buffer - stores unique parent cluster indices
	{
		uint32 BufferSize = ClusterCount * sizeof(uint32);
		LODClusterBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianLODClusterBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		LODClusterBufferUAV = RHICmdList.CreateUnorderedAccessView(
			LODClusterBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODClusterBufferSRV = RHICmdList.CreateShaderResourceView(
			LODClusterBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// LOD cluster count buffer (atomic counter)
	{
		uint32 BufferSize = sizeof(uint32);
		LODClusterCountBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianLODClusterCountBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		LODClusterCountBufferUAV = RHICmdList.CreateUnorderedAccessView(
			LODClusterCountBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODClusterCountBufferSRV = RHICmdList.CreateShaderResourceView(
			LODClusterCountBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// LOD cluster selected bitmap (same size as cluster visibility bitmap)
	// Also needs SRV for GPU-driven LOD shader to read
	{
		uint32 BitmapSize = FMath::DivideAndRoundUp(ClusterCount, 32) * sizeof(uint32);
		BitmapSize = FMath::Max(BitmapSize, (uint32)sizeof(uint32));

		LODClusterSelectedBitmap = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianLODClusterSelectedBitmap"),
			BitmapSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		LODClusterSelectedBitmapUAV = RHICmdList.CreateUnorderedAccessView(
			LODClusterSelectedBitmap, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODClusterSelectedBitmapSRV = RHICmdList.CreateShaderResourceView(
			LODClusterSelectedBitmap, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// LOD splat total buffer (atomic counter for total LOD splats)
	{
		uint32 BufferSize = sizeof(uint32);
		LODSplatTotalBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianLODSplatTotalBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		LODSplatTotalBufferUAV = RHICmdList.CreateUnorderedAccessView(
			LODSplatTotalBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODSplatTotalBufferSRV = RHICmdList.CreateShaderResourceView(
			LODSplatTotalBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// LOD splat output count buffer (atomic counter for valid LOD splat output)
	{
		uint32 BufferSize = sizeof(uint32);
		LODSplatOutputCountBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianLODSplatOutputCountBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		LODSplatOutputCountBufferUAV = RHICmdList.CreateUnorderedAccessView(
			LODSplatOutputCountBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODSplatOutputCountBufferSRV = RHICmdList.CreateShaderResourceView(
			LODSplatOutputCountBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created LOD cluster tracking buffers"));

	// Create splat compaction buffers (GPU-driven work reduction)
	// CompactedSplatIndicesBuffer - stores visible splat indices
	{
		const uint32 BufferSize = TotalSplatCount * sizeof(uint32);
		CompactedSplatIndicesBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianCompactedSplatIndicesBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		CompactedSplatIndicesBufferUAV = RHICmdList.CreateUnorderedAccessView(
			CompactedSplatIndicesBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		CompactedSplatIndicesBufferSRV = RHICmdList.CreateShaderResourceView(
			CompactedSplatIndicesBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// VisibleSplatCountBuffer - atomic counter for visible splat count
	{
		const uint32 BufferSize = sizeof(uint32);
		VisibleSplatCountBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianVisibleSplatCountBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);

		VisibleSplatCountBufferUAV = RHICmdList.CreateUnorderedAccessView(
			VisibleSplatCountBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		VisibleSplatCountBufferSRV = RHICmdList.CreateShaderResourceView(
			VisibleSplatCountBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// IndirectDispatchArgsBuffer - for indirect compute dispatch
	// Format: uint3 (numGroupsX, numGroupsY, numGroupsZ)
	{
		const uint32 BufferSize = 3 * sizeof(uint32);
		IndirectDispatchArgsBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianIndirectDispatchArgsBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer,
			ERHIAccess::IndirectArgs);

		// Initialize with default values (1, 1, 1)
		uint32 InitData[3] = { 1, 1, 1 };
		void* Data = RHICmdList.LockBuffer(IndirectDispatchArgsBuffer, 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, InitData, BufferSize);
		RHICmdList.UnlockBuffer(IndirectDispatchArgsBuffer);

		IndirectDispatchArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			IndirectDispatchArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// Sort indirect args + sort params: needed by PrepareIndirectArgs in the
	// Nanite compaction path. Created here (not in CreateDynamicBuffers) because
	// per-proxy dynamic buffers are skipped under the global accumulator, but
	// PrepareIndirectArgs still runs per-proxy to set up indirect dispatch for
	// CalcViewDataCompactedGlobal.
	{
		const uint32 BufferSize = 3 * sizeof(uint32);
		SortIndirectArgsBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianSortIndirectArgsBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		SortIndirectArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			SortIndirectArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}
	{
		const uint32 BufferSize = 2 * sizeof(uint32);
		SortParamsBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianSortParamsBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		SortParamsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			SortParamsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SortParamsBufferSRV = RHICmdList.CreateShaderResourceView(
			SortParamsBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	bSupportsCompaction = true;
	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created splat compaction buffers for %d total splats"), TotalSplatCount);
	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created cluster buffers for %d clusters"), ClusterCount);
}

//////////////////////////////////////////////////////////////////////////
// FGaussianSplatSceneProxy

//Constructor. Copies rendering parameters from the component
FGaussianSplatSceneProxy::FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
	, CachedAsset((InComponent->PagedSourceAsset || InComponent->SpatialLODSourceAsset || InComponent->TreeSourceAsset) ? nullptr : InComponent->SplatAsset)
	, CachedPagedAsset(InComponent->PagedSourceAsset)
	, CachedSpatialLODAsset(InComponent->SpatialLODSourceAsset)
	, CachedTreeAsset(InComponent->TreeSourceAsset)
	, TreeRuntimeStats(InComponent->GetTreeRuntimeStats())
	, SplatCount(InComponent->TreeSourceAsset && InComponent->TreeSourceAsset->HasValidMetadata()
		? static_cast<int32>(InComponent->TreeSourceAsset->GetNodeCount())
		: (InComponent->SpatialLODSourceAsset && InComponent->SpatialLODSourceAsset->HasValidMetadata()
		? static_cast<int32>(InComponent->SpatialLODSourceAsset->GetLevelSplatCounts()[0])
		: (InComponent->PagedSourceAsset && InComponent->PagedSourceAsset->HasValidMetadata()
			? static_cast<int32>(InComponent->PagedSourceAsset->GetTotalSplatCount())
			: (InComponent->SplatAsset ? InComponent->SplatAsset->GetSplatCount() : 0))))
	, SHOrder(InComponent->SHOrder)
	, OpacityScale(InComponent->OpacityScale)
	, SplatScale(InComponent->SplatScale)
	, LODErrorThreshold(InComponent->LODErrorThreshold)
	, bForceFullDetailLOD0(InComponent->bForceFullDetailLOD0)
	, TreeLODSplatBudget(InComponent->TreeLODSplatBudget)
	, TreeLODProgressiveSplatBudget(InComponent->TreeLODProgressiveSplatBudget)
	, TreeLODDetailScale(InComponent->TreeLODDetailScale)
{
	bWillEverBeLit = false;
	if (CachedTreeAsset)
	{
		if (CVarNanoGSTreeLOD.GetValueOnAnyThread() == 0 &&
			!FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreeLOD")))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("NanoGS Tree v2 asset '%s' is assigned but gs.TreeLOD=0; legacy paths remain untouched and this component will not render."),
				*CachedTreeAsset->GetPathName());
			CachedTreeAsset = nullptr;
			SplatCount = 0;
		}
		else
		{
			PagedRenderData = MakeShared<NanoGS::Paged::FPageRenderData, ESPMode::ThreadSafe>();
			PagedRenderData->SetSceneBudgetIdentity(
				static_cast<uint64>(reinterpret_cast<UPTRINT>(&GetScene())),
				static_cast<uint64>(reinterpret_cast<UPTRINT>(this)));
			TreeManifest = MakeShared<NanoGS::Tree::FManifest, ESPMode::ThreadSafe>();
			FString Error;
			if (!CachedTreeAsset->OpenTreeManifest(*TreeManifest, Error) ||
				!PagedRenderData->Initialize(CachedTreeAsset, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("NanoGS Tree v2 scene proxy rejected '%s': %s"),
					*CachedTreeAsset->GetPathName(), *Error);
				PagedRenderData.Reset();
				TreeManifest.Reset();
				CachedTreeAsset = nullptr;
				SplatCount = 0;
			}
			else
			{
				TreeParents = MakeShared<TArray<uint32>, ESPMode::ThreadSafe>();
				TreeParents->Init(MAX_uint32, TreeManifest->Nodes.Num());
				for (uint32 ParentIndex = 0; ParentIndex < static_cast<uint32>(TreeManifest->Nodes.Num()); ++ParentIndex)
				{
					const NanoGS::Tree::FNode& Parent = TreeManifest->Nodes[ParentIndex];
					for (uint32 Offset = 0; Offset < Parent.ChildCount; ++Offset)
						(*TreeParents)[Parent.FirstChild + Offset] = ParentIndex;
				}
			}
		}
	}
	else if (CachedSpatialLODAsset)
	{
		PagedRenderData = MakeShared<NanoGS::Paged::FPageRenderData, ESPMode::ThreadSafe>();
		PagedRenderData->SetSceneBudgetIdentity(
			static_cast<uint64>(reinterpret_cast<UPTRINT>(&GetScene())),
			static_cast<uint64>(reinterpret_cast<UPTRINT>(this)));
		SpatialLODManifest = MakeUnique<NanoGS::SpatialLOD::FManifest>();
		FString Error;
		if (!CachedSpatialLODAsset->OpenManifest(*SpatialLODManifest, Error) ||
			!PagedRenderData->Initialize(CachedSpatialLODAsset, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("NanoGS SpatialLOD scene proxy rejected '%s': %s"),
				*CachedSpatialLODAsset->GetPathName(), *Error);
			PagedRenderData.Reset();
			SpatialLODManifest.Reset();
			CachedSpatialLODAsset = nullptr;
			SplatCount = 0;
		}
	}
	else if (CachedPagedAsset)
	{
		PagedRenderData = MakeShared<NanoGS::Paged::FPageRenderData, ESPMode::ThreadSafe>();
		PagedRenderData->SetSceneBudgetIdentity(
			static_cast<uint64>(reinterpret_cast<UPTRINT>(&GetScene())),
			static_cast<uint64>(reinterpret_cast<UPTRINT>(this)));
		FString Error;
		if (!PagedRenderData->Initialize(CachedPagedAsset, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("NanoGS Page scene proxy rejected '%s': %s"),
				*CachedPagedAsset->GetPathName(), *Error);
			PagedRenderData.Reset();
			CachedPagedAsset = nullptr;
			SplatCount = 0;
		}
	}
}

FGaussianSplatSceneProxy::~FGaussianSplatSceneProxy()
{
}

SIZE_T FGaussianSplatSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FGaussianSplatSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

FPrimitiveViewRelevance FGaussianSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = false; // Gaussian splats don't cast shadows (yet)
	Result.bDynamicRelevance = true;
	Result.bStaticRelevance = false;
	Result.bRenderInMainPass = true;
	Result.bUsesLightingChannels = false;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();

	return Result;
}

#if WITH_EDITOR
HHitProxy* FGaussianSplatSceneProxy::CreateHitProxies(UPrimitiveComponent* Component, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies)
{
	// Let the base class create the default HActor hit proxy for the owning actor.
	HHitProxy* DefaultProxy = FPrimitiveSceneProxy::CreateHitProxies(Component, OutHitProxies);
	// Cache it so GetDynamicMeshElements can use it without touching game-thread UObjects.
	SelectionHitProxy = DefaultProxy;
	return DefaultProxy;
}
#endif

void FGaussianSplatSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);

			// Draw bounds when selected
			if (IsSelected())
			{
				RenderBounds(PDI, ViewFamily.EngineShowFlags, GetBounds(), true);
			}

#if WITH_EDITOR
			// During the hit proxy pass, render a solid box covering the local bounds.
			// This makes the splat selectable by clicking anywhere within its bounds in
			// the editor viewport. The box is invisible in normal rendering.
			if (ViewFamily.EngineShowFlags.HitProxies)
			{
				const FBox LocalBox = GetLocalBounds().GetBox();
				const FVector3f Min(LocalBox.Min);
				const FVector3f Max(LocalBox.Max);

				FDynamicMeshBuilder MeshBuilder(Views[ViewIndex]->GetFeatureLevel());

				// Tangent basis (arbitrary – not shaded, just needs to be valid)
				const FVector2f UV(0.f, 0.f);
				const FVector3f TX(1.f, 0.f, 0.f);
				const FVector3f TY(0.f, 1.f, 0.f);
				const FVector3f TZ(0.f, 0.f, 1.f);
				const FColor White = FColor::White;

				// 8 corners  (named by which axes are at Max: 0=Min, 1=Max)
				const int32 V000 = MeshBuilder.AddVertex(FVector3f(Min.X, Min.Y, Min.Z), UV, TX, TY, TZ, White);
				const int32 V100 = MeshBuilder.AddVertex(FVector3f(Max.X, Min.Y, Min.Z), UV, TX, TY, TZ, White);
				const int32 V010 = MeshBuilder.AddVertex(FVector3f(Min.X, Max.Y, Min.Z), UV, TX, TY, TZ, White);
				const int32 V110 = MeshBuilder.AddVertex(FVector3f(Max.X, Max.Y, Min.Z), UV, TX, TY, TZ, White);
				const int32 V001 = MeshBuilder.AddVertex(FVector3f(Min.X, Min.Y, Max.Z), UV, TX, TY, TZ, White);
				const int32 V101 = MeshBuilder.AddVertex(FVector3f(Max.X, Min.Y, Max.Z), UV, TX, TY, TZ, White);
				const int32 V011 = MeshBuilder.AddVertex(FVector3f(Min.X, Max.Y, Max.Z), UV, TX, TY, TZ, White);
				const int32 V111 = MeshBuilder.AddVertex(FVector3f(Max.X, Max.Y, Max.Z), UV, TX, TY, TZ, White);

				// -Z face
				MeshBuilder.AddTriangle(V000, V010, V100);
				MeshBuilder.AddTriangle(V010, V110, V100);
				// +Z face
				MeshBuilder.AddTriangle(V001, V101, V011);
				MeshBuilder.AddTriangle(V101, V111, V011);
				// -Y face
				MeshBuilder.AddTriangle(V000, V100, V001);
				MeshBuilder.AddTriangle(V100, V101, V001);
				// +Y face
				MeshBuilder.AddTriangle(V010, V011, V110);
				MeshBuilder.AddTriangle(V011, V111, V110);
				// -X face
				MeshBuilder.AddTriangle(V000, V001, V010);
				MeshBuilder.AddTriangle(V001, V011, V010);
				// +X face
				MeshBuilder.AddTriangle(V100, V110, V101);
				MeshBuilder.AddTriangle(V110, V111, V101);

				// Use default opaque surface material – only its depth output matters here.
				// bDisableBackfaceCulling=true so all faces are drawn regardless of winding.
				UMaterialInterface* HitMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
				MeshBuilder.GetMesh(
					GetLocalToWorld(),
					HitMaterial->GetRenderProxy(),
					SDPG_World,
					/*bDisableBackfaceCulling=*/true,
					/*bReceivesDecals=*/false,
					/*bUseSelectionOutline=*/false,
					ViewIndex,
					Collector,
					SelectionHitProxy);
			}
#endif // WITH_EDITOR
		}
	}
}

//critical setup. creates FGaussianSplatGPUResources which uploads all the GPU buffers 
// registers itself with the ViewExtension
void FGaussianSplatSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	if (PagedRenderData.IsValid())
	{
		GPUResources = new FGaussianSplatGPUResources();
		GPUResources->Initialize(PagedRenderData);
		GPUResources->bUsesLODOpacityEncoding = TreeManifest.IsValid() &&
			!EnumHasAnyFlags(TreeManifest->Header.Flags,
				NanoGS::Tree::ETreeFlags::InflatedParents);

		FGaussianSplatViewExtension* ViewExtension = FGaussianSplatViewExtension::Get();
		if (ViewExtension)
		{
			ViewExtension->RegisterProxy(const_cast<FGaussianSplatSceneProxy*>(this));
		}
	}
	else if (CachedAsset && CachedAsset->IsValid())
	{
		GPUResources = new FGaussianSplatGPUResources();
		GPUResources->Initialize(CachedAsset);

		// Register with view extension for rendering
		FGaussianSplatViewExtension* ViewExtension = FGaussianSplatViewExtension::Get();
		if (ViewExtension)
		{
			ViewExtension->RegisterProxy(const_cast<FGaussianSplatSceneProxy*>(this));
		}
	}
}

int32 FGaussianSplatSceneProxy::GetSplatCount() const
{
	return GetSplatCountForView(false);
}

int32 FGaussianSplatSceneProxy::GetSplatCountForView(const bool bCaptureFrontier) const
{
	if (GPUResources && GPUResources->bIsPagedSource)
	{
		const uint32 ActiveCount = GPUResources->GetActiveSplatCount(bCaptureFrontier);
		return ActiveCount <= static_cast<uint32>(MAX_int32) ? static_cast<int32>(ActiveCount) : 0;
	}
	return SplatCount;
}

void FGaussianSplatSceneProxy::PublishTreeRuntimeStats_RenderThread(
	const NanoGS::Tree::FSelection& Selection) const
{
	if (!TreeRuntimeStats.IsValid())
		return;
	TreeRuntimeStats->SelectedNodeCount.store(Selection.TargetNodes.Num(), std::memory_order_release);
	TreeRuntimeStats->ExactLeafNodeCount.store(Selection.ExactLeafNodeCount, std::memory_order_release);
	TreeRuntimeStats->MinTreeDepth.store(Selection.MinSelectedDepth, std::memory_order_release);
	TreeRuntimeStats->MaxTreeDepth.store(Selection.MaxSelectedDepth, std::memory_order_release);
	TreeRuntimeStats->bFullDetailActive.store(bForceFullDetailLOD0, std::memory_order_release);
}

void FGaussianSplatSceneProxy::RequestPagedExactHQForViewFamily_RenderThread(
	const FSceneViewFamily& ViewFamily,
	const uint64 StableCaptureIdentity) const
{
	if (!PagedRenderData.IsValid())
		return;
	if (TreeManifest.IsValid())
	{
		const bool bExactCapture = StableCaptureIdentity != 0;
		const bool bFullDetail = bExactCapture || bForceFullDetailLOD0;
		NanoGS::Tree::FSelectionConfig Config;
		Config.SplitThresholdPx = bFullDetail
			? 0.0f
			: FMath::Max(0.0f, CVarNanoGSTreeLODSplitPixels.GetValueOnRenderThread()) /
				FMath::Max(0.1f, TreeLODDetailScale);
		Config.MaxActiveNodes = bFullDetail
			? MAX_uint32
			: static_cast<uint32>(FMath::Max(1, TreeLODSplatBudget));
		// The tree runtime consumes TargetNodes only. Avoid constructing and sorting
		// millions of unused ancestor/fallback entries on the render thread.
		Config.bBuildTransitionData = false;
		Config.bUseHierarchicalHeap =
			CVarNanoGSTreeSelectorHierarchicalHeap.GetValueOnRenderThread() != 0;
		Config.bBuildPageOutputMask = true;
		Config.bBuildResidentProgressiveData = !bFullDetail;
		Config.bBuildResidentRefinementTopology = !bFullDetail &&
			CVarNanoGSTreeResidentProgressiveFullTopology.GetValueOnRenderThread() != 0;
		const int32 ProgressiveBudgetOverride =
			CVarNanoGSTreeProgressiveFirstBudget.GetValueOnRenderThread();
		const int32 EffectiveProgressiveBudget = ProgressiveBudgetOverride >= 0
			? ProgressiveBudgetOverride
			: TreeLODProgressiveSplatBudget;
		Config.ProgressiveNodeBudget = !bFullDetail
			? static_cast<uint32>(FMath::Clamp(
				EffectiveProgressiveBudget, 0,
				static_cast<int32>(FMath::Min<uint32>(Config.MaxActiveNodes, MAX_int32))))
			: 0u;
		const FTransform LocalToWorld(GetLocalToWorld());
		const double WorldScale = FMath::Max(0.0, LocalToWorld.GetMaximumAxisScale()) *
			FMath::Max(0.0, static_cast<double>(SplatScale));
		const bool bFrustumCullTree = !bFullDetail &&
			CVarNanoGSTreeLODFrustumCull.GetValueOnRenderThread() != 0;
		uint64 ViewDemandIdentity = StableCaptureIdentity != 0
			? (0x9000000000000000ull ^ StableCaptureIdentity)
			: (0x5000000000000000ull ^ static_cast<uint64>(reinterpret_cast<UPTRINT>(ViewFamily.RenderTarget)));
		if (ViewDemandIdentity == 0)
			ViewDemandIdentity = 1;
		const FSceneView* RepresentativeView = nullptr;
		for (const FSceneView* View : ViewFamily.Views)
		{
			if (View != nullptr)
			{
				RepresentativeView = View;
				break;
			}
		}
		if (RepresentativeView == nullptr)
			return;

		const double MinTreeUpdateIntervalSeconds = FMath::Clamp(
			static_cast<double>(CVarNanoGSTreeLODUpdateInterval.GetValueOnRenderThread()), 0.05, 2.0);
		const double MinTreeSelectionCancelAgeSeconds = FMath::Clamp(
			static_cast<double>(CVarNanoGSTreeSelectorCancelMinAge.GetValueOnRenderThread()), 0.0, 2.0);
		const double TreeMovementThresholdCentimeters = FMath::Clamp(
			static_cast<double>(CVarNanoGSTreeLODMovementThreshold.GetValueOnRenderThread()), 10.0, 10000.0);
		const double DirectionDegrees = FMath::Clamp(
			static_cast<double>(CVarNanoGSTreeLODDirectionDegrees.GetValueOnRenderThread()), 0.1, 45.0);
		const double TreeDirectionDotThreshold = FMath::Cos(FMath::DegreesToRadians(DirectionDegrees));
		const double NowSeconds = FPlatformTime::Seconds();
		const FVector RepresentativeOrigin = RepresentativeView->ViewMatrices.GetViewOrigin();
		const FVector RepresentativeDirection = RepresentativeView->GetViewDirection().GetSafeNormal();
		const FIntPoint RepresentativeViewSize = RepresentativeView->UnscaledViewRect.Size();
		const FMatrix RepresentativeProjection = RepresentativeView->ViewMatrices.GetProjectionNoAAMatrix();
		const double RepresentativeFocalPixels = FMath::Max(
			FMath::Abs(static_cast<double>(RepresentativeProjection.M[0][0])) * 0.5 * RepresentativeViewSize.X,
			FMath::Abs(static_cast<double>(RepresentativeProjection.M[1][1])) * 0.5 * RepresentativeViewSize.Y);
		FRecentTreeDemand& RecentDemand = RecentTreeDemands.FindOrAdd(ViewDemandIdentity);
		RecentDemand.LastSeenSeconds = NowSeconds;
		if (RecentDemand.PendingSelection.IsValid() &&
			RecentDemand.PendingSelection->bProgressiveComplete.load(std::memory_order_acquire) &&
			!RecentDemand.PendingSelection->bProgressiveConsumed.exchange(true, std::memory_order_acq_rel))
		{
			const TSharedPtr<FTreeSelectionTaskResult, ESPMode::ThreadSafe>& Pending =
				RecentDemand.PendingSelection;
			const bool bProgressiveStale =
				FVector::DistSquared(RepresentativeOrigin, Pending->ViewOrigin) >=
					FMath::Square(TreeMovementThresholdCentimeters) ||
				FVector::DotProduct(RepresentativeDirection, Pending->ViewDirection.GetSafeNormal()) <
					TreeDirectionDotThreshold ||
				RepresentativeViewSize != Pending->ViewSize ||
				!FMath::IsNearlyEqual(RepresentativeFocalPixels, Pending->FocalPixels, 0.01);
			const bool bProgressiveValid = !bProgressiveStale &&
				Pending->ProgressiveExplicitSplatRefs.IsValid() && Pending->ProgressivePageIds.IsValid() &&
				Pending->ProgressiveSplatRuns.IsValid() &&
				Pending->ProgressivePageIds->Num() <= static_cast<int32>(PagedRenderData->GetPhysicalSlotCount());
			if (bProgressiveValid)
			{
				TArray<uint64> ProgressiveProtectedPageIds(*Pending->ProgressivePageIds);
				TArray<uint64> ResidentPageIds;
				PagedRenderData->GetResidentPageIds_RenderThread(ResidentPageIds);
				ProgressiveProtectedPageIds.Append(ResidentPageIds);
				SortUniquePageIds(ProgressiveProtectedPageIds);
				uint64 CombinedDemandIdentity = 0xA000000000000000ull ^
					static_cast<uint64>(reinterpret_cast<UPTRINT>(this));
				if (CombinedDemandIdentity == 0)
				{
					CombinedDemandIdentity = 1;
				}
				PagedRenderData->RequestExplicitSplats_RenderThread(
					TArray<NanoGS::Paged::FExplicitSplatRef>(*Pending->ProgressiveExplicitSplatRefs),
					MoveTemp(ProgressiveProtectedPageIds),
					TArray<NanoGS::Paged::FExplicitSplatRun>(*Pending->ProgressiveSplatRuns),
					CombinedDemandIdentity, false, true);
				bool bPublishedResidentFallback = false;
				if (Pending->ProgressiveRenderSplatRefs.IsValid() &&
					Pending->ProgressiveRenderPageIds.IsValid() &&
					Pending->ProgressiveRenderSplatRuns.IsValid() &&
					PagedRenderData->GetStreamingState() ==
						NanoGS::Paged::EExactHQStreamingState::Loading)
				{
					bPublishedResidentFallback = PagedRenderData->PublishResidentTreeFrontier_RenderThread(
						TArray<NanoGS::Paged::FExplicitSplatRef>(*Pending->ProgressiveRenderSplatRefs),
						TArray<uint64>(*Pending->ProgressiveRenderPageIds),
						TArray<NanoGS::Paged::FExplicitSplatRun>(*Pending->ProgressiveRenderSplatRuns),
						true);
				}
				RecentDemand.bProgressiveSubmitted = true;
				if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
				{
					UE_LOG(LogTemp, Display,
						TEXT("NanoGS Tree progressive frontier submitted: ready=%.3f ms targets=%d pages=%d resident-fallback=%d render-splats=%d"),
						Pending->ProgressiveReadyMilliseconds, Pending->ProgressiveTargetCount,
						Pending->ProgressivePageIds->Num(), bPublishedResidentFallback ? 1 : 0,
						Pending->ProgressiveRenderSplatRefs.IsValid()
							? Pending->ProgressiveRenderSplatRefs->Num() : 0);
				}
			}
			else if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree progressive frontier skipped: stale=%d pages=%d capacity=%u"),
					bProgressiveStale ? 1 : 0,
					Pending->ProgressivePageIds.IsValid() ? Pending->ProgressivePageIds->Num() : 0,
					PagedRenderData->GetPhysicalSlotCount());
			}
		}
		if (RecentDemand.PendingSelection.IsValid() &&
			RecentDemand.PendingSelection->bComplete.load(std::memory_order_acquire))
		{
			TSharedPtr<FTreeSelectionTaskResult, ESPMode::ThreadSafe> Completed =
				MoveTemp(RecentDemand.PendingSelection);
			NanoGS::Tree::FSelection Selection = MoveTemp(Completed->Selection);
			if (Selection.bCancelled)
			{
				RecentDemand.LastUpdateSeconds = 0.0;
				if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
				{
					UE_LOG(LogTemp, Display, TEXT("NanoGS Tree discarded stale selection after %.3f ms"),
						Completed->SelectionMilliseconds);
				}
			}
			else
			{
			RecentDemand.ViewOrigin = Completed->ViewOrigin;
			RecentDemand.ViewDirection = Completed->ViewDirection;
			RecentDemand.ViewSize = Completed->ViewSize;
			RecentDemand.FocalPixels = Completed->FocalPixels;
			RecentDemand.LastUpdateSeconds = Completed->RequestSeconds;
			RecentDemand.ReadySinceSeconds = 0.0;
			RecentDemand.SplitThresholdPx = Completed->SplitThresholdPx;
			RecentDemand.MaxActiveNodes = Completed->MaxActiveNodes;
			RecentDemand.bFrustumCull = Completed->bFrustumCull;
			RecentDemand.bInitialized = true;
			RecentDemand.bExactCapture = Completed->bExactCapture;
			RecentDemand.bFullDetail = Completed->bFullDetail;
			PublishTreeRuntimeStats_RenderThread(Selection);

			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree async selection perf: %.3f ms index-prep=%.3f ms targets=%d capture=%d full-detail=%d"),
					Completed->SelectionMilliseconds, Completed->IndexPreparationMilliseconds,
					Selection.TargetNodes.Num(), Completed->bExactCapture ? 1 : 0,
					Completed->bFullDetail ? 1 : 0);
			}
			if (!Completed->bExactCapture &&
				FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				const NanoGS::Tree::FNode& RootNode = TreeManifest->Nodes[
					static_cast<uint32>(TreeManifest->Header.RootNode)];
				const FVector RootWorldCenter = LocalToWorld.TransformPosition(FVector(RootNode.CenterCm));
				const double RootDistanceCm = FMath::Max(
					1.0, FVector::Distance(Completed->ViewOrigin, RootWorldCenter));
				const double RootScorePx = Completed->FocalPixels *
					(static_cast<double>(RootNode.FeatureRadiusCm) * WorldScale) / RootDistanceCm;
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree selection: targets=%d depth=%u..%u split=%.3f budget-limited=%d view=%dx%d focal=%.3f root-distance-cm=%.3f root-score-px=%.6f origin=(%.3f,%.3f,%.3f)"),
					Selection.TargetNodes.Num(), static_cast<uint32>(FMath::Max(0, Selection.MinSelectedDepth)),
					static_cast<uint32>(FMath::Max(0, Selection.MaxSelectedDepth)), Completed->SplitThresholdPx,
					Selection.bBudgetLimited ? 1 : 0,
					Completed->ViewSize.X, Completed->ViewSize.Y, Completed->FocalPixels,
					RootDistanceCm, RootScorePx, Completed->ViewOrigin.X,
					Completed->ViewOrigin.Y, Completed->ViewOrigin.Z);
			}
			RecentDemand.TargetNodes = MoveTemp(Selection.TargetNodes);
			RecentDemand.RefinedNodes = MoveTemp(Selection.RefinedNodes);
			RecentDemand.DemandedNodes = MoveTemp(Selection.DemandedNodes);
			RecentDemand.SortedExplicitSplatRefs = MoveTemp(Completed->SortedExplicitSplatRefs);
			RecentDemand.SortedPageIds = MoveTemp(Completed->SortedPageIds);
			RecentDemand.SplatRuns = MoveTemp(Completed->SplatRuns);
			bTreeCombinedDemandDirty = true;
			}
		}
		const bool bMovedEnough = FVector::DistSquared(RepresentativeOrigin, RecentDemand.ViewOrigin) >=
			FMath::Square(TreeMovementThresholdCentimeters);
		const bool bTurnedEnough = FVector::DotProduct(
			RepresentativeDirection, RecentDemand.ViewDirection.GetSafeNormal()) < TreeDirectionDotThreshold;
		const bool bProjectionChanged = RepresentativeViewSize != RecentDemand.ViewSize ||
			!FMath::IsNearlyEqual(RepresentativeFocalPixels, RecentDemand.FocalPixels, 0.01);
		const bool bConfigChanged = Config.SplitThresholdPx != RecentDemand.SplitThresholdPx ||
			Config.MaxActiveNodes != RecentDemand.MaxActiveNodes ||
			bFrustumCullTree != RecentDemand.bFrustumCull ||
			bExactCapture != RecentDemand.bExactCapture ||
			bFullDetail != RecentDemand.bFullDetail;
		const bool bIntervalElapsed = NowSeconds - RecentDemand.LastUpdateSeconds >= MinTreeUpdateIntervalSeconds;
		const bool bRebuildDemand = !RecentDemand.bInitialized || bConfigChanged || bProjectionChanged ||
			(!bFullDetail && bIntervalElapsed && (bMovedEnough || bTurnedEnough));
		const bool bGPUResourcesCreated = !bExactCapture && !bFullDetail &&
			PagedRenderData->HasTreeGPUNodeBuffer();
		const bool bGPUResourcesAvailable = bGPUResourcesCreated &&
			CVarNanoGSTreeGPUExperimentalSelector.GetValueOnRenderThread() != 0;
		if (bGPUResourcesCreated && !bGPUResourcesAvailable &&
			PagedRenderData->IsTreeGPUFrontierPublished())
		{
			PagedRenderData->RetireTreeGPUFrontier_RenderThread(
				TEXT("Production one-pass CPU selector active"));
		}
		const bool bGPUExclusiveSelector = bGPUResourcesAvailable &&
			CVarNanoGSTreeGPUExclusiveSelector.GetValueOnRenderThread() != 0;
		if (!bExactCapture && bFullDetail && PagedRenderData->IsTreeGPUFrontierPublished())
		{
			PagedRenderData->RetireTreeGPUFrontier_RenderThread(TEXT("Force Full Detail LOD0 requested"));
		}
		const bool bBeginGPURefinement = bRebuildDemand && !RecentDemand.PendingSelection.IsValid();
		const bool bGPUAdvanceAttempted = bGPUResourcesAvailable &&
			(bBeginGPURefinement || PagedRenderData->IsTreeGPUFrontierRefinementPending_RenderThread());
		bool bGPUAdvanceAccepted = false;
		if (bGPUAdvanceAttempted)
		{
			const FVector LocalViewOrigin = LocalToWorld.InverseTransformPosition(RepresentativeOrigin);
			bGPUAdvanceAccepted = PagedRenderData->AdvanceTreeGPUFrontier_RenderThread(
				FRHICommandListImmediate::Get(),
				FVector3f(LocalViewOrigin),
				static_cast<float>(RepresentativeFocalPixels * FMath::Max(0.0, static_cast<double>(SplatScale))),
				Config.SplitThresholdPx,
				bBeginGPURefinement);
			if (bBeginGPURefinement && bGPUAdvanceAccepted &&
				RecentDemand.GPURefinementFirstRequestSeconds <= 0.0)
			{
				RecentDemand.GPURefinementFirstRequestSeconds = NowSeconds;
			}
			if (bBeginGPURefinement && bGPUAdvanceAccepted && RecentDemand.bInitialized &&
				bGPUExclusiveSelector)
			{
				// Once the GPU owns regular-view selection, its accepted request is the
				// demand baseline. Leaving the CPU bootstrap camera here would make the
				// same moved view look dirty every frame and restart scratch from root.
				RecentDemand.ViewOrigin = RepresentativeOrigin;
				RecentDemand.ViewDirection = RepresentativeDirection;
				RecentDemand.ViewSize = RepresentativeViewSize;
				RecentDemand.FocalPixels = RepresentativeFocalPixels;
				RecentDemand.LastUpdateSeconds = NowSeconds;
				RecentDemand.SplitThresholdPx = Config.SplitThresholdPx;
				RecentDemand.MaxActiveNodes = Config.MaxActiveNodes;
				RecentDemand.bFrustumCull = bFrustumCullTree;
				RecentDemand.bExactCapture = false;
				RecentDemand.bFullDetail = false;
			}
		}
		const bool bGPUAlreadyPublished = PagedRenderData->IsTreeGPUFrontierPublished();
		if (bGPUAlreadyPublished)
		{
			RecentDemand.GPURefinementFirstRequestSeconds = 0.0;
		}
		const double GPUFallbackTimeoutSeconds = FMath::Clamp(
			static_cast<double>(CVarNanoGSTreeGPUExclusiveBootstrapTimeout.GetValueOnRenderThread()),
			0.0, 300.0);
		const bool bWithinGPUFirstPublicationGrace =
			RecentDemand.GPURefinementFirstRequestSeconds > 0.0 &&
			(GPUFallbackTimeoutSeconds <= 0.0 ||
			 NowSeconds - RecentDemand.GPURefinementFirstRequestSeconds < GPUFallbackTimeoutSeconds);
		const bool bGPUOwnsRegularSelection = bGPUExclusiveSelector && RecentDemand.bInitialized &&
			(bGPUAlreadyPublished || (bGPUAdvanceAccepted && bWithinGPUFirstPublicationGrace));
		const bool bStartCPUFallback = bRebuildDemand || (bGPUAdvanceAttempted && !bGPUAdvanceAccepted);
		if (RecentDemand.PendingSelection.IsValid() &&
			!RecentDemand.PendingSelection->bComplete.load(std::memory_order_acquire))
		{
			const FTreeSelectionTaskResult& Pending = *RecentDemand.PendingSelection;
			const bool bPendingMovedEnough = FVector::DistSquared(RepresentativeOrigin, Pending.ViewOrigin) >=
				FMath::Square(TreeMovementThresholdCentimeters);
			const bool bPendingTurnedEnough = FVector::DotProduct(
				RepresentativeDirection, Pending.ViewDirection.GetSafeNormal()) < TreeDirectionDotThreshold;
			const bool bPendingProjectionChanged = RepresentativeViewSize != Pending.ViewSize ||
				!FMath::IsNearlyEqual(RepresentativeFocalPixels, Pending.FocalPixels, 0.01);
			const bool bPendingConfigChanged = Config.SplitThresholdPx != Pending.SplitThresholdPx ||
				Config.MaxActiveNodes != Pending.MaxActiveNodes || bFrustumCullTree != Pending.bFrustumCull ||
				bExactCapture != Pending.bExactCapture || bFullDetail != Pending.bFullDetail;
			const bool bPendingViewStale = bPendingMovedEnough || bPendingTurnedEnough;
			const bool bPendingOldEnoughToCancel =
				NowSeconds - Pending.RequestSeconds >= MinTreeSelectionCancelAgeSeconds;
			if (bGPUOwnsRegularSelection || bPendingProjectionChanged || bPendingConfigChanged ||
				(bPendingViewStale && bPendingOldEnoughToCancel))
				RecentDemand.PendingSelection->bCancelRequested.store(true, std::memory_order_release);
		}
		RecentDemand.LastSeenSeconds = NowSeconds;
		if (bStartCPUFallback && !RecentDemand.PendingSelection.IsValid() && bGPUOwnsRegularSelection)
		{
			++RecentDemand.GPUExclusiveCPUSuppressedCount;
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")) &&
				(RecentDemand.GPUExclusiveCPUSuppressedCount == 1u ||
				 (RecentDemand.GPUExclusiveCPUSuppressedCount % 64u) == 0u))
			{
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree GPU selector owns regular view; CPU selection suppressed count=%u published=%d"),
					RecentDemand.GPUExclusiveCPUSuppressedCount, bGPUAlreadyPublished ? 1 : 0);
			}
		}
		if (bStartCPUFallback && !RecentDemand.PendingSelection.IsValid() && !bGPUOwnsRegularSelection)
		{
			struct FSelectionViewSnapshot
			{
				FVector Origin = FVector::ZeroVector;
				FVector3f LocalOrigin = FVector3f::ZeroVector;
				FConvexVolume Frustum;
				double FocalPixels = 0.0;
				bool bPerspective = false;
			};
			TArray<FSelectionViewSnapshot> ViewSnapshots;
			ViewSnapshots.Reserve(ViewFamily.Views.Num());
			for (const FSceneView* View : ViewFamily.Views)
			{
				if (View == nullptr)
					continue;
				FSelectionViewSnapshot& Snapshot = ViewSnapshots.AddDefaulted_GetRef();
				Snapshot.Origin = View->ViewMatrices.GetViewOrigin();
				Snapshot.LocalOrigin = FVector3f(LocalToWorld.InverseTransformPosition(Snapshot.Origin));
				Snapshot.Frustum = View->ViewFrustum;
				const FIntPoint ViewSize = View->UnscaledViewRect.Size();
				const FMatrix Projection = View->ViewMatrices.GetProjectionNoAAMatrix();
				Snapshot.FocalPixels = FMath::Max(
					FMath::Abs(static_cast<double>(Projection.M[0][0])) * 0.5 * ViewSize.X,
					FMath::Abs(static_cast<double>(Projection.M[1][1])) * 0.5 * ViewSize.Y);
				Snapshot.bPerspective = View->IsPerspectiveProjection() && ViewSize.X > 0 && ViewSize.Y > 0;
			}

			TSharedPtr<FTreeSelectionTaskResult, ESPMode::ThreadSafe> TaskResult =
				MakeShared<FTreeSelectionTaskResult, ESPMode::ThreadSafe>();
			TaskResult->ViewOrigin = RepresentativeOrigin;
			TaskResult->ViewDirection = RepresentativeDirection;
			TaskResult->ViewSize = RepresentativeViewSize;
			TaskResult->FocalPixels = RepresentativeFocalPixels;
			TaskResult->RequestSeconds = NowSeconds;
			TaskResult->SplitThresholdPx = Config.SplitThresholdPx;
			TaskResult->MaxActiveNodes = Config.MaxActiveNodes;
			TaskResult->bFrustumCull = bFrustumCullTree;
			TaskResult->bExactCapture = bExactCapture;
			TaskResult->bFullDetail = bFullDetail;
			Config.CancelFlag = &TaskResult->bCancelRequested;
			RecentDemand.PendingSelection = TaskResult;
			TArray<uint64> SelectionResidentPageIds;
			PagedRenderData->GetResidentPageIds_RenderThread(SelectionResidentPageIds);
			TSharedPtr<NanoGS::Tree::FManifest, ESPMode::ThreadSafe> ManifestSnapshot = TreeManifest;
			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
				[ManifestSnapshot, Config, LocalToWorld, WorldScale, bFullDetail,
				 bFrustumCullTree, LocalFeatureScale = FMath::Max(0.0f, SplatScale),
				 ViewSnapshots = MoveTemp(ViewSnapshots),
				 SelectionResidentPageIds = MoveTemp(SelectionResidentPageIds), TaskResult]() mutable
				{
					if (bFullDetail)
					{
						const double SelectionStartSeconds = FPlatformTime::Seconds();
						auto ExplicitRefs = MakeShared<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe>();
						auto PageIds = MakeShared<TArray<uint64>, ESPMode::ThreadSafe>();
						auto SplatRuns = MakeShared<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe>();
						const uint64 ExactLeafCount = FMath::Min<uint64>(
							ManifestSnapshot->Header.ExactLeafCount, ManifestSnapshot->Nodes.Num());
						TMap<uint64, TBitArray<>> ExactPageMasks;
						ExactPageMasks.Reserve(FMath::DivideAndRoundUp(
							static_cast<int64>(ExactLeafCount), static_cast<int64>(ManifestSnapshot->Header.PagePoints)));
						uint64 CollectedExactLeafCount = 0;
						for (const NanoGS::Tree::FNode& Node : ManifestSnapshot->Nodes)
						{
							if (!Node.IsExactLeaf())
							{
								continue;
							}
							++CollectedExactLeafCount;
							TBitArray<>& PageMask = ExactPageMasks.FindOrAdd(Node.PageId);
							if (PageMask.IsEmpty())
							{
								PageMask.Init(false, ManifestSnapshot->Header.PagePoints);
							}
							if (PageMask.IsValidIndex(static_cast<int32>(Node.PageLocal)))
							{
								PageMask[static_cast<int32>(Node.PageLocal)] = true;
							}
						}
						if (CollectedExactLeafCount != ExactLeafCount)
						{
							UE_LOG(LogTemp, Error,
								TEXT("NanoGS Tree full-detail leaf count mismatch: collected=%llu expected=%llu"),
								CollectedExactLeafCount, ExactLeafCount);
						}
						ExactPageMasks.GetKeys(*PageIds);
						PageIds->Sort();
						for (const uint64 PageId : *PageIds)
						{
							const TBitArray<>* PageMask = ExactPageMasks.Find(PageId);
							if (PageMask == nullptr)
							{
								continue;
							}
							int32 Local = 0;
							while (Local < PageMask->Num())
							{
								while (Local < PageMask->Num() && !(*PageMask)[Local]) ++Local;
								const int32 RunStart = Local;
								while (Local < PageMask->Num() && (*PageMask)[Local]) ++Local;
								if (Local > RunStart)
								{
									SplatRuns->Add({PageId, static_cast<uint32>(RunStart), static_cast<uint32>(Local - RunStart)});
								}
							}
						}
						TaskResult->SortedExplicitSplatRefs = MoveTemp(ExplicitRefs);
						TaskResult->SortedPageIds = MoveTemp(PageIds);
						TaskResult->SplatRuns = MoveTemp(SplatRuns);
						TaskResult->SelectionMilliseconds =
							(FPlatformTime::Seconds() - SelectionStartSeconds) * 1000.0;
						TaskResult->IndexPreparationMilliseconds = 0.0;
						TaskResult->bComplete.store(true, std::memory_order_release);
						return;
					}

					const FVector ComponentScale = LocalToWorld.GetScale3D().GetAbs();
					const bool bUniformComponentScale = FMath::IsNearlyEqual(ComponentScale.X, ComponentScale.Y) &&
						FMath::IsNearlyEqual(ComponentScale.X, ComponentScale.Z);
					const bool bUseLocalScoreFastPath = !bFrustumCullTree && bUniformComponentScale &&
						ViewSnapshots.Num() == 1 && ViewSnapshots[0].bPerspective;
					const auto ObserveNode = [&ManifestSnapshot, &LocalToWorld, WorldScale,
						bFullDetail, bFrustumCullTree, bUseLocalScoreFastPath, LocalFeatureScale,
						&ViewSnapshots](uint32 NodeIndex)
					{
						NanoGS::Tree::FNodeObservation Result;
						if (!ManifestSnapshot->Nodes.IsValidIndex(static_cast<int32>(NodeIndex)))
							return Result;
						if (bFullDetail)
						{
							Result.bVisible = true;
							Result.ProjectedRadiusPx = std::numeric_limits<float>::infinity();
							return Result;
						}
						const NanoGS::Tree::FNode& Node = ManifestSnapshot->Nodes[NodeIndex];
						if (bUseLocalScoreFastPath)
						{
							const FSelectionViewSnapshot& View = ViewSnapshots[0];
							const FVector3f Delta = Node.CenterCm - View.LocalOrigin;
							const float Distance = FMath::Sqrt(FMath::Max(1.0f, Delta.SizeSquared()));
							Result.bVisible = true;
							Result.ProjectedRadiusPx = static_cast<float>(View.FocalPixels) *
								Node.FeatureRadiusCm * LocalFeatureScale / Distance;
							return Result;
						}
						const FVector WorldCenter = LocalToWorld.TransformPosition(FVector(Node.CenterCm));
						const double WorldSupportRadius = static_cast<double>(Node.SupportRadiusCm) * WorldScale;
						const double WorldFeatureSize = static_cast<double>(Node.FeatureRadiusCm) * WorldScale;
						for (const FSelectionViewSnapshot& View : ViewSnapshots)
						{
							if (bFrustumCullTree && !View.Frustum.IntersectSphere(WorldCenter, WorldSupportRadius))
								continue;
							Result.bVisible = true;
							double ProjectedRadiusPx = std::numeric_limits<double>::infinity();
							if (View.bPerspective)
							{
								const double Distance = FMath::Max(1.0, FVector::Distance(View.Origin, WorldCenter));
								ProjectedRadiusPx = View.FocalPixels * WorldFeatureSize / Distance;
							}
							Result.ProjectedRadiusPx = FMath::Max(
								Result.ProjectedRadiusPx, static_cast<float>(ProjectedRadiusPx));
						}
						return Result;
					};
					const double SelectionStartSeconds = FPlatformTime::Seconds();
					uint32 MaxSelectionResidentPageId = 0;
					for (const uint64 PageId : SelectionResidentPageIds)
					{
						if (PageId <= MAX_uint32)
						{
							MaxSelectionResidentPageId = FMath::Max(
								MaxSelectionResidentPageId, static_cast<uint32>(PageId));
						}
					}
					TBitArray<> SelectionResidentPages(
						false, static_cast<int32>(MaxSelectionResidentPageId) + 1);
					for (const uint64 PageId : SelectionResidentPageIds)
					{
						if (PageId < static_cast<uint64>(SelectionResidentPages.Num()))
						{
							SelectionResidentPages[static_cast<int32>(PageId)] = true;
						}
					}
					Config.ProgressiveCallback =
						[ManifestSnapshot, TaskResult, SelectionStartSeconds](
							NanoGS::Tree::FSelection&& ProgressiveSelection)
						{
							if (TaskResult->bCancelRequested.load(std::memory_order_acquire))
							{
								return;
							}
							auto Refs = MakeShared<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe>();
							auto PageIds = MakeShared<TArray<uint64>, ESPMode::ThreadSafe>();
							auto Runs = MakeShared<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe>();
							if (!BuildExplicitSplatFrontier(
								*ManifestSnapshot, ProgressiveSelection.TargetNodes,
								*Refs, *PageIds, *Runs, &ProgressiveSelection.SelectedPageOutputs))
							{
								return;
							}
							TaskResult->ProgressiveTargetCount = ProgressiveSelection.TargetNodes.Num();
							TaskResult->ProgressiveExplicitSplatRefs = MoveTemp(Refs);
							TaskResult->ProgressivePageIds = MoveTemp(PageIds);
							TaskResult->ProgressiveSplatRuns = MoveTemp(Runs);
							if (!ProgressiveSelection.RenderNodes.IsEmpty())
							{
								auto RenderRefs = MakeShared<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe>();
								auto RenderPageIds = MakeShared<TArray<uint64>, ESPMode::ThreadSafe>();
								auto RenderRuns = MakeShared<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe>();
								if (BuildExplicitSplatFrontier(
									*ManifestSnapshot, ProgressiveSelection.RenderNodes,
									*RenderRefs, *RenderPageIds, *RenderRuns,
									&ProgressiveSelection.RenderPageOutputs))
								{
									TaskResult->ProgressiveRenderSplatRefs = MoveTemp(RenderRefs);
									TaskResult->ProgressiveRenderPageIds = MoveTemp(RenderPageIds);
									TaskResult->ProgressiveRenderSplatRuns = MoveTemp(RenderRuns);
								}
							}
							TaskResult->ProgressiveReadyMilliseconds =
								(FPlatformTime::Seconds() - SelectionStartSeconds) * 1000.0;
							TaskResult->bProgressiveComplete.store(true, std::memory_order_release);
						};
					NanoGS::Tree::FSelector WorkerSelector;
					TaskResult->Selection = WorkerSelector.Select(
						*ManifestSnapshot, Config, ObserveNode,
						[&ManifestSnapshot, &SelectionResidentPages](const uint32 NodeIndex)
						{
							const uint32 PageId = ManifestSnapshot->Nodes[NodeIndex].PageId;
							return SelectionResidentPages.IsValidIndex(static_cast<int32>(PageId)) &&
								SelectionResidentPages[PageId];
						});
					TaskResult->SelectionMilliseconds =
						(FPlatformTime::Seconds() - SelectionStartSeconds) * 1000.0;
					if (TaskResult->Selection.bCancelled ||
						TaskResult->bCancelRequested.load(std::memory_order_acquire))
					{
						TaskResult->Selection = NanoGS::Tree::FSelection();
						TaskResult->Selection.bCancelled = true;
						TaskResult->bComplete.store(true, std::memory_order_release);
						return;
					}
					const double IndexPreparationStartSeconds = FPlatformTime::Seconds();
					auto ExplicitRefs = MakeShared<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe>();
					auto PageIds = MakeShared<TArray<uint64>, ESPMode::ThreadSafe>();
					auto SplatRuns = MakeShared<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe>();
					if (!BuildExplicitSplatFrontier(
						*ManifestSnapshot, TaskResult->Selection.TargetNodes,
						*ExplicitRefs, *PageIds, *SplatRuns,
						&TaskResult->Selection.SelectedPageOutputs))
					{
						ExplicitRefs->Reset();
						PageIds->Reset();
						SplatRuns->Reset();
						TArray<uint64> PackedRefs;
						PackedRefs.Reset(TaskResult->Selection.TargetNodes.Num());
						PackedRefs.Reserve(TaskResult->Selection.TargetNodes.Num());
						for (const uint32 NodeIndex : TaskResult->Selection.TargetNodes)
						{
							const NanoGS::Tree::FNode& Node = ManifestSnapshot->Nodes[NodeIndex];
							PackedRefs.Add(
								(static_cast<uint64>(Node.PageId) << 32u) | static_cast<uint64>(Node.PageLocal));
						}
						RadixSortPackedSplatRefs(PackedRefs);
						ExplicitRefs->Reserve(PackedRefs.Num());
						uint64 PreviousPageId = MAX_uint64;
						for (const uint64 PackedRef : PackedRefs)
						{
							const uint64 PageId = PackedRef >> 32u;
							const uint32 PageLocal = static_cast<uint32>(PackedRef);
							ExplicitRefs->Add({PageId, PageLocal});
							if (PageId != PreviousPageId)
							{
								PageIds->Add(PageId);
								PreviousPageId = PageId;
							}
							if (!SplatRuns->IsEmpty() && SplatRuns->Last().PageId == PageId &&
								SplatRuns->Last().PageLocal + SplatRuns->Last().Count == PageLocal)
							{
								++SplatRuns->Last().Count;
							}
							else
							{
								SplatRuns->Add({PageId, PageLocal, 1u});
							}
						}
					}
					TaskResult->SortedExplicitSplatRefs = MoveTemp(ExplicitRefs);
					TaskResult->SortedPageIds = MoveTemp(PageIds);
					TaskResult->SplatRuns = MoveTemp(SplatRuns);
					TaskResult->IndexPreparationMilliseconds =
						(FPlatformTime::Seconds() - IndexPreparationStartSeconds) * 1000.0;
					TaskResult->bComplete.store(true, std::memory_order_release);
				});
		}

#if 0 // Synchronous reference path retained until async runtime validation completes.
		const auto ObserveNode = [this, &ViewFamily, &LocalToWorld, WorldScale, bExactCapture, bFrustumCullTree](uint32 NodeIndex)
		{
			NanoGS::Tree::FNodeObservation Result;
			if (!TreeManifest->Nodes.IsValidIndex(static_cast<int32>(NodeIndex)))
				return Result;
			if (bExactCapture)
			{
				Result.bVisible = true;
				Result.ProjectedRadiusPx = std::numeric_limits<float>::infinity();
				return Result;
			}
			const NanoGS::Tree::FNode& Node = TreeManifest->Nodes[NodeIndex];
			const FVector WorldCenter = LocalToWorld.TransformPosition(FVector(Node.CenterCm));
			const double WorldSupportRadius = static_cast<double>(Node.SupportRadiusCm) * WorldScale;
			const double WorldFeatureSize = static_cast<double>(Node.FeatureRadiusCm) * WorldScale;
			for (const FSceneView* View : ViewFamily.Views)
			{
				if (View == nullptr || (bFrustumCullTree &&
					!View->ViewFrustum.IntersectSphere(WorldCenter, WorldSupportRadius)))
					continue;
				Result.bVisible = true;
				const FIntPoint ViewSize = View->UnscaledViewRect.Size();
				const FMatrix Projection = View->ViewMatrices.GetProjectionNoAAMatrix();
				double ProjectedRadiusPx = std::numeric_limits<double>::infinity();
				if (View->IsPerspectiveProjection() && ViewSize.X > 0 && ViewSize.Y > 0)
				{
					const double FocalPixels = FMath::Max(
						FMath::Abs(static_cast<double>(Projection.M[0][0])) * 0.5 * ViewSize.X,
						FMath::Abs(static_cast<double>(Projection.M[1][1])) * 0.5 * ViewSize.Y);
					const double Distance = FMath::Max(1.0, FVector::Distance(View->ViewMatrices.GetViewOrigin(), WorldCenter));
					ProjectedRadiusPx = FocalPixels * WorldFeatureSize / Distance;
				}
				Result.ProjectedRadiusPx = FMath::Max(
					Result.ProjectedRadiusPx, static_cast<float>(ProjectedRadiusPx));
			}
			return Result;
		};
		// Kept temporarily as a synchronous reference path while the worker result is
		// validated; it must never execute in the interactive runtime.
		if (false && bRebuildDemand)
		{
			const double SelectionStartSeconds = FPlatformTime::Seconds();
			const NanoGS::Tree::FSelection Selection = TreeSelector.Select(
				*TreeManifest, Config, ObserveNode, [](uint32 NodeIndex) { return false; });
			const double SelectionMilliseconds =
				(FPlatformTime::Seconds() - SelectionStartSeconds) * 1000.0;
			RecentDemand.TargetNodes = Selection.TargetNodes;
			RecentDemand.ViewOrigin = RepresentativeOrigin;
			RecentDemand.ViewDirection = RepresentativeDirection;
			RecentDemand.ViewSize = RepresentativeViewSize;
			RecentDemand.FocalPixels = RepresentativeFocalPixels;
			RecentDemand.LastUpdateSeconds = NowSeconds;
			RecentDemand.ReadySinceSeconds = 0.0;
			RecentDemand.SplitThresholdPx = Config.SplitThresholdPx;
			RecentDemand.MaxActiveNodes = Config.MaxActiveNodes;
			RecentDemand.bFrustumCull = bFrustumCullTree;
			RecentDemand.bInitialized = true;
			RecentDemand.bExactCapture = bExactCapture;
			RecentDemand.bFullDetail = bFullDetail;
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree selection perf: %.3f ms targets=%d exact=%d"),
					SelectionMilliseconds, Selection.TargetNodes.Num(), bExactCapture ? 1 : 0);
			}
			if (!bExactCapture && FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
			uint16 MinSelectedDepth = MAX_uint16;
			uint16 MaxSelectedDepth = 0;
			for (const uint32 NodeIndex : Selection.TargetNodes)
			{
				if (!TreeManifest->Nodes.IsValidIndex(static_cast<int32>(NodeIndex)))
					continue;
				const uint16 Depth = TreeManifest->Nodes[NodeIndex].Depth;
				MinSelectedDepth = FMath::Min(MinSelectedDepth, Depth);
				MaxSelectedDepth = FMath::Max(MaxSelectedDepth, Depth);
			}
			static uint32 LastLoggedTargetCount = MAX_uint32;
			if (LastLoggedTargetCount != static_cast<uint32>(Selection.TargetNodes.Num()))
			{
				LastLoggedTargetCount = Selection.TargetNodes.Num();
				FIntPoint DiagnosticViewSize = FIntPoint::ZeroValue;
				double DiagnosticFocalPixels = 0.0;
				double DiagnosticRootDistanceCm = 0.0;
				double DiagnosticRootScorePx = 0.0;
				FVector DiagnosticViewOrigin = FVector::ZeroVector;
				if (!ViewFamily.Views.IsEmpty() && ViewFamily.Views[0] != nullptr)
				{
					const FSceneView* DiagnosticView = ViewFamily.Views[0];
					DiagnosticViewSize = DiagnosticView->UnscaledViewRect.Size();
					const FMatrix DiagnosticProjection = DiagnosticView->ViewMatrices.GetProjectionNoAAMatrix();
					DiagnosticFocalPixels = FMath::Max(
						FMath::Abs(static_cast<double>(DiagnosticProjection.M[0][0])) * 0.5 * DiagnosticViewSize.X,
						FMath::Abs(static_cast<double>(DiagnosticProjection.M[1][1])) * 0.5 * DiagnosticViewSize.Y);
					DiagnosticViewOrigin = DiagnosticView->ViewMatrices.GetViewOrigin();
					const NanoGS::Tree::FNode& RootNode = TreeManifest->Nodes[
						static_cast<uint32>(TreeManifest->Header.RootNode)];
					const FVector RootWorldCenter = LocalToWorld.TransformPosition(FVector(RootNode.CenterCm));
					DiagnosticRootDistanceCm = FMath::Max(1.0, FVector::Distance(DiagnosticViewOrigin, RootWorldCenter));
					DiagnosticRootScorePx = DiagnosticFocalPixels *
						(static_cast<double>(RootNode.FeatureRadiusCm) * WorldScale) / DiagnosticRootDistanceCm;
				}
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree selection: targets=%d depth=%u..%u split=%.3f budget-limited=%d view=%dx%d focal=%.3f root-distance-cm=%.3f root-score-px=%.6f origin=(%.3f,%.3f,%.3f)"),
					Selection.TargetNodes.Num(), MinSelectedDepth == MAX_uint16 ? 0u : MinSelectedDepth,
					MaxSelectedDepth, Config.SplitThresholdPx, Selection.bBudgetLimited ? 1 : 0,
					DiagnosticViewSize.X, DiagnosticViewSize.Y, DiagnosticFocalPixels,
					DiagnosticRootDistanceCm, DiagnosticRootScorePx,
					DiagnosticViewOrigin.X, DiagnosticViewOrigin.Y, DiagnosticViewOrigin.Z);
			}
			}
		}
#endif
		constexpr double TreeDemandRetentionSeconds = 5.0;
		const bool bRequestedFrontierReady = PagedRenderData->IsRequestedFrontierReady();
		for (auto It = RecentTreeDemands.CreateIterator(); It; ++It)
		{
			if (It.Key() == ViewDemandIdentity)
				continue;
			FRecentTreeDemand& Demand = It.Value();
			if (!Demand.bExactCapture)
			{
				if (NowSeconds - Demand.LastSeenSeconds > TreeDemandRetentionSeconds)
				{
					It.RemoveCurrent();
					bTreeCombinedDemandDirty = true;
				}
				continue;
			}
			// A Tree strict-capture frontier is the complete Exact Leaf set and is
			// therefore camera-independent. Keep its demand for the source lifetime so
			// resident slots cannot be recycled while the capture active-index remains
			// published. A future scene budget manager may explicitly retire this pin.
			Demand.ReadySinceSeconds = PagedRenderData->IsCaptureFrontierReady()
				? FMath::Max(Demand.ReadySinceSeconds, NowSeconds)
				: 0.0;
			continue;
		}

		if (bTreeCombinedDemandDirty)
		{
			bTreeHasMainDemand = false;
			bTreeHasCaptureDemand = false;
			bTreeCombinedDemandExactCapture = false;
			bTreeCombinedDemandFullDetail = false;
			CachedTreeCombinedFrontier.Reset();
			CachedTreeCombinedExplicitSplatRefs.Reset();
			CachedTreeCombinedPageIds.Reset();
			CachedTreeCombinedSplatRuns.Reset();
			CachedTreeCombinedRefinedNodes.Reset();
			CachedTreeCombinedDemandedNodes.Reset();
			CachedTreeCaptureFrontier.Reset();
			CachedTreeCaptureExplicitSplatRefs.Reset();
			CachedTreeCapturePageIds.Reset();
			CachedTreeCaptureSplatRuns.Reset();

			TArray<const TArray<uint32>*> MainFrontiers;
			TArray<const TArray<uint32>*> CaptureFrontiers;
			const FRecentTreeDemand* SingleMainDemand = nullptr;
			const FRecentTreeDemand* SingleCaptureDemand = nullptr;
			const FRecentTreeDemand* FullDetailMainDemand = nullptr;
			for (const TPair<uint64, FRecentTreeDemand>& Pair : RecentTreeDemands)
			{
				const FRecentTreeDemand& Demand = Pair.Value;
				if (Demand.bExactCapture)
				{
					bTreeHasCaptureDemand = true;
					CaptureFrontiers.Add(&Demand.TargetNodes);
					SingleCaptureDemand = CaptureFrontiers.Num() == 1 ? &Demand : nullptr;
				}
				else
				{
					bTreeHasMainDemand = true;
					MainFrontiers.Add(&Demand.TargetNodes);
					SingleMainDemand = MainFrontiers.Num() == 1 ? &Demand : nullptr;
					if (Demand.bFullDetail)
					{
						FullDetailMainDemand = &Demand;
					}
				}
			}

			if (FullDetailMainDemand != nullptr)
			{
				bTreeCombinedDemandFullDetail = true;
				CachedTreeCombinedFrontier = FullDetailMainDemand->TargetNodes;
				CachedTreeCombinedExplicitSplatRefs = FullDetailMainDemand->SortedExplicitSplatRefs;
				CachedTreeCombinedPageIds = FullDetailMainDemand->SortedPageIds;
				CachedTreeCombinedSplatRuns = FullDetailMainDemand->SplatRuns;
			}
			else if (MainFrontiers.Num() == 1 && SingleMainDemand != nullptr)
			{
				CachedTreeCombinedFrontier = SingleMainDemand->TargetNodes;
				CachedTreeCombinedExplicitSplatRefs = SingleMainDemand->SortedExplicitSplatRefs;
				CachedTreeCombinedPageIds = SingleMainDemand->SortedPageIds;
				CachedTreeCombinedSplatRuns = SingleMainDemand->SplatRuns;
				CachedTreeCombinedRefinedNodes = SingleMainDemand->RefinedNodes;
				CachedTreeCombinedDemandedNodes = SingleMainDemand->DemandedNodes;
			}
			else if (MainFrontiers.Num() > 1)
			{
				CachedTreeCombinedFrontier = NanoGS::Tree::FSelector::MergeFrontiers(
					*TreeManifest, *TreeParents, MainFrontiers);
			}

			if (CaptureFrontiers.Num() == 1 && SingleCaptureDemand != nullptr)
			{
				CachedTreeCaptureFrontier = SingleCaptureDemand->TargetNodes;
				CachedTreeCaptureExplicitSplatRefs = SingleCaptureDemand->SortedExplicitSplatRefs;
				CachedTreeCapturePageIds = SingleCaptureDemand->SortedPageIds;
				CachedTreeCaptureSplatRuns = SingleCaptureDemand->SplatRuns;
			}
			else if (CaptureFrontiers.Num() > 1)
			{
				CachedTreeCaptureFrontier = NanoGS::Tree::FSelector::MergeFrontiers(
					*TreeManifest, *TreeParents, CaptureFrontiers);
			}

			if (!bTreeHasMainDemand && bTreeHasCaptureDemand)
			{
				bTreeCombinedDemandExactCapture = true;
				bTreeCombinedDemandFullDetail = true;
				CachedTreeCombinedFrontier = CachedTreeCaptureFrontier;
				CachedTreeCombinedExplicitSplatRefs = CachedTreeCaptureExplicitSplatRefs;
				CachedTreeCombinedPageIds = CachedTreeCapturePageIds;
				CachedTreeCombinedSplatRuns = CachedTreeCaptureSplatRuns;
			}

			PendingTreeTransition.Reset();
			LastScheduledTreeResidencyGeneration = MAX_uint32;
			++TreeCombinedTargetGeneration;
			if (TreeCombinedTargetGeneration == 0)
			{
				++TreeCombinedTargetGeneration;
			}
			bTreeCombinedDemandDirty = false;
			bTreeCombinedDemandSubmissionPending = !CachedTreeCombinedFrontier.IsEmpty() ||
				(bTreeCombinedDemandFullDetail && CachedTreeCombinedPageIds.IsValid());
			bTreeCombinedDemandHasSubmitted = SingleMainDemand != nullptr &&
				SingleMainDemand->bProgressiveSubmitted;
		}

		const bool bCanSubmitCombinedDemand = !bTreeCombinedDemandHasSubmitted ||
			PagedRenderData->IsRequestedFrontierReady();
		if (bTreeCombinedDemandFullDetail && bTreeCombinedDemandSubmissionPending && bCanSubmitCombinedDemand)
		{
			const bool bAlreadySortedUnique = CachedTreeCombinedExplicitSplatRefs.IsValid() &&
				CachedTreeCombinedPageIds.IsValid() && CachedTreeCombinedSplatRuns.IsValid();
			TArray<NanoGS::Paged::FExplicitSplatRef> Refs;
			TArray<uint64> PageIds;
			TArray<NanoGS::Paged::FExplicitSplatRun> SplatRuns;
			if (bAlreadySortedUnique)
			{
				Refs = *CachedTreeCombinedExplicitSplatRefs;
				PageIds = *CachedTreeCombinedPageIds;
				SplatRuns = *CachedTreeCombinedSplatRuns;
			}
			else if (!BuildExplicitSplatFrontier(
				*TreeManifest, CachedTreeCombinedFrontier, Refs, PageIds, SplatRuns))
			{
				UE_LOG(LogTemp, Error, TEXT("NanoGS Tree full-detail frontier preparation failed"));
				return;
			}
			uint64 CombinedDemandIdentity = 0xA000000000000000ull ^
				static_cast<uint64>(reinterpret_cast<UPTRINT>(this));
			if (CombinedDemandIdentity == 0)
				CombinedDemandIdentity = 1;
			const NanoGS::Paged::EExactHQStreamingState StateBeforeRequest =
				PagedRenderData->GetStreamingState();
			PagedRenderData->RequestExplicitSplats_RenderThread(
				MoveTemp(Refs), MoveTemp(PageIds), MoveTemp(SplatRuns),
				CombinedDemandIdentity, false, true);
			bTreeCombinedDemandHasSubmitted = true;
			if (StateBeforeRequest != NanoGS::Paged::EExactHQStreamingState::Loading &&
				StateBeforeRequest != NanoGS::Paged::EExactHQStreamingState::Draining)
			{
				bTreeCombinedDemandSubmissionPending = false;
			}
		}
		if (!bTreeCombinedDemandFullDetail && bTreeCombinedDemandSubmissionPending &&
			bCanSubmitCombinedDemand && CachedTreeCombinedExplicitSplatRefs.IsValid() &&
			CachedTreeCombinedPageIds.IsValid() && CachedTreeCombinedSplatRuns.IsValid())
		{
			uint64 CombinedDemandIdentity = 0xA000000000000000ull ^
				static_cast<uint64>(reinterpret_cast<UPTRINT>(this));
			if (CombinedDemandIdentity == 0)
			{
				CombinedDemandIdentity = 1;
			}
			PagedRenderData->RequestExplicitSplats_RenderThread(
				TArray<NanoGS::Paged::FExplicitSplatRef>(*CachedTreeCombinedExplicitSplatRefs),
				TArray<uint64>(*CachedTreeCombinedPageIds),
				TArray<NanoGS::Paged::FExplicitSplatRun>(*CachedTreeCombinedSplatRuns),
				CombinedDemandIdentity, false, true);
			bTreeCombinedDemandHasSubmitted = true;
			bTreeCombinedDemandSubmissionPending = false;
			RecentDemand.bProgressiveSubmitted = false;
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree target submitted before fallback resolution: targets=%d pages=%d"),
					CachedTreeCombinedFrontier.Num(), CachedTreeCombinedPageIds->Num());
			}
		}

		if (!bTreeCombinedDemandFullDetail && PendingTreeTransition.IsValid() &&
			PendingTreeTransition->bComplete.load(std::memory_order_acquire))
		{
			const TSharedPtr<FTreeTransitionTaskResult, ESPMode::ThreadSafe> CompletedTransition =
				PendingTreeTransition;
			if (CompletedTransition->TargetGeneration != TreeCombinedTargetGeneration)
			{
				PendingTreeTransition.Reset();
			}
			else if (!bTreeCombinedDemandSubmissionPending || bCanSubmitCombinedDemand)
			{
				if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
				{
					UE_LOG(LogTemp, Display,
						TEXT("NanoGS Tree transition prepared: target-nodes=%d request-pages=%d render-splats=%d render-pages=%d capacity-ok=%d residency-generation=%u refined-topology=%d resolve=%.3f ms"),
						CachedTreeCombinedFrontier.Num(), CompletedTransition->RequestedPageIds.Num(),
						CompletedTransition->RenderSplats.Num(), CompletedTransition->RenderPageIds.Num(),
						CompletedTransition->bFallbackCapacityAvailable ? 1 : 0,
						CompletedTransition->PageResidencyGeneration,
						CompletedTransition->bUsedRefinedTopology ? 1 : 0,
						CompletedTransition->ResolutionMilliseconds);
				}
				if (bTreeCombinedDemandSubmissionPending)
				{
					TArray<NanoGS::Paged::FExplicitSplatRef> Refs;
					TArray<uint64> TargetPageIds;
					TArray<NanoGS::Paged::FExplicitSplatRun> SplatRuns;
					if (CachedTreeCombinedExplicitSplatRefs.IsValid() &&
						CachedTreeCombinedPageIds.IsValid() && CachedTreeCombinedSplatRuns.IsValid())
					{
						Refs = *CachedTreeCombinedExplicitSplatRefs;
						TargetPageIds = *CachedTreeCombinedPageIds;
						SplatRuns = *CachedTreeCombinedSplatRuns;
					}
					else if (!BuildExplicitSplatFrontier(
						*TreeManifest, CachedTreeCombinedFrontier, Refs, TargetPageIds, SplatRuns))
					{
						UE_LOG(LogTemp, Error, TEXT("NanoGS Tree target frontier preparation failed"));
						return;
					}
					TArray<uint64> RequestedPageIds = CompletedTransition->RequestedPageIds;
					if (RequestedPageIds.IsEmpty())
					{
						RequestedPageIds = TargetPageIds;
					}
					if (bTreeHasCaptureDemand && CachedTreeCapturePageIds.IsValid())
					{
						RequestedPageIds.Append(*CachedTreeCapturePageIds);
						SortUniquePageIds(RequestedPageIds);
					}
					uint64 CombinedDemandIdentity = 0xA000000000000000ull ^
						static_cast<uint64>(reinterpret_cast<UPTRINT>(this));
					if (CombinedDemandIdentity == 0)
						CombinedDemandIdentity = 1;
					const NanoGS::Paged::EExactHQStreamingState StateBeforeRequest =
						PagedRenderData->GetStreamingState();
					PagedRenderData->RequestExplicitSplats_RenderThread(
						MoveTemp(Refs), MoveTemp(RequestedPageIds), MoveTemp(SplatRuns),
						CombinedDemandIdentity, false, true);
					bTreeCombinedDemandHasSubmitted = true;
					if (StateBeforeRequest != NanoGS::Paged::EExactHQStreamingState::Loading &&
						StateBeforeRequest != NanoGS::Paged::EExactHQStreamingState::Draining)
					{
						bTreeCombinedDemandSubmissionPending = false;
					}
				}

				const uint32 MissingTargetPages =
					PagedRenderData->GetMissingTargetPageCount_RenderThread();
				const uint32 CurrentActiveSplats = PagedRenderData->GetActiveSplatCount();
				const uint32 MinMissingPages = static_cast<uint32>(FMath::Max(
					0, CVarNanoGSTreeResidentProgressiveMinMissingPages.GetValueOnRenderThread()));
				const uint32 MinSplatGain = static_cast<uint32>(FMath::Max(
					0, CVarNanoGSTreeResidentProgressiveMinSplatGain.GetValueOnRenderThread()));
				const bool bHasUsefulSplatGain =
					static_cast<uint64>(CompletedTransition->RenderSplats.Num()) >=
					static_cast<uint64>(CurrentActiveSplats) + MinSplatGain;
				const bool bResidentProgressiveWorthPublishing =
					CompletedTransition->bUsedRefinedTopology &&
					MissingTargetPages >= MinMissingPages && bHasUsefulSplatGain;
				if (bTreeCombinedDemandHasSubmitted &&
					PagedRenderData->GetStreamingState() == NanoGS::Paged::EExactHQStreamingState::Loading &&
					!CompletedTransition->RenderSplats.IsEmpty() &&
					(!CompletedTransition->bUsedRefinedTopology || bResidentProgressiveWorthPublishing))
				{
					PagedRenderData->PublishResidentTreeFrontier_RenderThread(
						TArray<NanoGS::Paged::FExplicitSplatRef>(CompletedTransition->RenderSplats),
						TArray<uint64>(CompletedTransition->RenderPageIds),
						TArray<NanoGS::Paged::FExplicitSplatRun>(CompletedTransition->RenderRuns),
						true);
				}
				else if (CompletedTransition->bUsedRefinedTopology &&
					FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
				{
					UE_LOG(LogTemp, Display,
						TEXT("NanoGS Tree resident-local publication skipped: missing-pages=%u min-missing=%u render-splats=%d active=%u min-gain=%u state=%u"),
						MissingTargetPages, MinMissingPages, CompletedTransition->RenderSplats.Num(),
						CurrentActiveSplats, MinSplatGain,
						static_cast<uint32>(PagedRenderData->GetStreamingState()));
				}
				if (!bTreeCombinedDemandSubmissionPending)
				{
					PendingTreeTransition.Reset();
				}
			}
		}

		const bool bHasRefinedTransitionTopology =
			CachedTreeCombinedRefinedNodes.Num() == TreeManifest->Nodes.Num() &&
			CachedTreeCombinedDemandedNodes.Num() == TreeManifest->Nodes.Num();
		if (!bTreeCombinedDemandFullDetail && !CachedTreeCombinedFrontier.IsEmpty() &&
			(PagedRenderData->GetActiveSplatCount() == 0 || bHasRefinedTransitionTopology) &&
			!PendingTreeTransition.IsValid())
		{
			const uint32 PageResidencyGeneration = PagedRenderData->GetPageResidencyGeneration();
			const bool bNeedsInitialTransition = bTreeCombinedDemandSubmissionPending &&
				!bTreeCombinedDemandHasSubmitted;
			const uint32 MissingTargetPages =
				PagedRenderData->GetMissingTargetPageCount_RenderThread();
			const uint32 MinMissingPages = static_cast<uint32>(FMath::Max(
				0, CVarNanoGSTreeResidentProgressiveMinMissingPages.GetValueOnRenderThread()));
			const bool bNeedsResidencyRefresh = bTreeCombinedDemandHasSubmitted &&
				bHasRefinedTransitionTopology &&
				PagedRenderData->GetStreamingState() == NanoGS::Paged::EExactHQStreamingState::Loading &&
				PageResidencyGeneration != LastScheduledTreeResidencyGeneration &&
				MissingTargetPages >= MinMissingPages;
			if (bNeedsInitialTransition || bNeedsResidencyRefresh)
			{
				TArray<uint64> ResidentPageIds;
				PagedRenderData->GetResidentPageIds_RenderThread(ResidentPageIds);
				bool bAllTargetPagesResident = bNeedsInitialTransition &&
					CachedTreeCombinedExplicitSplatRefs.IsValid() &&
					CachedTreeCombinedPageIds.IsValid() && CachedTreeCombinedSplatRuns.IsValid();
				if (bAllTargetPagesResident)
				{
					TSet<uint64> ResidentPages;
					ResidentPages.Reserve(ResidentPageIds.Num());
					for (const uint64 PageId : ResidentPageIds)
					{
						ResidentPages.Add(PageId);
					}
					for (const uint64 PageId : *CachedTreeCombinedPageIds)
					{
						if (!ResidentPages.Contains(PageId))
						{
							bAllTargetPagesResident = false;
							break;
						}
					}
				}
				if (bAllTargetPagesResident)
				{
					uint64 CombinedDemandIdentity = 0xA000000000000000ull ^
						static_cast<uint64>(reinterpret_cast<UPTRINT>(this));
					if (CombinedDemandIdentity == 0)
					{
						CombinedDemandIdentity = 1;
					}
					PagedRenderData->RequestExplicitSplats_RenderThread(
						TArray<NanoGS::Paged::FExplicitSplatRef>(*CachedTreeCombinedExplicitSplatRefs),
						TArray<uint64>(*CachedTreeCombinedPageIds),
						TArray<NanoGS::Paged::FExplicitSplatRun>(*CachedTreeCombinedSplatRuns),
						CombinedDemandIdentity, false, true);
					bTreeCombinedDemandHasSubmitted = true;
					bTreeCombinedDemandSubmissionPending = false;
					LastScheduledTreeResidencyGeneration = PageResidencyGeneration;
					if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
					{
						UE_LOG(LogTemp, Display,
							TEXT("NanoGS Tree warm-resident frontier submitted directly: targets=%d pages=%d residency-generation=%u"),
							CachedTreeCombinedFrontier.Num(), CachedTreeCombinedPageIds->Num(),
							PageResidencyGeneration);
					}
				}
				else
				{
				const NanoGS::Tree::FNode& RootNode = TreeManifest->Nodes[
					static_cast<uint32>(TreeManifest->Header.RootNode)];
				if (bTreeCombinedDemandHasSubmitted &&
					PagedRenderData->GetStreamingState() == NanoGS::Paged::EExactHQStreamingState::Loading &&
					PagedRenderData->GetActiveSplatCount() == 0 &&
					ResidentPageIds.Contains(RootNode.PageId))
				{
					TArray<NanoGS::Paged::FExplicitSplatRef> RootRefs = {
						{RootNode.PageId, RootNode.PageLocal}};
					TArray<uint64> RootPages = {RootNode.PageId};
					TArray<NanoGS::Paged::FExplicitSplatRun> RootRuns = {
						{RootNode.PageId, RootNode.PageLocal, 1u}};
					PagedRenderData->PublishResidentTreeFrontier_RenderThread(
						MoveTemp(RootRefs), MoveTemp(RootPages), MoveTemp(RootRuns), true);
				}
				LastScheduledTreeResidencyGeneration = PageResidencyGeneration;
				TSharedPtr<FTreeTransitionTaskResult, ESPMode::ThreadSafe> TaskResult =
					MakeShared<FTreeTransitionTaskResult, ESPMode::ThreadSafe>();
				TaskResult->TargetGeneration = TreeCombinedTargetGeneration;
				TaskResult->PageResidencyGeneration = PageResidencyGeneration;
				PendingTreeTransition = TaskResult;
				TSharedPtr<NanoGS::Tree::FManifest, ESPMode::ThreadSafe> ManifestSnapshot = TreeManifest;
				TSharedPtr<TArray<uint32>, ESPMode::ThreadSafe> ParentsSnapshot = TreeParents;
				TArray<uint32> TargetNodes = CachedTreeCombinedFrontier;
				TBitArray<> RefinedNodes = CachedTreeCombinedRefinedNodes;
				TBitArray<> DemandedNodes = CachedTreeCombinedDemandedNodes;
				TArray<uint64> TargetPageIds;
				if (CachedTreeCombinedPageIds.IsValid())
				{
					TargetPageIds = *CachedTreeCombinedPageIds;
				}
				TArray<uint64> PublishedPageIds;
				PagedRenderData->GetPublishedPageIds_RenderThread(PublishedPageIds);
				const uint32 PageCapacity = PagedRenderData->GetPhysicalSlotCount();
				AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
					[ManifestSnapshot, ParentsSnapshot, TargetNodes = MoveTemp(TargetNodes),
					 RefinedNodes = MoveTemp(RefinedNodes), DemandedNodes = MoveTemp(DemandedNodes),
					 TargetPageIds = MoveTemp(TargetPageIds), PublishedPageIds = MoveTemp(PublishedPageIds),
					 ResidentPageIds = MoveTemp(ResidentPageIds), PageCapacity, TaskResult]() mutable
					{
						const double ResolutionStartSeconds = FPlatformTime::Seconds();
						const bool bHasRefinedTopology =
							RefinedNodes.Num() == ManifestSnapshot->Nodes.Num() &&
							DemandedNodes.Num() == ManifestSnapshot->Nodes.Num() &&
							!TargetPageIds.IsEmpty();
						TaskResult->bUsedRefinedTopology = bHasRefinedTopology;
						if (bHasRefinedTopology)
						{
							TaskResult->RequestedPageIds = TargetPageIds;
							TaskResult->bFallbackCapacityAvailable =
								TargetPageIds.Num() <= static_cast<int32>(PageCapacity);
							TArray<uint64> ProtectedPageIds = TargetPageIds;
							ProtectedPageIds.Append(PublishedPageIds);
							SortUniquePageIds(ProtectedPageIds);
							TArray<uint32> RenderNodes;
							if (TaskResult->bFallbackCapacityAvailable)
							{
								ResolveRefinedResidentFrontier(
									*ManifestSnapshot, RefinedNodes, DemandedNodes,
									ResidentPageIds, ProtectedPageIds, RenderNodes);
							}
							if (!RenderNodes.IsEmpty())
							{
								BuildExplicitSplatFrontier(
									*ManifestSnapshot, RenderNodes, TaskResult->RenderSplats,
									TaskResult->RenderPageIds, TaskResult->RenderRuns);
							}
						}
						else
						{
							TaskResult->bFallbackCapacityAvailable = BuildTransitionRequestPages(
								*ManifestSnapshot, *ParentsSnapshot, TargetNodes, ResidentPageIds,
								PageCapacity, TaskResult->RequestedPageIds);
							if (TaskResult->bFallbackCapacityAvailable)
							{
								TSet<uint64> ResidentPages;
								ResidentPages.Reserve(ResidentPageIds.Num());
								for (const uint64 PageId : ResidentPageIds)
									ResidentPages.Add(PageId);
								TSet<uint64> RequestedPages;
								RequestedPages.Reserve(TaskResult->RequestedPageIds.Num());
								for (const uint64 PageId : TaskResult->RequestedPageIds)
									RequestedPages.Add(PageId);
								TArray<uint32> IgnoredRequestedNodes;
								TArray<uint32> RenderNodes;
								NanoGS::Tree::FSelector::ResolveTransitionFrontier(
									*ManifestSnapshot, *ParentsSnapshot, TargetNodes,
									[&ManifestSnapshot, &ResidentPages, &RequestedPages](const uint32 NodeIndex)
									{
										const uint64 PageId = ManifestSnapshot->Nodes[NodeIndex].PageId;
										return ResidentPages.Contains(PageId) && RequestedPages.Contains(PageId);
									},
									IgnoredRequestedNodes, RenderNodes);
								if (!RenderNodes.IsEmpty())
								{
									BuildExplicitSplatFrontier(
										*ManifestSnapshot, RenderNodes, TaskResult->RenderSplats,
										TaskResult->RenderPageIds, TaskResult->RenderRuns);
								}
							}
						}
						TaskResult->ResolutionMilliseconds =
							(FPlatformTime::Seconds() - ResolutionStartSeconds) * 1000.0;
						TaskResult->bComplete.store(true, std::memory_order_release);
					});
				}
			}
		}
		if (bTreeHasCaptureDemand && CachedTreeCaptureExplicitSplatRefs.IsValid() &&
			CachedTreeCapturePageIds.IsValid() && CachedTreeCaptureSplatRuns.IsValid() &&
			!PagedRenderData->IsCaptureFrontierReady())
		{
			TArray<uint64> ResidentPageIds;
			PagedRenderData->GetResidentPageIds_RenderThread(ResidentPageIds);
			TSet<uint64> ResidentPages;
			ResidentPages.Reserve(ResidentPageIds.Num());
			for (const uint64 PageId : ResidentPageIds)
			{
				ResidentPages.Add(PageId);
			}
			bool bAllCapturePagesResident = true;
			for (const uint64 PageId : *CachedTreeCapturePageIds)
			{
				if (!ResidentPages.Contains(PageId))
				{
					bAllCapturePagesResident = false;
					break;
				}
			}
			if (bAllCapturePagesResident)
			{
				PagedRenderData->PublishResidentCaptureTreeFrontier_RenderThread(
					TArray<NanoGS::Paged::FExplicitSplatRef>(*CachedTreeCaptureExplicitSplatRefs),
					TArray<uint64>(*CachedTreeCapturePageIds),
					TArray<NanoGS::Paged::FExplicitSplatRun>(*CachedTreeCaptureSplatRuns),
					true);
			}
		}

		if (TreeRuntimeStats.IsValid())
		{
			TreeRuntimeStats->ActiveSplatCount.store(
				static_cast<int32>(PagedRenderData->GetActiveSplatCount()), std::memory_order_release);
			TreeRuntimeStats->LoadedPageCount.store(
				static_cast<int32>(PagedRenderData->GetLoadedPageCount()), std::memory_order_release);
			TreeRuntimeStats->RequestedPageCount.store(
				static_cast<int32>(PagedRenderData->GetRequestedPageCount()), std::memory_order_release);
		}
		return;
	}
	if (!SpatialLODManifest.IsValid())
	{
		PagedRenderData->RequestExactHQForViewFamily_RenderThread(
			ViewFamily, FTransform(GetLocalToWorld()), static_cast<double>(SplatScale), StableCaptureIdentity);
		return;
	}

	uint64 DemandIdentity = StableCaptureIdentity != 0
		? (0x8000000000000000ull ^ StableCaptureIdentity)
		: (0x4000000000000000ull ^ static_cast<uint64>(reinterpret_cast<UPTRINT>(ViewFamily.RenderTarget)));
	if (DemandIdentity == 0x4000000000000000ull && !ViewFamily.Views.IsEmpty() && ViewFamily.Views[0] != nullptr)
	{
		DemandIdentity ^= static_cast<uint64>(ViewFamily.Views[0]->GetViewKey()) + 1ull;
	}

	const FSceneView* RepresentativeView = nullptr;
	for (const FSceneView* View : ViewFamily.Views)
	{
		if (View != nullptr)
		{
			RepresentativeView = View;
			break;
		}
	}
	if (RepresentativeView == nullptr)
		return;

	constexpr double MinResidencyUpdateIntervalSeconds = 0.5;
	constexpr double ResidencyViewRetentionSeconds = 5.0;
	constexpr double MovementThresholdCentimeters = 1000.0;
	constexpr double DirectionDotThreshold = 0.9993908270190958; // cos(2 degrees)
	const double NowSeconds = FPlatformTime::Seconds();
	const FVector RepresentativeOrigin = RepresentativeView->ViewMatrices.GetViewOrigin();
	const FVector RepresentativeDirection = RepresentativeView->GetViewDirection().GetSafeNormal();
	const bool bForceSpatialL0 = FParse::Param(FCommandLine::Get(), TEXT("NanoGSSpatialLODForceL0"));
	FRecentSpatialLODDemand& RecentDemand = RecentSpatialLODDemands.FindOrAdd(DemandIdentity);
	const bool bMovedEnough = FVector::DistSquared(RepresentativeOrigin, RecentDemand.ViewOrigin) >=
		FMath::Square(MovementThresholdCentimeters);
	const bool bTurnedEnough = FVector::DotProduct(
		RepresentativeDirection, RecentDemand.ViewDirection.GetSafeNormal()) < DirectionDotThreshold;
	const bool bIntervalElapsed = NowSeconds - RecentDemand.LastUpdateSeconds >= MinResidencyUpdateIntervalSeconds;
	const bool bRebuildDemand = !RecentDemand.bInitialized ||
		(!bForceSpatialL0 && bIntervalElapsed && (bMovedEnough || bTurnedEnough));
	RecentDemand.LastSeenSeconds = NowSeconds;

	if (bRebuildDemand)
	{
		TArray<NanoGS::SpatialLOD::FViewObservations> SpatialViews;
		SpatialViews.Reserve(ViewFamily.Views.Num());
		const FTransform LocalToWorld(GetLocalToWorld());
		for (const FSceneView* View : ViewFamily.Views)
		{
			if (View == nullptr)
				continue;
			NanoGS::SpatialLOD::FViewObservations& SpatialView = SpatialViews.AddDefaulted_GetRef();
			const FIntPoint ViewSize = View->UnscaledViewRect.Size();
			const FMatrix Projection = View->ViewMatrices.GetProjectionNoAAMatrix();
			const double FocalPixels = FMath::Max(
				FMath::Abs(static_cast<double>(Projection.M[0][0])) * 0.5 * ViewSize.X,
				FMath::Abs(static_cast<double>(Projection.M[1][1])) * 0.5 * ViewSize.Y);
			const FVector ViewOrigin = View->ViewMatrices.GetViewOrigin();

			for (uint32 NodeIndex = 0; NodeIndex < static_cast<uint32>(SpatialLODManifest->Nodes.Num()); ++NodeIndex)
			{
				const NanoGS::SpatialLOD::FNode& Node = SpatialLODManifest->Nodes[NodeIndex];
				if (!Node.IsLeaf() || Node.IsEnvironment())
					continue;
				const FBox LocalBox(FVector(Node.BoundsMin) * 100.0, FVector(Node.BoundsMax) * 100.0);
				const FBox WorldBox = LocalBox.TransformBy(LocalToWorld);
				const FBoxSphereBounds WorldBounds(WorldBox);
				double ProjectedRadiusPx = std::numeric_limits<double>::infinity();
				if (!bForceSpatialL0 && View->IsPerspectiveProjection() && ViewSize.X > 0 && ViewSize.Y > 0 &&
					FMath::IsFinite(FocalPixels) && FocalPixels > 0.0)
				{
					const double ConservativeDistance = FMath::Sqrt(
						WorldBox.ComputeSquaredDistanceToPoint(ViewOrigin));
					const double ConservativeDepth = FMath::Max(
						1.0, ConservativeDistance - static_cast<double>(WorldBounds.SphereRadius));
					ProjectedRadiusPx = FocalPixels * WorldBounds.SphereRadius / ConservativeDepth;
				}
				SpatialView.Nodes.Add({NodeIndex, static_cast<float>(ProjectedRadiusPx), true});
			}
		}

		const NanoGS::SpatialLOD::FSelection Selection = SpatialLODSelector.Select(*SpatialLODManifest, SpatialViews);
		RecentDemand.LevelsByNode.Reset();
		RecentDemand.CertifiedLevelsByNode.Reset();
		for (const NanoGS::SpatialLOD::FNodeSelection& NodeSelection : Selection.Nodes)
		{
			RecentDemand.LevelsByNode.Add(
				NodeSelection.NodeIndex, bForceSpatialL0 ? 0u : NodeSelection.Level);
			RecentDemand.CertifiedLevelsByNode.Add(
				NodeSelection.NodeIndex, bForceSpatialL0 ? 0u : NodeSelection.CertifiedLevel);
		}
		RecentDemand.ViewOrigin = RepresentativeOrigin;
		RecentDemand.ViewDirection = RepresentativeDirection;
		RecentDemand.LastUpdateSeconds = NowSeconds;
		RecentDemand.bInitialized = true;
	}

	for (auto It = RecentSpatialLODDemands.CreateIterator(); It; ++It)
	{
		if (It.Key() != DemandIdentity && NowSeconds - It.Value().LastSeenSeconds > ResidencyViewRetentionSeconds)
		{
			It.RemoveCurrent();
		}
	}

	TMap<uint32, uint32> CombinedLevelsByNode;
	TMap<uint32, uint32> CombinedCertifiedLevelsByNode;
	for (const TPair<uint64, FRecentSpatialLODDemand>& Demand : RecentSpatialLODDemands)
	{
		for (const TPair<uint32, uint32>& NodeLevel : Demand.Value.LevelsByNode)
		{
			uint32& CombinedLevel = CombinedLevelsByNode.FindOrAdd(NodeLevel.Key, NodeLevel.Value);
			CombinedLevel = FMath::Min(CombinedLevel, NodeLevel.Value);
		}
		for (const TPair<uint32, uint32>& NodeLevel : Demand.Value.CertifiedLevelsByNode)
		{
			uint32& CombinedLevel = CombinedCertifiedLevelsByNode.FindOrAdd(NodeLevel.Key, NodeLevel.Value);
			CombinedLevel = FMath::Min(CombinedLevel, NodeLevel.Value);
		}
	}
	const int32 ConfiguredBudget = CVarNanoGSSpatialLODGlobalBudget.GetValueOnRenderThread();
	const uint64 GlobalSplatBudget = ConfiguredBudget > 0 ? static_cast<uint64>(ConfiguredBudget) : 0;
	NanoGS::SpatialLOD::FSoftBudgetResult BudgetResult;
	if (GlobalSplatBudget > 0)
	{
		BudgetResult = NanoGS::SpatialLOD::FSelector::ApplySoftBudget(
			*SpatialLODManifest, CombinedCertifiedLevelsByNode, GlobalSplatBudget, CombinedLevelsByNode);
		if (BudgetResult.bBudgetExceeded && FParse::Param(FCommandLine::Get(), TEXT("NanoGSSpatialLODPerf")))
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("NanoGS SpatialLOD soft budget exceeded safely: selected=%llu certified-min=%llu budget=%llu"),
				static_cast<unsigned long long>(BudgetResult.SelectedSplatCount),
				static_cast<unsigned long long>(BudgetResult.CertifiedMinimumSplatCount),
				static_cast<unsigned long long>(GlobalSplatBudget));
		}
	}

	const bool bSafeTransitionsEnabled = CVarNanoGSSpatialLODSafeTransitions.GetValueOnRenderThread() != 0;
	const NanoGS::Paged::EExactHQStreamingState StreamingStateBeforeRequest = PagedRenderData->GetStreamingState();
	if (StreamingStateBeforeRequest == NanoGS::Paged::EExactHQStreamingState::Ready &&
		!LastSubmittedSpatialLODLevels.IsEmpty())
	{
		LastPublishedSpatialLODLevels = LastSubmittedSpatialLODLevels;
	}

	TMap<uint32, uint32> SubmittedLevelsByNode = CombinedLevelsByNode;
	bool bHasRefinement = false;
	bool bHasCoarsening = false;
	if (bSafeTransitionsEnabled && LastPublishedSpatialLODLevels.Num() == SubmittedLevelsByNode.Num())
	{
		for (const TPair<uint32, uint32>& PublishedLevel : LastPublishedSpatialLODLevels)
		{
			const uint32* DesiredLevel = SubmittedLevelsByNode.Find(PublishedLevel.Key);
			if (DesiredLevel == nullptr)
			{
				bHasRefinement = true;
				break;
			}
			bHasRefinement |= *DesiredLevel < PublishedLevel.Value;
			bHasCoarsening |= *DesiredLevel > PublishedLevel.Value;
		}
		if (bHasRefinement && bHasCoarsening)
		{
			for (const TPair<uint32, uint32>& PublishedLevel : LastPublishedSpatialLODLevels)
			{
				uint32& SubmittedLevel = SubmittedLevelsByNode.FindChecked(PublishedLevel.Key);
				if (SubmittedLevel > PublishedLevel.Value)
				{
					SubmittedLevel = PublishedLevel.Value;
				}
			}
		}
	}
	const bool bPublishedFrontierRemainsCaptureSafe =
		bSafeTransitionsEnabled && !bHasRefinement && bHasCoarsening;

	TArray<uint64> PageIds;
	PageIds.Reserve(SubmittedLevelsByNode.Num());
	for (const TPair<uint32, uint32>& NodeLevel : SubmittedLevelsByNode)
	{
		int32 PageId = SpatialLODManifest->FindPageId(NodeLevel.Key, NodeLevel.Value);
		if (PageId == INDEX_NONE)
			PageId = SpatialLODManifest->FindPageId(NodeLevel.Key, 0);
		if (PageId != INDEX_NONE)
			PageIds.Add(static_cast<uint64>(PageId));
	}

	PageIds.Sort();
	if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSSpatialLODPerf")) && PageIds != LastSpatialLODPageIds)
	{
		int32 LevelCounts[5] = {};
		uint64 SelectedSplats = 0;
		for (const TPair<uint32, uint32>& NodeLevel : SubmittedLevelsByNode)
		{
			if (NodeLevel.Value < UE_ARRAY_COUNT(LevelCounts))
				++LevelCounts[NodeLevel.Value];
			if (const NanoGS::SpatialLOD::FNodeLodRange* Range = SpatialLODManifest->FindRange(NodeLevel.Key, NodeLevel.Value))
				SelectedSplats += Range->SplatCount;
		}
		UE_LOG(LogTemp, Log, TEXT("NanoGS SpatialLOD frontier: pages=%d splats=%llu levels=[%d,%d,%d,%d,%d] retained-views=%d mode=%s budget=%llu certified=%llu original=%llu steps=%u phase=%s"),
			PageIds.Num(), static_cast<unsigned long long>(SelectedSplats), LevelCounts[0], LevelCounts[1],
			LevelCounts[2], LevelCounts[3], LevelCounts[4], RecentSpatialLODDemands.Num(),
			bForceSpatialL0 ? TEXT("L0") : TEXT("dynamic"),
			static_cast<unsigned long long>(GlobalSplatBudget),
			static_cast<unsigned long long>(BudgetResult.CertifiedMinimumSplatCount),
			static_cast<unsigned long long>(BudgetResult.OriginalSplatCount),
			BudgetResult.CoarseningStepCount,
			bHasRefinement && bHasCoarsening ? TEXT("refine-first") :
				(bPublishedFrontierRemainsCaptureSafe ? TEXT("safe-coarsen") : TEXT("direct")));
		LastSpatialLODPageIds = PageIds;
	}

	uint64 CombinedDemandIdentity = 0x2000000000000000ull ^
		static_cast<uint64>(reinterpret_cast<UPTRINT>(this));
	if (CombinedDemandIdentity == 0)
		CombinedDemandIdentity = 1;
	PagedRenderData->RequestExplicitPages_RenderThread(
		PageIds, CombinedDemandIdentity, bPublishedFrontierRemainsCaptureSafe);
	if (StreamingStateBeforeRequest != NanoGS::Paged::EExactHQStreamingState::Loading)
	{
		LastSubmittedSpatialLODLevels = MoveTemp(SubmittedLevelsByNode);
	}
}

bool FGaussianSplatSceneProxy::GetPagedCaptureSourceSnapshot(
	NanoGS::Paged::FCaptureSourceSnapshot& OutSnapshot) const
{
	using namespace NanoGS::Paged;
	OutSnapshot = FCaptureSourceSnapshot();
	if (bPendingDestruction.load(std::memory_order_acquire) || !PagedRenderData.IsValid())
	{
		return false;
	}

	auto ConvertState = [](const EExactHQStreamingState State)
	{
		switch (State)
		{
		case EExactHQStreamingState::AllResident:
			return ECaptureStreamingState::AllResident;
		case EExactHQStreamingState::NotRequested:
			return ECaptureStreamingState::NotRequested;
		case EExactHQStreamingState::Loading:
			return ECaptureStreamingState::Loading;
		case EExactHQStreamingState::Draining:
			return ECaptureStreamingState::Draining;
		case EExactHQStreamingState::Ready:
			return ECaptureStreamingState::Ready;
		case EExactHQStreamingState::OutOfCapacity:
			return ECaptureStreamingState::OutOfCapacity;
		case EExactHQStreamingState::Failed:
			return ECaptureStreamingState::Failed;
		default:
			return ECaptureStreamingState::Unavailable;
		}
	};

	// State/generation double-read makes races fail closed. The remaining fields
	// are atomics whose publication is ordered before the final Ready state store.
	const bool bStrictTreeCapture = TreeManifest.IsValid() && PagedRenderData->IsBoundedStreamingEnabled();
	const EExactHQStreamingState StateBefore = PagedRenderData->GetStreamingState();
	const uint32 GenerationBefore = PagedRenderData->GetResidentGeneration(bStrictTreeCapture);
	const bool bCaptureReadyBefore = !bStrictTreeCapture || PagedRenderData->IsCaptureFrontierReady();
	OutSnapshot.SourceIdentity = static_cast<uint64>(
		reinterpret_cast<UPTRINT>(PagedRenderData.Get()));
	OutSnapshot.bBounded = PagedRenderData->IsBoundedStreamingEnabled();
	if (bStrictTreeCapture)
	{
		const uint32 CapturePageCount = PagedRenderData->GetPublishedCapturePageCount();
		OutSnapshot.RequestedPageCount = bCaptureReadyBefore ? CapturePageCount : 1u;
		OutSnapshot.PublishedPageCount = bCaptureReadyBefore ? CapturePageCount : 0u;
		OutSnapshot.ActiveSplatCount = PagedRenderData->GetActiveSplatCount(true);
		OutSnapshot.bUploadComplete = bCaptureReadyBefore;
	}
	else
	{
		OutSnapshot.RequestedPageCount = PagedRenderData->GetRequestedPageCount();
		OutSnapshot.PublishedPageCount = PagedRenderData->GetPublishedPageCount();
		OutSnapshot.ActiveSplatCount = PagedRenderData->GetActiveSplatCount();
		OutSnapshot.bUploadComplete = PagedRenderData->IsUploadComplete();
	}
	OutSnapshot.bUploadFailed = PagedRenderData->HasUploadFailed();
	const uint32 GenerationAfter = PagedRenderData->GetResidentGeneration(bStrictTreeCapture);
	const bool bCaptureReadyAfter = !bStrictTreeCapture || PagedRenderData->IsCaptureFrontierReady();
	const EExactHQStreamingState StateAfter = PagedRenderData->GetStreamingState();
	OutSnapshot.ResidentGeneration = GenerationAfter;
	OutSnapshot.bStableAtomicSample = StateBefore == StateAfter &&
		GenerationBefore == GenerationAfter && bCaptureReadyBefore == bCaptureReadyAfter;
	OutSnapshot.StreamingState = OutSnapshot.bStableAtomicSample
		? (bStrictTreeCapture
			? (bCaptureReadyAfter ? ECaptureStreamingState::Ready : ECaptureStreamingState::Loading)
			: ConvertState(StateAfter))
		: ECaptureStreamingState::Unavailable;
	if (!bStrictTreeCapture && OutSnapshot.bStableAtomicSample &&
		StateAfter == EExactHQStreamingState::Loading &&
		PagedRenderData->CanCapturePublishedFrontierDuringTransition())
	{
		OutSnapshot.StreamingState = ECaptureStreamingState::Ready;
		OutSnapshot.RequestedPageCount = OutSnapshot.PublishedPageCount;
		OutSnapshot.bUploadComplete = true;
	}
	return true;
}

void FGaussianSplatSceneProxy::DestroyRenderThreadResources()
{
	// CRITICAL: Mark as pending destruction FIRST, before any other operations.
	// This prevents render commands that have already captured a pointer to this proxy
	// from accessing our resources. The atomic flag is checked by IsValidForRendering().
	MarkPendingDestruction();
	if (PagedRenderData.IsValid())
	{
		// Invalidate workers/queued page commands before the RHI flush. Stale
		// commands retain their payload safely but are forbidden from touching the pool.
		PagedRenderData->CancelUpload();
	}

	// Unregister from view extension (under lock, so no new render passes will see us)
	FGaussianSplatViewExtension* ViewExtension = FGaussianSplatViewExtension::Get();
	if (ViewExtension)
	{
		ViewExtension->UnregisterProxy(const_cast<FGaussianSplatSceneProxy*>(this));
	}

	// Flush any pending render commands that might be referencing our resources.
	// This ensures that any RDG passes that captured our proxy pointer have completed
	// before we release the GPU resources.
	// Note: We're already on the render thread, so this flushes GPU work.
	FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

	if (GPUResources)
	{
		GPUResources->ReleaseResource();
		delete GPUResources;
		GPUResources = nullptr;
	}
	if (PagedRenderData.IsValid())
	{
		PagedRenderData->ReleaseGPUBuffers();
		PagedRenderData.Reset();
	}
}
