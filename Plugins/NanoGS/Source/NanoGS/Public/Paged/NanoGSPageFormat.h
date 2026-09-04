// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Stable, endian-independent on-disk contracts for NanoGS Page files.
 *
 * The wire format is always little-endian and is decoded field by field. These
 * C++ types describe values after decoding; their native sizeof/alignment is not
 * part of the file format. See Docs/nanogs_page_v1_format_cn.md for byte offsets.
 */
namespace NanoGS::Paged
{
	inline constexpr uint8 FileMagic[8] = {'N', 'G', 'S', 'P', 'A', 'G', 'E', '1'};
	inline constexpr uint32 FormatVersion = 1;
	inline constexpr uint32 LittleEndianTag = 0x01020304u;
	inline constexpr uint64 FileHeaderBytes = 256;
	inline constexpr uint64 PageRecordBytes = 160;
	inline constexpr uint64 StreamRecordBytes = 80;
	inline constexpr uint64 StreamsPerPage = 6;
	inline constexpr uint64 HeaderCRC32Offset = 192;
	inline constexpr uint64 HeaderCRC32Bytes = 4;

	enum class EPayloadFormat : uint32
	{
		LosslessGeometrySH3HalfRGBA8SoA = 1,
	};

	enum class ECoordinateSystem : uint32
	{
		/** NanoGS actor-local centimeters; source PLY XYZ basis is preserved. */
		NanoGSLocalCentimetersSourceXYZ = 1,
	};

	enum class ESHLayout : uint32
	{
		/** DC first, then 15 higher coefficients; each coefficient stores RGB. */
		GraphdecoCoefficientMajorRGB = 1,
	};

	enum class EStreamSemantic : uint32
	{
		Position = 1,
		Rotation = 2,
		Scale = 3,
		ColorOpacity = 4,
		SphericalHarmonics = 5,
		OriginalId = 6,
	};

	enum class EStreamEncoding : uint32
	{
		Float32 = 1,
		UNorm8 = 2,
		Float16 = 3,
		UInt64 = 4,
	};

	enum class EFileFlags : uint32
	{
		None = 0,
		PerStreamCRC32 = 1u << 0u,
		SourceIdsDenseZeroBased = 1u << 1u,
		Payload64ByteAligned = 1u << 2u,
	};

	ENUM_CLASS_FLAGS(EFileFlags);

	inline constexpr EFileFlags RequiredFileFlags =
		EFileFlags::PerStreamCRC32 |
		EFileFlags::SourceIdsDenseZeroBased |
		EFileFlags::Payload64ByteAligned;

	enum class EPageFlags : uint32
	{
		None = 0,
		/** SupportBounds are the union of per-splat rotated Gaussian 2-sigma AABBs. */
		RotatedGaussianAABB2Sigma = 1u << 0u,
	};

	ENUM_CLASS_FLAGS(EPageFlags);

	struct NANOGS_API FFileHeader
	{
		uint32 Version = 0;
		uint32 EndianTag = 0;
		EPayloadFormat PayloadFormat = static_cast<EPayloadFormat>(0);
		EFileFlags Flags = EFileFlags::None;
		uint64 HeaderBytes = 0;
		uint64 FileBytes = 0;
		uint64 PageCount = 0;
		uint64 PageDirectoryOffset = 0;
		uint64 PageRecordBytes = 0;
		uint64 StreamCount = 0;
		uint64 StreamDirectoryOffset = 0;
		uint64 StreamRecordBytes = 0;
		uint64 PayloadOffset = 0;
		uint64 PayloadBytes = 0;
		uint64 TotalSplatCount = 0;
		FVector3d SceneBoundsMin = FVector3d::ZeroVector;
		FVector3d SceneBoundsMax = FVector3d::ZeroVector;
		double SupportSigma = 0.0;
		double UnitsToCentimeters = 0.0;
		ECoordinateSystem CoordinateSystem = static_cast<ECoordinateSystem>(0);
		uint32 SHOrder = 0;
		ESHLayout SHLayout = static_cast<ESHLayout>(0);
		uint32 DirectoryCRC32 = 0;
		uint32 HeaderCRC32 = 0;
	};

	struct NANOGS_API FPageRecord
	{
		uint64 PageId = 0;
		uint64 FirstOutputIndex = 0;
		uint64 SplatCount = 0;
		uint64 StreamStart = 0;
		uint64 StreamCount = 0;
		FVector3d CenterBoundsMin = FVector3d::ZeroVector;
		FVector3d CenterBoundsMax = FVector3d::ZeroVector;
		FVector3d SupportBoundsMin = FVector3d::ZeroVector;
		FVector3d SupportBoundsMax = FVector3d::ZeroVector;
		double SupportSigma = 0.0;
		double MaxScaleCm = 0.0;
		EPageFlags Flags = EPageFlags::None;
	};

	struct NANOGS_API FStreamRecord
	{
		uint64 PageId = 0;
		EStreamSemantic Semantic = static_cast<EStreamSemantic>(0);
		EStreamEncoding Encoding = static_cast<EStreamEncoding>(0);
		uint64 ElementCount = 0;
		uint64 ComponentCount = 0;
		uint64 ElementStrideBytes = 0;
		uint64 FileOffset = 0;
		uint64 ByteSize = 0;
		uint64 UncompressedByteSize = 0;
		uint32 PayloadCRC32 = 0;
		/** Reserved for a future version; must be zero in v1. */
		uint32 Flags = 0;
	};

	struct NANOGS_API FManifest
	{
		FString SourceFilename;
		uint64 PhysicalFileBytes = 0;
		FFileHeader Header;
		TArray<FPageRecord> Pages;
		TArray<FStreamRecord> Streams;

		/**
		 * Build the transient PageId -> page-directory ordinal lookup.
		 *
		 * This is deliberately not part of the Page v1 wire format. ReadManifest
		 * calls it after validating the decoded directories. Manually assembled
		 * manifests must call it explicitly after their Pages array is final.
		 * Duplicate PageIds fail closed and leave no usable lookup behind.
		 */
		bool BuildPageLookup(FString& OutError);

		/** True only when a complete lookup was built for the current page count. */
		bool HasPageLookup() const;

		/** O(1) lookup. Missing or detectably stale lookup state fails closed. */
		const FPageRecord* FindPage(uint64 PageId) const;

	private:
		TMap<uint64, int32> PageIdToOrdinal;
		TArray<uint64> IndexedPageIdsByOrdinal;
		bool bPageLookupBuilt = false;
	};
}
