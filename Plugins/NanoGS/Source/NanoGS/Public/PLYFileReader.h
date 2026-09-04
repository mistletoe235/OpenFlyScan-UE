// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianDataTypes.h"

/**
 * PLY file header information
 */
struct FPLYHeader
{
	/** Total number of vertices/splats */
	int32 VertexCount = 0;

	/** Is binary little endian format */
	bool bBinaryLittleEndian = true;

	/** Property names in order */
	TArray<FString> PropertyNames;

	/** Property name to byte offset mapping */
	TMap<FString, int32> PropertyOffsets;

	/** Bytes per vertex */
	int32 VertexStride = 0;

	/** Header end position in file */
	int64 DataOffset = 0;
};

struct FPLYFileInfo
{
	/** Total file size on disk */
	int64 FileSize = 0;

	/** Total number of splats/vertices in the file */
	int32 VertexCount = 0;

	/** Bytes per vertex in the binary payload */
	int32 VertexStride = 0;

	/** Header end position in file */
	int64 DataOffset = 0;

	/** Detected SH bands (0-3) */
	int32 SHBands = 0;
};

/**
 * Utility class for reading PLY files containing Gaussian Splatting data
 * Supports the standard PLY format from 3D Gaussian Splatting training
 * Uses streamed I/O to handle files larger than 2 GB
 */
class NANOGS_API FPLYFileReader
{
public:
	/**
	 * Read PLY file and extract Gaussian splat data
	 * @param FilePath Path to the .ply file
	 * @param OutSplats Output array of splat data
	 * @param OutError Error message if reading failed
	 * @param OutSHBands Optional output for detected SH band count (0-3)
	 * @param MaxSHBandsToRead Maximum higher-order SH bands to decode (-1 = all detected)
	 * @return True if successful
	 */
	static bool ReadPLYFile(
		const FString& FilePath,
		TArray<FGaussianSplatData>& OutSplats,
		FString& OutError,
		int32* OutSHBands = nullptr,
		int32 MaxSHBandsToRead = INDEX_NONE);

	/**
	 * Read only the PLY header and basic metadata.
	 * Used for fast preflight validation before allocating large import buffers.
	 */
	static bool ProbePLYFile(const FString& FilePath, FPLYFileInfo& OutInfo, FString& OutError);

	/**
	 * Check if a file is a valid PLY file
	 * @param FilePath Path to the file
	 * @return True if the file appears to be a valid PLY file
	 */
	static bool IsValidPLYFile(const FString& FilePath);

private:
	/**
	 * Parse the PLY header from a file handle
	 * Reads header bytes and parses format/property information
	 * After parsing, the file handle position is at the start of vertex data
	 * @param FileHandle Open file handle positioned at beginning
	 * @param OutHeader Parsed header information
	 * @param OutError Error message if parsing failed
	 * @return True if successful
	 */
	static bool ParseHeader(IFileHandle* FileHandle, FPLYHeader& OutHeader, FString& OutError);

	/**
	 * Read vertex data from the PLY file using streamed I/O
	 * @param FileHandle Open file handle positioned at start of vertex data
	 * @param Header Parsed header information
	 * @param OutSplats Output array of splat data
	 * @param OutError Error message if reading failed
	 * @return True if successful
	 */
	static bool ReadVertexData(
		IFileHandle* FileHandle,
		const FPLYHeader& Header,
		TArray<FGaussianSplatData>& OutSplats,
		FString& OutError,
		int32 MaxSHBandsToRead);

	/**
	 * Linearize splat data from raw PLY values
	 * - Normalizes quaternion
	 * - Applies exp to scale (PLY stores log-scale)
	 * - Applies sigmoid to opacity
	 * @param Splat Splat data to linearize (modified in place)
	 */
	static void LinearizeSplatData(FGaussianSplatData& Splat);

};
