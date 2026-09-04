// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "Paged/NanoGSPageReader.h"

#include "NanoGSPagedSourceAsset.generated.h"

/**
 * Lightweight reference to an external NanoGS Page v1 container.
 *
 * This UObject deliberately serializes only a project-relative path and a
 * validated metadata summary. Page directories and payload bytes remain in the
 * external .ngsp file and are opened on demand by the streaming runtime.
 */
UCLASS(BlueprintType, hidecategories = Object)
class NANOGS_API UNanoGSPagedSourceAsset : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Parse ContainerFilename, validate its v1 metadata, and atomically replace
	 * this asset's stored summary. The container must live below ProjectDir.
	 */
	bool InitializeFromContainer(
		const FString& ContainerFilename,
		FString& OutError,
		bool bVerifyMetadataCRCs = true);

	/** Re-read the currently referenced container and refresh the stored summary. */
	bool RefreshFromContainer(FString& OutError, bool bVerifyMetadataCRCs = true);

	/**
	 * Parse the external manifest at runtime without loading any page payload.
	 * The parsed manifest must exactly match the summary stored in this UObject.
	 */
	bool OpenManifest(
		NanoGS::Paged::FManifest& OutManifest,
		FString& OutError,
		const NanoGS::Paged::FReadOptions& Options = NanoGS::Paged::FReadOptions()) const;

	/** Resolve the serialized project-relative path for the current process/build. */
	FString GetResolvedContainerPath() const;

	/**
	 * Resolve a serialized path against explicit runtime roots.
	 *
	 * Content/... paths are anchored to ProjectContentDirectory, not inferred
	 * from the executable working directory. Cooked builds pass true for
	 * bRequireStagedContentPath so only loose NonUFS files below
	 * Content/NanoGSData are accepted. Exposed for packaging validation tests.
	 */
	static bool ResolveContainerPathForRuntimeRoots(
		const FString& InProjectRelativePath,
		const FString& ProjectDirectory,
		const FString& ProjectContentDirectory,
		bool bRequireStagedContentPath,
		FString& OutResolvedPath,
		FString& OutError);

	/** True when a validated summary and a safe project-relative path are present. */
	bool HasValidMetadata() const;

	/**
	 * True when the container is in Content/NanoGSData, the supported packaging
	 * location. NanoGS.Build.cs stages this directory as a NonUFS runtime
	 * dependency; the project packaging setting remains an explicit safeguard.
	 */
	bool IsInRecommendedStagingDirectory() const;

	const FString& GetProjectRelativeContainerPath() const { return ProjectRelativeContainerPath; }
	const FString& GetMetadataFingerprint() const { return MetadataFingerprint; }
	uint64 GetPhysicalFileBytes() const { return PhysicalFileBytes; }
	uint64 GetPayloadBytes() const { return PayloadBytes; }
	uint64 GetPageCount() const { return PageCount; }
	uint64 GetTotalSplatCount() const { return TotalSplatCount; }
	const FBox& GetSceneBounds() const { return SceneBounds; }
	/** Local-space Page v1 bounds expanded to the renderer's 4-sigma support. */
	bool GetConservativeExactSupportBounds(double SplatScale, FBox& OutBounds) const;
	uint32 GetHeaderCRC32() const { return HeaderCRC32; }
	uint32 GetDirectoryCRC32() const { return DirectoryCRC32; }

	/** Project-relative directory used by the loose-file packaging contract. */
	static const TCHAR* GetRecommendedProjectRelativeDirectory();

private:
	static bool MakeSafeProjectRelativePath(
		const FString& ContainerFilename,
		FString& OutRelativePath,
		FString& OutResolvedPath,
		FString& OutError);

	static FString BuildMetadataFingerprint(const NanoGS::Paged::FManifest& Manifest);

	bool DoesManifestMatchStoredMetadata(
		const NanoGS::Paged::FManifest& Manifest,
		FString& OutError) const;

	void ApplyManifestSummary(
		const FString& InProjectRelativePath,
		const NanoGS::Paged::FManifest& Manifest);
	double ResolveGlobalMaxScaleCm() const;

private:
	/** Portable path relative to ProjectDir; never absolute and never contains '..'. */
	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "NanoGS|Paged Source")
	FString ProjectRelativeContainerPath;

	/** Identity over the validated v1 header/directory CRCs and key 64-bit counts. */
	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "NanoGS|Paged Source")
	FString MetadataFingerprint;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source")
	bool bMetadataValid = false;

	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "NanoGS|Paged Source|Format")
	uint32 FormatVersion = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Format")
	uint32 PayloadFormat = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Format")
	uint32 CoordinateSystem = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Format")
	uint32 SHOrder = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Format")
	uint32 SHLayout = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Integrity", meta = (DisplayName = "Header CRC32"))
	uint32 HeaderCRC32 = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Integrity", meta = (DisplayName = "Directory CRC32"))
	uint32 DirectoryCRC32 = 0;

	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "NanoGS|Paged Source|Counts")
	uint64 PhysicalFileBytes = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Counts")
	uint64 PayloadBytes = 0;

	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "NanoGS|Paged Source|Counts")
	uint64 PageCount = 0;

	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "NanoGS|Paged Source|Counts")
	uint64 TotalSplatCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Bounds")
	FBox SceneBounds = FBox(ForceInit);

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Bounds")
	double SupportSigma = 0.0;

	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Bounds")
	double UnitsToCentimeters = 0.0;

	/** Maximum principal Gaussian scale cached from the Page directory. */
	UPROPERTY(VisibleAnywhere, Category = "NanoGS|Paged Source|Bounds")
	double GlobalMaxScaleCm = -1.0;

	/** Backward-compatible lazy cache for assets saved before GlobalMaxScaleCm existed. */
	mutable double RuntimeResolvedMaxScaleCm = -1.0;
};
