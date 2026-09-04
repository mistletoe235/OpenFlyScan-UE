// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSTreeReader.h"

#include "HAL/PlatformFileManager.h"
#include "Paged/NanoGSPageReader.h"

namespace NanoGS::Tree
{
namespace
{
	constexpr uint8 Magic[8] = {'N', 'G', 'S', 'T', 'R', 'E', 'E', '2'};

	uint16 ReadU16(const uint8* Data) { return static_cast<uint16>(Data[0]) | static_cast<uint16>(Data[1]) << 8; }
	uint32 ReadU32(const uint8* Data) { return static_cast<uint32>(Data[0]) | static_cast<uint32>(Data[1]) << 8 | static_cast<uint32>(Data[2]) << 16 | static_cast<uint32>(Data[3]) << 24; }
	uint64 ReadU64(const uint8* Data) { return static_cast<uint64>(ReadU32(Data)) | static_cast<uint64>(ReadU32(Data + 4)) << 32; }
	float ReadF32(const uint8* Data) { const uint32 Bits = ReadU32(Data); float Value; FMemory::Memcpy(&Value, &Bits, sizeof(Value)); return Value; }

	bool CheckedNodeBytes(uint64 Count, uint64& OutBytes)
	{
		if (Count > MAX_uint64 / NodeRecordBytes)
			return false;
		OutBytes = Count * NodeRecordBytes;
		return true;
	}
}

bool FReader::ReadManifest(const FString& Filename, FManifest& OutManifest, FString& OutError)
{
	OutManifest = FManifest();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const int64 PhysicalFileBytes = PlatformFile.FileSize(*Filename);
	TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*Filename));
	if (!FileHandle || PhysicalFileBytes < static_cast<int64>(HeaderBytes))
	{
		OutError = FString::Printf(TEXT("Unable to read Tree v2 file: %s"), *Filename);
		return false;
	}
	uint8 HeaderStorage[HeaderBytes];
	if (!FileHandle->Read(HeaderStorage, HeaderBytes) ||
		FMemory::Memcmp(HeaderStorage, Magic, sizeof(Magic)) != 0)
	{
		OutError = TEXT("Tree v2 header is truncated or has invalid magic");
		return false;
	}
	const uint8* Header = HeaderStorage;
	if (ReadU32(Header + 8) != FormatVersion || ReadU32(Header + 12) != EndianMarker ||
		ReadU32(Header + 16) != HeaderBytes || ReadU32(Header + 20) != NodeRecordBytes)
	{
		OutError = TEXT("Tree v2 header is incompatible");
		return false;
	}
	TArray<uint8> HeaderForCRC;
	HeaderForCRC.Append(Header, HeaderBytes);
	FMemory::Memzero(HeaderForCRC.GetData() + 92, sizeof(uint32));
	const uint32 StoredHeaderCRC = ReadU32(Header + 92);
	if (NanoGS::Paged::UpdateCRC32(0, HeaderForCRC.GetData(), HeaderForCRC.Num()) != StoredHeaderCRC)
	{
		OutError = TEXT("Tree v2 header CRC mismatch");
		return false;
	}

	FHeader Decoded;
	Decoded.Flags = static_cast<ETreeFlags>(ReadU32(Header + 24));
	Decoded.PagePoints = ReadU32(Header + 28);
	Decoded.NodeCount = ReadU64(Header + 32);
	Decoded.RootNode = ReadU64(Header + 40);
	Decoded.InternalNodeCount = ReadU64(Header + 48);
	Decoded.ExactLeafCount = ReadU64(Header + 56);
	Decoded.SourceInputSplatCount = ReadU64(Header + 64);
	Decoded.NodesOffset = ReadU64(Header + 72);
	Decoded.FileBytes = ReadU64(Header + 80);
	Decoded.NodesCRC32 = ReadU32(Header + 88);
	Decoded.HeaderCRC32 = StoredHeaderCRC;
	const ETreeFlags RequiredFlags = ETreeFlags::ExactLeaves;
	uint64 NodeBytes = 0;
	if (!EnumHasAllFlags(Decoded.Flags, RequiredFlags) || Decoded.PagePoints == 0 || Decoded.NodeCount == 0 ||
		Decoded.NodeCount > MAX_int32 || Decoded.RootNode != 0 || Decoded.NodesOffset != HeaderBytes ||
		!CheckedNodeBytes(Decoded.NodeCount, NodeBytes) || Decoded.FileBytes != HeaderBytes + NodeBytes ||
		Decoded.FileBytes != static_cast<uint64>(PhysicalFileBytes) || NodeBytes > static_cast<uint64>(MAX_int64) ||
		Decoded.InternalNodeCount + Decoded.ExactLeafCount != Decoded.NodeCount)
	{
		OutError = TEXT("Tree v2 header dimensions or required flags are invalid");
		return false;
	}

	OutManifest.SourceFilename = FPaths::ConvertRelativePathToFull(Filename);
	OutManifest.Header = Decoded;
	OutManifest.Nodes.SetNum(static_cast<int32>(Decoded.NodeCount));
	TBitArray<> ClaimedPageSlots(false, static_cast<int32>(Decoded.NodeCount));
	uint64 ActualInternal = 0;
	uint64 ActualLeaves = 0;
	constexpr uint64 DecodeChunkBytes = 16ull * 1024ull * 1024ull;
	static_assert(DecodeChunkBytes % NodeRecordBytes == 0);
	TArray<uint8> DecodeChunk;
	DecodeChunk.SetNumUninitialized(static_cast<int32>(DecodeChunkBytes));
	uint64 DecodedBytes = 0;
	uint64 NodeIndex = 0;
	uint32 NodesCRC = 0;
	while (DecodedBytes < NodeBytes)
	{
		const uint64 BytesThisChunk = FMath::Min<uint64>(DecodeChunkBytes, NodeBytes - DecodedBytes);
		if (BytesThisChunk % NodeRecordBytes != 0 ||
			!FileHandle->Seek(static_cast<int64>(Decoded.NodesOffset + DecodedBytes)) ||
			!FileHandle->Read(DecodeChunk.GetData(), static_cast<int64>(BytesThisChunk)))
		{
			OutError = TEXT("Tree v2 node table is truncated");
			return false;
		}
		NodesCRC = NanoGS::Paged::UpdateCRC32(NodesCRC, DecodeChunk.GetData(), BytesThisChunk);
		for (uint64 ChunkOffset = 0; ChunkOffset < BytesThisChunk; ChunkOffset += NodeRecordBytes, ++NodeIndex)
		{
			const uint8* Record = DecodeChunk.GetData() + ChunkOffset;
			FNode& Node = OutManifest.Nodes[static_cast<int32>(NodeIndex)];
			Node.FirstChild = ReadU32(Record + 0);
			Node.ChildCount = ReadU16(Record + 4);
			Node.Depth = ReadU16(Record + 6);
			Node.SourceId = ReadU64(Record + 8);
			Node.PageId = ReadU32(Record + 16);
			Node.PageLocal = ReadU32(Record + 20);
			Node.CenterCm = FVector3f(ReadF32(Record + 24), ReadF32(Record + 28), ReadF32(Record + 32));
			Node.FeatureRadiusCm = ReadF32(Record + 36);
			Node.SupportRadiusCm = ReadF32(Record + 40);
			Node.Flags = static_cast<ENodeFlags>(ReadU32(Record + 44));
			Node.SubtreeLeafCount = ReadU32(Record + 48);
			const uint64 PageOutputIndex =
				static_cast<uint64>(Node.PageId) * Decoded.PagePoints + Node.PageLocal;
			if (ReadU32(Record + 52) != 0 || !FMath::IsFinite(Node.CenterCm.X) || !FMath::IsFinite(Node.CenterCm.Y) ||
				!FMath::IsFinite(Node.CenterCm.Z) || !FMath::IsFinite(Node.FeatureRadiusCm) || !FMath::IsFinite(Node.SupportRadiusCm) ||
				Node.FeatureRadiusCm < 0.0f || Node.SupportRadiusCm < Node.FeatureRadiusCm || Node.SubtreeLeafCount == 0 ||
				Node.PageLocal >= Decoded.PagePoints || PageOutputIndex >= Decoded.NodeCount ||
				ClaimedPageSlots[static_cast<int32>(PageOutputIndex)])
			{
				OutError = FString::Printf(TEXT("Tree v2 node %llu has invalid fields"), NodeIndex);
				return false;
			}
			ClaimedPageSlots[static_cast<int32>(PageOutputIndex)] = true;
			if (Node.ChildCount > 0)
			{
				++ActualInternal;
				if (!Node.IsInternal() || Node.IsExactLeaf() || Node.SourceId != InvalidSourceId ||
					Node.FirstChild <= NodeIndex || static_cast<uint64>(Node.FirstChild) + Node.ChildCount > Decoded.NodeCount)
				{
					OutError = FString::Printf(TEXT("Tree v2 internal node %llu is invalid"), NodeIndex);
					return false;
				}
			}
			else
			{
				++ActualLeaves;
				if (Node.IsInternal() || !Node.IsExactLeaf() || Node.SourceId >= Decoded.SourceInputSplatCount || Node.SubtreeLeafCount != 1)
				{
					OutError = FString::Printf(TEXT("Tree v2 exact leaf %llu is invalid"), NodeIndex);
					return false;
				}
			}
		}
		DecodedBytes += BytesThisChunk;
	}
	if (NodeIndex != Decoded.NodeCount || NodesCRC != Decoded.NodesCRC32)
	{
		OutError = NodesCRC != Decoded.NodesCRC32
			? TEXT("Tree v2 node CRC mismatch")
			: TEXT("Tree v2 decoded node count mismatch");
		return false;
	}
	if (ActualInternal != Decoded.InternalNodeCount || ActualLeaves != Decoded.ExactLeafCount || OutManifest.Nodes[0].Depth != 0)
	{
		OutError = TEXT("Tree v2 node class counts or root depth are invalid");
		return false;
	}
	TArray<uint8> ParentAssigned;
	ParentAssigned.SetNumZeroed(static_cast<int32>(Decoded.NodeCount));
	ParentAssigned[0] = 1;
	for (uint64 Index = 0; Index < Decoded.NodeCount; ++Index)
	{
		const FNode& Parent = OutManifest.Nodes[static_cast<int32>(Index)];
		uint64 LeafSum = 0;
		for (uint32 Offset = 0; Offset < Parent.ChildCount; ++Offset)
		{
			const uint32 ChildIndex = Parent.FirstChild + Offset;
			const FNode& Child = OutManifest.Nodes[ChildIndex];
			if (ParentAssigned[ChildIndex] || Child.Depth != Parent.Depth + 1)
			{
				OutError = FString::Printf(TEXT("Tree v2 child %u has invalid parent or depth"), ChildIndex);
				return false;
			}
			ParentAssigned[ChildIndex] = 1;
			LeafSum += Child.SubtreeLeafCount;
		}
		if (Parent.ChildCount > 0 && LeafSum != Parent.SubtreeLeafCount)
		{
			OutError = FString::Printf(TEXT("Tree v2 node %llu subtree leaf count mismatch"), Index);
			return false;
		}
	}
	if (ParentAssigned.Contains(0))
	{
		OutError = TEXT("Tree v2 contains an unreachable node");
		return false;
	}
	return true;
}

bool FReader::ValidatePageMapping(const FManifest& TreeManifest, const NanoGS::Paged::FManifest& PageManifest, FString& OutError)
{
	if (PageManifest.Header.TotalSplatCount != TreeManifest.Header.NodeCount)
	{
		OutError = TEXT("Tree v2 Page v1 splat count mismatch");
		return false;
	}
	for (uint64 Index = 0; Index < TreeManifest.Header.NodeCount; ++Index)
	{
		const FNode& Node = TreeManifest.Nodes[static_cast<int32>(Index)];
		const NanoGS::Paged::FPageRecord* Page = PageManifest.FindPage(Node.PageId);
		if (!Page || Node.PageLocal >= Page->SplatCount)
		{
			OutError = FString::Printf(TEXT("Tree v2 Page v1 mapping mismatch at node %llu"), Index);
			return false;
		}
	}
	return true;
}
}
