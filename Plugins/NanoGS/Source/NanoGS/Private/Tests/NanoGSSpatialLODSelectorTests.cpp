// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Paged/NanoGSSpatialLODSelector.h"

namespace NanoGS::SpatialLOD::Tests
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpatialLODSelectorTest, "NanoGS.SpatialLOD.Selector.SafeMultiView", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSpatialLODSelectorTest::RunTest(const FString& Parameters)
{
	FManifest Manifest;
	Manifest.LevelCount = 3;
	Manifest.EnvironmentNode = 2;
	Manifest.Nodes.SetNum(3);
	Manifest.Nodes[1].Flags = ENodeFlags::Leaf;
	Manifest.Nodes[2].Flags = ENodeFlags::Leaf | ENodeFlags::Environment;
	Manifest.Ranges.SetNum(9);
	Manifest.Errors.SetNum(9);
	for (uint32 Node : {1u, 2u})
	{
		for (uint32 Level = 0; Level < 3; ++Level)
			Manifest.Ranges[Node * 3 + Level].SplatCount = 1;
	}
	Manifest.Errors[1 * 3 + 1].MaxSafeProjectedRadiusPx = 80.0f;
	Manifest.Errors[1 * 3 + 2].MaxSafeProjectedRadiusPx = 20.0f;

	FViewObservations FarView;
	FarView.Nodes.Add({1, 10.0f, true});
	FViewObservations NearView;
	NearView.Nodes.Add({1, 60.0f, true});
	FSelector Selector;
	const FSelection Selection = Selector.Select(Manifest, {FarView, NearView});
	TestEqual(TEXT("Scene plus environment selected"), Selection.Nodes.Num(), 2);
	TestEqual(TEXT("Nearer view forces finer certified level"), Selection.Nodes[0].Level, 1u);
	TestEqual(TEXT("Environment remains L0"), Selection.Nodes[1].Level, 0u);

	FSelector HysteresisSelector;
	FViewObservations FarOnly;
	FarOnly.Nodes.Add({1, 10.0f, true});
	TestEqual(TEXT("Far node initially selects L2"), HysteresisSelector.Select(Manifest, {FarOnly}).Nodes[0].Level, 2u);
	FarOnly.Nodes[0].ProjectedRadiusPx = 21.0f;
	TestEqual(TEXT("Refinement is immediate"), HysteresisSelector.Select(Manifest, {FarOnly}).Nodes[0].Level, 1u);
	FarOnly.Nodes[0].ProjectedRadiusPx = 19.0f;
	TestEqual(TEXT("Coarsening waits inside hysteresis band"), HysteresisSelector.Select(Manifest, {FarOnly}).Nodes[0].Level, 1u);
	FarOnly.Nodes[0].ProjectedRadiusPx = 15.0f;
	TestEqual(TEXT("Coarsening occurs beyond hysteresis band"), HysteresisSelector.Select(Manifest, {FarOnly}).Nodes[0].Level, 2u);

	Manifest.Errors[1 * 3 + 1].MaxSafeProjectedRadiusPx = NAN;
	const FSelection Uncalibrated = Selector.Select(Manifest, {NearView});
	TestEqual(TEXT("Uncalibrated node fails closed to L0"), Uncalibrated.Nodes[0].Level, 0u);

	Manifest.Errors[1 * 3 + 1].MaxSafeProjectedRadiusPx = 80.0f;
	Manifest.Ranges[1 * 3 + 0].SplatCount = 100;
	Manifest.Ranges[1 * 3 + 1].SplatCount = 50;
	Manifest.Ranges[1 * 3 + 2].SplatCount = 10;
	FSelector BudgetSelector;
	FViewObservations BudgetView;
	BudgetView.Nodes.Add({1, 60.0f, true});
	TestEqual(TEXT("Budget setup selects certified L1"), BudgetSelector.Select(Manifest, {BudgetView}).Nodes[0].Level, 1u);
	BudgetView.Nodes[0].ProjectedRadiusPx = 19.0f;
	const FSelection UnbudgetedHysteresis = BudgetSelector.Select(Manifest, {BudgetView});
	TestEqual(TEXT("Disabled budget preserves legacy hysteresis"), UnbudgetedHysteresis.Nodes[0].Level, 1u);
	TMap<uint32, uint32> BudgetedLevels;
	TMap<uint32, uint32> CertifiedLevels;
	for (const FNodeSelection& Node : UnbudgetedHysteresis.Nodes)
	{
		BudgetedLevels.Add(Node.NodeIndex, Node.Level);
		CertifiedLevels.Add(Node.NodeIndex, Node.CertifiedLevel);
	}
	const FSoftBudgetResult Budgeted = FSelector::ApplySoftBudget(Manifest, CertifiedLevels, 20, BudgetedLevels);
	TestEqual(TEXT("Soft budget releases certified-safe hysteresis headroom"), BudgetedLevels.FindRef(1), 2u);
	TestEqual(TEXT("Budgeted count includes environment"), Budgeted.SelectedSplatCount, 11ull);
	TestFalse(TEXT("Reachable soft budget is not exceeded"), Budgeted.bBudgetExceeded);

	FSelector ImpossibleBudgetSelector;
	const FSelection ImpossibleSelection = ImpossibleBudgetSelector.Select(Manifest, {NearView});
	TMap<uint32, uint32> ImpossibleLevels;
	TMap<uint32, uint32> ImpossibleCertifiedLevels;
	for (const FNodeSelection& Node : ImpossibleSelection.Nodes)
	{
		ImpossibleLevels.Add(Node.NodeIndex, Node.Level);
		ImpossibleCertifiedLevels.Add(Node.NodeIndex, Node.CertifiedLevel);
	}
	const FSoftBudgetResult ImpossibleBudget = FSelector::ApplySoftBudget(
		Manifest, ImpossibleCertifiedLevels, 1, ImpossibleLevels);
	TestEqual(TEXT("Impossible budget never coarsens below certified L1"), ImpossibleLevels.FindRef(1), 1u);
	TestTrue(TEXT("Impossible budget reports safe overflow"), ImpossibleBudget.bBudgetExceeded);
	return true;
}
}

#endif
