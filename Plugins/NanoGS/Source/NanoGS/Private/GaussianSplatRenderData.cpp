// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatRenderData.h"
#include "GaussianSplatAsset.h"
#include "NanoGSRHICompat.h"
#include "RHICommandList.h"

FGaussianSplatRenderData::FGaussianSplatRenderData()
{
}

FGaussianSplatRenderData::~FGaussianSplatRenderData()
{
	ReleaseGPUBuffers();
}

void FGaussianSplatRenderData::Initialize(UGaussianSplatAsset* Asset)
{
	FScopeLock Lock(&InitLock);

	if (bIsInitialized)
	{
		return;
	}

	if (!Asset || !Asset->IsValid())
	{
		return;
	}

	AssetName = Asset->GetName();
	SplatCount = Asset->GetSplatCount();
	PositionFormat = Asset->PositionFormat;

	// Packed positions use float16 centimeters, whose largest finite value is
	// 65504. Keep the packed values near zero while preserving the asset's
	// original local-space coordinates for bounds, clusters and serialization.
	const FBox AssetBounds = Asset->GetBounds();
	if (AssetBounds.IsValid)
	{
		PackedPositionOrigin = FVector3f(AssetBounds.GetCenter());

		const FVector BoundsExtent = AssetBounds.GetExtent();
		const double MaxPackedMagnitude = FMath::Max3(BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z);
		if (MaxPackedMagnitude > 65504.0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("GaussianSplatRenderData: Asset '%s' has a centered extent %.3f cm larger than the float16 packed-position range; spatial partitioning is required to avoid overflow."),
				*AssetName, MaxPackedMagnitude);
		}
	}

	// --- Pack splat data (16 bytes/splat) ---
	{
		TArray<uint8> RawPositionData;
		TArray<uint8> RawOtherData;
		TArray<uint8> RawColorTextureData;
		Asset->GetPositionData(RawPositionData);
		Asset->GetOtherData(RawOtherData);
		Asset->GetColorTextureData(RawColorTextureData);

		const int64 ExpectedFloat32PositionBytes = static_cast<int64>(SplatCount) * 3 * sizeof(float);
		bHasFloat32PositionData =
			PositionFormat == EGaussianPositionFormat::Float32 &&
			RawPositionData.Num() == ExpectedFloat32PositionBytes;
		if (!bHasFloat32PositionData)
		{
			// NanoGS assets are currently authored from Float32 source geometry. The
			// packed GPU representation is rebuilt from that payload below, so an old
			// format or truncated bulk block cannot safely fall back here.
			UE_LOG(LogTemp, Error,
				TEXT("GaussianSplatRenderData: Rejecting asset '%s': invalid Float32 position payload (%d bytes, expected %lld). Reimport the asset or use Page v1 streaming."),
				*AssetName, RawPositionData.Num(), ExpectedFloat32PositionBytes);
			return;
		}

		const int64 ExpectedFloat32RotationScaleBytes = static_cast<int64>(SplatCount) * 7 * sizeof(float);
		bHasFloat32RotationScaleData = RawOtherData.Num() == ExpectedFloat32RotationScaleBytes;
		if (!bHasFloat32RotationScaleData)
		{
			UE_LOG(LogTemp, Error,
				TEXT("GaussianSplatRenderData: Rejecting asset '%s': invalid Float32 rotation/scale payload (%d bytes, expected %lld). Reimport the asset or use Page v1 streaming."),
				*AssetName, RawOtherData.Num(), ExpectedFloat32RotationScaleBytes);
			return;
		}

		// Exact-HQ assets already carry all geometry in Float32. When no generated
		// LOD splats depend on the legacy packed fallback, retain only its exact
		// RGBA8/opacity word instead of redundantly uploading another 12 bytes of
		// quantized geometry per splat.
		const bool bHasHierarchy = Asset->HasClusterHierarchy();
		const bool bHasGeneratedLODSplats = bHasHierarchy && Asset->GetGeneratedLODSplatCount() > 0;
		const bool bHierarchyMatchesSource = !bHasHierarchy ||
			(!bHasGeneratedLODSplats &&
			 Asset->GetClusterHierarchy().TotalSplatCount == static_cast<uint32>(SplatCount));
		if (bHasHierarchy && !bHierarchyMatchesSource && !bHasGeneratedLODSplats)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("GaussianSplatRenderData: Asset '%s' has a stale lossless hierarchy (%u hierarchy splats, %d asset splats); retaining packed geometry fallback."),
				*AssetName, Asset->GetClusterHierarchy().TotalSplatCount, SplatCount);
		}
		bUsesDedicatedColorOpacityBuffer =
			bHasFloat32PositionData && bHasFloat32RotationScaleData && bHierarchyMatchesSource;
		bHasPackedGeometryData = !bUsesDedicatedColorOpacityBuffer;

		const int32 ColorTexWidth = Asset->ColorTextureWidth;
		const int32 ColorTexHeight = Asset->ColorTextureHeight;
		const FFloat16Color* ColorPixels = nullptr;
		int64 ExpectedColorBytes = 0;
		if (ColorTexWidth > 0 && ColorTexHeight > 0)
		{
			const int64 PixelCount = static_cast<int64>(ColorTexWidth) * static_cast<int64>(ColorTexHeight);
			if (PixelCount <= TNumericLimits<int64>::Max() / static_cast<int64>(sizeof(FFloat16Color)))
			{
				ExpectedColorBytes = PixelCount * static_cast<int64>(sizeof(FFloat16Color));
			}
		}
		const bool bHasColor = ExpectedColorBytes > 0 &&
			static_cast<int64>(RawColorTextureData.Num()) >= ExpectedColorBytes;
		if (bHasColor)
		{
			ColorPixels = reinterpret_cast<const FFloat16Color*>(RawColorTextureData.GetData());
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("GaussianSplatRenderData: Asset '%s' has an invalid color payload (%d bytes, expected at least %lld for %dx%d); using opaque white fallback."),
				*AssetName, RawColorTextureData.Num(), ExpectedColorBytes, ColorTexWidth, ColorTexHeight);
		}

		uint32* PackedPtr = nullptr;
		uint32* ColorOpacityPtr = nullptr;
		if (bUsesDedicatedColorOpacityBuffer)
		{
			PackedColorOpacityData.SetNumUninitialized(SplatCount * sizeof(uint32));
			ColorOpacityPtr = reinterpret_cast<uint32*>(PackedColorOpacityData.GetData());
		}
		else
		{
			PackedSplatData.SetNumUninitialized(SplatCount * GaussianSplattingConstants::PackedSplatStride);
			PackedPtr = reinterpret_cast<uint32*>(PackedSplatData.GetData());
		}
		const float* PosFloats = reinterpret_cast<const float*>(RawPositionData.GetData());
		const float* OtherFloats = reinterpret_cast<const float*>(RawOtherData.GetData());

		for (int32 i = 0; i < SplatCount; i++)
		{
			FVector3f Position(
				PosFloats[i * 3 + 0],
				PosFloats[i * 3 + 1],
				PosFloats[i * 3 + 2]);

			const float* OtherBase = OtherFloats + i * 7;
			FQuat4f Rotation(OtherBase[0], OtherBase[1], OtherBase[2], OtherBase[3]);
			FVector3f Scale(OtherBase[4], OtherBase[5], OtherBase[6]);

			float ColorR = 1.0f, ColorG = 1.0f, ColorB = 1.0f, Opacity = 1.0f;
			if (bHasColor)
			{
				int32 TexX, TexY;
				GaussianSplattingUtils::SplatIndexToTextureCoord(i, ColorTexWidth, TexX, TexY);
				if (TexY < ColorTexHeight)
				{
					const FFloat16Color& Pixel = ColorPixels[TexY * ColorTexWidth + TexX];
					ColorR = FMath::Clamp(Pixel.R.GetFloat(), 0.0f, 1.0f);
					ColorG = FMath::Clamp(Pixel.G.GetFloat(), 0.0f, 1.0f);
					ColorB = FMath::Clamp(Pixel.B.GetFloat(), 0.0f, 1.0f);
					Opacity = FMath::Clamp(Pixel.A.GetFloat(), 0.0f, 1.0f);
				}
			}

			if (bUsesDedicatedColorOpacityBuffer)
			{
				ColorOpacityPtr[i] = GaussianSplattingUtils::PackColorOpacityToUint32(
					ColorR, ColorG, ColorB, Opacity);
			}
			else
			{
				GaussianSplattingUtils::PackSplatToUint4(
					Position - PackedPositionOrigin, Rotation, Scale,
					ColorR, ColorG, ColorB, Opacity,
					&PackedPtr[i * 4]);
			}
		}

		// Keep the asset's original Float32 positions until the shared GPU upload.
		// Unlike PackedSplatData, these coordinates are not quantized or recentered.
		if (bHasFloat32PositionData)
		{
			PositionData = MoveTemp(RawPositionData);
		}

		// Preserve the full-precision quaternion and scale payload for the optional
		// high-quality GPU path. PackedSplatData remains the low-bandwidth fallback.
		if (bHasFloat32RotationScaleData)
		{
			OtherData = MoveTemp(RawOtherData);
		}
	}

	// Load chunk data
	CachedChunkData = Asset->ChunkData;

	// Load SH data
	if (Asset->SHBands > 0)
	{
		Asset->GetSHData(SHData);
		SHBands = Asset->SHBands;
	}

	// Load Nanite cluster data
	bEnableNanite = Asset->IsNaniteEnabled();

	if (bEnableNanite && Asset->HasClusterHierarchy())
	{
		const FGaussianClusterHierarchy& Hierarchy = Asset->GetClusterHierarchy();
		Hierarchy.ToGPUClusters(CachedClusterData);
		ClusterCount = CachedClusterData.Num();
		LeafClusterCount = Hierarchy.NumLeafClusters;
		bHasClusterData = true;

		const uint32 OriginalSplatCount = Hierarchy.TotalSplatCount;
		LODSplatCount = Hierarchy.TotalLODSplatCount;
		bHasLODSplats = (LODSplatCount > 0);

		// Build splat-to-cluster index mapping
		CachedSplatClusterIndices.SetNumZeroed(SplatCount);

		for (int32 ClusterIdx = 0; ClusterIdx < Hierarchy.Clusters.Num(); ++ClusterIdx)
		{
			const FGaussianCluster& Cluster = Hierarchy.Clusters[ClusterIdx];
			if (Cluster.IsLeaf())
			{
				for (uint32 i = 0; i < Cluster.SplatCount; ++i)
				{
					uint32 SplatIdx = Cluster.SplatStartIndex + i;
					if (SplatIdx < OriginalSplatCount)
					{
						CachedSplatClusterIndices[SplatIdx] = ClusterIdx;
					}
				}
			}
		}

		for (int32 ClusterIdx = 0; ClusterIdx < Hierarchy.Clusters.Num(); ++ClusterIdx)
		{
			const FGaussianCluster& Cluster = Hierarchy.Clusters[ClusterIdx];
			if (!Cluster.IsLeaf() && Cluster.LODSplatCount > 0)
			{
				for (uint32 i = 0; i < Cluster.LODSplatCount; ++i)
				{
					uint32 SplatIdx = Cluster.LODSplatStartIndex + i;
					if (SplatIdx < static_cast<uint32>(SplatCount))
					{
						CachedSplatClusterIndices[SplatIdx] = ClusterIdx;
					}
				}
			}
		}
	}
	else
	{
		bHasClusterData = false;
		ClusterCount = 0;
		LeafClusterCount = 0;
		bHasLODSplats = false;
		LODSplatCount = 0;
	}

	bIsInitialized = true;

	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatRenderData: Created shared CPU data for asset '%s' (%d splats)"),
		*AssetName, SplatCount);
}

void FGaussianSplatRenderData::CreateGPUBuffers(FRHICommandListBase& RHICmdList)
{
	FScopeLock Lock(&GPUInitLock);

	if (bGPUBuffersCreated)
	{
		return;
	}

	int32 SharedBufferCount = 0;

	// --- Legacy packed splat buffer (or one-splat dummy for the HQ layout) ---
	{
		const uint32 PackedDataSize = PackedSplatData.Num() > 0
			? static_cast<uint32>(PackedSplatData.Num())
			: static_cast<uint32>(GaussianSplattingConstants::PackedSplatStride);
		PackedSplatBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianPackedSplatBuffer"),
			PackedDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer,
			ERHIAccess::SRVMask);

		void* Data = RHICmdList.LockBuffer(PackedSplatBuffer, 0, PackedDataSize, RLM_WriteOnly);
		if (PackedSplatData.Num() > 0)
		{
			FMemory::Memcpy(Data, PackedSplatData.GetData(), PackedSplatData.Num());
		}
		else
		{
			FMemory::Memzero(Data, PackedDataSize);
		}
		RHICmdList.UnlockBuffer(PackedSplatBuffer);

		PackedSplatBufferSRV = RHICmdList.CreateShaderResourceView(
			PackedSplatBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- Dedicated exact RGBA8/opacity stream (or one-word dummy) ---
	{
		const uint32 ColorOpacityDataSize = PackedColorOpacityData.Num() > 0
			? static_cast<uint32>(PackedColorOpacityData.Num())
			: static_cast<uint32>(sizeof(uint32));
		ColorOpacityBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianColorOpacityBuffer"),
			ColorOpacityDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer,
			ERHIAccess::SRVMask);

		void* Data = RHICmdList.LockBuffer(ColorOpacityBuffer, 0, ColorOpacityDataSize, RLM_WriteOnly);
		if (PackedColorOpacityData.Num() > 0)
		{
			FMemory::Memcpy(Data, PackedColorOpacityData.GetData(), PackedColorOpacityData.Num());
		}
		else
		{
			FMemory::Memzero(Data, ColorOpacityDataSize);
		}
		RHICmdList.UnlockBuffer(ColorOpacityBuffer);

		ColorOpacityBufferSRV = RHICmdList.CreateShaderResourceView(
			ColorOpacityBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		if (bUsesDedicatedColorOpacityBuffer)
		{
			UE_LOG(LogTemp, Log,
				TEXT("GaussianSplatRenderData: Uploaded dedicated HQ RGBA8/opacity buffer for '%s': %u bytes (4 bytes/splat; omitted %llu redundant packed-geometry bytes)"),
				*AssetName,
				ColorOpacityDataSize,
				static_cast<unsigned long long>(static_cast<uint64>(SplatCount) * 12ull));
		}
		SharedBufferCount++;
	}

	// --- Original Float32 position buffer ---
	// Always provide a valid SRV because the shader parameter is mandatory. Assets
	// without Float32 positions receive a tiny dummy buffer and use the packed path.
	{
		const uint32 PositionDataSize = PositionData.Num() > 0
			? static_cast<uint32>(PositionData.Num())
			: 12u;

		PositionBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianFloat32PositionBuffer"),
			PositionDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer,
			ERHIAccess::SRVMask);

		void* Data = RHICmdList.LockBuffer(PositionBuffer, 0, PositionDataSize, RLM_WriteOnly);
		if (PositionData.Num() > 0)
		{
			FMemory::Memcpy(Data, PositionData.GetData(), PositionData.Num());
		}
		else
		{
			FMemory::Memzero(Data, PositionDataSize);
		}
		RHICmdList.UnlockBuffer(PositionBuffer);

		PositionBufferSRV = RHICmdList.CreateShaderResourceView(
			PositionBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		if (bHasFloat32PositionData)
		{
			UE_LOG(LogTemp, Log,
				TEXT("GaussianSplatRenderData: Uploaded shared Float32 position buffer for '%s': %u bytes (%.1f MiB, 12 bytes/splat)"),
				*AssetName, PositionDataSize,
				static_cast<double>(PositionDataSize) / (1024.0 * 1024.0));
		}
		SharedBufferCount++;
	}

	// --- Original Float32 rotation + scale buffer ---
	// Always provide a valid SRV because the shader parameter is mandatory. Assets
	// without a complete 28-byte payload receive a one-splat dummy buffer and use
	// the packed rotation/scale path.
	{
		const uint32 OtherDataSize = OtherData.Num() > 0
			? static_cast<uint32>(OtherData.Num())
			: 28u;

		OtherDataBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianFloat32RotationScaleBuffer"),
			OtherDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer,
			ERHIAccess::SRVMask);

		void* Data = RHICmdList.LockBuffer(OtherDataBuffer, 0, OtherDataSize, RLM_WriteOnly);
		if (OtherData.Num() > 0)
		{
			FMemory::Memcpy(Data, OtherData.GetData(), OtherData.Num());
		}
		else
		{
			FMemory::Memzero(Data, OtherDataSize);
		}
		RHICmdList.UnlockBuffer(OtherDataBuffer);

		OtherDataBufferSRV = RHICmdList.CreateShaderResourceView(
			OtherDataBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		if (bHasFloat32RotationScaleData)
		{
			UE_LOG(LogTemp, Log,
				TEXT("GaussianSplatRenderData: Uploaded shared Float32 rotation/scale buffer for '%s': %u bytes (%.1f MiB, 28 bytes/splat)"),
				*AssetName, OtherDataSize,
				static_cast<double>(OtherDataSize) / (1024.0 * 1024.0));
		}
		SharedBufferCount++;
	}

	// --- SH buffer (always create at least a dummy for shader binding) ---
	{
		uint32 SHDataSize = SHData.Num();
		if (SHDataSize == 0)
		{
			SHDataSize = 16;
		}

		SHBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianSHBuffer"),
			SHDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer,
			ERHIAccess::SRVMask);

		void* Data = RHICmdList.LockBuffer(SHBuffer, 0, SHDataSize, RLM_WriteOnly);
		if (SHData.Num() > 0)
		{
			FMemory::Memcpy(Data, SHData.GetData(), SHData.Num());
		}
		else
		{
			FMemory::Memzero(Data, SHDataSize);
		}
		RHICmdList.UnlockBuffer(SHBuffer);

		SHBufferSRV = RHICmdList.CreateShaderResourceView(
			SHBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- Chunk buffer (always create at least a dummy for shader binding) ---
	{
		uint32 ChunkCount = CachedChunkData.Num();
		if (ChunkCount == 0)
		{
			ChunkCount = 1;
		}

		const uint32 ChunkSize = ChunkCount * sizeof(FGaussianChunkInfo);
		ChunkBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianChunkBuffer"),
			ChunkSize,
			sizeof(FGaussianChunkInfo),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::SRVMask);

		void* Data = RHICmdList.LockBuffer(ChunkBuffer, 0, ChunkSize, RLM_WriteOnly);
		if (CachedChunkData.Num() > 0)
		{
			FMemory::Memcpy(Data, CachedChunkData.GetData(), ChunkSize);
		}
		else
		{
			FMemory::Memzero(Data, ChunkSize);
		}
		RHICmdList.UnlockBuffer(ChunkBuffer);

		ChunkBufferSRV = RHICmdList.CreateShaderResourceView(
			ChunkBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FGaussianChunkInfo)));
		SharedBufferCount++;
	}

	// --- Index buffer (6 indices per quad) ---
	{
		TArray<uint16> Indices = { 0, 1, 2, 1, 3, 2 };

		IndexBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianSplatIndexBuffer"),
			Indices.Num() * sizeof(uint16),
			sizeof(uint16),
			BUF_Static | BUF_IndexBuffer,
			ERHIAccess::VertexOrIndexBuffer);

		void* Data = RHICmdList.LockBuffer(IndexBuffer, 0, Indices.Num() * sizeof(uint16), RLM_WriteOnly);
		FMemory::Memcpy(Data, Indices.GetData(), Indices.Num() * sizeof(uint16));
		RHICmdList.UnlockBuffer(IndexBuffer);
		SharedBufferCount++;
	}

	// --- Cluster buffer (static, from asset) ---
	if (bHasClusterData && CachedClusterData.Num() > 0)
	{
		const uint32 BufferSize = CachedClusterData.Num() * sizeof(FGaussianGPUCluster);
		ClusterBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianClusterBuffer"),
			BufferSize,
			sizeof(FGaussianGPUCluster),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::SRVMask);

		void* Data = RHICmdList.LockBuffer(ClusterBuffer, 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, CachedClusterData.GetData(), BufferSize);
		RHICmdList.UnlockBuffer(ClusterBuffer);

		ClusterBufferSRV = RHICmdList.CreateShaderResourceView(
			ClusterBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FGaussianGPUCluster)));
		SharedBufferCount++;
	}

	// --- Splat-to-cluster index buffer (static, from asset; or dummy for non-Nanite) ---
	{
		const uint32 BufferSize = CachedSplatClusterIndices.Num() > 0
			? CachedSplatClusterIndices.Num() * sizeof(uint32)
			: sizeof(uint32);  // dummy 1-element buffer for shader binding

		SplatClusterIndexBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GaussianSplatClusterIndexBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::SRVMask);

		void* Data = RHICmdList.LockBuffer(SplatClusterIndexBuffer, 0, BufferSize, RLM_WriteOnly);
		if (CachedSplatClusterIndices.Num() > 0)
		{
			FMemory::Memcpy(Data, CachedSplatClusterIndices.GetData(), BufferSize);
		}
		else
		{
			FMemory::Memzero(Data, BufferSize);
		}
		RHICmdList.UnlockBuffer(SplatClusterIndexBuffer);

		SplatClusterIndexBufferSRV = RHICmdList.CreateShaderResourceView(
			SplatClusterIndexBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SharedBufferCount++;
	}

	// Free CPU-side cached data after GPU upload
	PackedSplatData.Empty();
	PackedColorOpacityData.Empty();
	PositionData.Empty();
	OtherData.Empty();
	SHData.Empty();
	CachedChunkData.Empty();
	CachedClusterData.Empty();
	CachedSplatClusterIndices.Empty();

	bGPUBuffersCreated = true;

	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatRenderData: Created %d shared GPU buffers for asset '%s'"),
		SharedBufferCount, *AssetName);
}

void FGaussianSplatRenderData::ReleaseGPUBuffers()
{
	PackedSplatBuffer.SafeRelease();
	PackedSplatBufferSRV.SafeRelease();
	ColorOpacityBuffer.SafeRelease();
	ColorOpacityBufferSRV.SafeRelease();
	PositionBuffer.SafeRelease();
	PositionBufferSRV.SafeRelease();
	OtherDataBuffer.SafeRelease();
	OtherDataBufferSRV.SafeRelease();
	SHBuffer.SafeRelease();
	SHBufferSRV.SafeRelease();
	ChunkBuffer.SafeRelease();
	ChunkBufferSRV.SafeRelease();
	IndexBuffer.SafeRelease();
	ClusterBuffer.SafeRelease();
	ClusterBufferSRV.SafeRelease();
	SplatClusterIndexBuffer.SafeRelease();
	SplatClusterIndexBufferSRV.SafeRelease();
	bGPUBuffersCreated = false;
}
