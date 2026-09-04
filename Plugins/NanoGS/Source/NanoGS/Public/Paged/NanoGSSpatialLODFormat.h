// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace NanoGS::SpatialLOD
{
	inline constexpr uint8 NodeMagic[8] = {'L', 'X', 'L', 'N', 'O', 'D', 'E', 0};
	inline constexpr uint8 RangeMagic[8] = {'L', 'X', 'L', 'R', 'A', 'N', 'G', 'E'};
	inline constexpr uint8 ErrorMagic[8] = {'L', 'X', 'L', 'E', 'R', 'R', 'O', 'R'};
	inline constexpr uint8 LodMagic[8] = {'L', 'X', 'L', 'L', 'O', 'D', 0, 0};
	inline constexpr uint32 FormatVersion = 1;
	inline constexpr uint64 TableHeaderBytes = 32;
	inline constexpr uint64 LodHeaderBytes = 64;
	inline constexpr uint64 NodeRecordBytes = 48;
	inline constexpr uint64 RangeRecordBytes = 16;
	inline constexpr uint64 ErrorRecordBytes = 16;
	inline constexpr uint32 SplatStrideBytes = 248;
	inline constexpr uint32 SplatPropertyCount = 62;

	enum class ENodeFlags : uint32
	{
		None = 0,
		Leaf = 1u << 0u,
		Environment = 1u << 1u,
	};

	ENUM_CLASS_FLAGS(ENodeFlags);

	struct NANOGS_API FTableHeader
	{
		uint32 Version = 0;
		uint32 Count = 0;
		uint32 Aux0 = 0;
		uint32 RecordSize = 0;
		uint32 Levels = 0;
		uint32 Flags = 0;
	};

	struct NANOGS_API FLodHeader
	{
		uint32 Version = 0;
		uint32 Level = 0;
		uint32 SplatCount = 0;
		uint32 StrideBytes = 0;
		uint32 PropertyCount = 0;
		uint32 Flags = 0;
		uint64 DataOffset = 0;
		uint64 Reserved = 0;
	};

	struct NANOGS_API FNode
	{
		uint32 Parent = MAX_uint32;
		uint32 FirstChild = 0;
		uint16 ChildCount = 0;
		uint16 Depth = 0;
		ENodeFlags Flags = ENodeFlags::None;
		FVector3f BoundsMin = FVector3f::ZeroVector;
		FVector3f BoundsMax = FVector3f::ZeroVector;
		uint32 FirstLeafCluster = 0;
		uint32 LeafClusterCount = 0;

		bool IsLeaf() const { return EnumHasAnyFlags(Flags, ENodeFlags::Leaf); }
		bool IsEnvironment() const { return EnumHasAnyFlags(Flags, ENodeFlags::Environment); }
	};

	struct NANOGS_API FNodeLodRange
	{
		uint64 ByteOffset = 0;
		uint32 SplatCount = 0;
		uint32 Reserved = 0;
	};

	struct NANOGS_API FNodeLodError
	{
		float MaxSafeProjectedRadiusPx = NAN;
		float RmseThreshold = NAN;
		float MeasuredP99Error = NAN;
		float MeasuredSsimLoss = NAN;

		bool IsCalibrated() const { return FMath::IsFinite(MaxSafeProjectedRadiusPx); }
	};

	struct NANOGS_API FManifest
	{
		FString RootDirectory;
		uint32 RootNode = 0;
		uint32 LevelCount = 0;
		uint32 EnvironmentNode = MAX_uint32;
		TArray<FNode> Nodes;
		TArray<FNodeLodRange> Ranges;
		TArray<FNodeLodError> Errors;
		/** Runtime-only node-major/level-minor mapping into the combined Page v1 container. */
		TArray<int32> PageIds;
		TArray<FLodHeader> LodHeaders;
		TArray<uint64> LodFileBytes;

		const FNodeLodRange* FindRange(uint32 NodeIndex, uint32 Level) const;
		const FNodeLodError* FindError(uint32 NodeIndex, uint32 Level) const;
		int32 FindPageId(uint32 NodeIndex, uint32 Level) const;
	};
}
