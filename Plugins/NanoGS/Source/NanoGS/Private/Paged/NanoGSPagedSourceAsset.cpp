// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSPagedSourceAsset.h"

#include "Paged/NanoGSPageDemandSelector.h"

#include "Misc/Paths.h"

namespace
{
	constexpr TCHAR RecommendedPagedDataDirectory[] = TEXT("Content/NanoGSData");
	constexpr TCHAR ProjectContentPrefix[] = TEXT("Content/");
	constexpr TCHAR RecommendedPagedDataPrefix[] = TEXT("Content/NanoGSData/");

	bool IsUnsafeRelativePath(const FString& Path)
	{
		return Path.IsEmpty() ||
			!FPaths::IsRelative(Path) ||
			Path.Equals(TEXT(".."), ESearchCase::CaseSensitive) ||
			Path.StartsWith(TEXT("../"), ESearchCase::CaseSensitive) ||
			Path.Contains(TEXT("/../"), ESearchCase::CaseSensitive);
	}

	bool IsRecommendedPagedDataPath(const FString& Path)
	{
		return Path.StartsWith(RecommendedPagedDataPrefix, ESearchCase::CaseSensitive);
	}

	FString NormalizeFullPath(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Result);
		FPaths::CollapseRelativeDirectories(Result);
		return Result;
	}

	bool EqualVector(const FVector& A, const FVector3d& B)
	{
		return A.X == B.X && A.Y == B.Y && A.Z == B.Z;
	}

	double CalculateManifestMaxScaleCm(const NanoGS::Paged::FManifest& Manifest)
	{
		double Result = 0.0;
		for (const NanoGS::Paged::FPageRecord& Page : Manifest.Pages)
		{
			if (!FMath::IsFinite(Page.MaxScaleCm) || Page.MaxScaleCm < 0.0)
			{
				return -1.0;
			}
			Result = FMath::Max(Result, Page.MaxScaleCm);
		}
		return Result;
	}
}

const TCHAR* UNanoGSPagedSourceAsset::GetRecommendedProjectRelativeDirectory()
{
	return RecommendedPagedDataDirectory;
}

bool UNanoGSPagedSourceAsset::MakeSafeProjectRelativePath(
	const FString& ContainerFilename,
	FString& OutRelativePath,
	FString& OutResolvedPath,
	FString& OutError)
{
	OutRelativePath.Reset();
	OutResolvedPath.Reset();
	OutError.Reset();

	if (ContainerFilename.IsEmpty())
	{
		OutError = TEXT("NanoGS paged container path is empty");
		return false;
	}

	if (!FPaths::GetExtension(ContainerFilename).Equals(TEXT("ngsp"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("NanoGS paged source must use the .ngsp extension");
		return false;
	}

	FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectRoot);
	FPaths::CollapseRelativeDirectories(ProjectRoot);
	const FString ProjectPrefix = ProjectRoot + TEXT("/");

	FString ResolvedPath;
	if (FPaths::IsRelative(ContainerFilename))
	{
		// FPaths::Project*Dir() may itself return a path relative to BaseDir
		// (../../../../../Project/...). Prefer the project-relative interpretation,
		// but accept BaseDir-relative engine paths when that is the one below ProjectDir.
		const FString ProjectCandidate = NormalizeFullPath(FPaths::Combine(ProjectRoot, ContainerFilename));
		const FString BaseCandidate = NormalizeFullPath(ContainerFilename);
		ResolvedPath = FPaths::IsUnderDirectory(ProjectCandidate, ProjectRoot)
			? ProjectCandidate
			: BaseCandidate;
	}
	else
	{
		ResolvedPath = NormalizeFullPath(ContainerFilename);
	}

	if (!FPaths::IsUnderDirectory(ResolvedPath, ProjectRoot))
	{
		OutError = FString::Printf(
			TEXT("NanoGS paged container must be inside ProjectDir ('%s'); got '%s'"),
			*ProjectRoot,
			*ResolvedPath);
		return false;
	}

	FString RelativePath = ResolvedPath.RightChop(ProjectPrefix.Len());
	FPaths::NormalizeFilename(RelativePath);
	FPaths::CollapseRelativeDirectories(RelativePath);
	if (IsUnsafeRelativePath(RelativePath))
	{
		OutError = FString::Printf(TEXT("Unsafe project-relative NanoGS container path: '%s'"), *RelativePath);
		return false;
	}

	OutRelativePath = MoveTemp(RelativePath);
	OutResolvedPath = MoveTemp(ResolvedPath);
	return true;
}

bool UNanoGSPagedSourceAsset::ResolveContainerPathForRuntimeRoots(
	const FString& InProjectRelativePath,
	const FString& ProjectDirectory,
	const FString& ProjectContentDirectory,
	const bool bRequireStagedContentPath,
	FString& OutResolvedPath,
	FString& OutError)
{
	OutResolvedPath.Reset();
	OutError.Reset();

	FString RelativePath = InProjectRelativePath;
	FPaths::NormalizeFilename(RelativePath);
	FPaths::CollapseRelativeDirectories(RelativePath);
	if (IsUnsafeRelativePath(RelativePath))
	{
		OutError = FString::Printf(TEXT("Unsafe project-relative NanoGS container path: '%s'"), *RelativePath);
		return false;
	}
	if (!FPaths::GetExtension(RelativePath).Equals(TEXT("ngsp"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("NanoGS paged source must use the .ngsp extension");
		return false;
	}
	if (bRequireStagedContentPath && !IsRecommendedPagedDataPath(RelativePath))
	{
		OutError = FString::Printf(
			TEXT("Cooked NanoGS paged source must be staged below %s; got '%s'"),
			RecommendedPagedDataDirectory,
			*RelativePath);
		return false;
	}

	if (ProjectDirectory.IsEmpty() || ProjectContentDirectory.IsEmpty())
	{
		OutError = TEXT("NanoGS runtime project/content roots are empty or inconsistent");
		return false;
	}
	const FString ProjectRoot = NormalizeFullPath(ProjectDirectory);
	const FString ContentRoot = NormalizeFullPath(ProjectContentDirectory);
	if (ProjectRoot.IsEmpty() || ContentRoot.IsEmpty() ||
		!FPaths::IsUnderDirectory(ContentRoot, ProjectRoot))
	{
		OutError = TEXT("NanoGS runtime project/content roots are empty or inconsistent");
		return false;
	}

	FString ResolvedPath;
	FString RequiredRoot;
	if (RelativePath.StartsWith(ProjectContentPrefix, ESearchCase::CaseSensitive))
	{
		const FString ContentRelativePath = RelativePath.RightChop(UE_ARRAY_COUNT(ProjectContentPrefix) - 1);
		ResolvedPath = NormalizeFullPath(FPaths::Combine(ContentRoot, ContentRelativePath));
		RequiredRoot = ContentRoot;
	}
	else
	{
		ResolvedPath = NormalizeFullPath(FPaths::Combine(ProjectRoot, RelativePath));
		RequiredRoot = ProjectRoot;
	}

	if (!FPaths::IsUnderDirectory(ResolvedPath, RequiredRoot) ||
		!FPaths::IsUnderDirectory(ResolvedPath, ProjectRoot))
	{
		OutError = FString::Printf(
			TEXT("NanoGS container path escapes its runtime project/content root: '%s'"),
			*ResolvedPath);
		return false;
	}

	OutResolvedPath = MoveTemp(ResolvedPath);
	return true;
}

FString UNanoGSPagedSourceAsset::BuildMetadataFingerprint(const NanoGS::Paged::FManifest& Manifest)
{
	const NanoGS::Paged::FFileHeader& Header = Manifest.Header;
	return FString::Printf(
		TEXT("NGSPAGE1-v%u-h%08X-d%08X-f%016llX-p%016llX-s%016llX"),
		Header.Version,
		Header.HeaderCRC32,
		Header.DirectoryCRC32,
		static_cast<unsigned long long>(Manifest.PhysicalFileBytes),
		static_cast<unsigned long long>(Header.PageCount),
		static_cast<unsigned long long>(Header.TotalSplatCount));
}

void UNanoGSPagedSourceAsset::ApplyManifestSummary(
	const FString& InProjectRelativePath,
	const NanoGS::Paged::FManifest& Manifest)
{
	const NanoGS::Paged::FFileHeader& Header = Manifest.Header;

	ProjectRelativeContainerPath = InProjectRelativePath;
	MetadataFingerprint = BuildMetadataFingerprint(Manifest);
	bMetadataValid = true;
	FormatVersion = Header.Version;
	PayloadFormat = static_cast<uint32>(Header.PayloadFormat);
	CoordinateSystem = static_cast<uint32>(Header.CoordinateSystem);
	SHOrder = Header.SHOrder;
	SHLayout = static_cast<uint32>(Header.SHLayout);
	HeaderCRC32 = Header.HeaderCRC32;
	DirectoryCRC32 = Header.DirectoryCRC32;
	PhysicalFileBytes = Manifest.PhysicalFileBytes;
	PayloadBytes = Header.PayloadBytes;
	PageCount = Header.PageCount;
	TotalSplatCount = Header.TotalSplatCount;
	SceneBounds = FBox(
		FVector(Header.SceneBoundsMin.X, Header.SceneBoundsMin.Y, Header.SceneBoundsMin.Z),
		FVector(Header.SceneBoundsMax.X, Header.SceneBoundsMax.Y, Header.SceneBoundsMax.Z));
	SupportSigma = Header.SupportSigma;
	UnitsToCentimeters = Header.UnitsToCentimeters;
	GlobalMaxScaleCm = CalculateManifestMaxScaleCm(Manifest);
	RuntimeResolvedMaxScaleCm = GlobalMaxScaleCm;
}

double UNanoGSPagedSourceAsset::ResolveGlobalMaxScaleCm() const
{
	if (FMath::IsFinite(GlobalMaxScaleCm) && GlobalMaxScaleCm >= 0.0)
	{
		return GlobalMaxScaleCm;
	}
	if (FMath::IsFinite(RuntimeResolvedMaxScaleCm) && RuntimeResolvedMaxScaleCm >= 0.0)
	{
		return RuntimeResolvedMaxScaleCm;
	}

	NanoGS::Paged::FManifest Manifest;
	FString Error;
	NanoGS::Paged::FReadOptions Options;
	Options.bVerifyMetadataCRCs = true;
	Options.bVerifyPayloadCRCs = false;
	if (OpenManifest(Manifest, Error, Options))
	{
		const double ParsedMaxScale = CalculateManifestMaxScaleCm(Manifest);
		if (FMath::IsFinite(ParsedMaxScale) && ParsedMaxScale >= 0.0)
		{
			RuntimeResolvedMaxScaleCm = ParsedMaxScale;
			return ParsedMaxScale;
		}
	}

	// A v1 scene box is the union of rotated support AABBs. For any principal
	// axis, at least one world component has magnitude >= 1/sqrt(3), therefore
	// sqrt(3)*MaxSceneExtent/SupportSigma is a conservative scale upper bound.
	if (SceneBounds.IsValid && FMath::IsFinite(SupportSigma) && SupportSigma > 0.0)
	{
		const FVector Extent = SceneBounds.GetExtent();
		const double MaxExtent = FMath::Max3(
			FMath::Abs(Extent.X), FMath::Abs(Extent.Y), FMath::Abs(Extent.Z));
		const double FallbackMaxScale = FMath::Sqrt(3.0) * MaxExtent / SupportSigma;
		if (FMath::IsFinite(FallbackMaxScale) && FallbackMaxScale >= 0.0)
		{
			return FallbackMaxScale;
		}
	}
	return -1.0;
}

bool UNanoGSPagedSourceAsset::GetConservativeExactSupportBounds(
	const double SplatScale,
	FBox& OutBounds) const
{
	OutBounds = FBox(ForceInit);
	const double MaxScaleCm = ResolveGlobalMaxScaleCm();
	FVector3d ExpandedMin;
	FVector3d ExpandedMax;
	if (!SceneBounds.IsValid || MaxScaleCm < 0.0 ||
		!NanoGS::Paged::FPageDemandSelector::CalculateConservativeLocalSupportBounds(
			FVector3d(SceneBounds.Min),
			FVector3d(SceneBounds.Max),
			SupportSigma,
			MaxScaleCm,
			4.0,
			SplatScale,
			ExpandedMin,
			ExpandedMax))
	{
		return false;
	}
	OutBounds = FBox(FVector(ExpandedMin), FVector(ExpandedMax));
	return OutBounds.IsValid;
}

bool UNanoGSPagedSourceAsset::InitializeFromContainer(
	const FString& ContainerFilename,
	FString& OutError,
	const bool bVerifyMetadataCRCs)
{
	FString RelativePath;
	FString ResolvedPath;
	if (!MakeSafeProjectRelativePath(ContainerFilename, RelativePath, ResolvedPath, OutError))
	{
		return false;
	}

	NanoGS::Paged::FReadOptions Options;
	Options.bVerifyMetadataCRCs = bVerifyMetadataCRCs;
	Options.bVerifyPayloadCRCs = false;

	NanoGS::Paged::FManifest Manifest;
	if (!NanoGS::Paged::FPageFileReader::ReadManifest(ResolvedPath, Manifest, OutError, Options))
	{
		return false;
	}

	ApplyManifestSummary(RelativePath, Manifest);
	return true;
}

bool UNanoGSPagedSourceAsset::RefreshFromContainer(FString& OutError, const bool bVerifyMetadataCRCs)
{
	if (ProjectRelativeContainerPath.IsEmpty())
	{
		OutError = TEXT("NanoGS paged source has no container path to refresh");
		return false;
	}
	return InitializeFromContainer(ProjectRelativeContainerPath, OutError, bVerifyMetadataCRCs);
}

FString UNanoGSPagedSourceAsset::GetResolvedContainerPath() const
{
	FString ResolvedPath;
	FString Error;
	return ResolveContainerPathForRuntimeRoots(
		ProjectRelativeContainerPath,
		FPaths::ProjectDir(),
		FPaths::ProjectContentDir(),
		FPlatformProperties::RequiresCookedData(),
		ResolvedPath,
		Error)
		? ResolvedPath
		: FString();
}

bool UNanoGSPagedSourceAsset::HasValidMetadata() const
{
	if (!bMetadataValid || MetadataFingerprint.IsEmpty() || FormatVersion != NanoGS::Paged::FormatVersion ||
		PageCount == 0 || TotalSplatCount == 0 || PhysicalFileBytes == 0 || !SceneBounds.IsValid)
	{
		return false;
	}

	return !GetResolvedContainerPath().IsEmpty();
}

bool UNanoGSPagedSourceAsset::IsInRecommendedStagingDirectory() const
{
	if (!HasValidMetadata())
	{
		return false;
	}

	FString Normalized = ProjectRelativeContainerPath;
	FPaths::NormalizeFilename(Normalized);
	return IsRecommendedPagedDataPath(Normalized);
}

bool UNanoGSPagedSourceAsset::DoesManifestMatchStoredMetadata(
	const NanoGS::Paged::FManifest& Manifest,
	FString& OutError) const
{
	const NanoGS::Paged::FFileHeader& Header = Manifest.Header;
	const bool bMatches =
		BuildMetadataFingerprint(Manifest) == MetadataFingerprint &&
		Manifest.PhysicalFileBytes == PhysicalFileBytes &&
		Header.Version == FormatVersion &&
		static_cast<uint32>(Header.PayloadFormat) == PayloadFormat &&
		static_cast<uint32>(Header.CoordinateSystem) == CoordinateSystem &&
		Header.SHOrder == SHOrder &&
		static_cast<uint32>(Header.SHLayout) == SHLayout &&
		Header.HeaderCRC32 == HeaderCRC32 &&
		Header.DirectoryCRC32 == DirectoryCRC32 &&
		Header.PayloadBytes == PayloadBytes &&
		Header.PageCount == PageCount &&
		Header.TotalSplatCount == TotalSplatCount &&
		SceneBounds.IsValid &&
		EqualVector(SceneBounds.Min, Header.SceneBoundsMin) &&
		EqualVector(SceneBounds.Max, Header.SceneBoundsMax) &&
		Header.SupportSigma == SupportSigma &&
		Header.UnitsToCentimeters == UnitsToCentimeters &&
		(GlobalMaxScaleCm < 0.0 ||
			CalculateManifestMaxScaleCm(Manifest) == GlobalMaxScaleCm);

	if (!bMatches)
	{
		OutError = FString::Printf(
			TEXT("NanoGS paged container metadata no longer matches asset '%s'; reimport or refresh it before streaming"),
			*GetPathName());
		return false;
	}
	return true;
}

bool UNanoGSPagedSourceAsset::OpenManifest(
	NanoGS::Paged::FManifest& OutManifest,
	FString& OutError,
	const NanoGS::Paged::FReadOptions& Options) const
{
	OutManifest = NanoGS::Paged::FManifest();
	OutError.Reset();
	if (!HasValidMetadata())
	{
		OutError = FString::Printf(TEXT("NanoGS paged source asset '%s' has no valid metadata summary"), *GetPathName());
		return false;
	}

	const FString ResolvedPath = GetResolvedContainerPath();
	if (ResolvedPath.IsEmpty())
	{
		OutError = TEXT("NanoGS paged source contains an unsafe or unresolvable project-relative path");
		return false;
	}

	NanoGS::Paged::FManifest ParsedManifest;
	if (!NanoGS::Paged::FPageFileReader::ReadManifest(ResolvedPath, ParsedManifest, OutError, Options))
	{
		return false;
	}
	if (!DoesManifestMatchStoredMetadata(ParsedManifest, OutError))
	{
		return false;
	}

	OutManifest = MoveTemp(ParsedManifest);
	return true;
}
