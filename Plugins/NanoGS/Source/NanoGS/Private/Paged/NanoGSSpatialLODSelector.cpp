// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSSpatialLODSelector.h"

namespace NanoGS::SpatialLOD
{
uint32 FSelector::SelectRequiredLevel(const FManifest& Manifest, uint32 NodeIndex, float ProjectedRadiusPx) const
{
	if (!FMath::IsFinite(ProjectedRadiusPx) || ProjectedRadiusPx < 0.0f ||
		!Manifest.Nodes.IsValidIndex(static_cast<int32>(NodeIndex)) ||
		Manifest.Nodes[NodeIndex].IsEnvironment())
	{
		return 0;
	}

	for (uint32 Level = Manifest.LevelCount; Level-- > 1;)
	{
		const FNodeLodError* Error = Manifest.FindError(NodeIndex, Level);
		const FNodeLodRange* Range = Manifest.FindRange(NodeIndex, Level);
		if (Error != nullptr && Error->IsCalibrated() && Error->MaxSafeProjectedRadiusPx > 0.0f &&
			Range != nullptr && Range->SplatCount > 0 &&
			ProjectedRadiusPx <= Error->MaxSafeProjectedRadiusPx)
		{
			return Level;
		}
	}
	return 0;
}

FSelection FSelector::Select(const FManifest& Manifest, TConstArrayView<FViewObservations> Views)
{
	TMap<uint32, uint32> RequiredLevels;
	TMap<uint32, float> MaxProjectedRadii;
	for (const FViewObservations& View : Views)
	{
		for (const FNodeObservation& Observation : View.Nodes)
		{
			if (!Observation.bVisible || !Manifest.Nodes.IsValidIndex(static_cast<int32>(Observation.NodeIndex)) ||
				!Manifest.Nodes[Observation.NodeIndex].IsLeaf())
				continue;
			const uint32 Required = SelectRequiredLevel(Manifest, Observation.NodeIndex, Observation.ProjectedRadiusPx);
			uint32* Existing = RequiredLevels.Find(Observation.NodeIndex);
			if (Existing == nullptr)
				RequiredLevels.Add(Observation.NodeIndex, Required);
			else
				*Existing = FMath::Min(*Existing, Required);
			float& MaxRadius = MaxProjectedRadii.FindOrAdd(Observation.NodeIndex, 0.0f);
			MaxRadius = FMath::Max(MaxRadius, Observation.ProjectedRadiusPx);
		}
	}

	if (Manifest.EnvironmentNode != MAX_uint32)
	{
		RequiredLevels.FindOrAdd(Manifest.EnvironmentNode, 0) = 0;
	}

	FSelection Result;
	RequiredLevels.KeySort(TLess<uint32>());
	for (const TPair<uint32, uint32>& Pair : RequiredLevels)
	{
		uint32 TargetLevel = Pair.Value;
		const uint32* PreviousLevel = ActiveLevels.Find(Pair.Key);
		if (PreviousLevel != nullptr && TargetLevel > *PreviousLevel)
		{
			const FNodeLodError* Error = Manifest.FindError(Pair.Key, TargetLevel);
			const float Radius = MaxProjectedRadii.FindRef(Pair.Key);
			constexpr float CoarsenHysteresisRatio = 0.80f;
			if (Error == nullptr || !Error->IsCalibrated() ||
				Radius > Error->MaxSafeProjectedRadiusPx * CoarsenHysteresisRatio)
			{
				TargetLevel = *PreviousLevel;
			}
		}
		ActiveLevels.FindOrAdd(Pair.Key) = TargetLevel;
		Result.Nodes.Add({Pair.Key, TargetLevel, Pair.Value});
	}
	return Result;
}

FSoftBudgetResult FSelector::ApplySoftBudget(
	const FManifest& Manifest,
	const TMap<uint32, uint32>& CertifiedLevels,
	const uint64 MaxActiveSplats,
	TMap<uint32, uint32>& InOutLevels)
{
	auto CountSelectedSplats = [&Manifest](const TMap<uint32, uint32>& Levels)
	{
		uint64 Count = 0;
		for (const TPair<uint32, uint32>& Pair : Levels)
		{
			if (const FNodeLodRange* Range = Manifest.FindRange(Pair.Key, Pair.Value))
			{
				Count += Range->SplatCount;
			}
		}
		return Count;
	};

	FSoftBudgetResult Result;
	Result.SelectedSplatCount = CountSelectedSplats(InOutLevels);
	Result.OriginalSplatCount = Result.SelectedSplatCount;
	Result.CertifiedMinimumSplatCount = CountSelectedSplats(CertifiedLevels);
	while (MaxActiveSplats > 0 && Result.SelectedSplatCount > MaxActiveSplats)
	{
		uint32 BestNode = MAX_uint32;
		uint32 BestNextLevel = 0;
		uint64 BestSaving = 0;
		for (const TPair<uint32, uint32>& Pair : InOutLevels)
		{
			const uint32* RequiredLevelPtr = CertifiedLevels.Find(Pair.Key);
			if (RequiredLevelPtr == nullptr)
				continue;
			const uint32 RequiredLevel = *RequiredLevelPtr;
			if (Pair.Value >= RequiredLevel)
				continue;
			const uint32 NextLevel = Pair.Value + 1;
			const FNodeLodRange* CurrentRange = Manifest.FindRange(Pair.Key, Pair.Value);
			const FNodeLodRange* NextRange = Manifest.FindRange(Pair.Key, NextLevel);
			if (CurrentRange == nullptr || NextRange == nullptr ||
				NextRange->SplatCount >= CurrentRange->SplatCount)
			{
				continue;
			}
			const uint64 Saving = CurrentRange->SplatCount - NextRange->SplatCount;
			if (Saving > BestSaving || (Saving == BestSaving && Pair.Key < BestNode))
			{
				BestNode = Pair.Key;
				BestNextLevel = NextLevel;
				BestSaving = Saving;
			}
		}
		if (BestNode == MAX_uint32)
			break;
		InOutLevels.FindChecked(BestNode) = BestNextLevel;
		Result.SelectedSplatCount -= BestSaving;
		++Result.CoarseningStepCount;
	}

	Result.bBudgetExceeded = MaxActiveSplats > 0 && Result.SelectedSplatCount > MaxActiveSplats;
	return Result;
}

void FSelector::Reset()
{
	ActiveLevels.Reset();
}
}
