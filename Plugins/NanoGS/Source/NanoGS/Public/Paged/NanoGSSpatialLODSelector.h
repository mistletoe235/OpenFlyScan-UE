// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Paged/NanoGSSpatialLODFormat.h"

namespace NanoGS::SpatialLOD
{
	struct NANOGS_API FNodeObservation
	{
		uint32 NodeIndex = MAX_uint32;
		float ProjectedRadiusPx = 0.0f;
		bool bVisible = false;
	};

	struct NANOGS_API FViewObservations
	{
		TArray<FNodeObservation> Nodes;
	};

	struct NANOGS_API FNodeSelection
	{
		uint32 NodeIndex = MAX_uint32;
		uint32 Level = 0;
		uint32 CertifiedLevel = 0;
	};

	struct NANOGS_API FSoftBudgetResult
	{
		uint64 OriginalSplatCount = 0;
		uint64 SelectedSplatCount = 0;
		uint64 CertifiedMinimumSplatCount = 0;
		uint32 CoarseningStepCount = 0;
		bool bBudgetExceeded = false;
	};

	struct NANOGS_API FSelection
	{
		TArray<FNodeSelection> Nodes;
	};

	class NANOGS_API FSelector
	{
	public:
		FSelection Select(const FManifest& Manifest, TConstArrayView<FViewObservations> Views);
		static FSoftBudgetResult ApplySoftBudget(
			const FManifest& Manifest,
			const TMap<uint32, uint32>& CertifiedLevels,
			uint64 MaxActiveSplats,
			TMap<uint32, uint32>& InOutLevels);
		void Reset();

	private:
		uint32 SelectRequiredLevel(const FManifest& Manifest, uint32 NodeIndex, float ProjectedRadiusPx) const;
		TMap<uint32, uint32> ActiveLevels;
	};
}
