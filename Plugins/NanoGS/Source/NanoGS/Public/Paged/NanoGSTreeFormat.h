// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace NanoGS::Tree
{
	inline constexpr uint32 FormatVersion = 2;
	inline constexpr uint32 EndianMarker = 0x01020304;
	inline constexpr uint32 HeaderBytes = 128;
	inline constexpr uint32 NodeRecordBytes = 64;
	inline constexpr uint64 InvalidSourceId = MAX_uint64;

	enum class ETreeFlags : uint32
	{
		None = 0,
		ExactLeaves = 1u << 0,
		InflatedParents = 1u << 1,
	};
	ENUM_CLASS_FLAGS(ETreeFlags);

	enum class ENodeFlags : uint32
	{
		None = 0,
		Internal = 1u << 0,
		ExactLeaf = 1u << 1,
	};
	ENUM_CLASS_FLAGS(ENodeFlags);

	struct NANOGS_API FHeader
	{
		ETreeFlags Flags = ETreeFlags::None;
		uint32 PagePoints = 0;
		uint64 NodeCount = 0;
		uint64 RootNode = 0;
		uint64 InternalNodeCount = 0;
		uint64 ExactLeafCount = 0;
		uint64 SourceInputSplatCount = 0;
		uint64 NodesOffset = 0;
		uint64 FileBytes = 0;
		uint32 NodesCRC32 = 0;
		uint32 HeaderCRC32 = 0;
	};

	struct NANOGS_API FNode
	{
		uint32 FirstChild = 0;
		uint16 ChildCount = 0;
		uint16 Depth = 0;
		uint64 SourceId = InvalidSourceId;
		uint32 PageId = 0;
		uint32 PageLocal = 0;
		FVector3f CenterCm = FVector3f::ZeroVector;
		float FeatureRadiusCm = 0.0f;
		float SupportRadiusCm = 0.0f;
		ENodeFlags Flags = ENodeFlags::None;
		uint32 SubtreeLeafCount = 0;

		bool IsInternal() const { return EnumHasAnyFlags(Flags, ENodeFlags::Internal); }
		bool IsExactLeaf() const { return EnumHasAnyFlags(Flags, ENodeFlags::ExactLeaf); }
	};

	struct NANOGS_API FManifest
	{
		FString SourceFilename;
		FHeader Header;
		TArray<FNode> Nodes;
	};
}
