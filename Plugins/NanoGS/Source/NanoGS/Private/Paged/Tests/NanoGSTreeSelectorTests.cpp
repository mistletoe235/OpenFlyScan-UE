// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Paged/NanoGSTreeSelector.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace NanoGS::Tree::Tests
{
namespace
{
	FManifest MakeThreeNodeTree()
	{
		FManifest Manifest;
		Manifest.Header.NodeCount = 3;
		Manifest.Header.RootNode = 0;
		Manifest.Nodes.SetNum(3);
		Manifest.Nodes[0].FirstChild = 1;
		Manifest.Nodes[0].ChildCount = 2;
		Manifest.Nodes[0].Flags = ENodeFlags::Internal;
		Manifest.Nodes[0].SubtreeLeafCount = 2;
		Manifest.Nodes[1].Depth = 1;
		Manifest.Nodes[1].Flags = ENodeFlags::ExactLeaf;
		Manifest.Nodes[1].SubtreeLeafCount = 1;
		Manifest.Nodes[2].Depth = 1;
		Manifest.Nodes[2].Flags = ENodeFlags::ExactLeaf;
		Manifest.Nodes[2].SubtreeLeafCount = 1;
		return Manifest;
	}

	FManifest MakeMixedPriorityTree()
	{
		FManifest Manifest;
		Manifest.Header.NodeCount = 5;
		Manifest.Header.RootNode = 0;
		Manifest.Nodes.SetNum(5);
		Manifest.Nodes[0].FirstChild = 1;
		Manifest.Nodes[0].ChildCount = 2;
		Manifest.Nodes[0].Flags = ENodeFlags::Internal;
		Manifest.Nodes[1].Depth = 1;
		Manifest.Nodes[1].Flags = ENodeFlags::ExactLeaf;
		Manifest.Nodes[2].FirstChild = 3;
		Manifest.Nodes[2].ChildCount = 2;
		Manifest.Nodes[2].Depth = 1;
		Manifest.Nodes[2].Flags = ENodeFlags::Internal;
		Manifest.Nodes[3].Depth = 2;
		Manifest.Nodes[3].Flags = ENodeFlags::ExactLeaf;
		Manifest.Nodes[4].Depth = 2;
		Manifest.Nodes[4].Flags = ENodeFlags::ExactLeaf;
		return Manifest;
	}

	FManifest MakeBudgetBreakTree()
	{
		FManifest Manifest;
		Manifest.Header.NodeCount = 7;
		Manifest.Header.RootNode = 0;
		Manifest.Nodes.SetNum(7);
		Manifest.Nodes[0].FirstChild = 1;
		Manifest.Nodes[0].ChildCount = 2;
		Manifest.Nodes[0].Flags = ENodeFlags::Internal;
		Manifest.Nodes[1].FirstChild = 3;
		Manifest.Nodes[1].ChildCount = 3;
		Manifest.Nodes[1].Flags = ENodeFlags::Internal;
		Manifest.Nodes[2].FirstChild = 6;
		Manifest.Nodes[2].ChildCount = 1;
		Manifest.Nodes[2].Flags = ENodeFlags::Internal;
		for (uint32 NodeIndex = 3; NodeIndex < 7; ++NodeIndex)
			Manifest.Nodes[NodeIndex].Flags = ENodeFlags::ExactLeaf;
		return Manifest;
	}

	FManifest MakeSameBucketPriorityTree()
	{
		FManifest Manifest;
		Manifest.Header.NodeCount = 7;
		Manifest.Header.RootNode = 0;
		Manifest.Nodes.SetNum(7);
		Manifest.Nodes[0].FirstChild = 1;
		Manifest.Nodes[0].ChildCount = 2;
		Manifest.Nodes[0].Flags = ENodeFlags::Internal;
		Manifest.Nodes[1].FirstChild = 3;
		Manifest.Nodes[1].ChildCount = 2;
		Manifest.Nodes[1].Flags = ENodeFlags::Internal;
		Manifest.Nodes[2].FirstChild = 5;
		Manifest.Nodes[2].ChildCount = 2;
		Manifest.Nodes[2].Flags = ENodeFlags::Internal;
		for (uint32 NodeIndex = 3; NodeIndex < 7; ++NodeIndex)
		{
			Manifest.Nodes[NodeIndex].Flags = ENodeFlags::ExactLeaf;
		}
		return Manifest;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNanoGSTreeResidentTransitionTest,
	"NanoGS.Tree.Selector.ResidentTransition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNanoGSTreeResidentTransitionTest::RunTest(const FString& Parameters)
{
	const FManifest Manifest = MakeThreeNodeTree();
	FSelectionConfig Config;
	Config.SplitThresholdPx = 2.0f;
	Config.MaxActiveNodes = 8;
	const auto Observe = [](uint32 NodeIndex)
	{
		return FNodeObservation{NodeIndex == 0 ? 10.0f : 1.0f, true};
	};
	FSelector Selector;
	const FSelection ParentFallback = Selector.Select(
		Manifest, Config, Observe, [](uint32 NodeIndex) { return NodeIndex == 0; });
	TestEqual(TEXT("target child count"), ParentFallback.TargetNodes.Num(), 2);
	TestTrue(TEXT("target child 1"), ParentFallback.TargetNodes.Contains(1));
	TestTrue(TEXT("target child 2"), ParentFallback.TargetNodes.Contains(2));
	TestEqual(TEXT("fallback render count"), ParentFallback.RenderNodes.Num(), 1);
	TestEqual(TEXT("fallback parent"), ParentFallback.RenderNodes[0], 0u);
	TestEqual(TEXT("requests include parent and children"), ParentFallback.RequestedNodes.Num(), 3);

	const FSelection ChildrenReady = Selector.Select(
		Manifest, Config, Observe, [](uint32 NodeIndex) { return NodeIndex != 0; });
	TestEqual(TEXT("ready child render count"), ChildrenReady.RenderNodes.Num(), 2);
	TestTrue(TEXT("ready child 1"), ChildrenReady.RenderNodes.Contains(1));
	TestTrue(TEXT("ready child 2"), ChildrenReady.RenderNodes.Contains(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNanoGSTreeLinearTransitionTest,
	"NanoGS.Tree.Selector.LinearTransition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNanoGSTreeLinearTransitionTest::RunTest(const FString& Parameters)
{
	const FManifest Manifest = MakeMixedPriorityTree();
	const TArray<uint32> Parents = {MAX_uint32, 0, 0, 2, 2};
	const TArray<uint32> Targets = {1, 3, 4};
	TArray<uint32> Requested;
	TArray<uint32> Render;

	FSelector::ResolveTransitionFrontier(
		Manifest, Parents, Targets,
		[](uint32 Node) { return Node == 0; }, Requested, Render);
	TestEqual(TEXT("all ancestors requested"), Requested.Num(), 5);
	TestEqual(TEXT("root covers incomplete descendants"), Render.Num(), 1);
	TestEqual(TEXT("root fallback"), Render[0], 0u);

	FSelector::ResolveTransitionFrontier(
		Manifest, Parents, Targets,
		[](uint32 Node) { return Node == 1 || Node == 2; }, Requested, Render);
	TestEqual(TEXT("subtree parent and sibling render"), Render.Num(), 2);
	TestTrue(TEXT("resident sibling leaf"), Render.Contains(1));
	TestTrue(TEXT("resident subtree parent"), Render.Contains(2));
	TestFalse(TEXT("parent and child never overlap"), Render.Contains(3));

	FSelector::ResolveTransitionFrontier(
		Manifest, Parents, Targets,
		[](uint32 Node) { return Node == 1 || Node == 3 || Node == 4; }, Requested, Render);
	TestEqual(TEXT("all target children replace parents"), Render.Num(), 3);
	TestTrue(TEXT("target 1"), Render.Contains(1));
	TestTrue(TEXT("target 3"), Render.Contains(3));
	TestTrue(TEXT("target 4"), Render.Contains(4));
	TestFalse(TEXT("root removed after complete refinement"), Render.Contains(0));
	TestFalse(TEXT("internal parent removed after complete refinement"), Render.Contains(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNanoGSTreeBudgetTest,
	"NanoGS.Tree.Selector.Budget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNanoGSTreeBudgetTest::RunTest(const FString& Parameters)
{
	const FManifest Manifest = MakeThreeNodeTree();
	FSelectionConfig Config;
	Config.SplitThresholdPx = 2.0f;
	Config.MaxActiveNodes = 1;
	FSelector Selector;
	const FSelection Selection = Selector.Select(
		Manifest, Config,
		[](uint32 NodeIndex) { return FNodeObservation{10.0f, true}; },
		[](uint32 NodeIndex) { return true; });
	TestTrue(TEXT("budget limited"), Selection.bBudgetLimited);
	TestEqual(TEXT("root target"), Selection.TargetNodes.Num(), 1);
	TestEqual(TEXT("root render"), Selection.RenderNodes.Num(), 1);
	TestEqual(TEXT("root index"), Selection.RenderNodes[0], 0u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNanoGSTreePriorityOrderTest,
	"NanoGS.Tree.Selector.LargestProjectedErrorFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNanoGSTreePriorityOrderTest::RunTest(const FString& Parameters)
{
	const FManifest Manifest = MakeMixedPriorityTree();
	FSelectionConfig Config;
	Config.SplitThresholdPx = 2.0f;
	Config.MaxActiveNodes = 8;
	const auto Observe = [](uint32 NodeIndex)
	{
		const float Scores[] = {10.0f, 1.0f, 9.0f, 1.0f, 1.0f};
		return FNodeObservation{Scores[NodeIndex], true};
	};
	FSelector Selector;
	const FSelection Selection = Selector.Select(
		Manifest, Config, Observe, [](uint32 NodeIndex) { return true; });
	TestEqual(TEXT("mixed-priority target count"), Selection.TargetNodes.Num(), 3);
	TestTrue(TEXT("small sibling remains"), Selection.TargetNodes.Contains(1));
	TestFalse(TEXT("large internal node is refined"), Selection.TargetNodes.Contains(2));
	TestTrue(TEXT("first refined child"), Selection.TargetNodes.Contains(3));
	TestTrue(TEXT("second refined child"), Selection.TargetNodes.Contains(4));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNanoGSTreeFastPathTest,
	"NanoGS.Tree.Selector.FastPathMatchesSparkBudgetBreak",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNanoGSTreeFastPathTest::RunTest(const FString& Parameters)
{
	const FManifest Manifest = MakeBudgetBreakTree();
	FSelectionConfig Config;
	Config.SplitThresholdPx = 2.0f;
	Config.MaxActiveNodes = 3;
	Config.bBuildTransitionData = false;
	const float Scores[] = {10.0f, 9.0f, 8.0f, 1.0f, 1.0f, 1.0f, 1.0f};
	FSelector Selector;
	const FSelection Selection = Selector.Select(
		Manifest, Config,
		[&Scores](uint32 NodeIndex) { return FNodeObservation{Scores[NodeIndex], true}; },
		[](uint32 NodeIndex) { return true; });
	TestTrue(TEXT("budget limited"), Selection.bBudgetLimited);
	TestEqual(TEXT("frontier remains at two nodes"), Selection.TargetNodes.Num(), 2);
	TestTrue(TEXT("highest-priority parent remains"), Selection.TargetNodes.Contains(1));
	TestTrue(TEXT("lower-priority sibling is not refined"), Selection.TargetNodes.Contains(2));

	Config.bUseHierarchicalHeap = false;
	const FSelection Reference = Selector.Select(
		Manifest, Config,
		[&Scores](uint32 NodeIndex) { return FNodeObservation{Scores[NodeIndex], true}; },
		[](uint32 NodeIndex) { return true; });
	TArray<uint32> HierarchicalTargets = Selection.TargetNodes;
	TArray<uint32> ReferenceTargets = Reference.TargetNodes;
	HierarchicalTargets.Sort();
	ReferenceTargets.Sort();
	TestEqual(TEXT("hierarchical heap matches exact global heap"), HierarchicalTargets, ReferenceTargets);

	const FManifest SameBucketManifest = MakeSameBucketPriorityTree();
	const float SameBucketScores[] = {10.0f, 8.00001f, 8.00002f, 1.0f, 1.0f, 1.0f, 1.0f};
	Config.MaxActiveNodes = 3;
	Config.bUseHierarchicalHeap = true;
	const FSelection SameBucketSelection = Selector.Select(
		SameBucketManifest, Config,
		[&SameBucketScores](uint32 NodeIndex)
		{
			return FNodeObservation{SameBucketScores[NodeIndex], true};
		},
		[](uint32 NodeIndex) { return true; });
	Config.bUseHierarchicalHeap = false;
	const FSelection SameBucketReference = Selector.Select(
		SameBucketManifest, Config,
		[&SameBucketScores](uint32 NodeIndex)
		{
			return FNodeObservation{SameBucketScores[NodeIndex], true};
		},
		[](uint32 NodeIndex) { return true; });
	TArray<uint32> SameBucketTargets = SameBucketSelection.TargetNodes;
	TArray<uint32> SameBucketReferenceTargets = SameBucketReference.TargetNodes;
	SameBucketTargets.Sort();
	SameBucketReferenceTargets.Sort();
	TestEqual(TEXT("same 24-bit bucket preserves reference priority"),
		SameBucketTargets, SameBucketReferenceTargets);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNanoGSTreeCancellationTest,
	"NanoGS.Tree.Selector.Cancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNanoGSTreeCancellationTest::RunTest(const FString& Parameters)
{
	const FManifest Manifest = MakeThreeNodeTree();
	std::atomic<bool> Cancelled{true};
	FSelectionConfig Config;
	Config.bBuildTransitionData = false;
	Config.CancelFlag = &Cancelled;
	FSelector Selector;
	const FSelection Selection = Selector.Select(
		Manifest, Config,
		[](uint32 NodeIndex) { return FNodeObservation{10.0f, true}; },
		[](uint32 NodeIndex) { return true; });
	TestTrue(TEXT("selection reports cancellation"), Selection.bCancelled);
	TestTrue(TEXT("cancelled selection publishes no targets"), Selection.TargetNodes.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNanoGSTreeFastResidentProgressiveTest,
	"NanoGS.Tree.Selector.FastResidentProgressive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNanoGSTreeFastResidentProgressiveTest::RunTest(const FString& Parameters)
{
	FManifest Manifest = MakeMixedPriorityTree();
	Manifest.Header.PagePoints = Manifest.Nodes.Num();
	for (int32 NodeIndex = 0; NodeIndex < Manifest.Nodes.Num(); ++NodeIndex)
	{
		Manifest.Nodes[NodeIndex].PageId = 0;
		Manifest.Nodes[NodeIndex].PageLocal = static_cast<uint32>(NodeIndex);
	}
	FSelectionConfig Config;
	Config.SplitThresholdPx = 2.0f;
	Config.MaxActiveNodes = 8;
	Config.bBuildTransitionData = false;
	Config.bBuildPageOutputMask = true;
	Config.bBuildResidentProgressiveData = true;
	Config.bBuildResidentRefinementTopology = true;
	const float Scores[] = {10.0f, 1.0f, 9.0f, 1.0f, 1.0f};
	FSelector Selector;

	const FSelection Partial = Selector.Select(
		Manifest, Config,
		[&Scores](const uint32 NodeIndex)
		{
			return FNodeObservation{Scores[NodeIndex], true};
		},
		[](const uint32 NodeIndex)
		{
			return NodeIndex == 0 || NodeIndex == 1 || NodeIndex == 2 || NodeIndex == 3;
		});
	TestEqual(TEXT("exact target remains unchanged"), Partial.TargetNodes.Num(), 3);
	TestTrue(TEXT("exact target contains sibling"), Partial.TargetNodes.Contains(1));
	TestTrue(TEXT("exact target contains first grandchild"), Partial.TargetNodes.Contains(3));
	TestTrue(TEXT("exact target contains missing grandchild"), Partial.TargetNodes.Contains(4));
	TestEqual(TEXT("resident-safe frontier has sibling and parent fallback"), Partial.RenderNodes.Num(), 2);
	TestTrue(TEXT("resident sibling is rendered"), Partial.RenderNodes.Contains(1));
	TestTrue(TEXT("incomplete subtree keeps parent"), Partial.RenderNodes.Contains(2));
	TestFalse(TEXT("parent and child never overlap"), Partial.RenderNodes.Contains(3));
	TestTrue(TEXT("root split recorded"), Partial.RefinedNodes[0]);
	TestTrue(TEXT("nested split recorded"), Partial.RefinedNodes[2]);
	TestEqual(TEXT("render page-output count matches render frontier"),
		Partial.RenderPageOutputs.CountSetBits(), Partial.RenderNodes.Num());

	const FSelection Complete = Selector.Select(
		Manifest, Config,
		[&Scores](const uint32 NodeIndex)
		{
			return FNodeObservation{Scores[NodeIndex], true};
		},
		[](const uint32 NodeIndex) { return true; });
	TestEqual(TEXT("complete resident frontier reaches exact target"), Complete.RenderNodes.Num(), 3);
	TestTrue(TEXT("complete sibling"), Complete.RenderNodes.Contains(1));
	TestTrue(TEXT("complete first grandchild"), Complete.RenderNodes.Contains(3));
	TestTrue(TEXT("complete second grandchild"), Complete.RenderNodes.Contains(4));
	TestFalse(TEXT("complete frontier removes nested parent"), Complete.RenderNodes.Contains(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNanoGSTreeMergeFrontiersTest,
	"NanoGS.Tree.Selector.MergeFrontiers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNanoGSTreeMergeFrontiersTest::RunTest(const FString& Parameters)
{
	const FManifest Manifest = MakeThreeNodeTree();
	const TArray<uint32> Parents = {MAX_uint32, 0u, 0u};
	const TArray<uint32> FrontierA = {0u};
	const TArray<uint32> FrontierB = {1u};
	const TArray<const TArray<uint32>*> Frontiers = {&FrontierA, &FrontierB};
	const TArray<uint32> Merged = FSelector::MergeFrontiers(Manifest, Parents, Frontiers);
	TestEqual(TEXT("merged count"), Merged.Num(), 2);
	TestTrue(TEXT("requested refined child"), Merged.Contains(1));
	TestTrue(TEXT("sibling preserves coarse coverage"), Merged.Contains(2));
	TestFalse(TEXT("ancestor removed"), Merged.Contains(0));
	return true;
}
}

#endif
