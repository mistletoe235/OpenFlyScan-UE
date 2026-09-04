// Copyright Epic Games, Inc. All Rights Reserved.

#include "PLYFileReader.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

bool FPLYFileReader::ProbePLYFile(const FString& FilePath, FPLYFileInfo& OutInfo, FString& OutError)
{
	OutInfo = FPLYFileInfo{};

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*FilePath));

	if (!FileHandle)
	{
		OutError = FString::Printf(TEXT("Failed to open file: %s"), *FilePath);
		return false;
	}

	OutInfo.FileSize = FileHandle->Size();
	if (OutInfo.FileSize < 4)
	{
		OutError = TEXT("File too small to be a valid PLY file");
		return false;
	}

	FPLYHeader Header;
	if (!ParseHeader(FileHandle.Get(), Header, OutError))
	{
		return false;
	}

	OutInfo.VertexCount = Header.VertexCount;
	OutInfo.VertexStride = Header.VertexStride;
	OutInfo.DataOffset = Header.DataOffset;

	if (Header.PropertyOffsets.Contains(TEXT("f_rest_44")))
	{
		OutInfo.SHBands = 3;
	}
	else if (Header.PropertyOffsets.Contains(TEXT("f_rest_23")))
	{
		OutInfo.SHBands = 2;
	}
	else if (Header.PropertyOffsets.Contains(TEXT("f_rest_8")))
	{
		OutInfo.SHBands = 1;
	}
	else
	{
		OutInfo.SHBands = 0;
	}

	const int64 ExpectedEnd = Header.DataOffset + static_cast<int64>(Header.VertexCount) * Header.VertexStride;
	if (ExpectedEnd > OutInfo.FileSize)
	{
		OutError = FString::Printf(TEXT("File truncated: expected %lld bytes of vertex data, file size is %lld"),
			ExpectedEnd, OutInfo.FileSize);
		return false;
	}

	return true;
}

bool FPLYFileReader::ReadPLYFile(
	const FString& FilePath,
	TArray<FGaussianSplatData>& OutSplats,
	FString& OutError,
	int32* OutSHBands,
	int32 MaxSHBandsToRead)
{
	OutSplats.Empty();

	FPLYFileInfo FileInfo;
	if (!ProbePLYFile(FilePath, FileInfo, OutError))
	{
		return false;
	}

	// Open file with IFileHandle for streamed reading (supports files > 2 GB)
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*FilePath));

	if (!FileHandle)
	{
		OutError = FString::Printf(TEXT("Failed to open file: %s"), *FilePath);
		return false;
	}

	const int64 FileSize = FileInfo.FileSize;
	UE_LOG(LogTemp, Log, TEXT("PLY file size: %lld bytes (%.2f GB)"), FileSize, FileSize / (1024.0 * 1024.0 * 1024.0));

	// Parse header again so the file handle is positioned at vertex data for streamed reads.
	FPLYHeader Header;
	if (!ParseHeader(FileHandle.Get(), Header, OutError))
	{
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("PLY Header parsed: %d vertices, %d bytes per vertex, data at offset %lld"),
		Header.VertexCount, Header.VertexStride, Header.DataOffset);

	// Detect SH band count from header properties
	// PLY stores SH in planar format: all R coefficients, then G, then B
	// Band counts and f_rest indices:
	// - 0 bands: no f_rest (DC only in f_dc)
	// - 1 band:  3 coeffs/channel × 3 = 9 total  → f_rest_0..8
	// - 2 bands: 8 coeffs/channel × 3 = 24 total → f_rest_0..23
	// - 3 bands: 15 coeffs/channel × 3 = 45 total → f_rest_0..44
	if (OutSHBands)
	{
		if (Header.PropertyOffsets.Contains(TEXT("f_rest_44")))
		{
			*OutSHBands = 3;  // Has all 45 coefficients (15 per channel)
		}
		else if (Header.PropertyOffsets.Contains(TEXT("f_rest_23")))
		{
			*OutSHBands = 2;  // Has 24 coefficients (8 per channel)
		}
		else if (Header.PropertyOffsets.Contains(TEXT("f_rest_8")))
		{
			*OutSHBands = 1;  // Has 9 coefficients (3 per channel)
		}
		else
		{
			*OutSHBands = 0;  // No f_rest data (DC only)
		}
		UE_LOG(LogTemp, Log, TEXT("PLYFileReader: Detected SH bands = %d"), *OutSHBands);
	}

	// Validate file size against expected data
	const int64 ExpectedEnd = Header.DataOffset + static_cast<int64>(Header.VertexCount) * Header.VertexStride;
	if (ExpectedEnd > FileSize)
	{
		OutError = FString::Printf(TEXT("File truncated: expected %lld bytes of vertex data, file size is %lld"),
			ExpectedEnd, FileSize);
		return false;
	}

	// Read vertex data using streamed I/O
	if (!ReadVertexData(FileHandle.Get(), Header, OutSplats, OutError, MaxSHBandsToRead))
	{
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("Successfully read %d splats from PLY file"), OutSplats.Num());
	return true;
}

bool FPLYFileReader::IsValidPLYFile(const FString& FilePath)
{
	// Quick check: read first few bytes and look for "ply" magic
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*FilePath));

	if (!FileHandle)
	{
		return false;
	}

	uint8 HeaderBytes[4];
	if (!FileHandle->Read(HeaderBytes, 4))
	{
		return false;
	}

	// Check for "ply\n" or "ply\r\n" magic
	return HeaderBytes[0] == 'p' && HeaderBytes[1] == 'l' && HeaderBytes[2] == 'y';
}

bool FPLYFileReader::ParseHeader(IFileHandle* FileHandle, FPLYHeader& OutHeader, FString& OutError)
{
	// PLY headers are ASCII text, typically < 4 KB but we read up to 64 KB to be safe
	constexpr int32 MaxHeaderSize = 65536;
	TArray<uint8> HeaderBuffer;
	HeaderBuffer.SetNumUninitialized(MaxHeaderSize);

	// Read header bytes from the start of the file
	FileHandle->Seek(0);
	const int64 FileSize = FileHandle->Size();
	const int32 BytesToRead = static_cast<int32>(FMath::Min(static_cast<int64>(MaxHeaderSize), FileSize));

	if (!FileHandle->Read(HeaderBuffer.GetData(), BytesToRead))
	{
		OutError = TEXT("Failed to read PLY header bytes");
		return false;
	}

	// Find "end_header" marker
	const char* EndHeaderMarker = "end_header";
	const int32 MarkerLen = FCStringAnsi::Strlen(EndHeaderMarker);
	int32 HeaderEnd = -1;

	for (int32 i = 0; i < BytesToRead - MarkerLen; i++)
	{
		if (FMemory::Memcmp(&HeaderBuffer[i], EndHeaderMarker, MarkerLen) == 0)
		{
			// Find the newline after end_header
			for (int32 j = i + MarkerLen; j < BytesToRead; j++)
			{
				if (HeaderBuffer[j] == '\n')
				{
					HeaderEnd = j + 1;
					break;
				}
			}
			break;
		}
	}

	if (HeaderEnd < 0)
	{
		OutError = TEXT("Could not find 'end_header' in PLY file (or header exceeds 64 KB)");
		return false;
	}

	// Convert header to string
	FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(HeaderBuffer.GetData()), HeaderEnd);
	FString HeaderString(Converter.Length(), Converter.Get());

	OutHeader.DataOffset = HeaderEnd;

	// Parse header lines
	TArray<FString> Lines;
	HeaderString.ParseIntoArrayLines(Lines);

	bool bFoundPly = false;
	bool bInVertexElement = false;
	int32 CurrentOffset = 0;

	for (const FString& Line : Lines)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();

		if (TrimmedLine == TEXT("ply"))
		{
			bFoundPly = true;
			continue;
		}

		if (TrimmedLine.StartsWith(TEXT("format")))
		{
			if (TrimmedLine.Contains(TEXT("binary_little_endian")))
			{
				OutHeader.bBinaryLittleEndian = true;
			}
			else if (TrimmedLine.Contains(TEXT("binary_big_endian")))
			{
				OutHeader.bBinaryLittleEndian = false;
				OutError = TEXT("Big endian PLY files are not supported");
				return false;
			}
			else if (TrimmedLine.Contains(TEXT("ascii")))
			{
				OutError = TEXT("ASCII PLY files are not supported, please use binary format");
				return false;
			}
			continue;
		}

		if (TrimmedLine.StartsWith(TEXT("element vertex")))
		{
			TArray<FString> Parts;
			TrimmedLine.ParseIntoArray(Parts, TEXT(" "));
			if (Parts.Num() >= 3)
			{
				OutHeader.VertexCount = FCString::Atoi(*Parts[2]);
			}
			bInVertexElement = true;
			continue;
		}

		if (TrimmedLine.StartsWith(TEXT("element")))
		{
			bInVertexElement = false;
			continue;
		}

		if (bInVertexElement && TrimmedLine.StartsWith(TEXT("property")))
		{
			TArray<FString> Parts;
			TrimmedLine.ParseIntoArray(Parts, TEXT(" "));

			if (Parts.Num() >= 3)
			{
				FString Type = Parts[1];
				FString Name = Parts[2];

				int32 TypeSize = 0;
				if (Type == TEXT("float") || Type == TEXT("float32"))
				{
					TypeSize = 4;
				}
				else if (Type == TEXT("double") || Type == TEXT("float64"))
				{
					TypeSize = 8;
				}
				else if (Type == TEXT("uchar") || Type == TEXT("uint8"))
				{
					TypeSize = 1;
				}
				else if (Type == TEXT("int") || Type == TEXT("int32"))
				{
					TypeSize = 4;
				}
				else if (Type == TEXT("short") || Type == TEXT("int16"))
				{
					TypeSize = 2;
				}

				OutHeader.PropertyNames.Add(Name);
				OutHeader.PropertyOffsets.Add(Name, CurrentOffset);
				CurrentOffset += TypeSize;
			}
			continue;
		}
	}

	if (!bFoundPly)
	{
		OutError = TEXT("File does not start with 'ply' magic");
		return false;
	}

	if (OutHeader.VertexCount <= 0)
	{
		OutError = TEXT("No vertices found in PLY file");
		return false;
	}

	OutHeader.VertexStride = CurrentOffset;

	// Verify we have required properties
	TArray<FString> RequiredProps = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("opacity"),
		TEXT("scale_0"), TEXT("scale_1"), TEXT("scale_2"),
		TEXT("rot_0"), TEXT("rot_1"), TEXT("rot_2"), TEXT("rot_3") };

	for (const FString& Prop : RequiredProps)
	{
		if (!OutHeader.PropertyOffsets.Contains(Prop))
		{
			OutError = FString::Printf(TEXT("Missing required property: %s"), *Prop);
			return false;
		}
	}

	// Seek file handle to start of vertex data
	FileHandle->Seek(OutHeader.DataOffset);

	return true;
}

bool FPLYFileReader::ReadVertexData(
	IFileHandle* FileHandle,
	const FPLYHeader& Header,
	TArray<FGaussianSplatData>& OutSplats,
	FString& OutError,
	int32 MaxSHBandsToRead)
{
	OutSplats.SetNum(Header.VertexCount);

	// Detect SH coefficients per channel from available properties
	// PLY stores SH in planar format: all R coefficients, then G, then B
	// - 1 band:  3 coeffs/channel (f_rest_0..8)
	// - 2 bands: 8 coeffs/channel (f_rest_0..23)
	// - 3 bands: 15 coeffs/channel (f_rest_0..44)
	int32 SourceCoeffsPerChannel = 0;
	if (Header.PropertyOffsets.Contains(TEXT("f_rest_44")))
	{
		SourceCoeffsPerChannel = 15;  // 3 bands
	}
	else if (Header.PropertyOffsets.Contains(TEXT("f_rest_23")))
	{
		SourceCoeffsPerChannel = 8;   // 2 bands
	}
	else if (Header.PropertyOffsets.Contains(TEXT("f_rest_8")))
	{
		SourceCoeffsPerChannel = 3;   // 1 band
	}

	const int32 MaxCoeffsToRead =
		(MaxSHBandsToRead < 0) ? GaussianSplattingConstants::NumSHCoefficients :
		(MaxSHBandsToRead == 0) ? 0 :
		(MaxSHBandsToRead == 1) ? 3 :
		(MaxSHBandsToRead == 2) ? 8 : GaussianSplattingConstants::NumSHCoefficients;
	const int32 CoeffsToRead = FMath::Min(SourceCoeffsPerChannel, MaxCoeffsToRead);

	UE_LOG(LogTemp, Log, TEXT("PLYFileReader: VertexCount=%d, source SH coefficients/channel=%d, reading=%d"),
		Header.VertexCount, SourceCoeffsPerChannel, CoeffsToRead);

	// Resolve every property name once. The old inner loop built and hashed up to
	// 45 FString property names for every splat, which dominates imports of large
	// captures. Vertex decoding below is now only integer-offset loads.
	const auto FindOffset = [&Header](const TCHAR* PropertyName) -> int32
	{
		const int32* Offset = Header.PropertyOffsets.Find(PropertyName);
		return Offset ? *Offset : INDEX_NONE;
	};
	const auto ReadFloat = [](const uint8* VertexData, int32 Offset, float DefaultValue = 0.0f) -> float
	{
		return Offset == INDEX_NONE
			? DefaultValue
			: *reinterpret_cast<const float*>(VertexData + Offset);
	};

	const int32 XOffset = FindOffset(TEXT("x"));
	const int32 YOffset = FindOffset(TEXT("y"));
	const int32 ZOffset = FindOffset(TEXT("z"));
	const int32 RotationOffsets[4] = {
		FindOffset(TEXT("rot_0")), FindOffset(TEXT("rot_1")),
		FindOffset(TEXT("rot_2")), FindOffset(TEXT("rot_3"))
	};
	const int32 ScaleOffsets[3] = {
		FindOffset(TEXT("scale_0")), FindOffset(TEXT("scale_1")), FindOffset(TEXT("scale_2"))
	};
	const int32 OpacityOffset = FindOffset(TEXT("opacity"));
	const int32 DCOffsets[3] = {
		FindOffset(TEXT("f_dc_0")), FindOffset(TEXT("f_dc_1")), FindOffset(TEXT("f_dc_2"))
	};
	int32 SHOffsets[GaussianSplattingConstants::NumSHCoefficients][3];
	for (int32 CoeffIndex = 0; CoeffIndex < GaussianSplattingConstants::NumSHCoefficients; ++CoeffIndex)
	{
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			SHOffsets[CoeffIndex][Channel] = INDEX_NONE;
		}

		if (CoeffIndex < CoeffsToRead)
		{
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				const int32 SourceIndex = CoeffIndex + Channel * SourceCoeffsPerChannel;
				const FString PropertyName = FString::Printf(TEXT("f_rest_%d"), SourceIndex);
				SHOffsets[CoeffIndex][Channel] = FindOffset(*PropertyName);
			}
		}
	}

	// Read vertices in chunks for efficiency (4096 vertices per chunk)
	constexpr int32 ChunkSize = 4096;
	const int32 VertexStride = Header.VertexStride;
	TArray<uint8> ChunkBuffer;
	ChunkBuffer.SetNumUninitialized(ChunkSize * VertexStride);

	int32 VerticesRemaining = Header.VertexCount;
	int32 VertexIndex = 0;

	while (VerticesRemaining > 0)
	{
		const int32 VerticesToRead = FMath::Min(ChunkSize, VerticesRemaining);
		const int32 BytesToRead = VerticesToRead * VertexStride;

		if (!FileHandle->Read(ChunkBuffer.GetData(), BytesToRead))
		{
			OutError = FString::Printf(TEXT("Failed to read vertex data at vertex %d"), VertexIndex);
			return false;
		}

		for (int32 i = 0; i < VerticesToRead; i++)
		{
			const uint8* VertexData = ChunkBuffer.GetData() + i * VertexStride;
			FGaussianSplatData& Splat = OutSplats[VertexIndex];

			// Position - Preserve the original PLY basis and only convert meters to UE centimeters.
			// Some datasets already store splats in the world basis expected by the user, and
			// hardcoded axis remapping here forces an extra 90-degree compensation on placement.
			constexpr float MetersToUE = 100.0f;
			const float PlyX = ReadFloat(VertexData, XOffset);
			const float PlyY = ReadFloat(VertexData, YOffset);
			const float PlyZ = ReadFloat(VertexData, ZOffset);
			Splat.Position.X = PlyX * MetersToUE;
			Splat.Position.Y = PlyY * MetersToUE;
			Splat.Position.Z = PlyZ * MetersToUE;

			// Rotation (quaternion) - Preserve the original PLY basis.
			// PLY stores (w, x, y, z) and we keep that basis consistent with the stored positions.
			const float QW = ReadFloat(VertexData, RotationOffsets[0]);
			const float QX = ReadFloat(VertexData, RotationOffsets[1]);
			const float QY = ReadFloat(VertexData, RotationOffsets[2]);
			const float QZ = ReadFloat(VertexData, RotationOffsets[3]);
			Splat.Rotation.W = QW;
			Splat.Rotation.X = QX;
			Splat.Rotation.Y = QY;
			Splat.Rotation.Z = QZ;

			// Scale - Preserve the original axis ordering.
			const float ScaleX = ReadFloat(VertexData, ScaleOffsets[0]);
			const float ScaleY = ReadFloat(VertexData, ScaleOffsets[1]);
			const float ScaleZ = ReadFloat(VertexData, ScaleOffsets[2]);
			Splat.Scale.X = ScaleX;
			Splat.Scale.Y = ScaleY;
			Splat.Scale.Z = ScaleZ;

			// Opacity
			Splat.Opacity = ReadFloat(VertexData, OpacityOffset);

			// SH DC (base color)
			Splat.SH_DC.X = ReadFloat(VertexData, DCOffsets[0]);
			Splat.SH_DC.Y = ReadFloat(VertexData, DCOffsets[1]);
			Splat.SH_DC.Z = ReadFloat(VertexData, DCOffsets[2]);

			// SH rest coefficients (bands 1-3)
			// Uses the cached source layout and requested import order from above.
			for (int32 c = 0; c < GaussianSplattingConstants::NumSHCoefficients; c++)
			{
				if (c < CoeffsToRead)
				{
					Splat.SH[c].X = ReadFloat(VertexData, SHOffsets[c][0]);
					Splat.SH[c].Y = ReadFloat(VertexData, SHOffsets[c][1]);
					Splat.SH[c].Z = ReadFloat(VertexData, SHOffsets[c][2]);
				}
				else
				{
					// Zero out coefficients beyond what the file contains
					Splat.SH[c] = FVector3f::ZeroVector;
				}
			}

			// Linearize the data
			LinearizeSplatData(Splat);

			VertexIndex++;
		}

		VerticesRemaining -= VerticesToRead;

		// Log progress for large files
		if (Header.VertexCount > 1000000 && VertexIndex % (Header.VertexCount / 10) < ChunkSize)
		{
			UE_LOG(LogTemp, Log, TEXT("  Reading PLY vertices: %d / %d (%.0f%%)"),
				VertexIndex, Header.VertexCount, 100.0f * VertexIndex / Header.VertexCount);
		}
	}

	return true;
}

void FPLYFileReader::LinearizeSplatData(FGaussianSplatData& Splat)
{
	// Normalize quaternion
	Splat.Rotation = GaussianSplattingUtils::NormalizeQuat(Splat.Rotation);

	// Apply exp to scale (PLY stores log-scale)
	// Then multiply by 100 to convert from meters (PLY) to centimeters (UE)
	constexpr float MetersToUE = 100.0f;
	Splat.Scale.X = FMath::Exp(Splat.Scale.X) * MetersToUE;
	Splat.Scale.Y = FMath::Exp(Splat.Scale.Y) * MetersToUE;
	Splat.Scale.Z = FMath::Exp(Splat.Scale.Z) * MetersToUE;

	// Apply sigmoid to opacity
	Splat.Opacity = GaussianSplattingUtils::Sigmoid(Splat.Opacity);
}
