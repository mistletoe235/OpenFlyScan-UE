// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Paged/NanoGSPageDemandSelector.h"
#include "Paged/NanoGSPageFormat.h"
#include "Paged/NanoGSPageResidentPool.h"
#include "Paged/NanoGSTreeGPUData.h"
#include "Paged/NanoGSTreeV3Reader.h"
#include "RHI.h"
#include "RHIResources.h"

#include <atomic>

class UNanoGSPagedSourceAsset;
class UNanoGSSpatialLODSourceAsset;
class UNanoGSTreeSourceAsset;
class FSceneViewFamily;
class FRHIGPUBufferReadback;

namespace NanoGS::Paged
{
	struct FSharedTreeGPUResources;
	struct NANOGS_API FExplicitSplatRef
	{
		uint64 PageId = 0;
		uint32 PageLocal = 0;

		bool operator==(const FExplicitSplatRef& Other) const
		{
			return PageId == Other.PageId && PageLocal == Other.PageLocal;
		}
	};

	struct NANOGS_API FExplicitSplatRun
	{
		uint64 PageId = 0;
		uint32 PageLocal = 0;
		uint32 Count = 0;
	};

	enum class EExactHQStreamingState : uint8
	{
		/** The validated legacy behavior: every Page v1 payload is loaded. */
		AllResident = 0,
		/** Bounded mode has no view demand yet. */
		NotRequested,
		/** A complete target frontier is being read/uploaded; the old frontier remains active. */
		Loading,
		/**
		 * A disjoint target fits by itself but not beside the old frontier. Drawing is
		 * suspended until the old frontier's last-use fence makes its slots reusable.
		 */
		Draining,
		/** The last complete requested frontier is atomically published. */
		Ready,
		/** The complete target cannot fit; drawing is suspended while published storage is retained. */
		OutOfCapacity,
		/** IO, CRC, upload, or active-index publication failed; the old frontier remains active. */
		Failed,
	};

	/**
	 * Shared GPU source pool for one Page v1 container.
	 *
	 * The initial implementation deliberately publishes only complete pages.  A
	 * background worker owns at most one decoded page, waits for its render-thread
	 * upload, then advances to the next page.  Consequently an active physical
	 * index can never reference partially uploaded source streams.
	 *
	 * Page v1 stores rotation and scale as separate SoA streams while the existing
	 * exact-HQ NanoGS shader consumes the legacy interleaved 28-byte stream.  The
	 * bounded page scratch interleaves those two streams without changing values.
	 */
	class NANOGS_API FPageRenderData final
		: public TSharedFromThis<FPageRenderData, ESPMode::ThreadSafe>
	{
	public:
		FPageRenderData();
		~FPageRenderData();

		/** Bind this source to a scene-level budget owner before Initialize. */
		void SetSceneBudgetIdentity(uint64 SceneIdentity, uint64 OwnerIdentity);

		/** Open and validate metadata only. Safe to call before the render resource exists. */
		bool Initialize(const UNanoGSPagedSourceAsset* Asset, FString& OutError);
		bool Initialize(const UNanoGSSpatialLODSourceAsset* Asset, FString& OutError);
		bool Initialize(const UNanoGSTreeSourceAsset* Asset, FString& OutError);

		/** Allocate the fixed GPU pool and start bounded, page-at-a-time Exact-HQ upload. */
		bool CreateGPUBuffers(FRHICommandListBase& RHICmdList, FString& OutError);

		/** Release this object's GPU references. Call on the render thread. */
		void ReleaseGPUBuffers();

		/** Invalidate in-flight I/O and queued uploads without releasing RHI references. */
		void CancelUpload();

		/**
		 * Submit the conservative Exact-HQ union for every view in one view family.
		 *
		 * This is render-thread-only and is a no-op unless bounded complete-frontier
		 * streaming was selected before Initialize. Invalid view/projection/bounds
		 * data deliberately selects every page. A target is never partially published.
		 */
		void RequestExactHQForViewFamily_RenderThread(
			const FSceneViewFamily& ViewFamily,
			const FTransform& LocalToWorld,
			double SplatScale,
			uint64 StableCaptureIdentity = 0);

		/** Submit an already validated, complete page frontier (used by SpatialLOD). */
		void RequestExplicitPages_RenderThread(
			TConstArrayView<uint64> RequestedPageIds,
			uint64 DemandIdentity,
			bool bAllowPublishedFrontierForCapture = false);

		/** Request a sparse active frontier while retaining/loading complete source pages transactionally. */
		void RequestExplicitSplats_RenderThread(
			TArray<FExplicitSplatRef>&& RequestedSplats,
			TArray<uint64>&& PrevalidatedSortedPageIds,
			TArray<FExplicitSplatRun>&& PrecomputedRuns,
			uint64 DemandIdentity,
			bool bAllowPublishedFrontierForCapture = false,
			bool bAlreadySortedUnique = false);

		/** Snapshot resident source pages for background Tree transition resolution. */
		void GetResidentPageIds_RenderThread(TArray<uint64>& OutPageIds) const;
		/** Snapshot the currently published page pin set. */
		void GetPublishedPageIds_RenderThread(TArray<uint64>& OutPageIds) const;
		uint32 GetMissingTargetPageCount_RenderThread() const;

		/**
		 * Publish a Tree fallback frontier using only pages already resident in the
		 * current target transaction. The outstanding target I/O remains active.
		 */
		bool PublishResidentTreeFrontier_RenderThread(
			TArray<FExplicitSplatRef>&& RenderSplats,
			TArray<uint64>&& RenderPageIds,
			TArray<FExplicitSplatRun>&& RenderRuns,
			bool bAlreadySortedUnique = false);

		/** Publish the strict SceneCapture frontier without replacing the main-view frontier. */
		bool PublishResidentCaptureTreeFrontier_RenderThread(
			TArray<FExplicitSplatRef>&& RenderSplats,
			TArray<uint64>&& RenderPageIds,
			TArray<FExplicitSplatRun>&& RenderRuns,
			bool bAlreadySortedUnique = false);

		/** Write a fence after a draw that consumed the selected view frontier. */
		void NotifyFrontierDrawn_RenderThread(FRHICommandListImmediate& RHICmdList, bool bCaptureFrontier = false);

		/** Render-thread-only access to the selected view active-index buffer. */
		const FBufferRHIRef& GetPublishedActivePhysicalIndexBuffer_RenderThread(bool bCaptureFrontier = false) const;
		const FShaderResourceViewRHIRef& GetPublishedActivePhysicalIndexBufferSRV_RenderThread(bool bCaptureFrontier = false) const;

		bool IsInitialized() const { return bInitialized; }
		bool AreGPUBuffersCreated() const { return bGPUBuffersCreated; }
		bool HasUploadFailed() const { return bUploadFailed.load(std::memory_order_acquire); }
		bool IsUploadComplete() const { return bUploadComplete.load(std::memory_order_acquire); }
		bool IsBoundedStreamingEnabled() const { return bBoundedStreamingEnabled; }
		bool CanCapturePublishedFrontierDuringTransition() const
		{
			return bPublishedFrontierCaptureSafeDuringTransition.load(std::memory_order_acquire);
		}
		EExactHQStreamingState GetStreamingState() const
		{
			return static_cast<EExactHQStreamingState>(StreamingState.load(std::memory_order_acquire));
		}
		bool IsRequestedFrontierReady() const
		{
			return GetStreamingState() == EExactHQStreamingState::Ready;
		}

		uint32 GetActiveSplatCount(bool bCaptureFrontier = false) const
		{
			if (bCaptureFrontier)
			{
				return CaptureActiveSplatCount.load(std::memory_order_acquire);
			}
			return bTreeGPUFrontierPublished.load(std::memory_order_acquire)
				? TreeGPUActiveSplatCount.load(std::memory_order_acquire)
				: ActiveSplatCount.load(std::memory_order_acquire);
		}
		uint32 GetResidentGeneration(bool bCaptureFrontier = false) const
		{
			return bCaptureFrontier
				? CaptureResidentGeneration.load(std::memory_order_acquire)
				: ResidentGeneration.load(std::memory_order_acquire);
		}
		bool IsCaptureFrontierReady() const
		{
			return bCaptureFrontierReady.load(std::memory_order_acquire);
		}
		uint32 GetPublishedCapturePageCount() const
		{
			return PublishedCapturePageCount.load(std::memory_order_acquire);
		}
		uint32 GetPageResidencyGeneration() const { return PageResidencyGeneration.load(std::memory_order_acquire); }
		uint32 GetLoadedPageCount() const { return LoadedPageCount.load(std::memory_order_acquire); }
		uint32 GetRequestedPageCount() const { return RequestedPageCount.load(std::memory_order_acquire); }
		uint32 GetPublishedPageCount() const { return PublishedPageCount.load(std::memory_order_acquire); }
		uint32 GetSourceCapacity() const { return SourceCapacity; }
		uint32 GetTotalSplatCount() const { return TotalSplatCount; }
		uint32 GetPageSlotCapacity() const { return PageSlotCapacity; }
		uint32 GetPhysicalSlotCount() const { return PhysicalSlotCount; }
		const FBox& GetSceneBounds() const { return SceneBounds; }
		const FString& GetSourceFilename() const { return Manifest.SourceFilename; }
		bool HasTreeGPUNodeBuffer() const { return TreeGPUNodeBufferSRV.IsValid() && TreeGPUNodeCount > 0; }
		bool IsTreeGPUFrontierPublished() const
		{
			return bTreeGPUFrontierPublished.load(std::memory_order_acquire);
		}
		uint32 GetTreeGPUNodeCount() const { return TreeGPUNodeCount; }
		const FShaderResourceViewRHIRef& GetTreeGPUNodeBufferSRV() const { return TreeGPUNodeBufferSRV; }
		bool AdvanceTreeGPUFrontier_RenderThread(
			FRHICommandListImmediate& RHICmdList,
			const FVector3f& ViewOriginLocal,
			float ProjectionScale,
			float SplitThresholdPixels,
			bool bResetToRoot);
		bool IsTreeGPUFrontierRefinementPending_RenderThread() const
		{
			check(IsInRenderingThread());
			return bTreeGPUFrontierRefinementPending;
		}
		void RetireTreeGPUFrontier_RenderThread(const TCHAR* Reason);
		const FShaderResourceViewRHIRef& GetTreeGPUFrontierSRV() const
		{
			return TreeGPUFrontierSRVs[TreeGPUFrontierReadSlot];
		}
		const FShaderResourceViewRHIRef& GetTreeGPUFrontierCountSRV() const
		{
			return TreeGPUFrontierCountSRVs[TreeGPUFrontierReadSlot];
		}
		void ConsumeTreeGPUBlockRequests_RenderThread(TArray<uint32>& OutBlockIds);

	public:
		// Exact-HQ source buffers. OriginalId is intentionally disk/validation only.
		FBufferRHIRef PackedSplatBuffer;
		FShaderResourceViewRHIRef PackedSplatBufferSRV;
		FBufferRHIRef ColorOpacityBuffer;
		FShaderResourceViewRHIRef ColorOpacityBufferSRV;
		FBufferRHIRef PositionBuffer;
		FShaderResourceViewRHIRef PositionBufferSRV;
		FBufferRHIRef OtherDataBuffer;
		FShaderResourceViewRHIRef OtherDataBufferSRV;
		FBufferRHIRef SHBuffer;
		FShaderResourceViewRHIRef SHBufferSRV;

		/** Mandatory shader-binding dummy; paged Exact-HQ never performs cluster LOD. */
		FBufferRHIRef SplatClusterIndexBuffer;
		FShaderResourceViewRHIRef SplatClusterIndexBufferSRV;

		/** Shared six-index quad. */
		FBufferRHIRef IndexBuffer;

	private:
		struct FPageUpload;

		bool InitializeManifest(FManifest&& InManifest, bool bForceBounded, FString& OutError);
		void StartAllResidentUpload(uint32 UploadEpochValue);
		void UploadPage_RenderThread(
			FRHICommandListImmediate& RHICmdList,
			FPageUpload& Upload,
			uint32 UploadEpochValue);
		void StartBoundedLoads(
			TArray<FPageLoadOperation> LoadOperations,
			uint32 UploadEpochValue);
		void UploadBoundedPage_RenderThread(
			FRHICommandListImmediate& RHICmdList,
			FPageUpload& Upload,
			uint32 UploadEpochValue);
		void HandleBoundedFailure_RenderThread(const FString& Error, uint32 UploadEpochValue);
		bool CancelBoundedTransition_RenderThread();
		void BeginBoundedDrain_RenderThread(TConstArrayView<uint64> RequestedPageIds);
		void AdvanceBoundedDrain_RenderThread();
		void RestorePublishedFrontierAfterDrain_RenderThread();
		void SuspendPublishedFrontierOutOfCapacity_RenderThread(
			TConstArrayView<uint64> RequestedPageIds,
			const FString& Reason);
		void FailFenceSafetyClosed_RenderThread(const FString& Error);
		bool ReleaseCompletedRetirements_RenderThread();
		void ApplyBoundedDemand_RenderThread(
			TArray<uint64> RequestedPageIds,
			bool bAllowPublishedFrontierForCapture = false,
			TArray<FExplicitSplatRef> RequestedSplats = {},
			TArray<FExplicitSplatRun> RequestedRuns = {});
		void TryPublishBoundedFrontier_RenderThread(uint32 UploadEpochValue);
		bool BuildActivePhysicalIndicesGPU_RenderThread(
			uint8 PublishIndexBufferSlot,
			TConstArrayView<FUintVector4> PageDescriptors);
		bool BuildDemandViews_RenderThread(
			const FSceneViewFamily& ViewFamily,
			const FTransform& LocalToWorld,
			double SplatScale,
			uint64 StableCaptureIdentity,
			TArray<FExactHQPageDemandView>& OutViews) const;
		void MarkUploadFailure(const FString& Error, uint32 UploadEpochValue);
		void StartTreeV3BlockLoads_RenderThread(TConstArrayView<uint32> BlockIds);
		bool AllocateTreeGPUBlockNodes_RenderThread(uint32 BlockId, uint32 NodeCount, uint32& OutNodeBase);
		void FreeTreeGPUBlockNodes_RenderThread(FRHICommandListImmediate& RHICmdList, uint32 BlockId);
		void TryStartTreeGPUPagePrefetch_RenderThread();

		struct FTreeGPUBlockFreeRange
		{
			uint32 FirstNode = 0;
			uint32 NodeCount = 0;
		};

		struct FRetiredFrontier
		{
			uint64 PinGeneration = 0;
			uint8 ActiveIndexBufferSlot = 0;
			FGPUFenceRHIRef LastUseFence;
		};
		struct FRecentViewDemand
		{
			uint64 LastSeenFrame = 0;
			double LastSeenSeconds = 0.0;
			TArray<uint64> PageIds;
			TArray<FExplicitSplatRef> SplatRefs;
			TArray<FExplicitSplatRun> SplatRuns;
			bool bSparse = false;
			bool bAllowPublishedFrontierForCapture = false;
		};

		FManifest Manifest;
		/** Immutable topology plus render-thread cache for Exact-HQ demand queries. */
		mutable FPageDemandHierarchy DemandHierarchy;
		FBox SceneBounds = FBox(ForceInit);
		uint32 TotalSplatCount = 0;
		uint32 PageSlotCapacity = 0;
		uint32 SourceCapacity = 0;
		uint32 ActiveIndexCapacity = 0;
		uint32 ActivePageDescriptorCapacity = 0;
		uint32 PhysicalSlotCount = 0;
		uint8 ActiveIndexBufferCount = 1;
		uint64 SceneBudgetIdentity = 0;
		uint64 SceneBudgetOwnerIdentity = 0;
		bool bSceneBudgetRegistered = false;
		uint8 PublishedActiveIndexBufferSlot = 0;
		bool bInitialized = false;
		bool bGPUBuffersCreated = false;
		bool bBoundedStreamingEnabled = false;
		TArray<NanoGS::Tree::FGPUNodeRecord> TreeGPUNodeUploadData;
		TUniquePtr<NanoGS::TreeV3::FManifest> TreeV3Manifest;
		FBufferRHIRef TreeGPUNodeBuffer;
		FShaderResourceViewRHIRef TreeGPUNodeBufferSRV;
		TSharedPtr<FSharedTreeGPUResources, ESPMode::ThreadSafe> SharedTreeGPUResources;
		uint32 TreeGPUNodeCount = 0;
		uint32 TreeGPURootNode = 0;
		uint32 TreeGPUFrontierCapacity = 0;
		uint8 TreeGPUFrontierReadSlot = 0;
		FBufferRHIRef TreeGPUFrontierBuffers[2];
		FShaderResourceViewRHIRef TreeGPUFrontierSRVs[2];
		FUnorderedAccessViewRHIRef TreeGPUFrontierUAVs[2];
		FBufferRHIRef TreeGPUFrontierCountBuffers[2];
		FShaderResourceViewRHIRef TreeGPUFrontierCountSRVs[2];
		FUnorderedAccessViewRHIRef TreeGPUFrontierCountUAVs[2];
		FBufferRHIRef TreeGPUPhysicalIndexBuffers[2];
		FShaderResourceViewRHIRef TreeGPUPhysicalIndexSRVs[2];
		FUnorderedAccessViewRHIRef TreeGPUPhysicalIndexUAVs[2];
		// Build slots above are overwritten by every refinement pass. Published
		// indices live in a separate immutable display buffer so a new scratch
		// traversal can never corrupt the last complete frontier.
		FBufferRHIRef TreeGPUPublishedPhysicalIndexBuffer;
		FShaderResourceViewRHIRef TreeGPUPublishedPhysicalIndexSRV;
		TUniquePtr<FRHIGPUBufferReadback> TreeGPUFrontierStatusReadback;
		uint8 TreeGPUFrontierStatusSlot = 0;
		uint8 TreeGPUPublishedPhysicalIndexSlot = 0;
		bool bTreeGPUFrontierStatusReadbackPending = false;
		bool bTreeGPUFrontierRefinementPending = false;
		uint32 TreeGPUFrontierPriorityPass = 0;
		float TreeGPUFrontierPriorityThresholdPixels = 0.0f;
		float TreeGPUFrontierLastStableThresholdPixels = 0.0f;
		float TreeGPUFrontierOverflowThresholdPixels = 0.0f;
		std::atomic<bool> bTreeGPUFrontierPublished{false};
		std::atomic<uint32> TreeGPUActiveSplatCount{0};
		FBufferRHIRef TreeGPUFrontierIndirectArgsBuffer;
		FUnorderedAccessViewRHIRef TreeGPUFrontierIndirectArgsUAV;
		FBufferRHIRef TreeGPUBlockRequestBuffer;
		FUnorderedAccessViewRHIRef TreeGPUBlockRequestUAV;
		FBufferRHIRef TreeGPUBlockRequestBitsBuffer;
		FUnorderedAccessViewRHIRef TreeGPUBlockRequestBitsUAV;
		TUniquePtr<FRHIGPUBufferReadback> TreeGPUBlockRequestReadback;
		TArray<uint32> PendingTreeGPUBlockRequests;
		uint32 TreeGPUBlockRequestCapacity = 0;
		bool bTreeGPUBlockRequestReadbackPending = false;
		uint32 LastLoggedTreeGPUBlockRequestCount = MAX_uint32;
		uint32 LastLoggedTreeGPUFrontierCount = MAX_uint32;
		uint32 LastLoggedTreeGPUFrontierFlags = MAX_uint32;
		TArray<uint64> TreeGPUPublishedPageIds;
		uint64 TreeGPURefinementGeneration = 0;
		double TreeGPURefinementStartSeconds = 0.0;
		uint32 TreeGPURefinementDispatchCount = 0;
		uint32 TreeGPUStatusSampleCount = 0;
		uint32 TreeGPUStatusWaitFrames = 0;
		uint32 TreeGPUPageReadbackWaitFrames = 0;
		uint32 TreeGPUBlockReadbackWaitFrames = 0;
		uint32 TreeGPUPriorityRestartCount = 0;
		FBufferRHIRef TreeGPUBlockNodeBuffer;
		FShaderResourceViewRHIRef TreeGPUBlockNodeBufferSRV;
		FBufferRHIRef TreeGPUBlockResidencyBuffer;
		FShaderResourceViewRHIRef TreeGPUBlockResidencyBufferSRV;
		FBufferRHIRef TreeGPUPageResidencyBuffer;
		FShaderResourceViewRHIRef TreeGPUPageResidencyBufferSRV;
		FBufferRHIRef TreeGPUPageRequestBuffer;
		FUnorderedAccessViewRHIRef TreeGPUPageRequestUAV;
		FBufferRHIRef TreeGPUPageRequestBitsBuffer;
		FUnorderedAccessViewRHIRef TreeGPUPageRequestBitsUAV;
		TUniquePtr<FRHIGPUBufferReadback> TreeGPUPageRequestReadback;
		TArray<uint32> PendingTreeGPUPageRequests;
		uint32 TreeGPUPageAddressCapacity = 0;
		uint32 TreeGPUPageRequestCapacity = 0;
		uint32 LastTreeGPUPageResidencyGeneration = MAX_uint32;
		bool bTreeGPUPageRequestReadbackPending = false;
		uint32 TreeGPUBlockPoolNodeCapacity = 0;
		uint32 TreeGPUBlockMaxNodeCount = 0;
		TArray<int32> TreeGPUBlockToNodeBase;
		TArray<uint32> TreeGPUBlockAllocationNodeCounts;
		TArray<uint64> TreeGPUBlockLastRequestedFeedbackEpoch;
		uint64 TreeGPUBlockFeedbackEpoch = 0;
		TArray<FTreeGPUBlockFreeRange> TreeGPUBlockFreeRanges;
		TArray<FVector4f> TreeGPUBlockCenterAndFeatureRadius;
		FVector3f TreeGPULastViewOriginLocal = FVector3f::ZeroVector;
		float TreeGPULastProjectionScale = 0.0f;
		TSet<uint32> TreeGPUBlocksInFlight;

		/** Render-thread-only residency/frontier state. Async workers only carry tokenized copies. */
		FPageResidentPool ResidentPool;
		TArray<uint64> PublishedPageIds;
		TArray<uint64> TargetPageIds;
		TArray<FExplicitSplatRef> PublishedSplatRefs;
		TArray<FExplicitSplatRun> PublishedSplatRuns;
		TArray<FExplicitSplatRef> TargetSplatRefs;
		TArray<FExplicitSplatRun> TargetSplatRuns;
		TMap<uint64, FRecentViewDemand> RecentViewDemands;
		/** Tree v2 stores exact leaves first; later records are Spark LoD parents. */
		uint64 TreeExactLeafCount = 0;
		bool bTreeLODSource = false;
		/** Last exact requirement time per page; retained pages are conservative extras only. */
		TMap<uint64, double> LastRequiredPageSeconds;
		TArray<FPageLoadOperation> InFlightLoadOperations;
		TArray<FRetiredFrontier> RetiredFrontiers;
		FGPUFenceRHIRef CurrentFrontierLastUseFence;
		FBufferRHIRef ActivePhysicalIndexBuffers[4];
		FShaderResourceViewRHIRef ActivePhysicalIndexBufferSRVs[4];
		FUnorderedAccessViewRHIRef ActivePhysicalIndexBufferUAVs[4];
		FBufferRHIRef ActivePageDescriptorBuffer;
		FShaderResourceViewRHIRef ActivePageDescriptorBufferSRV;
		bool bPublicationPendingOnIndexFence = false;
		uint32 PendingPublicationUploadEpoch = 0;
		uint64 NextRequestGeneration = 1;
		uint64 CurrentTransitionPinGeneration = 0;
		uint64 LastDemandAttemptFrame = MAX_uint64;
		double LastBoundedDemandApplySeconds = 0.0;
		double LastCaptureBudgetWarningSeconds = -1.0;
		uint32 PublishedActiveSplatCount = 0;
		uint8 PublishedCaptureActiveIndexBufferSlot = 2;
		uint32 PublishedCaptureActiveSplatCount = 0;
		TArray<FExplicitSplatRef> PublishedCaptureSplatRefs;
		TArray<FExplicitSplatRun> PublishedCaptureSplatRuns;
		FGPUFenceRHIRef CaptureFrontierLastUseFence;
		bool bCaptureFrontierHasBeenDrawn = false;
		bool bCaptureFrontierFenceUnavailable = false;
		bool bCurrentFrontierHasBeenDrawn = false;
		bool bCurrentFrontierFenceUnavailable = false;
		bool bPublishingIntermediateTreeFrontier = false;
		bool bPublishingCaptureTreeFrontier = false;
		bool bPermanentFenceSafetyFailure = false;

		std::atomic<uint32> ActiveSplatCount{0};
		std::atomic<uint32> ResidentGeneration{0};
		std::atomic<uint32> CaptureActiveSplatCount{0};
		std::atomic<uint32> CaptureResidentGeneration{0};
		std::atomic<uint32> PublishedCapturePageCount{0};
		std::atomic<bool> bCaptureFrontierReady{false};
		std::atomic<uint32> PageResidencyGeneration{0};
		std::atomic<uint32> LoadedPageCount{0};
		std::atomic<uint32> RequestedPageCount{0};
		std::atomic<uint32> PublishedPageCount{0};
		std::atomic<uint8> StreamingState{static_cast<uint8>(EExactHQStreamingState::AllResident)};
		std::atomic<bool> bUploadComplete{false};
		std::atomic<bool> bUploadFailed{false};
		std::atomic<bool> bPublishedFrontierCaptureSafeDuringTransition{false};
		std::atomic<bool> bStopRequested{false};
		/** Invalidates workers and queued render commands across resource rebuilds. */
		std::atomic<uint32> UploadEpoch{0};
	};
}
