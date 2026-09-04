// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSTreeV3Reader.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Paged/NanoGSPageReader.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace NanoGS::TreeV3
{
namespace
{
	uint32 ReadU32(const uint8* Data)
	{
		return static_cast<uint32>(Data[0]) |
			static_cast<uint32>(Data[1]) << 8u |
			static_cast<uint32>(Data[2]) << 16u |
			static_cast<uint32>(Data[3]) << 24u;
	}

	float ReadF32(const uint8* Data)
	{
		const uint32 Bits = ReadU32(Data);
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}

	bool ReadBinarySection(
		const FString& Directory,
		const TSharedPtr<FJsonObject>& Section,
		const uint32 ExpectedRecordBytes,
		TArray<uint8>& OutBytes,
		FString& OutFilename,
		FString& OutError)
	{
		if (!Section.IsValid())
		{
			OutError = TEXT("Tree v3 manifest section is missing");
			return false;
		}
		const int64 RecordBytes = static_cast<int64>(Section->GetNumberField(TEXT("record_bytes")));
		const int64 Records = static_cast<int64>(Section->GetNumberField(TEXT("records")));
		const int64 DeclaredBytes = static_cast<int64>(Section->GetNumberField(TEXT("bytes")));
		const uint32 DeclaredCRC = static_cast<uint32>(Section->GetNumberField(TEXT("crc32")));
		OutFilename = FPaths::Combine(Directory, Section->GetStringField(TEXT("file")));
		if (RecordBytes != ExpectedRecordBytes || Records < 0 || DeclaredBytes < 0 ||
			Records > MAX_int32 || DeclaredBytes != Records * RecordBytes || DeclaredBytes > MAX_int32)
		{
			OutError = FString::Printf(TEXT("Tree v3 section dimensions are invalid: %s"), *OutFilename);
			return false;
		}
		if (!FFileHelper::LoadFileToArray(OutBytes, *OutFilename) || OutBytes.Num() != DeclaredBytes)
		{
			OutError = FString::Printf(TEXT("Unable to read Tree v3 section: %s"), *OutFilename);
			return false;
		}
		if (NanoGS::Paged::UpdateCRC32(0, OutBytes.GetData(), OutBytes.Num()) != DeclaredCRC)
		{
			OutError = FString::Printf(TEXT("Tree v3 section CRC mismatch: %s"), *OutFilename);
			return false;
		}
		return true;
	}
}

bool FReader::ReadFromDirectory(const FString& Directory, FManifest& OutManifest, FString& OutError)
{
	OutManifest = FManifest();
	OutError.Reset();
	const FString ResolvedDirectory = FPaths::ConvertRelativePathToFull(Directory);
	const FString ManifestFilename = FPaths::Combine(ResolvedDirectory, TEXT("tree_v3_manifest.json"));
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ManifestFilename))
	{
		OutError = FString::Printf(TEXT("Tree v3 manifest is unavailable: %s"), *ManifestFilename);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() ||
		Root->GetStringField(TEXT("schema")) != TEXT("nanogs.tree_v3") ||
		static_cast<uint32>(Root->GetNumberField(TEXT("version"))) != FormatVersion)
	{
		OutError = TEXT("Tree v3 manifest schema/version is invalid");
		return false;
	}

	const TSharedPtr<FJsonObject> Partition = Root->GetObjectField(TEXT("partition"));
	OutManifest.Directory = ResolvedDirectory;
	OutManifest.CacheKey = Root->GetStringField(TEXT("tree_v3_key"));
	OutManifest.MaxBlockLeaves = static_cast<uint32>(Partition->GetNumberField(TEXT("max_block_leaves")));
	OutManifest.MaxBlockNodes = static_cast<uint32>(Partition->GetNumberField(TEXT("max_block_nodes")));

	TArray<uint8> SkeletonBytes;
	FString SkeletonFilename;
	if (!ReadBinarySection(ResolvedDirectory, Root->GetObjectField(TEXT("skeleton")),
		sizeof(NanoGS::Tree::FGPUNodeRecord), SkeletonBytes, SkeletonFilename, OutError))
	{
		return false;
	}
	OutManifest.SkeletonNodes.SetNumUninitialized(
		SkeletonBytes.Num() / sizeof(NanoGS::Tree::FGPUNodeRecord));
	for (int32 Index = 0; Index < OutManifest.SkeletonNodes.Num(); ++Index)
	{
		const uint8* Record = SkeletonBytes.GetData() + Index * sizeof(NanoGS::Tree::FGPUNodeRecord);
		NanoGS::Tree::FGPUNodeRecord& Node = OutManifest.SkeletonNodes[Index];
		Node.CenterAndSupportRadius = FVector4f(
			ReadF32(Record), ReadF32(Record + 4), ReadF32(Record + 8), ReadF32(Record + 12));
		Node.FirstChild = ReadU32(Record + 16);
		Node.ChildCountAndFlags = ReadU32(Record + 20);
		Node.FeatureRadiusBits = ReadU32(Record + 24);
		Node.PackedPageAddress = ReadU32(Record + 28);
		const uint32 ChildCount = Node.GetChildCount();
		const bool bBlockProxy = (Node.ChildCountAndFlags & BlockProxyFlag) != 0;
		if ((!bBlockProxy && ChildCount > 0 &&
			static_cast<uint64>(Node.FirstChild) + ChildCount > static_cast<uint64>(OutManifest.SkeletonNodes.Num())) ||
			(bBlockProxy && ChildCount != 0))
		{
			OutError = FString::Printf(TEXT("Tree v3 skeleton node %d is invalid"), Index);
			return false;
		}
	}

	TArray<uint8> DirectoryBytes;
	FString DirectoryFilename;
	if (!ReadBinarySection(ResolvedDirectory, Root->GetObjectField(TEXT("block_directory")),
		BlockRecordBytes, DirectoryBytes, DirectoryFilename, OutError))
	{
		return false;
	}
	OutManifest.Blocks.SetNumUninitialized(DirectoryBytes.Num() / BlockRecordBytes);
	for (int32 Index = 0; Index < OutManifest.Blocks.Num(); ++Index)
	{
		const uint8* Record = DirectoryBytes.GetData() + Index * BlockRecordBytes;
		FBlockRecord& Block = OutManifest.Blocks[Index];
		Block.SourceRootNode = ReadU32(Record);
		Block.FirstNode = ReadU32(Record + 4);
		Block.NodeCount = ReadU32(Record + 8);
		Block.LeafCount = ReadU32(Record + 12);
		Block.MaxDepth = ReadU32(Record + 16);
		Block.RootPageId = ReadU32(Record + 20);
		Block.FirstPage = ReadU32(Record + 24);
		Block.PageCount = ReadU32(Record + 28);
		if (Block.NodeCount == 0 || Block.LeafCount == 0 || Block.LeafCount > OutManifest.MaxBlockLeaves ||
			Block.NodeCount > OutManifest.MaxBlockNodes || Block.PageCount == 0)
		{
			OutError = FString::Printf(TEXT("Tree v3 block %d is invalid"), Index);
			return false;
		}
	}
	TArray<uint8> BlockPageBytes;
	FString BlockPageFilename;
	if (!ReadBinarySection(ResolvedDirectory, Root->GetObjectField(TEXT("block_pages")),
		sizeof(uint32), BlockPageBytes, BlockPageFilename, OutError))
	{
		return false;
	}
	OutManifest.BlockPageIds.SetNumUninitialized(BlockPageBytes.Num() / sizeof(uint32));
	for (int32 Index = 0; Index < OutManifest.BlockPageIds.Num(); ++Index)
	{
		OutManifest.BlockPageIds[Index] = ReadU32(BlockPageBytes.GetData() + Index * sizeof(uint32));
	}
	for (int32 BlockIndex = 0; BlockIndex < OutManifest.Blocks.Num(); ++BlockIndex)
	{
		const FBlockRecord& Block = OutManifest.Blocks[BlockIndex];
		if (static_cast<uint64>(Block.FirstPage) + Block.PageCount >
			static_cast<uint64>(OutManifest.BlockPageIds.Num()))
		{
			OutError = FString::Printf(TEXT("Tree v3 block %d page range is invalid"), BlockIndex);
			return false;
		}
	}
	for (const NanoGS::Tree::FGPUNodeRecord& Node : OutManifest.SkeletonNodes)
	{
		if ((Node.ChildCountAndFlags & BlockProxyFlag) != 0 &&
			!OutManifest.Blocks.IsValidIndex(static_cast<int32>(Node.FirstChild)))
		{
			OutError = TEXT("Tree v3 skeleton references an invalid block");
			return false;
		}
	}
	OutManifest.BlockDataFilename = FPaths::Combine(
		ResolvedDirectory, Root->GetObjectField(TEXT("blocks"))->GetStringField(TEXT("file")));
	if (IFileManager::Get().FileSize(*OutManifest.BlockDataFilename) <= 0)
	{
		OutError = TEXT("Tree v3 block payload is unavailable");
		return false;
	}
	return !OutManifest.SkeletonNodes.IsEmpty() && !OutManifest.Blocks.IsEmpty();
}
}
