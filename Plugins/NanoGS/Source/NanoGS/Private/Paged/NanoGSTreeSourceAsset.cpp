// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSTreeSourceAsset.h"

#include "Misc/Paths.h"

namespace
{
	constexpr TCHAR TreeRequiredPrefix[] = TEXT("Content/NanoGSData/");

	FString NormalizeTreeFull(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Result);
		FPaths::CollapseRelativeDirectories(Result);
		return Result;
	}
}

bool UNanoGSTreeSourceAsset::InitializeFromDirectory(const FString& Directory, FString& OutError)
{
	OutError.Reset();
	const FString ContentRoot = NormalizeTreeFull(FPaths::ProjectContentDir());
	const FString ProjectRoot = FPaths::GetPath(ContentRoot);
	const FString Resolved = FPaths::IsRelative(Directory)
		? NormalizeTreeFull(FPaths::Combine(ProjectRoot, Directory))
		: NormalizeTreeFull(Directory);
	if (!FPaths::IsUnderDirectory(Resolved, ProjectRoot))
	{
		OutError = TEXT("Tree v2 directory must be inside ProjectDir");
		return false;
	}
	FString Relative = Resolved.RightChop(ProjectRoot.Len() + 1);
	FPaths::NormalizeFilename(Relative);
	if (!Relative.StartsWith(TreeRequiredPrefix, ESearchCase::CaseSensitive) || Relative.Contains(TEXT("..")))
	{
		OutError = TEXT("Tree v2 directory must be below Content/NanoGSData");
		return false;
	}
	NanoGS::Tree::FManifest TreeManifest;
	if (!NanoGS::Tree::FReader::ReadManifest(FPaths::Combine(Resolved, TEXT("tree.ngst")), TreeManifest, OutError))
		return false;
	NanoGS::Paged::FManifest PageManifest;
	NanoGS::Paged::FReadOptions Options;
	Options.bVerifyMetadataCRCs = true;
	if (!NanoGS::Paged::FPageFileReader::ReadManifest(FPaths::Combine(Resolved, TEXT("tree_nodes.ngsp")), PageManifest, OutError, Options) ||
		!NanoGS::Tree::FReader::ValidatePageMapping(TreeManifest, PageManifest, OutError))
		return false;
	return ApplyManifest(Relative, TreeManifest, PageManifest, OutError);
}

bool UNanoGSTreeSourceAsset::Refresh(FString& OutError)
{
	return InitializeFromDirectory(GetResolvedDirectory(), OutError);
}

FString UNanoGSTreeSourceAsset::GetResolvedDirectory() const
{
	if (!ProjectRelativeDirectory.StartsWith(TEXT("Content/"), ESearchCase::CaseSensitive))
		return FString();
	return NormalizeTreeFull(FPaths::Combine(FPaths::ProjectContentDir(), ProjectRelativeDirectory.RightChop(8)));
}

bool UNanoGSTreeSourceAsset::ApplyManifest(const FString& RelativeDirectory, const NanoGS::Tree::FManifest& TreeManifest, const NanoGS::Paged::FManifest& PageManifest, FString& OutError)
{
	if (TreeManifest.Nodes.IsEmpty() || PageManifest.Pages.IsEmpty())
	{
		OutError = TEXT("Tree v2 manifest summary is incomplete");
		return false;
	}
	ProjectRelativeDirectory = RelativeDirectory;
	NodeCount = static_cast<int64>(TreeManifest.Header.NodeCount);
	ExactLeafCount = static_cast<int64>(TreeManifest.Header.ExactLeafCount);
	SourceInputSplatCount = static_cast<int64>(TreeManifest.Header.SourceInputSplatCount);
	PageCount = PageManifest.Pages.Num();
	PagePoints = static_cast<int32>(TreeManifest.Header.PagePoints);
	SceneBounds = FBox(FVector(PageManifest.Header.SceneBoundsMin), FVector(PageManifest.Header.SceneBoundsMax));
	MetadataFingerprint = FString::Printf(TEXT("T2-%08X-%08X-%llu-%llu-%u-%08X-%08X"),
		TreeManifest.Header.HeaderCRC32, TreeManifest.Header.NodesCRC32,
		TreeManifest.Header.NodeCount, TreeManifest.Header.ExactLeafCount, TreeManifest.Header.PagePoints,
		PageManifest.Header.HeaderCRC32, PageManifest.Header.DirectoryCRC32);
	bMetadataValid = true;
	return true;
}

bool UNanoGSTreeSourceAsset::HasValidMetadata() const
{
	return bMetadataValid && NodeCount > 0 && ExactLeafCount > 0 && SourceInputSplatCount > 0 &&
		PageCount > 0 && PagePoints > 0 && SceneBounds.IsValid && !MetadataFingerprint.IsEmpty() && !GetResolvedDirectory().IsEmpty();
}

bool UNanoGSTreeSourceAsset::OpenTreeManifest(NanoGS::Tree::FManifest& OutManifest, FString& OutError) const
{
	if (!HasValidMetadata() || !NanoGS::Tree::FReader::ReadManifest(FPaths::Combine(GetResolvedDirectory(), TEXT("tree.ngst")), OutManifest, OutError))
		return false;
	if (static_cast<int64>(OutManifest.Header.NodeCount) != NodeCount ||
		static_cast<int64>(OutManifest.Header.ExactLeafCount) != ExactLeafCount ||
		static_cast<int64>(OutManifest.Header.SourceInputSplatCount) != SourceInputSplatCount ||
		static_cast<int32>(OutManifest.Header.PagePoints) != PagePoints)
	{
		OutError = TEXT("Tree v2 metadata changed after asset initialization");
		return false;
	}
	return true;
}

bool UNanoGSTreeSourceAsset::OpenPageManifest(NanoGS::Paged::FManifest& OutManifest, FString& OutError) const
{
	if (!HasValidMetadata())
	{
		OutError = TEXT("Tree v2 source asset metadata is invalid");
		return false;
	}
	NanoGS::Paged::FReadOptions Options;
	Options.bVerifyMetadataCRCs = true;
	if (!NanoGS::Paged::FPageFileReader::ReadManifest(FPaths::Combine(GetResolvedDirectory(), TEXT("tree_nodes.ngsp")), OutManifest, OutError, Options))
		return false;
	if (OutManifest.Pages.Num() != PageCount || static_cast<int64>(OutManifest.Header.TotalSplatCount) != NodeCount)
	{
		OutError = TEXT("Tree v2 Page v1 metadata changed after asset initialization");
		return false;
	}
	return true;
}
