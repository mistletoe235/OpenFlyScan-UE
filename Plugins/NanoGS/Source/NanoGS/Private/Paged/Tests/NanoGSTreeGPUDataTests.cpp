// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Paged/NanoGSTreeGPUData.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNanoGSTreeGPUNodePackingTest,
	"NanoGS.Tree.GPUData.NodePacking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNanoGSTreeGPUNodePackingTest::RunTest(const FString& Parameters)
{
	NanoGS::Tree::FManifest Manifest;
	Manifest.Header.NodeCount = 2;
	Manifest.Header.RootNode = 0;
	Manifest.Nodes.SetNum(2);
	Manifest.Nodes[0].CenterCm = FVector3f(1.0f, 2.0f, 3.0f);
	Manifest.Nodes[0].SupportRadiusCm = 8.0f;
	Manifest.Nodes[0].FeatureRadiusCm = 4.0f;
	Manifest.Nodes[0].FirstChild = 1;
	Manifest.Nodes[0].ChildCount = 1;
	Manifest.Nodes[0].PageId = 7;
	Manifest.Nodes[0].PageLocal = 23;
	Manifest.Nodes[0].Flags = NanoGS::Tree::ENodeFlags::Internal;
	Manifest.Nodes[1].Flags = NanoGS::Tree::ENodeFlags::ExactLeaf;

	TArray<NanoGS::Tree::FGPUNodeRecord> Nodes;
	FString Error;
	TestTrue(TEXT("GPU node table builds"), NanoGS::Tree::BuildGPUNodeTable(Manifest, Nodes, Error));
	TestEqual(TEXT("GPU node count"), Nodes.Num(), 2);
	TestEqual(TEXT("child count"), Nodes[0].GetChildCount(), 1u);
	TestTrue(TEXT("internal flag"), Nodes[0].IsInternal());
	TestTrue(TEXT("leaf flag"), Nodes[1].IsExactLeaf());
	TestEqual(TEXT("feature radius"), Nodes[0].GetFeatureRadiusCm(), 4.0f);
	TestEqual(TEXT("support radius"), Nodes[0].CenterAndSupportRadius.W, 8.0f);
	TestEqual(TEXT("page id"), Nodes[0].GetPageId(), 7u);
	TestEqual(TEXT("page local"), Nodes[0].GetPageLocal(), 23u);
	return true;
}

#endif
