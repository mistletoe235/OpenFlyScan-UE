// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Paged/NanoGSPageReader.h"
#include "Paged/NanoGSTreeReader.h"
#include "NanoGSTreeSourceAsset.generated.h"

UCLASS(BlueprintType, hidecategories = Object)
class NANOGS_API UNanoGSTreeSourceAsset : public UObject
{
	GENERATED_BODY()

public:
	bool InitializeFromDirectory(const FString& Directory, FString& OutError);
	bool Refresh(FString& OutError);
	bool OpenTreeManifest(NanoGS::Tree::FManifest& OutManifest, FString& OutError) const;
	bool OpenPageManifest(NanoGS::Paged::FManifest& OutManifest, FString& OutError) const;
	FString GetResolvedDirectory() const;
	bool HasValidMetadata() const;

	const FString& GetProjectRelativeDirectory() const { return ProjectRelativeDirectory; }
	int64 GetNodeCount() const { return NodeCount; }
	int64 GetExactLeafCount() const { return ExactLeafCount; }
	int64 GetSourceInputSplatCount() const { return SourceInputSplatCount; }
	int32 GetPageCount() const { return PageCount; }
	int32 GetPagePoints() const { return PagePoints; }
	const FString& GetMetadataFingerprint() const { return MetadataFingerprint; }
	const FBox& GetSceneBounds() const { return SceneBounds; }

private:
	bool ApplyManifest(const FString& RelativeDirectory, const NanoGS::Tree::FManifest& TreeManifest, const NanoGS::Paged::FManifest& PageManifest, FString& OutError);

	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "NanoGS|Tree v2")
	FString ProjectRelativeDirectory;

	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "NanoGS|Tree v2")
	FString MetadataFingerprint;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Tree v2")
	bool bMetadataValid = false;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Tree v2")
	int64 NodeCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Tree v2")
	int64 ExactLeafCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Tree v2")
	int64 SourceInputSplatCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Tree v2")
	int32 PageCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Tree v2")
	int32 PagePoints = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Tree v2")
	FBox SceneBounds = FBox(ForceInit);
};
