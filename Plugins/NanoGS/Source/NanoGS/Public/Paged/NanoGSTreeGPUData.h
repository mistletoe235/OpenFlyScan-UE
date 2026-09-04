// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Paged/NanoGSTreeFormat.h"

namespace NanoGS::Tree
{
	inline constexpr uint32 GPUNodeFlagInternal = 1u << 16u;
	inline constexpr uint32 GPUNodeFlagExactLeaf = 1u << 17u;

	struct alignas(16) NANOGS_API FGPUNodeRecord
	{
		FVector4f CenterAndSupportRadius = FVector4f::Zero();
		uint32 FirstChild = 0;
		uint32 ChildCountAndFlags = 0;
		uint32 FeatureRadiusBits = 0;
		uint32 PackedPageAddress = 0;

		uint32 GetChildCount() const { return ChildCountAndFlags & 0xffffu; }
		bool IsInternal() const { return (ChildCountAndFlags & GPUNodeFlagInternal) != 0; }
		bool IsExactLeaf() const { return (ChildCountAndFlags & GPUNodeFlagExactLeaf) != 0; }
		float GetFeatureRadiusCm() const
		{
			float Value = 0.0f;
			FMemory::Memcpy(&Value, &FeatureRadiusBits, sizeof(Value));
			return Value;
		}
		uint32 GetPageId() const { return PackedPageAddress >> 16u; }
		uint32 GetPageLocal() const { return PackedPageAddress & 0xffffu; }
	};
	static_assert(sizeof(FGPUNodeRecord) == 32, "GPU tree node layout must remain shader-compatible");

	NANOGS_API bool BuildGPUNodeTable(
		const FManifest& Manifest,
		TArray<FGPUNodeRecord>& OutNodes,
		FString& OutError);
}
