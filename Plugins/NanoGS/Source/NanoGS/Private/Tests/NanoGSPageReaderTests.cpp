#if WITH_DEV_AUTOMATION_TESTS

#include "Paged/NanoGSPageReader.h"
#include "Paged/NanoGSPagedSourceAsset.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace NanoGS::Paged::Tests
{
	namespace
	{
		constexpr uint64 TestPageDirectoryOffset = FileHeaderBytes;
		constexpr uint64 TestPageCount = 2;
		constexpr uint64 TestStreamCount = TestPageCount * StreamsPerPage;
		constexpr uint64 TestPageDirectoryBytes = TestPageCount * PageRecordBytes;
		constexpr uint64 TestStreamDirectoryOffset = TestPageDirectoryOffset + TestPageDirectoryBytes;
		constexpr uint64 TestStreamDirectoryBytes = TestStreamCount * StreamRecordBytes;
		constexpr uint64 TestPayloadOffset = TestStreamDirectoryOffset + TestStreamDirectoryBytes;

		static_assert((TestPayloadOffset & 63ull) == 0, "Test fixture payload must be 64-byte aligned");

		void PutU32LE(TArray<uint8>& Bytes, const uint64 Offset, const uint32 Value)
		{
			check(Offset + sizeof(Value) <= static_cast<uint64>(Bytes.Num()));
			Bytes[static_cast<int32>(Offset + 0)] = uint8(Value);
			Bytes[static_cast<int32>(Offset + 1)] = uint8(Value >> 8u);
			Bytes[static_cast<int32>(Offset + 2)] = uint8(Value >> 16u);
			Bytes[static_cast<int32>(Offset + 3)] = uint8(Value >> 24u);
		}

		void PutU64LE(TArray<uint8>& Bytes, const uint64 Offset, const uint64 Value)
		{
			check(Offset + sizeof(Value) <= static_cast<uint64>(Bytes.Num()));
			for (uint32 ByteIndex = 0; ByteIndex < sizeof(Value); ++ByteIndex)
			{
				Bytes[static_cast<int32>(Offset + ByteIndex)] = uint8(Value >> (ByteIndex * 8u));
			}
		}

		uint64 GetU64LE(const TArray<uint8>& Bytes, const uint64 Offset)
		{
			check(Offset + sizeof(uint64) <= static_cast<uint64>(Bytes.Num()));
			uint64 Value = 0;
			for (uint32 ByteIndex = 0; ByteIndex < sizeof(uint64); ++ByteIndex)
			{
				Value |= uint64(Bytes[static_cast<int32>(Offset + ByteIndex)]) << (ByteIndex * 8u);
			}
			return Value;
		}

		void PutF64LE(TArray<uint8>& Bytes, const uint64 Offset, const double Value)
		{
			uint64 Bits = 0;
			FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
			PutU64LE(Bytes, Offset, Bits);
		}

		uint64 Align64(const uint64 Value)
		{
			return (Value + 63ull) & ~63ull;
		}

		struct FFixtureStream
		{
			uint64 PageId = 0;
			EStreamSemantic Semantic = static_cast<EStreamSemantic>(0);
			EStreamEncoding Encoding = static_cast<EStreamEncoding>(0);
			uint64 ComponentCount = 0;
			uint64 Stride = 0;
			uint64 Offset = 0;
		};

		void GetFixtureStreamContract(
			const EStreamSemantic Semantic,
			EStreamEncoding& OutEncoding,
			uint64& OutComponents,
			uint64& OutStride)
		{
			switch (Semantic)
			{
			case EStreamSemantic::Position:
				OutEncoding = EStreamEncoding::Float32;
				OutComponents = 3;
				OutStride = 12;
				break;
			case EStreamSemantic::Rotation:
				OutEncoding = EStreamEncoding::Float32;
				OutComponents = 4;
				OutStride = 16;
				break;
			case EStreamSemantic::Scale:
				OutEncoding = EStreamEncoding::Float32;
				OutComponents = 3;
				OutStride = 12;
				break;
			case EStreamSemantic::ColorOpacity:
				OutEncoding = EStreamEncoding::UNorm8;
				OutComponents = 4;
				OutStride = 4;
				break;
			case EStreamSemantic::SphericalHarmonics:
				OutEncoding = EStreamEncoding::Float16;
				OutComponents = 48;
				OutStride = 96;
				break;
			case EStreamSemantic::OriginalId:
				OutEncoding = EStreamEncoding::UInt64;
				OutComponents = 1;
				OutStride = 8;
				break;
			default:
				checkNoEntry();
			}
		}

		void RecomputeMetadataCRCs(TArray<uint8>& Bytes)
		{
			uint32 DirectoryCRC = UpdateCRC32(
				0,
				Bytes.GetData() + TestPageDirectoryOffset,
				TestPageDirectoryBytes);
			DirectoryCRC = UpdateCRC32(
				DirectoryCRC,
				Bytes.GetData() + TestStreamDirectoryOffset,
				TestStreamDirectoryBytes);
			PutU32LE(Bytes, 188, DirectoryCRC);

			PutU32LE(Bytes, HeaderCRC32Offset, 0);
			const uint32 HeaderCRC = UpdateCRC32(0, Bytes.GetData(), FileHeaderBytes);
			PutU32LE(Bytes, HeaderCRC32Offset, HeaderCRC);
		}

		TArray<uint8> BuildValidFixture()
		{
			TArray<FFixtureStream> Streams;
			Streams.Reserve(static_cast<int32>(TestStreamCount));
			uint64 NextPayloadOffset = TestPayloadOffset;
			for (uint64 PageIndex = 0; PageIndex < TestPageCount; ++PageIndex)
			{
				const uint64 PageId = PageIndex == 0 ? 10 : 20;
				for (uint32 SemanticValue = 1; SemanticValue <= StreamsPerPage; ++SemanticValue)
				{
					FFixtureStream& Stream = Streams.AddDefaulted_GetRef();
					Stream.PageId = PageId;
					Stream.Semantic = static_cast<EStreamSemantic>(SemanticValue);
					GetFixtureStreamContract(Stream.Semantic, Stream.Encoding, Stream.ComponentCount, Stream.Stride);
					Stream.Offset = NextPayloadOffset;
					NextPayloadOffset = Align64(NextPayloadOffset + Stream.Stride);
				}
			}

			const uint64 LastPayloadEnd = Streams.Last().Offset + Streams.Last().Stride;
			check(LastPayloadEnd <= static_cast<uint64>(MAX_int32));
			TArray<uint8> Bytes;
			Bytes.SetNumZeroed(static_cast<int32>(LastPayloadEnd));

			FMemory::Memcpy(Bytes.GetData(), FileMagic, UE_ARRAY_COUNT(FileMagic));
			PutU32LE(Bytes, 8, FormatVersion);
			PutU32LE(Bytes, 12, LittleEndianTag);
			PutU32LE(Bytes, 16, static_cast<uint32>(EPayloadFormat::LosslessGeometrySH3HalfRGBA8SoA));
			PutU32LE(Bytes, 20, static_cast<uint32>(RequiredFileFlags));
			PutU64LE(Bytes, 24, FileHeaderBytes);
			PutU64LE(Bytes, 32, LastPayloadEnd);
			PutU64LE(Bytes, 40, TestPageCount);
			PutU64LE(Bytes, 48, TestPageDirectoryOffset);
			PutU64LE(Bytes, 56, PageRecordBytes);
			PutU64LE(Bytes, 64, TestStreamCount);
			PutU64LE(Bytes, 72, TestStreamDirectoryOffset);
			PutU64LE(Bytes, 80, StreamRecordBytes);
			PutU64LE(Bytes, 88, TestPayloadOffset);
			PutU64LE(Bytes, 96, LastPayloadEnd - TestPayloadOffset);
			PutU64LE(Bytes, 104, TestPageCount);
			PutF64LE(Bytes, 112, -4.0);
			PutF64LE(Bytes, 120, -4.0);
			PutF64LE(Bytes, 128, -4.0);
			PutF64LE(Bytes, 136, 4.0);
			PutF64LE(Bytes, 144, 4.0);
			PutF64LE(Bytes, 152, 4.0);
			PutF64LE(Bytes, 160, 2.0);
			PutF64LE(Bytes, 168, 1.0);
			PutU32LE(Bytes, 176, static_cast<uint32>(ECoordinateSystem::NanoGSLocalCentimetersSourceXYZ));
			PutU32LE(Bytes, 180, 3);
			PutU32LE(Bytes, 184, static_cast<uint32>(ESHLayout::GraphdecoCoefficientMajorRGB));

			for (uint64 PageIndex = 0; PageIndex < TestPageCount; ++PageIndex)
			{
				const uint64 Record = TestPageDirectoryOffset + PageIndex * PageRecordBytes;
				const uint64 PageId = PageIndex == 0 ? 10 : 20;
				const double CenterMin = PageIndex == 0 ? -2.0 : 1.0;
				const double CenterMax = PageIndex == 0 ? -1.0 : 2.0;
				const double SupportMin = PageIndex == 0 ? -4.0 : -1.0;
				const double SupportMax = PageIndex == 0 ? 1.0 : 4.0;
				PutU64LE(Bytes, Record + 0, PageId);
				PutU64LE(Bytes, Record + 8, PageIndex);
				PutU64LE(Bytes, Record + 16, 1);
				PutU64LE(Bytes, Record + 24, PageIndex * StreamsPerPage);
				PutU64LE(Bytes, Record + 32, StreamsPerPage);
				for (uint64 Axis = 0; Axis < 3; ++Axis)
				{
					PutF64LE(Bytes, Record + 40 + Axis * 8, CenterMin);
					PutF64LE(Bytes, Record + 64 + Axis * 8, CenterMax);
					PutF64LE(Bytes, Record + 88 + Axis * 8, SupportMin);
					PutF64LE(Bytes, Record + 112 + Axis * 8, SupportMax);
				}
				PutF64LE(Bytes, Record + 136, 2.0);
				PutF64LE(Bytes, Record + 144, 1.0);
				PutU32LE(Bytes, Record + 152, static_cast<uint32>(EPageFlags::RotatedGaussianAABB2Sigma));
			}

			for (int32 StreamIndex = 0; StreamIndex < Streams.Num(); ++StreamIndex)
			{
				const FFixtureStream& Stream = Streams[StreamIndex];
				for (uint64 ByteIndex = 0; ByteIndex < Stream.Stride; ++ByteIndex)
				{
					Bytes[static_cast<int32>(Stream.Offset + ByteIndex)] = uint8(StreamIndex * 17 + ByteIndex);
				}
				if (Stream.Semantic == EStreamSemantic::OriginalId)
				{
					PutU64LE(Bytes, Stream.Offset, Stream.PageId == 10 ? 0 : 1);
				}

				const uint64 Record = TestStreamDirectoryOffset + static_cast<uint64>(StreamIndex) * StreamRecordBytes;
				PutU64LE(Bytes, Record + 0, Stream.PageId);
				PutU32LE(Bytes, Record + 8, static_cast<uint32>(Stream.Semantic));
				PutU32LE(Bytes, Record + 12, static_cast<uint32>(Stream.Encoding));
				PutU64LE(Bytes, Record + 16, 1);
				PutU64LE(Bytes, Record + 24, Stream.ComponentCount);
				PutU64LE(Bytes, Record + 32, Stream.Stride);
				PutU64LE(Bytes, Record + 40, Stream.Offset);
				PutU64LE(Bytes, Record + 48, Stream.Stride);
				PutU64LE(Bytes, Record + 56, Stream.Stride);
				PutU32LE(Bytes, Record + 64,
					UpdateCRC32(0, Bytes.GetData() + Stream.Offset, Stream.Stride));
			}

			RecomputeMetadataCRCs(Bytes);
			return Bytes;
		}

		FString WriteFixture(const TArray<uint8>& Bytes, const FString& Name)
		{
			const FString Directory = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("NanoGSPageTests"));
			IFileManager::Get().MakeDirectory(*Directory, true);
			const FString Filename = FPaths::Combine(Directory, Name + TEXT(".ngsp"));
			IFileManager::Get().Delete(*Filename, false, true);
			check(FFileHelper::SaveArrayToFile(Bytes, *Filename));
			return Filename;
		}

		bool ReadBytes(
			const TArray<uint8>& Bytes,
			const FString& Name,
			FManifest& OutManifest,
			FString& OutError,
			const bool bVerifyPayload = false)
		{
			const FString Filename = WriteFixture(Bytes, Name);
			FReadOptions Options;
			Options.bVerifyPayloadCRCs = bVerifyPayload;
			const bool bResult = FPageFileReader::ReadManifest(Filename, OutManifest, OutError, Options);
			IFileManager::Get().Delete(*Filename, false, true);
			return bResult;
		}
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageV1ValidManifestTest,
		"NanoGS.Paged.V1.ValidManifestAndPayloadCRC",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageV1ValidManifestTest::RunTest(const FString& Parameters)
	{
		const ANSICHAR KnownCRCInput[] = "123456789";
		TestEqual(TEXT("CRC-32 matches the standard IEEE/zlib vector"),
			UpdateCRC32(0, reinterpret_cast<const uint8*>(KnownCRCInput), 9), 0xCBF43926u);

		FManifest Manifest;
		FString Error;
		TestTrue(TEXT("Valid v1 fixture and every payload CRC are accepted"),
			ReadBytes(BuildValidFixture(), TEXT("valid"), Manifest, Error, true));
		if (!Error.IsEmpty())
		{
			AddError(Error);
		}
		TestEqual(TEXT("Two pages decoded"), Manifest.Pages.Num(), 2);
		TestEqual(TEXT("Twelve streams decoded"), Manifest.Streams.Num(), 12);
		TestTrue(TEXT("Validated manifest publishes its transient PageId lookup"), Manifest.HasPageLookup());
		TestNotNull(TEXT("64-bit page ID lookup succeeds"), Manifest.FindPage(20));
		if (Manifest.Streams.Num() > 4)
		{
			TestEqual(TEXT("Full SH3 half stride is retained"), Manifest.Streams[4].ElementStrideBytes, 96ull);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageV1PageLookupTest,
		"NanoGS.Paged.V1.PageLookup",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageV1PageLookupTest::RunTest(const FString& Parameters)
	{
		constexpr int32 LargePageCount = 100000;
		constexpr uint64 FirstLargePageId = (1ull << 40u) + 3ull;
		constexpr uint64 PageIdStride = 17ull;

		FManifest Manifest;
		Manifest.Pages.SetNum(LargePageCount);
		for (int32 PageOrdinal = 0; PageOrdinal < LargePageCount; ++PageOrdinal)
		{
			Manifest.Pages[PageOrdinal].PageId =
				FirstLargePageId + static_cast<uint64>(PageOrdinal) * PageIdStride;
		}

		TestFalse(TEXT("A hand-built manifest fails closed until its lookup is built"), Manifest.HasPageLookup());
		TestTrue(TEXT("Missing lookup never falls back to a linear scan"),
			Manifest.FindPage(FirstLargePageId) == nullptr);

		FString Error;
		if (!TestTrue(TEXT("A large unique page directory builds an in-memory lookup"),
			Manifest.BuildPageLookup(Error)))
		{
			AddError(Error);
			return false;
		}
		TestTrue(TEXT("Large lookup is complete"), Manifest.HasPageLookup());

		for (int32 PageOrdinal = 0; PageOrdinal < LargePageCount; PageOrdinal += 97)
		{
			const uint64 PageId = FirstLargePageId + static_cast<uint64>(PageOrdinal) * PageIdStride;
			const FPageRecord* Found = Manifest.FindPage(PageId);
			if (Found != &Manifest.Pages[PageOrdinal] || Found->PageId != PageId)
			{
				AddError(FString::Printf(TEXT("Large lookup returned the wrong ordinal for page %llu"),
					static_cast<unsigned long long>(PageId)));
				return false;
			}
		}
		TestTrue(TEXT("Unknown 64-bit PageId is rejected"),
			Manifest.FindPage(FirstLargePageId - 1ull) == nullptr);

		const uint64 OriginalFirstId = Manifest.Pages[0].PageId;
		const uint64 OriginalLastId = Manifest.Pages.Last().PageId;
		Swap(Manifest.Pages[0], Manifest.Pages.Last());
		TestTrue(TEXT("A stale ordinal cannot return the wrong first page"),
			Manifest.FindPage(OriginalFirstId) == nullptr);
		TestTrue(TEXT("A stale ordinal cannot return the wrong last page"),
			Manifest.FindPage(OriginalLastId) == nullptr);
		TestTrue(TEXT("Explicit rebuild repairs an intentionally reordered hand-built directory"),
			Manifest.BuildPageLookup(Error));
		TestTrue(TEXT("Rebuilt lookup resolves the moved first ID"),
			Manifest.FindPage(OriginalFirstId) == &Manifest.Pages.Last());

		Manifest.Pages.AddDefaulted();
		Manifest.Pages.Last().PageId = OriginalLastId + PageIdStride;
		TestFalse(TEXT("A page-count change invalidates lookup completeness"), Manifest.HasPageLookup());
		TestTrue(TEXT("A count-stale lookup fails closed"), Manifest.FindPage(OriginalFirstId) == nullptr);

		FManifest DuplicateManifest;
		DuplicateManifest.Pages.SetNum(2);
		DuplicateManifest.Pages[0].PageId = 77;
		DuplicateManifest.Pages[1].PageId = 77;
		Error.Reset();
		TestFalse(TEXT("Duplicate PageIds are rejected while building the lookup"),
			DuplicateManifest.BuildPageLookup(Error));
		TestTrue(TEXT("Duplicate PageId error is explicit"), Error.Contains(TEXT("Duplicate page ID")));
		TestFalse(TEXT("A rejected duplicate build leaves no usable lookup"), DuplicateManifest.HasPageLookup());
		TestTrue(TEXT("A rejected duplicate build cannot return either duplicate"),
			DuplicateManifest.FindPage(77) == nullptr);

		FManifest MutatedManifest;
		MutatedManifest.Pages.SetNum(2);
		MutatedManifest.Pages[0].PageId = 10;
		MutatedManifest.Pages[1].PageId = 20;
		TestTrue(TEXT("Unique two-page lookup builds"), MutatedManifest.BuildPageLookup(Error));
		MutatedManifest.Pages[0].PageId = 20;
		TestTrue(TEXT("A stale entry whose record ID changed cannot be returned"),
			MutatedManifest.FindPage(10) == nullptr);
		Error.Reset();
		TestFalse(TEXT("Rebuilding after an in-place duplicate mutation rejects the directory"),
			MutatedManifest.BuildPageLookup(Error));
		TestFalse(TEXT("Rejected duplicate rebuild invalidates all previous entries"),
			MutatedManifest.HasPageLookup());
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageV1StrictValidationTest,
		"NanoGS.Paged.V1.StrictValidation",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageV1StrictValidationTest::RunTest(const FString& Parameters)
	{
		const TArray<uint8> Valid = BuildValidFixture();
		FManifest Manifest;
		FString Error;

		auto ExpectRejected = [this, &Manifest, &Error](const TArray<uint8>& Bytes, const TCHAR* Name, const TCHAR* ErrorFragment, const bool bVerifyPayload = false)
		{
			Error.Reset();
			const bool bAccepted = ReadBytes(Bytes, Name, Manifest, Error, bVerifyPayload);
			TestFalse(FString::Printf(TEXT("%s is rejected"), Name), bAccepted);
			TestTrue(FString::Printf(TEXT("%s reports a useful error: %s"), Name, *Error), Error.Contains(ErrorFragment));
		};

		TArray<uint8> UnsupportedFormat = Valid;
		PutU32LE(UnsupportedFormat, 16, 999);
		RecomputeMetadataCRCs(UnsupportedFormat);
		ExpectRejected(UnsupportedFormat, TEXT("unsupported_format"), TEXT("payload format"));

		TArray<uint8> WrongEndian = Valid;
		PutU32LE(WrongEndian, 12, 0x04030201u);
		RecomputeMetadataCRCs(WrongEndian);
		ExpectRejected(WrongEndian, TEXT("wrong_endian"), TEXT("endian"));

		TArray<uint8> OverflowingCount = Valid;
		PutU64LE(OverflowingCount, 40, MAX_uint64);
		RecomputeMetadataCRCs(OverflowingCount);
		ExpectRejected(OverflowingCount, TEXT("overflowing_count"), TEXT("overflows"));

		TArray<uint8> DuplicatePage = Valid;
		PutU64LE(DuplicatePage, TestPageDirectoryOffset + PageRecordBytes, 10);
		for (uint64 StreamInPage = 0; StreamInPage < StreamsPerPage; ++StreamInPage)
		{
			PutU64LE(DuplicatePage,
				TestStreamDirectoryOffset + (StreamsPerPage + StreamInPage) * StreamRecordBytes,
				10);
		}
		RecomputeMetadataCRCs(DuplicatePage);
		ExpectRejected(DuplicatePage, TEXT("duplicate_page"), TEXT("Duplicate page ID"));

		TArray<uint8> DuplicateSemantic = Valid;
		const uint64 OriginalIdSemanticOffset = TestStreamDirectoryOffset + 5 * StreamRecordBytes + 8;
		PutU32LE(DuplicateSemantic, OriginalIdSemanticOffset, static_cast<uint32>(EStreamSemantic::SphericalHarmonics));
		RecomputeMetadataCRCs(DuplicateSemantic);
		ExpectRejected(DuplicateSemantic, TEXT("duplicate_semantic"), TEXT("duplicate stream semantic"));

		TArray<uint8> OverlappingPayload = Valid;
		const uint64 FirstStreamOffset = TestPayloadOffset;
		PutU64LE(OverlappingPayload, TestStreamDirectoryOffset + StreamRecordBytes + 40, FirstStreamOffset);
		RecomputeMetadataCRCs(OverlappingPayload);
		ExpectRejected(OverlappingPayload, TEXT("overlapping_payload"), TEXT("overlap"));

		TArray<uint8> OutOfBoundsPayload = Valid;
		PutU64LE(OutOfBoundsPayload, TestStreamDirectoryOffset + 40, MAX_uint64 - 63ull);
		RecomputeMetadataCRCs(OutOfBoundsPayload);
		ExpectRejected(OutOfBoundsPayload, TEXT("out_of_bounds_payload"), TEXT("outside the payload range or overflows"));

		TArray<uint8> DirectoryCorruption = Valid;
		DirectoryCorruption[static_cast<int32>(TestPageDirectoryOffset + 120)] ^= 0x1u;
		ExpectRejected(DirectoryCorruption, TEXT("directory_crc"), TEXT("Directory CRC mismatch"));

		TArray<uint8> PayloadCorruption = Valid;
		PayloadCorruption[static_cast<int32>(TestPayloadOffset)] ^= 0x1u;
		ExpectRejected(PayloadCorruption, TEXT("payload_crc"), TEXT("Payload CRC mismatch"), true);

		TArray<uint8> DuplicateOriginalId = Valid;
		const uint64 SecondOriginalIdRecord = TestStreamDirectoryOffset + 11 * StreamRecordBytes;
		const uint64 SecondOriginalIdPayload = GetU64LE(DuplicateOriginalId, SecondOriginalIdRecord + 40);
		PutU64LE(DuplicateOriginalId, SecondOriginalIdPayload, 0);
		PutU32LE(DuplicateOriginalId, SecondOriginalIdRecord + 64,
			UpdateCRC32(0, DuplicateOriginalId.GetData() + SecondOriginalIdPayload, 8));
		RecomputeMetadataCRCs(DuplicateOriginalId);
		ExpectRejected(DuplicateOriginalId, TEXT("duplicate_original_id"), TEXT("Duplicate OriginalId"), true);

		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageV1RandomAccessPayloadTest,
		"NanoGS.Paged.V1.RandomAccessPagePayload",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageV1RandomAccessPayloadTest::RunTest(const FString& Parameters)
	{
		const TArray<uint8> Valid = BuildValidFixture();
		const FString Filename = WriteFixture(Valid, TEXT("random_access_page"));
		FManifest Manifest;
		FString Error;
		FReadOptions ManifestOptions;
		ManifestOptions.bVerifyPayloadCRCs = false;
		if (!TestTrue(TEXT("Random-access fixture manifest opens"),
			FPageFileReader::ReadManifest(Filename, Manifest, Error, ManifestOptions)))
		{
			AddError(Error);
			IFileManager::Get().Delete(*Filename, false, true);
			return false;
		}

		FPagePayloadReadOptions PageOptions;
		PageOptions.bVerifyStreamCRCs = true;
		PageOptions.MaxPagePayloadBytes = 148;
		FOwnedPagePayload Payload;
		TestTrue(TEXT("A page reads with a limit smaller than the file but equal to its six owned streams"),
			FPageFileReader::ReadPagePayload(Manifest, 20, Payload, Error, PageOptions));
		if (!Error.IsEmpty())
		{
			AddError(Error);
		}
		TestEqual(TEXT("Exactly six streams are owned"), Payload.Streams.Num(), 6);
		TestEqual(TEXT("Only the selected page's 148 payload bytes are allocated"), Payload.TotalOwnedBytes, uint64(148));
		TestEqual(TEXT("Position float view has XYZ"), Payload.GetPositions().Num(), int64(3));
		TestEqual(TEXT("Rotation float view has XYZW"), Payload.GetRotations().Num(), int64(4));
		TestEqual(TEXT("Scale float view has XYZ"), Payload.GetScales().Num(), int64(3));
		TestEqual(TEXT("Color/opacity view has RGBA"), Payload.GetColorOpacity().Num(), int64(4));
		TestEqual(TEXT("SH view has 48 half words"), Payload.GetSphericalHarmonicsHalfBits().Num(), int64(48));
		TestEqual(TEXT("OriginalId view has one source ID"), Payload.GetOriginalIds().Num(), int64(1));
		if (Payload.GetOriginalIds().Num() == 1)
		{
			TestEqual(TEXT("Second page retains source ID 1"), Payload.GetOriginalIds()[0], uint64(1));
		}

		Error.Reset();
		TestFalse(TEXT("Unknown PageId is rejected"),
			FPageFileReader::ReadPagePayload(Manifest, 9999, Payload, Error, PageOptions));
		TestTrue(TEXT("Unknown PageId error is explicit"), Error.Contains(TEXT("not found")));

		FPagePayloadReadOptions TooSmall = PageOptions;
		TooSmall.MaxPagePayloadBytes = 147;
		Error.Reset();
		TestFalse(TEXT("Per-page allocation guard is enforced"),
			FPageFileReader::ReadPagePayload(Manifest, 10, Payload, Error, TooSmall));
		TestTrue(TEXT("Allocation guard error is explicit"), Error.Contains(TEXT("exceeding the configured limit")));

		FManifest DuplicateSemantic = Manifest;
		DuplicateSemantic.Streams[1].Semantic = EStreamSemantic::Position;
		Error.Reset();
		TestFalse(TEXT("Mutated page-directory semantics are revalidated before I/O"),
			FPageFileReader::ReadPagePayload(DuplicateSemantic, 10, Payload, Error, PageOptions));
		TestTrue(TEXT("Duplicate semantic error is explicit"), Error.Contains(TEXT("duplicate stream semantic")));

		FManifest OverflowingOffset = Manifest;
		OverflowingOffset.Streams[0].FileOffset = MAX_uint64 - 63ull;
		Error.Reset();
		TestFalse(TEXT("Overflowing uint64 stream ranges are rejected before I/O"),
			FPageFileReader::ReadPagePayload(OverflowingOffset, 10, Payload, Error, PageOptions));
		TestTrue(TEXT("Overflow error is explicit"), Error.Contains(TEXT("overflows")));
		IFileManager::Get().Delete(*Filename, false, true);

		TArray<uint8> Corrupted = Valid;
		Corrupted[static_cast<int32>(TestPayloadOffset)] ^= 0x1u;
		const FString CorruptedFilename = WriteFixture(Corrupted, TEXT("random_access_crc"));
		FManifest CorruptedManifest;
		Error.Reset();
		TestTrue(TEXT("Metadata-only manifest open does not scan payload"),
			FPageFileReader::ReadManifest(CorruptedFilename, CorruptedManifest, Error, ManifestOptions));
		Error.Reset();
		TestFalse(TEXT("Selected-page CRC verification catches payload corruption"),
			FPageFileReader::ReadPagePayload(CorruptedManifest, 10, Payload, Error, PageOptions));
		TestTrue(TEXT("Selected-page CRC error is explicit"), Error.Contains(TEXT("CRC mismatch")));
		FPagePayloadReadOptions NoCRC = PageOptions;
		NoCRC.bVerifyStreamCRCs = false;
		Error.Reset();
		TestTrue(TEXT("Per-stream CRC verification can be disabled explicitly"),
			FPageFileReader::ReadPagePayload(CorruptedManifest, 10, Payload, Error, NoCRC));
		IFileManager::Get().Delete(*CorruptedFilename, false, true);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageV1RealSJTURandomReadTest,
		"NanoGS.Paged.V1.RealSJTUFirstMiddleLastRandomRead",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageV1RealSJTURandomReadTest::RunTest(const FString& Parameters)
	{
		FString Filename;
		if (!FParse::Value(FCommandLine::Get(), TEXT("NanoGSPageSmokeFile="), Filename))
		{
			Filename = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("NanoGSPageBuilderSmoke/sjtu_demo.ngsp"));
		}
		if (!IFileManager::Get().FileExists(*Filename))
		{
			AddInfo(FString::Printf(TEXT("Real SJTU page file is unavailable; random-read smoke skipped: %s"), *Filename));
			return true;
		}

		FManifest Manifest;
		FString Error;
		FReadOptions ManifestOptions;
		ManifestOptions.bVerifyPayloadCRCs = false;
		if (!TestTrue(TEXT("Real SJTU manifest passes strict metadata validation"),
			FPageFileReader::ReadManifest(Filename, Manifest, Error, ManifestOptions)))
		{
			AddError(Error);
			return false;
		}
		if (!TestTrue(TEXT("Real SJTU manifest has at least three pages"), Manifest.Pages.Num() >= 3))
		{
			return false;
		}

		// Deliberately non-sequential: end, beginning, then middle.
		const int32 PageIndices[] = {Manifest.Pages.Num() - 1, 0, Manifest.Pages.Num() / 2};
		FPagePayloadReadOptions PageOptions;
		PageOptions.bVerifyStreamCRCs = true;
		for (const int32 PageIndex : PageIndices)
		{
			const FPageRecord& Page = Manifest.Pages[PageIndex];
			FOwnedPagePayload Payload;
			Error.Reset();
			if (!TestTrue(
				FString::Printf(TEXT("Real SJTU page %llu random read and six CRCs succeed"),
					static_cast<unsigned long long>(Page.PageId)),
				FPageFileReader::ReadPagePayload(Manifest, Page.PageId, Payload, Error, PageOptions)))
			{
				AddError(Error);
				continue;
			}

			TestEqual(TEXT("Real page owns six streams"), Payload.Streams.Num(), 6);
			TestEqual(TEXT("Real page position count is 3N"),
				Payload.GetPositions().Num(), static_cast<int64>(Page.SplatCount * 3));
			TestEqual(TEXT("Real page rotation count is 4N"),
				Payload.GetRotations().Num(), static_cast<int64>(Page.SplatCount * 4));
			TestEqual(TEXT("Real page scale count is 3N"),
				Payload.GetScales().Num(), static_cast<int64>(Page.SplatCount * 3));
			TestEqual(TEXT("Real page RGBA count is 4N"),
				Payload.GetColorOpacity().Num(), static_cast<int64>(Page.SplatCount * 4));
			TestEqual(TEXT("Real page SH half count is 48N"),
				Payload.GetSphericalHarmonicsHalfBits().Num(), static_cast<int64>(Page.SplatCount * 48));
			TestEqual(TEXT("Real page OriginalId count is N"),
				Payload.GetOriginalIds().Num(), static_cast<int64>(Page.SplatCount));
			TestEqual(TEXT("Real page allocates exactly 148N stream bytes"),
				Payload.TotalOwnedBytes, Page.SplatCount * uint64(148));
			TestTrue(TEXT("Random page read never allocates the full file"),
				Payload.TotalOwnedBytes < Manifest.PhysicalFileBytes);
			if (!Payload.GetOriginalIds().IsEmpty())
			{
				TestTrue(TEXT("First OriginalId is within the declared source range"),
					Payload.GetOriginalIds()[0] < Manifest.Header.TotalSplatCount);
				TestTrue(TEXT("Last OriginalId is within the declared source range"),
					Payload.GetOriginalIds().Last() < Manifest.Header.TotalSplatCount);
			}
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPagedSourceAssetMetadataOnlyTest,
		"NanoGS.Paged.SourceAsset.MetadataOnlyReferenceAndStaleDetection",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPagedSourceAssetMetadataOnlyTest::RunTest(const FString& Parameters)
	{
		const TArray<uint8> ValidBytes = BuildValidFixture();
		const FString Filename = WriteFixture(ValidBytes, TEXT("source_asset_metadata"));
		UNanoGSPagedSourceAsset* Asset = NewObject<UNanoGSPagedSourceAsset>(GetTransientPackage());
		FString Error;

		if (!TestTrue(TEXT("A valid in-project Page v1 container initializes a source asset"),
			Asset->InitializeFromContainer(Filename, Error, true)))
		{
			AddError(Error);
			IFileManager::Get().Delete(*Filename, false, true);
			return false;
		}

		TestTrue(TEXT("The source asset stores a valid metadata summary"), Asset->HasValidMetadata());
		TestTrue(TEXT("The serialized container path is project-relative"),
			FPaths::IsRelative(Asset->GetProjectRelativeContainerPath()));
		TestFalse(TEXT("An Intermediate fixture is correctly reported as not packaging-ready"),
			Asset->IsInRecommendedStagingDirectory());
		TestEqual(TEXT("Summary retains 64-bit page count"), Asset->GetPageCount(), uint64(2));
		TestEqual(TEXT("Summary retains 64-bit splat count"), Asset->GetTotalSplatCount(), uint64(2));
		TestEqual(TEXT("Summary retains the physical file size"),
			Asset->GetPhysicalFileBytes(), static_cast<uint64>(ValidBytes.Num()));
		TestFalse(TEXT("Summary fingerprint is present"), Asset->GetMetadataFingerprint().IsEmpty());

		FReadOptions Options;
		Options.bVerifyPayloadCRCs = false;
		FManifest Manifest;
		Error.Reset();
		TestTrue(TEXT("Runtime OpenManifest reparses metadata without loading page payload"),
			Asset->OpenManifest(Manifest, Error, Options));
		if (!Error.IsEmpty())
		{
			AddError(Error);
		}
		TestEqual(TEXT("Runtime manifest exposes the referenced source filename"),
			Manifest.SourceFilename, Asset->GetResolvedContainerPath());

		TArray<uint8> CorruptedBytes = ValidBytes;
		CorruptedBytes[200] = 1; // Reserved header byte; header CRC and v1 semantics now fail.
		TestTrue(TEXT("The test can replace the external payload without touching the UObject"),
			FFileHelper::SaveArrayToFile(CorruptedBytes, *Filename));
		const FString OriginalFingerprint = Asset->GetMetadataFingerprint();
		Error.Reset();
		TestFalse(TEXT("OpenManifest rejects a container changed after asset import"),
			Asset->OpenManifest(Manifest, Error, Options));
		TestFalse(TEXT("Failed external refresh leaves the last known-good UObject summary intact"),
			Asset->RefreshFromContainer(Error, true));
		TestEqual(TEXT("Failed refresh is transactional"), Asset->GetMetadataFingerprint(), OriginalFingerprint);

		Error.Reset();
		TestFalse(TEXT("An absolute container outside ProjectDir is rejected"),
			Asset->InitializeFromContainer(TEXT("/tmp/nanogs_outside_project.ngsp"), Error, true));
		TestTrue(TEXT("Outside-project rejection explains the portable path requirement"),
			Error.Contains(TEXT("inside ProjectDir")));

		IFileManager::Get().Delete(*Filename, false, true);

		const FString RealFilename = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("NanoGSPageBuilderSmoke/sjtu_demo.ngsp"));
		if (IFileManager::Get().FileExists(*RealFilename))
		{
			UNanoGSPagedSourceAsset* RealAsset = NewObject<UNanoGSPagedSourceAsset>(GetTransientPackage());
			Error.Reset();
			TestTrue(TEXT("The metadata-only UObject accepts the real multi-GB SJTU container"),
				RealAsset->InitializeFromContainer(RealFilename, Error, true));
			if (!Error.IsEmpty())
			{
				AddError(Error);
			}
			TestTrue(TEXT("The real container size remains 64-bit in the UObject summary"),
				RealAsset->GetPhysicalFileBytes() > static_cast<uint64>(MAX_int32));
			TestEqual(TEXT("The real container exposes all 354 pages"), RealAsset->GetPageCount(), uint64(354));
			TestEqual(TEXT("The real container exposes all 23,135,444 L0 splats"),
				RealAsset->GetTotalSplatCount(), uint64(23135444));

			FManifest RealManifest;
			Error.Reset();
			TestTrue(TEXT("The real multi-GB container reopens through the stored portable reference"),
				RealAsset->OpenManifest(RealManifest, Error, Options));
			if (!Error.IsEmpty())
			{
				AddError(Error);
			}
		}
		else
		{
			AddInfo(FString::Printf(TEXT("Real SJTU page file is unavailable; source-asset 64-bit smoke skipped: %s"), *RealFilename));
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPagedSourceAssetPackagedPathTest,
		"NanoGS.Paged.SourceAsset.PackagedPathResolution",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPagedSourceAssetPackagedPathTest::RunTest(const FString& Parameters)
	{
		const FString MockProjectRoot = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("NanoGSPackagedPathTest/Linux/OpenFlySplatUE"));
		const FString MockContentRoot = FPaths::Combine(MockProjectRoot, TEXT("Content"));
		const FString CanonicalRelativePath = TEXT("Content/NanoGSData/campus/sjtu_demo.ngsp");
		FString ResolvedPath;
		FString Error;

		TestTrue(TEXT("Cooked runtime resolves canonical sidecar path against ProjectContentDir"),
			UNanoGSPagedSourceAsset::ResolveContainerPathForRuntimeRoots(
				CanonicalRelativePath,
				MockProjectRoot,
				MockContentRoot,
				true,
				ResolvedPath,
				Error));
		FString ExpectedPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(MockContentRoot, TEXT("NanoGSData/campus/sjtu_demo.ngsp")));
		FPaths::NormalizeFilename(ExpectedPath);
		TestEqual(TEXT("Cooked path matches the staged loose Content tree"), ResolvedPath, ExpectedPath);

		Error.Reset();
		TestFalse(TEXT("Cooked runtime rejects an editor-only Saved sidecar"),
			UNanoGSPagedSourceAsset::ResolveContainerPathForRuntimeRoots(
				TEXT("Saved/sjtu_demo.ngsp"), MockProjectRoot, MockContentRoot, true, ResolvedPath, Error));
		TestTrue(TEXT("Cooked staging rejection is explicit"), Error.Contains(TEXT("must be staged below")));

		Error.Reset();
		TestTrue(TEXT("Editor compatibility retains safe project-relative non-Content paths"),
			UNanoGSPagedSourceAsset::ResolveContainerPathForRuntimeRoots(
				TEXT("Saved/sjtu_demo.ngsp"), MockProjectRoot, MockContentRoot, false, ResolvedPath, Error));
		ExpectedPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(MockProjectRoot, TEXT("Saved/sjtu_demo.ngsp")));
		FPaths::NormalizeFilename(ExpectedPath);
		TestEqual(TEXT("Editor-only path remains anchored to ProjectDir"), ResolvedPath, ExpectedPath);

		Error.Reset();
		TestFalse(TEXT("Linux cooked path matching is case-correct for the staged directory"),
			UNanoGSPagedSourceAsset::ResolveContainerPathForRuntimeRoots(
				TEXT("Content/nanogsdata/sjtu_demo.ngsp"),
				MockProjectRoot,
				MockContentRoot,
				true,
				ResolvedPath,
				Error));
		TestFalse(TEXT("Collapsed traversal cannot escape the recommended staging directory"),
			UNanoGSPagedSourceAsset::ResolveContainerPathForRuntimeRoots(
				TEXT("Content/NanoGSData/../../escape.ngsp"),
				MockProjectRoot,
				MockContentRoot,
				true,
				ResolvedPath,
				Error));
		TestFalse(TEXT("Absolute serialized paths are rejected"),
			UNanoGSPagedSourceAsset::ResolveContainerPathForRuntimeRoots(
				TEXT("/tmp/escape.ngsp"),
				MockProjectRoot,
				MockContentRoot,
				false,
				ResolvedPath,
				Error));
		TestFalse(TEXT("Inconsistent runtime content root is rejected"),
			UNanoGSPagedSourceAsset::ResolveContainerPathForRuntimeRoots(
				CanonicalRelativePath,
				MockProjectRoot,
				TEXT("/tmp/not-the-project-content"),
				true,
				ResolvedPath,
				Error));

		const FString FixtureDirectory = FPaths::Combine(
			FPaths::ProjectContentDir(), TEXT("NanoGSData/Automation"));
		const FString FixtureFilename = FPaths::Combine(FixtureDirectory, TEXT("packaged_path_fixture.ngsp"));
		IFileManager::Get().MakeDirectory(*FixtureDirectory, true);
		const TArray<uint8> FixtureBytes = BuildValidFixture();
		if (!TestTrue(TEXT("Recommended Content fixture is written"),
			FFileHelper::SaveArrayToFile(FixtureBytes, *FixtureFilename)))
		{
			return false;
		}

		UNanoGSPagedSourceAsset* Asset = NewObject<UNanoGSPagedSourceAsset>(GetTransientPackage());
		Error.Reset();
		const bool bInitialized = Asset->InitializeFromContainer(FixtureFilename, Error, true);
		TestTrue(TEXT("Recommended Content fixture initializes metadata asset"), bInitialized);
		if (!bInitialized)
		{
			AddError(Error);
		}
		else
		{
			TestTrue(TEXT("Content fixture is packaging-ready"), Asset->IsInRecommendedStagingDirectory());
			FString ExpectedEditorPath = FPaths::ConvertRelativePathToFull(FixtureFilename);
			FPaths::NormalizeFilename(ExpectedEditorPath);
			TestEqual(TEXT("Editor and packaged path contracts share the same Content-relative reference"),
				Asset->GetResolvedContainerPath(), ExpectedEditorPath);
		}

		IFileManager::Get().Delete(*FixtureFilename, false, true);
		IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, true);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageV1ExternalInteropTest,
		"NanoGS.Paged.V1.ExternalBuilderInterop",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageV1ExternalInteropTest::RunTest(const FString& Parameters)
	{
		FString Filename;
		if (!FParse::Value(FCommandLine::Get(), TEXT("NanoGSPageInteropFile="), Filename))
		{
			AddInfo(TEXT("No -NanoGSPageInteropFile=... argument supplied; external builder interop check skipped."));
			return true;
		}

		FReadOptions Options;
		Options.bVerifyPayloadCRCs = true;
		FManifest Manifest;
		FString Error;
		const bool bRead = FPageFileReader::ReadManifest(Filename, Manifest, Error, Options);
		TestTrue(FString::Printf(TEXT("External builder file passes full v1 validation: %s"), *Error), bRead);
		if (bRead)
		{
			TestTrue(TEXT("External builder file contains pages"), Manifest.Header.PageCount > 0);
			TestEqual(TEXT("External stream count is six per page"),
				Manifest.Header.StreamCount, Manifest.Header.PageCount * StreamsPerPage);
		}
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
