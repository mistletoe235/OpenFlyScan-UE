// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSPagedRenderData.h"

#include "Paged/NanoGSPageReader.h"
#include "Paged/NanoGSSceneBudget.h"
#include "Paged/NanoGSPagedSourceAsset.h"
#include "Paged/NanoGSSpatialLODSourceAsset.h"
#include "Paged/NanoGSTreeSourceAsset.h"
#include "NanoGSRHICompat.h"
#include "GaussianSplatShaders.h"

#include "Async/Async.h"
#include "Algo/Unique.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "EngineGlobals.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "SceneView.h"
#include "SceneRendering.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "ShaderParameterUtils.h"

namespace NanoGS::Paged
{
	struct FSharedTreeGPUResources
	{
		FBufferRHIRef NodeBuffer;
		FShaderResourceViewRHIRef NodeBufferSRV;
		uint32 NodeCount = 0;
	};

	namespace
	{
		FCriticalSection GTreeGPUResourceCacheMutex;
		TMap<FString, TSharedPtr<FSharedTreeGPUResources, ESPMode::ThreadSafe>> GTreeGPUResourceCache;
		constexpr uint64 ExactHQBytesPerPhysicalSplat = 12ull + 28ull + 4ull + 96ull;
		constexpr uint64 ActiveIndexBytesPerSplat = sizeof(uint32);
		constexpr uint64 BoundedActiveIndexBufferCount = 4ull;

		TAutoConsoleVariable<int32> CVarNanoGSTreeGPUFrontier(
			TEXT("gs.TreeGPUFrontier"),
			0,
			TEXT("Experimental persistent GPU Tree frontier. 0 keeps the validated CPU Tree v2 path; 1 enables GPU resources."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSTreeGPUBlockLoadsPerUpdate(
			TEXT("gs.TreeGPUBlockLoadsPerUpdate"),
			16,
			TEXT("Maximum Tree v3 blocks scheduled from one GPU feedback batch (default 16)."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSTreeGPUBlockEvictionGraceEpochs(
			TEXT("gs.TreeGPUBlockEvictionGraceEpochs"),
			6,
			TEXT("Minimum completed GPU feedback epochs a resident Tree v3 block must remain unrequested before pressure eviction (default 6)."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSTreeGPUBlockPoolMB(
			TEXT("gs.TreeGPUBlockPoolMB"),
			256,
			TEXT("GPU memory budget in MiB for streamed Tree v3 node blocks (default 256)."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSTreeGPUPagePrefetchPerUpdate(
			TEXT("gs.TreeGPUPagePrefetchPerUpdate"),
			16,
			TEXT("Maximum missing Page v1 payloads admitted from one GPU frontier feedback batch (default 16)."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSTreeGPUFrontierMaxNodes(
			TEXT("gs.TreeGPUFrontierMaxNodes"),
			10000000,
			TEXT("Maximum persistent GPU Tree frontier entries per component (default 10000000)."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSTreeGPUFrontierPublish(
			TEXT("gs.TreeGPUFrontierPublish"),
			0,
			TEXT("Publish a converged, fully resident GPU Tree frontier. 0 keeps CPU rendering; 1 enables atomic GPU frontier publication."),
			ECVF_Default);

		TAutoConsoleVariable<float> CVarNanoGSTreeGPUFrontierPriorityStartPixels(
			TEXT("gs.TreeGPUFrontierPriorityStartPixels"),
			4096.0f,
			TEXT("Initial projected-error threshold for staged GPU frontier refinement (default 4096 pixels)."),
			ECVF_Default);

		TAutoConsoleVariable<float> CVarNanoGSTreeGPUFrontierPriorityDecay(
			TEXT("gs.TreeGPUFrontierPriorityDecay"),
			0.5f,
			TEXT("Per-pass multiplier for staged GPU frontier refinement threshold (default 0.5)."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSPagedPoolMB(
			TEXT("gs.PagedPoolMB"),
			4096,
			TEXT("Hard GPU source-pool budget in MiB for a NanoGS Page v1 component. "
				 "Exact-HQ uses all-resident storage when it fits and automatically falls back to "
				 "bounded complete-frontier streaming when it does not."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSPagedVerifyPayloadCRC(
			TEXT("gs.PagedVerifyPayloadCRC"),
			1,
			TEXT("Verify each Page v1 stream CRC before publishing its GPU slot (default 1)."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSPagedExperimentalBoundedStreaming(
			TEXT("gs.PagedExperimentalBoundedStreaming"),
			0,
			TEXT("Exact-HQ fixed-slot view streaming policy.\n")
			TEXT(" 0: auto; use all-resident when safe, otherwise bounded streaming\n")
			TEXT(" 1: force conservative per-view demand with atomic complete-frontier publication\n")
			TEXT("Changing this requires recreating the Gaussian scene proxy."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSPagedViewDemandTTLFrames(
			TEXT("gs.PagedViewDemandTTLFrames"),
			8,
			TEXT("Frames for which an independently rendered view/capture remains in the bounded Exact-HQ demand union (minimum 1)."),
			ECVF_RenderThreadSafe);

		TAutoConsoleVariable<float> CVarNanoGSPagedViewDemandTTLSeconds(
			TEXT("gs.PagedViewDemandTTLSeconds"),
			1.0f,
			TEXT("Minimum wall-clock lifetime for an independently rendered view/capture in the bounded Exact-HQ demand union (default 1.0 s). This prevents uncapped sensor servers from expiring another camera after only a few milliseconds."),
			ECVF_RenderThreadSafe);

		TAutoConsoleVariable<float> CVarNanoGSPagedDemandGuardPixels(
			TEXT("gs.PagedDemandGuardPixels"),
			2.0f,
			TEXT("Conservative bounded Exact-HQ raster guard in pixels (minimum/default 2). "
				 "Larger values prefetch outside the current raster and never omit an exact required page."),
			ECVF_RenderThreadSafe);

		TAutoConsoleVariable<float> CVarNanoGSPagedPageRetentionSeconds(
			TEXT("gs.PagedPageRetentionSeconds"),
			0.0f,
			TEXT("Keep recently required bounded Exact-HQ pages as conservative extras for this many seconds "
				 "(0 disables/default). Exact required pages always win when the fixed pool is full."),
			ECVF_RenderThreadSafe);

		TAutoConsoleVariable<float> CVarNanoGSPagedShrinkIntervalSeconds(
			TEXT("gs.PagedShrinkIntervalSeconds"),
			0.5f,
			TEXT("Minimum interval between pure-shrink bounded Exact-HQ frontier publications "
				 "(0 disables, default 0.5 s). Additions remain immediate; a held old frontier is an exact-safe superset."),
			ECVF_RenderThreadSafe);

		TAutoConsoleVariable<int32> CVarNanoGSPagedGPUActiveIndex(
			TEXT("gs.PagedGPUActiveIndex"),
			1,
			TEXT("Generate bounded Exact-HQ active->physical indices from compact page descriptors on GPU "
				 "(0 = CPU reference upload, 1 = GPU/default). Does not change the active set or ordering."),
			ECVF_RenderThreadSafe);

		TAutoConsoleVariable<int32> CVarNanoGSPagedIOWorkers(
			TEXT("gs.PagedIOWorkers"),
			4,
			TEXT("Bounded Exact-HQ page read/decode workers (1-8, default 4). "
				 "Each worker bounds CPU scratch to one decoded page plus its upload staging."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSPagedGlobalPoolMB(
			TEXT("gs.PagedGlobalPoolMB"),
			8192,
			TEXT("Scene-level hard budget in MiB for all NanoGS Page source and active-index pools. "
				 "Zero disables the global cap; a rejected source is not partially allocated."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSPagedGlobalResidentSlots(
			TEXT("gs.PagedGlobalResidentSlots"),
			0,
			TEXT("Optional scene-level cap for the sum of fixed Page resident slots. Zero is unlimited."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSPagedGlobalIOPages(
			TEXT("gs.PagedGlobalIOPages"),
			8,
			TEXT("Maximum Page read/decode/upload operations in flight across one scene. Zero is unlimited."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarNanoGSPagedGlobalMainActiveSplats(
			TEXT("gs.PagedGlobalMainActiveSplats"),
			0,
			TEXT("Optional scene-level main-view active-splat admission budget. Zero is unlimited; overflow fails closed."),
			ECVF_RenderThreadSafe);

		TAutoConsoleVariable<int32> CVarNanoGSPagedGlobalCaptureActiveSplats(
			TEXT("gs.PagedGlobalCaptureActiveSplats"),
			0,
			TEXT("Optional scene-level strict-capture active-splat admission budget. Zero is unlimited; overflow returns NotReady."),
			ECVF_RenderThreadSafe);

		bool CheckedMultiply(const uint64 A, const uint64 B, uint64& Out)
		{
			if (A != 0 && B > MAX_uint64 / A)
			{
				return false;
			}
			Out = A * B;
			return true;
		}

		FBufferRHIRef CreateRawBuffer(
			FRHICommandListBase& RHICmdList,
			const TCHAR* Name,
			const uint32 ByteCount)
		{
			return NanoGS::RHICompat::CreateBuffer(
				RHICmdList,
				Name,
				FMath::Max(ByteCount, 4u),
				0,
				BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer,
				ERHIAccess::SRVMask);
		}

		FShaderResourceViewRHIRef CreateRawSRV(
			FRHICommandListBase& RHICmdList,
			const FBufferRHIRef& Buffer)
		{
			return RHICmdList.CreateShaderResourceView(
				Buffer,
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Raw));
		}

		bool ToBufferBytes(
			const uint64 ElementCount,
			const uint64 Stride,
			const TCHAR* Label,
			uint32& OutBytes,
			FString& OutError)
		{
			uint64 Bytes = 0;
			if (!CheckedMultiply(ElementCount, Stride, Bytes) || Bytes == 0 || Bytes > MAX_uint32)
			{
				OutError = FString::Printf(
					TEXT("NanoGS Page %s buffer size is outside the uint32 RHI contract (%llu x %llu bytes)"),
					Label,
					static_cast<unsigned long long>(ElementCount),
					static_cast<unsigned long long>(Stride));
				return false;
			}
			OutBytes = static_cast<uint32>(Bytes);
			return true;
		}

		void SortUniquePageIds(TArray<uint64>& PageIds)
		{
			PageIds.Sort();
			if (PageIds.Num() < 2)
			{
				return;
			}

			int32 WriteIndex = 1;
			for (int32 ReadIndex = 1; ReadIndex < PageIds.Num(); ++ReadIndex)
			{
				if (PageIds[ReadIndex] != PageIds[WriteIndex - 1])
				{
					PageIds[WriteIndex++] = PageIds[ReadIndex];
				}
			}
			PageIds.SetNum(WriteIndex, EAllowShrinking::No);
		}

		bool ArePageIdArraysEqual(TConstArrayView<uint64> A, TConstArrayView<uint64> B)
		{
			return A.Num() == B.Num() &&
				(A.IsEmpty() || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(uint64)) == 0);
		}

		void SortUniqueSplatRefs(TArray<FExplicitSplatRef>& Refs)
		{
			Refs.Sort([](const FExplicitSplatRef& A, const FExplicitSplatRef& B)
			{
				return A.PageId < B.PageId || (A.PageId == B.PageId && A.PageLocal < B.PageLocal);
			});
			if (Refs.Num() < 2)
				return;
			int32 WriteIndex = 1;
			for (int32 ReadIndex = 1; ReadIndex < Refs.Num(); ++ReadIndex)
			{
				if (!(Refs[ReadIndex] == Refs[WriteIndex - 1]))
					Refs[WriteIndex++] = Refs[ReadIndex];
			}
			Refs.SetNum(WriteIndex, EAllowShrinking::No);
		}

		bool AreSplatRefArraysEqual(TConstArrayView<FExplicitSplatRef> A, TConstArrayView<FExplicitSplatRef> B)
		{
			if (A.Num() != B.Num())
				return false;
			for (int32 Index = 0; Index < A.Num(); ++Index)
			{
				if (!(A[Index] == B[Index]))
					return false;
			}
			return true;
		}

		bool AreSplatRunArraysEqual(TConstArrayView<FExplicitSplatRun> A, TConstArrayView<FExplicitSplatRun> B)
		{
			if (A.Num() != B.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < A.Num(); ++Index)
			{
				if (A[Index].PageId != B[Index].PageId ||
					A[Index].PageLocal != B[Index].PageLocal ||
					A[Index].Count != B[Index].Count)
				{
					return false;
				}
			}
			return true;
		}

		bool IsSortedPageIdSubset(TConstArrayView<uint64> Candidate, TConstArrayView<uint64> Superset)
		{
			int32 SupersetIndex = 0;
			for (const uint64 PageId : Candidate)
			{
				while (SupersetIndex < Superset.Num() && Superset[SupersetIndex] < PageId)
				{
					++SupersetIndex;
				}
				if (SupersetIndex >= Superset.Num() || Superset[SupersetIndex] != PageId)
				{
					return false;
				}
				++SupersetIndex;
			}
			return true;
		}
	}

	struct FPageRenderData::FPageUpload
	{
		uint64 PageId = 0;
		uint32 PageOrdinal = 0;
		uint32 PhysicalBase = 0;
		uint32 ActiveBase = 0;
		uint32 SplatCount = 0;
		FPageLoadOperation LoadOperation;
		// Keep the reader-owned SoA streams alive through the render command. Moving
		// the payload avoids duplicate Position/Color/SH staging copies; only the
		// Rotation+Scale streams need a new interleaved 28-byte GPU layout.
		FOwnedPagePayload Payload;
		TArray64<uint8> Other;
		TArray<uint32> ActivePhysicalIndices;
		double ReadMilliseconds = 0.0;
		double DecodeMilliseconds = 0.0;
		double EnqueueSeconds = 0.0;
		std::atomic<bool> bRenderCommandComplete{false};
	};

	FPageRenderData::FPageRenderData() = default;

	FPageRenderData::~FPageRenderData()
	{
		bStopRequested.store(true, std::memory_order_release);
		if (bSceneBudgetRegistered)
		{
			FSceneBudgetRegistry::Get().ReleaseOwner(SceneBudgetIdentity, SceneBudgetOwnerIdentity);
		}
	}

	void FPageRenderData::SetSceneBudgetIdentity(const uint64 SceneIdentity, const uint64 OwnerIdentity)
	{
		check(!bInitialized);
		SceneBudgetIdentity = SceneIdentity;
		SceneBudgetOwnerIdentity = OwnerIdentity;
	}

	bool FPageRenderData::Initialize(const UNanoGSPagedSourceAsset* Asset, FString& OutError)
	{
		OutError.Reset();
		if (bInitialized)
			return true;
		if (Asset == nullptr || !Asset->HasValidMetadata())
		{
			OutError = TEXT("NanoGS paged render data requires a valid UNanoGSPagedSourceAsset");
			return false;
		}
		FManifest LoadedManifest;
		FReadOptions Options;
		Options.bVerifyMetadataCRCs = true;
		Options.bVerifyPayloadCRCs = false;
		if (!Asset->OpenManifest(LoadedManifest, OutError, Options))
			return false;
		return InitializeManifest(MoveTemp(LoadedManifest), false, OutError);
	}

	bool FPageRenderData::Initialize(const UNanoGSSpatialLODSourceAsset* Asset, FString& OutError)
	{
		OutError.Reset();
		if (bInitialized)
			return true;
		if (Asset == nullptr || !Asset->HasValidMetadata())
		{
			OutError = TEXT("NanoGS paged render data requires a valid SpatialLOD source asset");
			return false;
		}
		FManifest LoadedManifest;
		if (!Asset->OpenPageManifest(LoadedManifest, OutError))
			return false;
		return InitializeManifest(MoveTemp(LoadedManifest), true, OutError);
	}

	bool FPageRenderData::Initialize(const UNanoGSTreeSourceAsset* Asset, FString& OutError)
	{
		OutError.Reset();
		if (bInitialized)
			return true;
		if (Asset == nullptr || !Asset->HasValidMetadata())
		{
			OutError = TEXT("NanoGS paged render data requires a valid Tree v2 source asset");
			return false;
		}
		const int64 ExactLeafCount = Asset->GetExactLeafCount();
		if (ExactLeafCount <= 0)
		{
			OutError = TEXT("NanoGS Tree v2 source has no exact-leaf boundary");
			return false;
		}
		FManifest LoadedManifest;
		if (!Asset->OpenPageManifest(LoadedManifest, OutError))
			return false;
		if (CVarNanoGSTreeGPUFrontier.GetValueOnAnyThread() != 0 ||
			FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreeGPUFrontier")))
		{
			FString TreeGPUError;
			NanoGS::TreeV3::FManifest LoadedTreeV3;
			if (NanoGS::TreeV3::FReader::ReadFromDirectory(
				Asset->GetResolvedDirectory(), LoadedTreeV3, TreeGPUError))
			{
				TreeGPUNodeUploadData = MoveTemp(LoadedTreeV3.SkeletonNodes);
				TreeGPUNodeCount = static_cast<uint32>(TreeGPUNodeUploadData.Num());
				TreeGPURootNode = 0;
				TreeV3Manifest = MakeUnique<NanoGS::TreeV3::FManifest>(MoveTemp(LoadedTreeV3));
				UE_LOG(LogTemp, Log,
					TEXT("NanoGS Tree v3 sidecar selected: %u skeleton nodes, %d streamable blocks"),
					TreeGPUNodeCount, TreeV3Manifest->Blocks.Num());
			}
			else
			{
				NanoGS::Tree::FManifest TreeManifest;
				if (Asset->OpenTreeManifest(TreeManifest, TreeGPUError))
				{
					TreeGPUNodeCount = static_cast<uint32>(TreeManifest.Nodes.Num());
					TreeGPURootNode = TreeManifest.Header.RootNode;
					const FString SharedKey = FString::Printf(
						TEXT("%s#%u"), *LoadedManifest.SourceFilename, TreeGPUNodeCount);
					{
						FScopeLock CacheLock(&GTreeGPUResourceCacheMutex);
						if (const TSharedPtr<FSharedTreeGPUResources, ESPMode::ThreadSafe>* Cached =
							GTreeGPUResourceCache.Find(SharedKey))
						{
							SharedTreeGPUResources = *Cached;
						}
					}
					if (!SharedTreeGPUResources.IsValid() &&
						!NanoGS::Tree::BuildGPUNodeTable(TreeManifest, TreeGPUNodeUploadData, TreeGPUError))
					{
						TreeGPUNodeCount = 0;
					}
				}
			}
			if (TreeGPUNodeCount == 0)
			{
				TreeGPUNodeUploadData.Reset();
				UE_LOG(LogTemp, Warning,
					TEXT("NanoGS Tree GPU node packing disabled; CPU Tree v2 remains active: %s"),
					*TreeGPUError);
			}
		}
		TreeExactLeafCount = static_cast<uint64>(ExactLeafCount);
		bTreeLODSource = true;
		return InitializeManifest(MoveTemp(LoadedManifest), true, OutError);
	}

	bool FPageRenderData::InitializeManifest(FManifest&& InManifest, const bool bForceBounded, FString& OutError)
	{
		OutError.Reset();
		if (bInitialized)
			return true;
		Manifest = MoveTemp(InManifest);
		if (Manifest.Header.TotalSplatCount > static_cast<uint64>(MAX_int32) ||
			Manifest.Header.PageCount > static_cast<uint64>(MAX_int32))
		{
			OutError = TEXT("NanoGS Page container exceeds the current renderer's signed 32-bit active-count contract");
			return false;
		}
		uint64 MaxPageSplats = 0;
		for (const FPageRecord& Page : Manifest.Pages)
		{
			MaxPageSplats = FMath::Max(MaxPageSplats, Page.SplatCount);
		}
		if (MaxPageSplats == 0 || MaxPageSplats > MAX_uint32)
		{
			OutError = TEXT("NanoGS Page slot capacity is outside the uint32 physical-index contract");
			return false;
		}

		int32 ConfiguredPoolMB = FMath::Max(1, CVarNanoGSPagedPoolMB.GetValueOnAnyThread());
		int32 CommandLinePoolMB = 0;
		if (FParse::Value(FCommandLine::Get(), TEXT("NanoGSPagedPoolMB="), CommandLinePoolMB))
		{
			ConfiguredPoolMB = FMath::Max(1, CommandLinePoolMB);
		}
		const uint64 BudgetBytes =
			static_cast<uint64>(ConfiguredPoolMB) * 1024ull * 1024ull;

		uint64 AllResidentCapacity = 0;
		uint64 AllResidentSourceBytes = 0;
		uint64 AllResidentActiveBytes = 0;
		const bool bAllResidentArithmeticSafe =
			CheckedMultiply(Manifest.Header.PageCount, MaxPageSplats, AllResidentCapacity) &&
			CheckedMultiply(AllResidentCapacity, ExactHQBytesPerPhysicalSplat, AllResidentSourceBytes) &&
			CheckedMultiply(
				Manifest.Header.TotalSplatCount,
				ActiveIndexBytesPerSplat,
				AllResidentActiveBytes) &&
			AllResidentSourceBytes <= MAX_uint64 - AllResidentActiveBytes;
		const bool bAllResidentRHIAddressable = bAllResidentArithmeticSafe &&
			AllResidentCapacity > 0 &&
			AllResidentCapacity <= static_cast<uint64>(MAX_int32) &&
			AllResidentCapacity <= static_cast<uint64>(MAX_uint32) / 12ull &&
			AllResidentCapacity <= static_cast<uint64>(MAX_uint32) / 28ull &&
			AllResidentCapacity <= static_cast<uint64>(MAX_uint32) / 4ull &&
			AllResidentCapacity <= static_cast<uint64>(MAX_uint32) / 96ull &&
			Manifest.Header.TotalSplatCount <= static_cast<uint64>(MAX_uint32) / 4ull;
		const bool bAllResidentFitsBudget = bAllResidentArithmeticSafe &&
			AllResidentSourceBytes + AllResidentActiveBytes <= BudgetBytes;
		const bool bForceBoundedStreaming = bForceBounded ||
			CVarNanoGSPagedExperimentalBoundedStreaming.GetValueOnAnyThread() != 0 ||
			FParse::Param(FCommandLine::Get(), TEXT("NanoGSBoundedStreaming"));
		bBoundedStreamingEnabled = bForceBoundedStreaming ||
			!bAllResidentRHIAddressable || !bAllResidentFitsBudget;
		if (bBoundedStreamingEnabled &&
			!DemandHierarchy.Initialize(Manifest.Pages, OutError))
		{
			return false;
		}
		if (bBoundedStreamingEnabled && !bForceBoundedStreaming)
		{
			UE_LOG(LogTemp, Log,
				TEXT("NanoGS Page Exact-HQ auto-selected bounded streaming: all-resident capacity is %llu splats / %.1f MiB, pool budget is %d MiB, RHI-addressable=%s."),
				static_cast<unsigned long long>(AllResidentCapacity),
				bAllResidentArithmeticSafe
					? double(AllResidentSourceBytes + AllResidentActiveBytes) / (1024.0 * 1024.0)
					: -1.0,
				ConfiguredPoolMB,
				bAllResidentRHIAddressable ? TEXT("yes") : TEXT("no"));
		}
		uint64 Capacity64 = 0;
		uint64 ActiveCapacity64 = 0;
		uint64 SlotCount64 = Manifest.Header.PageCount;

		if (bBoundedStreamingEnabled)
		{
			uint64 BytesPerSlot = 0;
			if (!CheckedMultiply(
				MaxPageSplats,
				ExactHQBytesPerPhysicalSplat +
					BoundedActiveIndexBufferCount * ActiveIndexBytesPerSplat,
				BytesPerSlot) ||
				BytesPerSlot == 0)
			{
				OutError = TEXT("NanoGS bounded Page slot byte calculation overflowed");
				return false;
			}
			const uint64 BudgetSlotCount = BudgetBytes / BytesPerSlot;
			// The 96-byte SH3 source is the widest single RHI allocation.  A large
			// user budget must not make an otherwise streamable 100M+ container fail
			// merely because the chosen fixed pool crosses UE's uint32 buffer-size
			// limit. Clamp the number of slots; Exact-HQ demand remains atomic and an
			// oversized view still reports OutOfCapacity rather than truncating.
			const uint64 MaxPhysicalSplatsByRHI = FMath::Min<uint64>(
				static_cast<uint64>(MAX_int32),
				static_cast<uint64>(MAX_uint32) / 96ull);
			const uint64 RHISafeSlotCount = MaxPhysicalSplatsByRHI / MaxPageSplats;
			SlotCount64 = FMath::Min(
				Manifest.Header.PageCount,
				FMath::Min(BudgetSlotCount, RHISafeSlotCount));
			if (SlotCount64 == 0)
			{
				OutError = FString::Printf(
					TEXT("Exact-HQ bounded Page pool cannot fit one %llu-splat slot in gs.PagedPoolMB=%d or the uint32 RHI buffer contract"),
					static_cast<unsigned long long>(MaxPageSplats),
					ConfiguredPoolMB);
				return false;
			}
			if (SlotCount64 < FMath::Min(Manifest.Header.PageCount, BudgetSlotCount))
			{
				UE_LOG(LogTemp, Warning,
					TEXT("NanoGS bounded Page pool capped at %llu slots / %llu splats by the uint32 RHI buffer contract; requested budget gs.PagedPoolMB=%d could otherwise fit %llu slots."),
					static_cast<unsigned long long>(SlotCount64),
					static_cast<unsigned long long>(SlotCount64 * MaxPageSplats),
					ConfiguredPoolMB,
					static_cast<unsigned long long>(BudgetSlotCount));
			}
			if (!CheckedMultiply(SlotCount64, MaxPageSplats, Capacity64))
			{
				OutError = TEXT("NanoGS bounded Page physical capacity overflowed");
				return false;
			}
			ActiveCapacity64 = Capacity64;
		}
		else
		{
			if (!CheckedMultiply(Manifest.Header.PageCount, MaxPageSplats, Capacity64))
			{
				OutError = TEXT("NanoGS all-resident Page physical capacity overflowed");
				return false;
			}
			ActiveCapacity64 = Manifest.Header.TotalSplatCount;
		}

		if (Capacity64 == 0 || Capacity64 > static_cast<uint64>(MAX_int32) ||
			ActiveCapacity64 == 0 || ActiveCapacity64 > static_cast<uint64>(MAX_int32) ||
			SlotCount64 == 0 || SlotCount64 > MAX_uint32)
		{
			OutError = TEXT("NanoGS Page fixed pool exceeds the renderer's signed 32-bit source/index contract");
			return false;
		}

		uint64 SourceBytes = 0;
		uint64 ActiveBytesPerBuffer = 0;
		uint64 ActiveBytes = 0;
		if (!CheckedMultiply(Capacity64, ExactHQBytesPerPhysicalSplat, SourceBytes) ||
			!CheckedMultiply(ActiveCapacity64, ActiveIndexBytesPerSplat, ActiveBytesPerBuffer) ||
			!CheckedMultiply(
				ActiveBytesPerBuffer,
				bBoundedStreamingEnabled ? BoundedActiveIndexBufferCount : 1ull,
				ActiveBytes) ||
			SourceBytes > MAX_uint64 - ActiveBytes)
		{
			OutError = TEXT("NanoGS Page GPU-pool byte calculation overflowed");
			return false;
		}
		const uint64 RequiredBytes = SourceBytes + ActiveBytes;
		if (RequiredBytes > BudgetBytes)
		{
			OutError = FString::Printf(
				TEXT("Exact-HQ Page pool is out of capacity: needs %.1f MiB for %llu splats/%llu pages, budget gs.PagedPoolMB=%d. "
					 "No partial scene will be published; increase the budget enough to hold at least one complete page slot."),
				double(RequiredBytes) / (1024.0 * 1024.0),
				static_cast<unsigned long long>(Manifest.Header.TotalSplatCount),
				static_cast<unsigned long long>(Manifest.Header.PageCount),
				ConfiguredPoolMB);
			return false;
		}

		// Every individual RHI allocation still has a 32-bit byte-size contract.
		uint32 IgnoredBytes = 0;
		if (!ToBufferBytes(Capacity64, 12, TEXT("position"), IgnoredBytes, OutError) ||
			!ToBufferBytes(Capacity64, 28, TEXT("rotation/scale"), IgnoredBytes, OutError) ||
			!ToBufferBytes(Capacity64, 4, TEXT("color/opacity"), IgnoredBytes, OutError) ||
			!ToBufferBytes(Capacity64, 96, TEXT("SH3"), IgnoredBytes, OutError) ||
			!ToBufferBytes(ActiveCapacity64, 4, TEXT("active-index"), IgnoredBytes, OutError))
		{
			return false;
		}

		TotalSplatCount = static_cast<uint32>(Manifest.Header.TotalSplatCount);
		PageSlotCapacity = static_cast<uint32>(MaxPageSplats);
		SourceCapacity = static_cast<uint32>(Capacity64);
		ActiveIndexCapacity = static_cast<uint32>(ActiveCapacity64);
		PhysicalSlotCount = static_cast<uint32>(SlotCount64);
		ActiveIndexBufferCount = bBoundedStreamingEnabled
			? static_cast<uint8>(BoundedActiveIndexBufferCount)
			: 1u;
		PublishedActiveIndexBufferSlot = 0;
		PublishedCaptureActiveIndexBufferSlot = 2;
		SceneBounds = FBox(
			FVector(Manifest.Header.SceneBoundsMin),
			FVector(Manifest.Header.SceneBoundsMax));
		if (bBoundedStreamingEnabled)
		{
			TArray<FPageCatalogEntry> Catalog;
			Catalog.Reserve(Manifest.Pages.Num());
			for (const FPageRecord& Page : Manifest.Pages)
			{
				Catalog.Add({Page.PageId, Page.SplatCount});
			}
			FPageResidentPoolConfig PoolConfig;
			PoolConfig.SlotCount = SlotCount64;
			PoolConfig.SplatsPerSlot = MaxPageSplats;
			PoolConfig.KeepAliveGenerations = 2;
			if (!ResidentPool.Initialize(PoolConfig, Catalog, OutError))
			{
				return false;
			}
			StreamingState.store(
				static_cast<uint8>(EExactHQStreamingState::NotRequested),
				std::memory_order_release);
		}
		else
		{
			StreamingState.store(
				static_cast<uint8>(EExactHQStreamingState::AllResident),
				std::memory_order_release);
		}
		if (SceneBudgetIdentity != 0 && SceneBudgetOwnerIdentity != 0)
		{
			int32 ConfiguredGlobalPoolMB = FMath::Max(0, CVarNanoGSPagedGlobalPoolMB.GetValueOnAnyThread());
			FParse::Value(FCommandLine::Get(), TEXT("NanoGSPagedGlobalPoolMB="), ConfiguredGlobalPoolMB);
			ConfiguredGlobalPoolMB = FMath::Max(0, ConfiguredGlobalPoolMB);
			int32 ConfiguredGlobalResidentSlots = FMath::Max(
				0, CVarNanoGSPagedGlobalResidentSlots.GetValueOnAnyThread());
			FParse::Value(
				FCommandLine::Get(), TEXT("NanoGSPagedGlobalResidentSlots="), ConfiguredGlobalResidentSlots);
			ConfiguredGlobalResidentSlots = FMath::Max(0, ConfiguredGlobalResidentSlots);
			const uint64 GlobalPoolLimitBytes =
				static_cast<uint64>(ConfiguredGlobalPoolMB) * 1024ull * 1024ull;
			const uint64 GlobalResidentSlotLimit = static_cast<uint64>(ConfiguredGlobalResidentSlots);
			FSceneBudgetRegistry& BudgetRegistry = FSceneBudgetRegistry::Get();
			if (!BudgetRegistry.TrySetPoolBytes(
				SceneBudgetIdentity, SceneBudgetOwnerIdentity, RequiredBytes, GlobalPoolLimitBytes, OutError) ||
				!BudgetRegistry.TrySetResidentSlots(
					SceneBudgetIdentity, SceneBudgetOwnerIdentity, SlotCount64, GlobalResidentSlotLimit, OutError))
			{
				BudgetRegistry.ReleaseOwner(SceneBudgetIdentity, SceneBudgetOwnerIdentity);
				ResidentPool.Reset();
				return false;
			}
			bSceneBudgetRegistered = true;
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				const FSceneBudgetSnapshot Snapshot = BudgetRegistry.GetSnapshot(SceneBudgetIdentity);
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS scene budget reserve: scene=%llu owners=%u pool=%.1f MiB slots=%llu"),
					static_cast<unsigned long long>(SceneBudgetIdentity), Snapshot.OwnerCount,
					double(Snapshot.PoolBytes) / (1024.0 * 1024.0),
					static_cast<unsigned long long>(Snapshot.ResidentSlots));
			}
		}

		bInitialized = true;

		UE_LOG(LogTemp, Log,
			TEXT("NanoGS Page Exact-HQ manifest ready: %u splats, %d pages, %u splats/slot, %u fixed slots (%s), %.1f MiB GPU source+active pool"),
			TotalSplatCount,
			Manifest.Pages.Num(),
			PageSlotCapacity,
			PhysicalSlotCount,
			bBoundedStreamingEnabled ? TEXT("bounded complete-frontier") : TEXT("all-resident"),
			double(RequiredBytes) / (1024.0 * 1024.0));
		return true;
	}

	bool FPageRenderData::CreateGPUBuffers(FRHICommandListBase& RHICmdList, FString& OutError)
	{
		check(IsInRenderingThread());
		OutError.Reset();
		if (bGPUBuffersCreated)
		{
			return true;
		}
		if (!bInitialized || TotalSplatCount == 0 || SourceCapacity == 0)
		{
			OutError = TEXT("NanoGS Page render data has not been initialized");
			return false;
		}

		uint32 PositionBytes = 0;
		uint32 OtherBytes = 0;
		uint32 ColorBytes = 0;
		uint32 SHBytes = 0;
		uint32 ActiveBytes = 0;
		if (!ToBufferBytes(SourceCapacity, 12, TEXT("position"), PositionBytes, OutError) ||
			!ToBufferBytes(SourceCapacity, 28, TEXT("rotation/scale"), OtherBytes, OutError) ||
			!ToBufferBytes(SourceCapacity, 4, TEXT("color/opacity"), ColorBytes, OutError) ||
			!ToBufferBytes(SourceCapacity, 96, TEXT("SH3"), SHBytes, OutError) ||
			!ToBufferBytes(ActiveIndexCapacity, 4, TEXT("active-index"), ActiveBytes, OutError))
		{
			return false;
		}

		PackedSplatBuffer = CreateRawBuffer(RHICmdList, TEXT("NanoGSPagedPackedDummy"), 16);
		PackedSplatBufferSRV = CreateRawSRV(RHICmdList, PackedSplatBuffer);
		{
			void* Dest = RHICmdList.LockBuffer(PackedSplatBuffer, 0, 16, RLM_WriteOnly);
			FMemory::Memzero(Dest, 16);
			RHICmdList.UnlockBuffer(PackedSplatBuffer);
		}

		PositionBuffer = CreateRawBuffer(RHICmdList, TEXT("NanoGSPagedPositionPool"), PositionBytes);
		PositionBufferSRV = CreateRawSRV(RHICmdList, PositionBuffer);
		OtherDataBuffer = CreateRawBuffer(RHICmdList, TEXT("NanoGSPagedRotationScalePool"), OtherBytes);
		OtherDataBufferSRV = CreateRawSRV(RHICmdList, OtherDataBuffer);
		ColorOpacityBuffer = CreateRawBuffer(RHICmdList, TEXT("NanoGSPagedColorOpacityPool"), ColorBytes);
		ColorOpacityBufferSRV = CreateRawSRV(RHICmdList, ColorOpacityBuffer);
		SHBuffer = CreateRawBuffer(RHICmdList, TEXT("NanoGSPagedSH3Pool"), SHBytes);
		SHBufferSRV = CreateRawSRV(RHICmdList, SHBuffer);

		if (TreeGPUNodeCount > 0 &&
			(SharedTreeGPUResources.IsValid() ||
				TreeGPUNodeUploadData.Num() == static_cast<int32>(TreeGPUNodeCount)))
		{
			uint32 TreeNodeBytes = 0;
			if (!ToBufferBytes(TreeGPUNodeCount, sizeof(NanoGS::Tree::FGPUNodeRecord),
				TEXT("tree-gpu-node"), TreeNodeBytes, OutError))
			{
				TreeGPUNodeUploadData.Reset();
				TreeGPUNodeCount = 0;
			}
			else
			{
				const FString SharedKey = FString::Printf(TEXT("%s#%u"), *Manifest.SourceFilename, TreeGPUNodeCount);
				if (!SharedTreeGPUResources.IsValid())
				{
					FScopeLock CacheLock(&GTreeGPUResourceCacheMutex);
					if (const TSharedPtr<FSharedTreeGPUResources, ESPMode::ThreadSafe>* Cached =
						GTreeGPUResourceCache.Find(SharedKey))
					{
						SharedTreeGPUResources = *Cached;
					}
				}
				if (SharedTreeGPUResources.IsValid() && SharedTreeGPUResources->NodeCount == TreeGPUNodeCount)
				{
					TreeGPUNodeBuffer = SharedTreeGPUResources->NodeBuffer;
					TreeGPUNodeBufferSRV = SharedTreeGPUResources->NodeBufferSRV;
					UE_LOG(LogTemp, Log, TEXT("NanoGS reused shared Tree GPU node table: %u nodes / %.1f MiB"),
						TreeGPUNodeCount, double(TreeNodeBytes) / (1024.0 * 1024.0));
				}
				else if (TreeGPUNodeUploadData.Num() == static_cast<int32>(TreeGPUNodeCount))
				{
					TreeGPUNodeBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreeGPUNodeTable"), TreeNodeBytes,
						sizeof(NanoGS::Tree::FGPUNodeRecord),
						BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer, ERHIAccess::SRVMask);
					void* Dest = RHICmdList.LockBuffer(TreeGPUNodeBuffer, 0, TreeNodeBytes, RLM_WriteOnly);
					FMemory::Memcpy(Dest, TreeGPUNodeUploadData.GetData(), TreeNodeBytes);
					RHICmdList.UnlockBuffer(TreeGPUNodeBuffer);
					TreeGPUNodeBufferSRV = RHICmdList.CreateShaderResourceView(
						TreeGPUNodeBuffer, FRHIViewDesc::CreateBufferSRV()
							.SetType(FRHIViewDesc::EBufferType::Structured)
							.SetStride(sizeof(NanoGS::Tree::FGPUNodeRecord)));
					SharedTreeGPUResources = MakeShared<FSharedTreeGPUResources, ESPMode::ThreadSafe>();
					SharedTreeGPUResources->NodeBuffer = TreeGPUNodeBuffer;
					SharedTreeGPUResources->NodeBufferSRV = TreeGPUNodeBufferSRV;
					SharedTreeGPUResources->NodeCount = TreeGPUNodeCount;
					FScopeLock CacheLock(&GTreeGPUResourceCacheMutex);
					GTreeGPUResourceCache.Add(SharedKey, SharedTreeGPUResources);
				}
				TreeGPUNodeUploadData.Reset();
				TreeGPUNodeUploadData.Shrink();

				const uint32 ConfiguredFrontierCapacity = static_cast<uint32>(FMath::Max(
					1, CVarNanoGSTreeGPUFrontierMaxNodes.GetValueOnRenderThread()));
				TreeGPUFrontierCapacity = TreeV3Manifest.IsValid()
					? FMath::Min(ActiveIndexCapacity, ConfiguredFrontierCapacity)
					: FMath::Min3(TreeGPUNodeCount, ActiveIndexCapacity, ConfiguredFrontierCapacity);
				uint32 FrontierBytes = 0;
				if (TreeGPUFrontierCapacity == 0 ||
					!ToBufferBytes(TreeGPUFrontierCapacity, sizeof(uint32), TEXT("tree-gpu-frontier"), FrontierBytes, OutError))
				{
					TreeGPUNodeBufferSRV.SafeRelease();
					TreeGPUNodeBuffer.SafeRelease();
					SharedTreeGPUResources.Reset();
					TreeGPUNodeCount = 0;
					TreeGPUFrontierCapacity = 0;
				}
				else
				{
					for (uint8 Slot = 0; Slot < 2; ++Slot)
					{
						TreeGPUFrontierBuffers[Slot] = NanoGS::RHICompat::CreateBuffer(
							RHICmdList, Slot == 0 ? TEXT("NanoGSTreeFrontierA") : TEXT("NanoGSTreeFrontierB"),
							FrontierBytes, sizeof(uint32),
							BUF_Static | BUF_ShaderResource | BUF_UnorderedAccess | BUF_StructuredBuffer,
							ERHIAccess::SRVMask);
						TreeGPUFrontierSRVs[Slot] = RHICmdList.CreateShaderResourceView(
							TreeGPUFrontierBuffers[Slot], FRHIViewDesc::CreateBufferSRV()
								.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
						TreeGPUFrontierUAVs[Slot] = RHICmdList.CreateUnorderedAccessView(
							TreeGPUFrontierBuffers[Slot], FRHIViewDesc::CreateBufferUAV()
								.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));

						TreeGPUFrontierCountBuffers[Slot] = NanoGS::RHICompat::CreateBuffer(
							RHICmdList, Slot == 0 ? TEXT("NanoGSTreeFrontierCountA") : TEXT("NanoGSTreeFrontierCountB"),
							4u * sizeof(uint32), sizeof(uint32),
							BUF_Static | BUF_ShaderResource | BUF_UnorderedAccess | BUF_StructuredBuffer,
							ERHIAccess::SRVMask);
						TreeGPUFrontierCountSRVs[Slot] = RHICmdList.CreateShaderResourceView(
							TreeGPUFrontierCountBuffers[Slot], FRHIViewDesc::CreateBufferSRV()
								.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
						TreeGPUFrontierCountUAVs[Slot] = RHICmdList.CreateUnorderedAccessView(
							TreeGPUFrontierCountBuffers[Slot], FRHIViewDesc::CreateBufferUAV()
								.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
						TreeGPUPhysicalIndexBuffers[Slot] = NanoGS::RHICompat::CreateBuffer(
							RHICmdList, Slot == 0 ? TEXT("NanoGSTreePhysicalIndicesA") : TEXT("NanoGSTreePhysicalIndicesB"),
							FrontierBytes, sizeof(uint32),
							BUF_Static | BUF_ShaderResource | BUF_UnorderedAccess | BUF_StructuredBuffer,
							ERHIAccess::SRVMask);
						TreeGPUPhysicalIndexSRVs[Slot] = RHICmdList.CreateShaderResourceView(
							TreeGPUPhysicalIndexBuffers[Slot], FRHIViewDesc::CreateBufferSRV()
								.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
						TreeGPUPhysicalIndexUAVs[Slot] = RHICmdList.CreateUnorderedAccessView(
							TreeGPUPhysicalIndexBuffers[Slot], FRHIViewDesc::CreateBufferUAV()
								.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
					}
					TreeGPUPublishedPhysicalIndexBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreePublishedPhysicalIndices"),
						FrontierBytes, sizeof(uint32),
						BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
						ERHIAccess::SRVMask);
					TreeGPUPublishedPhysicalIndexSRV = RHICmdList.CreateShaderResourceView(
						TreeGPUPublishedPhysicalIndexBuffer, FRHIViewDesc::CreateBufferSRV()
							.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
					uint32* RootDest = static_cast<uint32*>(RHICmdList.LockBuffer(
						TreeGPUFrontierBuffers[0], 0, sizeof(uint32), RLM_WriteOnly));
					*RootDest = TreeGPURootNode;
					RHICmdList.UnlockBuffer(TreeGPUFrontierBuffers[0]);
					for (uint8 Slot = 0; Slot < 2; ++Slot)
					{
						const uint32 InitialCounts[4] = {Slot == 0 ? 1u : 0u, 0u, 0u, 1u};
						void* CountDest = RHICmdList.LockBuffer(
							TreeGPUFrontierCountBuffers[Slot], 0, sizeof(InitialCounts), RLM_WriteOnly);
						FMemory::Memcpy(CountDest, InitialCounts, sizeof(InitialCounts));
						RHICmdList.UnlockBuffer(TreeGPUFrontierCountBuffers[Slot]);
					}
					TreeGPUFrontierIndirectArgsBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreeFrontierIndirectArgs"), 3u * sizeof(uint32), sizeof(uint32),
						BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer, ERHIAccess::IndirectArgs);
					TreeGPUFrontierIndirectArgsUAV = RHICmdList.CreateUnorderedAccessView(
						TreeGPUFrontierIndirectArgsBuffer, FRHIViewDesc::CreateBufferUAV()
							.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
					TreeGPUFrontierStatusReadback = MakeUnique<FRHIGPUBufferReadback>(
						TEXT("NanoGSTreeFrontierStatusReadback"));
					bTreeGPUFrontierRefinementPending = true;
					TreeGPUBlockRequestCapacity = TreeV3Manifest.IsValid()
						? static_cast<uint32>(TreeV3Manifest->Blocks.Num())
						: 1u;
					const uint32 BlockRequestBytes = (TreeGPUBlockRequestCapacity + 1u) * sizeof(uint32);
					TreeGPUBlockRequestBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreeBlockRequests"), BlockRequestBytes, sizeof(uint32),
						BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer, ERHIAccess::UAVCompute);
					TreeGPUBlockRequestUAV = RHICmdList.CreateUnorderedAccessView(
						TreeGPUBlockRequestBuffer, FRHIViewDesc::CreateBufferUAV()
							.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
					TreeGPUBlockRequestBitsBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreeBlockRequestBits"),
						FMath::Max(TreeGPUBlockRequestCapacity, 1u) * sizeof(uint32), sizeof(uint32),
						BUF_UnorderedAccess | BUF_StructuredBuffer, ERHIAccess::UAVCompute);
					TreeGPUBlockRequestBitsUAV = RHICmdList.CreateUnorderedAccessView(
						TreeGPUBlockRequestBitsBuffer, FRHIViewDesc::CreateBufferUAV()
							.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
					TreeGPUBlockRequestReadback = MakeUnique<FRHIGPUBufferReadback>(
						TEXT("NanoGSTreeBlockRequestReadback"));

					TreeGPUBlockMaxNodeCount = TreeV3Manifest.IsValid()
						? TreeV3Manifest->MaxBlockNodes
						: 1u;
					uint64 TotalBlockNodeCount = 1u;
					if (TreeV3Manifest.IsValid())
					{
						TotalBlockNodeCount = 0;
						for (const NanoGS::TreeV3::FBlockRecord& Block : TreeV3Manifest->Blocks)
						{
							TotalBlockNodeCount += Block.NodeCount;
						}
					}
					int32 ConfiguredBlockPoolMB = FMath::Max(
						1, CVarNanoGSTreeGPUBlockPoolMB.GetValueOnRenderThread());
					FParse::Value(
						FCommandLine::Get(), TEXT("NanoGSTreeGPUBlockPoolMB="), ConfiguredBlockPoolMB);
					const uint64 BlockPoolBudgetBytes =
						static_cast<uint64>(FMath::Max(1, ConfiguredBlockPoolMB)) * 1024ull * 1024ull;
					TreeGPUBlockPoolNodeCapacity = static_cast<uint32>(FMath::Clamp<uint64>(
						BlockPoolBudgetBytes / sizeof(NanoGS::Tree::FGPUNodeRecord),
						FMath::Max<uint64>(1u, TreeGPUBlockMaxNodeCount),
						FMath::Max<uint64>(1u, TotalBlockNodeCount)));
					uint32 BlockPoolBytes = 0;
					if (!ToBufferBytes(TreeGPUBlockPoolNodeCapacity,
						sizeof(NanoGS::Tree::FGPUNodeRecord), TEXT("tree-v3-block-pool"), BlockPoolBytes, OutError))
					{
						ReleaseGPUBuffers();
						return false;
					}
					TreeGPUBlockNodeBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreeV3BlockNodePool"), BlockPoolBytes,
						sizeof(NanoGS::Tree::FGPUNodeRecord), BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
						ERHIAccess::SRVMask);
					TreeGPUBlockNodeBufferSRV = RHICmdList.CreateShaderResourceView(
						TreeGPUBlockNodeBuffer, FRHIViewDesc::CreateBufferSRV()
							.SetType(FRHIViewDesc::EBufferType::Structured)
							.SetStride(sizeof(NanoGS::Tree::FGPUNodeRecord)));
					const uint32 ResidencyEntries = FMath::Max(TreeGPUBlockRequestCapacity, 1u);
					TreeGPUBlockResidencyBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreeV3BlockResidency"), ResidencyEntries * sizeof(uint32),
						sizeof(uint32), BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer, ERHIAccess::SRVMask);
					uint32* ResidencyDest = static_cast<uint32*>(RHICmdList.LockBuffer(
						TreeGPUBlockResidencyBuffer, 0, ResidencyEntries * sizeof(uint32), RLM_WriteOnly));
					for (uint32 Index = 0; Index < ResidencyEntries; ++Index) ResidencyDest[Index] = MAX_uint32;
					RHICmdList.UnlockBuffer(TreeGPUBlockResidencyBuffer);
					TreeGPUBlockResidencyBufferSRV = RHICmdList.CreateShaderResourceView(
						TreeGPUBlockResidencyBuffer, FRHIViewDesc::CreateBufferSRV()
							.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));

					uint64 MaxPageId = 0;
					for (const FPageRecord& Page : Manifest.Pages) MaxPageId = FMath::Max(MaxPageId, Page.PageId);
					if (MaxPageId >= MAX_uint32)
					{
						ReleaseGPUBuffers();
						OutError = TEXT("Tree GPU page IDs exceed uint32 addressability");
						return false;
					}
					TreeGPUPageAddressCapacity = static_cast<uint32>(MaxPageId + 1u);
					TreeGPUPageRequestCapacity = static_cast<uint32>(Manifest.Pages.Num());
					TreeGPUPageResidencyBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreePageResidency"), TreeGPUPageAddressCapacity * sizeof(uint32),
						sizeof(uint32), BUF_Dynamic | BUF_ShaderResource | BUF_StructuredBuffer, ERHIAccess::SRVMask);
					uint32* PageResidencyDest = static_cast<uint32*>(RHICmdList.LockBuffer(
						TreeGPUPageResidencyBuffer, 0, TreeGPUPageAddressCapacity * sizeof(uint32), RLM_WriteOnly));
					for (uint32 Index = 0; Index < TreeGPUPageAddressCapacity; ++Index) PageResidencyDest[Index] = MAX_uint32;
					RHICmdList.UnlockBuffer(TreeGPUPageResidencyBuffer);
					TreeGPUPageResidencyBufferSRV = RHICmdList.CreateShaderResourceView(
						TreeGPUPageResidencyBuffer, FRHIViewDesc::CreateBufferSRV()
							.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
					TreeGPUPageRequestBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreePageRequests"), (TreeGPUPageRequestCapacity + 1u) * sizeof(uint32),
						sizeof(uint32), BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer, ERHIAccess::UAVCompute);
					TreeGPUPageRequestUAV = RHICmdList.CreateUnorderedAccessView(
						TreeGPUPageRequestBuffer, FRHIViewDesc::CreateBufferUAV()
							.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
					TreeGPUPageRequestBitsBuffer = NanoGS::RHICompat::CreateBuffer(
						RHICmdList, TEXT("NanoGSTreePageRequestBits"), TreeGPUPageAddressCapacity * sizeof(uint32),
						sizeof(uint32), BUF_UnorderedAccess | BUF_StructuredBuffer, ERHIAccess::UAVCompute);
					TreeGPUPageRequestBitsUAV = RHICmdList.CreateUnorderedAccessView(
						TreeGPUPageRequestBitsBuffer, FRHIViewDesc::CreateBufferUAV()
							.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
					TreeGPUPageRequestReadback = MakeUnique<FRHIGPUBufferReadback>(
						TEXT("NanoGSTreePageRequestReadback"));
					TreeGPUBlockToNodeBase.Init(INDEX_NONE, TreeGPUBlockRequestCapacity);
					TreeGPUBlockAllocationNodeCounts.Init(0u, TreeGPUBlockRequestCapacity);
					TreeGPUBlockLastRequestedFeedbackEpoch.Init(0u, TreeGPUBlockRequestCapacity);
					TreeGPUBlockFeedbackEpoch = 0;
					TreeGPUBlockFreeRanges.Reset();
					TreeGPUBlockFreeRanges.Add({0u, TreeGPUBlockPoolNodeCapacity});
					TreeGPUBlockCenterAndFeatureRadius.Init(FVector4f::Zero(), TreeGPUBlockRequestCapacity);
					if (TreeV3Manifest.IsValid())
					{
						for (const NanoGS::Tree::FGPUNodeRecord& Node : TreeV3Manifest->SkeletonNodes)
						{
							if ((Node.ChildCountAndFlags & NanoGS::TreeV3::BlockProxyFlag) != 0 &&
								TreeGPUBlockCenterAndFeatureRadius.IsValidIndex(static_cast<int32>(Node.FirstChild)))
							{
								TreeGPUBlockCenterAndFeatureRadius[Node.FirstChild] = FVector4f(
									Node.CenterAndSupportRadius.X, Node.CenterAndSupportRadius.Y,
									Node.CenterAndSupportRadius.Z, Node.GetFeatureRadiusCm());
							}
						}
					}
					TreeGPUFrontierReadSlot = 0;
					UE_LOG(LogTemp, Log,
						TEXT("NanoGS Tree GPU buffers ready: %u skeleton nodes, %u frontier capacity, %.1f MiB shared nodes, %.1f MiB compact block pool"),
						TreeGPUNodeCount, TreeGPUFrontierCapacity,
						double(TreeNodeBytes) / (1024.0 * 1024.0),
						double(BlockPoolBytes) / (1024.0 * 1024.0));
				}
			}
		}

		for (uint8 BufferSlot = 0; BufferSlot < ActiveIndexBufferCount; ++BufferSlot)
		{
			const TCHAR* BufferName = BufferSlot == 0
				? TEXT("NanoGSPagedActivePhysicalIndicesMainA")
				: BufferSlot == 1
					? TEXT("NanoGSPagedActivePhysicalIndicesMainB")
					: BufferSlot == 2
						? TEXT("NanoGSPagedActivePhysicalIndicesCaptureA")
						: TEXT("NanoGSPagedActivePhysicalIndicesCaptureB");
			const EBufferUsageFlags ActiveIndexUsage =
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer |
				(bBoundedStreamingEnabled ? BUF_UnorderedAccess : BUF_None);
			ActivePhysicalIndexBuffers[BufferSlot] = NanoGS::RHICompat::CreateBuffer(
				RHICmdList,
				BufferName,
				ActiveBytes,
				sizeof(uint32),
				ActiveIndexUsage,
				ERHIAccess::SRVMask);
			ActivePhysicalIndexBufferSRVs[BufferSlot] = RHICmdList.CreateShaderResourceView(
				ActivePhysicalIndexBuffers[BufferSlot],
				FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(uint32)));
			if (bBoundedStreamingEnabled)
			{
				ActivePhysicalIndexBufferUAVs[BufferSlot] = RHICmdList.CreateUnorderedAccessView(
					ActivePhysicalIndexBuffers[BufferSlot],
					FRHIViewDesc::CreateBufferUAV()
						.SetType(FRHIViewDesc::EBufferType::Structured)
						.SetStride(sizeof(uint32)));
			}
		}
		if (bBoundedStreamingEnabled)
		{
			ActivePageDescriptorCapacity = FMath::Min(
				ActiveIndexCapacity,
				static_cast<uint32>(GRHIMaxDispatchThreadGroupsPerDimension.X));
			uint32 DescriptorBytes = 0;
			if (ActivePageDescriptorCapacity == 0 ||
				!ToBufferBytes(ActivePageDescriptorCapacity, sizeof(FUintVector4),
					TEXT("active-page-descriptor"), DescriptorBytes, OutError))
			{
				ReleaseGPUBuffers();
				return false;
			}
			ActivePageDescriptorBuffer = NanoGS::RHICompat::CreateBuffer(
				RHICmdList,
				TEXT("NanoGSPagedActivePageDescriptors"),
				DescriptorBytes,
				sizeof(FUintVector4),
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
				ERHIAccess::SRVMask);
			ActivePageDescriptorBufferSRV = RHICmdList.CreateShaderResourceView(
				ActivePageDescriptorBuffer,
				FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(FUintVector4)));
		}

		SplatClusterIndexBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("NanoGSPagedClusterIndexDummy"),
			sizeof(uint32),
			sizeof(uint32),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::SRVMask);
		{
			uint32* Dest = static_cast<uint32*>(RHICmdList.LockBuffer(
				SplatClusterIndexBuffer, 0, sizeof(uint32), RLM_WriteOnly));
			*Dest = 0;
			RHICmdList.UnlockBuffer(SplatClusterIndexBuffer);
		}
		SplatClusterIndexBufferSRV = RHICmdList.CreateShaderResourceView(
			SplatClusterIndexBuffer,
			FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));

		IndexBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("NanoGSPagedQuadIndexBuffer"),
			6u * sizeof(uint16),
			sizeof(uint16),
			BUF_Static | BUF_IndexBuffer,
			ERHIAccess::VertexOrIndexBuffer);
		{
			const uint16 Indices[6] = {0, 1, 2, 1, 3, 2};
			void* Dest = RHICmdList.LockBuffer(IndexBuffer, 0, sizeof(Indices), RLM_WriteOnly);
			FMemory::Memcpy(Dest, Indices, sizeof(Indices));
			RHICmdList.UnlockBuffer(IndexBuffer);
		}

		ActiveSplatCount.store(0, std::memory_order_release);
		PublishedActiveSplatCount = 0;
		LoadedPageCount.store(0, std::memory_order_release);
		RequestedPageCount.store(0, std::memory_order_release);
		PublishedPageCount.store(0, std::memory_order_release);
		bUploadComplete.store(false, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		bUploadFailed.store(false, std::memory_order_release);
		bStopRequested.store(false, std::memory_order_release);
		bCurrentFrontierHasBeenDrawn = false;
		bCurrentFrontierFenceUnavailable = false;
		bPermanentFenceSafetyFailure = false;
		const uint32 NewUploadEpoch = UploadEpoch.fetch_add(1, std::memory_order_acq_rel) + 1u;
		bGPUBuffersCreated = true;
		if (bBoundedStreamingEnabled)
		{
			TArray<FPageCatalogEntry> Catalog;
			Catalog.Reserve(Manifest.Pages.Num());
			for (const FPageRecord& Page : Manifest.Pages)
			{
				Catalog.Add({Page.PageId, Page.SplatCount});
			}
			FPageResidentPoolConfig PoolConfig;
			PoolConfig.SlotCount = PhysicalSlotCount;
			PoolConfig.SplatsPerSlot = PageSlotCapacity;
			PoolConfig.KeepAliveGenerations = 2;
			if (!ResidentPool.Initialize(PoolConfig, Catalog, OutError))
			{
				ReleaseGPUBuffers();
				return false;
			}
			PublishedPageIds.Reset();
			TargetPageIds.Reset();
			PublishedSplatRefs.Reset();
		PublishedSplatRuns.Reset();
			TargetSplatRefs.Reset();
			RecentViewDemands.Reset();
			LastRequiredPageSeconds.Reset();
			LastBoundedDemandApplySeconds = 0.0;
			InFlightLoadOperations.Reset();
			RetiredFrontiers.Reset();
			CurrentFrontierLastUseFence.SafeRelease();
			PublishedActiveIndexBufferSlot = 0;
			bPublicationPendingOnIndexFence = false;
			PendingPublicationUploadEpoch = 0;
			CurrentTransitionPinGeneration = 0;
			NextRequestGeneration = 1;
			StreamingState.store(
				static_cast<uint8>(EExactHQStreamingState::NotRequested),
				std::memory_order_release);
			UE_LOG(LogTemp, Log,
				TEXT("NanoGS Page bounded complete-frontier pool created: %u slots / %u splats capacity; waiting for conservative view demand"),
				PhysicalSlotCount,
				SourceCapacity);
		}
		else
		{
			StreamingState.store(
				static_cast<uint8>(EExactHQStreamingState::AllResident),
				std::memory_order_release);
			StartAllResidentUpload(NewUploadEpoch);
		}
		return true;
	}

	void FPageRenderData::ReleaseGPUBuffers()
	{
		check(IsInRenderingThread());
		CancelUpload();
		PackedSplatBufferSRV.SafeRelease();
		PackedSplatBuffer.SafeRelease();
		ColorOpacityBufferSRV.SafeRelease();
		ColorOpacityBuffer.SafeRelease();
		PositionBufferSRV.SafeRelease();
		PositionBuffer.SafeRelease();
		OtherDataBufferSRV.SafeRelease();
		OtherDataBuffer.SafeRelease();
		SHBufferSRV.SafeRelease();
		SHBuffer.SafeRelease();
		TreeGPUNodeBufferSRV.SafeRelease();
		TreeGPUNodeBuffer.SafeRelease();
		SharedTreeGPUResources.Reset();
		for (uint8 Slot = 0; Slot < 2; ++Slot)
		{
			TreeGPUFrontierUAVs[Slot].SafeRelease();
			TreeGPUFrontierSRVs[Slot].SafeRelease();
			TreeGPUFrontierBuffers[Slot].SafeRelease();
			TreeGPUFrontierCountUAVs[Slot].SafeRelease();
			TreeGPUFrontierCountSRVs[Slot].SafeRelease();
			TreeGPUFrontierCountBuffers[Slot].SafeRelease();
			TreeGPUPhysicalIndexUAVs[Slot].SafeRelease();
			TreeGPUPhysicalIndexSRVs[Slot].SafeRelease();
			TreeGPUPhysicalIndexBuffers[Slot].SafeRelease();
		}
		TreeGPUPublishedPhysicalIndexSRV.SafeRelease();
		TreeGPUPublishedPhysicalIndexBuffer.SafeRelease();
		TreeGPUFrontierIndirectArgsUAV.SafeRelease();
		TreeGPUFrontierIndirectArgsBuffer.SafeRelease();
		TreeGPUFrontierStatusReadback.Reset();
		TreeGPUBlockRequestUAV.SafeRelease();
		TreeGPUBlockRequestBuffer.SafeRelease();
		TreeGPUBlockRequestBitsUAV.SafeRelease();
		TreeGPUBlockRequestBitsBuffer.SafeRelease();
		TreeGPUBlockRequestReadback.Reset();
		TreeGPUBlockNodeBufferSRV.SafeRelease();
		TreeGPUBlockNodeBuffer.SafeRelease();
		TreeGPUBlockResidencyBufferSRV.SafeRelease();
		TreeGPUBlockResidencyBuffer.SafeRelease();
		TreeGPUPageResidencyBufferSRV.SafeRelease();
		TreeGPUPageResidencyBuffer.SafeRelease();
		TreeGPUPageRequestUAV.SafeRelease();
		TreeGPUPageRequestBuffer.SafeRelease();
		TreeGPUPageRequestBitsUAV.SafeRelease();
		TreeGPUPageRequestBitsBuffer.SafeRelease();
		TreeGPUPageRequestReadback.Reset();
		PendingTreeGPUBlockRequests.Reset();
		PendingTreeGPUPageRequests.Reset();
		TreeGPUBlockToNodeBase.Reset();
		TreeGPUBlockAllocationNodeCounts.Reset();
		TreeGPUBlockLastRequestedFeedbackEpoch.Reset();
		TreeGPUBlockFeedbackEpoch = 0;
		TreeGPUBlockFreeRanges.Reset();
		TreeGPUBlockCenterAndFeatureRadius.Reset();
		TreeGPUBlocksInFlight.Reset();
		TreeGPUBlockPoolNodeCapacity = 0;
		TreeGPUBlockMaxNodeCount = 0;
		TreeGPUBlockRequestCapacity = 0;
		LastLoggedTreeGPUBlockRequestCount = MAX_uint32;
		LastLoggedTreeGPUFrontierCount = MAX_uint32;
		LastLoggedTreeGPUFrontierFlags = MAX_uint32;
		TreeGPUPublishedPageIds.Reset();
		TreeGPURefinementGeneration = 0;
		TreeGPURefinementStartSeconds = 0.0;
		TreeGPURefinementDispatchCount = 0;
		TreeGPUStatusSampleCount = 0;
		TreeGPUStatusWaitFrames = 0;
		TreeGPUPageReadbackWaitFrames = 0;
		TreeGPUBlockReadbackWaitFrames = 0;
		TreeGPUPriorityRestartCount = 0;
		bTreeGPUBlockRequestReadbackPending = false;
		bTreeGPUPageRequestReadbackPending = false;
		bTreeGPUFrontierStatusReadbackPending = false;
		bTreeGPUFrontierRefinementPending = false;
		TreeGPUFrontierPriorityPass = 0;
		TreeGPUFrontierPriorityThresholdPixels = 0.0f;
		TreeGPUFrontierLastStableThresholdPixels = 0.0f;
		TreeGPUFrontierOverflowThresholdPixels = 0.0f;
		bTreeGPUFrontierPublished.store(false, std::memory_order_release);
		TreeGPUActiveSplatCount.store(0, std::memory_order_release);
		TreeGPUPageAddressCapacity = 0;
		TreeGPUPageRequestCapacity = 0;
		LastTreeGPUPageResidencyGeneration = MAX_uint32;
		TreeGPUFrontierCapacity = 0;
		TreeGPUFrontierReadSlot = 0;
		for (uint8 BufferSlot = 0; BufferSlot < 4; ++BufferSlot)
		{
			ActivePhysicalIndexBufferUAVs[BufferSlot].SafeRelease();
			ActivePhysicalIndexBufferSRVs[BufferSlot].SafeRelease();
			ActivePhysicalIndexBuffers[BufferSlot].SafeRelease();
		}
		ActivePageDescriptorBufferSRV.SafeRelease();
		ActivePageDescriptorBuffer.SafeRelease();
		ActivePageDescriptorCapacity = 0;
		SplatClusterIndexBufferSRV.SafeRelease();
		SplatClusterIndexBuffer.SafeRelease();
		IndexBuffer.SafeRelease();
		CurrentFrontierLastUseFence.SafeRelease();
		RetiredFrontiers.Reset();
		PublishedActiveIndexBufferSlot = 0;
		PublishedCaptureActiveIndexBufferSlot = 2;
		CaptureFrontierLastUseFence.SafeRelease();
		PublishedCaptureSplatRefs.Reset();
		PublishedCaptureSplatRuns.Reset();
		PublishedCaptureActiveSplatCount = 0;
		bCaptureFrontierHasBeenDrawn = false;
		bCaptureFrontierFenceUnavailable = false;
		CaptureActiveSplatCount.store(0, std::memory_order_release);
		CaptureResidentGeneration.store(0, std::memory_order_release);
		PublishedCapturePageCount.store(0, std::memory_order_release);
		bCaptureFrontierReady.store(false, std::memory_order_release);
		bPublicationPendingOnIndexFence = false;
		PendingPublicationUploadEpoch = 0;
		PublishedActiveSplatCount = 0;
		bCurrentFrontierHasBeenDrawn = false;
		bCurrentFrontierFenceUnavailable = false;
		bPermanentFenceSafetyFailure = false;
		PublishedPageIds.Reset();
		TargetPageIds.Reset();
		PublishedSplatRefs.Reset();
		TargetSplatRefs.Reset();
		RecentViewDemands.Reset();
		LastRequiredPageSeconds.Reset();
		LastBoundedDemandApplySeconds = 0.0;
		InFlightLoadOperations.Reset();
		ResidentPool.Reset();
		bGPUBuffersCreated = false;
	}

	void FPageRenderData::CancelUpload()
	{
		bStopRequested.store(true, std::memory_order_release);
		UploadEpoch.fetch_add(1, std::memory_order_acq_rel);
		ActiveSplatCount.store(0, std::memory_order_release);
		PublishedActiveSplatCount = 0;
		LoadedPageCount.store(0, std::memory_order_release);
		RequestedPageCount.store(0, std::memory_order_release);
		PublishedPageCount.store(0, std::memory_order_release);
		bUploadComplete.store(false, std::memory_order_release);
		if (bBoundedStreamingEnabled)
		{
			StreamingState.store(
				static_cast<uint8>(EExactHQStreamingState::NotRequested),
				std::memory_order_release);
		}
		ResidentGeneration.fetch_add(1, std::memory_order_acq_rel);
	}

	bool FPageRenderData::BuildDemandViews_RenderThread(
		const FSceneViewFamily& ViewFamily,
		const FTransform& LocalToWorld,
		const double SplatScale,
		const uint64 StableCaptureIdentity,
		TArray<FExactHQPageDemandView>& OutViews) const
	{
		check(IsInRenderingThread());
		OutViews.Reset();
		if (!FMath::IsFinite(SplatScale) || ViewFamily.Views.IsEmpty())
		{
			return false;
		}

		FVector3d WorldCorners[8];
		FExactHQPageDemandConfig DemandConfig;
		DemandConfig.ExactSupportSigma = 4.0;
		DemandConfig.SplatScale = SplatScale;
		DemandConfig.RasterGuardPixels = FMath::Max(
			2.0, static_cast<double>(CVarNanoGSPagedDemandGuardPixels.GetValueOnRenderThread()));
		if (!DemandHierarchy.Prepare(LocalToWorld, DemandConfig) ||
			!DemandHierarchy.GetPreparedSceneWorldCorners(WorldCorners))
		{
			return false;
		}

		OutViews.Reserve(ViewFamily.Views.Num());
		for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ++ViewIndex)
		{
			const FSceneView* View = ViewFamily.Views[ViewIndex];
			if (View == nullptr)
			{
				return false;
			}

			FExactHQPageDemandView& DemandView = OutViews.AddDefaulted_GetRef();
			// Independent SceneCapture components can expose the same engine ViewKey
			// (notably AirSim cameras whose transient view states are rebuilt every
			// capture).  Keying those demands by ViewKey makes camera A replace camera
			// B at the start of every render frame, so the complete-frontier target
			// oscillates forever instead of retaining their union.  The render-target
			// resource is stable for the component's lifetime and is the same identity
			// used by the strict capture ticket, so prefer it for SceneCapture families.
			const UPTRINT RenderTargetIdentity =
				reinterpret_cast<UPTRINT>(ViewFamily.RenderTarget);
			if (StableCaptureIdentity != 0)
			{
				DemandView.ViewId = 0x8000000000000000ull ^
					StableCaptureIdentity ^
					(0x9e3779b97f4a7c15ull * static_cast<uint64>(ViewIndex + 1));
			}
			else if (View->bIsSceneCapture && RenderTargetIdentity != 0)
			{
				DemandView.ViewId = 0x4000000000000000ull ^
					static_cast<uint64>(RenderTargetIdentity) ^
					(0x9e3779b97f4a7c15ull * static_cast<uint64>(ViewIndex + 1));
			}
			else if (const uint32 EngineViewKey = View->GetViewKey(); EngineViewKey != 0)
			{
				DemandView.ViewId = 0x100000000ull | static_cast<uint64>(EngineViewKey);
			}
			else
			{
				const UPTRINT StablePointer = View->State != nullptr
					? reinterpret_cast<UPTRINT>(View->State)
					: (View->ViewActor != nullptr
						? reinterpret_cast<UPTRINT>(View->ViewActor)
						: reinterpret_cast<UPTRINT>(&ViewFamily));
				DemandView.ViewId = static_cast<uint64>(StablePointer) ^
					(0x9e3779b97f4a7c15ull * static_cast<uint64>(ViewIndex + 1));
			}

			DemandView.FrustumPlanes.Reserve(View->ViewFrustum.Planes.Num());
			for (const FPlane& Plane : View->ViewFrustum.Planes)
			{
				// FConvexVolume considers PlaneDot=P.N-W <= 0 inside; the demand
				// selector uses Dot(N,P)+Offset >= 0, hence this exact sign flip.
				FPageDemandPlane& DemandPlane = DemandView.FrustumPlanes.AddDefaulted_GetRef();
				DemandPlane.Normal = -FVector3d(Plane.X, Plane.Y, Plane.Z);
				DemandPlane.Offset = Plane.W;
			}

			const FIntPoint UnscaledViewSize = View->UnscaledViewRect.Size();
			const FIntPoint RasterViewSize = static_cast<const FViewInfo&>(*View).ViewRect.Size();
			const FVector3d ViewOrigin(View->ViewMatrices.GetViewOrigin());
			const FVector3d ViewDirection(View->GetViewDirection());
			double MaxPositiveDepth = 0.0;
			for (const FVector3d& Corner : WorldCorners)
			{
				MaxPositiveDepth = FMath::Max(
					MaxPositiveDepth,
					FVector3d::DotProduct(Corner - ViewOrigin, ViewDirection));
			}

			const FMatrix Projection = View->ViewMatrices.GetProjectionNoAAMatrix();
			const double ProjectionScaleX = FMath::Abs(Projection.M[0][0]);
			const double ProjectionScaleY = FMath::Abs(Projection.M[1][1]);
			// Dynamic resolution / ScreenPercentage may make the raster ViewRect smaller
			// than UnscaledViewRect. The smaller focal-pixel count produces the larger,
			// conservative cm-per-pixel guard; never derive support from unscaled alone.
			if (!FPageDemandSelector::CalculateConservativeMaxWorldCentimetersPerPixel(
				UnscaledViewSize,
				RasterViewSize,
				ProjectionScaleX,
				ProjectionScaleY,
				View->IsPerspectiveProjection(),
				MaxPositiveDepth,
				DemandView.MaxWorldCentimetersPerPixel))
			{
				return false;
			}
		}
		return true;
	}

	void FPageRenderData::RequestExactHQForViewFamily_RenderThread(
		const FSceneViewFamily& ViewFamily,
		const FTransform& LocalToWorld,
		const double SplatScale,
		const uint64 StableCaptureIdentity)
	{
		check(IsInRenderingThread());
		if (!bBoundedStreamingEnabled || !bGPUBuffersCreated ||
			bStopRequested.load(std::memory_order_acquire))
		{
			return;
		}

		const bool bReleasedRetirement = ReleaseCompletedRetirements_RenderThread();
		if (bReleasedRetirement && bPublicationPendingOnIndexFence)
		{
			TryPublishBoundedFrontier_RenderThread(PendingPublicationUploadEpoch);
		}
		const uint64 FrameNumber = static_cast<uint64>(GFrameNumberRenderThread);
		const double NowSeconds = FPlatformTime::Seconds();
		TArray<FExactHQPageDemandView> DemandViews;
		const bool bViewsValid = BuildDemandViews_RenderThread(
			ViewFamily, LocalToWorld, SplatScale, StableCaptureIdentity, DemandViews);

		if (!bViewsValid)
		{
			// A synthetic invalid view is intentionally passed through the selector so
			// its fail-open behavior remains covered by the same implementation/tests.
			DemandViews.Reset();
			FExactHQPageDemandView& InvalidView = DemandViews.AddDefaulted_GetRef();
			InvalidView.ViewId = static_cast<uint64>(reinterpret_cast<UPTRINT>(&ViewFamily));
		}

		FExactHQPageDemandConfig DemandConfig;
		DemandConfig.ExactSupportSigma = 4.0;
		DemandConfig.SplatScale = SplatScale;
		DemandConfig.RasterGuardPixels = FMath::Max(
			2.0, static_cast<double>(CVarNanoGSPagedDemandGuardPixels.GetValueOnRenderThread()));
		const FExactHQPageDemandResult Demand = DemandHierarchy.SelectExactHQPages(
			LocalToWorld, DemandViews, DemandConfig);

		for (const FExactHQPageDemandViewResult& PerView : Demand.PerView)
		{
			FRecentViewDemand& Recent = RecentViewDemands.FindOrAdd(PerView.ViewId);
			if (Recent.LastSeenFrame == FrameNumber)
			{
				Recent.PageIds.Append(PerView.RequiredPageIds);
				SortUniquePageIds(Recent.PageIds);
			}
			else
			{
				Recent.PageIds = PerView.RequiredPageIds;
			}
			Recent.LastSeenFrame = FrameNumber;
			Recent.LastSeenSeconds = NowSeconds;
		}

		const uint64 DemandTTL = static_cast<uint64>(
			FMath::Max(1, CVarNanoGSPagedViewDemandTTLFrames.GetValueOnRenderThread()));
		const double DemandTTLSeconds = static_cast<double>(FMath::Clamp(
			CVarNanoGSPagedViewDemandTTLSeconds.GetValueOnRenderThread(), 0.0f, 60.0f));
		TArray<uint64> UnionPageIds;
		for (auto It = RecentViewDemands.CreateIterator(); It; ++It)
		{
			const bool bFrameCounterWrapped = FrameNumber < It.Value().LastSeenFrame;
			const uint64 Age = bFrameCounterWrapped ? MAX_uint64 : FrameNumber - It.Value().LastSeenFrame;
			const double AgeSeconds = FMath::Max(0.0, NowSeconds - It.Value().LastSeenSeconds);
			if (Age > DemandTTL && AgeSeconds > DemandTTLSeconds)
			{
				It.RemoveCurrent();
				continue;
			}
			UnionPageIds.Append(It.Value().PageIds);
		}
		SortUniquePageIds(UnionPageIds);

		// LCC-style frontier hysteresis, but strictly conservative: pages that are
		// exact requirements enter immediately, while recently exited pages may stay
		// active briefly.  Keeping extras cannot change a visible pixel because their
		// expanded support is outside every current view; it only avoids rebuilding and
		// uploading a multi-megabyte active-index frontier when a page boundary jitters.
		const float RetentionSeconds = FMath::Clamp(
			CVarNanoGSPagedPageRetentionSeconds.GetValueOnRenderThread(), 0.0f, 10.0f);
		if (RetentionSeconds <= 0.0f)
		{
			if (!LastRequiredPageSeconds.IsEmpty())
			{
				LastRequiredPageSeconds.Reset();
			}
		}
		else
		{
			TSet<uint64> ExactRequiredPages;
			ExactRequiredPages.Reserve(UnionPageIds.Num());
			for (const uint64 PageId : UnionPageIds)
			{
				ExactRequiredPages.Add(PageId);
				LastRequiredPageSeconds.FindOrAdd(PageId) = NowSeconds;
			}
			if (UnionPageIds.Num() >= static_cast<int32>(PhysicalSlotCount))
			{
				LastRequiredPageSeconds.Reset();
			}
			else
			{
				struct FRetainedPageCandidate
				{
					uint64 PageId = 0;
					double LastRequiredSeconds = 0.0;
				};
				TArray<FRetainedPageCandidate> Candidates;
				for (auto It = LastRequiredPageSeconds.CreateIterator(); It; ++It)
				{
					if (ExactRequiredPages.Contains(It.Key()))
					{
						continue;
					}
					const double AgeSeconds = FMath::Max(0.0, NowSeconds - It.Value());
					if (AgeSeconds > static_cast<double>(RetentionSeconds))
					{
						It.RemoveCurrent();
						continue;
					}
					Candidates.Add({It.Key(), It.Value()});
				}
				Candidates.Sort([](const FRetainedPageCandidate& A, const FRetainedPageCandidate& B)
				{
					return A.LastRequiredSeconds > B.LastRequiredSeconds;
				});
				const int32 AvailableExtras = static_cast<int32>(PhysicalSlotCount) - UnionPageIds.Num();
				for (int32 Index = 0; Index < FMath::Min(AvailableExtras, Candidates.Num()); ++Index)
				{
					UnionPageIds.Add(Candidates[Index].PageId);
				}
				SortUniquePageIds(UnionPageIds);
			}
		}

		const EExactHQStreamingState State = GetStreamingState();
		if (ArePageIdArraysEqual(UnionPageIds, TargetPageIds) &&
			(State == EExactHQStreamingState::Loading ||
			 State == EExactHQStreamingState::OutOfCapacity ||
			 LastDemandAttemptFrame == FrameNumber))
		{
			return;
		}
		const double ShrinkIntervalSeconds = static_cast<double>(FMath::Clamp(
			CVarNanoGSPagedShrinkIntervalSeconds.GetValueOnRenderThread(), 0.0f, 10.0f));
		if (State == EExactHQStreamingState::Ready &&
			ShrinkIntervalSeconds > 0.0 &&
			UnionPageIds.Num() < PublishedPageIds.Num() &&
			IsSortedPageIdSubset(UnionPageIds, PublishedPageIds) &&
			NowSeconds - LastBoundedDemandApplySeconds < ShrinkIntervalSeconds)
		{
			// Every exact required page is already published.  Delay only the removal
			// and keep drawing/capturing from the old conservative superset.
			return;
		}
		ApplyBoundedDemand_RenderThread(MoveTemp(UnionPageIds));
		LastDemandAttemptFrame = FrameNumber;
		LastBoundedDemandApplySeconds = NowSeconds;
	}

	void FPageRenderData::RequestExplicitPages_RenderThread(
		TConstArrayView<uint64> RequestedPageIds,
		const uint64 DemandIdentity,
		const bool bAllowPublishedFrontierForCapture)
	{
		check(IsInRenderingThread());
		if (!bBoundedStreamingEnabled || DemandIdentity == 0)
			return;
		const bool bReleasedRetirement = ReleaseCompletedRetirements_RenderThread();
		if (bReleasedRetirement && bPublicationPendingOnIndexFence)
			TryPublishBoundedFrontier_RenderThread(PendingPublicationUploadEpoch);

		TArray<uint64> SortedPageIds(RequestedPageIds);
		SortUniquePageIds(SortedPageIds);
		const uint64 FrameNumber = GFrameCounter;
		const double NowSeconds = FPlatformTime::Seconds();
		FRecentViewDemand& Recent = RecentViewDemands.FindOrAdd(DemandIdentity);
		Recent.PageIds = MoveTemp(SortedPageIds);
		Recent.SplatRefs.Reset();
		Recent.bSparse = false;
		Recent.LastSeenFrame = FrameNumber;
		Recent.LastSeenSeconds = NowSeconds;
		Recent.bAllowPublishedFrontierForCapture = bAllowPublishedFrontierForCapture;

		const uint64 DemandTTL = static_cast<uint64>(
			FMath::Max(1, CVarNanoGSPagedViewDemandTTLFrames.GetValueOnRenderThread()));
		const double DemandTTLSeconds = static_cast<double>(FMath::Clamp(
			CVarNanoGSPagedViewDemandTTLSeconds.GetValueOnRenderThread(), 0.0f, 60.0f));
		TArray<uint64> UnionPageIds;
		bool bUnionAllowsPublishedFrontierForCapture = true;
		for (auto It = RecentViewDemands.CreateIterator(); It; ++It)
		{
			const bool bFrameCounterWrapped = FrameNumber < It.Value().LastSeenFrame;
			const uint64 Age = bFrameCounterWrapped ? MAX_uint64 : FrameNumber - It.Value().LastSeenFrame;
			const double AgeSeconds = FMath::Max(0.0, NowSeconds - It.Value().LastSeenSeconds);
			if (Age > DemandTTL && AgeSeconds > DemandTTLSeconds)
			{
				It.RemoveCurrent();
				continue;
			}
			UnionPageIds.Append(It.Value().PageIds);
			bUnionAllowsPublishedFrontierForCapture &= It.Value().bAllowPublishedFrontierForCapture;
		}
		SortUniquePageIds(UnionPageIds);
		ApplyBoundedDemand_RenderThread(MoveTemp(UnionPageIds), bUnionAllowsPublishedFrontierForCapture);
	}

	void FPageRenderData::RequestExplicitSplats_RenderThread(
		TArray<FExplicitSplatRef>&& RequestedSplats,
		TArray<uint64>&& PrevalidatedSortedPageIds,
		TArray<FExplicitSplatRun>&& PrecomputedRuns,
		const uint64 DemandIdentity,
		const bool bAllowPublishedFrontierForCapture,
		const bool bAlreadySortedUnique)
	{
		check(IsInRenderingThread());
		const double RequestStartSeconds = FPlatformTime::Seconds();
		if (!bBoundedStreamingEnabled || DemandIdentity == 0)
			return;
		const bool bReleasedRetirement = ReleaseCompletedRetirements_RenderThread();
		if (bReleasedRetirement && bPublicationPendingOnIndexFence)
			TryPublishBoundedFrontier_RenderThread(PendingPublicationUploadEpoch);

		TArray<FExplicitSplatRef> SortedRefs = MoveTemp(RequestedSplats);
		if (!bAlreadySortedUnique)
			SortUniqueSplatRefs(SortedRefs);
		TArray<uint64> SortedPageIds = MoveTemp(PrevalidatedSortedPageIds);
		if (!bAlreadySortedUnique || (SortedRefs.Num() > 0 && SortedPageIds.IsEmpty()))
		{
			SortedPageIds.Reset();
			SortedPageIds.Reserve(SortedRefs.Num());
			for (const FExplicitSplatRef& Ref : SortedRefs)
			{
				const FPageRecord* Page = Manifest.FindPage(Ref.PageId);
				if (Page == nullptr || Ref.PageLocal >= Page->SplatCount)
				{
					UE_LOG(LogTemp, Error,
						TEXT("NanoGS sparse frontier rejected invalid ref page=%llu local=%u"),
						static_cast<unsigned long long>(Ref.PageId), Ref.PageLocal);
					return;
				}
				SortedPageIds.Add(Ref.PageId);
			}
			SortUniquePageIds(SortedPageIds);
		}

		if (!PendingTreeGPUPageRequests.IsEmpty() &&
			GetStreamingState() != EExactHQStreamingState::Loading &&
			GetStreamingState() != EExactHQStreamingState::Draining &&
			RetiredFrontiers.IsEmpty())
		{
			TSet<uint64> ProtectedPages;
			ProtectedPages.Reserve(SortedPageIds.Num() + PublishedPageIds.Num());
			for (const uint64 PageId : SortedPageIds) ProtectedPages.Add(PageId);
			for (const uint64 PageId : PublishedPageIds) ProtectedPages.Add(PageId);
			const int32 RemainingCapacity = FMath::Max(
				0, static_cast<int32>(PhysicalSlotCount) - ProtectedPages.Num());
			const int32 PrefetchLimit = FMath::Min(
				RemainingCapacity,
				FMath::Max(0, CVarNanoGSTreeGPUPagePrefetchPerUpdate.GetValueOnRenderThread()));
			int32 AddedPrefetchPages = 0;
			for (const uint32 PageId : PendingTreeGPUPageRequests)
			{
				FPagePhysicalRange ExistingRange;
				if (Manifest.FindPage(PageId) == nullptr || ProtectedPages.Contains(PageId) ||
					(ResidentPool.FindPhysicalRange(PageId, ExistingRange) &&
					 ExistingRange.State == EPageSlotState::Resident))
				{
					continue;
				}
				if (AddedPrefetchPages >= PrefetchLimit)
				{
					break;
				}
				SortedPageIds.Add(PageId);
				ProtectedPages.Add(PageId);
				++AddedPrefetchPages;
			}
			if (AddedPrefetchPages > 0)
			{
				SortUniquePageIds(SortedPageIds);
				if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
				{
					UE_LOG(LogTemp, Display,
						TEXT("NanoGS Tree GPU admitted %d prefetch-only pages; visible sparse refs unchanged"),
						AddedPrefetchPages);
				}
			}
		}

		const uint64 FrameNumber = GFrameCounter;
		const double NowSeconds = FPlatformTime::Seconds();
		FRecentViewDemand& Recent = RecentViewDemands.FindOrAdd(DemandIdentity);
		Recent.PageIds = MoveTemp(SortedPageIds);
		Recent.SplatRefs = MoveTemp(SortedRefs);
		Recent.SplatRuns = MoveTemp(PrecomputedRuns);
		Recent.bSparse = true;
		Recent.LastSeenFrame = FrameNumber;
		Recent.LastSeenSeconds = NowSeconds;
		Recent.bAllowPublishedFrontierForCapture = bAllowPublishedFrontierForCapture;

		// TreeLOD normally has one combined demand. It is already sorted/unique
		// above, so do not copy and sort the same multi-million-ref array again.
		if (RecentViewDemands.Num() == 1)
		{
			const int32 RequestedRefCount = Recent.SplatRefs.Num();
			const int32 RequestedPageCountValue = Recent.PageIds.Num();
			const double ApplyStartSeconds = FPlatformTime::Seconds();
			ApplyBoundedDemand_RenderThread(
				MoveTemp(Recent.PageIds),
				Recent.bAllowPublishedFrontierForCapture,
				MoveTemp(Recent.SplatRefs),
				MoveTemp(Recent.SplatRuns));
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				const double EndSeconds = FPlatformTime::Seconds();
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree render-thread demand: prepare=%.3f ms apply=%.3f ms refs=%d pages=%d sorted=%d"),
					(ApplyStartSeconds - RequestStartSeconds) * 1000.0,
					(EndSeconds - ApplyStartSeconds) * 1000.0,
					RequestedRefCount, RequestedPageCountValue, bAlreadySortedUnique ? 1 : 0);
			}
			return;
		}

		const uint64 DemandTTL = static_cast<uint64>(
			FMath::Max(1, CVarNanoGSPagedViewDemandTTLFrames.GetValueOnRenderThread()));
		const double DemandTTLSeconds = static_cast<double>(FMath::Clamp(
			CVarNanoGSPagedViewDemandTTLSeconds.GetValueOnRenderThread(), 0.0f, 60.0f));
		TArray<uint64> UnionPageIds;
		TArray<FExplicitSplatRef> UnionRefs;
		bool bAllSparse = true;
		bool bUnionAllowsPublishedFrontierForCapture = true;
		for (auto It = RecentViewDemands.CreateIterator(); It; ++It)
		{
			const bool bFrameCounterWrapped = FrameNumber < It.Value().LastSeenFrame;
			const uint64 Age = bFrameCounterWrapped ? MAX_uint64 : FrameNumber - It.Value().LastSeenFrame;
			const double AgeSeconds = FMath::Max(0.0, NowSeconds - It.Value().LastSeenSeconds);
			if (Age > DemandTTL && AgeSeconds > DemandTTLSeconds)
			{
				It.RemoveCurrent();
				continue;
			}
			UnionPageIds.Append(It.Value().PageIds);
			bAllSparse &= It.Value().bSparse;
			if (It.Value().bSparse)
				UnionRefs.Append(It.Value().SplatRefs);
			bUnionAllowsPublishedFrontierForCapture &= It.Value().bAllowPublishedFrontierForCapture;
		}
		SortUniquePageIds(UnionPageIds);
		if (bAllSparse)
		{
			SortUniqueSplatRefs(UnionRefs);
			ApplyBoundedDemand_RenderThread(
				MoveTemp(UnionPageIds), bUnionAllowsPublishedFrontierForCapture, MoveTemp(UnionRefs));
		}
		else
		{
			ApplyBoundedDemand_RenderThread(MoveTemp(UnionPageIds), bUnionAllowsPublishedFrontierForCapture);
		}
	}

	void FPageRenderData::GetResidentPageIds_RenderThread(TArray<uint64>& OutPageIds) const
	{
		check(IsInRenderingThread());
		OutPageIds.Reset();
		OutPageIds.Reserve(Manifest.Pages.Num());
		for (const FPageRecord& Page : Manifest.Pages)
		{
			FPagePhysicalRange Range;
			if (ResidentPool.FindPhysicalRange(Page.PageId, Range) &&
				Range.State == EPageSlotState::Resident && Range.SplatCount == Page.SplatCount)
			{
				OutPageIds.Add(Page.PageId);
			}
		}
	}

	void FPageRenderData::GetPublishedPageIds_RenderThread(TArray<uint64>& OutPageIds) const
	{
		check(IsInRenderingThread());
		OutPageIds = PublishedPageIds;
	}

	uint32 FPageRenderData::GetMissingTargetPageCount_RenderThread() const
	{
		check(IsInRenderingThread());
		uint32 MissingCount = 0;
		for (const uint64 PageId : TargetPageIds)
		{
			FPagePhysicalRange Range;
			if (!ResidentPool.FindPhysicalRange(PageId, Range) ||
				Range.State != EPageSlotState::Resident)
			{
				++MissingCount;
			}
		}
		return MissingCount;
	}

	bool FPageRenderData::PublishResidentTreeFrontier_RenderThread(
		TArray<FExplicitSplatRef>&& RenderSplats,
		TArray<uint64>&& RenderPageIds,
		TArray<FExplicitSplatRun>&& RenderRuns,
		const bool bAlreadySortedUnique)
	{
		check(IsInRenderingThread());
		if (!bTreeLODSource || !bBoundedStreamingEnabled ||
			GetStreamingState() != EExactHQStreamingState::Loading ||
			(RenderSplats.IsEmpty() && RenderRuns.IsEmpty()))
		{
			return false;
		}
		if (!bAlreadySortedUnique)
		{
			SortUniqueSplatRefs(RenderSplats);
			RenderPageIds.Reset();
			RenderPageIds.Reserve(RenderSplats.Num());
			for (const FExplicitSplatRef& Ref : RenderSplats)
			{
				const FPageRecord* Page = Manifest.FindPage(Ref.PageId);
				if (Page == nullptr || Ref.PageLocal >= Page->SplatCount)
					return false;
				RenderPageIds.Add(Ref.PageId);
			}
			SortUniquePageIds(RenderPageIds);
			RenderRuns.Reset();
		}
		if (AreSplatRefArraysEqual(RenderSplats, PublishedSplatRefs) &&
			AreSplatRunArraysEqual(RenderRuns, PublishedSplatRuns))
			return true;

		TArray<uint64> SavedTargetPageIds = MoveTemp(TargetPageIds);
		TArray<FExplicitSplatRef> SavedTargetSplatRefs = MoveTemp(TargetSplatRefs);
		TArray<FExplicitSplatRun> SavedTargetSplatRuns = MoveTemp(TargetSplatRuns);
		TargetPageIds = MoveTemp(RenderPageIds);
		TargetSplatRefs = MoveTemp(RenderSplats);
		TargetSplatRuns = MoveTemp(RenderRuns);
		bPublishingIntermediateTreeFrontier = true;
		TryPublishBoundedFrontier_RenderThread(UploadEpoch.load(std::memory_order_acquire));
		bPublishingIntermediateTreeFrontier = false;
		const bool bPublished = AreSplatRefArraysEqual(TargetSplatRefs, PublishedSplatRefs) &&
			AreSplatRunArraysEqual(TargetSplatRuns, PublishedSplatRuns);
		TargetPageIds = MoveTemp(SavedTargetPageIds);
		TargetSplatRefs = MoveTemp(SavedTargetSplatRefs);
		TargetSplatRuns = MoveTemp(SavedTargetSplatRuns);
		return bPublished;
	}


	bool FPageRenderData::PublishResidentCaptureTreeFrontier_RenderThread(
		TArray<FExplicitSplatRef>&& RenderSplats,
		TArray<uint64>&& RenderPageIds,
		TArray<FExplicitSplatRun>&& RenderRuns,
		const bool bAlreadySortedUnique)
	{
		check(IsInRenderingThread());
		if (!bTreeLODSource || !bBoundedStreamingEnabled ||
			(RenderSplats.IsEmpty() && RenderRuns.IsEmpty()))
		{
			return false;
		}
		if (!bAlreadySortedUnique)
		{
			SortUniqueSplatRefs(RenderSplats);
			RenderPageIds.Reset();
			RenderPageIds.Reserve(RenderSplats.Num());
			for (const FExplicitSplatRef& Ref : RenderSplats)
			{
				const FPageRecord* Page = Manifest.FindPage(Ref.PageId);
				if (Page == nullptr || Ref.PageLocal >= Page->SplatCount)
					return false;
				RenderPageIds.Add(Ref.PageId);
			}
			SortUniquePageIds(RenderPageIds);
			RenderRuns.Reset();
		}
		if (AreSplatRefArraysEqual(RenderSplats, PublishedCaptureSplatRefs) &&
			AreSplatRunArraysEqual(RenderRuns, PublishedCaptureSplatRuns))
		{
			return true;
		}

		TArray<uint64> SavedTargetPageIds = MoveTemp(TargetPageIds);
		TArray<FExplicitSplatRef> SavedTargetSplatRefs = MoveTemp(TargetSplatRefs);
		TArray<FExplicitSplatRun> SavedTargetSplatRuns = MoveTemp(TargetSplatRuns);
		TargetPageIds = MoveTemp(RenderPageIds);
		TargetSplatRefs = MoveTemp(RenderSplats);
		TargetSplatRuns = MoveTemp(RenderRuns);
		bPublishingCaptureTreeFrontier = true;
		TryPublishBoundedFrontier_RenderThread(UploadEpoch.load(std::memory_order_acquire));
		bPublishingCaptureTreeFrontier = false;
		const bool bPublished = AreSplatRefArraysEqual(TargetSplatRefs, PublishedCaptureSplatRefs) &&
			AreSplatRunArraysEqual(TargetSplatRuns, PublishedCaptureSplatRuns);
		TargetPageIds = MoveTemp(SavedTargetPageIds);
		TargetSplatRefs = MoveTemp(SavedTargetSplatRefs);
		TargetSplatRuns = MoveTemp(SavedTargetSplatRuns);
		return bPublished;
	}

	void FPageRenderData::NotifyFrontierDrawn_RenderThread(FRHICommandListImmediate& RHICmdList, const bool bCaptureFrontier)
	{
		check(IsInRenderingThread());
		const uint32 DrawCount = bCaptureFrontier
			? CaptureActiveSplatCount.load(std::memory_order_acquire)
			: ActiveSplatCount.load(std::memory_order_acquire);
		if (!bBoundedStreamingEnabled || DrawCount == 0)
		{
			return;
		}

		if (bCaptureFrontier)
		{
			bCaptureFrontierHasBeenDrawn = true;
		}
		else
		{
			bCurrentFrontierHasBeenDrawn = true;
		}
		FGPUFenceRHIRef Fence = RHICreateGPUFence(
			bCaptureFrontier ? TEXT("NanoGSCaptureFrontierLastUse") : TEXT("NanoGSExactHQFrontierLastUse"));
		if (Fence.IsValid())
		{
			RHICmdList.WriteGPUFence(Fence);
			if (bCaptureFrontier)
			{
				CaptureFrontierLastUseFence = MoveTemp(Fence);
				bCaptureFrontierFenceUnavailable = false;
			}
			else
			{
				CurrentFrontierLastUseFence = MoveTemp(Fence);
				bCurrentFrontierFenceUnavailable = false;
			}
		}
		else if (bCaptureFrontier)
		{
			bCaptureFrontierFenceUnavailable = true;
		}
		else
		{
			bCurrentFrontierFenceUnavailable = true;
		}
	}

	const FBufferRHIRef& FPageRenderData::GetPublishedActivePhysicalIndexBuffer_RenderThread(const bool bCaptureFrontier) const
	{
		check(IsInRenderingThread());
		if (!bCaptureFrontier && bTreeGPUFrontierPublished.load(std::memory_order_acquire))
		{
			return TreeGPUPublishedPhysicalIndexBuffer;
		}
		return ActivePhysicalIndexBuffers[bCaptureFrontier ? PublishedCaptureActiveIndexBufferSlot : PublishedActiveIndexBufferSlot];
	}

	const FShaderResourceViewRHIRef&
	FPageRenderData::GetPublishedActivePhysicalIndexBufferSRV_RenderThread(const bool bCaptureFrontier) const
	{
		check(IsInRenderingThread());
		if (!bCaptureFrontier && bTreeGPUFrontierPublished.load(std::memory_order_acquire))
		{
			return TreeGPUPublishedPhysicalIndexSRV;
		}
		return ActivePhysicalIndexBufferSRVs[bCaptureFrontier ? PublishedCaptureActiveIndexBufferSlot : PublishedActiveIndexBufferSlot];
	}

	bool FPageRenderData::ReleaseCompletedRetirements_RenderThread()
	{
		check(IsInRenderingThread());
		bool bReleasedAny = false;
		for (int32 Index = RetiredFrontiers.Num() - 1; Index >= 0; --Index)
		{
			const FRetiredFrontier& Retired = RetiredFrontiers[Index];
			const bool bFenceComplete = !Retired.LastUseFence.IsValid() ||
				(Retired.LastUseFence->NumPendingWriteCommands.GetValue() == 0 &&
				 Retired.LastUseFence->Poll());
			if (!bFenceComplete)
			{
				continue;
			}

			FString Error;
			if (Retired.PinGeneration != 0 &&
				!ResidentPool.ReleaseCaptureGeneration(Retired.PinGeneration, Error))
			{
				UE_LOG(LogTemp, Error,
					TEXT("NanoGS bounded Page retirement generation %llu could not be released: %s"),
					static_cast<unsigned long long>(Retired.PinGeneration), *Error);
				continue;
			}
			RetiredFrontiers.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			bReleasedAny = true;
		}
		return bReleasedAny;
	}

	void FPageRenderData::FailFenceSafetyClosed_RenderThread(const FString& Error)
	{
		check(IsInRenderingThread());
		UploadEpoch.fetch_add(1, std::memory_order_acq_rel);
		bPublicationPendingOnIndexFence = false;
		PendingPublicationUploadEpoch = 0;
		bPermanentFenceSafetyFailure = true;
		bUploadFailed.store(true, std::memory_order_release);
		bUploadComplete.store(false, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		StreamingState.store(
			static_cast<uint8>(EExactHQStreamingState::Failed),
			std::memory_order_release);
		UE_LOG(LogTemp, Error,
			TEXT("NanoGS bounded Exact-HQ fence safety failure: %s. In-use source/index slots remain pinned and will not be overwritten."),
			*Error);
	}

	void FPageRenderData::RestorePublishedFrontierAfterDrain_RenderThread()
	{
		check(IsInRenderingThread());
		if (bPermanentFenceSafetyFailure)
		{
			return;
		}
		TargetPageIds = PublishedPageIds;
		RequestedPageCount.store(static_cast<uint32>(PublishedPageIds.Num()), std::memory_order_release);
		if (SceneBudgetIdentity != 0 && SceneBudgetOwnerIdentity != 0)
		{
			FString BudgetError;
			const uint64 ActiveLimit = static_cast<uint64>(FMath::Max(
				0, CVarNanoGSPagedGlobalMainActiveSplats.GetValueOnRenderThread()));
			if (!FSceneBudgetRegistry::Get().TrySetActiveSplats(
				SceneBudgetIdentity, SceneBudgetOwnerIdentity, ESceneActiveBudgetLane::Main,
				PublishedActiveSplatCount, ActiveLimit, BudgetError))
			{
				StreamingState.store(
					static_cast<uint8>(EExactHQStreamingState::OutOfCapacity),
					std::memory_order_release);
				UE_LOG(LogTemp, Warning,
					TEXT("NanoGS published frontier remains suspended by scene active budget: %s"),
					*BudgetError);
				return;
			}
		}
		const uint32 PreviousActiveCount =
			ActiveSplatCount.exchange(PublishedActiveSplatCount, std::memory_order_acq_rel);
		if (PreviousActiveCount != PublishedActiveSplatCount)
		{
			ResidentGeneration.fetch_add(1, std::memory_order_acq_rel);
		}
		bUploadFailed.store(false, std::memory_order_release);
		bUploadComplete.store(true, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		StreamingState.store(
			static_cast<uint8>(EExactHQStreamingState::Ready),
			std::memory_order_release);
	}

	void FPageRenderData::SuspendPublishedFrontierOutOfCapacity_RenderThread(
		TConstArrayView<uint64> RequestedPageIds,
		const FString& Reason)
	{
		check(IsInRenderingThread());
		TargetPageIds.Reset(RequestedPageIds.Num());
		TargetPageIds.Append(RequestedPageIds);
		RequestedPageCount.store(static_cast<uint32>(TargetPageIds.Num()), std::memory_order_release);
		bUploadFailed.store(false, std::memory_order_release);
		bUploadComplete.store(false, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		const uint32 PreviousActiveCount = ActiveSplatCount.exchange(0, std::memory_order_acq_rel);
		if (SceneBudgetIdentity != 0 && SceneBudgetOwnerIdentity != 0)
		{
			FString IgnoredBudgetError;
			FSceneBudgetRegistry::Get().TrySetActiveSplats(
				SceneBudgetIdentity, SceneBudgetOwnerIdentity, ESceneActiveBudgetLane::Main,
				0, 0, IgnoredBudgetError);
		}
		if (PreviousActiveCount != 0)
		{
			// Generation changes only when renderer-visible membership changes. Repeated
			// OOC requests therefore do not churn static-view sort invalidation.
			ResidentGeneration.fetch_add(1, std::memory_order_acq_rel);
		}
		StreamingState.store(
			static_cast<uint8>(EExactHQStreamingState::OutOfCapacity),
			std::memory_order_release);
		UE_LOG(LogTemp, Warning,
			TEXT("NanoGS bounded Exact-HQ demand OutOfCapacity: target=%d pages, published=%d pages retained, fixed slots=%u: %s. Rendering is suspended (NotReady); no stale partial frontier will be drawn."),
			TargetPageIds.Num(), PublishedPageIds.Num(), PhysicalSlotCount, *Reason);
	}

	void FPageRenderData::BeginBoundedDrain_RenderThread(
		TConstArrayView<uint64> RequestedPageIds)
	{
		check(IsInRenderingThread());
		if (bPermanentFenceSafetyFailure)
		{
			return;
		}
		if (CurrentTransitionPinGeneration != 0 || !InFlightLoadOperations.IsEmpty())
		{
			FailFenceSafetyClosed_RenderThread(
				TEXT("drain requested while another pinned/load transition was still active"));
			return;
		}

		TargetPageIds.Reset(RequestedPageIds.Num());
		TargetPageIds.Append(RequestedPageIds);
		RequestedPageCount.store(static_cast<uint32>(TargetPageIds.Num()), std::memory_order_release);
		bUploadFailed.store(false, std::memory_order_release);
		bUploadComplete.store(false, std::memory_order_release);
		const uint32 PreviousActiveCount = ActiveSplatCount.exchange(0, std::memory_order_acq_rel);
		if (SceneBudgetIdentity != 0 && SceneBudgetOwnerIdentity != 0)
		{
			FString IgnoredBudgetError;
			FSceneBudgetRegistry::Get().TrySetActiveSplats(
				SceneBudgetIdentity, SceneBudgetOwnerIdentity, ESceneActiveBudgetLane::Main,
				0, 0, IgnoredBudgetError);
		}
		if (PreviousActiveCount != 0)
		{
			ResidentGeneration.fetch_add(1, std::memory_order_acq_rel);
		}
		StreamingState.store(
			static_cast<uint8>(EExactHQStreamingState::Draining),
			std::memory_order_release);
		UE_LOG(LogTemp, Log,
			TEXT("NanoGS bounded Exact-HQ drain started: target=%d pages fits %u slots but cannot coexist with published=%d pages; rendering is NotReady until the old last-use fence retires."),
			TargetPageIds.Num(), PhysicalSlotCount, PublishedPageIds.Num());
		AdvanceBoundedDrain_RenderThread();
	}

	void FPageRenderData::AdvanceBoundedDrain_RenderThread()
	{
		check(IsInRenderingThread());
		if (GetStreamingState() != EExactHQStreamingState::Draining ||
			bPermanentFenceSafetyFailure)
		{
			return;
		}

		ReleaseCompletedRetirements_RenderThread();
		if (!PublishedPageIds.IsEmpty())
		{
			if (bCurrentFrontierHasBeenDrawn)
			{
				if (bCurrentFrontierFenceUnavailable || !CurrentFrontierLastUseFence.IsValid())
				{
					FailFenceSafetyClosed_RenderThread(
						TEXT("the suspended published frontier was drawn but has no usable last-use GPU fence"));
					return;
				}
				const bool bFenceComplete =
					CurrentFrontierLastUseFence->NumPendingWriteCommands.GetValue() == 0 &&
					CurrentFrontierLastUseFence->Poll();
				if (!bFenceComplete)
				{
					return;
				}
			}

			// No future draw can reference this frontier (active count is zero), and
			// its last prior draw is complete. Its unpinned pool slots are now reusable.
			PublishedPageIds.Reset();
			PublishedSplatRefs.Reset();
			PublishedActiveSplatCount = 0;
			PublishedPageCount.store(0, std::memory_order_release);
			LoadedPageCount.store(0, std::memory_order_release);
			CurrentFrontierLastUseFence.SafeRelease();
			bCurrentFrontierHasBeenDrawn = false;
			bCurrentFrontierFenceUnavailable = false;
		}

		// Older double-buffer frontiers own independent pins/fences. Wait rather
		// than repeatedly asking the transactional planner to evict them.
		if (!RetiredFrontiers.IsEmpty())
		{
			return;
		}

		TArray<uint64> DrainedTarget = TargetPageIds;
		TArray<FExplicitSplatRef> DrainedSplats = TargetSplatRefs;
		TArray<FExplicitSplatRun> DrainedRuns = TargetSplatRuns;
		TargetPageIds.Reset();
		TargetSplatRefs.Reset();
		TargetSplatRuns.Reset();
		StreamingState.store(
			static_cast<uint8>(EExactHQStreamingState::NotRequested),
			std::memory_order_release);
		ApplyBoundedDemand_RenderThread(MoveTemp(DrainedTarget), false, MoveTemp(DrainedSplats), MoveTemp(DrainedRuns));
	}

	bool FPageRenderData::CancelBoundedTransition_RenderThread()
	{
		check(IsInRenderingThread());
		UploadEpoch.fetch_add(1, std::memory_order_acq_rel);
		for (const FPageLoadOperation& Load : InFlightLoadOperations)
		{
			FString IgnoredError;
			ResidentPool.AbandonLoad(Load, IgnoredError);
		}
		InFlightLoadOperations.Reset();
		bPublicationPendingOnIndexFence = false;
		PendingPublicationUploadEpoch = 0;
		if (CurrentTransitionPinGeneration != 0)
		{
			FString Error;
			if (!ResidentPool.ReleaseCaptureGeneration(CurrentTransitionPinGeneration, Error))
			{
				FailFenceSafetyClosed_RenderThread(FString::Printf(
					TEXT("cancelled transition pin %llu could not be released: %s"),
					static_cast<unsigned long long>(CurrentTransitionPinGeneration), *Error));
				return false;
			}
			CurrentTransitionPinGeneration = 0;
		}
		TargetPageIds.Reset();
		TargetSplatRefs.Reset();
		TargetSplatRuns.Reset();
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		return true;
	}

	void FPageRenderData::ApplyBoundedDemand_RenderThread(
		TArray<uint64> RequestedPageIds,
		const bool bAllowPublishedFrontierForCapture,
		TArray<FExplicitSplatRef> RequestedSplats,
		TArray<FExplicitSplatRun> RequestedRuns)
	{
		check(IsInRenderingThread());
		if (!bBoundedStreamingEnabled || !ResidentPool.IsInitialized())
		{
			return;
		}
		if (bPermanentFenceSafetyFailure)
		{
			return;
		}

		const EExactHQStreamingState ExistingState = GetStreamingState();
		if (ExistingState == EExactHQStreamingState::Draining)
		{
			RequestedPageCount.store(static_cast<uint32>(RequestedPageIds.Num()), std::memory_order_release);
			if (RequestedPageIds.Num() > static_cast<int32>(PhysicalSlotCount))
			{
				SuspendPublishedFrontierOutOfCapacity_RenderThread(
					RequestedPageIds,
					TEXT("target alone exceeds the fixed slot budget during drain"));
				return;
			}
			if (ArePageIdArraysEqual(RequestedPageIds, PublishedPageIds) &&
				AreSplatRefArraysEqual(RequestedSplats, PublishedSplatRefs) &&
				AreSplatRunArraysEqual(RequestedRuns, PublishedSplatRuns))
			{
				RestorePublishedFrontierAfterDrain_RenderThread();
				return;
			}
			TargetPageIds = MoveTemp(RequestedPageIds);
			TargetSplatRefs = MoveTemp(RequestedSplats);
			TargetSplatRuns = MoveTemp(RequestedRuns);
			AdvanceBoundedDrain_RenderThread();
			return;
		}

		if (ExistingState == EExactHQStreamingState::Loading)
		{
			if (ArePageIdArraysEqual(RequestedPageIds, TargetPageIds) &&
				AreSplatRefArraysEqual(RequestedSplats, TargetSplatRefs) &&
				AreSplatRunArraysEqual(RequestedRuns, TargetSplatRuns))
			{
				return;
			}
			// LCC-style stable residency: never discard useful in-flight IO/upload
			// because a newer camera sample arrived. The old complete frontier stays
			// drawable, the current transition publishes atomically, and the next
			// render request applies the latest node-level union.
			return;
		}

		RequestedPageCount.store(static_cast<uint32>(RequestedPageIds.Num()), std::memory_order_release);
		bUploadFailed.store(false, std::memory_order_release);
		if (RequestedPageIds.Num() > static_cast<int32>(PhysicalSlotCount))
		{
			SuspendPublishedFrontierOutOfCapacity_RenderThread(
				RequestedPageIds,
				TEXT("target alone exceeds the fixed slot budget"));
			return;
		}
		if (ArePageIdArraysEqual(RequestedPageIds, PublishedPageIds) &&
			AreSplatRefArraysEqual(RequestedSplats, PublishedSplatRefs) &&
			AreSplatRunArraysEqual(RequestedRuns, PublishedSplatRuns))
		{
			bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
			RestorePublishedFrontierAfterDrain_RenderThread();
			return;
		}

		ReleaseCompletedRetirements_RenderThread();
		if (bTreeGPUFrontierPublished.load(std::memory_order_acquire))
		{
			bool bKeepsPublishedGPUFrontierResident = true;
			for (const uint64 PageId : TreeGPUPublishedPageIds)
			{
				if (!RequestedPageIds.Contains(PageId))
				{
					bKeepsPublishedGPUFrontierResident = false;
					break;
				}
			}
			if (!bKeepsPublishedGPUFrontierResident)
			{
				RetireTreeGPUFrontier_RenderThread(TEXT("CPU target does not retain the published GPU pages"));
			}
		}
		TargetPageIds = MoveTemp(RequestedPageIds);
		TargetSplatRefs = MoveTemp(RequestedSplats);
		TargetSplatRuns = MoveTemp(RequestedRuns);
		FExactHQPageRequest Request;
		Request.Generation = NextRequestGeneration++;
		Request.VisiblePageIds = TargetPageIds;
		Request.PreferredLoadPageIds = TargetPageIds;
		// The complete old frontier is a retain/pin set, never an eviction candidate
		// while its replacement is loading. The pool plans over target U old.
		Request.RequiredPageIds = PublishedPageIds;

		FExactHQResidentPlan Plan;
		const EPageResidentPlanResult Result = ResidentPool.ApplyExactHQRequest(Request, Plan);
		if (Result != EPageResidentPlanResult::Success)
		{
			bUploadComplete.store(false, std::memory_order_release);
			bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
			const bool bCapacity = Result == EPageResidentPlanResult::OutOfCapacity;
			if (bCapacity &&
				(!PublishedPageIds.IsEmpty() || !RetiredFrontiers.IsEmpty()))
			{
				// The target fits by itself. Stop issuing draws, wait for every old
				// last-use fence, then re-plan without retaining the old frontier.
				BeginBoundedDrain_RenderThread(RequestedPageIds);
				return;
			}
			StreamingState.store(
				static_cast<uint8>(bCapacity
					? EExactHQStreamingState::OutOfCapacity
					: EExactHQStreamingState::Failed),
				std::memory_order_release);
			bUploadFailed.store(!bCapacity, std::memory_order_release);
			if (bCapacity)
			{
				SuspendPublishedFrontierOutOfCapacity_RenderThread(
					RequestedPageIds,
					Plan.Error.IsEmpty() ? TEXT("transactional planner has no reusable safe slots") : Plan.Error);
			}
			else
			{
				UE_LOG(LogTemp, Error,
					TEXT("NanoGS bounded Exact-HQ demand rejected: target=%d pages, published=%d pages, fixed slots=%u, result=%u: %s. Published frontier storage is retained; renderer visibility is unchanged."),
					TargetPageIds.Num(), PublishedPageIds.Num(), PhysicalSlotCount,
					static_cast<uint32>(Result), *Plan.Error);
			}
			return;
		}
		PageResidencyGeneration.fetch_add(1, std::memory_order_acq_rel);

		CurrentTransitionPinGeneration = PublishedPageIds.IsEmpty() ? 0 : Request.Generation;
		InFlightLoadOperations = Plan.LoadOperations;
		bUploadComplete.store(false, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(
			bAllowPublishedFrontierForCapture && !PublishedPageIds.IsEmpty(),
			std::memory_order_release);
		const uint32 NewUploadEpoch = UploadEpoch.fetch_add(1, std::memory_order_acq_rel) + 1u;
		if (InFlightLoadOperations.IsEmpty())
		{
			TryPublishBoundedFrontier_RenderThread(NewUploadEpoch);
			return;
		}

		StreamingState.store(static_cast<uint8>(EExactHQStreamingState::Loading), std::memory_order_release);
		StartBoundedLoads(InFlightLoadOperations, NewUploadEpoch);
	}

	void FPageRenderData::HandleBoundedFailure_RenderThread(
		const FString& Error,
		const uint32 UploadEpochValue)
	{
		check(IsInRenderingThread());
		if (UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
		{
			return;
		}

		UploadEpoch.fetch_add(1, std::memory_order_acq_rel);
		for (const FPageLoadOperation& Load : InFlightLoadOperations)
		{
			FString IgnoredError;
			ResidentPool.AbandonLoad(Load, IgnoredError);
		}
		InFlightLoadOperations.Reset();
		bPublicationPendingOnIndexFence = false;
		PendingPublicationUploadEpoch = 0;
		if (CurrentTransitionPinGeneration != 0)
		{
			FString ReleaseError;
			if (!ResidentPool.ReleaseCaptureGeneration(CurrentTransitionPinGeneration, ReleaseError))
			{
				UE_LOG(LogTemp, Warning,
					TEXT("NanoGS bounded Page failed transition pin release also failed: %s"),
					*ReleaseError);
			}
			CurrentTransitionPinGeneration = 0;
		}

		bUploadFailed.store(true, std::memory_order_release);
		bUploadComplete.store(false, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		StreamingState.store(static_cast<uint8>(EExactHQStreamingState::Failed), std::memory_order_release);
		UE_LOG(LogTemp, Error,
			TEXT("NanoGS bounded Exact-HQ target failed: %s. Published frontier storage is retained; renderer visibility is unchanged."),
			*Error);
	}

	bool FPageRenderData::AllocateTreeGPUBlockNodes_RenderThread(
		const uint32 BlockId,
		const uint32 NodeCount,
		uint32& OutNodeBase)
	{
		check(IsInRenderingThread());
		OutNodeBase = 0;
		if (NodeCount == 0 || NodeCount > TreeGPUBlockPoolNodeCapacity ||
			!TreeGPUBlockToNodeBase.IsValidIndex(static_cast<int32>(BlockId)) ||
			TreeGPUBlockToNodeBase[BlockId] != INDEX_NONE)
		{
			return false;
		}

		int32 BestRangeIndex = INDEX_NONE;
		uint32 BestRangeSize = MAX_uint32;
		for (int32 RangeIndex = 0; RangeIndex < TreeGPUBlockFreeRanges.Num(); ++RangeIndex)
		{
			const FTreeGPUBlockFreeRange& Range = TreeGPUBlockFreeRanges[RangeIndex];
			if (Range.NodeCount >= NodeCount && Range.NodeCount < BestRangeSize)
			{
				BestRangeIndex = RangeIndex;
				BestRangeSize = Range.NodeCount;
			}
		}
		if (BestRangeIndex == INDEX_NONE)
		{
			return false;
		}

		FTreeGPUBlockFreeRange& Range = TreeGPUBlockFreeRanges[BestRangeIndex];
		OutNodeBase = Range.FirstNode;
		Range.FirstNode += NodeCount;
		Range.NodeCount -= NodeCount;
		if (Range.NodeCount == 0)
		{
			TreeGPUBlockFreeRanges.RemoveAtSwap(BestRangeIndex, 1, EAllowShrinking::No);
		}
		TreeGPUBlockToNodeBase[BlockId] = static_cast<int32>(OutNodeBase);
		TreeGPUBlockAllocationNodeCounts[BlockId] = NodeCount;
		return true;
	}

	void FPageRenderData::FreeTreeGPUBlockNodes_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const uint32 BlockId)
	{
		check(IsInRenderingThread());
		if (!TreeGPUBlockToNodeBase.IsValidIndex(static_cast<int32>(BlockId)) ||
			TreeGPUBlockToNodeBase[BlockId] == INDEX_NONE)
		{
			return;
		}

		const uint32 NodeBase = static_cast<uint32>(TreeGPUBlockToNodeBase[BlockId]);
		const uint32 NodeCount = TreeGPUBlockAllocationNodeCounts[BlockId];
		TreeGPUBlockToNodeBase[BlockId] = INDEX_NONE;
		TreeGPUBlockAllocationNodeCounts[BlockId] = 0;
		if (NodeCount > 0)
		{
			TreeGPUBlockFreeRanges.Add({NodeBase, NodeCount});
			TreeGPUBlockFreeRanges.Sort([](const FTreeGPUBlockFreeRange& A, const FTreeGPUBlockFreeRange& B)
			{
				return A.FirstNode < B.FirstNode;
			});
			for (int32 RangeIndex = TreeGPUBlockFreeRanges.Num() - 1; RangeIndex > 0; --RangeIndex)
			{
				FTreeGPUBlockFreeRange& Previous = TreeGPUBlockFreeRanges[RangeIndex - 1];
				const FTreeGPUBlockFreeRange& Current = TreeGPUBlockFreeRanges[RangeIndex];
				if (Previous.FirstNode + Previous.NodeCount == Current.FirstNode)
				{
					Previous.NodeCount += Current.NodeCount;
					TreeGPUBlockFreeRanges.RemoveAt(RangeIndex, 1, EAllowShrinking::No);
				}
			}
		}
		if (TreeGPUBlockResidencyBuffer.IsValid())
		{
			uint32* ResidencyDest = static_cast<uint32*>(RHICmdList.LockBuffer(
				TreeGPUBlockResidencyBuffer, BlockId * sizeof(uint32), sizeof(uint32), RLM_WriteOnly));
			*ResidencyDest = MAX_uint32;
			RHICmdList.UnlockBuffer(TreeGPUBlockResidencyBuffer);
		}
	}

	void FPageRenderData::StartTreeV3BlockLoads_RenderThread(TConstArrayView<uint32> BlockIds)
	{
		check(IsInRenderingThread());
		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		if (!TreeV3Manifest.IsValid() || !TreeGPUBlockNodeBuffer.IsValid() ||
			!TreeGPUBlockResidencyBuffer.IsValid() || TreeGPUBlockPoolNodeCapacity == 0)
		{
			return;
		}
		if (!TreeGPUBlocksInFlight.IsEmpty())
		{
			return;
		}

		struct FBlockLoadJob
		{
			uint32 BlockId = 0;
			uint32 NodeBase = 0;
			NanoGS::TreeV3::FBlockRecord Record;
		};
		TArray<FBlockLoadJob> Jobs;
		const int32 MaxLoads = FMath::Clamp(
			CVarNanoGSTreeGPUBlockLoadsPerUpdate.GetValueOnRenderThread(), 1, 256);
		auto BlockPriority = [this](const uint32 BlockId)
		{
			if (!TreeGPUBlockCenterAndFeatureRadius.IsValidIndex(static_cast<int32>(BlockId)))
			{
				return 0.0f;
			}
			const FVector4f& Data = TreeGPUBlockCenterAndFeatureRadius[BlockId];
			const FVector3f Center(Data.X, Data.Y, Data.Z);
			const float Distance = FMath::Max((Center - TreeGPULastViewOriginLocal).Length(), 1.0f);
			return Data.W * TreeGPULastProjectionScale / Distance;
		};
		TArray<uint32> PrioritizedBlockIds(BlockIds);
		PrioritizedBlockIds.Sort([&BlockPriority](const uint32 A, const uint32 B)
		{
			const float ScoreA = BlockPriority(A);
			const float ScoreB = BlockPriority(B);
			return ScoreA != ScoreB ? ScoreA > ScoreB : A < B;
		});

		TSet<uint32> RequestedBlocks;
		RequestedBlocks.Reserve(PrioritizedBlockIds.Num());
		for (const uint32 BlockId : PrioritizedBlockIds)
		{
			if (TreeGPUBlockToNodeBase.IsValidIndex(static_cast<int32>(BlockId)))
			{
				RequestedBlocks.Add(BlockId);
			}
		}

		TArray<uint32> EvictionCandidates;
		EvictionCandidates.Reserve(TreeGPUBlockToNodeBase.Num());
		const uint64 EvictionGraceEpochs = static_cast<uint64>(FMath::Clamp(
			CVarNanoGSTreeGPUBlockEvictionGraceEpochs.GetValueOnRenderThread(), 0, 64));
		for (int32 BlockIndex = 0; BlockIndex < TreeGPUBlockToNodeBase.Num(); ++BlockIndex)
		{
			const uint32 BlockId = static_cast<uint32>(BlockIndex);
			if (TreeGPUBlockToNodeBase[BlockIndex] == INDEX_NONE || RequestedBlocks.Contains(BlockId) ||
				TreeGPUBlocksInFlight.Contains(BlockId))
			{
				continue;
			}
			const uint64 LastRequestedEpoch = TreeGPUBlockLastRequestedFeedbackEpoch.IsValidIndex(BlockIndex)
				? TreeGPUBlockLastRequestedFeedbackEpoch[BlockIndex]
				: 0u;
			if (LastRequestedEpoch != 0u &&
				TreeGPUBlockFeedbackEpoch - LastRequestedEpoch < EvictionGraceEpochs)
			{
				continue;
			}
			EvictionCandidates.Add(BlockId);
		}
		EvictionCandidates.Sort([&BlockPriority](const uint32 A, const uint32 B)
		{
			const float ScoreA = BlockPriority(A);
			const float ScoreB = BlockPriority(B);
			return ScoreA != ScoreB ? ScoreA < ScoreB : A > B;
		});
		int32 NextEvictionCandidate = 0;
		int32 EvictedBlockCount = 0;

		TSet<uint32> UniqueRequests;
		for (const uint32 BlockId : PrioritizedBlockIds)
		{
			if (Jobs.Num() >= MaxLoads || !TreeV3Manifest->Blocks.IsValidIndex(static_cast<int32>(BlockId)) ||
				UniqueRequests.Contains(BlockId) || TreeGPUBlocksInFlight.Contains(BlockId) ||
				(TreeGPUBlockToNodeBase.IsValidIndex(static_cast<int32>(BlockId)) &&
					TreeGPUBlockToNodeBase[BlockId] != INDEX_NONE))
			{
				continue;
			}
			UniqueRequests.Add(BlockId);
			const NanoGS::TreeV3::FBlockRecord& Record = TreeV3Manifest->Blocks[BlockId];
			uint32 NodeBase = 0;
			while (!AllocateTreeGPUBlockNodes_RenderThread(BlockId, Record.NodeCount, NodeBase) &&
				NextEvictionCandidate < EvictionCandidates.Num())
			{
				const uint32 EvictedBlockId = EvictionCandidates[NextEvictionCandidate++];
				if (TreeGPUBlockToNodeBase[EvictedBlockId] == INDEX_NONE)
				{
					continue;
				}
				FreeTreeGPUBlockNodes_RenderThread(RHICmdList, EvictedBlockId);
				++EvictedBlockCount;
			}
			if (TreeGPUBlockToNodeBase[BlockId] == INDEX_NONE)
			{
				continue;
			}
			TreeGPUBlocksInFlight.Add(BlockId);
			Jobs.Add({BlockId, NodeBase, Record});
		}
		if (EvictedBlockCount > 0 && FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
		{
			UE_LOG(LogTemp, Display, TEXT("NanoGS Tree GPU evicted %d pressure-selected v3 blocks"),
				EvictedBlockCount);
		}
		if (Jobs.IsEmpty())
		{
			return;
		}

		const FString BlockFilename = TreeV3Manifest->BlockDataFilename;
		TWeakPtr<FPageRenderData, ESPMode::ThreadSafe> WeakThis = AsShared();
		Async(EAsyncExecution::ThreadPool,
			[WeakThis, BlockFilename, Jobs = MoveTemp(Jobs)]() mutable
			{
				struct FLoadedBlock
				{
					uint32 BlockId = 0;
					uint32 NodeBase = 0;
					TArray<NanoGS::Tree::FGPUNodeRecord> Nodes;
					bool bValid = false;
				};
				TArray<FLoadedBlock> LoadedBlocks;
				LoadedBlocks.Reserve(Jobs.Num());
				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*BlockFilename));
				for (const FBlockLoadJob& Job : Jobs)
				{
					FLoadedBlock& Loaded = LoadedBlocks.AddDefaulted_GetRef();
					Loaded.BlockId = Job.BlockId;
					Loaded.NodeBase = Job.NodeBase;
					if (!FileHandle)
					{
						continue;
					}
					const uint64 ByteOffset = static_cast<uint64>(Job.Record.FirstNode) *
						sizeof(NanoGS::Tree::FGPUNodeRecord);
					const uint64 ByteCount = static_cast<uint64>(Job.Record.NodeCount) *
						sizeof(NanoGS::Tree::FGPUNodeRecord);
					Loaded.Nodes.SetNumUninitialized(static_cast<int32>(Job.Record.NodeCount));
					if (!FileHandle->Seek(static_cast<int64>(ByteOffset)) ||
						!FileHandle->Read(reinterpret_cast<uint8*>(Loaded.Nodes.GetData()), static_cast<int64>(ByteCount)))
					{
						Loaded.Nodes.Reset();
						continue;
					}
					Loaded.bValid = true;
					for (const NanoGS::Tree::FGPUNodeRecord& Node : Loaded.Nodes)
					{
						if (Node.IsInternal() &&
							static_cast<uint64>(Node.FirstChild) + Node.GetChildCount() > Job.Record.NodeCount)
						{
							Loaded.bValid = false;
							break;
						}
					}
				}
				ENQUEUE_RENDER_COMMAND(NanoGSUploadTreeV3Blocks)(
					[WeakThis, LoadedBlocks = MoveTemp(LoadedBlocks)](FRHICommandListImmediate& RHICmdList) mutable
					{
						TSharedPtr<FPageRenderData, ESPMode::ThreadSafe> Self = WeakThis.Pin();
						if (!Self.IsValid())
						{
							return;
						}
						for (FLoadedBlock& Loaded : LoadedBlocks)
						{
							Self->TreeGPUBlocksInFlight.Remove(Loaded.BlockId);
							if (!Loaded.bValid || !Self->TreeGPUBlockNodeBuffer.IsValid() ||
								!Self->TreeGPUBlockResidencyBuffer.IsValid())
							{
								Self->FreeTreeGPUBlockNodes_RenderThread(RHICmdList, Loaded.BlockId);
								continue;
							}
							const uint32 ByteOffset = Loaded.NodeBase * sizeof(NanoGS::Tree::FGPUNodeRecord);
							const uint32 ByteCount = Loaded.Nodes.Num() * sizeof(NanoGS::Tree::FGPUNodeRecord);
							void* Dest = RHICmdList.LockBuffer(
								Self->TreeGPUBlockNodeBuffer, ByteOffset, ByteCount, RLM_WriteOnly);
							FMemory::Memcpy(Dest, Loaded.Nodes.GetData(), ByteCount);
							RHICmdList.UnlockBuffer(Self->TreeGPUBlockNodeBuffer);
							uint32* ResidencyDest = static_cast<uint32*>(RHICmdList.LockBuffer(
								Self->TreeGPUBlockResidencyBuffer, Loaded.BlockId * sizeof(uint32),
								sizeof(uint32), RLM_WriteOnly));
							*ResidencyDest = Loaded.NodeBase;
							RHICmdList.UnlockBuffer(Self->TreeGPUBlockResidencyBuffer);
						}
					});
			});
	}

	void FPageRenderData::ConsumeTreeGPUBlockRequests_RenderThread(TArray<uint32>& OutBlockIds)
	{
		check(IsInRenderingThread());
		OutBlockIds = MoveTemp(PendingTreeGPUBlockRequests);
		PendingTreeGPUBlockRequests.Reset();
	}


	void FPageRenderData::TryStartTreeGPUPagePrefetch_RenderThread()
	{
		check(IsInRenderingThread());
		if (PendingTreeGPUPageRequests.IsEmpty() ||
			GetStreamingState() != EExactHQStreamingState::Ready ||
			!bBoundedStreamingEnabled || !ResidentPool.IsInitialized())
		{
			return;
		}

		ReleaseCompletedRetirements_RenderThread();
		if (!RetiredFrontiers.IsEmpty())
		{
			return;
		}

		TArray<uint64> PrefetchPageIds = PublishedPageIds;
		TSet<uint64> ProtectedPages;
		ProtectedPages.Reserve(PublishedPageIds.Num() + PendingTreeGPUPageRequests.Num());
		for (const uint64 PageId : PublishedPageIds)
		{
			ProtectedPages.Add(PageId);
		}

		const int32 PrefetchLimit = FMath::Max(
			0, CVarNanoGSTreeGPUPagePrefetchPerUpdate.GetValueOnRenderThread());
		int32 MissingPagesAdmitted = 0;
		int32 TotalPagesAdmitted = 0;
		for (const uint32 PageId : PendingTreeGPUPageRequests)
		{
			if (ProtectedPages.Contains(PageId) || Manifest.FindPage(PageId) == nullptr ||
				PrefetchPageIds.Num() >= static_cast<int32>(PhysicalSlotCount))
			{
				continue;
			}

			FPagePhysicalRange ExistingRange;
			const bool bAlreadyResident = ResidentPool.FindPhysicalRange(PageId, ExistingRange) &&
				ExistingRange.State == EPageSlotState::Resident;
			if (!bAlreadyResident && MissingPagesAdmitted >= PrefetchLimit)
			{
				continue;
			}

			PrefetchPageIds.Add(PageId);
			ProtectedPages.Add(PageId);
			++TotalPagesAdmitted;
			MissingPagesAdmitted += bAlreadyResident ? 0 : 1;
		}
		if (TotalPagesAdmitted == 0)
		{
			return;
		}
		SortUniquePageIds(PrefetchPageIds);

		FExactHQPageRequest Request;
		Request.Generation = NextRequestGeneration++;
		Request.VisiblePageIds = PrefetchPageIds;
		Request.RequiredPageIds = PublishedPageIds;
		Request.PreferredLoadPageIds = PrefetchPageIds;
		FExactHQResidentPlan Plan;
		const EPageResidentPlanResult Result = ResidentPool.ApplyExactHQRequest(Request, Plan);
		if (Result != EPageResidentPlanResult::Success)
		{
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree GPU prefetch deferred: admitted=%d missing=%d published=%d slots=%u result=%u: %s"),
					TotalPagesAdmitted, MissingPagesAdmitted, PublishedPageIds.Num(), PhysicalSlotCount,
					static_cast<uint32>(Result), *Plan.Error);
			}
			return;
		}

		TargetPageIds = MoveTemp(PrefetchPageIds);
		TargetSplatRefs = MoveTemp(PublishedSplatRefs);
		TargetSplatRuns = MoveTemp(PublishedSplatRuns);
		CurrentTransitionPinGeneration = PublishedPageIds.IsEmpty() ? 0 : Request.Generation;
		InFlightLoadOperations = Plan.LoadOperations;
		bUploadComplete.store(false, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		const uint32 NewUploadEpoch = UploadEpoch.fetch_add(1, std::memory_order_acq_rel) + 1u;

		if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
		{
			UE_LOG(LogTemp, Display,
				TEXT("NanoGS Tree GPU prefetch transaction: admitted=%d missing=%d target=%d published=%d loads=%d epoch=%u"),
				TotalPagesAdmitted, MissingPagesAdmitted, TargetPageIds.Num(), PublishedPageIds.Num(),
				InFlightLoadOperations.Num(), NewUploadEpoch);
		}

		if (InFlightLoadOperations.IsEmpty())
		{
			TryPublishBoundedFrontier_RenderThread(NewUploadEpoch);
			return;
		}
		StreamingState.store(static_cast<uint8>(EExactHQStreamingState::Loading), std::memory_order_release);
		StartBoundedLoads(InFlightLoadOperations, NewUploadEpoch);
	}

	void FPageRenderData::RetireTreeGPUFrontier_RenderThread(const TCHAR* Reason)
	{
		check(IsInRenderingThread());
		const bool bWasPublished = bTreeGPUFrontierPublished.exchange(false, std::memory_order_acq_rel);
		TreeGPUActiveSplatCount.store(0, std::memory_order_release);
		TreeGPUPublishedPageIds.Reset();
		if (bWasPublished)
		{
			ResidentGeneration.fetch_add(1, std::memory_order_acq_rel);
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				UE_LOG(LogTemp, Display, TEXT("NanoGS Tree GPU published frontier retired: %s"),
					Reason != nullptr ? Reason : TEXT("unspecified"));
			}
		}
	}

	bool FPageRenderData::AdvanceTreeGPUFrontier_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const FVector3f& ViewOriginLocal,
		const float ProjectionScale,
		const float SplitThresholdPixels,
		const bool bResetToRoot)
	{
		check(IsInRenderingThread());
		TreeGPULastViewOriginLocal = ViewOriginLocal;
		TreeGPULastProjectionScale = ProjectionScale;
		bool bRestartForPriorityStage = false;
		if (!HasTreeGPUNodeBuffer() || TreeGPUFrontierCapacity == 0 ||
			!TreeGPUFrontierIndirectArgsBuffer.IsValid() || !TreeGPUFrontierIndirectArgsUAV.IsValid() ||
			!TreeGPUPublishedPhysicalIndexBuffer.IsValid() || !TreeGPUPublishedPhysicalIndexSRV.IsValid() ||
			!TreeGPUBlockRequestBuffer.IsValid() || !TreeGPUBlockRequestUAV.IsValid() ||
			!TreeGPUBlockRequestBitsBuffer.IsValid() || !TreeGPUBlockRequestBitsUAV.IsValid() ||
			!TreeGPUBlockNodeBufferSRV.IsValid() || !TreeGPUBlockResidencyBufferSRV.IsValid() ||
			!TreeGPUPageResidencyBufferSRV.IsValid() || !TreeGPUPageRequestBuffer.IsValid() ||
			!TreeGPUPageRequestUAV.IsValid() || !TreeGPUPageRequestBitsBuffer.IsValid() ||
			!TreeGPUPageRequestBitsUAV.IsValid())
		{
			return false;
		}
		if (bTreeGPUPageRequestReadbackPending &&
			(!TreeGPUPageRequestReadback.IsValid() || !TreeGPUPageRequestReadback->IsReady()))
		{
			++TreeGPUPageReadbackWaitFrames;
		}
		if (bTreeGPUBlockRequestReadbackPending &&
			(!TreeGPUBlockRequestReadback.IsValid() || !TreeGPUBlockRequestReadback->IsReady()))
		{
			++TreeGPUBlockReadbackWaitFrames;
		}
		if (bTreeGPUPageRequestReadbackPending && TreeGPUPageRequestReadback.IsValid() &&
			TreeGPUPageRequestReadback->IsReady())
		{
			const uint32 ReadbackBytes = (TreeGPUPageRequestCapacity + 1u) * sizeof(uint32);
			const uint32* Readback = static_cast<const uint32*>(TreeGPUPageRequestReadback->Lock(ReadbackBytes));
			if (Readback != nullptr)
			{
				const uint32 RequestCount = FMath::Min(Readback[0], TreeGPUPageRequestCapacity);
				PendingTreeGPUPageRequests.Reset(RequestCount);
				PendingTreeGPUPageRequests.Append(Readback + 1, RequestCount);
				TreeGPUPageRequestReadback->Unlock();
				PendingTreeGPUPageRequests.Sort();
				PendingTreeGPUPageRequests.SetNum(Algo::Unique(PendingTreeGPUPageRequests));
				if (!PendingTreeGPUPageRequests.IsEmpty() &&
					FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
				{
					UE_LOG(LogTemp, Display, TEXT("NanoGS Tree GPU requested %d unique Page v1 pages"),
						PendingTreeGPUPageRequests.Num());
				}
			}
			bTreeGPUPageRequestReadbackPending = false;
		}

		const uint32 CurrentPageResidencyGeneration =
			PageResidencyGeneration.load(std::memory_order_acquire);
		if (CurrentPageResidencyGeneration != LastTreeGPUPageResidencyGeneration)
		{
			TArray<uint32> PagePhysicalBases;
			PagePhysicalBases.Init(MAX_uint32, TreeGPUPageAddressCapacity);
			for (const FPageRecord& Page : Manifest.Pages)
			{
				FPagePhysicalRange Range;
				if (Page.PageId < TreeGPUPageAddressCapacity && ResidentPool.FindPhysicalRange(Page.PageId, Range) &&
					Range.State == EPageSlotState::Resident && Range.FirstSplat <= MAX_uint32)
				{
					PagePhysicalBases[static_cast<int32>(Page.PageId)] = static_cast<uint32>(Range.FirstSplat);
				}
			}
			void* ResidencyDest = RHICmdList.LockBuffer(
				TreeGPUPageResidencyBuffer, 0, PagePhysicalBases.Num() * sizeof(uint32), RLM_WriteOnly);
			FMemory::Memcpy(ResidencyDest, PagePhysicalBases.GetData(), PagePhysicalBases.Num() * sizeof(uint32));
			RHICmdList.UnlockBuffer(TreeGPUPageResidencyBuffer);
			LastTreeGPUPageResidencyGeneration = CurrentPageResidencyGeneration;
		}

		if (bTreeGPUBlockRequestReadbackPending && TreeGPUBlockRequestReadback.IsValid() &&
			TreeGPUBlockRequestReadback->IsReady())
		{
			const uint32 ReadbackBytes = (TreeGPUBlockRequestCapacity + 1u) * sizeof(uint32);
			const uint32* Readback = static_cast<const uint32*>(TreeGPUBlockRequestReadback->Lock(ReadbackBytes));
			if (Readback != nullptr)
			{
				const uint32 RequestCount = FMath::Min(Readback[0], TreeGPUBlockRequestCapacity);
				PendingTreeGPUBlockRequests.Reset(RequestCount);
				PendingTreeGPUBlockRequests.Append(Readback + 1, RequestCount);
				TreeGPUBlockRequestReadback->Unlock();
				PendingTreeGPUBlockRequests.Sort();
				PendingTreeGPUBlockRequests.SetNum(Algo::Unique(PendingTreeGPUBlockRequests));
				++TreeGPUBlockFeedbackEpoch;
				for (const uint32 BlockId : PendingTreeGPUBlockRequests)
				{
					if (TreeGPUBlockLastRequestedFeedbackEpoch.IsValidIndex(static_cast<int32>(BlockId)))
					{
						TreeGPUBlockLastRequestedFeedbackEpoch[BlockId] = TreeGPUBlockFeedbackEpoch;
					}
				}
				if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")) &&
					LastLoggedTreeGPUBlockRequestCount != static_cast<uint32>(PendingTreeGPUBlockRequests.Num()))
				{
					uint64 RequestedNodeCount = 0;
					if (TreeV3Manifest.IsValid())
					{
						for (const uint32 BlockId : PendingTreeGPUBlockRequests)
						{
							if (TreeV3Manifest->Blocks.IsValidIndex(static_cast<int32>(BlockId)))
							{
								RequestedNodeCount += TreeV3Manifest->Blocks[BlockId].NodeCount;
							}
						}
					}
					LastLoggedTreeGPUBlockRequestCount = static_cast<uint32>(PendingTreeGPUBlockRequests.Num());
					UE_LOG(LogTemp, Display,
						TEXT("NanoGS Tree GPU requested %d unique v3 blocks, %.1f MiB compact nodes"),
						PendingTreeGPUBlockRequests.Num(),
						double(RequestedNodeCount * sizeof(NanoGS::Tree::FGPUNodeRecord)) / (1024.0 * 1024.0));
				}
			}
			bTreeGPUBlockRequestReadbackPending = false;
			StartTreeV3BlockLoads_RenderThread(PendingTreeGPUBlockRequests);
		}
		TryStartTreeGPUPagePrefetch_RenderThread();

		if (bTreeGPUFrontierStatusReadbackPending &&
			(!TreeGPUFrontierStatusReadback.IsValid() || !TreeGPUFrontierStatusReadback->IsReady() ||
			 bTreeGPUPageRequestReadbackPending || bTreeGPUBlockRequestReadbackPending))
		{
			++TreeGPUStatusWaitFrames;
			return true;
		}
		if (bTreeGPUFrontierStatusReadbackPending && TreeGPUFrontierStatusReadback.IsValid())
		{
			const uint32* Status = static_cast<const uint32*>(
				TreeGPUFrontierStatusReadback->Lock(4u * sizeof(uint32)));
			if (Status != nullptr)
			{
				++TreeGPUStatusSampleCount;
				const uint32 FrontierCount = FMath::Min(Status[0], TreeGPUFrontierCapacity);
				const bool bOverflow = Status[1] != 0u;
				const bool bMissingPages = Status[2] != 0u;
				const bool bNeedsAnotherPass = Status[3] != 0u;
				TreeGPUFrontierStatusReadback->Unlock();
				const float TargetSplitThresholdPixels = FMath::Max(SplitThresholdPixels, 0.0f);
				const float PriorityStartPixels = FMath::Max(
					TargetSplitThresholdPixels,
					CVarNanoGSTreeGPUFrontierPriorityStartPixels.GetValueOnRenderThread());
				const float PriorityDecay = FMath::Clamp(
					CVarNanoGSTreeGPUFrontierPriorityDecay.GetValueOnRenderThread(), 0.1f, 0.95f);
				const float CurrentPriorityThresholdPixels = FMath::Max(
					TargetSplitThresholdPixels,
					TreeGPUFrontierPriorityThresholdPixels > 0.0f
						? TreeGPUFrontierPriorityThresholdPixels
						: PriorityStartPixels);
				const bool bStageStable = !bOverflow && !bMissingPages && !bNeedsAnotherPass;
				const uint32 NearBudgetSlack = FMath::Max(1024u, TreeGPUFrontierCapacity / 1000u);
				const bool bNearBudget = FrontierCount + NearBudgetSlack >= TreeGPUFrontierCapacity;
				const bool bAtTargetThreshold =
					CurrentPriorityThresholdPixels <= TargetSplitThresholdPixels + KINDA_SMALL_NUMBER;
				const bool bAdvancePriorityStage = bStageStable && !bAtTargetThreshold && !bNearBudget;
				const bool bRetryOverflowStage = bOverflow && !bMissingPages;
				if (bAdvancePriorityStage)
				{
					TreeGPUFrontierLastStableThresholdPixels = CurrentPriorityThresholdPixels;
					const float Occupancy = TreeGPUFrontierCapacity > 0
						? static_cast<float>(FrontierCount) / static_cast<float>(TreeGPUFrontierCapacity)
						: 0.0f;
					float NextThresholdPixels = 0.0f;
					if (TreeGPUFrontierOverflowThresholdPixels > 0.0f &&
						TreeGPUFrontierOverflowThresholdPixels < CurrentPriorityThresholdPixels)
					{
						NextThresholdPixels = 0.5f * (
							CurrentPriorityThresholdPixels + TreeGPUFrontierOverflowThresholdPixels);
					}
					else
					{
						const float AdaptiveDecay = FMath::Clamp(Occupancy / 0.999f, PriorityDecay, 0.98f);
						NextThresholdPixels = CurrentPriorityThresholdPixels * AdaptiveDecay;
					}
					TreeGPUFrontierPriorityThresholdPixels = FMath::Max(
						TargetSplitThresholdPixels, NextThresholdPixels);
					++TreeGPUFrontierPriorityPass;
					++TreeGPUPriorityRestartCount;
					bRestartForPriorityStage = true;
					if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
					{
						UE_LOG(LogTemp, Display,
							TEXT("NanoGS Tree GPU priority stage advanced: pass=%u threshold=%.3f next=%.3f target=%.3f frontier=%u"),
							TreeGPUFrontierPriorityPass, CurrentPriorityThresholdPixels,
							TreeGPUFrontierPriorityThresholdPixels, TargetSplitThresholdPixels, FrontierCount);
					}
				}
				else if (bRetryOverflowStage)
				{
					TreeGPUFrontierOverflowThresholdPixels = CurrentPriorityThresholdPixels;
					const float RetryThresholdPixels = TreeGPUFrontierLastStableThresholdPixels > CurrentPriorityThresholdPixels
						? 0.5f * (TreeGPUFrontierLastStableThresholdPixels + CurrentPriorityThresholdPixels)
						: CurrentPriorityThresholdPixels / PriorityDecay;
					TreeGPUFrontierPriorityThresholdPixels = FMath::Min(
						PriorityStartPixels, FMath::Max(TargetSplitThresholdPixels, RetryThresholdPixels));
					++TreeGPUFrontierPriorityPass;
					++TreeGPUPriorityRestartCount;
					bRestartForPriorityStage = true;
					if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
					{
						UE_LOG(LogTemp, Display,
							TEXT("NanoGS Tree GPU priority overflow retry: pass=%u threshold=%.3f retry=%.3f stable-high=%.3f frontier=%u"),
							TreeGPUFrontierPriorityPass, CurrentPriorityThresholdPixels,
							TreeGPUFrontierPriorityThresholdPixels,
							TreeGPUFrontierLastStableThresholdPixels, FrontierCount);
					}
				}
				const uint32 FrontierFlags = (bOverflow ? 1u : 0u) |
					(bMissingPages ? 2u : 0u) | (bNeedsAnotherPass ? 4u : 0u);
				const bool bFrontierCountLogStep = LastLoggedTreeGPUFrontierCount == MAX_uint32 ||
					FMath::Abs(static_cast<int64>(FrontierCount) -
						static_cast<int64>(LastLoggedTreeGPUFrontierCount)) >= 250000 ||
					FrontierCount == TreeGPUFrontierCapacity;
				if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")) &&
					(bFrontierCountLogStep || FrontierFlags != LastLoggedTreeGPUFrontierFlags))
				{
					UE_LOG(LogTemp, Display,
						TEXT("NanoGS Tree GPU frontier status: count=%u overflow=%d missing-pages=%d needs-pass=%d"),
						FrontierCount, bOverflow ? 1 : 0, bMissingPages ? 1 : 0,
						bNeedsAnotherPass ? 1 : 0);
					LastLoggedTreeGPUFrontierCount = FrontierCount;
					LastLoggedTreeGPUFrontierFlags = FrontierFlags;
				}
				bTreeGPUFrontierRefinementPending =
					bMissingPages || bNeedsAnotherPass || bRestartForPriorityStage;
				bool bAllFrontierPagesProtected = !PendingTreeGPUPageRequests.IsEmpty();
				for (const uint32 PageId : PendingTreeGPUPageRequests)
				{
					FPagePhysicalRange Range;
					if (!PublishedPageIds.Contains(PageId) ||
						!ResidentPool.FindPhysicalRange(PageId, Range) ||
						Range.State != EPageSlotState::Resident)
					{
						bAllFrontierPagesProtected = false;
						break;
					}
				}
				const bool bPublishRequested =
					CVarNanoGSTreeGPUFrontierPublish.GetValueOnRenderThread() != 0 ||
					FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreeGPUFrontierPublish"));
				if (bPublishRequested && FrontierCount > 0 && bStageStable &&
					!bRestartForPriorityStage && (bAtTargetThreshold || bNearBudget) &&
					bAllFrontierPagesProtected)
				{
					const uint64 PublishedBytes = static_cast<uint64>(FrontierCount) * sizeof(uint32);
					RHICmdList.Transition(FRHITransitionInfo(
						TreeGPUPhysicalIndexBuffers[TreeGPUFrontierStatusSlot],
						ERHIAccess::SRVMask, ERHIAccess::CopySrc));
					RHICmdList.Transition(FRHITransitionInfo(
						TreeGPUPublishedPhysicalIndexBuffer,
						ERHIAccess::SRVMask, ERHIAccess::CopyDest));
					RHICmdList.CopyBufferRegion(
						TreeGPUPublishedPhysicalIndexBuffer, 0,
						TreeGPUPhysicalIndexBuffers[TreeGPUFrontierStatusSlot], 0,
						PublishedBytes);
					RHICmdList.Transition(FRHITransitionInfo(
						TreeGPUPhysicalIndexBuffers[TreeGPUFrontierStatusSlot],
						ERHIAccess::CopySrc, ERHIAccess::SRVMask));
					RHICmdList.Transition(FRHITransitionInfo(
						TreeGPUPublishedPhysicalIndexBuffer,
						ERHIAccess::CopyDest, ERHIAccess::SRVMask));
					TreeGPUPublishedPhysicalIndexSlot = TreeGPUFrontierStatusSlot;
					TreeGPUPublishedPageIds.Reset(PendingTreeGPUPageRequests.Num());
					for (const uint32 PageId : PendingTreeGPUPageRequests)
					{
						TreeGPUPublishedPageIds.Add(PageId);
					}
					SortUniquePageIds(TreeGPUPublishedPageIds);
					TreeGPUActiveSplatCount.store(FrontierCount, std::memory_order_release);
					bTreeGPUFrontierPublished.store(true, std::memory_order_release);
					const uint32 PublishedResidentGeneration =
						ResidentGeneration.fetch_add(1, std::memory_order_acq_rel) + 1u;
					bTreeGPUFrontierRefinementPending = false;
					const double RefinementMilliseconds = TreeGPURefinementStartSeconds > 0.0
						? (FPlatformTime::Seconds() - TreeGPURefinementStartSeconds) * 1000.0
						: 0.0;
					UE_LOG(LogTemp, Log,
						TEXT("NanoGS Tree GPU frontier published atomically: %u splats, build-slot=%u, budget-limited=%d refinement-generation=%llu resident-generation=%u elapsed=%.3f ms dispatches=%u status-samples=%u status-wait-frames=%u page-wait-frames=%u block-wait-frames=%u priority-restarts=%u"),
						FrontierCount, TreeGPUPublishedPhysicalIndexSlot,
						bAtTargetThreshold ? 0 : 1,
						static_cast<unsigned long long>(TreeGPURefinementGeneration),
						PublishedResidentGeneration,
						RefinementMilliseconds, TreeGPURefinementDispatchCount,
						TreeGPUStatusSampleCount, TreeGPUStatusWaitFrames,
						TreeGPUPageReadbackWaitFrames, TreeGPUBlockReadbackWaitFrames,
						TreeGPUPriorityRestartCount);
				}
			}
			bTreeGPUFrontierStatusReadbackPending = false;
		}


		if (bResetToRoot)
		{
			// A completed publication leaves refinement idle. A later camera demand
			// must reactivate the scratch state before the idle fast-return below;
			// otherwise the first published GPU cut would never respond to movement.
			bTreeGPUFrontierRefinementPending = true;
		}
		if (!bTreeGPUFrontierRefinementPending)
		{
			return true;
		}
		if (bResetToRoot)
		{
			++TreeGPURefinementGeneration;
			TreeGPURefinementStartSeconds = FPlatformTime::Seconds();
			TreeGPURefinementDispatchCount = 0;
			TreeGPUStatusSampleCount = 0;
			TreeGPUStatusWaitFrames = 0;
			TreeGPUPageReadbackWaitFrames = 0;
			TreeGPUBlockReadbackWaitFrames = 0;
			TreeGPUPriorityRestartCount = 0;
			TreeGPUFrontierPriorityPass = 0;
			TreeGPUFrontierPriorityThresholdPixels = FMath::Max(
				FMath::Max(SplitThresholdPixels, 0.0f),
				CVarNanoGSTreeGPUFrontierPriorityStartPixels.GetValueOnRenderThread());
			TreeGPUFrontierLastStableThresholdPixels = 0.0f;
			TreeGPUFrontierOverflowThresholdPixels = 0.0f;
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				UE_LOG(LogTemp, Display, TEXT("NanoGS Tree GPU priority reset to root"));
			}
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")) &&
				bTreeGPUFrontierPublished.load(std::memory_order_acquire))
			{
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree GPU refinement generation=%llu keeps the previous published frontier visible"),
					static_cast<unsigned long long>(TreeGPURefinementGeneration));
			}
		}
		if (bResetToRoot || bRestartForPriorityStage)
		{
			uint32* RootDest = static_cast<uint32*>(RHICmdList.LockBuffer(
				TreeGPUFrontierBuffers[0], 0, sizeof(uint32), RLM_WriteOnly));
			*RootDest = TreeGPURootNode;
			RHICmdList.UnlockBuffer(TreeGPUFrontierBuffers[0]);
			const uint32 InitialCounts[4] = {1u, 0u, 0u, 1u};
			void* CountDest = RHICmdList.LockBuffer(
				TreeGPUFrontierCountBuffers[0], 0, sizeof(InitialCounts), RLM_WriteOnly);
			FMemory::Memcpy(CountDest, InitialCounts, sizeof(InitialCounts));
			RHICmdList.UnlockBuffer(TreeGPUFrontierCountBuffers[0]);
			TreeGPUFrontierReadSlot = 0;
		}

		const uint8 ReadSlot = TreeGPUFrontierReadSlot;
		const uint8 WriteSlot = 1u - ReadSlot;
		if (!TreeGPUFrontierSRVs[ReadSlot].IsValid() ||
			!TreeGPUFrontierCountSRVs[ReadSlot].IsValid() ||
			!TreeGPUFrontierUAVs[WriteSlot].IsValid() ||
			!TreeGPUFrontierCountUAVs[WriteSlot].IsValid() ||
			!TreeGPUPhysicalIndexBuffers[WriteSlot].IsValid() ||
			!TreeGPUPhysicalIndexUAVs[WriteSlot].IsValid())
		{
			return false;
		}

		TShaderMapRef<FNanoGSTreeFrontierPrepareCS> PrepareShader(
			GetGlobalShaderMap(GMaxRHIFeatureLevel));
		TShaderMapRef<FNanoGSTreeFrontierSplitCS> SplitShader(
			GetGlobalShaderMap(GMaxRHIFeatureLevel));
		if (!PrepareShader.IsValid() || !SplitShader.IsValid())
		{
			return false;
		}

		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUFrontierBuffers[WriteSlot], ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUFrontierCountBuffers[WriteSlot], ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUPhysicalIndexBuffers[WriteSlot], ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUFrontierIndirectArgsBuffer, ERHIAccess::IndirectArgs, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUBlockRequestBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUBlockRequestBitsBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		RHICmdList.ClearUAVUint(TreeGPUBlockRequestBitsUAV, FUintVector4(0u, 0u, 0u, 0u));
		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUPageRequestBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUPageRequestBitsBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
		RHICmdList.ClearUAVUint(TreeGPUPageRequestBitsUAV, FUintVector4(0u, 0u, 0u, 0u));

		FNanoGSTreeFrontierPrepareCS::FParameters PrepareParameters;
		PrepareParameters.InputCountAndOverflow = TreeGPUFrontierCountSRVs[ReadSlot];
		PrepareParameters.OutputCountAndOverflow = TreeGPUFrontierCountUAVs[WriteSlot];
		PrepareParameters.IndirectDispatchArgs = TreeGPUFrontierIndirectArgsUAV;
		PrepareParameters.RequestedBlocks = TreeGPUBlockRequestUAV;
		PrepareParameters.RequestedPages = TreeGPUPageRequestUAV;
		PrepareParameters.OutputCapacity = TreeGPUFrontierCapacity;
		SetComputePipelineState(RHICmdList, PrepareShader.GetComputeShader());
		SetShaderParameters(
			RHICmdList, PrepareShader, PrepareShader.GetComputeShader(), PrepareParameters);
		RHICmdList.DispatchComputeShader(1, 1, 1);
		UnsetShaderUAVs(RHICmdList, PrepareShader, PrepareShader.GetComputeShader());

		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUFrontierIndirectArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));

		FNanoGSTreeFrontierSplitCS::FParameters SplitParameters;
		SplitParameters.TreeNodes = TreeGPUNodeBufferSRV;
		SplitParameters.BlockNodes = TreeGPUBlockNodeBufferSRV;
		SplitParameters.BlockResidency = TreeGPUBlockResidencyBufferSRV;
		SplitParameters.PageResidency = TreeGPUPageResidencyBufferSRV;
		SplitParameters.InputFrontier = TreeGPUFrontierSRVs[ReadSlot];
		SplitParameters.InputPhysicalIndices = TreeGPUPhysicalIndexSRVs[ReadSlot];
		SplitParameters.InputCountAndOverflow = TreeGPUFrontierCountSRVs[ReadSlot];
		SplitParameters.OutputFrontier = TreeGPUFrontierUAVs[WriteSlot];
		SplitParameters.OutputPhysicalIndices = TreeGPUPhysicalIndexUAVs[WriteSlot];
		SplitParameters.OutputCountAndOverflow = TreeGPUFrontierCountUAVs[WriteSlot];
		SplitParameters.RequestedBlocks = TreeGPUBlockRequestUAV;
		SplitParameters.RequestedBlockBits = TreeGPUBlockRequestBitsUAV;
		SplitParameters.RequestedPages = TreeGPUPageRequestUAV;
		SplitParameters.RequestedPageBits = TreeGPUPageRequestBitsUAV;
		SplitParameters.ViewOriginLocal = ViewOriginLocal;
		SplitParameters.ProjectionScale = FMath::Max(ProjectionScale, 0.0f);
		const float TargetSplitThresholdPixels = FMath::Max(SplitThresholdPixels, 0.0f);
		const float PriorityStartPixels = FMath::Max(
			TargetSplitThresholdPixels,
			CVarNanoGSTreeGPUFrontierPriorityStartPixels.GetValueOnRenderThread());
		const float PriorityDecay = FMath::Clamp(
			CVarNanoGSTreeGPUFrontierPriorityDecay.GetValueOnRenderThread(), 0.1f, 0.95f);
		const float PriorityThresholdPixels = TreeGPUFrontierPriorityThresholdPixels > 0.0f
			? TreeGPUFrontierPriorityThresholdPixels
			: PriorityStartPixels;
		const float EffectiveSplitThresholdPixels = FMath::Max(
			TargetSplitThresholdPixels, PriorityThresholdPixels);
		SplitParameters.SplitThresholdPixels = EffectiveSplitThresholdPixels;
		SplitParameters.AllowThresholdTerminalization = 1u;
		SplitParameters.OutputCapacity = TreeGPUFrontierCapacity;
		SplitParameters.BlockRequestCapacity = TreeGPUBlockRequestCapacity;
		SplitParameters.BlockAddressCapacity = TreeGPUBlockRequestCapacity;
		SplitParameters.PageRequestCapacity = TreeGPUPageRequestCapacity;
		SplitParameters.PageAddressCapacity = TreeGPUPageAddressCapacity;
		SetComputePipelineState(RHICmdList, SplitShader.GetComputeShader());
		SetShaderParameters(RHICmdList, SplitShader, SplitShader.GetComputeShader(), SplitParameters);
		RHICmdList.DispatchIndirectComputeShader(TreeGPUFrontierIndirectArgsBuffer, 0);
		++TreeGPURefinementDispatchCount;
		UnsetShaderUAVs(RHICmdList, SplitShader, SplitShader.GetComputeShader());

		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUFrontierBuffers[WriteSlot], ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUFrontierCountBuffers[WriteSlot], ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(
			TreeGPUPhysicalIndexBuffers[WriteSlot], ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
		if (!bTreeGPUFrontierStatusReadbackPending && TreeGPUFrontierStatusReadback.IsValid())
		{
			RHICmdList.Transition(FRHITransitionInfo(
				TreeGPUFrontierCountBuffers[WriteSlot], ERHIAccess::SRVMask, ERHIAccess::CopySrc));
			TreeGPUFrontierStatusReadback->EnqueueCopy(
				RHICmdList, TreeGPUFrontierCountBuffers[WriteSlot], 4u * sizeof(uint32));
			RHICmdList.Transition(FRHITransitionInfo(
				TreeGPUFrontierCountBuffers[WriteSlot], ERHIAccess::CopySrc, ERHIAccess::SRVMask));
			TreeGPUFrontierStatusSlot = WriteSlot;
			bTreeGPUFrontierStatusReadbackPending = true;
		}
		if (!bTreeGPUBlockRequestReadbackPending && TreeGPUBlockRequestReadback.IsValid())
		{
			RHICmdList.Transition(FRHITransitionInfo(
				TreeGPUBlockRequestBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
			TreeGPUBlockRequestReadback->EnqueueCopy(
				RHICmdList, TreeGPUBlockRequestBuffer,
				(TreeGPUBlockRequestCapacity + 1u) * sizeof(uint32));
			bTreeGPUBlockRequestReadbackPending = true;
		}
		if (!bTreeGPUPageRequestReadbackPending && TreeGPUPageRequestReadback.IsValid())
		{
			RHICmdList.Transition(FRHITransitionInfo(
				TreeGPUPageRequestBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
			TreeGPUPageRequestReadback->EnqueueCopy(
				RHICmdList, TreeGPUPageRequestBuffer,
				(TreeGPUPageRequestCapacity + 1u) * sizeof(uint32));
			bTreeGPUPageRequestReadbackPending = true;
		}
		TreeGPUFrontierReadSlot = WriteSlot;
		bTreeGPUFrontierRefinementPending = true;
		return true;
	}

	bool FPageRenderData::BuildActivePhysicalIndicesGPU_RenderThread(
		const uint8 PublishIndexBufferSlot,
		const TConstArrayView<FUintVector4> PageDescriptors)
	{
		check(IsInRenderingThread());
		if (PageDescriptors.IsEmpty())
		{
			return true;
		}
		if (PublishIndexBufferSlot >= ActiveIndexBufferCount ||
			!ActivePageDescriptorBuffer.IsValid() ||
			!ActivePageDescriptorBufferSRV.IsValid() ||
			!ActivePhysicalIndexBuffers[PublishIndexBufferSlot].IsValid() ||
			!ActivePhysicalIndexBufferUAVs[PublishIndexBufferSlot].IsValid() ||
			PageDescriptors.Num() > static_cast<int32>(ActivePageDescriptorCapacity) ||
			PageDescriptors.Num() > GRHIMaxDispatchThreadGroupsPerDimension.X)
		{
			return false;
		}

		TShaderMapRef<FNanoGSPagedBuildActiveIndexCS> ComputeShader(
			GetGlobalShaderMap(GMaxRHIFeatureLevel));
		if (!ComputeShader.IsValid())
		{
			return false;
		}

		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		const uint32 DescriptorBytes =
			static_cast<uint32>(PageDescriptors.Num()) * sizeof(FUintVector4);
		void* DescriptorDest = RHICmdList.LockBuffer(
			ActivePageDescriptorBuffer, 0, DescriptorBytes, RLM_WriteOnly);
		if (DescriptorDest == nullptr)
		{
			return false;
		}
		FMemory::Memcpy(DescriptorDest, PageDescriptors.GetData(), DescriptorBytes);
		RHICmdList.UnlockBuffer(ActivePageDescriptorBuffer);

		RHICmdList.Transition(FRHITransitionInfo(
			ActivePageDescriptorBuffer, ERHIAccess::Unknown, ERHIAccess::SRVCompute));
		RHICmdList.Transition(FRHITransitionInfo(
			ActivePhysicalIndexBuffers[PublishIndexBufferSlot],
			ERHIAccess::SRVMask,
			ERHIAccess::UAVCompute));

		FNanoGSPagedBuildActiveIndexCS::FParameters Parameters;
		Parameters.PageDescriptors = ActivePageDescriptorBufferSRV;
		Parameters.ActivePhysicalIndices = ActivePhysicalIndexBufferUAVs[PublishIndexBufferSlot];
		Parameters.PageCount = static_cast<uint32>(PageDescriptors.Num());
		SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
		SetShaderParameters(
			RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
		RHICmdList.DispatchComputeShader(PageDescriptors.Num(), 1, 1);
		UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
		RHICmdList.Transition(FRHITransitionInfo(
			ActivePhysicalIndexBuffers[PublishIndexBufferSlot],
			ERHIAccess::UAVCompute,
			ERHIAccess::SRVMask));
		return true;
	}

	void FPageRenderData::TryPublishBoundedFrontier_RenderThread(const uint32 UploadEpochValue)
	{
		check(IsInRenderingThread());
		const double PublicationStartSeconds = FPlatformTime::Seconds();
		if (UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
		{
			return;
		}
		ReleaseCompletedRetirements_RenderThread();

		TArray<FPagePhysicalRange> ActiveRanges;
		TArray<uint64> NotResident;
		FString Error;
		if (!ResidentPool.CollectActivePhysicalRanges(
			TargetPageIds, {}, ActiveRanges, NotResident, Error))
		{
			if (!Error.IsEmpty())
			{
				HandleBoundedFailure_RenderThread(Error, UploadEpochValue);
			}
			return;
		}

		const bool bCapturePublication = bPublishingCaptureTreeFrontier;
		const uint8 PublishIndexBufferSlot = bCapturePublication
			? static_cast<uint8>(PublishedCaptureActiveIndexBufferSlot == 2u ? 3u : 2u)
			: static_cast<uint8>((PublishedActiveIndexBufferSlot + 1u) % 2u);
		for (const FRetiredFrontier& Retired : RetiredFrontiers)
		{
			if (Retired.ActiveIndexBufferSlot == PublishIndexBufferSlot)
			{
				// All target source pages are complete, but this inactive index
				// buffer is still referenced by an older GPU draw. Keep rendering
				// the current complete frontier and retry after its fence retires.
				if (!bCapturePublication)
				{
					bPublicationPendingOnIndexFence = true;
					PendingPublicationUploadEpoch = UploadEpochValue;
					StreamingState.store(
						static_cast<uint8>(EExactHQStreamingState::Loading),
						std::memory_order_release);
				}
				return;
			}
		}
		if (!ActivePhysicalIndexBuffers[PublishIndexBufferSlot].IsValid() ||
			!ActivePhysicalIndexBufferSRVs[PublishIndexBufferSlot].IsValid())
		{
			HandleBoundedFailure_RenderThread(
				TEXT("inactive active-index publication buffer is unavailable"),
				UploadEpochValue);
			return;
		}
		const bool bPublishedFrontierDrawn = bCapturePublication
			? bCaptureFrontierHasBeenDrawn
			: bCurrentFrontierHasBeenDrawn;
		const bool bPublishedFenceUnavailable = bCapturePublication
			? bCaptureFrontierFenceUnavailable
			: bCurrentFrontierFenceUnavailable;
		const FGPUFenceRHIRef& PublishedLastUseFence = bCapturePublication
			? CaptureFrontierLastUseFence
			: CurrentFrontierLastUseFence;
		if (bPublishedFrontierDrawn &&
			(bPublishedFenceUnavailable || !PublishedLastUseFence.IsValid()))
		{
			FailFenceSafetyClosed_RenderThread(
				bCapturePublication
					? TEXT("the capture frontier was drawn but no usable last-use fence exists for retirement")
					: TEXT("the published frontier was drawn but no usable last-use fence exists for retirement"));
			return;
		}

		const double DescriptorStartSeconds = FPlatformTime::Seconds();
		struct FOrderedPageRange
		{
			const FPageRecord* Page = nullptr;
			FPagePhysicalRange Range;
		};
		TArray<FOrderedPageRange> Ordered;
		Ordered.Reserve(TargetPageIds.Num());
		TMap<uint64, FPagePhysicalRange> PhysicalRanges;
		for (const uint64 PageId : TargetPageIds)
		{
			const FPageRecord* Page = Manifest.FindPage(PageId);
			FPagePhysicalRange Range;
			if (Page == nullptr || !ResidentPool.FindPhysicalRange(PageId, Range) ||
				Range.State != EPageSlotState::Resident || Range.SplatCount != Page->SplatCount)
			{
				HandleBoundedFailure_RenderThread(
					TEXT("resident target changed while building the active frontier"),
					UploadEpochValue);
				return;
			}
			Ordered.Add({Page, Range});
			PhysicalRanges.Add(PageId, Range);
		}
		Ordered.Sort([](const FOrderedPageRange& A, const FOrderedPageRange& B)
		{
			return A.Page->FirstOutputIndex < B.Page->FirstOutputIndex;
		});

		// Active indices are signed-31-bit safe by InitializeManifest. Reuse the
		// otherwise unused high bit as a transient parent tag, avoiding a separate
		// per-splat GPU flag buffer. Page v1 payload bytes remain unchanged.
		constexpr uint32 TreeLODParentIndexBit = 0x80000000u;
		TArray<FUintVector4> PageDescriptors;
		PageDescriptors.Reserve(TargetSplatRefs.IsEmpty() ? Ordered.Num() : TargetSplatRefs.Num());
		uint64 ActiveBase64 = 0;
		const auto AppendDescriptors = [
			this, &PageDescriptors, &ActiveBase64](const uint64 PhysicalStart,
				const uint64 OutputStart, const uint32 Count)
		{
			const uint64 OutputEnd = OutputStart + Count;
			const bool bCrossesLeafBoundary = bTreeLODSource &&
				OutputStart < TreeExactLeafCount && OutputEnd > TreeExactLeafCount;
			const uint32 LeafCount = bCrossesLeafBoundary
				? static_cast<uint32>(TreeExactLeafCount - OutputStart)
				: 0u;
			const auto AddOne = [&PageDescriptors, &ActiveBase64](
				const uint64 Physical, const uint32 SegmentCount, const bool bParent)
			{
				if (SegmentCount == 0)
					return;
				PageDescriptors.Add(FUintVector4(
					static_cast<uint32>(ActiveBase64),
					static_cast<uint32>(Physical),
					SegmentCount,
					bParent ? TreeLODParentIndexBit : 0u));
				ActiveBase64 += SegmentCount;
			};
			if (bCrossesLeafBoundary)
			{
				AddOne(PhysicalStart, LeafCount, false);
				AddOne(PhysicalStart + LeafCount, Count - LeafCount, true);
			}
			else
			{
				AddOne(PhysicalStart, Count,
					bTreeLODSource && OutputStart >= TreeExactLeafCount);
			}
		};
		if (TargetSplatRefs.IsEmpty() && TargetSplatRuns.IsEmpty())
		{
			for (const FOrderedPageRange& Entry : Ordered)
			{
				if (ActiveBase64 > MAX_uint32 || Entry.Range.FirstSplat > MAX_uint32 ||
					Entry.Range.SplatCount > MAX_uint32 ||
					ActiveBase64 > static_cast<uint64>(ActiveIndexCapacity) - Entry.Range.SplatCount)
				{
					HandleBoundedFailure_RenderThread(
						TEXT("target active or physical page range exceeds capacity"), UploadEpochValue);
					return;
				}
				AppendDescriptors(
					Entry.Range.FirstSplat,
					Entry.Page->FirstOutputIndex,
					static_cast<uint32>(Entry.Range.SplatCount));
			}
		}
		else if (!TargetSplatRuns.IsEmpty())
		{
			for (const FExplicitSplatRun& Run : TargetSplatRuns)
			{
				const FPagePhysicalRange* Range = PhysicalRanges.Find(Run.PageId);
				const FPageRecord* Page = Manifest.FindPage(Run.PageId);
				if (Range == nullptr || Page == nullptr || Run.Count == 0 ||
					Run.PageLocal >= Page->SplatCount || Run.Count > Page->SplatCount - Run.PageLocal)
				{
					HandleBoundedFailure_RenderThread(TEXT("precomputed sparse target run is invalid"), UploadEpochValue);
					return;
				}
				const uint64 PhysicalStart = Range->FirstSplat + Run.PageLocal;
				if (ActiveBase64 > static_cast<uint64>(ActiveIndexCapacity) - Run.Count || PhysicalStart > MAX_uint32)
				{
					HandleBoundedFailure_RenderThread(TEXT("sparse target exceeds active-index capacity"), UploadEpochValue);
					return;
				}
				AppendDescriptors(PhysicalStart, Page->FirstOutputIndex + Run.PageLocal, Run.Count);
			}
		}
		else
		{
			int32 RefIndex = 0;
			while (RefIndex < TargetSplatRefs.Num())
			{
				const FExplicitSplatRef& First = TargetSplatRefs[RefIndex];
				const FPagePhysicalRange* Range = PhysicalRanges.Find(First.PageId);
				const FPageRecord* Page = Manifest.FindPage(First.PageId);
				if (Range == nullptr || Page == nullptr || First.PageLocal >= Page->SplatCount)
				{
					HandleBoundedFailure_RenderThread(TEXT("sparse target ref is not resident"), UploadEpochValue);
					return;
				}
				int32 RunEnd = RefIndex + 1;
				while (RunEnd < TargetSplatRefs.Num() &&
					TargetSplatRefs[RunEnd].PageId == First.PageId &&
					TargetSplatRefs[RunEnd].PageLocal == TargetSplatRefs[RunEnd - 1].PageLocal + 1u)
				{
					++RunEnd;
				}
				const uint32 RunCount = static_cast<uint32>(RunEnd - RefIndex);
				const uint64 PhysicalStart = Range->FirstSplat + First.PageLocal;
				if (ActiveBase64 > static_cast<uint64>(ActiveIndexCapacity) - RunCount || PhysicalStart > MAX_uint32)
				{
					HandleBoundedFailure_RenderThread(TEXT("sparse target exceeds active-index capacity"), UploadEpochValue);
					return;
				}
				AppendDescriptors(
					PhysicalStart,
					Page->FirstOutputIndex + First.PageLocal,
					RunCount);
				RefIndex = RunEnd;
			}
		}
		const uint64 ActiveCount64 = ActiveBase64;
		const double DescriptorEndSeconds = FPlatformTime::Seconds();
		if (SceneBudgetIdentity != 0 && SceneBudgetOwnerIdentity != 0)
		{
			const int32 ConfiguredActiveLimit = bCapturePublication
				? CVarNanoGSPagedGlobalCaptureActiveSplats.GetValueOnRenderThread()
				: CVarNanoGSPagedGlobalMainActiveSplats.GetValueOnRenderThread();
			FString BudgetError;
			if (!FSceneBudgetRegistry::Get().TrySetActiveSplats(
				SceneBudgetIdentity, SceneBudgetOwnerIdentity,
				bCapturePublication ? ESceneActiveBudgetLane::Capture : ESceneActiveBudgetLane::Main,
				ActiveCount64, static_cast<uint64>(FMath::Max(0, ConfiguredActiveLimit)), BudgetError))
			{
				if (bCapturePublication)
				{
					bCaptureFrontierReady.store(false, std::memory_order_release);
					const double NowSeconds = FPlatformTime::Seconds();
					if (LastCaptureBudgetWarningSeconds < 0.0 ||
						NowSeconds - LastCaptureBudgetWarningSeconds >= 1.0)
					{
						LastCaptureBudgetWarningSeconds = NowSeconds;
						UE_LOG(LogTemp, Warning, TEXT("NanoGS strict capture waiting for scene active budget: %s"), *BudgetError);
					}
				}
				else
				{
					SuspendPublishedFrontierOutOfCapacity_RenderThread(TargetPageIds, BudgetError);
				}
				return;
			}
		}

		if (bCapturePublication)
		{
			LastCaptureBudgetWarningSeconds = -1.0;
		}

		const double IndexStartSeconds = FPlatformTime::Seconds();
		bool bBuiltActiveIndexOnGPU = false;
		if (CVarNanoGSPagedGPUActiveIndex.GetValueOnRenderThread() != 0)
		{
			bBuiltActiveIndexOnGPU = BuildActivePhysicalIndicesGPU_RenderThread(
				PublishIndexBufferSlot, PageDescriptors);
		}
		if (!bBuiltActiveIndexOnGPU && ActiveCount64 != 0)
		{
			TArray<uint32> ActiveIndices;
			ActiveIndices.Reserve(static_cast<int32>(ActiveCount64));
			for (const FUintVector4& Descriptor : PageDescriptors)
			{
				for (uint32 LocalIndex = 0; LocalIndex < Descriptor.Z; ++LocalIndex)
				{
					ActiveIndices.Add(
						(Descriptor.Y + LocalIndex) | Descriptor.W);
				}
			}
			const uint32 ActiveBytes = static_cast<uint32>(ActiveIndices.Num()) * sizeof(uint32);
			FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
			void* Dest = RHICmdList.LockBuffer(
				ActivePhysicalIndexBuffers[PublishIndexBufferSlot], 0, ActiveBytes, RLM_WriteOnly);
			FMemory::Memcpy(Dest, ActiveIndices.GetData(), ActiveBytes);
			RHICmdList.UnlockBuffer(ActivePhysicalIndexBuffers[PublishIndexBufferSlot]);
		}
		const double IndexEndSeconds = FPlatformTime::Seconds();

		if (bCapturePublication)
		{
			if (bCaptureFrontierHasBeenDrawn && CaptureFrontierLastUseFence.IsValid())
			{
				FRetiredFrontier& Retired = RetiredFrontiers.AddDefaulted_GetRef();
				Retired.PinGeneration = 0;
				Retired.ActiveIndexBufferSlot = PublishedCaptureActiveIndexBufferSlot;
				Retired.LastUseFence = CaptureFrontierLastUseFence;
			}
			const double CommitStartSeconds = FPlatformTime::Seconds();
			PublishedCaptureActiveIndexBufferSlot = PublishIndexBufferSlot;
			PublishedCaptureSplatRefs = TargetSplatRefs;
			PublishedCaptureSplatRuns = TargetSplatRuns;
			PublishedCaptureActiveSplatCount = static_cast<uint32>(ActiveCount64);
			CaptureFrontierLastUseFence.SafeRelease();
			bCaptureFrontierHasBeenDrawn = false;
			bCaptureFrontierFenceUnavailable = false;
			CaptureActiveSplatCount.store(PublishedCaptureActiveSplatCount, std::memory_order_release);
			PublishedCapturePageCount.store(static_cast<uint32>(TargetPageIds.Num()), std::memory_order_release);
			CaptureResidentGeneration.fetch_add(1, std::memory_order_acq_rel);
			bCaptureFrontierReady.store(true, std::memory_order_release);
			const double PublicationEndSeconds = FPlatformTime::Seconds();
			if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
			{
				UE_LOG(LogTemp, Display,
					TEXT("NanoGS Tree capture publish: total=%.3f ms descriptors=%.3f ms index-submit=%.3f ms commit=%.3f ms splats=%u refs=%d runs=%d pages=%d slot=%u generation=%u"),
					(PublicationEndSeconds - PublicationStartSeconds) * 1000.0,
					(DescriptorEndSeconds - DescriptorStartSeconds) * 1000.0,
					(IndexEndSeconds - IndexStartSeconds) * 1000.0,
					(PublicationEndSeconds - CommitStartSeconds) * 1000.0,
					PublishedCaptureActiveSplatCount, TargetSplatRefs.Num(), PageDescriptors.Num(),
					TargetPageIds.Num(), PublishedCaptureActiveIndexBufferSlot,
					CaptureResidentGeneration.load(std::memory_order_acquire));
			}
			return;
		}

		const uint64 RetiringPinGeneration = CurrentTransitionPinGeneration;
		if (bCurrentFrontierHasBeenDrawn && CurrentFrontierLastUseFence.IsValid())
		{
			FRetiredFrontier& Retired = RetiredFrontiers.AddDefaulted_GetRef();
			Retired.PinGeneration = RetiringPinGeneration;
			Retired.ActiveIndexBufferSlot = PublishedActiveIndexBufferSlot;
			Retired.LastUseFence = CurrentFrontierLastUseFence;
		}
		else if (RetiringPinGeneration != 0)
		{
			FString ReleaseError;
			if (!ResidentPool.ReleaseCaptureGeneration(RetiringPinGeneration, ReleaseError))
			{
				HandleBoundedFailure_RenderThread(ReleaseError, UploadEpochValue);
				return;
			}
		}

		const double CommitStartSeconds = FPlatformTime::Seconds();
		const bool bFinalizeTarget = !bPublishingIntermediateTreeFrontier;
		// Publication is last: every target source stream and the complete active
		// index were written on this render command list before readers see count/gen.
		PublishedPageIds = TargetPageIds;
		PublishedSplatRefs = TargetSplatRefs;
		PublishedSplatRuns = TargetSplatRuns;
		CurrentTransitionPinGeneration = 0;
		CurrentFrontierLastUseFence.SafeRelease();
		if (bFinalizeTarget)
		{
			InFlightLoadOperations.Reset();
		}
		PublishedActiveIndexBufferSlot = PublishIndexBufferSlot;
		bPublicationPendingOnIndexFence = false;
		PendingPublicationUploadEpoch = 0;
		const uint32 ActiveCount = static_cast<uint32>(ActiveCount64);
		PublishedActiveSplatCount = ActiveCount;
		bCurrentFrontierHasBeenDrawn = false;
		bCurrentFrontierFenceUnavailable = false;
		ActiveSplatCount.store(ActiveCount, std::memory_order_release);
		if (bFinalizeTarget)
		{
			LoadedPageCount.store(static_cast<uint32>(PublishedPageIds.Num()), std::memory_order_release);
		}
		PublishedPageCount.store(static_cast<uint32>(PublishedPageIds.Num()), std::memory_order_release);
		ResidentGeneration.fetch_add(1, std::memory_order_acq_rel);
		bUploadFailed.store(false, std::memory_order_release);
		bUploadComplete.store(bFinalizeTarget, std::memory_order_release);
		bPublishedFrontierCaptureSafeDuringTransition.store(false, std::memory_order_release);
		StreamingState.store(
			static_cast<uint8>(bFinalizeTarget
				? EExactHQStreamingState::Ready
				: EExactHQStreamingState::Loading),
			std::memory_order_release);
		const double PublicationEndSeconds = FPlatformTime::Seconds();
		if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
		{
			UE_LOG(LogTemp, Display,
				TEXT("NanoGS Tree render-thread publish: total=%.3f ms descriptors=%.3f ms index-submit=%.3f ms commit=%.3f ms refs=%d runs=%d pages=%d"),
				(PublicationEndSeconds - PublicationStartSeconds) * 1000.0,
				(DescriptorEndSeconds - DescriptorStartSeconds) * 1000.0,
				(IndexEndSeconds - IndexStartSeconds) * 1000.0,
				(PublicationEndSeconds - CommitStartSeconds) * 1000.0,
				TargetSplatRefs.Num(), PageDescriptors.Num(), TargetPageIds.Num());
		}
		if (bFinalizeTarget)
		{
			UE_LOG(LogTemp, Log,
				TEXT("NanoGS bounded Exact-HQ frontier ready: %u splats / %d pages in %u fixed slots (generation=%u) active-index=%s"),
				ActiveCount, PublishedPageIds.Num(), PhysicalSlotCount,
				ResidentGeneration.load(std::memory_order_acquire),
				bBuiltActiveIndexOnGPU ? TEXT("gpu") : TEXT("cpu"));
		}
	}

	void FPageRenderData::StartBoundedLoads(
		TArray<FPageLoadOperation> LoadOperations,
		const uint32 UploadEpochValue)
	{
		if (LoadOperations.IsEmpty())
		{
			return;
		}
		const TSharedRef<FPageRenderData, ESPMode::ThreadSafe> Self = AsShared();
		const bool bVerifyStreamCRCs =
			CVarNanoGSPagedVerifyPayloadCRC.GetValueOnAnyThread() != 0;
		const int32 WorkerCount = FMath::Clamp(
			CVarNanoGSPagedIOWorkers.GetValueOnAnyThread(),
			1,
			FMath::Min(8, LoadOperations.Num()));
		int32 ConfiguredGlobalIOPages = FMath::Max(
			0, CVarNanoGSPagedGlobalIOPages.GetValueOnAnyThread());
		FParse::Value(FCommandLine::Get(), TEXT("NanoGSPagedGlobalIOPages="), ConfiguredGlobalIOPages);
		const uint64 GlobalIOPageLimit = static_cast<uint64>(FMath::Max(0, ConfiguredGlobalIOPages));
		TArray<TArray<FPageLoadOperation>> WorkerLoadOperations;
		WorkerLoadOperations.SetNum(WorkerCount);
		for (int32 WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
		{
			WorkerLoadOperations[WorkerIndex].Reserve(
				(LoadOperations.Num() + WorkerCount - 1 - WorkerIndex) / WorkerCount);
		}
		for (int32 OperationIndex = 0; OperationIndex < LoadOperations.Num(); ++OperationIndex)
		{
			WorkerLoadOperations[OperationIndex % WorkerCount].Add(
				MoveTemp(LoadOperations[OperationIndex]));
		}

		UE_LOG(LogTemp, Log,
			TEXT("NanoGS bounded Exact-HQ loading %d pages with %d bounded I/O workers (epoch=%u)"),
			LoadOperations.Num(), WorkerCount, UploadEpochValue);
		for (TArray<FPageLoadOperation>& WorkerLoads : WorkerLoadOperations)
		{
			Async(EAsyncExecution::ThreadPool,
			[Self, WorkerLoads = MoveTemp(WorkerLoads), UploadEpochValue, bVerifyStreamCRCs, GlobalIOPageLimit]() mutable
		{
			FPagePayloadReadOptions ReadOptions;
			ReadOptions.bVerifyStreamCRCs = bVerifyStreamCRCs;
			for (const FPageLoadOperation& Operation : WorkerLoads)
			{
				if (Self->bStopRequested.load(std::memory_order_acquire) ||
					Self->UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
				{
					return;
				}

				bool bGlobalIOPermit = false;
				while (Self->SceneBudgetIdentity != 0 && Self->SceneBudgetOwnerIdentity != 0 &&
					!(bGlobalIOPermit = FSceneBudgetRegistry::Get().TryAcquireInFlightPages(
						Self->SceneBudgetIdentity, Self->SceneBudgetOwnerIdentity, 1, GlobalIOPageLimit)))
				{
					if (Self->bStopRequested.load(std::memory_order_acquire) ||
						Self->UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
					{
						return;
					}
					FPlatformProcess::SleepNoStats(0.001f);
				}
				ON_SCOPE_EXIT
				{
					if (bGlobalIOPermit)
					{
						FSceneBudgetRegistry::Get().ReleaseInFlightPages(
							Self->SceneBudgetIdentity, Self->SceneBudgetOwnerIdentity, 1);
					}
				};

				const FPageRecord* Page = Self->Manifest.FindPage(Operation.Destination.PageId);
				if (Page == nullptr || Page->SplatCount != Operation.Destination.SplatCount ||
					Operation.Destination.FirstSplat + Operation.Destination.SplatCount > Self->SourceCapacity)
				{
					const FString Error = TEXT("tokenized bounded load does not match the Page v1 manifest/pool");
					ENQUEUE_RENDER_COMMAND(NanoGSBoundedLoadMetadataFailure)(
						[Self, Error, UploadEpochValue](FRHICommandListImmediate&)
						{
							Self->HandleBoundedFailure_RenderThread(Error, UploadEpochValue);
						});
					return;
				}

				FOwnedPagePayload Payload;
				FString ReadError;
				const double ReadStartSeconds = FPlatformTime::Seconds();
				if (!FPageFileReader::ReadPagePayload(
					Self->Manifest, Page->PageId, Payload, ReadError, ReadOptions))
				{
					ENQUEUE_RENDER_COMMAND(NanoGSBoundedLoadReadFailure)(
						[Self, ReadError, UploadEpochValue](FRHICommandListImmediate&)
						{
							Self->HandleBoundedFailure_RenderThread(ReadError, UploadEpochValue);
						});
					return;
				}

				const uint32 Count = static_cast<uint32>(Page->SplatCount);
				TSharedRef<FPageUpload, ESPMode::ThreadSafe> Upload =
					MakeShared<FPageUpload, ESPMode::ThreadSafe>();
				Upload->PageId = Page->PageId;
				Upload->PhysicalBase = static_cast<uint32>(Operation.Destination.FirstSplat);
				Upload->SplatCount = Count;
				Upload->LoadOperation = Operation;
				Upload->ReadMilliseconds = (FPlatformTime::Seconds() - ReadStartSeconds) * 1000.0;
				const double DecodeStartSeconds = FPlatformTime::Seconds();

				const TConstArrayView64<float> Rotations = Payload.GetRotations();
				const TConstArrayView64<float> Scales = Payload.GetScales();
				if (Rotations.Num() != static_cast<int64>(Count) * 4 ||
					Scales.Num() != static_cast<int64>(Count) * 3)
				{
					const FString Error = TEXT("bounded page rotation/scale length mismatch");
					ENQUEUE_RENDER_COMMAND(NanoGSBoundedLoadDecodeFailure)(
						[Self, Error, UploadEpochValue](FRHICommandListImmediate&)
						{
							Self->HandleBoundedFailure_RenderThread(Error, UploadEpochValue);
						});
					return;
				}

				Upload->Other.SetNumUninitialized(static_cast<int64>(Count) * 28);
				for (uint32 LocalIndex = 0; LocalIndex < Count; ++LocalIndex)
				{
					uint8* Dest = Upload->Other.GetData() + static_cast<uint64>(LocalIndex) * 28ull;
					FMemory::Memcpy(Dest, Rotations.GetData() + static_cast<uint64>(LocalIndex) * 4ull, 16);
					FMemory::Memcpy(Dest + 16, Scales.GetData() + static_cast<uint64>(LocalIndex) * 3ull, 12);
				}
				if (Payload.GetRawStream(EStreamSemantic::Position).Num() != static_cast<int64>(Count) * 12 ||
					Upload->Other.Num() != static_cast<int64>(Count) * 28 ||
					Payload.GetRawStream(EStreamSemantic::ColorOpacity).Num() != static_cast<int64>(Count) * 4 ||
					Payload.GetRawStream(EStreamSemantic::SphericalHarmonics).Num() != static_cast<int64>(Count) * 96)
				{
					const FString Error = TEXT("bounded decoded page stream byte length mismatch");
					ENQUEUE_RENDER_COMMAND(NanoGSBoundedLoadByteFailure)(
						[Self, Error, UploadEpochValue](FRHICommandListImmediate&)
						{
							Self->HandleBoundedFailure_RenderThread(Error, UploadEpochValue);
						});
					return;
				}
				Upload->Payload = MoveTemp(Payload);
				Upload->DecodeMilliseconds = (FPlatformTime::Seconds() - DecodeStartSeconds) * 1000.0;
				Upload->EnqueueSeconds = FPlatformTime::Seconds();

				ENQUEUE_RENDER_COMMAND(NanoGSUploadBoundedExactHQPage)(
					[Self, Upload, UploadEpochValue](FRHICommandListImmediate& RHICmdList)
					{
						Self->UploadBoundedPage_RenderThread(RHICmdList, *Upload, UploadEpochValue);
					});
				while (Self->UploadEpoch.load(std::memory_order_acquire) == UploadEpochValue &&
					!Upload->bRenderCommandComplete.load(std::memory_order_acquire))
				{
					FPlatformProcess::SleepNoStats(0.001f);
				}
				if (Self->UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
				{
					return;
				}
			}
			});
		}
	}

	void FPageRenderData::UploadBoundedPage_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		FPageUpload& Upload,
		const uint32 UploadEpochValue)
	{
		check(IsInRenderingThread());
		ON_SCOPE_EXIT
		{
			Upload.bRenderCommandComplete.store(true, std::memory_order_release);
		};
		if (UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
		{
			return;
		}
		if (bStopRequested.load(std::memory_order_acquire) || !bGPUBuffersCreated ||
			!PositionBuffer.IsValid() || !OtherDataBuffer.IsValid() ||
			!ColorOpacityBuffer.IsValid() || !SHBuffer.IsValid() ||
			!ActivePhysicalIndexBuffers[0].IsValid())
		{
			HandleBoundedFailure_RenderThread(
				TEXT("GPU source pool disappeared before a bounded page upload"), UploadEpochValue);
			return;
		}

		const double UploadStartSeconds = FPlatformTime::Seconds();
		const uint32 Count = Upload.SplatCount;
		const uint32 PositionOffset = Upload.PhysicalBase * 12u;
		const uint32 OtherOffset = Upload.PhysicalBase * 28u;
		const uint32 ColorOffset = Upload.PhysicalBase * 4u;
		const uint32 SHOffset = Upload.PhysicalBase * 96u;
		auto UploadBytes = [&RHICmdList](
			const FBufferRHIRef& Buffer,
			const uint32 Offset,
			const TConstArrayView64<uint8> Bytes)
		{
			void* Dest = RHICmdList.LockBuffer(Buffer, Offset, static_cast<uint32>(Bytes.Num()), RLM_WriteOnly);
			FMemory::Memcpy(Dest, Bytes.GetData(), static_cast<SIZE_T>(Bytes.Num()));
			RHICmdList.UnlockBuffer(Buffer);
		};
		UploadBytes(PositionBuffer, PositionOffset,
			Upload.Payload.GetRawStream(EStreamSemantic::Position));
		UploadBytes(OtherDataBuffer, OtherOffset, Upload.Other);
		UploadBytes(ColorOpacityBuffer, ColorOffset,
			Upload.Payload.GetRawStream(EStreamSemantic::ColorOpacity));
		UploadBytes(SHBuffer, SHOffset,
			Upload.Payload.GetRawStream(EStreamSemantic::SphericalHarmonics));

		const double UploadEndSeconds = FPlatformTime::Seconds();
		if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
		{
			UE_LOG(LogTemp, Display,
				TEXT("NanoGS Tree page pipeline: page=%llu splats=%u read=%.3f ms decode=%.3f ms queue=%.3f ms upload=%.3f ms"),
				static_cast<unsigned long long>(Upload.PageId), Count, Upload.ReadMilliseconds,
				Upload.DecodeMilliseconds, (UploadStartSeconds - Upload.EnqueueSeconds) * 1000.0,
				(UploadEndSeconds - UploadStartSeconds) * 1000.0);
		}

		FString Error;
		if (!ResidentPool.MarkLoadComplete(Upload.LoadOperation, Error))
		{
			HandleBoundedFailure_RenderThread(Error, UploadEpochValue);
			return;
		}
		PageResidencyGeneration.fetch_add(1, std::memory_order_acq_rel);
		TryPublishBoundedFrontier_RenderThread(UploadEpochValue);
	}

	void FPageRenderData::MarkUploadFailure(const FString& Error, const uint32 UploadEpochValue)
	{
		if (UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
		{
			return;
		}
		bUploadFailed.store(true, std::memory_order_release);
		bUploadComplete.store(false, std::memory_order_release);
		// Exact-HQ fails closed: never leave a silently incomplete scene active.
		ActiveSplatCount.store(0, std::memory_order_release);
		if (SceneBudgetIdentity != 0 && SceneBudgetOwnerIdentity != 0)
		{
			FString IgnoredBudgetError;
			FSceneBudgetRegistry::Get().TrySetActiveSplats(
				SceneBudgetIdentity, SceneBudgetOwnerIdentity, ESceneActiveBudgetLane::Main,
				0, 0, IgnoredBudgetError);
		}
		ResidentGeneration.fetch_add(1, std::memory_order_acq_rel);
		UE_LOG(LogTemp, Error, TEXT("NanoGS Page Exact-HQ upload failed: %s"), *Error);
	}

	void FPageRenderData::StartAllResidentUpload(const uint32 UploadEpochValue)
	{
		const TSharedRef<FPageRenderData, ESPMode::ThreadSafe> Self = AsShared();
		const double StartSeconds = FPlatformTime::Seconds();
		const bool bVerifyStreamCRCs =
			CVarNanoGSPagedVerifyPayloadCRC.GetValueOnAnyThread() != 0;
		int32 ConfiguredGlobalIOPages = FMath::Max(
			0, CVarNanoGSPagedGlobalIOPages.GetValueOnAnyThread());
		FParse::Value(FCommandLine::Get(), TEXT("NanoGSPagedGlobalIOPages="), ConfiguredGlobalIOPages);
		const uint64 GlobalIOPageLimit = static_cast<uint64>(FMath::Max(0, ConfiguredGlobalIOPages));
		Async(EAsyncExecution::ThreadPool,
			[Self, StartSeconds, UploadEpochValue, bVerifyStreamCRCs, GlobalIOPageLimit]()
		{
			FPagePayloadReadOptions ReadOptions;
			ReadOptions.bVerifyStreamCRCs = bVerifyStreamCRCs;

			TArray<int32> LoadOrder;
			LoadOrder.Reserve(Self->Manifest.Pages.Num());
			for (int32 PageOrdinal = 0; PageOrdinal < Self->Manifest.Pages.Num(); ++PageOrdinal)
			{
				LoadOrder.Add(PageOrdinal);
			}
			LoadOrder.Sort([Self](const int32 A, const int32 B)
			{
				return Self->Manifest.Pages[A].FirstOutputIndex < Self->Manifest.Pages[B].FirstOutputIndex;
			});

			uint32 ExpectedActiveBase = 0;
			uint32 ExpectedLoadedPageCount = 0;
			for (const int32 PageOrdinal : LoadOrder)
			{
				if (Self->bStopRequested.load(std::memory_order_acquire) ||
					Self->UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
				{
					return;
				}

				bool bGlobalIOPermit = false;
				while (Self->SceneBudgetIdentity != 0 && Self->SceneBudgetOwnerIdentity != 0 &&
					!(bGlobalIOPermit = FSceneBudgetRegistry::Get().TryAcquireInFlightPages(
						Self->SceneBudgetIdentity, Self->SceneBudgetOwnerIdentity, 1, GlobalIOPageLimit)))
				{
					if (Self->bStopRequested.load(std::memory_order_acquire) ||
						Self->UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
					{
						return;
					}
					FPlatformProcess::SleepNoStats(0.001f);
				}
				ON_SCOPE_EXIT
				{
					if (bGlobalIOPermit)
					{
						FSceneBudgetRegistry::Get().ReleaseInFlightPages(
							Self->SceneBudgetIdentity, Self->SceneBudgetOwnerIdentity, 1);
					}
				};

				const FPageRecord& Page = Self->Manifest.Pages[PageOrdinal];
				if (Page.SplatCount > MAX_uint32 || Page.FirstOutputIndex != ExpectedActiveBase)
				{
					Self->MarkUploadFailure(FString::Printf(
						TEXT("page %llu breaks the sequential active-output contract"),
						static_cast<unsigned long long>(Page.PageId)), UploadEpochValue);
					return;
				}

				FOwnedPagePayload Payload;
				FString ReadError;
				if (!FPageFileReader::ReadPagePayload(
					Self->Manifest, Page.PageId, Payload, ReadError, ReadOptions))
				{
					Self->MarkUploadFailure(ReadError, UploadEpochValue);
					return;
				}

				const uint32 Count = static_cast<uint32>(Page.SplatCount);
				const uint64 PhysicalBase64 = static_cast<uint64>(PageOrdinal) * Self->PageSlotCapacity;
				if (PhysicalBase64 + Count > Self->SourceCapacity)
				{
					Self->MarkUploadFailure(TEXT("page physical slot exceeds allocated source pool"), UploadEpochValue);
					return;
				}

				TSharedRef<FPageUpload, ESPMode::ThreadSafe> Upload = MakeShared<FPageUpload, ESPMode::ThreadSafe>();
				Upload->PageId = Page.PageId;
				Upload->PageOrdinal = static_cast<uint32>(PageOrdinal);
				Upload->PhysicalBase = static_cast<uint32>(PhysicalBase64);
				Upload->ActiveBase = ExpectedActiveBase;
				Upload->SplatCount = Count;

				const TConstArrayView64<float> Rotations = Payload.GetRotations();
				const TConstArrayView64<float> Scales = Payload.GetScales();
				if (Rotations.Num() != static_cast<int64>(Count) * 4 ||
					Scales.Num() != static_cast<int64>(Count) * 3)
				{
					Self->MarkUploadFailure(TEXT("decoded page rotation/scale view length mismatch"), UploadEpochValue);
					return;
				}
				Upload->Other.SetNumUninitialized(static_cast<int64>(Count) * 28);
				for (uint32 LocalIndex = 0; LocalIndex < Count; ++LocalIndex)
				{
					uint8* Dest = Upload->Other.GetData() + static_cast<uint64>(LocalIndex) * 28ull;
					FMemory::Memcpy(Dest, Rotations.GetData() + static_cast<uint64>(LocalIndex) * 4ull, 16);
					FMemory::Memcpy(Dest + 16, Scales.GetData() + static_cast<uint64>(LocalIndex) * 3ull, 12);
				}
				if (Payload.GetRawStream(EStreamSemantic::Position).Num() != static_cast<int64>(Count) * 12 ||
					Upload->Other.Num() != static_cast<int64>(Count) * 28 ||
					Payload.GetRawStream(EStreamSemantic::ColorOpacity).Num() != static_cast<int64>(Count) * 4 ||
					Payload.GetRawStream(EStreamSemantic::SphericalHarmonics).Num() != static_cast<int64>(Count) * 96)
				{
					Self->MarkUploadFailure(TEXT("decoded page stream byte length mismatch"), UploadEpochValue);
					return;
				}
				Upload->Payload = MoveTemp(Payload);

				Upload->ActivePhysicalIndices.SetNumUninitialized(Count);
				for (uint32 LocalIndex = 0; LocalIndex < Count; ++LocalIndex)
				{
					Upload->ActivePhysicalIndices[LocalIndex] = Upload->PhysicalBase + LocalIndex;
				}

				// Bound queued CPU memory to one page: do not decode the next page until
				// this page has been copied into the render-thread-owned buffers. Epoch
				// polling avoids a shutdown deadlock if the render thread stops servicing
				// commands; the queued command itself owns Upload until it runs or is freed.
				ENQUEUE_RENDER_COMMAND(NanoGSUploadExactHQPage)(
					[Self, Upload, UploadEpochValue](FRHICommandListImmediate& RHICmdList)
					{
						Self->UploadPage_RenderThread(RHICmdList, *Upload, UploadEpochValue);
					});
				++ExpectedLoadedPageCount;
				while (Self->UploadEpoch.load(std::memory_order_acquire) == UploadEpochValue &&
					!Self->bUploadFailed.load(std::memory_order_acquire) &&
					!Upload->bRenderCommandComplete.load(std::memory_order_acquire))
				{
					FPlatformProcess::SleepNoStats(0.001f);
				}
				if (Self->UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
				{
					return;
				}

				if (Self->bUploadFailed.load(std::memory_order_acquire))
				{
					return;
				}
				if (Self->LoadedPageCount.load(std::memory_order_acquire) != ExpectedLoadedPageCount)
				{
					Self->MarkUploadFailure(
						TEXT("all-resident loaded-page progress did not advance exactly once"),
						UploadEpochValue);
					return;
				}
				ExpectedActiveBase += Count;
			}

			if (ExpectedActiveBase != Self->TotalSplatCount)
			{
				Self->MarkUploadFailure(TEXT("published active count does not equal the Page v1 total"), UploadEpochValue);
				return;
			}
			if (Self->UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
			{
				return;
			}
			if (!Self->bUploadComplete.load(std::memory_order_acquire) ||
				Self->ActiveSplatCount.load(std::memory_order_acquire) != Self->TotalSplatCount)
			{
				Self->MarkUploadFailure(
					TEXT("all-resident source upload finished without one complete atomic publication"),
					UploadEpochValue);
				return;
			}
			UE_LOG(LogTemp, Log,
				TEXT("NanoGS Page Exact-HQ upload complete: %u splats / %u pages in %.3f s (CRC=%s, bounded CPU scratch=one page)"),
				Self->TotalSplatCount,
				Self->LoadedPageCount.load(std::memory_order_acquire),
				FPlatformTime::Seconds() - StartSeconds,
				ReadOptions.bVerifyStreamCRCs ? TEXT("on") : TEXT("off"));
		});
	}

	void FPageRenderData::UploadPage_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		FPageUpload& Upload,
		const uint32 UploadEpochValue)
	{
		check(IsInRenderingThread());
		if (UploadEpoch.load(std::memory_order_acquire) != UploadEpochValue)
		{
			return;
		}
		if (bStopRequested.load(std::memory_order_acquire) || !bGPUBuffersCreated ||
			!PositionBuffer.IsValid() || !OtherDataBuffer.IsValid() ||
			!ColorOpacityBuffer.IsValid() || !SHBuffer.IsValid() ||
			!ActivePhysicalIndexBuffers[0].IsValid())
		{
			MarkUploadFailure(TEXT("GPU source pool disappeared before a queued page upload"), UploadEpochValue);
			return;
		}

		const uint32 Count = Upload.SplatCount;
		const uint32 PositionOffset = Upload.PhysicalBase * 12u;
		const uint32 OtherOffset = Upload.PhysicalBase * 28u;
		const uint32 ColorOffset = Upload.PhysicalBase * 4u;
		const uint32 SHOffset = Upload.PhysicalBase * 96u;
		const uint32 ActiveOffset = Upload.ActiveBase * sizeof(uint32);

		auto UploadBytes = [&RHICmdList](
			const FBufferRHIRef& Buffer,
			const uint32 Offset,
			const TConstArrayView64<uint8> Bytes)
		{
			void* Dest = RHICmdList.LockBuffer(Buffer, Offset, static_cast<uint32>(Bytes.Num()), RLM_WriteOnly);
			FMemory::Memcpy(Dest, Bytes.GetData(), static_cast<SIZE_T>(Bytes.Num()));
			RHICmdList.UnlockBuffer(Buffer);
		};

		UploadBytes(PositionBuffer, PositionOffset,
			Upload.Payload.GetRawStream(EStreamSemantic::Position));
		UploadBytes(OtherDataBuffer, OtherOffset, Upload.Other);
		UploadBytes(ColorOpacityBuffer, ColorOffset,
			Upload.Payload.GetRawStream(EStreamSemantic::ColorOpacity));
		UploadBytes(SHBuffer, SHOffset,
			Upload.Payload.GetRawStream(EStreamSemantic::SphericalHarmonics));
		{
			const uint32 ActiveBytes = Count * sizeof(uint32);
			void* Dest = RHICmdList.LockBuffer(
				ActivePhysicalIndexBuffers[0], ActiveOffset, ActiveBytes, RLM_WriteOnly);
			FMemory::Memcpy(Dest, Upload.ActivePhysicalIndices.GetData(), ActiveBytes);
			RHICmdList.UnlockBuffer(ActivePhysicalIndexBuffers[0]);
		}

		// LoadedPageCount is progress only. ActiveSplatCount must remain zero until
		// every source stream and every active-index entry for the whole container is
		// complete, otherwise an early AirSim request can capture a plausible but
		// permanently incomplete scene.
		const uint32 PreviousLoadedPageCount =
			LoadedPageCount.fetch_add(1, std::memory_order_acq_rel);
		const uint32 NewLoadedPageCount = PreviousLoadedPageCount + 1u;
		if (PreviousLoadedPageCount >= static_cast<uint32>(Manifest.Pages.Num()))
		{
			MarkUploadFailure(TEXT("all-resident page upload progress exceeded manifest page count"), UploadEpochValue);
			return;
		}
		if (NewLoadedPageCount == static_cast<uint32>(Manifest.Pages.Num()))
		{
			if (Upload.ActiveBase > TotalSplatCount || Count != TotalSplatCount - Upload.ActiveBase)
			{
				MarkUploadFailure(TEXT("final all-resident page does not terminate at total splat count"), UploadEpochValue);
				return;
			}
			PublishedActiveSplatCount = TotalSplatCount;
			PublishedPageCount.store(NewLoadedPageCount, std::memory_order_release);
			ActiveSplatCount.store(TotalSplatCount, std::memory_order_release);
			ResidentGeneration.fetch_add(1, std::memory_order_acq_rel);
			bUploadComplete.store(true, std::memory_order_release);
		}
		Upload.bRenderCommandComplete.store(true, std::memory_order_release);
	}
}
