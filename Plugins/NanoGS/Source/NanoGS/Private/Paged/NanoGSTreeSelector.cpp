// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSTreeSelector.h"

namespace NanoGS::Tree
{
namespace
{
	struct FCandidate
	{
		uint32 NodeIndex = 0;
		float Score = 0.0f;

		bool operator<(const FCandidate& Other) const
		{
			// Unreal's TArray heap is a min-heap with TLess. Reverse the comparison
			// so the node with the largest projected error is refined first, matching
			// Spark's BinaryHeap traversal.
			return Score > Other.Score || (Score == Other.Score && NodeIndex < Other.NodeIndex);
		}
	};

	class FCandidateQueue
	{
		struct FSecondaryBucket
		{
			TArray<TArray<FCandidate>> Buckets;
			TBitArray<> HeapifiedBuckets;
			uint32 MaxNonEmptyBucket = 0;

			FSecondaryBucket()
			{
				Buckets.SetNum(SecondaryBucketCount);
				HeapifiedBuckets.Init(false, SecondaryBucketCount);
			}
		};

	public:
		explicit FCandidateQueue(const bool bInUseHierarchicalHeap, const int32 ReserveCount)
			: bUseHierarchicalHeap(bInUseHierarchicalHeap)
		{
			if (bUseHierarchicalHeap)
			{
				Buckets.SetNum(PrimaryBucketCount);
			}
			else
			{
				GlobalHeap.Reserve(ReserveCount);
			}
		}

		bool IsEmpty() const
		{
			return bUseHierarchicalHeap ? CandidateCount == 0 : GlobalHeap.IsEmpty();
		}

		void Push(const FCandidate& Candidate)
		{
			if (!bUseHierarchicalHeap)
			{
				GlobalHeap.HeapPush(Candidate);
				return;
			}
			uint32 ScoreBits = 0;
			FMemory::Memcpy(&ScoreBits, &Candidate.Score, sizeof(ScoreBits));
			const uint32 PrimaryIndex = FMath::Min(ScoreBits >> 16u, PrimaryBucketCount - 1u);
			const uint32 SecondaryIndex = (ScoreBits >> 8u) & 0xffu;
			if (!Buckets[PrimaryIndex].IsValid())
			{
				Buckets[PrimaryIndex] = MakeUnique<FSecondaryBucket>();
			}
			FSecondaryBucket& Primary = *Buckets[PrimaryIndex];
			TArray<FCandidate>& Bucket = Primary.Buckets[SecondaryIndex];
			if (Primary.HeapifiedBuckets[SecondaryIndex])
			{
				Bucket.HeapPush(Candidate);
			}
			else
			{
				Bucket.Add(Candidate);
			}
			Primary.MaxNonEmptyBucket = FMath::Max(Primary.MaxNonEmptyBucket, SecondaryIndex);
			MaxNonEmptyBucket = FMath::Max(MaxNonEmptyBucket, PrimaryIndex);
			++CandidateCount;
		}

		const FCandidate& Peek()
		{
			if (!bUseHierarchicalHeap)
			{
				return GlobalHeap[0];
			}
			PrepareTopBucket();
			FSecondaryBucket& Primary = *Buckets[MaxNonEmptyBucket];
			return Primary.Buckets[Primary.MaxNonEmptyBucket][0];
		}

		FCandidate Pop()
		{
			FCandidate Result;
			if (!bUseHierarchicalHeap)
			{
				GlobalHeap.HeapPop(Result, EAllowShrinking::No);
				return Result;
			}
			PrepareTopBucket();
			FSecondaryBucket& Primary = *Buckets[MaxNonEmptyBucket];
			TArray<FCandidate>& Bucket = Primary.Buckets[Primary.MaxNonEmptyBucket];
			Bucket.HeapPop(Result, EAllowShrinking::No);
			--CandidateCount;
			if (Bucket.IsEmpty())
			{
				Primary.HeapifiedBuckets[Primary.MaxNonEmptyBucket] = false;
				while (Primary.MaxNonEmptyBucket > 0 &&
					Primary.Buckets[Primary.MaxNonEmptyBucket].IsEmpty())
				{
					--Primary.MaxNonEmptyBucket;
				}
				if (Primary.Buckets[Primary.MaxNonEmptyBucket].IsEmpty())
				{
					Buckets[MaxNonEmptyBucket].Reset();
					while (MaxNonEmptyBucket > 0 && !Buckets[MaxNonEmptyBucket].IsValid())
					{
						--MaxNonEmptyBucket;
					}
				}
			}
			return Result;
		}

		void AppendRemaining(TFunctionRef<void(uint32)> AppendNode) const
		{
			if (!bUseHierarchicalHeap)
			{
				for (const FCandidate& Candidate : GlobalHeap)
				{
					AppendNode(Candidate.NodeIndex);
				}
				return;
			}
			for (const TUniquePtr<FSecondaryBucket>& Primary : Buckets)
			{
				if (!Primary.IsValid())
					continue;
				for (const TArray<FCandidate>& Bucket : Primary->Buckets)
				{
					for (const FCandidate& Candidate : Bucket)
					{
						AppendNode(Candidate.NodeIndex);
					}
				}
			}
		}

	private:
		static constexpr uint32 PrimaryBucketCount = 1u << 15u;
		static constexpr uint32 SecondaryBucketCount = 1u << 8u;

		void PrepareTopBucket()
		{
			while (MaxNonEmptyBucket > 0 && !Buckets[MaxNonEmptyBucket].IsValid())
			{
				--MaxNonEmptyBucket;
			}
			FSecondaryBucket& Primary = *Buckets[MaxNonEmptyBucket];
			while (Primary.MaxNonEmptyBucket > 0 &&
				Primary.Buckets[Primary.MaxNonEmptyBucket].IsEmpty())
			{
				--Primary.MaxNonEmptyBucket;
			}
			if (!Primary.HeapifiedBuckets[Primary.MaxNonEmptyBucket])
			{
				Primary.Buckets[Primary.MaxNonEmptyBucket].Heapify();
				Primary.HeapifiedBuckets[Primary.MaxNonEmptyBucket] = true;
			}
		}

		bool bUseHierarchicalHeap = false;
		TArray<FCandidate> GlobalHeap;
		TArray<TUniquePtr<FSecondaryBucket>> Buckets;
		uint32 MaxNonEmptyBucket = 0;
		uint32 CandidateCount = 0;
	};

	bool ResolveRenderFrontier(
		uint32 NodeIndex,
		const TSet<uint32>& TargetNodes,
		const TMap<uint32, TArray<uint32>>& RefinedChildren,
		TFunctionRef<bool(uint32)> IsNodeResident,
		TArray<uint32>& InOutRenderNodes)
	{
		if (TargetNodes.Contains(NodeIndex))
		{
			if (!IsNodeResident(NodeIndex))
				return false;
			InOutRenderNodes.Add(NodeIndex);
			return true;
		}
		const TArray<uint32>* Children = RefinedChildren.Find(NodeIndex);
		if (Children == nullptr)
			return false;
		const int32 PreviousCount = InOutRenderNodes.Num();
		for (const uint32 ChildIndex : *Children)
		{
			if (!ResolveRenderFrontier(ChildIndex, TargetNodes, RefinedChildren, IsNodeResident, InOutRenderNodes))
			{
				InOutRenderNodes.SetNum(PreviousCount, EAllowShrinking::No);
				if (!IsNodeResident(NodeIndex))
					return false;
				InOutRenderNodes.Add(NodeIndex);
				return true;
			}
		}
		return true;
	}
}

FSelection FSelector::Select(
	const FManifest& Manifest,
	const FSelectionConfig& Config,
	TFunctionRef<FNodeObservation(uint32)> ObserveNode,
	TFunctionRef<bool(uint32)> IsNodeResident) const
{
	FSelection Result;
	if (Config.CancelFlag != nullptr && Config.CancelFlag->load(std::memory_order_acquire))
	{
		Result.bCancelled = true;
		return Result;
	}
	if (Manifest.Nodes.IsEmpty() || Manifest.Header.RootNode >= static_cast<uint64>(Manifest.Nodes.Num()))
		return Result;

	const uint32 RootIndex = static_cast<uint32>(Manifest.Header.RootNode);
	const FNodeObservation RootObservation = ObserveNode(RootIndex);
	if (!RootObservation.bVisible)
		return Result;

	const uint32 NodeBudget = FMath::Max(1u, Config.MaxActiveNodes);
	const float SplitThreshold = FMath::Max(0.0f, Config.SplitThresholdPx);
	if (!Config.bBuildTransitionData)
	{
		// Spark's runtime traversal keeps only a max-priority frontier and the
		// final output. Avoid building a multi-million-entry TSet/TMap and avoid
		// sorting the output: every tree node is reached through exactly one
		// parent, so the frontier is already unique.
		const int32 HeapReserveCount = static_cast<int32>(FMath::Min<uint64>(
			NodeBudget, Manifest.Header.InternalNodeCount));
		FCandidateQueue Frontier(Config.bUseHierarchicalHeap, HeapReserveCount);
		Result.TargetNodes.Reserve(static_cast<int32>(FMath::Min<uint32>(NodeBudget, MAX_int32)));
		if (Config.bBuildPageOutputMask)
		{
			Result.SelectedPageOutputs.Init(false, Manifest.Nodes.Num());
		}
		TBitArray<> ResidentFrontier;
		TBitArray<> ResidentPageOutputs;
		if (Config.bBuildResidentProgressiveData)
		{
			if (Config.bBuildResidentRefinementTopology)
			{
				Result.RefinedNodes.Init(false, Manifest.Nodes.Num());
				Result.DemandedNodes.Init(false, Manifest.Nodes.Num());
				Result.DemandedNodes[RootIndex] = true;
			}
			ResidentFrontier.Init(false, Manifest.Nodes.Num());
			ResidentPageOutputs.Init(false, Manifest.Nodes.Num());
			Result.bRootReady = IsNodeResident(RootIndex);
			if (Result.bRootReady)
			{
				ResidentFrontier[RootIndex] = true;
				const FNode& RootNode = Manifest.Nodes[RootIndex];
				const uint64 RootPageOutputIndex =
					static_cast<uint64>(RootNode.PageId) * Manifest.Header.PagePoints + RootNode.PageLocal;
				check(RootPageOutputIndex < static_cast<uint64>(ResidentPageOutputs.Num()));
				ResidentPageOutputs[static_cast<int32>(RootPageOutputIndex)] = true;
			}
		}
		const auto AppendNodeToSelection = [&Manifest](FSelection& Selection, const uint32 NodeIndex)
		{
			Selection.TargetNodes.Add(NodeIndex);
			const FNode& Node = Manifest.Nodes[NodeIndex];
			Selection.ExactLeafNodeCount += Node.IsExactLeaf() ? 1 : 0;
			Selection.MinSelectedDepth = Selection.MinSelectedDepth < 0
				? static_cast<int32>(Node.Depth)
				: FMath::Min(Selection.MinSelectedDepth, static_cast<int32>(Node.Depth));
			Selection.MaxSelectedDepth = FMath::Max(
				Selection.MaxSelectedDepth, static_cast<int32>(Node.Depth));
			if (!Selection.SelectedPageOutputs.IsEmpty())
			{
				const uint64 PageOutputIndex =
					static_cast<uint64>(Node.PageId) * Manifest.Header.PagePoints + Node.PageLocal;
				check(PageOutputIndex < static_cast<uint64>(Selection.SelectedPageOutputs.Num()));
				Selection.SelectedPageOutputs[static_cast<int32>(PageOutputIndex)] = true;
			}
		};
		const auto AppendTargetNode = [&Result, &AppendNodeToSelection](const uint32 NodeIndex)
		{
			AppendNodeToSelection(Result, NodeIndex);
		};
		const auto SnapshotResidentProgressiveData =
			[&ResidentFrontier, &ResidentPageOutputs, &Result](FSelection& Selection)
		{
			if (ResidentFrontier.IsEmpty())
			{
				return;
			}
			Selection.RenderPageOutputs = ResidentPageOutputs;
			Selection.RenderNodes.Reset(ResidentFrontier.CountSetBits());
			for (TConstSetBitIterator<> It(ResidentFrontier); It; ++It)
			{
				Selection.RenderNodes.Add(static_cast<uint32>(It.GetIndex()));
			}
			Selection.bRootReady = Result.bRootReady;
		};
		Frontier.Push({RootIndex, RootObservation.ProjectedRadiusPx});
		uint32 ActiveNodeCount = 1;
		uint32 ProcessedCandidateCount = 0;
		bool bProgressiveCheckpointEmitted = false;

		while (!Frontier.IsEmpty())
		{
			if ((ProcessedCandidateCount++ & 4095u) == 0u && Config.CancelFlag != nullptr &&
				Config.CancelFlag->load(std::memory_order_relaxed))
			{
				Result = FSelection();
				Result.bCancelled = true;
				return Result;
			}
			const FCandidate Candidate = Frontier.Peek();
			if (Candidate.Score <= SplitThreshold)
				break;

			const FNode& Parent = Manifest.Nodes[Candidate.NodeIndex];
			if (!Parent.IsInternal() || Parent.ChildCount == 0)
			{
				const FCandidate Leaf = Frontier.Pop();
				AppendTargetNode(Leaf.NodeIndex);
				continue;
			}

			TArray<FCandidate, TInlineAllocator<8>> VisibleChildren;
			for (uint32 Offset = 0; Offset < Parent.ChildCount; ++Offset)
			{
				const uint32 ChildIndex = Parent.FirstChild + Offset;
				const FNodeObservation Observation = ObserveNode(ChildIndex);
				if (Observation.bVisible)
					VisibleChildren.Add({ChildIndex, Observation.ProjectedRadiusPx});
			}

			const uint64 NewActiveNodeCount =
				static_cast<uint64>(ActiveNodeCount) - 1u + static_cast<uint64>(VisibleChildren.Num());
			if (!bProgressiveCheckpointEmitted && Config.ProgressiveCallback &&
				Config.ProgressiveNodeBudget > 0 && Config.ProgressiveNodeBudget < NodeBudget &&
				NewActiveNodeCount > Config.ProgressiveNodeBudget)
			{
				FSelection ProgressiveSelection;
				ProgressiveSelection.TargetNodes = Result.TargetNodes;
				ProgressiveSelection.TargetNodes.Reserve(static_cast<int32>(ActiveNodeCount));
				ProgressiveSelection.SelectedPageOutputs = Result.SelectedPageOutputs;
				ProgressiveSelection.ExactLeafNodeCount = Result.ExactLeafNodeCount;
				ProgressiveSelection.MinSelectedDepth = Result.MinSelectedDepth;
				ProgressiveSelection.MaxSelectedDepth = Result.MaxSelectedDepth;
				Frontier.AppendRemaining(
					[&ProgressiveSelection, &AppendNodeToSelection](const uint32 NodeIndex)
					{
						AppendNodeToSelection(ProgressiveSelection, NodeIndex);
					});
				ProgressiveSelection.RefinedParentCount = Result.RefinedParentCount;
				ProgressiveSelection.bBudgetLimited = true;
				ProgressiveSelection.bRootReady = Result.bRootReady;
				SnapshotResidentProgressiveData(ProgressiveSelection);
				Config.ProgressiveCallback(MoveTemp(ProgressiveSelection));
				bProgressiveCheckpointEmitted = true;
				if (!Config.bBuildResidentRefinementTopology)
				{
					// The production path needs resident tracking only for the fast first
					// publication. Continuing millions of residency probes after this point
					// delays the unchanged final 10M cut without improving its quality.
					ResidentFrontier.Reset();
					ResidentPageOutputs.Reset();
				}
			}
			if (NewActiveNodeCount > NodeBudget)
			{
				// Match Spark: the highest-error split does not fit, so stop the
				// whole refinement loop instead of refining lower-priority nodes.
				Result.bBudgetLimited = true;
				break;
			}

			const FCandidate RefinedParent = Frontier.Pop();
			ActiveNodeCount = static_cast<uint32>(NewActiveNodeCount);
			++Result.RefinedParentCount;
			if (!Result.RefinedNodes.IsEmpty())
			{
				Result.RefinedNodes[RefinedParent.NodeIndex] = true;
				for (const FCandidate& Child : VisibleChildren)
				{
					Result.DemandedNodes[Child.NodeIndex] = true;
				}
			}

			if (!ResidentFrontier.IsEmpty() && ResidentFrontier[RefinedParent.NodeIndex])
			{
				bool bAllVisibleChildrenResident = true;
				for (const FCandidate& Child : VisibleChildren)
				{
					if (!IsNodeResident(Child.NodeIndex))
					{
						bAllVisibleChildrenResident = false;
						break;
					}
				}
				if (bAllVisibleChildrenResident)
				{
					ResidentFrontier[RefinedParent.NodeIndex] = false;
					const FNode& RefinedNode = Manifest.Nodes[RefinedParent.NodeIndex];
					const uint64 ParentPageOutputIndex =
						static_cast<uint64>(RefinedNode.PageId) * Manifest.Header.PagePoints +
						RefinedNode.PageLocal;
					check(ParentPageOutputIndex < static_cast<uint64>(ResidentPageOutputs.Num()));
					ResidentPageOutputs[static_cast<int32>(ParentPageOutputIndex)] = false;
					for (const FCandidate& Child : VisibleChildren)
					{
						ResidentFrontier[Child.NodeIndex] = true;
						const FNode& ChildNode = Manifest.Nodes[Child.NodeIndex];
						const uint64 ChildPageOutputIndex =
							static_cast<uint64>(ChildNode.PageId) * Manifest.Header.PagePoints +
							ChildNode.PageLocal;
						check(ChildPageOutputIndex < static_cast<uint64>(ResidentPageOutputs.Num()));
						ResidentPageOutputs[static_cast<int32>(ChildPageOutputIndex)] = true;
					}
				}
			}
			for (const FCandidate& Child : VisibleChildren)
			{
				const FNode& ChildNode = Manifest.Nodes[Child.NodeIndex];
				// Leaves can never be refined. Keeping them in the global priority
				// queue only adds one heap push and one heap pop per rendered splat.
				// Large trees contain millions of leaves, so append them directly;
				// this is output-equivalent and leaves the heap for internal nodes.
				if (!ChildNode.IsInternal() || ChildNode.ChildCount == 0 || Child.Score <= SplitThreshold)
					AppendTargetNode(Child.NodeIndex);
				else
					Frontier.Push(Child);
			}
		}

		// The caller consumes this frontier as a set and builds its own packed-key
		// ordering. Draining a million-entry heap only to discard heap order costs
		// O(N log N); copying the remaining unique frontier is exactly equivalent.
		Frontier.AppendRemaining(AppendTargetNode);
		SnapshotResidentProgressiveData(Result);
		return Result;
	}

	TSet<uint32> TargetSet;
	TMap<uint32, TArray<uint32>> RefinedChildren;
	TArray<FCandidate> Candidates;
	TargetSet.Add(RootIndex);
	Candidates.HeapPush({RootIndex, RootObservation.ProjectedRadiusPx});

	while (!Candidates.IsEmpty())
	{
		FCandidate Candidate;
		Candidates.HeapPop(Candidate, EAllowShrinking::No);
		if (Candidate.Score <= SplitThreshold)
			break;
		const FNode& Parent = Manifest.Nodes[Candidate.NodeIndex];
		if (!Parent.IsInternal() || Parent.ChildCount == 0)
			continue;

		TArray<uint32> VisibleChildren;
		TArray<FNodeObservation> ChildObservations;
		VisibleChildren.Reserve(Parent.ChildCount);
		ChildObservations.Reserve(Parent.ChildCount);
		for (uint32 Offset = 0; Offset < Parent.ChildCount; ++Offset)
		{
			const uint32 ChildIndex = Parent.FirstChild + Offset;
			const FNodeObservation Observation = ObserveNode(ChildIndex);
			if (Observation.bVisible)
			{
				VisibleChildren.Add(ChildIndex);
				ChildObservations.Add(Observation);
			}
		}
		if (VisibleChildren.IsEmpty())
		{
			TargetSet.Remove(Candidate.NodeIndex);
			continue;
		}
		const uint32 NewTargetCount = TargetSet.Num() - 1 + VisibleChildren.Num();
		if (NewTargetCount > NodeBudget)
		{
			Result.bBudgetLimited = true;
			continue;
		}
		TargetSet.Remove(Candidate.NodeIndex);
		if (Config.bBuildTransitionData)
			RefinedChildren.Add(Candidate.NodeIndex, VisibleChildren);
		for (int32 Index = 0; Index < VisibleChildren.Num(); ++Index)
		{
			TargetSet.Add(VisibleChildren[Index]);
			Candidates.HeapPush({VisibleChildren[Index], ChildObservations[Index].ProjectedRadiusPx});
		}
	}

	Result.TargetNodes = TargetSet.Array();
	Result.TargetNodes.Sort();
	for (const uint32 NodeIndex : Result.TargetNodes)
	{
		const FNode& Node = Manifest.Nodes[NodeIndex];
		Result.ExactLeafNodeCount += Node.IsExactLeaf() ? 1 : 0;
		Result.MinSelectedDepth = Result.MinSelectedDepth < 0
			? static_cast<int32>(Node.Depth)
			: FMath::Min(Result.MinSelectedDepth, static_cast<int32>(Node.Depth));
		Result.MaxSelectedDepth = FMath::Max(
			Result.MaxSelectedDepth, static_cast<int32>(Node.Depth));
	}
	TSet<uint32> RequestedSet = TargetSet;
	for (const TPair<uint32, TArray<uint32>>& Pair : RefinedChildren)
		RequestedSet.Add(Pair.Key);
	Result.RequestedNodes = RequestedSet.Array();
	Result.RequestedNodes.Sort();
	Result.RefinedParentCount = RefinedChildren.Num();
	Result.bRootReady = IsNodeResident(RootIndex);
	ResolveRenderFrontier(RootIndex, TargetSet, RefinedChildren, IsNodeResident, Result.RenderNodes);
	Result.RenderNodes.Sort();
	return Result;
}

void FSelector::ResolveTransitionFrontier(
	const FManifest& Manifest,
	TConstArrayView<uint32> ParentIndices,
	TConstArrayView<uint32> TargetNodes,
	TFunctionRef<bool(uint32)> IsNodeResident,
	TArray<uint32>& OutRequestedNodes,
	TArray<uint32>& OutRenderNodes)
{
	OutRequestedNodes.Reset();
	OutRenderNodes.Reset();
	if (Manifest.Nodes.IsEmpty() || ParentIndices.Num() != Manifest.Nodes.Num() ||
		Manifest.Header.RootNode >= static_cast<uint64>(Manifest.Nodes.Num()))
		return;

	TBitArray<> IsTarget(false, Manifest.Nodes.Num());
	TBitArray<> HasDemand(false, Manifest.Nodes.Num());
	for (const uint32 Target : TargetNodes)
	{
		if (!Manifest.Nodes.IsValidIndex(static_cast<int32>(Target)))
			continue;
		IsTarget[Target] = true;
		uint32 Node = Target;
		while (Node != MAX_uint32 && !HasDemand[Node])
		{
			HasDemand[Node] = true;
			Node = ParentIndices[Node];
		}
	}
	OutRequestedNodes.Reserve(HasDemand.CountSetBits());
	for (TConstSetBitIterator<> It(HasDemand); It; ++It)
		OutRequestedNodes.Add(static_cast<uint32>(It.GetIndex()));

	TFunction<bool(uint32)> Resolve;
	Resolve = [&Manifest, &IsTarget, &HasDemand, &IsNodeResident, &OutRenderNodes, &Resolve](uint32 NodeIndex)
	{
		if (IsTarget[NodeIndex])
		{
			if (!IsNodeResident(NodeIndex))
				return false;
			OutRenderNodes.Add(NodeIndex);
			return true;
		}
		const FNode& Node = Manifest.Nodes[NodeIndex];
		const int32 PreviousCount = OutRenderNodes.Num();
		bool bHasDemandedChild = false;
		for (uint32 Offset = 0; Offset < Node.ChildCount; ++Offset)
		{
			const uint32 Child = Node.FirstChild + Offset;
			if (!HasDemand[Child])
				continue;
			bHasDemandedChild = true;
			if (!Resolve(Child))
			{
				OutRenderNodes.SetNum(PreviousCount, EAllowShrinking::No);
				if (!IsNodeResident(NodeIndex))
					return false;
				OutRenderNodes.Add(NodeIndex);
				return true;
			}
		}
		return bHasDemandedChild;
	};

	Resolve(static_cast<uint32>(Manifest.Header.RootNode));
}

TArray<uint32> FSelector::MergeFrontiers(
	const FManifest& Manifest,
	TConstArrayView<uint32> ParentIndices,
	TConstArrayView<const TArray<uint32>*> Frontiers)
{
	TArray<uint32> Result;
	if (Manifest.Nodes.IsEmpty() || ParentIndices.Num() != Manifest.Nodes.Num())
		return Result;
	TSet<uint32> DirectDemand;
	TSet<uint32> HasDemandInSubtree;
	for (const TArray<uint32>* Frontier : Frontiers)
	{
		if (Frontier == nullptr)
			continue;
		for (const uint32 NodeIndex : *Frontier)
		{
			if (!Manifest.Nodes.IsValidIndex(static_cast<int32>(NodeIndex)))
				continue;
			DirectDemand.Add(NodeIndex);
			uint32 Ancestor = NodeIndex;
			while (Ancestor != MAX_uint32 && !HasDemandInSubtree.Contains(Ancestor))
			{
				HasDemandInSubtree.Add(Ancestor);
				Ancestor = ParentIndices[Ancestor];
			}
		}
	}
	TFunction<void(uint32, bool)> Visit;
	Visit = [&Manifest, &DirectDemand, &HasDemandInSubtree, &Result, &Visit](uint32 NodeIndex, bool bInheritedCoverage)
	{
		const FNode& Node = Manifest.Nodes[NodeIndex];
		const bool bCoverage = bInheritedCoverage || DirectDemand.Contains(NodeIndex);
		bool bHasDemandBelow = false;
		for (uint32 Offset = 0; Offset < Node.ChildCount; ++Offset)
			bHasDemandBelow |= HasDemandInSubtree.Contains(Node.FirstChild + Offset);
		if (!bHasDemandBelow)
		{
			if (bCoverage)
				Result.Add(NodeIndex);
			return;
		}
		for (uint32 Offset = 0; Offset < Node.ChildCount; ++Offset)
		{
			const uint32 ChildIndex = Node.FirstChild + Offset;
			if (HasDemandInSubtree.Contains(ChildIndex))
				Visit(ChildIndex, bCoverage);
			else if (bCoverage)
				Result.Add(ChildIndex);
		}
	};
	const uint32 RootIndex = static_cast<uint32>(Manifest.Header.RootNode);
	if (Manifest.Nodes.IsValidIndex(static_cast<int32>(RootIndex)) && HasDemandInSubtree.Contains(RootIndex))
		Visit(RootIndex, false);
	Result.Sort();
	return Result;
}
}
