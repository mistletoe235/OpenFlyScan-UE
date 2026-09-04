// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSPageReader.h"

#include "Algo/Sort.h"
#include "Containers/BitArray.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"

namespace NanoGS::Paged
{
	namespace
	{
		struct FThreadLocalPageFile
		{
			FString Filename;
			uint64 ExpectedFileBytes = 0;
			TUniquePtr<IFileHandle> Handle;

			IFileHandle* Acquire(const FManifest& Manifest, FString& OutError)
			{
				if (!Handle || Filename != Manifest.SourceFilename || ExpectedFileBytes != Manifest.PhysicalFileBytes)
				{
					IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
					Handle.Reset(PlatformFile.OpenRead(*Manifest.SourceFilename));
					Filename = Manifest.SourceFilename;
					ExpectedFileBytes = Manifest.PhysicalFileBytes;
				}
				if (!Handle)
				{
					OutError = FString::Printf(TEXT("Failed to open NanoGS Page file: %s"), *Manifest.SourceFilename);
					return nullptr;
				}
				const int64 ActualFileBytes = Handle->Size();
				if (ActualFileBytes < 0 || static_cast<uint64>(ActualFileBytes) != Manifest.PhysicalFileBytes)
				{
					OutError = FString::Printf(TEXT("NanoGS Page file size changed after its manifest was read: %s"), *Manifest.SourceFilename);
					Handle.Reset();
					return nullptr;
				}
				return Handle.Get();
			}

			void Invalidate()
			{
				Handle.Reset();
			}
		};

		thread_local FThreadLocalPageFile GThreadLocalPageFile;

		struct FByteCursor
		{
			const uint8* Data = nullptr;
			uint64 Size = 0;
			uint64 Offset = 0;

			bool ReadBytes(uint8* OutData, const uint64 NumBytes)
			{
				if (Offset > Size || NumBytes > Size - Offset)
				{
					return false;
				}
				if (NumBytes > 0)
				{
					FMemory::Memcpy(OutData, Data + Offset, static_cast<SIZE_T>(NumBytes));
				}
				Offset += NumBytes;
				return true;
			}

			bool ReadU32(uint32& OutValue)
			{
				uint8 Bytes[4];
				if (!ReadBytes(Bytes, UE_ARRAY_COUNT(Bytes)))
				{
					return false;
				}
				OutValue =
					uint32(Bytes[0]) |
					(uint32(Bytes[1]) << 8u) |
					(uint32(Bytes[2]) << 16u) |
					(uint32(Bytes[3]) << 24u);
				return true;
			}

			bool ReadU64(uint64& OutValue)
			{
				uint8 Bytes[8];
				if (!ReadBytes(Bytes, UE_ARRAY_COUNT(Bytes)))
				{
					return false;
				}
				OutValue = 0;
				for (uint32 ByteIndex = 0; ByteIndex < UE_ARRAY_COUNT(Bytes); ++ByteIndex)
				{
					OutValue |= uint64(Bytes[ByteIndex]) << (ByteIndex * 8u);
				}
				return true;
			}

			bool ReadF64(double& OutValue)
			{
				uint64 Bits = 0;
				if (!ReadU64(Bits))
				{
					return false;
				}
				FMemory::Memcpy(&OutValue, &Bits, sizeof(OutValue));
				return true;
			}

			bool ReadVector3d(FVector3d& OutValue)
			{
				return ReadF64(OutValue.X) && ReadF64(OutValue.Y) && ReadF64(OutValue.Z);
			}
		};

		struct FRange
		{
			uint64 Begin = 0;
			uint64 End = 0;
			uint64 Label = 0;
		};

		bool CheckedAdd(const uint64 A, const uint64 B, uint64& OutValue)
		{
			if (B > MAX_uint64 - A)
			{
				return false;
			}
			OutValue = A + B;
			return true;
		}

		bool CheckedPageMultiply(const uint64 A, const uint64 B, uint64& OutValue)
		{
			if (A != 0 && B > MAX_uint64 / A)
			{
				return false;
			}
			OutValue = A * B;
			return true;
		}

		bool IsFiniteOrderedPageBounds(const FVector3d& Min, const FVector3d& Max)
		{
			return
				FMath::IsFinite(Min.X) && FMath::IsFinite(Min.Y) && FMath::IsFinite(Min.Z) &&
				FMath::IsFinite(Max.X) && FMath::IsFinite(Max.Y) && FMath::IsFinite(Max.Z) &&
				Min.X <= Max.X && Min.Y <= Max.Y && Min.Z <= Max.Z;
		}

		bool ContainsBounds(
			const FVector3d& OuterMin,
			const FVector3d& OuterMax,
			const FVector3d& InnerMin,
			const FVector3d& InnerMax)
		{
			return
				OuterMin.X <= InnerMin.X && OuterMin.Y <= InnerMin.Y && OuterMin.Z <= InnerMin.Z &&
				OuterMax.X >= InnerMax.X && OuterMax.Y >= InnerMax.Y && OuterMax.Z >= InnerMax.Z;
		}

		bool ValidateRange(
			const uint64 Offset,
			const uint64 Bytes,
			const uint64 FileBytes,
			uint64& OutEnd)
		{
			return CheckedAdd(Offset, Bytes, OutEnd) && OutEnd <= FileBytes;
		}

		bool RangesOverlap(const FRange& A, const FRange& B)
		{
			return A.Begin < B.End && B.Begin < A.End;
		}

		bool ReadFileRange(
			IFileHandle& File,
			const uint64 Offset,
			const uint64 Bytes,
			TArray<uint8>& OutBytes,
			FString& OutError,
			const TCHAR* RangeName)
		{
			if (Offset > static_cast<uint64>(MAX_int64) || Bytes > static_cast<uint64>(MAX_int32))
			{
				OutError = FString::Printf(
					TEXT("%s cannot be represented by the runtime reader (offset=%llu, bytes=%llu)"),
					RangeName,
					static_cast<unsigned long long>(Offset),
					static_cast<unsigned long long>(Bytes));
				return false;
			}

			OutBytes.SetNumUninitialized(static_cast<int32>(Bytes));
			if (!File.Seek(static_cast<int64>(Offset)) ||
				(Bytes > 0 && !File.Read(OutBytes.GetData(), static_cast<int64>(Bytes))))
			{
				OutError = FString::Printf(
					TEXT("Failed to read %s at offset %llu (%llu bytes)"),
					RangeName,
					static_cast<unsigned long long>(Offset),
					static_cast<unsigned long long>(Bytes));
				return false;
			}
			return true;
		}

		bool ParseHeader(const TArray<uint8>& Bytes, FFileHeader& OutHeader, FString& OutError)
		{
			if (Bytes.Num() != static_cast<int32>(FileHeaderBytes))
			{
				OutError = TEXT("NanoGS Page header is truncated");
				return false;
			}

			FByteCursor Cursor{Bytes.GetData(), static_cast<uint64>(Bytes.Num()), 0};
			uint8 Magic[UE_ARRAY_COUNT(FileMagic)];
			uint32 PayloadFormat = 0;
			uint32 Flags = 0;
			uint32 CoordinateSystem = 0;
			uint32 SHLayout = 0;
			if (!Cursor.ReadBytes(Magic, UE_ARRAY_COUNT(Magic)) ||
				!Cursor.ReadU32(OutHeader.Version) ||
				!Cursor.ReadU32(OutHeader.EndianTag) ||
				!Cursor.ReadU32(PayloadFormat) ||
				!Cursor.ReadU32(Flags) ||
				!Cursor.ReadU64(OutHeader.HeaderBytes) ||
				!Cursor.ReadU64(OutHeader.FileBytes) ||
				!Cursor.ReadU64(OutHeader.PageCount) ||
				!Cursor.ReadU64(OutHeader.PageDirectoryOffset) ||
				!Cursor.ReadU64(OutHeader.PageRecordBytes) ||
				!Cursor.ReadU64(OutHeader.StreamCount) ||
				!Cursor.ReadU64(OutHeader.StreamDirectoryOffset) ||
				!Cursor.ReadU64(OutHeader.StreamRecordBytes) ||
				!Cursor.ReadU64(OutHeader.PayloadOffset) ||
				!Cursor.ReadU64(OutHeader.PayloadBytes) ||
				!Cursor.ReadU64(OutHeader.TotalSplatCount) ||
				!Cursor.ReadVector3d(OutHeader.SceneBoundsMin) ||
				!Cursor.ReadVector3d(OutHeader.SceneBoundsMax) ||
				!Cursor.ReadF64(OutHeader.SupportSigma) ||
				!Cursor.ReadF64(OutHeader.UnitsToCentimeters) ||
				!Cursor.ReadU32(CoordinateSystem) ||
				!Cursor.ReadU32(OutHeader.SHOrder) ||
				!Cursor.ReadU32(SHLayout) ||
				!Cursor.ReadU32(OutHeader.DirectoryCRC32) ||
				!Cursor.ReadU32(OutHeader.HeaderCRC32))
			{
				OutError = TEXT("NanoGS Page header fields are truncated");
				return false;
			}

			if (FMemory::Memcmp(Magic, FileMagic, UE_ARRAY_COUNT(FileMagic)) != 0)
			{
				OutError = TEXT("Unsupported NanoGS Page magic (expected NGSPAGE1)");
				return false;
			}

			for (; Cursor.Offset < Cursor.Size; ++Cursor.Offset)
			{
				if (Cursor.Data[Cursor.Offset] != 0)
				{
					OutError = FString::Printf(
						TEXT("NanoGS Page header reserved byte at offset %llu is non-zero"),
						static_cast<unsigned long long>(Cursor.Offset));
					return false;
				}
			}

			OutHeader.PayloadFormat = static_cast<EPayloadFormat>(PayloadFormat);
			OutHeader.Flags = static_cast<EFileFlags>(Flags);
			OutHeader.CoordinateSystem = static_cast<ECoordinateSystem>(CoordinateSystem);
			OutHeader.SHLayout = static_cast<ESHLayout>(SHLayout);
			return true;
		}

		bool ParsePages(
			const TArray<uint8>& Bytes,
			const uint64 PageCount,
			TArray<FPageRecord>& OutPages,
			FString& OutError)
		{
			OutPages.SetNum(static_cast<int32>(PageCount));
			FByteCursor Cursor{Bytes.GetData(), static_cast<uint64>(Bytes.Num()), 0};
			for (uint64 PageIndex = 0; PageIndex < PageCount; ++PageIndex)
			{
				FPageRecord& Page = OutPages[static_cast<int32>(PageIndex)];
				uint32 Flags = 0;
				uint32 Reserved = 0;
				if (!Cursor.ReadU64(Page.PageId) ||
					!Cursor.ReadU64(Page.FirstOutputIndex) ||
					!Cursor.ReadU64(Page.SplatCount) ||
					!Cursor.ReadU64(Page.StreamStart) ||
					!Cursor.ReadU64(Page.StreamCount) ||
					!Cursor.ReadVector3d(Page.CenterBoundsMin) ||
					!Cursor.ReadVector3d(Page.CenterBoundsMax) ||
					!Cursor.ReadVector3d(Page.SupportBoundsMin) ||
					!Cursor.ReadVector3d(Page.SupportBoundsMax) ||
					!Cursor.ReadF64(Page.SupportSigma) ||
					!Cursor.ReadF64(Page.MaxScaleCm) ||
					!Cursor.ReadU32(Flags) ||
					!Cursor.ReadU32(Reserved))
				{
					OutError = FString::Printf(TEXT("Page directory record %llu is truncated"),
						static_cast<unsigned long long>(PageIndex));
					return false;
				}
				if (Reserved != 0)
				{
					OutError = FString::Printf(TEXT("Page %llu has non-zero reserved fields"),
						static_cast<unsigned long long>(Page.PageId));
					return false;
				}
				Page.Flags = static_cast<EPageFlags>(Flags);
			}
			return Cursor.Offset == Cursor.Size;
		}

		bool ParseStreams(
			const TArray<uint8>& Bytes,
			const uint64 StreamCount,
			TArray<FStreamRecord>& OutStreams,
			FString& OutError)
		{
			OutStreams.SetNum(static_cast<int32>(StreamCount));
			FByteCursor Cursor{Bytes.GetData(), static_cast<uint64>(Bytes.Num()), 0};
			for (uint64 StreamIndex = 0; StreamIndex < StreamCount; ++StreamIndex)
			{
				FStreamRecord& Stream = OutStreams[static_cast<int32>(StreamIndex)];
				uint32 Semantic = 0;
				uint32 Encoding = 0;
				uint32 Flags = 0;
				uint64 Reserved = 0;
				if (!Cursor.ReadU64(Stream.PageId) ||
					!Cursor.ReadU32(Semantic) ||
					!Cursor.ReadU32(Encoding) ||
					!Cursor.ReadU64(Stream.ElementCount) ||
					!Cursor.ReadU64(Stream.ComponentCount) ||
					!Cursor.ReadU64(Stream.ElementStrideBytes) ||
					!Cursor.ReadU64(Stream.FileOffset) ||
					!Cursor.ReadU64(Stream.ByteSize) ||
					!Cursor.ReadU64(Stream.UncompressedByteSize) ||
					!Cursor.ReadU32(Stream.PayloadCRC32) ||
					!Cursor.ReadU32(Flags) ||
					!Cursor.ReadU64(Reserved))
				{
					OutError = FString::Printf(TEXT("Stream directory record %llu is truncated"),
						static_cast<unsigned long long>(StreamIndex));
					return false;
				}
				if (Reserved != 0)
				{
					OutError = FString::Printf(TEXT("Stream directory record %llu has non-zero reserved fields"),
						static_cast<unsigned long long>(StreamIndex));
					return false;
				}
				Stream.Semantic = static_cast<EStreamSemantic>(Semantic);
				Stream.Encoding = static_cast<EStreamEncoding>(Encoding);
				Stream.Flags = Flags;
			}
			return Cursor.Offset == Cursor.Size;
		}

		bool GetExpectedStreamContract(
			const EStreamSemantic Semantic,
			EStreamEncoding& OutEncoding,
			uint64& OutComponentCount,
			uint64& OutStrideBytes)
		{
			switch (Semantic)
			{
			case EStreamSemantic::Position:
				OutEncoding = EStreamEncoding::Float32;
				OutComponentCount = 3;
				OutStrideBytes = 12;
				return true;
			case EStreamSemantic::Rotation:
				OutEncoding = EStreamEncoding::Float32;
				OutComponentCount = 4;
				OutStrideBytes = 16;
				return true;
			case EStreamSemantic::Scale:
				OutEncoding = EStreamEncoding::Float32;
				OutComponentCount = 3;
				OutStrideBytes = 12;
				return true;
			case EStreamSemantic::ColorOpacity:
				OutEncoding = EStreamEncoding::UNorm8;
				OutComponentCount = 4;
				OutStrideBytes = 4;
				return true;
			case EStreamSemantic::SphericalHarmonics:
				OutEncoding = EStreamEncoding::Float16;
				OutComponentCount = 48;
				OutStrideBytes = 96;
				return true;
			case EStreamSemantic::OriginalId:
				OutEncoding = EStreamEncoding::UInt64;
				OutComponentCount = 1;
				OutStrideBytes = 8;
				return true;
			default:
				return false;
			}
		}

		bool CollectAndValidatePageStreams(
			const FManifest& Manifest,
			const uint64 PageId,
			const FPageRecord*& OutPage,
			TArray<const FStreamRecord*>& OutStreamsBySemantic,
			uint64& OutTotalBytes,
			FString& OutError)
		{
			OutPage = nullptr;
			OutStreamsBySemantic.Init(nullptr, static_cast<int32>(StreamsPerPage));
			OutTotalBytes = 0;

			const FFileHeader& Header = Manifest.Header;
			if (Manifest.SourceFilename.IsEmpty())
			{
				OutError = TEXT("Manifest has no source filename for random-access page reads");
				return false;
			}
			if (Header.Version != FormatVersion ||
				Header.PayloadFormat != EPayloadFormat::LosslessGeometrySH3HalfRGBA8SoA ||
				Header.Flags != RequiredFileFlags)
			{
				OutError = TEXT("Manifest header does not satisfy the NanoGS Page v1 payload contract");
				return false;
			}
			if (Header.FileBytes != Manifest.PhysicalFileBytes ||
				Header.PageCount != static_cast<uint64>(Manifest.Pages.Num()) ||
				Header.StreamCount != static_cast<uint64>(Manifest.Streams.Num()))
			{
				OutError = TEXT("Manifest header counts or file size do not match its decoded directories");
				return false;
			}

			if (!Manifest.HasPageLookup())
			{
				OutError = TEXT("Manifest has no valid PageId lookup; call BuildPageLookup after finalizing its page directory");
				return false;
			}
			OutPage = Manifest.FindPage(PageId);
			if (OutPage == nullptr)
			{
				OutError = FString::Printf(TEXT("Page ID %llu was not found in the manifest, or its lookup entry is stale"),
					static_cast<unsigned long long>(PageId));
				return false;
			}

			const FPageRecord& Page = *OutPage;
			if (Page.SplatCount == 0 || Page.StreamCount != StreamsPerPage)
			{
				OutError = FString::Printf(TEXT("Page %llu has invalid splat/stream counts (%llu/%llu)"),
					static_cast<unsigned long long>(PageId),
					static_cast<unsigned long long>(Page.SplatCount),
					static_cast<unsigned long long>(Page.StreamCount));
				return false;
			}
			uint64 OutputEnd = 0;
			uint64 StreamEndIndex = 0;
			if (!CheckedAdd(Page.FirstOutputIndex, Page.SplatCount, OutputEnd) ||
				OutputEnd > Header.TotalSplatCount)
			{
				OutError = FString::Printf(TEXT("Page %llu output-index range overflows or exceeds TotalSplatCount"),
					static_cast<unsigned long long>(PageId));
				return false;
			}
			if (!CheckedAdd(Page.StreamStart, Page.StreamCount, StreamEndIndex) ||
				StreamEndIndex > static_cast<uint64>(Manifest.Streams.Num()))
			{
				OutError = FString::Printf(TEXT("Page %llu stream-index range overflows or exceeds the decoded directory"),
					static_cast<unsigned long long>(PageId));
				return false;
			}
			if (!IsFiniteOrderedPageBounds(Page.CenterBoundsMin, Page.CenterBoundsMax) ||
				!IsFiniteOrderedPageBounds(Page.SupportBoundsMin, Page.SupportBoundsMax) ||
				!ContainsBounds(Page.SupportBoundsMin, Page.SupportBoundsMax, Page.CenterBoundsMin, Page.CenterBoundsMax) ||
				!ContainsBounds(Header.SceneBoundsMin, Header.SceneBoundsMax, Page.SupportBoundsMin, Page.SupportBoundsMax) ||
				!FMath::IsFinite(Page.SupportSigma) ||
				!FMath::IsNearlyEqual(Page.SupportSigma, Header.SupportSigma, 1.e-12) ||
				!FMath::IsFinite(Page.MaxScaleCm) || Page.MaxScaleCm < 0.0 ||
				Page.Flags != EPageFlags::RotatedGaussianAABB2Sigma)
			{
				OutError = FString::Printf(TEXT("Page %llu has invalid bounds or support-expansion metadata"),
					static_cast<unsigned long long>(PageId));
				return false;
			}

			uint64 PayloadEnd = 0;
			if (!CheckedAdd(Header.PayloadOffset, Header.PayloadBytes, PayloadEnd) ||
				PayloadEnd > Manifest.PhysicalFileBytes)
			{
				OutError = TEXT("Manifest payload range overflows or exceeds the physical file size");
				return false;
			}

			TArray<FRange> PagePayloadRanges;
			PagePayloadRanges.Reserve(static_cast<int32>(StreamsPerPage));
			for (uint64 RelativeIndex = 0; RelativeIndex < Page.StreamCount; ++RelativeIndex)
			{
				const uint64 StreamIndex = Page.StreamStart + RelativeIndex;
				const FStreamRecord& Stream = Manifest.Streams[static_cast<int32>(StreamIndex)];
				if (Stream.PageId != Page.PageId)
				{
					OutError = FString::Printf(TEXT("Page %llu directory range contains a stream owned by page %llu"),
						static_cast<unsigned long long>(PageId),
						static_cast<unsigned long long>(Stream.PageId));
					return false;
				}

				EStreamEncoding ExpectedEncoding = static_cast<EStreamEncoding>(0);
				uint64 ExpectedComponents = 0;
				uint64 ExpectedStride = 0;
				if (!GetExpectedStreamContract(Stream.Semantic, ExpectedEncoding, ExpectedComponents, ExpectedStride))
				{
					OutError = FString::Printf(TEXT("Page %llu contains unsupported stream semantic %u"),
						static_cast<unsigned long long>(PageId), static_cast<uint32>(Stream.Semantic));
					return false;
				}
				const uint32 SemanticIndex = static_cast<uint32>(Stream.Semantic) - 1u;
				if (OutStreamsBySemantic[static_cast<int32>(SemanticIndex)] != nullptr)
				{
					OutError = FString::Printf(TEXT("Page %llu contains duplicate stream semantic %u"),
						static_cast<unsigned long long>(PageId), static_cast<uint32>(Stream.Semantic));
					return false;
				}
				if (Stream.Encoding != ExpectedEncoding ||
					Stream.ComponentCount != ExpectedComponents ||
					Stream.ElementStrideBytes != ExpectedStride ||
					Stream.ElementCount != Page.SplatCount ||
					Stream.Flags != 0)
				{
					OutError = FString::Printf(TEXT("Page %llu stream semantic %u violates its v1 descriptor contract"),
						static_cast<unsigned long long>(PageId), static_cast<uint32>(Stream.Semantic));
					return false;
				}

				uint64 ExpectedBytes = 0;
				uint64 StreamEnd = 0;
				if (!CheckedPageMultiply(Stream.ElementCount, Stream.ElementStrideBytes, ExpectedBytes) ||
					Stream.ByteSize != ExpectedBytes ||
					Stream.UncompressedByteSize != ExpectedBytes)
				{
					OutError = FString::Printf(TEXT("Page %llu stream semantic %u has overflowing or inconsistent byte sizes"),
						static_cast<unsigned long long>(PageId), static_cast<uint32>(Stream.Semantic));
					return false;
				}
				if ((Stream.FileOffset & 63ull) != 0 ||
					!CheckedAdd(Stream.FileOffset, Stream.ByteSize, StreamEnd) ||
					Stream.FileOffset < Header.PayloadOffset || StreamEnd > PayloadEnd)
				{
					OutError = FString::Printf(TEXT("Page %llu stream semantic %u is unaligned, out of bounds, or overflows"),
						static_cast<unsigned long long>(PageId), static_cast<uint32>(Stream.Semantic));
					return false;
				}
				if (!CheckedAdd(OutTotalBytes, Stream.ByteSize, OutTotalBytes))
				{
					OutError = FString::Printf(TEXT("Page %llu total payload byte size overflows uint64"),
						static_cast<unsigned long long>(PageId));
					return false;
				}
				OutStreamsBySemantic[static_cast<int32>(SemanticIndex)] = &Stream;
				PagePayloadRanges.Add({Stream.FileOffset, StreamEnd, StreamIndex});
			}

			for (const FStreamRecord* Stream : OutStreamsBySemantic)
			{
				if (Stream == nullptr)
				{
					OutError = FString::Printf(TEXT("Page %llu is missing one or more mandatory streams"),
						static_cast<unsigned long long>(PageId));
					return false;
				}
			}
			Algo::Sort(PagePayloadRanges, [](const FRange& A, const FRange& B) { return A.Begin < B.Begin; });
			for (int32 RangeIndex = 1; RangeIndex < PagePayloadRanges.Num(); ++RangeIndex)
			{
				if (RangesOverlap(PagePayloadRanges[RangeIndex - 1], PagePayloadRanges[RangeIndex]))
				{
					OutError = FString::Printf(TEXT("Page %llu payload streams overlap"),
						static_cast<unsigned long long>(PageId));
					return false;
				}
			}
			return true;
		}

		bool ValidateManifest(FManifest& Manifest, FString& OutError)
		{
			const FFileHeader& Header = Manifest.Header;
			if (Header.Version != FormatVersion)
			{
				OutError = FString::Printf(TEXT("Unsupported NanoGS Page version %u (expected %u)"),
					Header.Version, FormatVersion);
				return false;
			}
			if (Header.EndianTag != LittleEndianTag)
			{
				OutError = FString::Printf(TEXT("Unsupported endian tag 0x%08x (v1 is little-endian)"), Header.EndianTag);
				return false;
			}
			if (Header.PayloadFormat != EPayloadFormat::LosslessGeometrySH3HalfRGBA8SoA)
			{
				OutError = FString::Printf(TEXT("Unsupported NanoGS Page payload format %u"),
					static_cast<uint32>(Header.PayloadFormat));
				return false;
			}
			if (Header.Flags != RequiredFileFlags)
			{
				OutError = FString::Printf(TEXT("Unsupported or incomplete NanoGS Page flags 0x%08x (expected 0x%08x)"),
					static_cast<uint32>(Header.Flags), static_cast<uint32>(RequiredFileFlags));
				return false;
			}
			if (Header.HeaderBytes != FileHeaderBytes ||
				Header.PageRecordBytes != PageRecordBytes ||
				Header.StreamRecordBytes != StreamRecordBytes)
			{
				OutError = FString::Printf(
					TEXT("Unsupported record sizes (header=%llu, page=%llu, stream=%llu; expected 256/160/80)"),
					static_cast<unsigned long long>(Header.HeaderBytes),
					static_cast<unsigned long long>(Header.PageRecordBytes),
					static_cast<unsigned long long>(Header.StreamRecordBytes));
				return false;
			}
			if (Header.FileBytes != Manifest.PhysicalFileBytes)
			{
				OutError = FString::Printf(TEXT("Header file size %llu does not match physical file size %llu"),
					static_cast<unsigned long long>(Header.FileBytes),
					static_cast<unsigned long long>(Manifest.PhysicalFileBytes));
				return false;
			}
			if (Header.PageCount == 0 || Header.TotalSplatCount == 0 || Header.PayloadBytes == 0)
			{
				OutError = TEXT("NanoGS Page v1 requires non-zero pages, splats, and payload bytes");
				return false;
			}

			uint64 ExpectedStreamCount = 0;
			if (!CheckedPageMultiply(Header.PageCount, StreamsPerPage, ExpectedStreamCount) ||
				Header.StreamCount != ExpectedStreamCount)
			{
				OutError = FString::Printf(TEXT("Stream count %llu does not equal page count %llu x %llu"),
					static_cast<unsigned long long>(Header.StreamCount),
					static_cast<unsigned long long>(Header.PageCount),
					static_cast<unsigned long long>(StreamsPerPage));
				return false;
			}
			if (Header.PageCount > static_cast<uint64>(MAX_int32) ||
				Header.StreamCount > static_cast<uint64>(MAX_int32))
			{
				OutError = TEXT("Page or stream directory count exceeds the runtime TArray limit");
				return false;
			}
			if (Manifest.Pages.Num() != static_cast<int32>(Header.PageCount) ||
				Manifest.Streams.Num() != static_cast<int32>(Header.StreamCount))
			{
				OutError = TEXT("Decoded directory counts do not match the header");
				return false;
			}
			if (!IsFiniteOrderedPageBounds(Header.SceneBoundsMin, Header.SceneBoundsMax))
			{
				OutError = TEXT("Scene bounds contain non-finite or reversed coordinates");
				return false;
			}
			if (!FMath::IsFinite(Header.SupportSigma) || !FMath::IsNearlyEqual(Header.SupportSigma, 2.0, 1.e-12) ||
				!FMath::IsFinite(Header.UnitsToCentimeters) || !FMath::IsNearlyEqual(Header.UnitsToCentimeters, 1.0, 1.e-12))
			{
				OutError = TEXT("NanoGS Page v1 requires SupportSigma=2.0 and payload units already in centimeters (UnitsToCentimeters=1.0)");
				return false;
			}
			if (Header.CoordinateSystem != ECoordinateSystem::NanoGSLocalCentimetersSourceXYZ)
			{
				OutError = FString::Printf(TEXT("Unsupported coordinate-system contract %u"),
					static_cast<uint32>(Header.CoordinateSystem));
				return false;
			}
			if (Header.SHOrder != 3 || Header.SHLayout != ESHLayout::GraphdecoCoefficientMajorRGB)
			{
				OutError = FString::Printf(TEXT("Unsupported SH contract (order=%u, layout=%u); v1 requires SH3 coefficient-major RGB"),
					Header.SHOrder, static_cast<uint32>(Header.SHLayout));
				return false;
			}

			TSet<uint64> PageIds;
			TArray<FRange> OutputRanges;
			TArray<FRange> StreamIndexRanges;
			OutputRanges.Reserve(Manifest.Pages.Num());
			StreamIndexRanges.Reserve(Manifest.Pages.Num());

			for (const FPageRecord& Page : Manifest.Pages)
			{
				if (PageIds.Contains(Page.PageId))
				{
					OutError = FString::Printf(TEXT("Duplicate page ID %llu"), static_cast<unsigned long long>(Page.PageId));
					return false;
				}
				PageIds.Add(Page.PageId);
				if (Page.SplatCount == 0 || Page.StreamCount != StreamsPerPage)
				{
					OutError = FString::Printf(TEXT("Page %llu has invalid splat/stream counts (%llu/%llu)"),
						static_cast<unsigned long long>(Page.PageId),
						static_cast<unsigned long long>(Page.SplatCount),
						static_cast<unsigned long long>(Page.StreamCount));
					return false;
				}
				uint64 OutputEnd = 0;
				uint64 StreamEnd = 0;
				if (!CheckedAdd(Page.FirstOutputIndex, Page.SplatCount, OutputEnd) ||
					OutputEnd > Header.TotalSplatCount)
				{
					OutError = FString::Printf(TEXT("Page %llu output-index range overflows or exceeds TotalSplatCount"),
						static_cast<unsigned long long>(Page.PageId));
					return false;
				}
				if (!CheckedAdd(Page.StreamStart, Page.StreamCount, StreamEnd) ||
					StreamEnd > Header.StreamCount)
				{
					OutError = FString::Printf(TEXT("Page %llu stream-index range overflows or exceeds StreamCount"),
						static_cast<unsigned long long>(Page.PageId));
					return false;
				}
				OutputRanges.Add({Page.FirstOutputIndex, OutputEnd, Page.PageId});
				StreamIndexRanges.Add({Page.StreamStart, StreamEnd, Page.PageId});

				if (!IsFiniteOrderedPageBounds(Page.CenterBoundsMin, Page.CenterBoundsMax) ||
					!IsFiniteOrderedPageBounds(Page.SupportBoundsMin, Page.SupportBoundsMax))
				{
					OutError = FString::Printf(TEXT("Page %llu contains non-finite or reversed bounds"),
						static_cast<unsigned long long>(Page.PageId));
					return false;
				}
				if (!ContainsBounds(Page.SupportBoundsMin, Page.SupportBoundsMax, Page.CenterBoundsMin, Page.CenterBoundsMax) ||
					!ContainsBounds(Header.SceneBoundsMin, Header.SceneBoundsMax, Page.SupportBoundsMin, Page.SupportBoundsMax))
				{
					OutError = FString::Printf(TEXT("Page %llu support bounds do not contain center bounds, or exceed scene bounds"),
						static_cast<unsigned long long>(Page.PageId));
					return false;
				}
				if (!FMath::IsFinite(Page.SupportSigma) ||
					!FMath::IsNearlyEqual(Page.SupportSigma, Header.SupportSigma, 1.e-12) ||
					!FMath::IsFinite(Page.MaxScaleCm) || Page.MaxScaleCm < 0.0)
				{
					OutError = FString::Printf(TEXT("Page %llu has invalid support-expansion metadata"),
						static_cast<unsigned long long>(Page.PageId));
					return false;
				}
				if (Page.Flags != EPageFlags::RotatedGaussianAABB2Sigma)
				{
					OutError = FString::Printf(TEXT("Page %llu uses unsupported support-bounds flags 0x%08x"),
						static_cast<unsigned long long>(Page.PageId), static_cast<uint32>(Page.Flags));
					return false;
				}
			}

			const auto ValidateCompletePartition = [&OutError](TArray<FRange>& Ranges, const uint64 ExpectedEnd, const TCHAR* Name)
			{
				Algo::Sort(Ranges, [](const FRange& A, const FRange& B) { return A.Begin < B.Begin; });
				uint64 Cursor = 0;
				for (const FRange& Range : Ranges)
				{
					if (Range.Begin != Cursor)
					{
						OutError = FString::Printf(TEXT("%s ranges overlap or leave a gap at %llu (page %llu starts at %llu)"),
							Name,
							static_cast<unsigned long long>(Cursor),
							static_cast<unsigned long long>(Range.Label),
							static_cast<unsigned long long>(Range.Begin));
						return false;
					}
					Cursor = Range.End;
				}
				if (Cursor != ExpectedEnd)
				{
					OutError = FString::Printf(TEXT("%s ranges end at %llu instead of %llu"),
						Name,
						static_cast<unsigned long long>(Cursor),
						static_cast<unsigned long long>(ExpectedEnd));
					return false;
				}
				return true;
			};

			if (!ValidateCompletePartition(OutputRanges, Header.TotalSplatCount, TEXT("Page output-index")) ||
				!ValidateCompletePartition(StreamIndexRanges, Header.StreamCount, TEXT("Page stream-index")))
			{
				return false;
			}

			uint64 PayloadEnd = 0;
			if (!CheckedAdd(Header.PayloadOffset, Header.PayloadBytes, PayloadEnd))
			{
				OutError = TEXT("Payload range overflows uint64");
				return false;
			}
			if ((Header.PayloadOffset & 63ull) != 0)
			{
				OutError = FString::Printf(TEXT("Payload offset %llu is not 64-byte aligned"),
					static_cast<unsigned long long>(Header.PayloadOffset));
				return false;
			}

			TArray<FRange> PayloadRanges;
			PayloadRanges.Reserve(Manifest.Streams.Num());
			for (const FPageRecord& Page : Manifest.Pages)
			{
				uint32 SeenSemanticMask = 0;
				for (uint64 RelativeIndex = 0; RelativeIndex < Page.StreamCount; ++RelativeIndex)
				{
					const uint64 StreamIndex = Page.StreamStart + RelativeIndex;
					const FStreamRecord& Stream = Manifest.Streams[static_cast<int32>(StreamIndex)];
					if (Stream.PageId != Page.PageId)
					{
						OutError = FString::Printf(TEXT("Stream %llu claims page %llu but belongs to page-directory range for %llu"),
							static_cast<unsigned long long>(StreamIndex),
							static_cast<unsigned long long>(Stream.PageId),
							static_cast<unsigned long long>(Page.PageId));
						return false;
					}

					EStreamEncoding ExpectedEncoding = static_cast<EStreamEncoding>(0);
					uint64 ExpectedComponents = 0;
					uint64 ExpectedStride = 0;
					if (!GetExpectedStreamContract(Stream.Semantic, ExpectedEncoding, ExpectedComponents, ExpectedStride))
					{
						OutError = FString::Printf(TEXT("Stream %llu has unsupported semantic %u"),
							static_cast<unsigned long long>(StreamIndex), static_cast<uint32>(Stream.Semantic));
						return false;
					}
					const uint32 SemanticBit = 1u << (static_cast<uint32>(Stream.Semantic) - 1u);
					if ((SeenSemanticMask & SemanticBit) != 0)
					{
						OutError = FString::Printf(TEXT("Page %llu contains duplicate stream semantic %u"),
							static_cast<unsigned long long>(Page.PageId), static_cast<uint32>(Stream.Semantic));
						return false;
					}
					SeenSemanticMask |= SemanticBit;

					if (Stream.Encoding != ExpectedEncoding ||
						Stream.ComponentCount != ExpectedComponents ||
						Stream.ElementStrideBytes != ExpectedStride ||
						Stream.ElementCount != Page.SplatCount)
					{
						OutError = FString::Printf(
							TEXT("Page %llu stream semantic %u violates its encoding/components/stride/count contract"),
							static_cast<unsigned long long>(Page.PageId), static_cast<uint32>(Stream.Semantic));
						return false;
					}
					uint64 ExpectedBytes = 0;
					if (!CheckedPageMultiply(Stream.ElementCount, Stream.ElementStrideBytes, ExpectedBytes))
					{
						OutError = FString::Printf(TEXT("Page %llu stream semantic %u byte size overflows uint64"),
							static_cast<unsigned long long>(Page.PageId), static_cast<uint32>(Stream.Semantic));
						return false;
					}
					if (Stream.ByteSize != ExpectedBytes || Stream.UncompressedByteSize != ExpectedBytes)
					{
						OutError = FString::Printf(TEXT("Page %llu stream semantic %u has inconsistent byte sizes"),
							static_cast<unsigned long long>(Page.PageId), static_cast<uint32>(Stream.Semantic));
						return false;
					}
					if (Stream.Flags != 0)
					{
						OutError = FString::Printf(TEXT("Page %llu stream semantic %u has unsupported flags 0x%08x"),
							static_cast<unsigned long long>(Page.PageId),
							static_cast<uint32>(Stream.Semantic), static_cast<uint32>(Stream.Flags));
						return false;
					}
					if ((Stream.FileOffset & 63ull) != 0)
					{
						OutError = FString::Printf(TEXT("Page %llu stream semantic %u offset %llu is not 64-byte aligned"),
							static_cast<unsigned long long>(Page.PageId),
							static_cast<uint32>(Stream.Semantic),
							static_cast<unsigned long long>(Stream.FileOffset));
						return false;
					}

					uint64 StreamEnd = 0;
					if (!CheckedAdd(Stream.FileOffset, Stream.ByteSize, StreamEnd) ||
						Stream.FileOffset < Header.PayloadOffset || StreamEnd > PayloadEnd)
					{
						OutError = FString::Printf(TEXT("Page %llu stream semantic %u is outside the payload range or overflows"),
							static_cast<unsigned long long>(Page.PageId), static_cast<uint32>(Stream.Semantic));
						return false;
					}
					PayloadRanges.Add({Stream.FileOffset, StreamEnd, StreamIndex});
				}
				const uint32 RequiredSemanticMask = (1u << static_cast<uint32>(StreamsPerPage)) - 1u;
				if (SeenSemanticMask != RequiredSemanticMask)
				{
					OutError = FString::Printf(TEXT("Page %llu is missing one or more mandatory streams"),
						static_cast<unsigned long long>(Page.PageId));
					return false;
				}
			}

			Algo::Sort(PayloadRanges, [](const FRange& A, const FRange& B) { return A.Begin < B.Begin; });
			for (int32 RangeIndex = 1; RangeIndex < PayloadRanges.Num(); ++RangeIndex)
			{
				if (RangesOverlap(PayloadRanges[RangeIndex - 1], PayloadRanges[RangeIndex]))
				{
					OutError = FString::Printf(TEXT("Payload streams %llu and %llu overlap"),
						static_cast<unsigned long long>(PayloadRanges[RangeIndex - 1].Label),
						static_cast<unsigned long long>(PayloadRanges[RangeIndex].Label));
					return false;
				}
			}
			return true;
		}

		bool VerifyPayloadCRCs(
			IFileHandle& File,
			const FManifest& Manifest,
			const uint64 RequestedChunkBytes,
			FString& OutError)
		{
			if (RequestedChunkBytes == 0)
			{
				OutError = TEXT("CRCReadChunkBytes must be greater than zero");
				return false;
			}
			const uint64 ChunkBytes = FMath::Min<uint64>(RequestedChunkBytes, static_cast<uint64>(MAX_int32));
			TArray<uint8> Scratch;
			Scratch.SetNumUninitialized(static_cast<int32>(FMath::Max<uint64>(ChunkBytes, 8)));
			if (Manifest.Header.TotalSplatCount > static_cast<uint64>(MAX_int32))
			{
				OutError = TEXT("Full OriginalId permutation validation exceeds the runtime bit-array limit");
				return false;
			}
			TBitArray<> SeenOriginalIds(false, static_cast<int32>(Manifest.Header.TotalSplatCount));
			uint64 SeenOriginalIdCount = 0;

			for (int32 StreamIndex = 0; StreamIndex < Manifest.Streams.Num(); ++StreamIndex)
			{
				const FStreamRecord& Stream = Manifest.Streams[StreamIndex];
				const bool bOriginalIdStream = Stream.Semantic == EStreamSemantic::OriginalId;
				const uint64 StreamChunkBytes = bOriginalIdStream
					? FMath::Max<uint64>(8, (ChunkBytes / 8) * 8)
					: ChunkBytes;
				if (Stream.FileOffset > static_cast<uint64>(MAX_int64) || !File.Seek(static_cast<int64>(Stream.FileOffset)))
				{
					OutError = FString::Printf(TEXT("Failed to seek to payload stream %d"), StreamIndex);
					return false;
				}
				uint64 Remaining = Stream.ByteSize;
				uint32 CRC = 0;
				while (Remaining > 0)
				{
					const uint64 ThisRead = FMath::Min(Remaining, StreamChunkBytes);
					if (!File.Read(Scratch.GetData(), static_cast<int64>(ThisRead)))
					{
						OutError = FString::Printf(TEXT("Failed to read payload stream %d for CRC verification"), StreamIndex);
						return false;
					}
					CRC = UpdateCRC32(CRC, Scratch.GetData(), ThisRead);
					if (bOriginalIdStream)
					{
						check((ThisRead & 7ull) == 0);
						for (uint64 ByteOffset = 0; ByteOffset < ThisRead; ByteOffset += 8)
						{
							uint64 OriginalId = 0;
							for (uint32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
							{
								OriginalId |= uint64(Scratch[static_cast<int32>(ByteOffset + ByteIndex)]) << (ByteIndex * 8u);
							}
							if (OriginalId >= Manifest.Header.TotalSplatCount)
							{
								OutError = FString::Printf(TEXT("OriginalId %llu in stream %d is outside [0, %llu)"),
									static_cast<unsigned long long>(OriginalId),
									StreamIndex,
									static_cast<unsigned long long>(Manifest.Header.TotalSplatCount));
								return false;
							}
							if (SeenOriginalIds[static_cast<int32>(OriginalId)])
							{
								OutError = FString::Printf(TEXT("Duplicate OriginalId %llu in stream %d"),
									static_cast<unsigned long long>(OriginalId), StreamIndex);
								return false;
							}
							SeenOriginalIds[static_cast<int32>(OriginalId)] = true;
							++SeenOriginalIdCount;
						}
					}
					Remaining -= ThisRead;
				}
				if (CRC != Stream.PayloadCRC32)
				{
					OutError = FString::Printf(TEXT("Payload CRC mismatch for stream %d (page %llu, semantic %u): got 0x%08x, expected 0x%08x"),
						StreamIndex,
						static_cast<unsigned long long>(Stream.PageId),
						static_cast<uint32>(Stream.Semantic),
						CRC,
						Stream.PayloadCRC32);
					return false;
				}
			}
			if (SeenOriginalIdCount != Manifest.Header.TotalSplatCount)
			{
				OutError = FString::Printf(TEXT("OriginalId streams contain %llu IDs instead of the declared %llu"),
					static_cast<unsigned long long>(SeenOriginalIdCount),
					static_cast<unsigned long long>(Manifest.Header.TotalSplatCount));
				return false;
			}
			return true;
		}
	}

	uint32 UpdateCRC32(const uint32 PreviousCRC, const uint8* Data, const uint64 NumBytes)
	{
		static const TArray<uint32> Table = []
		{
			TArray<uint32> Result;
			Result.SetNumUninitialized(256);
			for (uint32 TableIndex = 0; TableIndex < 256; ++TableIndex)
			{
				uint32 Entry = TableIndex;
				for (uint32 BitIndex = 0; BitIndex < 8; ++BitIndex)
				{
					Entry = (Entry >> 1u) ^ (0xEDB88320u & (0u - (Entry & 1u)));
				}
				Result[static_cast<int32>(TableIndex)] = Entry;
			}
			return Result;
		}();

		uint32 CRC = ~PreviousCRC;
		for (uint64 ByteIndex = 0; ByteIndex < NumBytes; ++ByteIndex)
		{
			CRC = (CRC >> 8u) ^ Table[static_cast<int32>((CRC ^ Data[ByteIndex]) & 0xffu)];
		}
		return ~CRC;
	}

	bool FManifest::BuildPageLookup(FString& OutError)
	{
		OutError.Reset();
		bPageLookupBuilt = false;
		PageIdToOrdinal.Reset();
		IndexedPageIdsByOrdinal.Reset();

		PageIdToOrdinal.Reserve(Pages.Num());
		IndexedPageIdsByOrdinal.Reserve(Pages.Num());
		for (int32 PageOrdinal = 0; PageOrdinal < Pages.Num(); ++PageOrdinal)
		{
			const uint64 PageId = Pages[PageOrdinal].PageId;
			if (PageIdToOrdinal.Contains(PageId))
			{
				OutError = FString::Printf(TEXT("Duplicate page ID %llu while building the PageId lookup"),
					static_cast<unsigned long long>(PageId));
				PageIdToOrdinal.Reset();
				IndexedPageIdsByOrdinal.Reset();
				return false;
			}
			PageIdToOrdinal.Add(PageId, PageOrdinal);
			IndexedPageIdsByOrdinal.Add(PageId);
		}

		bPageLookupBuilt = true;
		return true;
	}

	bool FManifest::HasPageLookup() const
	{
		return bPageLookupBuilt &&
			PageIdToOrdinal.Num() == Pages.Num() &&
			IndexedPageIdsByOrdinal.Num() == Pages.Num();
	}

	const FPageRecord* FManifest::FindPage(const uint64 PageId) const
	{
		if (!HasPageLookup())
		{
			return nullptr;
		}

		const int32* PageOrdinal = PageIdToOrdinal.Find(PageId);
		if (PageOrdinal == nullptr ||
			!Pages.IsValidIndex(*PageOrdinal) ||
			!IndexedPageIdsByOrdinal.IsValidIndex(*PageOrdinal) ||
			IndexedPageIdsByOrdinal[*PageOrdinal] != PageId ||
			Pages[*PageOrdinal].PageId != PageId)
		{
			return nullptr;
		}
		return &Pages[*PageOrdinal];
	}

	TConstArrayView64<uint8> FOwnedPageStream::GetRawView() const
	{
		return TConstArrayView64<uint8>(Bytes.GetData(), Bytes.Num());
	}

	const FOwnedPageStream* FOwnedPagePayload::FindStream(const EStreamSemantic Semantic) const
	{
		return Streams.FindByPredicate([Semantic](const FOwnedPageStream& Stream)
		{
			return Stream.Record.Semantic == Semantic;
		});
	}

	TConstArrayView64<uint8> FOwnedPagePayload::GetRawStream(const EStreamSemantic Semantic) const
	{
		const FOwnedPageStream* Stream = FindStream(Semantic);
		return Stream != nullptr ? Stream->GetRawView() : TConstArrayView64<uint8>();
	}

	namespace
	{
		template <typename ElementType>
		TConstArrayView64<ElementType> GetTypedPageStream(
			const FOwnedPagePayload& Payload,
			const EStreamSemantic Semantic,
			const EStreamEncoding Encoding)
		{
#if PLATFORM_LITTLE_ENDIAN
			const FOwnedPageStream* Stream = Payload.FindStream(Semantic);
			if (Stream == nullptr || Stream->Record.Encoding != Encoding ||
				(Stream->Bytes.Num() % static_cast<int64>(sizeof(ElementType))) != 0 ||
				(reinterpret_cast<UPTRINT>(Stream->Bytes.GetData()) % alignof(ElementType)) != 0)
			{
				return TConstArrayView64<ElementType>();
			}
			return TConstArrayView64<ElementType>(
				reinterpret_cast<const ElementType*>(Stream->Bytes.GetData()),
				Stream->Bytes.Num() / static_cast<int64>(sizeof(ElementType)));
#else
			return TConstArrayView64<ElementType>();
#endif
		}
	}

	TConstArrayView64<float> FOwnedPagePayload::GetPositions() const
	{
		return GetTypedPageStream<float>(*this, EStreamSemantic::Position, EStreamEncoding::Float32);
	}

	TConstArrayView64<float> FOwnedPagePayload::GetRotations() const
	{
		return GetTypedPageStream<float>(*this, EStreamSemantic::Rotation, EStreamEncoding::Float32);
	}

	TConstArrayView64<float> FOwnedPagePayload::GetScales() const
	{
		return GetTypedPageStream<float>(*this, EStreamSemantic::Scale, EStreamEncoding::Float32);
	}

	TConstArrayView64<uint8> FOwnedPagePayload::GetColorOpacity() const
	{
		return GetRawStream(EStreamSemantic::ColorOpacity);
	}

	TConstArrayView64<uint16> FOwnedPagePayload::GetSphericalHarmonicsHalfBits() const
	{
		return GetTypedPageStream<uint16>(*this, EStreamSemantic::SphericalHarmonics, EStreamEncoding::Float16);
	}

	TConstArrayView64<uint64> FOwnedPagePayload::GetOriginalIds() const
	{
		return GetTypedPageStream<uint64>(*this, EStreamSemantic::OriginalId, EStreamEncoding::UInt64);
	}

	bool FPageFileReader::ReadManifest(
		const FString& Filename,
		FManifest& OutManifest,
		FString& OutError,
		const FReadOptions& Options)
	{
		OutManifest = FManifest();
		OutError.Reset();

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TUniquePtr<IFileHandle> File(PlatformFile.OpenRead(*Filename));
		if (!File)
		{
			OutError = FString::Printf(TEXT("Failed to open NanoGS Page file: %s"), *Filename);
			return false;
		}

		const int64 SignedPhysicalFileBytes = File->Size();
		if (SignedPhysicalFileBytes < static_cast<int64>(FileHeaderBytes))
		{
			OutError = FString::Printf(TEXT("NanoGS Page file is smaller than its %llu-byte header: %s"),
				static_cast<unsigned long long>(FileHeaderBytes), *Filename);
			return false;
		}
		const uint64 PhysicalFileBytes = static_cast<uint64>(SignedPhysicalFileBytes);

		TArray<uint8> HeaderBytes;
		if (!ReadFileRange(*File, 0, FileHeaderBytes, HeaderBytes, OutError, TEXT("file header")))
		{
			return false;
		}

		FManifest Candidate;
		Candidate.SourceFilename = Filename;
		Candidate.PhysicalFileBytes = PhysicalFileBytes;
		if (!ParseHeader(HeaderBytes, Candidate.Header, OutError))
		{
			return false;
		}

		const FFileHeader& Header = Candidate.Header;
		if (Header.FileBytes != PhysicalFileBytes)
		{
			OutError = FString::Printf(TEXT("Header file size %llu does not match physical file size %llu"),
				static_cast<unsigned long long>(Header.FileBytes),
				static_cast<unsigned long long>(PhysicalFileBytes));
			return false;
		}
		if (Header.HeaderBytes != FileHeaderBytes ||
			Header.PageRecordBytes != PageRecordBytes ||
			Header.StreamRecordBytes != StreamRecordBytes)
		{
			OutError = TEXT("Unsupported NanoGS Page header or directory record size");
			return false;
		}

		uint64 PageDirectoryBytes = 0;
		uint64 StreamDirectoryBytes = 0;
		uint64 TotalDirectoryBytes = 0;
		if (!CheckedPageMultiply(Header.PageCount, Header.PageRecordBytes, PageDirectoryBytes) ||
			!CheckedPageMultiply(Header.StreamCount, Header.StreamRecordBytes, StreamDirectoryBytes) ||
			!CheckedAdd(PageDirectoryBytes, StreamDirectoryBytes, TotalDirectoryBytes))
		{
			OutError = TEXT("Directory byte-size arithmetic overflows uint64");
			return false;
		}
		if (TotalDirectoryBytes > Options.MaxDirectoryBytes ||
			PageDirectoryBytes > static_cast<uint64>(MAX_int32) ||
			StreamDirectoryBytes > static_cast<uint64>(MAX_int32))
		{
			OutError = FString::Printf(TEXT("Directory requires %llu bytes, exceeding the configured/runtime allocation limit"),
				static_cast<unsigned long long>(TotalDirectoryBytes));
			return false;
		}

		uint64 PageDirectoryEnd = 0;
		uint64 StreamDirectoryEnd = 0;
		uint64 PayloadEnd = 0;
		if (!ValidateRange(Header.PageDirectoryOffset, PageDirectoryBytes, PhysicalFileBytes, PageDirectoryEnd) ||
			!ValidateRange(Header.StreamDirectoryOffset, StreamDirectoryBytes, PhysicalFileBytes, StreamDirectoryEnd) ||
			!ValidateRange(Header.PayloadOffset, Header.PayloadBytes, PhysicalFileBytes, PayloadEnd))
		{
			OutError = TEXT("Header contains an out-of-file or overflowing directory/payload range");
			return false;
		}

		const FRange HeaderRange{0, Header.HeaderBytes, 0};
		const FRange PageDirectoryRange{Header.PageDirectoryOffset, PageDirectoryEnd, 1};
		const FRange StreamDirectoryRange{Header.StreamDirectoryOffset, StreamDirectoryEnd, 2};
		const FRange PayloadRange{Header.PayloadOffset, PayloadEnd, 3};
		if (RangesOverlap(HeaderRange, PageDirectoryRange) ||
			RangesOverlap(HeaderRange, StreamDirectoryRange) ||
			RangesOverlap(HeaderRange, PayloadRange) ||
			RangesOverlap(PageDirectoryRange, StreamDirectoryRange) ||
			RangesOverlap(PageDirectoryRange, PayloadRange) ||
			RangesOverlap(StreamDirectoryRange, PayloadRange))
		{
			OutError = TEXT("Header, page directory, stream directory, and payload ranges must not overlap");
			return false;
		}

		TArray<uint8> PageDirectoryData;
		TArray<uint8> StreamDirectoryData;
		if (!ReadFileRange(*File, Header.PageDirectoryOffset, PageDirectoryBytes,
			PageDirectoryData, OutError, TEXT("page directory")) ||
			!ReadFileRange(*File, Header.StreamDirectoryOffset, StreamDirectoryBytes,
				StreamDirectoryData, OutError, TEXT("stream directory")))
		{
			return false;
		}

		if (Options.bVerifyMetadataCRCs)
		{
			TArray<uint8> HeaderForCRC = HeaderBytes;
			FMemory::Memzero(
				HeaderForCRC.GetData() + HeaderCRC32Offset,
				static_cast<SIZE_T>(HeaderCRC32Bytes));
			const uint32 ComputedHeaderCRC = UpdateCRC32(0, HeaderForCRC.GetData(), HeaderForCRC.Num());
			if (ComputedHeaderCRC != Header.HeaderCRC32)
			{
				OutError = FString::Printf(TEXT("Header CRC mismatch: got 0x%08x, expected 0x%08x"),
					ComputedHeaderCRC, Header.HeaderCRC32);
				return false;
			}

			uint32 ComputedDirectoryCRC = UpdateCRC32(0, PageDirectoryData.GetData(), PageDirectoryData.Num());
			ComputedDirectoryCRC = UpdateCRC32(
				ComputedDirectoryCRC,
				StreamDirectoryData.GetData(),
				StreamDirectoryData.Num());
			if (ComputedDirectoryCRC != Header.DirectoryCRC32)
			{
				OutError = FString::Printf(TEXT("Directory CRC mismatch: got 0x%08x, expected 0x%08x"),
					ComputedDirectoryCRC, Header.DirectoryCRC32);
				return false;
			}
		}

		if (!ParsePages(PageDirectoryData, Header.PageCount, Candidate.Pages, OutError) ||
			!ParseStreams(StreamDirectoryData, Header.StreamCount, Candidate.Streams, OutError) ||
			!ValidateManifest(Candidate, OutError) ||
			!Candidate.BuildPageLookup(OutError))
		{
			return false;
		}

		if (Options.bVerifyPayloadCRCs &&
			!VerifyPayloadCRCs(*File, Candidate, Options.CRCReadChunkBytes, OutError))
		{
			return false;
		}

		OutManifest = MoveTemp(Candidate);
		return true;
	}

	bool FPageFileReader::ReadPagePayload(
		const FManifest& Manifest,
		const uint64 PageId,
		FOwnedPagePayload& OutPayload,
		FString& OutError,
		const FPagePayloadReadOptions& Options)
	{
		OutPayload = FOwnedPagePayload();
		OutError.Reset();

		const FPageRecord* Page = nullptr;
		TArray<const FStreamRecord*> StreamsBySemantic;
		uint64 TotalBytes = 0;
		if (!CollectAndValidatePageStreams(
			Manifest, PageId, Page, StreamsBySemantic, TotalBytes, OutError))
		{
			return false;
		}
		if (TotalBytes > Options.MaxPagePayloadBytes)
		{
			OutError = FString::Printf(TEXT("Page %llu requires %llu payload bytes, exceeding the configured limit %llu"),
				static_cast<unsigned long long>(PageId),
				static_cast<unsigned long long>(TotalBytes),
				static_cast<unsigned long long>(Options.MaxPagePayloadBytes));
			return false;
		}

		IFileHandle* File = GThreadLocalPageFile.Acquire(Manifest, OutError);
		if (File == nullptr)
		{
			OutError = FString::Printf(TEXT("Failed to acquire NanoGS Page file for page %llu: %s"),
				static_cast<unsigned long long>(PageId), *OutError);
			return false;
		}

		FOwnedPagePayload Candidate;
		Candidate.Page = *Page;
		Candidate.TotalOwnedBytes = TotalBytes;
		Candidate.Streams.Reserve(static_cast<int32>(StreamsPerPage));
		for (const FStreamRecord* Stream : StreamsBySemantic)
		{
			if (Stream->FileOffset > static_cast<uint64>(MAX_int64) ||
				Stream->ByteSize > static_cast<uint64>(MAX_int64))
			{
				OutError = FString::Printf(TEXT("Page %llu stream semantic %u exceeds the platform file/allocation limit"),
					static_cast<unsigned long long>(PageId), static_cast<uint32>(Stream->Semantic));
				return false;
			}

			FOwnedPageStream& Owned = Candidate.Streams.AddDefaulted_GetRef();
			Owned.Record = *Stream;
			Owned.Bytes.SetNumUninitialized(static_cast<int64>(Stream->ByteSize));
			if (!File->Seek(static_cast<int64>(Stream->FileOffset)) ||
				!File->Read(Owned.Bytes.GetData(), static_cast<int64>(Stream->ByteSize)))
			{
				GThreadLocalPageFile.Invalidate();
				OutError = FString::Printf(TEXT("Failed to read page %llu stream semantic %u at offset %llu (%llu bytes)"),
					static_cast<unsigned long long>(PageId),
					static_cast<uint32>(Stream->Semantic),
					static_cast<unsigned long long>(Stream->FileOffset),
					static_cast<unsigned long long>(Stream->ByteSize));
				return false;
			}
			if (Options.bVerifyStreamCRCs)
			{
				const uint32 CRC = UpdateCRC32(0, Owned.Bytes.GetData(), Stream->ByteSize);
				if (CRC != Stream->PayloadCRC32)
				{
					OutError = FString::Printf(TEXT("Page %llu stream semantic %u CRC mismatch: got 0x%08x, expected 0x%08x"),
						static_cast<unsigned long long>(PageId),
						static_cast<uint32>(Stream->Semantic),
						CRC,
						Stream->PayloadCRC32);
					return false;
				}
			}
		}

		OutPayload = MoveTemp(Candidate);
		return true;
	}
}
