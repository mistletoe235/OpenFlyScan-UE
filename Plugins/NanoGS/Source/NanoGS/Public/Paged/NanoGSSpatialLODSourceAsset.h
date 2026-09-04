// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Paged/NanoGSSpatialLODReader.h"
#include "Paged/NanoGSPageReader.h"
#include "NanoGSSpatialLODSourceAsset.generated.h"

UCLASS(BlueprintType, hidecategories = Object)
class NANOGS_API UNanoGSSpatialLODSourceAsset : public UObject
{
	GENERATED_BODY()

public:
	bool InitializeFromDirectory(const FString& Directory, FString& OutError);
	bool Refresh(FString& OutError);
	bool OpenManifest(NanoGS::SpatialLOD::FManifest& OutManifest, FString& OutError) const;
	bool OpenPageManifest(NanoGS::Paged::FManifest& OutManifest, FString& OutError) const;
	FString GetResolvedDirectory() const;
	bool HasValidMetadata() const;

	const FString& GetProjectRelativeDirectory() const { return ProjectRelativeDirectory; }
	int32 GetNodeCount() const { return NodeCount; }
	int32 GetLevelCount() const { return LevelCount; }
	int32 GetEnvironmentNode() const { return EnvironmentNode; }
	const TArray<int64>& GetLevelSplatCounts() const { return LevelSplatCounts; }
	const FBox& GetSceneBounds() const { return SceneBounds; }
	int32 GetPageCount() const { return PageCount; }
	int64 GetTotalRepresentationSplats() const { return TotalRepresentationSplats; }

private:
	bool ApplyManifest(const FString& RelativeDirectory, const NanoGS::SpatialLOD::FManifest& Manifest, const NanoGS::Paged::FManifest& PageManifest, FString& OutError);

	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "NanoGS|Spatial LOD")
	FString ProjectRelativeDirectory;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Spatial LOD")
	bool bMetadataValid = false;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Spatial LOD")
	int32 NodeCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Spatial LOD")
	int32 LevelCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Spatial LOD")
	int32 EnvironmentNode = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Spatial LOD")
	TArray<int64> LevelSplatCounts;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Spatial LOD")
	FBox SceneBounds = FBox(ForceInit);

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Spatial LOD")
	int32 PageCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Spatial LOD")
	int64 TotalRepresentationSplats = 0;
};
