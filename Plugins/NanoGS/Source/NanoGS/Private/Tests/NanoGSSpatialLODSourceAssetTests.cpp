// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Paged/NanoGSSpatialLODSourceAsset.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpatialLODRealPackageTest, "NanoGS.SpatialLOD.SourceAsset.RealPackage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSpatialLODRealPackageTest::RunTest(const FString& Parameters)
{
	UNanoGSSpatialLODSourceAsset* Asset = NewObject<UNanoGSSpatialLODSourceAsset>();
	FString Error;
	const FString Directory = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("NanoGSData/SJTU_SpatialLOD"));
	if (!TestTrue(TEXT("Real package initializes"), Asset->InitializeFromDirectory(Directory, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Node count"), Asset->GetNodeCount(), 257);
	TestEqual(TEXT("Level count"), Asset->GetLevelCount(), 5);
	TestEqual(TEXT("Environment node"), Asset->GetEnvironmentNode(), 2);
	TestEqual(TEXT("Combined page count"), Asset->GetPageCount(), 645);
	TestEqual(TEXT("Combined representation splats"), Asset->GetTotalRepresentationSplats(), static_cast<int64>(15293297));
	NanoGS::SpatialLOD::FManifest Manifest;
	if (!TestTrue(TEXT("Stored metadata reopens"), Asset->OpenManifest(Manifest, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Node 256 L2 page id"), Manifest.FindPageId(256, 2), 642);
	NanoGS::Paged::FManifest PageManifest;
	TestTrue(TEXT("Combined Page manifest reopens"), Asset->OpenPageManifest(PageManifest, Error));
	TestEqual(TEXT("Mapped page splat count"), PageManifest.Pages[642].SplatCount, static_cast<uint64>(9442));
	TArray64<uint8> Payload;
	if (!TestTrue(TEXT("Node 256 L2 payload reads"), NanoGS::SpatialLOD::FReader::ReadNodeLodPayload(Manifest, 256, 2, Payload, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Node 256 L2 splat count"), Payload.Num() / NanoGS::SpatialLOD::SplatStrideBytes, static_cast<int64>(9442));
	return true;
}
#endif
