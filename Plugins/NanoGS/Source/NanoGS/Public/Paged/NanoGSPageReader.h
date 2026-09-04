// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Paged/NanoGSPageFormat.h"

namespace NanoGS::Paged
{
	struct NANOGS_API FReadOptions
	{
		/** Verify the header CRC and directory CRC while loading. */
		bool bVerifyMetadataCRCs = true;

		/** Stream every payload range and verify its CRC. Off by default for fast manifest opens. */
		bool bVerifyPayloadCRCs = false;

		/** Hard allocation guard for both directories combined. */
		uint64 MaxDirectoryBytes = 512ull * 1024ull * 1024ull;

		/** Scratch size for streaming payload CRC verification. */
		uint64 CRCReadChunkBytes = 8ull * 1024ull * 1024ull;
	};

	/** Options for a single random-access page payload read. */
	struct NANOGS_API FPagePayloadReadOptions
	{
		/** Verify each of the selected page's six payload CRCs after reading. */
		bool bVerifyStreamCRCs = true;

		/** Allocation guard over the six streams of one page (alignment gaps are not allocated). */
		uint64 MaxPagePayloadBytes = 512ull * 1024ull * 1024ull;
	};

	/** One owned stream from a page. Views remain valid while this object is not moved or modified. */
	struct NANOGS_API FOwnedPageStream
	{
		FStreamRecord Record;
		TArray64<uint8> Bytes;

		TConstArrayView64<uint8> GetRawView() const;
	};

	/** The six independently-owned SoA streams for one page. */
	struct NANOGS_API FOwnedPagePayload
	{
		FPageRecord Page;
		uint64 TotalOwnedBytes = 0;
		TArray<FOwnedPageStream> Streams;

		const FOwnedPageStream* FindStream(EStreamSemantic Semantic) const;
		TConstArrayView64<uint8> GetRawStream(EStreamSemantic Semantic) const;

		/** Flattened XYZ / XYZW values, with counts 3N / 4N / 3N. */
		TConstArrayView64<float> GetPositions() const;
		TConstArrayView64<float> GetRotations() const;
		TConstArrayView64<float> GetScales() const;

		/** Interleaved RGBA bytes (4N), coefficient-major SH half bits (48N), and source IDs (N). */
		TConstArrayView64<uint8> GetColorOpacity() const;
		TConstArrayView64<uint16> GetSphericalHarmonicsHalfBits() const;
		TConstArrayView64<uint64> GetOriginalIds() const;
	};

	/** Standard reflected IEEE CRC-32 (polynomial 0xEDB88320), zlib-compatible. */
	NANOGS_API uint32 UpdateCRC32(uint32 PreviousCRC, const uint8* Data, uint64 NumBytes);

	class NANOGS_API FPageFileReader
	{
	public:
		/** Read and strictly validate a v1 manifest. Renderer/resource creation is intentionally out of scope. */
		static bool ReadManifest(
			const FString& Filename,
			FManifest& OutManifest,
			FString& OutError,
			const FReadOptions& Options = FReadOptions());

		/**
		 * Open Manifest.SourceFilename and read only the six ranges belonging to PageId.
		 * The manifest/page descriptors are revalidated defensively before allocation.
		 */
		static bool ReadPagePayload(
			const FManifest& Manifest,
			uint64 PageId,
			FOwnedPagePayload& OutPayload,
			FString& OutError,
			const FPagePayloadReadOptions& Options = FPagePayloadReadOptions());
	};
}
