// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianGlobalAccumulator.h"
#include "GaussianDataTypes.h"
#include "NanoGSRHICompat.h"
#include "RHICommandList.h"
#include "RHIResources.h"

bool FGaussianGlobalAccumulator::ResizeIfNeeded(
	FRHICommandListBase& RHICmdList,
	uint64 NewTotalCount,
	uint32 MaxRenderBudget)
{
	// Apply render budget cap: working buffers only need to hold up to MaxRenderBudget splats,
	// not ALL splats. With Nanite LOD compaction, visible count is typically much smaller.
	const uint64 MaxBudget = static_cast<uint64>(MaxRenderBudget);
	if (MaxBudget > 0 && NewTotalCount > MaxBudget)
	{
		NewTotalCount = MaxBudget;
	}

	if (NewTotalCount <= static_cast<uint64>(AllocatedCount))
	{
		LastRejectedTotalCount = 0;
		return true;
	}

	// UE 5.5 represents FRHIBufferDesc::Size and CreateBuffer's byte size as uint32.
	// Perform all capacity arithmetic in 64 bits and validate every narrowing cast
	// before releasing the currently valid buffer set.
	constexpr uint64 MaxRHIBufferBytes = static_cast<uint64>(MAX_uint32);
	const uint64 UintStride64 = static_cast<uint64>(sizeof(uint32));
	const uint64 ViewDataStride64 = static_cast<uint64>(sizeof(FGaussianSplatViewData));
	static_assert(sizeof(FGaussianSplatViewData) <= MAX_uint32, "View-data stride must fit the RHI's uint32 stride field");

	const uint64 Headroom = NewTotalCount / 5;
	const uint64 DesiredCount = NewTotalCount > MAX_uint64 - Headroom
		? MAX_uint64
		: NewTotalCount + Headroom;
	uint64 NewAllocatedCount64 = FMath::Max<uint64>(DesiredCount, 4096ull);
	if (MaxBudget > 0)
	{
		NewAllocatedCount64 = FMath::Min(NewAllocatedCount64, MaxBudget);
	}

	// ViewData is the widest per-splat allocation today, but validate every
	// buffer independently below so future stride changes cannot reintroduce a wrap.
	const uint64 MaxCountByViewData = MaxRHIBufferBytes / ViewDataStride64;
	if (NewAllocatedCount64 > MaxCountByViewData)
	{
		NewAllocatedCount64 = MaxCountByViewData;
	}

	if (NewAllocatedCount64 < NewTotalCount || NewAllocatedCount64 > static_cast<uint64>(MAX_uint32))
	{
		const uint64 RequiredViewDataBytes = NewTotalCount <= MAX_uint64 / ViewDataStride64
			? NewTotalCount * ViewDataStride64
			: MAX_uint64;
		if (LastRejectedTotalCount != NewTotalCount)
		{
			UE_LOG(LogTemp, Error,
				TEXT("GaussianGlobalAccumulator: Cannot allocate %llu splats. The widest per-splat buffer requires %llu bytes, but UE's RHI buffer size is limited to %llu bytes (maximum %llu splats at %llu bytes each). Use spatial paging/streaming or lower gs.MaxRenderBudget."),
				static_cast<unsigned long long>(NewTotalCount),
				static_cast<unsigned long long>(RequiredViewDataBytes),
				static_cast<unsigned long long>(MaxRHIBufferBytes),
				static_cast<unsigned long long>(MaxCountByViewData),
				static_cast<unsigned long long>(ViewDataStride64));
		}
		LastRejectedTotalCount = NewTotalCount;
		bHasCachedSortData = false;
		return false;
	}

	const uint64 NewNumTiles64 = (NewAllocatedCount64 + 1023ull) / 1024ull;
	const uint64 HistogramCount64 = NewNumTiles64 * 256ull;

	uint32 ViewDataBytes = 0;
	uint32 SortBytes = 0;
	uint32 HistogramBytes = 0;
	const TCHAR* InvalidBufferName = nullptr;
	uint64 InvalidElementCount = 0;
	uint64 InvalidStride = 0;
	uint64 InvalidRequiredBytes = 0;
	const auto TryCalculateRHIBytes = [&InvalidBufferName, &InvalidElementCount, &InvalidStride, &InvalidRequiredBytes](
		const TCHAR* BufferName, uint64 ElementCount, uint64 Stride, uint32& OutBytes)
	{
		if (ElementCount == 0 || Stride == 0 || ElementCount > static_cast<uint64>(MAX_uint32) / Stride)
		{
			InvalidBufferName = BufferName;
			InvalidElementCount = ElementCount;
			InvalidStride = Stride;
			InvalidRequiredBytes = (Stride != 0 && ElementCount <= MAX_uint64 / Stride)
				? ElementCount * Stride
				: MAX_uint64;
			return false;
		}

		OutBytes = static_cast<uint32>(ElementCount * Stride);
		return true;
	};

	if (!TryCalculateRHIBytes(TEXT("GlobalViewDataBuffer"), NewAllocatedCount64, ViewDataStride64, ViewDataBytes) ||
		!TryCalculateRHIBytes(TEXT("GlobalSortBuffers"), NewAllocatedCount64, UintStride64, SortBytes) ||
		!TryCalculateRHIBytes(TEXT("GlobalRadixHistogramBuffer"), HistogramCount64, UintStride64, HistogramBytes) ||
		NewNumTiles64 > static_cast<uint64>(MAX_uint32))
	{
		if (LastRejectedTotalCount != NewTotalCount)
		{
			UE_LOG(LogTemp, Error,
				TEXT("GaussianGlobalAccumulator: Refusing unsafe RHI allocation for %llu splats: %s needs %llu elements x %llu bytes = %llu bytes; the UE RHI byte-size limit is %llu."),
				static_cast<unsigned long long>(NewTotalCount),
				InvalidBufferName ? InvalidBufferName : TEXT("radix tile metadata"),
				static_cast<unsigned long long>(InvalidElementCount),
				static_cast<unsigned long long>(InvalidStride),
				static_cast<unsigned long long>(InvalidRequiredBytes),
				static_cast<unsigned long long>(MaxRHIBufferBytes));
		}
		LastRejectedTotalCount = NewTotalCount;
		bHasCachedSortData = false;
		return false;
	}

	const uint32 NewAllocatedCount = static_cast<uint32>(NewAllocatedCount64);
	const uint32 NewNumTiles = static_cast<uint32>(NewNumTiles64);
	const uint32 UintStride = static_cast<uint32>(UintStride64);
	const uint32 ViewDataStride = static_cast<uint32>(ViewDataStride64);
	const uint64 TotalWorkingBytes = static_cast<uint64>(ViewDataBytes) +
		4ull * static_cast<uint64>(SortBytes) + static_cast<uint64>(HistogramBytes) +
		256ull * UintStride64 + 2ull * UintStride64;

	UE_LOG(LogTemp, Verbose,
		TEXT("GaussianGlobalAccumulator: Resizing from %u to %u splats (%u tiles, %.2f MiB working buffers)"),
		AllocatedCount, NewAllocatedCount, NewNumTiles,
		static_cast<double>(TotalWorkingBytes) / (1024.0 * 1024.0));

	// Release old buffers before reallocation
	Release();

	// --- ViewData buffer ---
	{
		GlobalViewDataBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalViewDataBuffer"),
			ViewDataBytes,
			ViewDataStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalViewDataBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalViewDataBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(ViewDataStride));
		GlobalViewDataBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalViewDataBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(ViewDataStride));
	}

	// --- Sort distance buffer (primary) ---
	{
		GlobalSortDistanceBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalSortDistanceBuffer"),
			SortBytes,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalSortDistanceBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortDistanceBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Sort distance buffer (alt) ---
	{
		GlobalSortDistanceBufferAlt = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalSortDistanceBufferAlt"),
			SortBytes,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalSortDistanceBufferAltUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortDistanceBufferAlt, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Sort keys buffer (primary) ---
	{
		GlobalSortKeysBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalSortKeysBuffer"),
			SortBytes,
			UintStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalSortKeysBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortKeysBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
		GlobalSortKeysBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalSortKeysBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Sort keys buffer (alt) ---
	{
		GlobalSortKeysBufferAlt = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalSortKeysBufferAlt"),
			SortBytes,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalSortKeysBufferAltUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortKeysBufferAlt, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Radix histogram: 256 * NumTiles ---
	{
		GlobalRadixHistogramBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalRadixHistogramBuffer"),
			HistogramBytes,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalRadixHistogramBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalRadixHistogramBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Radix digit offset: 256 entries ---
	{
		GlobalRadixDigitOffsetBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalRadixDigitOffsetBuffer"),
			256 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalRadixDigitOffsetBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalRadixDigitOffsetBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- SortParams: 2 uints — SRV for non-compaction sort, UAV written by PrefixSumCS ---
	{
		GlobalSortParamsBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalSortParamsBuffer"),
			2 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalSortParamsBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalSortParamsBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
		GlobalSortParamsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortParamsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	AllocatedCount = NewAllocatedCount;
	AllocatedNumTiles = NewNumTiles;
	LastRejectedTotalCount = 0;

	// Invalidate cache since buffers changed
	bHasCachedSortData = false;
	return true;
}

void FGaussianGlobalAccumulator::EnsureCompactionBuffersAllocated(FRHICommandListBase& RHICmdList)
{
	if (bCompactionBuffersAllocated)
	{
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("GaussianGlobalAccumulator: Allocating compaction prefix-sum buffers (MaxProxies=%u)"), MAX_PROXY_COUNT);

	const uint32 UintStride = sizeof(uint32);

	// --- GlobalVisibleCountArray: [MAX_PROXY_COUNT] uints ---
	{
		GlobalVisibleCountArrayBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalVisibleCountArrayBuffer"),
			MAX_PROXY_COUNT * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalVisibleCountArrayBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalVisibleCountArrayBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
		GlobalVisibleCountArrayBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalVisibleCountArrayBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- GlobalBaseOffsets: [MAX_PROXY_COUNT + 1] uints ---
	{
		GlobalBaseOffsetsBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalBaseOffsetsBuffer"),
			(MAX_PROXY_COUNT + 1) * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalBaseOffsetsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalBaseOffsetsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
		GlobalBaseOffsetsBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalBaseOffsetsBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- GlobalCalcDistIndirectArgs: [3] uints ---
	{
		GlobalCalcDistIndirectArgsBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalCalcDistIndirectArgsBuffer"),
			3 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalCalcDistIndirectArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalCalcDistIndirectArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- GlobalSortIndirectArgsGlobal: [3] uints ---
	{
		GlobalSortIndirectArgsGlobalBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalSortIndirectArgsGlobalBuffer"),
			3 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalSortIndirectArgsGlobalBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortIndirectArgsGlobalBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- GlobalDrawIndirectArgs: [5] uints ---
	{
		GlobalDrawIndirectArgsBuffer = NanoGS::RHICompat::CreateBuffer(
			RHICmdList,
			TEXT("GlobalDrawIndirectArgsBuffer"),
			5 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer,
			ERHIAccess::UAVCompute);
		GlobalDrawIndirectArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalDrawIndirectArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	bCompactionBuffersAllocated = true;
}

void FGaussianGlobalAccumulator::Release()
{
	GlobalViewDataBuffer.SafeRelease();
	GlobalViewDataBufferUAV.SafeRelease();
	GlobalViewDataBufferSRV.SafeRelease();

	GlobalSortDistanceBuffer.SafeRelease();
	GlobalSortDistanceBufferUAV.SafeRelease();
	GlobalSortDistanceBufferAlt.SafeRelease();
	GlobalSortDistanceBufferAltUAV.SafeRelease();

	GlobalSortKeysBuffer.SafeRelease();
	GlobalSortKeysBufferUAV.SafeRelease();
	GlobalSortKeysBufferSRV.SafeRelease();
	GlobalSortKeysBufferAlt.SafeRelease();
	GlobalSortKeysBufferAltUAV.SafeRelease();
	GlobalPublishedSortKeysBuffer.SafeRelease();
	GlobalPublishedSortKeysBufferSRV.SafeRelease();
	bHasPublishedSortKeys = false;
	bUsePublishedSortKeysForDraw = false;

	GlobalRadixHistogramBuffer.SafeRelease();
	GlobalRadixHistogramBufferUAV.SafeRelease();

	GlobalRadixDigitOffsetBuffer.SafeRelease();
	GlobalRadixDigitOffsetBufferUAV.SafeRelease();

	GlobalSortParamsBuffer.SafeRelease();
	GlobalSortParamsBufferSRV.SafeRelease();
	GlobalSortParamsBufferUAV.SafeRelease();

	// Compaction prefix-sum buffers
	GlobalVisibleCountArrayBuffer.SafeRelease();
	GlobalVisibleCountArrayBufferUAV.SafeRelease();
	GlobalVisibleCountArrayBufferSRV.SafeRelease();

	GlobalBaseOffsetsBuffer.SafeRelease();
	GlobalBaseOffsetsBufferUAV.SafeRelease();
	GlobalBaseOffsetsBufferSRV.SafeRelease();

	GlobalCalcDistIndirectArgsBuffer.SafeRelease();
	GlobalCalcDistIndirectArgsBufferUAV.SafeRelease();

	GlobalSortIndirectArgsGlobalBuffer.SafeRelease();
	GlobalSortIndirectArgsGlobalBufferUAV.SafeRelease();

	GlobalDrawIndirectArgsBuffer.SafeRelease();
	GlobalDrawIndirectArgsBufferUAV.SafeRelease();

	AllocatedCount = 0;
	AllocatedNumTiles = 0;
	bHasCachedSortData = false;
	bHasLastObservedViewProjection = false;
	InteractiveViewChangeSequence = 0;
	LastInteractiveViewChangeSeconds = 0.0;
	bPublishedSortPendingExact = false;
	bPassTimingOneShotConsumed = false;
	bCompactionBuffersAllocated = false;
}
