// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Paged/NanoGSSpatialLODReader.h"

#include <limits>

namespace NanoGS::SpatialLOD::Tests
{
namespace
{
	void PutU16LE(TArray<uint8>& Bytes, int64 Offset, uint16 Value)
	{
		Bytes[Offset + 0] = static_cast<uint8>(Value);
		Bytes[Offset + 1] = static_cast<uint8>(Value >> 8u);
	}

	void PutU32LE(TArray<uint8>& Bytes, int64 Offset, uint32 Value)
	{
		for (int32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<uint8>(Value >> (Byte * 8));
	}

	void PutU64LE(TArray<uint8>& Bytes, int64 Offset, uint64 Value)
	{
		PutU32LE(Bytes, Offset, static_cast<uint32>(Value));
		PutU32LE(Bytes, Offset + 4, static_cast<uint32>(Value >> 32u));
	}

	void PutF32LE(TArray<uint8>& Bytes, int64 Offset, float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		PutU32LE(Bytes, Offset, Bits);
	}

	void PutMagic(TArray<uint8>& Bytes, const uint8 (&Magic)[8])
	{
		FMemory::Memcpy(Bytes.GetData(), Magic, 8);
	}

	TArray<uint8> MakeTable(
		const uint8 (&Magic)[8],
		uint32 Count,
		uint32 Aux0,
		uint32 RecordSize,
		uint32 Levels,
		uint32 RecordsPerItem = 1)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(
			TableHeaderBytes + static_cast<int64>(Count) * RecordsPerItem * RecordSize);
		PutMagic(Bytes, Magic);
		PutU32LE(Bytes, 8, FormatVersion);
		PutU32LE(Bytes, 12, Count);
		PutU32LE(Bytes, 16, Aux0);
		PutU32LE(Bytes, 20, RecordSize);
		PutU32LE(Bytes, 24, Levels);
		return Bytes;
	}

	void PutNode(
		TArray<uint8>& Bytes,
		uint32 Index,
		uint32 Parent,
		uint32 FirstChild,
		uint16 ChildCount,
		uint16 Depth,
		uint32 Flags,
		float Min,
		float Max,
		uint32 FirstLeaf,
		uint32 LeafCount)
	{
		const int64 Record = TableHeaderBytes + static_cast<int64>(Index) * NodeRecordBytes;
		PutU32LE(Bytes, Record + 0, Parent);
		PutU32LE(Bytes, Record + 4, FirstChild);
		PutU16LE(Bytes, Record + 8, ChildCount);
		PutU16LE(Bytes, Record + 10, Depth);
		PutU32LE(Bytes, Record + 12, Flags);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			PutF32LE(Bytes, Record + 16 + Axis * 4, Min);
			PutF32LE(Bytes, Record + 28 + Axis * 4, Max);
		}
		PutU32LE(Bytes, Record + 40, FirstLeaf);
		PutU32LE(Bytes, Record + 44, LeafCount);
	}

	void PutRange(TArray<uint8>& Bytes, uint32 Node, uint32 Level, uint32 Levels, uint64 Offset, uint32 Count)
	{
		const int64 Index = static_cast<int64>(Node) * Levels + Level;
		const int64 Record = TableHeaderBytes + Index * RangeRecordBytes;
		PutU64LE(Bytes, Record + 0, Offset);
		PutU32LE(Bytes, Record + 8, Count);
	}

	void PutError(TArray<uint8>& Bytes, uint32 Node, uint32 Level, uint32 Levels, float Radius)
	{
		const int64 Index = static_cast<int64>(Node) * Levels + Level;
		const int64 Record = TableHeaderBytes + Index * ErrorRecordBytes;
		PutF32LE(Bytes, Record + 0, Radius);
		PutF32LE(Bytes, Record + 4, 1.0f / 255.0f);
		PutF32LE(Bytes, Record + 8, 3.0f / 255.0f);
		PutF32LE(Bytes, Record + 12, 0.002f);
	}

	TArray<uint8> MakeLod(uint32 Level, float FirstX, float SecondX)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(LodHeaderBytes + 2 * SplatStrideBytes);
		PutMagic(Bytes, LodMagic);
		PutU32LE(Bytes, 8, FormatVersion);
		PutU32LE(Bytes, 12, Level);
		PutU32LE(Bytes, 16, 2);
		PutU32LE(Bytes, 20, SplatStrideBytes);
		PutU32LE(Bytes, 24, SplatPropertyCount);
		PutU64LE(Bytes, 32, LodHeaderBytes);
		PutF32LE(Bytes, LodHeaderBytes, FirstX);
		PutF32LE(Bytes, LodHeaderBytes + SplatStrideBytes, SecondX);
		return Bytes;
	}

	FString WriteFixture(bool bCorruptRange)
	{
		const FString Directory = FPaths::Combine(
			FPaths::ProjectIntermediateDir(),
			TEXT("NanoGSSpatialLODTests"),
			bCorruptRange ? TEXT("Corrupt") : TEXT("Valid"));
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		IFileManager::Get().MakeDirectory(*Directory, true);

		TArray<uint8> Nodes = MakeTable(NodeMagic, 3, 0, NodeRecordBytes, 2);
		PutNode(Nodes, 0, MAX_uint32, 1, 2, 0, 0, -2.0f, 2.0f, 0, 2);
		PutNode(Nodes, 1, 0, 0, 0, 1, static_cast<uint32>(ENodeFlags::Leaf), -2.0f, 0.0f, 0, 1);
		PutNode(Nodes, 2, 0, 0, 0, 1,
			static_cast<uint32>(ENodeFlags::Leaf | ENodeFlags::Environment), 0.0f, 2.0f, 1, 1);

		TArray<uint8> Ranges = MakeTable(RangeMagic, 3, 2, RangeRecordBytes, 2, 2);
		for (uint32 Level = 0; Level < 2; ++Level)
		{
			PutRange(Ranges, 1, Level, 2, LodHeaderBytes, 1);
			PutRange(Ranges, 2, Level, 2,
				bCorruptRange && Level == 1 ? MAX_uint64 - 8 : LodHeaderBytes + SplatStrideBytes, 1);
		}

		TArray<uint8> Errors = MakeTable(ErrorMagic, 3, 2, ErrorRecordBytes, 2, 2);
		const float NaN = std::numeric_limits<float>::quiet_NaN();
		for (uint32 Node = 0; Node < 3; ++Node)
		{
			PutError(Errors, Node, 0, 2, NaN);
			PutError(Errors, Node, 1, 2, Node == 1 ? 64.0f : NaN);
		}

		FFileHelper::SaveArrayToFile(Nodes, *FPaths::Combine(Directory, TEXT("nodes.bin")));
		FFileHelper::SaveArrayToFile(Ranges, *FPaths::Combine(Directory, TEXT("node_lod_ranges.bin")));
		FFileHelper::SaveArrayToFile(Errors, *FPaths::Combine(Directory, TEXT("node_lod_errors.bin")));
		FFileHelper::SaveArrayToFile(MakeLod(0, 10.0f, 20.0f), *FPaths::Combine(Directory, TEXT("lod0.bin")));
		FFileHelper::SaveArrayToFile(MakeLod(1, 11.0f, 21.0f), *FPaths::Combine(Directory, TEXT("lod1.bin")));
		return Directory;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpatialLODReaderValidTest,
	"NanoGS.SpatialLOD.Reader.ValidManifestAndPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSpatialLODReaderValidTest::RunTest(const FString& Parameters)
{
	FManifest Manifest;
	FString Error;
	const bool bLoaded = FReader::ReadManifest(WriteFixture(false), Manifest, Error);
	TestTrue(TEXT("Valid fixture loads"), bLoaded);
	if (!bLoaded)
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Node count"), Manifest.Nodes.Num(), 3);
	TestEqual(TEXT("Level count"), Manifest.LevelCount, 2u);
	TestEqual(TEXT("Environment node"), Manifest.EnvironmentNode, 2u);
	const FNodeLodError* Calibrated = Manifest.FindError(1, 1);
	const FNodeLodError* Uncalibrated = Manifest.FindError(2, 1);
	TestTrue(TEXT("Scene LOD1 is calibrated"), Calibrated != nullptr && Calibrated->IsCalibrated());
	TestTrue(TEXT("Environment LOD1 stays uncalibrated"), Uncalibrated != nullptr && !Uncalibrated->IsCalibrated());

	TArray64<uint8> Payload;
	const bool bPayloadRead = FReader::ReadNodeLodPayload(Manifest, 2, 1, Payload, Error);
	TestTrue(TEXT("Node payload reads"), bPayloadRead);
	if (!bPayloadRead)
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Payload byte count"), Payload.Num(), static_cast<int64>(SplatStrideBytes));
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, Payload.GetData(), sizeof(Bits));
	float FirstX = 0.0f;
	FMemory::Memcpy(&FirstX, &Bits, sizeof(FirstX));
	TestEqual(TEXT("Payload starts at selected node range"), FirstX, 21.0f);

	const FString RuntimeDirectory = WriteFixture(false);
	IFileManager::Get().Delete(*FPaths::Combine(RuntimeDirectory, TEXT("lod0.bin")));
	IFileManager::Get().Delete(*FPaths::Combine(RuntimeDirectory, TEXT("lod1.bin")));
	FManifest RuntimeManifest;
	TestTrue(TEXT("Runtime manifest does not require source LOD payloads"),
		FReader::ReadRuntimeManifest(RuntimeDirectory, RuntimeManifest, Error));
	TestEqual(TEXT("Runtime L0 splat count is reconstructed"), RuntimeManifest.LodHeaders[0].SplatCount, 2u);
	TestFalse(TEXT("Import manifest still requires source LOD payloads"),
		FReader::ReadManifest(RuntimeDirectory, RuntimeManifest, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpatialLODReaderRangeGuardTest,
	"NanoGS.SpatialLOD.Reader.RejectsOutOfBoundsRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSpatialLODReaderRangeGuardTest::RunTest(const FString& Parameters)
{
	FManifest Manifest;
	FString Error;
	TestFalse(TEXT("Corrupt range fails closed"), FReader::ReadManifest(WriteFixture(true), Manifest, Error));
	TestTrue(TEXT("Error identifies range overflow"), Error.Contains(TEXT("range exceeds payload")));
	return true;
}
}

#endif
