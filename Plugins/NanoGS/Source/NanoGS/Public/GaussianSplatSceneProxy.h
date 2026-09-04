// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "GaussianDataTypes.h"
#include "GaussianClusterTypes.h"
#include "Paged/NanoGSSpatialLODFormat.h"
#include "Paged/NanoGSSpatialLODSelector.h"
#include "Paged/NanoGSTreeSelector.h"
#include "Paged/NanoGSPagedRenderData.h"
#include "RenderResource.h"
#include "RHI.h"
#include "RHIResources.h"
#include <atomic>

class UGaussianSplatComponent;
struct FNanoGSTreeRuntimeStats;
class UGaussianSplatAsset;
class UNanoGSPagedSourceAsset;
class UNanoGSSpatialLODSourceAsset;
class UNanoGSTreeSourceAsset;
class FGaussianSplatRenderData;
namespace NanoGS::Paged
{
	class FPageRenderData;
	struct FCaptureSourceSnapshot;
}


/**
 * GPU resources for Gaussian Splatting rendering
 * FBufferRHIRef: Raw GPU Memory
 * ShaderResourceViewRHIRef(SRV): interpret the memory into corresponding type, shader read only
 * UnorderedAccessViewRHIRef(UAV): interpret the memory into corresponding type, shader read/write
 */
class FGaussianSplatGPUResources : public FRenderResource
{
public:
	FGaussianSplatGPUResources();
	virtual ~FGaussianSplatGPUResources();

	/** Initialize resources from asset data */
	void Initialize(UGaussianSplatAsset* Asset);

	/** Initialize from a metadata-only Page v1 source and its shared resident pool. */
	void Initialize(const TSharedPtr<NanoGS::Paged::FPageRenderData, ESPMode::ThreadSafe>& InPagedData);

	/** Check if resources are valid */
	bool IsValid() const
	{
		const bool bGeometryStreamsValid = bUsesDedicatedColorOpacityBuffer
			? ColorOpacityBufferSRV.IsValid() && PositionBufferSRV.IsValid() && OtherDataBufferSRV.IsValid()
			: PackedSplatBufferSRV.IsValid();
		const bool bSHStreamValid = SHBands <= 0 || SHBufferSRV.IsValid();
		return bInitialized && SplatCount > 0 && bGeometryStreamsValid && bSHStreamValid;
	}

	/** Get number of splats */
	int32 GetSplatCount() const { return SplatCount; }
	uint32 GetActiveSplatCount(bool bCaptureFrontier = false) const;
	uint32 GetSourceSplatCapacity() const;
	uint32 GetResidentGeneration(bool bCaptureFrontier = false) const;
	bool IsPagedUploadComplete() const;
	bool IsBoundedPagedStreamingEnabled() const;
	void RefreshPagedFrontierBinding_RenderThread(bool bCaptureFrontier = false);
	void NotifyPagedFrontierDrawn_RenderThread(FRHICommandListImmediate& RHICmdList, bool bCaptureFrontier = false);

	//~ Begin FRenderResource Interface
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;
	//~ End FRenderResource Interface

public:
	/** Packed splat data buffer (16 bytes/splat: RGBA + float16 pos + octahedral quat + log scale) */
	FBufferRHIRef PackedSplatBuffer;
	FShaderResourceViewRHIRef PackedSplatBufferSRV;

	/** Exact 4-byte RGBA8/opacity stream used by the Float32 HQ source layout. */
	FBufferRHIRef ColorOpacityBuffer;
	FShaderResourceViewRHIRef ColorOpacityBufferSRV;

	/** Original Float32 positions (12 bytes/splat), shared per asset for high-quality rendering. */
	FBufferRHIRef PositionBuffer;
	FShaderResourceViewRHIRef PositionBufferSRV;

	/** Original Float32 rotation + scale data (28 bytes/splat), shared per asset for high-quality rendering. */
	FBufferRHIRef OtherDataBuffer;
	FShaderResourceViewRHIRef OtherDataBufferSRV;

	/** Spherical harmonics buffer */
	FBufferRHIRef SHBuffer;
	FShaderResourceViewRHIRef SHBufferSRV;

	/** Chunk info buffer */
	FBufferRHIRef ChunkBuffer;
	FShaderResourceViewRHIRef ChunkBufferSRV;

	/** View data buffer (computed per-frame) */
	FBufferRHIRef ViewDataBuffer;
	FUnorderedAccessViewRHIRef ViewDataBufferUAV;
	FShaderResourceViewRHIRef ViewDataBufferSRV;

	/** Sort distance buffer */
	FBufferRHIRef SortDistanceBuffer;
	FUnorderedAccessViewRHIRef SortDistanceBufferUAV;
	FShaderResourceViewRHIRef SortDistanceBufferSRV;

	/** Sort keys buffer (double buffered) */
	FBufferRHIRef SortKeysBuffer;
	FUnorderedAccessViewRHIRef SortKeysBufferUAV;
	FShaderResourceViewRHIRef SortKeysBufferSRV;

	FBufferRHIRef SortKeysBufferAlt;
	FUnorderedAccessViewRHIRef SortKeysBufferAltUAV;
	FShaderResourceViewRHIRef SortKeysBufferAltSRV;

	/** Sort distance buffer alt (for radix sort ping-pong) */
	FBufferRHIRef SortDistanceBufferAlt;
	FUnorderedAccessViewRHIRef SortDistanceBufferAltUAV;

	/** Radix sort histogram buffer */
	FBufferRHIRef RadixHistogramBuffer;
	FUnorderedAccessViewRHIRef RadixHistogramBufferUAV;

	/** Radix sort digit offset buffer */
	FBufferRHIRef RadixDigitOffsetBuffer;
	FUnorderedAccessViewRHIRef RadixDigitOffsetBufferUAV;

	/** Sort indirect dispatch args buffer (for indirect radix sort)
	 * Format: uint3 (NumTiles, 1, 1) - used by CountCS and ScatterCS
	 * Written by PrepareIndirectArgs, read by DispatchIndirectComputeShader
	 */
	FBufferRHIRef SortIndirectArgsBuffer;
	FUnorderedAccessViewRHIRef SortIndirectArgsBufferUAV;

	/** Sort parameters buffer (for indirect radix sort)
	 * Format: uint2 (SortCount, NumTiles)
	 * Written by PrepareIndirectArgs, read by radix sort shaders
	 */
	FBufferRHIRef SortParamsBuffer;
	FUnorderedAccessViewRHIRef SortParamsBufferUAV;
	FShaderResourceViewRHIRef SortParamsBufferSRV;

	/** Index buffer for quad rendering */
	FBufferRHIRef IndexBuffer;

	/** Position format used by this asset (Float32, Norm16, etc.) */
	EGaussianPositionFormat PositionFormat = EGaussianPositionFormat::Float32;

	/** Asset-local origin used to restore float16 packed positions. */
	FVector3f PackedPositionOrigin = FVector3f::ZeroVector;

	/** Whether PositionBuffer contains a complete Float32 position for every splat. */
	bool bHasFloat32PositionData = false;

	/** Whether OtherDataBuffer contains a complete Float32 quaternion and scale for every original splat. */
	bool bHasFloat32RotationScaleData = false;

	/** HQ assets omit the redundant packed geometry and keep only its RGBA lane. */
	bool bUsesDedicatedColorOpacityBuffer = false;
	/** Tree v2 pages encode opacity over Spark's [0,2] LoD range in RGBA8. */
	bool bUsesLODOpacityEncoding = false;
	bool bHasPackedGeometryData = false;

	//----------------------------------------------------------------------
	// Cluster culling resources (Nanite-style optimization)
	//----------------------------------------------------------------------

	/** Cluster data buffer (static, loaded from asset) */
	FBufferRHIRef ClusterBuffer;
	FShaderResourceViewRHIRef ClusterBufferSRV;

	/** Visible cluster indices buffer (written by culling shader) */
	FBufferRHIRef VisibleClusterBuffer;
	FUnorderedAccessViewRHIRef VisibleClusterBufferUAV;
	FShaderResourceViewRHIRef VisibleClusterBufferSRV;

	/** Visible cluster count buffer (atomic counter) */
	FBufferRHIRef VisibleClusterCountBuffer;
	FUnorderedAccessViewRHIRef VisibleClusterCountBufferUAV;
	FShaderResourceViewRHIRef VisibleClusterCountBufferSRV;

	/** Number of clusters */
	int32 ClusterCount = 0;

	/** Number of leaf clusters (for rendering) */
	int32 LeafClusterCount = 0;

	/** Whether cluster culling is available */
	bool bHasClusterData = false;

	/** Whether Nanite is enabled for this asset (per-asset setting) */
	bool bEnableNanite = false;

	//----------------------------------------------------------------------
	// LOD splat resources (for parent cluster rendering)
	//----------------------------------------------------------------------

	/** LOD splat data buffer (static, loaded from asset) */
	FBufferRHIRef LODSplatBuffer;
	FShaderResourceViewRHIRef LODSplatBufferSRV;

	/** Number of LOD splats */
	int32 LODSplatCount = 0;

	/** Whether LOD splats are available */
	bool bHasLODSplats = false;

	//----------------------------------------------------------------------
	// Indirect draw resources (GPU-driven rendering)
	//----------------------------------------------------------------------

	/** Indirect draw argument buffer for GPU-driven rendering
	 * Structure: IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation
	 * Written by cluster culling, read by DrawIndexedPrimitiveIndirect
	 */
	FBufferRHIRef IndirectDrawArgsBuffer;
	FUnorderedAccessViewRHIRef IndirectDrawArgsBufferUAV;

	/** Whether indirect draw is available */
	bool bSupportsIndirectDraw = false;

	//----------------------------------------------------------------------
	// Cluster visibility integration resources
	//----------------------------------------------------------------------

	/** Maps each splat index to its cluster index
	 * Used by CalcViewData to check if splat's cluster is visible
	 */
	FBufferRHIRef SplatClusterIndexBuffer;
	FShaderResourceViewRHIRef SplatClusterIndexBufferSRV;

	/** Bitmap for cluster visibility (1 bit per cluster)
	 * Written by cluster culling, read by CalcViewData
	 * Size: ceil(ClusterCount / 32) uint32s
	 */
	FBufferRHIRef ClusterVisibilityBitmap;
	FUnorderedAccessViewRHIRef ClusterVisibilityBitmapUAV;
	FShaderResourceViewRHIRef ClusterVisibilityBitmapSRV;

	/** Selected cluster ID buffer for Nanite-style debug visualization
	 * Maps each leaf cluster to its selected cluster ID (may be parent if LOD is used)
	 * Written by cluster culling, read by CalcViewData
	 * Size: LeafClusterCount uint32s
	 */
	FBufferRHIRef SelectedClusterBuffer;
	FUnorderedAccessViewRHIRef SelectedClusterBufferUAV;
	FShaderResourceViewRHIRef SelectedClusterBufferSRV;

	//----------------------------------------------------------------------
	// LOD cluster selection output (for LOD rendering)
	//----------------------------------------------------------------------

	/** Buffer of unique parent cluster indices that need their LOD splats rendered
	 * Written by cluster culling, read by LOD rendering pass
	 * Size: ClusterCount uint32s (worst case all clusters could be selected)
	 */
	FBufferRHIRef LODClusterBuffer;
	FUnorderedAccessViewRHIRef LODClusterBufferUAV;
	FShaderResourceViewRHIRef LODClusterBufferSRV;

	/** Count of unique LOD clusters (atomic counter)
	 * Written by cluster culling, read back for LOD rendering dispatch
	 */
	FBufferRHIRef LODClusterCountBuffer;
	FUnorderedAccessViewRHIRef LODClusterCountBufferUAV;
	FShaderResourceViewRHIRef LODClusterCountBufferSRV;

	/** Bitmap to track which parent clusters have been claimed (1 bit per cluster)
	 * Used during cluster culling to ensure each parent is only added once
	 * Also read by GPU-driven LOD shader to check if cluster is selected
	 */
	FBufferRHIRef LODClusterSelectedBitmap;
	FUnorderedAccessViewRHIRef LODClusterSelectedBitmapUAV;
	FShaderResourceViewRHIRef LODClusterSelectedBitmapSRV;

	/** Total LOD splats to render (sum of all selected parent cluster LOD splat counts)
	 * Written by cluster culling, used for indirect draw args
	 */
	FBufferRHIRef LODSplatTotalBuffer;
	FUnorderedAccessViewRHIRef LODSplatTotalBufferUAV;
	FShaderResourceViewRHIRef LODSplatTotalBufferSRV;

	//----------------------------------------------------------------------
	// GPU-driven LOD rendering resources
	//----------------------------------------------------------------------

	/** Maps each LOD splat index to its owning cluster ID
	 * Used by GPU-driven LOD shader to check if LOD splat's cluster is selected
	 * Size: TotalLODSplats * sizeof(uint32)
	 */
	FBufferRHIRef LODSplatClusterIndexBuffer;
	FShaderResourceViewRHIRef LODSplatClusterIndexBufferSRV;

	/** Atomic counter for valid LOD splat output
	 * Written by CalcLODViewDataGPUDriven, read by UpdateDrawArgs
	 * Size: 4 bytes
	 */
	FBufferRHIRef LODSplatOutputCountBuffer;
	FUnorderedAccessViewRHIRef LODSplatOutputCountBufferUAV;
	FShaderResourceViewRHIRef LODSplatOutputCountBufferSRV;

	/** Total splat count for buffer allocation (SplatCount + TotalLODSplats) */
	int32 TotalSplatCount = 0;

	//----------------------------------------------------------------------
	// Splat compaction resources (GPU-driven work reduction)
	//----------------------------------------------------------------------

	/** Compacted list of visible splat indices
	 * Written by CompactSplats, read by CalcViewData (when using compaction)
	 * Size: TotalSplatCount * sizeof(uint32) (worst case all visible)
	 */
	FBufferRHIRef CompactedSplatIndicesBuffer;
	FUnorderedAccessViewRHIRef CompactedSplatIndicesBufferUAV;
	FShaderResourceViewRHIRef CompactedSplatIndicesBufferSRV;

	/** Atomic counter for visible splat count
	 * Written by CompactSplats, read by PrepareIndirectArgs
	 * Size: 4 bytes (single uint32)
	 */
	FBufferRHIRef VisibleSplatCountBuffer;
	FUnorderedAccessViewRHIRef VisibleSplatCountBufferUAV;
	FShaderResourceViewRHIRef VisibleSplatCountBufferSRV;

	/** Indirect dispatch arguments for compute shaders (CalcViewData, CalcDistances)
	 * Written by PrepareIndirectArgs, read by DispatchComputeIndirect
	 * Format: uint3 (numGroupsX, numGroupsY, numGroupsZ)
	 * Size: 12 bytes (3 uint32)
	 */
	FBufferRHIRef IndirectDispatchArgsBuffer;
	FUnorderedAccessViewRHIRef IndirectDispatchArgsBufferUAV;

	/** Whether splat compaction is enabled and buffers are available */
	bool bSupportsCompaction = false;

	/** The active-index buffer was authored by Page residency, not cluster compaction. */
	bool bHasPrecompactedActiveList = false;
	bool bIsPagedSource = false;

	/** Get position format as uint for shader */
	uint32 GetPositionFormatUint() const { return static_cast<uint32>(PositionFormat); }

	/** Get number of SH bands stored in SHBuffer (0 = no SH data) */
	int32 GetSHBands() const { return SHBands; }

private:
	/** Create per-instance cluster/compaction/sort buffers */
	void CreatePerInstanceBuffers(FRHICommandListBase& RHICmdList);

private:
	/** Shared render data (holds shared GPU buffers, ref-counted) */
	TSharedPtr<FGaussianSplatRenderData> SharedData;
	TSharedPtr<NanoGS::Paged::FPageRenderData, ESPMode::ThreadSafe> PagedSharedData;

	int32 SplatCount = 0;
	int32 SHBands = 0;  // Number of SH bands stored in SHBuffer (0 = no SH data)
	bool bInitialized = false;

public:
	/** Cached state for camera-static sort skipping */
	FMatrix CachedViewProjectionMatrix = FMatrix::Identity;
	FIntPoint CachedViewRectSize = FIntPoint::ZeroValue;
	FMatrix CachedLocalToWorld = FMatrix::Identity;
	float CachedOpacityScale = -1.0f;
	float CachedSplatScale = -1.0f;
	float CachedErrorThreshold = -1.0f;
	int32 CachedDebugMode = -1;
	int32 CachedDebugForceLODLevel = -1;
	int32 CachedUseFloat32Position = -1;
	int32 CachedUseFloat32RotationScale = -1;
	int32 CachedSHOrder = -1;
	uint32 CachedResidentGeneration = MAX_uint32;
	bool bHasCachedSortData = false;
};

/**
 * Scene proxy for rendering Gaussian Splatting
 */
class FGaussianSplatSceneProxy : public FPrimitiveSceneProxy
{
public:
	FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent);
	virtual ~FGaussianSplatSceneProxy();

	//~ Begin FPrimitiveSceneProxy Interface
	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
#if WITH_EDITOR
	virtual HHitProxy* CreateHitProxies(UPrimitiveComponent* Component, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies) override;
#endif
	//~ End FPrimitiveSceneProxy Interface

	/** Get GPU resources (may return nullptr if pending destruction) */
	FGaussianSplatGPUResources* GetGPUResources() const { return GPUResources; }

	/** Get splat count */
	int32 GetSplatCount() const;
	int32 GetSplatCountForView(bool bCaptureFrontier) const;
	bool IsPagedSource() const { return PagedRenderData.IsValid(); }
	void RequestPagedExactHQForViewFamily_RenderThread(
		const FSceneViewFamily& ViewFamily,
		uint64 StableCaptureIdentity = 0) const;
	/** Build one any-thread capture snapshot while the view-extension proxy lock is held. */
	bool GetPagedCaptureSourceSnapshot(NanoGS::Paged::FCaptureSourceSnapshot& OutSnapshot) const;

	/** Get rendering parameters */
	int32 GetSHOrder() const { return SHOrder; }
	float GetOpacityScale() const { return OpacityScale; }
	float GetSplatScale() const { return SplatScale; }
	float GetLODErrorThreshold() const { return LODErrorThreshold; }
	void SetRenderingParameters_RenderThread(
		int32 InSHOrder,
		float InOpacityScale,
		float InSplatScale,
		float InLODErrorThreshold,
		bool bInForceFullDetailLOD0,
		int32 InTreeLODSplatBudget,
		int32 InTreeLODProgressiveSplatBudget,
		float InTreeLODDetailScale)
	{
		SHOrder = InSHOrder;
		OpacityScale = InOpacityScale;
		SplatScale = InSplatScale;
		LODErrorThreshold = InLODErrorThreshold;
		bForceFullDetailLOD0 = bInForceFullDetailLOD0;
		TreeLODSplatBudget = InTreeLODSplatBudget;
		TreeLODProgressiveSplatBudget = InTreeLODProgressiveSplatBudget;
		TreeLODDetailScale = InTreeLODDetailScale;
	}

	/** Check if this proxy is safe to use for rendering.
	 *  Returns false if proxy is being destroyed or has invalid resources.
	 *  Thread-safe - can be called from render thread.
	 */
	bool IsValidForRendering() const
	{
		// Check destruction flag first (atomic, no lock needed)
		if (bPendingDestruction.load(std::memory_order_acquire))
		{
			return false;
		}
		// Then check resources
		return GPUResources != nullptr && GPUResources->IsValid();
	}

	/** Mark this proxy as pending destruction. Called at start of DestroyRenderThreadResources. */
	void MarkPendingDestruction()
	{
		bPendingDestruction.store(true, std::memory_order_release);
	}

private:
	void PublishTreeRuntimeStats_RenderThread(const NanoGS::Tree::FSelection& Selection) const;

	/** Atomic flag indicating this proxy is being destroyed.
	 *  Set at the start of DestroyRenderThreadResources to prevent
	 *  render commands from accessing resources during/after destruction.
	 */
	std::atomic<bool> bPendingDestruction{false};


	/** GPU resources */
	FGaussianSplatGPUResources* GPUResources = nullptr;

	/** Cached asset for initialization */
	UGaussianSplatAsset* CachedAsset = nullptr;
	UNanoGSPagedSourceAsset* CachedPagedAsset = nullptr;
	UNanoGSSpatialLODSourceAsset* CachedSpatialLODAsset = nullptr;
	UNanoGSTreeSourceAsset* CachedTreeAsset = nullptr;
	TUniquePtr<NanoGS::SpatialLOD::FManifest> SpatialLODManifest;
	TSharedPtr<NanoGS::Tree::FManifest, ESPMode::ThreadSafe> TreeManifest;
	TSharedPtr<TArray<uint32>, ESPMode::ThreadSafe> TreeParents;
	mutable NanoGS::SpatialLOD::FSelector SpatialLODSelector;
	mutable NanoGS::Tree::FSelector TreeSelector;
	struct FTreeSelectionTaskResult
	{
		std::atomic<bool> bComplete{false};
		std::atomic<bool> bCancelRequested{false};
		std::atomic<bool> bProgressiveComplete{false};
		std::atomic<bool> bProgressiveConsumed{false};
		NanoGS::Tree::FSelection Selection;
		TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe> ProgressiveExplicitSplatRefs;
		TSharedPtr<TArray<uint64>, ESPMode::ThreadSafe> ProgressivePageIds;
		TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe> ProgressiveSplatRuns;
		TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe> ProgressiveRenderSplatRefs;
		TSharedPtr<TArray<uint64>, ESPMode::ThreadSafe> ProgressiveRenderPageIds;
		TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe> ProgressiveRenderSplatRuns;
		TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe> SortedExplicitSplatRefs;
		TSharedPtr<TArray<uint64>, ESPMode::ThreadSafe> SortedPageIds;
		TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe> SplatRuns;
		FVector ViewOrigin = FVector::ZeroVector;
		FVector ViewDirection = FVector::ForwardVector;
		FIntPoint ViewSize = FIntPoint::ZeroValue;
		double FocalPixels = 0.0;
		double RequestSeconds = 0.0;
		double SelectionMilliseconds = 0.0;
		double IndexPreparationMilliseconds = 0.0;
		double ProgressiveReadyMilliseconds = 0.0;
		int32 ProgressiveTargetCount = 0;
		float SplitThresholdPx = -1.0f;
		uint32 MaxActiveNodes = 0;
		bool bFrustumCull = false;
		bool bExactCapture = false;
		bool bFullDetail = false;
	};
	struct FTreeTransitionTaskResult
	{
		std::atomic<bool> bComplete{false};
		TArray<uint64> RequestedPageIds;
		TArray<NanoGS::Paged::FExplicitSplatRef> RenderSplats;
		TArray<uint64> RenderPageIds;
		TArray<NanoGS::Paged::FExplicitSplatRun> RenderRuns;
		uint32 TargetGeneration = 0;
		uint32 PageResidencyGeneration = 0;
		double ResolutionMilliseconds = 0.0;
		bool bFallbackCapacityAvailable = false;
		bool bUsedRefinedTopology = false;
	};
	struct FRecentTreeDemand
	{
		TArray<uint32> TargetNodes;
		TBitArray<> RefinedNodes;
		TBitArray<> DemandedNodes;
		TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe> SortedExplicitSplatRefs;
		TSharedPtr<TArray<uint64>, ESPMode::ThreadSafe> SortedPageIds;
		TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe> SplatRuns;
		TSharedPtr<FTreeSelectionTaskResult, ESPMode::ThreadSafe> PendingSelection;
		FVector ViewOrigin = FVector::ZeroVector;
		FVector ViewDirection = FVector::ForwardVector;
		FIntPoint ViewSize = FIntPoint::ZeroValue;
		double FocalPixels = 0.0;
		double LastUpdateSeconds = 0.0;
		double LastSeenSeconds = 0.0;
		double ReadySinceSeconds = 0.0;
		float SplitThresholdPx = -1.0f;
		uint32 MaxActiveNodes = 0;
		bool bFrustumCull = false;
		bool bInitialized = false;
		bool bExactCapture = false;
		bool bFullDetail = false;
		bool bProgressiveSubmitted = false;
		double GPURefinementFirstRequestSeconds = 0.0;
		uint32 GPUExclusiveCPUSuppressedCount = 0;
	};
	mutable TMap<uint64, FRecentTreeDemand> RecentTreeDemands;
	mutable TArray<uint32> CachedTreeCombinedFrontier;
	mutable TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe> CachedTreeCombinedExplicitSplatRefs;
	mutable TSharedPtr<TArray<uint64>, ESPMode::ThreadSafe> CachedTreeCombinedPageIds;
	mutable TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe> CachedTreeCombinedSplatRuns;
	mutable TBitArray<> CachedTreeCombinedRefinedNodes;
	mutable TBitArray<> CachedTreeCombinedDemandedNodes;
	mutable TArray<uint32> CachedTreeCaptureFrontier;
	mutable TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRef>, ESPMode::ThreadSafe> CachedTreeCaptureExplicitSplatRefs;
	mutable TSharedPtr<TArray<uint64>, ESPMode::ThreadSafe> CachedTreeCapturePageIds;
	mutable TSharedPtr<TArray<NanoGS::Paged::FExplicitSplatRun>, ESPMode::ThreadSafe> CachedTreeCaptureSplatRuns;
	mutable bool bTreeHasMainDemand = false;
	mutable bool bTreeHasCaptureDemand = false;
	mutable bool bTreeCombinedDemandDirty = true;
	mutable bool bTreeCombinedDemandSubmissionPending = false;
	mutable bool bTreeCombinedDemandHasSubmitted = false;
	mutable bool bTreeCombinedDemandExactCapture = false;
	mutable bool bTreeCombinedDemandFullDetail = false;
	mutable uint32 TreeCombinedTargetGeneration = 0;
	mutable uint32 LastScheduledTreeResidencyGeneration = MAX_uint32;
	mutable TSharedPtr<FTreeTransitionTaskResult, ESPMode::ThreadSafe> PendingTreeTransition;
	struct FRecentSpatialLODDemand
	{
		TMap<uint32, uint32> LevelsByNode;
		TMap<uint32, uint32> CertifiedLevelsByNode;
		FVector ViewOrigin = FVector::ZeroVector;
		FVector ViewDirection = FVector::ForwardVector;
		double LastUpdateSeconds = 0.0;
		double LastSeenSeconds = 0.0;
		bool bInitialized = false;
	};
	mutable TMap<uint64, FRecentSpatialLODDemand> RecentSpatialLODDemands;
	mutable TArray<uint64> LastSpatialLODPageIds;
	mutable TMap<uint32, uint32> LastSubmittedSpatialLODLevels;
	mutable TMap<uint32, uint32> LastPublishedSpatialLODLevels;
	TSharedPtr<FNanoGSTreeRuntimeStats, ESPMode::ThreadSafe> TreeRuntimeStats;
	TSharedPtr<NanoGS::Paged::FPageRenderData, ESPMode::ThreadSafe> PagedRenderData;

	/** Rendering parameters */
	int32 SplatCount = 0;
	int32 SHOrder = 3;
	float OpacityScale = 1.0f;
	float SplatScale = 1.0f;
	float LODErrorThreshold = 0.03f;
	bool bForceFullDetailLOD0 = false;
	int32 TreeLODSplatBudget = 8000000;
	int32 TreeLODProgressiveSplatBudget = 4000000;
	float TreeLODDetailScale = 1.0f;

#if WITH_EDITOR
	/** Cached hit proxy created in CreateHitProxies, used for editor viewport click selection. */
	HHitProxy* SelectionHitProxy = nullptr;
#endif
};
