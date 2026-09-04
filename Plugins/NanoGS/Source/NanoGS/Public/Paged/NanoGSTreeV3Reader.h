// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Paged/NanoGSTreeGPUData.h"

namespace NanoGS::TreeV3
{
	inline constexpr uint32 FormatVersion = 3;
	inline constexpr uint32 BlockProxyFlag = 1u << 18u;
	inline constexpr uint32 BlockRecordBytes = 32;

	struct NANOGS_API FBlockRecord
	{
		uint32 SourceRootNode = 0;
		uint32 FirstNode = 0;
		uint32 NodeCount = 0;
		uint32 LeafCount = 0;
		uint32 MaxDepth = 0;
		uint32 RootPageId = 0;
		uint32 FirstPage = 0;
		uint32 PageCount = 0;
	};

	struct NANOGS_API FManifest
	{
		FString Directory;
		FString CacheKey;
		FString BlockDataFilename;
		uint32 MaxBlockLeaves = 0;
		uint32 MaxBlockNodes = 0;
		TArray<NanoGS::Tree::FGPUNodeRecord> SkeletonNodes;
		TArray<FBlockRecord> Blocks;
		TArray<uint32> BlockPageIds;
	};

	class NANOGS_API FReader
	{
	public:
		static bool ReadFromDirectory(const FString& Directory, FManifest& OutManifest, FString& OutError);
	};
}
