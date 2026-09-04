// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSSpatialLODSourceAsset.h"

#include "Misc/Paths.h"

namespace
{
	constexpr TCHAR RequiredPrefix[] = TEXT("Content/NanoGSData/");

	FString NormalizeFull(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Result);
		FPaths::CollapseRelativeDirectories(Result);
		return Result;
	}
}

bool UNanoGSSpatialLODSourceAsset::InitializeFromDirectory(const FString& Directory, FString& OutError)
{
	OutError.Reset();
	const FString ContentRoot = NormalizeFull(FPaths::ProjectContentDir());
	const FString ProjectRoot = FPaths::GetPath(ContentRoot);
	FString Resolved;
	if (FPaths::IsRelative(Directory))
	{
		const FString ProjectCandidate = NormalizeFull(FPaths::Combine(ProjectRoot, Directory));
		const FString BaseCandidate = NormalizeFull(Directory);
		Resolved = FPaths::IsUnderDirectory(BaseCandidate, ProjectRoot) ? BaseCandidate : ProjectCandidate;
	}
	else
	{
		Resolved = NormalizeFull(Directory);
	}
	if (!FPaths::IsUnderDirectory(Resolved, ProjectRoot))
	{
		OutError = FString::Printf(TEXT("Spatial LOD directory must be inside ProjectDir: resolved=%s project=%s content=%s"), *Resolved, *ProjectRoot, *ContentRoot);
		return false;
	}
	FString Relative = Resolved.RightChop(ProjectRoot.Len() + 1);
	FPaths::NormalizeFilename(Relative);
	if (!Relative.StartsWith(RequiredPrefix, ESearchCase::CaseSensitive) || Relative.Contains(TEXT("..")))
	{
		OutError = TEXT("Spatial LOD directory must be below Content/NanoGSData");
		return false;
	}
	NanoGS::SpatialLOD::FManifest Manifest;
	if (!NanoGS::SpatialLOD::FReader::ReadManifest(Resolved, Manifest, OutError))
		return false;
	NanoGS::Paged::FManifest PageManifest;
	NanoGS::Paged::FReadOptions PageOptions;
	PageOptions.bVerifyMetadataCRCs = true;
	if (!NanoGS::Paged::FPageFileReader::ReadManifest(
		FPaths::Combine(Resolved, TEXT("sjtu_spatial_lod.ngsp")), PageManifest, OutError, PageOptions))
		return false;
	return ApplyManifest(Relative, Manifest, PageManifest, OutError);
}

bool UNanoGSSpatialLODSourceAsset::Refresh(FString& OutError)
{
	return InitializeFromDirectory(GetResolvedDirectory(), OutError);
}

FString UNanoGSSpatialLODSourceAsset::GetResolvedDirectory() const
{
	if (!ProjectRelativeDirectory.StartsWith(TEXT("Content/"), ESearchCase::CaseSensitive))
		return FString();
	return NormalizeFull(FPaths::Combine(FPaths::ProjectContentDir(), ProjectRelativeDirectory.RightChop(8)));
}

bool UNanoGSSpatialLODSourceAsset::ApplyManifest(const FString& RelativeDirectory, const NanoGS::SpatialLOD::FManifest& Manifest, const NanoGS::Paged::FManifest& PageManifest, FString& OutError)
{
	if (Manifest.Nodes.IsEmpty() || Manifest.LevelCount == 0 || Manifest.LodHeaders.Num() != static_cast<int32>(Manifest.LevelCount))
	{
		OutError = TEXT("Spatial LOD manifest summary is incomplete");
		return false;
	}
	int32 ExpectedPageCount = 0;
	for (uint32 NodeIndex = 0; NodeIndex < static_cast<uint32>(Manifest.Nodes.Num()); ++NodeIndex)
	{
		for (uint32 Level = 0; Level < Manifest.LevelCount; ++Level)
		{
			const int32 PageId = Manifest.FindPageId(NodeIndex, Level);
			if (PageId == INDEX_NONE)
				continue;
			if (!PageManifest.Pages.IsValidIndex(PageId) ||
				PageManifest.Pages[PageId].PageId != static_cast<uint64>(PageId) ||
				PageManifest.Pages[PageId].SplatCount != Manifest.FindRange(NodeIndex, Level)->SplatCount)
			{
				OutError = FString::Printf(TEXT("Spatial LOD page mapping mismatch at node %u level %u"), NodeIndex, Level);
				return false;
			}
			++ExpectedPageCount;
		}
	}
	if (PageManifest.Pages.Num() != ExpectedPageCount)
	{
		OutError = TEXT("Spatial LOD combined Page container has an unexpected page count");
		return false;
	}
	ProjectRelativeDirectory = RelativeDirectory;
	NodeCount = Manifest.Nodes.Num();
	LevelCount = static_cast<int32>(Manifest.LevelCount);
	EnvironmentNode = Manifest.EnvironmentNode == MAX_uint32 ? INDEX_NONE : static_cast<int32>(Manifest.EnvironmentNode);
	LevelSplatCounts.Reset(LevelCount);
	for (const NanoGS::SpatialLOD::FLodHeader& Header : Manifest.LodHeaders)
		LevelSplatCounts.Add(static_cast<int64>(Header.SplatCount));
	const NanoGS::SpatialLOD::FNode& Root = Manifest.Nodes[Manifest.RootNode];
	SceneBounds = FBox(FVector(Root.BoundsMin) * 100.0, FVector(Root.BoundsMax) * 100.0);
	PageCount = PageManifest.Pages.Num();
	TotalRepresentationSplats = static_cast<int64>(PageManifest.Header.TotalSplatCount);
	bMetadataValid = true;
	return true;
}

bool UNanoGSSpatialLODSourceAsset::HasValidMetadata() const
{
	return bMetadataValid && NodeCount > 0 && LevelCount > 0 && PageCount > 0 && TotalRepresentationSplats > 0 && LevelSplatCounts.Num() == LevelCount && !GetResolvedDirectory().IsEmpty();
}

bool UNanoGSSpatialLODSourceAsset::OpenManifest(NanoGS::SpatialLOD::FManifest& OutManifest, FString& OutError) const
{
	if (!HasValidMetadata())
	{
		OutError = TEXT("Spatial LOD source asset metadata is invalid");
		return false;
	}
	if (!NanoGS::SpatialLOD::FReader::ReadRuntimeManifest(GetResolvedDirectory(), OutManifest, OutError))
		return false;
	if (OutManifest.Nodes.Num() != NodeCount || static_cast<int32>(OutManifest.LevelCount) != LevelCount ||
		(OutManifest.EnvironmentNode == MAX_uint32 ? INDEX_NONE : static_cast<int32>(OutManifest.EnvironmentNode)) != EnvironmentNode)
	{
		OutError = TEXT("Spatial LOD source metadata changed after asset initialization");
		return false;
	}
	for (int32 Level = 0; Level < LevelCount; ++Level)
	{
		if (static_cast<int64>(OutManifest.LodHeaders[Level].SplatCount) != LevelSplatCounts[Level])
		{
			OutError = TEXT("Spatial LOD level splat counts changed after asset initialization");
			return false;
		}
	}
	return true;
}


bool UNanoGSSpatialLODSourceAsset::OpenPageManifest(NanoGS::Paged::FManifest& OutManifest, FString& OutError) const
{
	if (!HasValidMetadata())
	{
		OutError = TEXT("Spatial LOD source asset metadata is invalid");
		return false;
	}
	NanoGS::Paged::FReadOptions Options;
	Options.bVerifyMetadataCRCs = true;
	if (!NanoGS::Paged::FPageFileReader::ReadManifest(
		FPaths::Combine(GetResolvedDirectory(), TEXT("sjtu_spatial_lod.ngsp")), OutManifest, OutError, Options))
		return false;
	if (OutManifest.Pages.Num() != PageCount || static_cast<int64>(OutManifest.Header.TotalSplatCount) != TotalRepresentationSplats)
	{
		OutError = TEXT("Spatial LOD combined Page container changed after asset initialization");
		return false;
	}
	return true;
}
