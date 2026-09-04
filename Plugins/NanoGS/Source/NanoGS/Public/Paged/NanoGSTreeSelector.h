// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Paged/NanoGSTreeFormat.h"

namespace NanoGS::Tree
{
	struct FSelection;

	struct NANOGS_API FNodeObservation
	{
		float ProjectedRadiusPx = 0.0f;
		bool bVisible = false;
	};

	struct NANOGS_API FSelectionConfig
	{
		float SplitThresholdPx = 2.0f;
		uint32 MaxActiveNodes = 1000000;
		/** Build ancestor/requested and resident fallback frontiers for callers that consume them. */
		bool bBuildTransitionData = true;
		/** Use the exact two-level priority heap. It preserves full-float ordering while reducing heap depth. */
		bool bUseHierarchicalHeap = true;
		/** Build the physical page-output bit mask while the selected nodes are already cache-hot. */
		bool bBuildPageOutputMask = false;
		/**
		 * Track the accepted split topology and a resident-safe render cut while selecting.
		 * This is the Spark-style path: a parent is replaced only when every demanded child
		 * is resident, so missing pages never create holes or parent/child overlap.
		 */
		bool bBuildResidentProgressiveData = false;
		/** Keep split topology after the first checkpoint for experimental repeated local publication. */
		bool bBuildResidentRefinementTopology = false;
		/** Optional exact-budget checkpoint emitted from the same traversal before final refinement. */
		uint32 ProgressiveNodeBudget = 0;
		TFunction<void(FSelection&&)> ProgressiveCallback;
		/** Optional cooperative cancellation for expensive background selections. */
		const std::atomic<bool>* CancelFlag = nullptr;
	};

	struct NANOGS_API FSelection
	{
		TArray<uint32> TargetNodes;
		TArray<uint32> RenderNodes;
		TArray<uint32> RequestedNodes;
		TBitArray<> SelectedPageOutputs;
		TBitArray<> RenderPageOutputs;
		TBitArray<> RefinedNodes;
		TBitArray<> DemandedNodes;
		uint32 RefinedParentCount = 0;
		int32 ExactLeafNodeCount = 0;
		int32 MinSelectedDepth = -1;
		int32 MaxSelectedDepth = -1;
		bool bBudgetLimited = false;
		bool bRootReady = false;
		bool bCancelled = false;
	};

	class NANOGS_API FSelector
	{
	public:
		FSelection Select(
			const FManifest& Manifest,
			const FSelectionConfig& Config,
			TFunctionRef<FNodeObservation(uint32)> ObserveNode,
			TFunctionRef<bool(uint32)> IsNodeResident) const;

		/**
		 * Resolve one target frontier against current residency without overlap.
		 * RequestedNodes contains targets plus ancestors needed as lossless-coverage
		 * fallbacks; RenderNodes replaces a parent only when every demanded child
		 * subtree has a resident render frontier.
		 */
		static void ResolveTransitionFrontier(
			const FManifest& Manifest, TConstArrayView<uint32> ParentIndices,
			TConstArrayView<uint32> TargetNodes, TFunctionRef<bool(uint32)> IsNodeResident,
			TArray<uint32>& OutRequestedNodes, TArray<uint32>& OutRenderNodes);

		static TArray<uint32> MergeFrontiers(
			const FManifest& Manifest,
			TConstArrayView<uint32> ParentIndices,
			TConstArrayView<const TArray<uint32>*> Frontiers);
	};
}
