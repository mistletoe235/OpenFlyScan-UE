// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSSpatialLODReader.h"

#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace NanoGS::SpatialLOD
{
namespace
{
	uint16 ReadU16LE(const uint8* Data)
	{
		return static_cast<uint16>(Data[0]) |
			(static_cast<uint16>(Data[1]) << 8u);
	}

	uint32 ReadU32LE(const uint8* Data)
	{
		return static_cast<uint32>(Data[0]) |
			(static_cast<uint32>(Data[1]) << 8u) |
			(static_cast<uint32>(Data[2]) << 16u) |
			(static_cast<uint32>(Data[3]) << 24u);
	}

	uint64 ReadU64LE(const uint8* Data)
	{
		return static_cast<uint64>(ReadU32LE(Data)) |
			(static_cast<uint64>(ReadU32LE(Data + 4)) << 32u);
	}

	float ReadF32LE(const uint8* Data)
	{
		const uint32 Bits = ReadU32LE(Data);
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}

	bool HasMagic(const TArray64<uint8>& Bytes, const uint8 (&Magic)[8])
	{
		return Bytes.Num() >= 8 && FMemory::Memcmp(Bytes.GetData(), Magic, 8) == 0;
	}

	bool LoadFile(const FString& Filename, TArray64<uint8>& OutBytes, FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const int64 FileBytes = PlatformFile.FileSize(*Filename);
		if (FileBytes < 0)
		{
			OutError = FString::Printf(TEXT("Spatial LOD file is missing: %s"), *Filename);
			return false;
		}
		TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Filename));
		if (!Handle)
		{
			OutError = FString::Printf(TEXT("Spatial LOD file cannot be opened: %s"), *Filename);
			return false;
		}
		OutBytes.SetNumUninitialized(FileBytes);
		if (FileBytes > 0 && !Handle->Read(OutBytes.GetData(), FileBytes))
		{
			OutBytes.Reset();
			OutError = FString::Printf(TEXT("Spatial LOD file read failed: %s"), *Filename);
			return false;
		}
		return true;
	}

	bool DecodeTableHeader(
		const TArray64<uint8>& Bytes,
		const uint8 (&Magic)[8],
		FTableHeader& OutHeader,
		FString& OutError)
	{
		if (Bytes.Num() < static_cast<int64>(TableHeaderBytes) || !HasMagic(Bytes, Magic))
		{
			OutError = TEXT("Spatial LOD table has an invalid header or magic");
			return false;
		}
		const uint8* Data = Bytes.GetData();
		OutHeader.Version = ReadU32LE(Data + 8);
		OutHeader.Count = ReadU32LE(Data + 12);
		OutHeader.Aux0 = ReadU32LE(Data + 16);
		OutHeader.RecordSize = ReadU32LE(Data + 20);
		OutHeader.Levels = ReadU32LE(Data + 24);
		OutHeader.Flags = ReadU32LE(Data + 28);
		if (OutHeader.Version != FormatVersion)
		{
			OutError = FString::Printf(TEXT("Unsupported Spatial LOD version: %u"), OutHeader.Version);
			return false;
		}
		return true;
	}

	bool CheckedTableBytes(uint32 Count, uint32 RecordSize, uint64& OutBytes)
	{
		if (RecordSize == 0 || Count > (MAX_uint64 - TableHeaderBytes) / RecordSize)
			return false;
		OutBytes = TableHeaderBytes + static_cast<uint64>(Count) * RecordSize;
		return true;
	}

	bool DecodeNodes(const TArray64<uint8>& Bytes, FManifest& Manifest, FString& OutError)
	{
		FTableHeader Header;
		if (!DecodeTableHeader(Bytes, NodeMagic, Header, OutError))
			return false;
		if (Header.RecordSize != NodeRecordBytes || Header.Count == 0)
		{
			OutError = TEXT("Spatial LOD node table dimensions are invalid");
			return false;
		}
		uint64 RequiredBytes = 0;
		if (!CheckedTableBytes(Header.Count, Header.RecordSize, RequiredBytes) ||
			RequiredBytes != static_cast<uint64>(Bytes.Num()))
		{
			OutError = TEXT("Spatial LOD node table byte size is invalid");
			return false;
		}
		Manifest.RootNode = Header.Aux0;
		Manifest.LevelCount = Header.Levels;
		Manifest.Nodes.SetNum(Header.Count);
		for (uint32 Index = 0; Index < Header.Count; ++Index)
		{
			const uint8* Record = Bytes.GetData() + TableHeaderBytes + Index * NodeRecordBytes;
			FNode& Node = Manifest.Nodes[Index];
			Node.Parent = ReadU32LE(Record + 0);
			Node.FirstChild = ReadU32LE(Record + 4);
			Node.ChildCount = ReadU16LE(Record + 8);
			Node.Depth = ReadU16LE(Record + 10);
			Node.Flags = static_cast<ENodeFlags>(ReadU32LE(Record + 12));
			Node.BoundsMin = FVector3f(ReadF32LE(Record + 16), ReadF32LE(Record + 20), ReadF32LE(Record + 24));
			Node.BoundsMax = FVector3f(ReadF32LE(Record + 28), ReadF32LE(Record + 32), ReadF32LE(Record + 36));
			Node.FirstLeafCluster = ReadU32LE(Record + 40);
			Node.LeafClusterCount = ReadU32LE(Record + 44);
			if (!FMath::IsFinite(Node.BoundsMin.X) || !FMath::IsFinite(Node.BoundsMin.Y) ||
				!FMath::IsFinite(Node.BoundsMin.Z) || !FMath::IsFinite(Node.BoundsMax.X) ||
				!FMath::IsFinite(Node.BoundsMax.Y) || !FMath::IsFinite(Node.BoundsMax.Z) ||
				Node.BoundsMin.X > Node.BoundsMax.X ||
				Node.BoundsMin.Y > Node.BoundsMax.Y ||
				Node.BoundsMin.Z > Node.BoundsMax.Z)
			{
				OutError = FString::Printf(TEXT("Spatial LOD node %u has invalid bounds"), Index);
				return false;
			}
			if (Node.IsEnvironment())
			{
				if (Manifest.EnvironmentNode != MAX_uint32)
				{
					OutError = TEXT("Spatial LOD contains multiple environment nodes");
					return false;
				}
				Manifest.EnvironmentNode = Index;
			}
		}
		if (Manifest.RootNode >= static_cast<uint32>(Manifest.Nodes.Num()))
		{
			OutError = TEXT("Spatial LOD root node is out of range");
			return false;
		}
		for (uint32 Index = 0; Index < static_cast<uint32>(Manifest.Nodes.Num()); ++Index)
		{
			const FNode& Node = Manifest.Nodes[Index];
			if (Index == Manifest.RootNode)
			{
				if (Node.Parent != MAX_uint32)
				{
					OutError = TEXT("Spatial LOD root parent is invalid");
					return false;
				}
			}
			else if (Node.Parent >= static_cast<uint32>(Manifest.Nodes.Num()))
			{
				OutError = FString::Printf(TEXT("Spatial LOD node %u parent is out of range"), Index);
				return false;
			}
			if (Node.IsLeaf())
			{
				if (Node.ChildCount != 0)
				{
					OutError = FString::Printf(TEXT("Spatial LOD leaf node %u has children"), Index);
					return false;
				}
			}
			else if (Node.ChildCount == 0 ||
				static_cast<uint64>(Node.FirstChild) + Node.ChildCount > static_cast<uint64>(Manifest.Nodes.Num()))
			{
				OutError = FString::Printf(TEXT("Spatial LOD internal node %u child range is invalid"), Index);
				return false;
			}
			for (uint32 ChildOffset = 0; ChildOffset < Node.ChildCount; ++ChildOffset)
			{
				const uint32 ChildIndex = Node.FirstChild + ChildOffset;
				if (Manifest.Nodes[ChildIndex].Parent != Index)
				{
					OutError = FString::Printf(TEXT("Spatial LOD node %u child parent mismatch"), Index);
					return false;
				}
			}
		}
		return true;
	}

	bool DecodeRanges(const TArray64<uint8>& Bytes, FManifest& Manifest, FString& OutError)
	{
		FTableHeader Header;
		if (!DecodeTableHeader(Bytes, RangeMagic, Header, OutError))
			return false;
		const uint64 ExpectedCount = static_cast<uint64>(Manifest.Nodes.Num()) * Manifest.LevelCount;
		if (Header.RecordSize != RangeRecordBytes || Header.Count != Manifest.Nodes.Num() ||
			Header.Levels != Manifest.LevelCount || Header.Aux0 != Manifest.LevelCount)
		{
			OutError = TEXT("Spatial LOD range table dimensions are invalid");
			return false;
		}
		uint64 RequiredBytes = 0;
		if (ExpectedCount > MAX_uint32 ||
			!CheckedTableBytes(static_cast<uint32>(ExpectedCount), Header.RecordSize, RequiredBytes) ||
			RequiredBytes != static_cast<uint64>(Bytes.Num()))
		{
			OutError = TEXT("Spatial LOD range table byte size is invalid");
			return false;
		}
		Manifest.Ranges.SetNum(static_cast<int32>(ExpectedCount));
		for (uint64 Index = 0; Index < ExpectedCount; ++Index)
		{
			const uint8* Record = Bytes.GetData() + TableHeaderBytes + Index * RangeRecordBytes;
			FNodeLodRange& Range = Manifest.Ranges[static_cast<int32>(Index)];
			Range.ByteOffset = ReadU64LE(Record + 0);
			Range.SplatCount = ReadU32LE(Record + 8);
			Range.Reserved = ReadU32LE(Record + 12);
			if (Range.Reserved != 0)
			{
				OutError = FString::Printf(TEXT("Spatial LOD range %llu reserved field is nonzero"), Index);
				return false;
			}
		}
		return true;
	}

	bool DecodeErrors(const TArray64<uint8>& Bytes, FManifest& Manifest, FString& OutError)
	{
		FTableHeader Header;
		if (!DecodeTableHeader(Bytes, ErrorMagic, Header, OutError))
			return false;
		const uint64 ExpectedCount = static_cast<uint64>(Manifest.Nodes.Num()) * Manifest.LevelCount;
		if (Header.RecordSize != ErrorRecordBytes || Header.Count != Manifest.Nodes.Num() ||
			Header.Levels != Manifest.LevelCount || Header.Aux0 != Manifest.LevelCount)
		{
			OutError = TEXT("Spatial LOD error table dimensions are invalid");
			return false;
		}
		uint64 RequiredBytes = 0;
		if (ExpectedCount > MAX_uint32 ||
			!CheckedTableBytes(static_cast<uint32>(ExpectedCount), Header.RecordSize, RequiredBytes) ||
			RequiredBytes != static_cast<uint64>(Bytes.Num()))
		{
			OutError = TEXT("Spatial LOD error table byte size is invalid");
			return false;
		}
		Manifest.Errors.SetNum(static_cast<int32>(ExpectedCount));
		for (uint64 Index = 0; Index < ExpectedCount; ++Index)
		{
			const uint8* Record = Bytes.GetData() + TableHeaderBytes + Index * ErrorRecordBytes;
			FNodeLodError& Error = Manifest.Errors[static_cast<int32>(Index)];
			Error.MaxSafeProjectedRadiusPx = ReadF32LE(Record + 0);
			Error.RmseThreshold = ReadF32LE(Record + 4);
			Error.MeasuredP99Error = ReadF32LE(Record + 8);
			Error.MeasuredSsimLoss = ReadF32LE(Record + 12);
			if (Error.IsCalibrated() && Error.MaxSafeProjectedRadiusPx < 0.0f)
			{
				OutError = FString::Printf(TEXT("Spatial LOD error %llu has a negative safe radius"), Index);
				return false;
			}
		}
		return true;
	}

	bool DecodeLodHeader(
		const FString& Filename,
		uint32 ExpectedLevel,
		FLodHeader& OutHeader,
		uint64& OutFileBytes,
		FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const int64 FileBytes = PlatformFile.FileSize(*Filename);
		if (FileBytes < static_cast<int64>(LodHeaderBytes))
		{
			OutError = FString::Printf(TEXT("Spatial LOD payload is missing or truncated: %s"), *Filename);
			return false;
		}
		TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Filename));
		uint8 HeaderBytes[LodHeaderBytes] = {};
		if (!Handle || !Handle->Read(HeaderBytes, LodHeaderBytes) ||
			FMemory::Memcmp(HeaderBytes, LodMagic, 8) != 0)
		{
			OutError = FString::Printf(TEXT("Spatial LOD payload header is invalid: %s"), *Filename);
			return false;
		}
		OutHeader.Version = ReadU32LE(HeaderBytes + 8);
		OutHeader.Level = ReadU32LE(HeaderBytes + 12);
		OutHeader.SplatCount = ReadU32LE(HeaderBytes + 16);
		OutHeader.StrideBytes = ReadU32LE(HeaderBytes + 20);
		OutHeader.PropertyCount = ReadU32LE(HeaderBytes + 24);
		OutHeader.Flags = ReadU32LE(HeaderBytes + 28);
		OutHeader.DataOffset = ReadU64LE(HeaderBytes + 32);
		OutHeader.Reserved = ReadU64LE(HeaderBytes + 40);
		if (OutHeader.Version != FormatVersion || OutHeader.Level != ExpectedLevel ||
			OutHeader.StrideBytes != SplatStrideBytes ||
			OutHeader.PropertyCount != SplatPropertyCount ||
			OutHeader.DataOffset < LodHeaderBytes || OutHeader.Reserved != 0)
		{
			OutError = FString::Printf(TEXT("Spatial LOD payload contract mismatch: %s"), *Filename);
			return false;
		}
		const uint64 ExpectedBytes = OutHeader.DataOffset +
			static_cast<uint64>(OutHeader.SplatCount) * OutHeader.StrideBytes;
		if (ExpectedBytes != static_cast<uint64>(FileBytes))
		{
			OutError = FString::Printf(TEXT("Spatial LOD payload byte size mismatch: %s"), *Filename);
			return false;
		}
		OutFileBytes = FileBytes;
		return true;
	}

	bool ValidateRanges(FManifest& Manifest, FString& OutError)
	{
		Manifest.PageIds.Init(INDEX_NONE, Manifest.Ranges.Num());
		int32 NextPageId = 0;
		for (uint32 NodeIndex = 0; NodeIndex < static_cast<uint32>(Manifest.Nodes.Num()); ++NodeIndex)
		{
			const FNode& Node = Manifest.Nodes[NodeIndex];
			for (uint32 Level = 0; Level < Manifest.LevelCount; ++Level)
			{
				const FNodeLodRange& Range = *Manifest.FindRange(NodeIndex, Level);
				if (!Node.IsLeaf())
				{
					if (Range.ByteOffset != 0 || Range.SplatCount != 0)
					{
						OutError = FString::Printf(TEXT("Spatial LOD internal node %u has payload"), NodeIndex);
						return false;
					}
					continue;
				}
				if (Range.SplatCount == 0 || Range.ByteOffset < Manifest.LodHeaders[Level].DataOffset)
				{
					OutError = FString::Printf(TEXT("Spatial LOD leaf node %u level %u range is empty or before payload"), NodeIndex, Level);
					return false;
				}
				const uint64 ByteCount = static_cast<uint64>(Range.SplatCount) * SplatStrideBytes;
				if (Range.ByteOffset > MAX_uint64 - ByteCount ||
					Range.ByteOffset + ByteCount > Manifest.LodFileBytes[Level])
				{
					OutError = FString::Printf(TEXT("Spatial LOD leaf node %u level %u range exceeds payload"), NodeIndex, Level);
					return false;
				}
				const uint64 MappingIndex = static_cast<uint64>(NodeIndex) * Manifest.LevelCount + Level;
				Manifest.PageIds[static_cast<int32>(MappingIndex)] = NextPageId++;
			}
		}
		return true;
	}

	bool ReadMetadataTables(const FString& RootDirectory, FManifest& OutManifest, FString& OutError)
	{
		OutManifest = FManifest{};
		OutError.Reset();
		OutManifest.RootDirectory = FPaths::ConvertRelativePathToFull(RootDirectory);

		TArray64<uint8> Bytes;
		if (!LoadFile(FPaths::Combine(OutManifest.RootDirectory, TEXT("nodes.bin")), Bytes, OutError) ||
			!DecodeNodes(Bytes, OutManifest, OutError))
			return false;
		if (OutManifest.LevelCount == 0 || OutManifest.LevelCount > 32)
		{
			OutError = TEXT("Spatial LOD level count is invalid");
			return false;
		}
		if (!LoadFile(FPaths::Combine(OutManifest.RootDirectory, TEXT("node_lod_ranges.bin")), Bytes, OutError) ||
			!DecodeRanges(Bytes, OutManifest, OutError))
			return false;
		if (!LoadFile(FPaths::Combine(OutManifest.RootDirectory, TEXT("node_lod_errors.bin")), Bytes, OutError) ||
			!DecodeErrors(Bytes, OutManifest, OutError))
			return false;
		return true;
	}

	bool BuildRuntimeLodSummaries(FManifest& Manifest, FString& OutError)
	{
		Manifest.LodHeaders.SetNum(Manifest.LevelCount);
		Manifest.LodFileBytes.SetNum(Manifest.LevelCount);
		for (uint32 Level = 0; Level < Manifest.LevelCount; ++Level)
		{
			struct FRuntimeRange
			{
				uint64 ByteOffset = 0;
				uint32 SplatCount = 0;
			};
			TArray<FRuntimeRange> RuntimeRanges;
			uint64 SplatCount = 0;
			for (uint32 NodeIndex = 0; NodeIndex < static_cast<uint32>(Manifest.Nodes.Num()); ++NodeIndex)
			{
				const FNodeLodRange& Range = *Manifest.FindRange(NodeIndex, Level);
				if (!Manifest.Nodes[NodeIndex].IsLeaf())
					continue;
				if (Range.SplatCount == 0 || SplatCount > MAX_uint32 - Range.SplatCount)
				{
					OutError = FString::Printf(TEXT("Spatial LOD runtime range layout is invalid at node %u level %u"), NodeIndex, Level);
					return false;
				}
				RuntimeRanges.Add({Range.ByteOffset, Range.SplatCount});
				SplatCount += Range.SplatCount;
			}
			RuntimeRanges.Sort([](const FRuntimeRange& A, const FRuntimeRange& B)
			{
				return A.ByteOffset < B.ByteOffset;
			});

			uint64 ExpectedOffset = LodHeaderBytes;
			for (const FRuntimeRange& Range : RuntimeRanges)
			{
				const uint64 ByteCount = static_cast<uint64>(Range.SplatCount) * SplatStrideBytes;
				if (Range.ByteOffset != ExpectedOffset || ExpectedOffset > MAX_uint64 - ByteCount)
				{
					OutError = FString::Printf(TEXT("Spatial LOD runtime level %u ranges are overlapping or non-contiguous"), Level);
					return false;
				}
				ExpectedOffset += ByteCount;
			}

			FLodHeader& Header = Manifest.LodHeaders[Level];
			Header.Version = FormatVersion;
			Header.Level = Level;
			Header.SplatCount = static_cast<uint32>(SplatCount);
			Header.StrideBytes = SplatStrideBytes;
			Header.PropertyCount = SplatPropertyCount;
			Header.DataOffset = LodHeaderBytes;
			Manifest.LodFileBytes[Level] = ExpectedOffset;
		}
		return ValidateRanges(Manifest, OutError);
	}
}

const FNodeLodRange* FManifest::FindRange(uint32 NodeIndex, uint32 Level) const
{
	if (NodeIndex >= static_cast<uint32>(Nodes.Num()) || Level >= LevelCount)
		return nullptr;
	const uint64 Index = static_cast<uint64>(NodeIndex) * LevelCount + Level;
	return Ranges.IsValidIndex(static_cast<int32>(Index)) ? &Ranges[static_cast<int32>(Index)] : nullptr;
}

int32 FManifest::FindPageId(uint32 NodeIndex, uint32 Level) const
{
	if (NodeIndex >= static_cast<uint32>(Nodes.Num()) || Level >= LevelCount)
		return INDEX_NONE;
	const uint64 Index = static_cast<uint64>(NodeIndex) * LevelCount + Level;
	return PageIds.IsValidIndex(static_cast<int32>(Index)) ? PageIds[static_cast<int32>(Index)] : INDEX_NONE;
}

const FNodeLodError* FManifest::FindError(uint32 NodeIndex, uint32 Level) const
{
	if (NodeIndex >= static_cast<uint32>(Nodes.Num()) || Level >= LevelCount)
		return nullptr;
	const uint64 Index = static_cast<uint64>(NodeIndex) * LevelCount + Level;
	return Errors.IsValidIndex(static_cast<int32>(Index)) ? &Errors[static_cast<int32>(Index)] : nullptr;
}

bool FReader::ReadManifest(const FString& RootDirectory, FManifest& OutManifest, FString& OutError)
{
	if (!ReadMetadataTables(RootDirectory, OutManifest, OutError))
		return false;

	OutManifest.LodHeaders.SetNum(OutManifest.LevelCount);
	OutManifest.LodFileBytes.SetNum(OutManifest.LevelCount);
	for (uint32 Level = 0; Level < OutManifest.LevelCount; ++Level)
	{
		const FString Filename = FPaths::Combine(
			OutManifest.RootDirectory, FString::Printf(TEXT("lod%u.bin"), Level));
		if (!DecodeLodHeader(Filename, Level, OutManifest.LodHeaders[Level],
				OutManifest.LodFileBytes[Level], OutError))
			return false;
	}
	return ValidateRanges(OutManifest, OutError);
}

bool FReader::ReadRuntimeManifest(const FString& RootDirectory, FManifest& OutManifest, FString& OutError)
{
	if (!ReadMetadataTables(RootDirectory, OutManifest, OutError))
		return false;
	return BuildRuntimeLodSummaries(OutManifest, OutError);
}

bool FReader::ReadNodeLodPayload(
	const FManifest& Manifest,
	uint32 NodeIndex,
	uint32 Level,
	TArray64<uint8>& OutBytes,
	FString& OutError)
{
	OutBytes.Reset();
	OutError.Reset();
	const FNodeLodRange* Range = Manifest.FindRange(NodeIndex, Level);
	if (Range == nullptr || Range->SplatCount == 0 || Level >= static_cast<uint32>(Manifest.LodFileBytes.Num()))
	{
		OutError = TEXT("Spatial LOD node or level has no payload range");
		return false;
	}
	const uint64 ByteCount = static_cast<uint64>(Range->SplatCount) * SplatStrideBytes;
	if (ByteCount > MAX_int64 || Range->ByteOffset > MAX_int64)
	{
		OutError = TEXT("Spatial LOD payload range exceeds platform limits");
		return false;
	}
	const FString Filename = FPaths::Combine(
		Manifest.RootDirectory, FString::Printf(TEXT("lod%u.bin"), Level));
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Filename));
	if (!Handle || !Handle->Seek(Range->ByteOffset))
	{
		OutError = FString::Printf(TEXT("Spatial LOD payload seek failed: %s"), *Filename);
		return false;
	}
	OutBytes.SetNumUninitialized(static_cast<int64>(ByteCount));
	if (!Handle->Read(OutBytes.GetData(), ByteCount))
	{
		OutBytes.Reset();
		OutError = FString::Printf(TEXT("Spatial LOD payload read failed: %s"), *Filename);
		return false;
	}
	return true;
}
}
