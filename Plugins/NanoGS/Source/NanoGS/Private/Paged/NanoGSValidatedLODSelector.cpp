// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSValidatedLODSelector.h"

namespace NanoGS::Paged
{
	namespace
	{
		struct FAtomicGroup
		{
			TArray<int32> CellIndices;
			int32 TargetLevel = 0;
			bool bHasRefinement = false;
			bool bHasCoarsening = false;
			bool bTargetPagesReady = false;
			bool bSwitchIntervalElapsed = false;
			TArray<FValidatedLODPageKey> MissingPages;
		};

		bool IsFiniteNonNegative(const double Value)
		{
			return FMath::IsFinite(Value) && Value >= 0.0;
		}

		void SortAndUniquePageKeys(TArray<FValidatedLODPageKey>& Keys)
		{
			Keys.Sort([](const FValidatedLODPageKey& A, const FValidatedLODPageKey& B)
			{
				return A.PayloadId != B.PayloadId
					? A.PayloadId < B.PayloadId
					: A.PageId < B.PageId;
			});
		for (int32 Index = Keys.Num() - 1; Index > 0; --Index)
		{
			if (Keys[Index] == Keys[Index - 1])
			{
				Keys.RemoveAt(Index, 1, EAllowShrinking::No);
			}
		}
	}

		bool GetMissingPages(
			const FValidatedLODLevel& Level,
			const TSet<FValidatedLODPageKey>& ReadyPages,
			TArray<FValidatedLODPageKey>& OutMissingPages)
		{
			for (const FValidatedLODPageKey& Page : Level.Pages)
			{
				if (!ReadyPages.Contains(Page))
				{
					OutMissingPages.Add(Page);
				}
			}
			return OutMissingPages.IsEmpty();
		}

		bool ValidateSettings(const FValidatedLODSelectorSettings& Settings, FString& OutError)
		{
			if (!IsFiniteNonNegative(Settings.CoarsenSSEThreshold)
				|| !IsFiniteNonNegative(Settings.RefineSSEThreshold)
				|| Settings.CoarsenSSEThreshold >= Settings.RefineSSEThreshold)
			{
				OutError = TEXT("ValidatedLOD requires finite thresholds with 0 <= coarsen < refine");
				return false;
			}
			if (!IsFiniteNonNegative(Settings.MinimumSwitchIntervalSeconds))
			{
				OutError = TEXT("ValidatedLOD minimum switch interval must be finite and non-negative");
				return false;
			}
			if (Settings.MaximumNeighborLevelDelta < 0)
			{
				OutError = TEXT("ValidatedLOD maximum neighbor level delta must be non-negative");
				return false;
			}
			return true;
		}

		bool ValidateLevel(
			const FValidatedLODCell& Cell,
			const int32 LevelIndex,
			const int32 DirectionBinCount,
			FString& OutError)
		{
			const FValidatedLODLevel& Level = Cell.Levels[LevelIndex];
			if (Level.Level != LevelIndex)
			{
				OutError = FString::Printf(
					TEXT("Cell %llu levels must be contiguous and ordered from L0 (found L%d at index %d)"),
					static_cast<unsigned long long>(Cell.CellId),
					Level.Level,
					LevelIndex);
				return false;
			}
			if (Level.DirectionalErrorWorldCm.Num() != DirectionBinCount)
			{
				OutError = FString::Printf(
					TEXT("Cell %llu L%d direction-bin count differs from L0"),
					static_cast<unsigned long long>(Cell.CellId),
					LevelIndex);
				return false;
			}
			if (!IsFiniteNonNegative(Level.MinimumCertifiedDistanceCm))
			{
				OutError = FString::Printf(
					TEXT("Cell %llu L%d has an invalid minimum certified distance"),
					static_cast<unsigned long long>(Cell.CellId),
					LevelIndex);
				return false;
			}
			if (Level.Pages.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Cell %llu L%d has no complete-representation pages"),
					static_cast<unsigned long long>(Cell.CellId),
					LevelIndex);
				return false;
			}

			TSet<FValidatedLODPageKey> UniquePages;
			for (const FValidatedLODPageKey& Page : Level.Pages)
			{
				if (UniquePages.Contains(Page))
				{
					OutError = FString::Printf(
						TEXT("Cell %llu L%d repeats payload %llu page %llu"),
						static_cast<unsigned long long>(Cell.CellId),
						LevelIndex,
						static_cast<unsigned long long>(Page.PayloadId),
						static_cast<unsigned long long>(Page.PageId));
					return false;
				}
				UniquePages.Add(Page);
			}

			for (int32 BinIndex = 0; BinIndex < DirectionBinCount; ++BinIndex)
			{
				const double Error = Level.DirectionalErrorWorldCm[BinIndex];
				if (!IsFiniteNonNegative(Error))
				{
					OutError = FString::Printf(
						TEXT("Cell %llu L%d has invalid direction-bin error"),
						static_cast<unsigned long long>(Cell.CellId),
						LevelIndex);
					return false;
				}
				if (LevelIndex == 0 && Error != 0.0)
				{
					OutError = FString::Printf(
						TEXT("Cell %llu L0 must be exact (zero certified error)"),
						static_cast<unsigned long long>(Cell.CellId));
					return false;
				}
				if (LevelIndex > 0
					&& Error < Cell.Levels[LevelIndex - 1].DirectionalErrorWorldCm[BinIndex])
				{
					OutError = FString::Printf(
						TEXT("Cell %llu L%d direction-bin error is not monotonic"),
						static_cast<unsigned long long>(Cell.CellId),
						LevelIndex);
					return false;
				}
			}

			if (LevelIndex == 0 && Level.MinimumCertifiedDistanceCm != 0.0)
			{
				OutError = FString::Printf(
					TEXT("Cell %llu Exact-HQ L0 must have zero minimum distance"),
					static_cast<unsigned long long>(Cell.CellId));
				return false;
			}
			if (LevelIndex > 0
				&& Level.MinimumCertifiedDistanceCm < Cell.Levels[LevelIndex - 1].MinimumCertifiedDistanceCm)
			{
				OutError = FString::Printf(
					TEXT("Cell %llu L%d minimum certified distance is not monotonic"),
					static_cast<unsigned long long>(Cell.CellId),
					LevelIndex);
				return false;
			}
			return true;
		}

		bool IsObservationDomainSafe(
			const FValidatedLODCellObservation& Observation,
			EValidatedLODDiagnostic& OutFailure)
		{
			if (!Observation.bCertificateDomainValid)
			{
				OutFailure |= EValidatedLODDiagnostic::CertificateDomainInvalid;
				return false;
			}
			if (!FMath::IsFinite(Observation.ConservativeDepthCm)
				|| !FMath::IsFinite(Observation.NearPlaneCm)
				|| !FMath::IsFinite(Observation.HorizontalFocalPixels)
				|| !FMath::IsFinite(Observation.VerticalFocalPixels)
				|| Observation.ConservativeDepthCm <= 0.0
				|| Observation.NearPlaneCm < 0.0
				|| FMath::Max(Observation.HorizontalFocalPixels, Observation.VerticalFocalPixels) <= 0.0)
			{
				OutFailure |= EValidatedLODDiagnostic::InvalidObservationFallback;
				return false;
			}
			if (Observation.bIntersectsNearPlane
				|| Observation.ConservativeDepthCm <= Observation.NearPlaneCm)
			{
				OutFailure |= EValidatedLODDiagnostic::NearPlaneFallback;
				return false;
			}
			return true;
		}

		bool IsLevelSafeForObservation(
			const FValidatedLODLevel& Level,
			const FValidatedLODCellObservation& Observation,
			const double SSEThreshold,
			const bool bRequireStrictlyBelowThreshold,
			EValidatedLODDiagnostic& OutFailure)
		{
			if (Level.Level == 0)
			{
				return true;
			}
			if (!Level.bCertificateValid)
			{
				OutFailure |= EValidatedLODDiagnostic::CertificateUnavailable;
				return false;
			}
			if (Observation.ConservativeDepthCm < Level.MinimumCertifiedDistanceCm)
			{
				OutFailure |= EValidatedLODDiagnostic::BelowMinimumCertifiedDistance;
				return false;
			}

			double ConservativeError = 0.0;
			if (Observation.ConservativeDirectionBins.IsEmpty())
			{
				for (const double Error : Level.DirectionalErrorWorldCm)
				{
					ConservativeError = FMath::Max(ConservativeError, Error);
				}
			}
			else
			{
				for (const int32 Bin : Observation.ConservativeDirectionBins)
				{
					if (!Level.DirectionalErrorWorldCm.IsValidIndex(Bin))
					{
						OutFailure |= EValidatedLODDiagnostic::InvalidObservationFallback;
						return false;
					}
					ConservativeError = FMath::Max(ConservativeError, Level.DirectionalErrorWorldCm[Bin]);
				}
			}

			const double FocalPixels = FMath::Max(
				Observation.HorizontalFocalPixels,
				Observation.VerticalFocalPixels);
			const double SSE = ConservativeError * FocalPixels / Observation.ConservativeDepthCm;
			if (!FMath::IsFinite(SSE)
				|| (bRequireStrictlyBelowThreshold ? SSE >= SSEThreshold : SSE > SSEThreshold))
			{
				OutFailure |= EValidatedLODDiagnostic::RefineSSEExceeded;
				return false;
			}
			return true;
		}

		int32 ComputeViewRequiredLevel(
			const FValidatedLODCell& Cell,
			const int32 ActiveLevel,
			const FValidatedLODCellObservation& Observation,
			const FValidatedLODSelectorSettings& Settings,
			EValidatedLODDiagnostic& OutDiagnostics)
		{
			EValidatedLODDiagnostic DomainFailure = EValidatedLODDiagnostic::None;
			if (!IsObservationDomainSafe(Observation, DomainFailure))
			{
				OutDiagnostics |= DomainFailure;
				return 0;
			}

			if (ActiveLevel > 0)
			{
				EValidatedLODDiagnostic ActiveFailure = EValidatedLODDiagnostic::None;
				if (!IsLevelSafeForObservation(
					Cell.Levels[ActiveLevel],
					Observation,
					Settings.RefineSSEThreshold,
					false,
					ActiveFailure))
				{
					OutDiagnostics |= ActiveFailure;
					for (int32 Candidate = ActiveLevel - 1; Candidate > 0; --Candidate)
					{
						EValidatedLODDiagnostic IgnoredFailure = EValidatedLODDiagnostic::None;
						if (IsLevelSafeForObservation(
							Cell.Levels[Candidate],
							Observation,
							Settings.RefineSSEThreshold,
							false,
							IgnoredFailure))
						{
							return Candidate;
						}
					}
					return 0;
				}
			}

			int32 RequiredLevel = ActiveLevel;
			for (int32 Candidate = ActiveLevel + 1; Candidate < Cell.Levels.Num(); ++Candidate)
			{
				EValidatedLODDiagnostic IgnoredFailure = EValidatedLODDiagnostic::None;
				if (!IsLevelSafeForObservation(
					Cell.Levels[Candidate],
					Observation,
					Settings.CoarsenSSEThreshold,
					true,
					IgnoredFailure))
				{
					break;
				}
				RequiredLevel = Candidate;
			}
			return RequiredLevel;
		}

		bool FrontierObeysNeighbors(
			const FValidatedLODModel& Model,
			const TMap<uint64, int32>& CellIndexById,
			const TArray<int32>& Levels,
			const int32 MaximumDelta)
		{
			for (int32 CellIndex = 0; CellIndex < Model.Cells.Num(); ++CellIndex)
			{
				for (const uint64 NeighborId : Model.Cells[CellIndex].NeighborCellIds)
				{
					const int32* NeighborIndex = CellIndexById.Find(NeighborId);
					check(NeighborIndex != nullptr);
					if (FMath::Abs(Levels[CellIndex] - Levels[*NeighborIndex]) > MaximumDelta)
					{
						return false;
					}
				}
			}
			return true;
		}
	}

	bool FValidatedLODSelector::Initialize(
		const FValidatedLODModel& InModel,
		const FValidatedLODSelectorSettings& InSettings,
		FString& OutError)
	{
		Reset();
		OutError.Reset();
		if (!ValidateSettings(InSettings, OutError))
		{
			return false;
		}
		if (InModel.Cells.IsEmpty())
		{
			OutError = TEXT("ValidatedLOD model contains no cells");
			return false;
		}

		Model = InModel;
		Model.Cells.Sort([](const FValidatedLODCell& A, const FValidatedLODCell& B)
		{
			return A.CellId < B.CellId;
		});

		for (int32 CellIndex = 0; CellIndex < Model.Cells.Num(); ++CellIndex)
		{
			FValidatedLODCell& Cell = Model.Cells[CellIndex];
			if (CellIndexById.Contains(Cell.CellId))
			{
				OutError = FString::Printf(
					TEXT("ValidatedLOD repeats cell ID %llu"),
					static_cast<unsigned long long>(Cell.CellId));
				Reset();
				return false;
			}
			CellIndexById.Add(Cell.CellId, CellIndex);
			if (Cell.Levels.IsEmpty() || Cell.Levels[0].DirectionalErrorWorldCm.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Cell %llu requires Exact-HQ L0 and at least one error direction bin"),
					static_cast<unsigned long long>(Cell.CellId));
				Reset();
				return false;
			}

			const int32 DirectionBinCount = Cell.Levels[0].DirectionalErrorWorldCm.Num();
			for (int32 LevelIndex = 0; LevelIndex < Cell.Levels.Num(); ++LevelIndex)
			{
				if (!ValidateLevel(Cell, LevelIndex, DirectionBinCount, OutError))
				{
					Reset();
					return false;
				}
			}

			Cell.NeighborCellIds.Sort();
			for (int32 NeighborIndex = Cell.NeighborCellIds.Num() - 1; NeighborIndex > 0; --NeighborIndex)
			{
				if (Cell.NeighborCellIds[NeighborIndex] == Cell.NeighborCellIds[NeighborIndex - 1])
				{
					OutError = FString::Printf(
						TEXT("Cell %llu repeats a neighbor ID"),
						static_cast<unsigned long long>(Cell.CellId));
					Reset();
					return false;
				}
			}
		}

		for (const FValidatedLODCell& Cell : Model.Cells)
		{
			for (const uint64 NeighborId : Cell.NeighborCellIds)
			{
				const int32* NeighborIndex = CellIndexById.Find(NeighborId);
				if (NeighborId == Cell.CellId || NeighborIndex == nullptr)
				{
					OutError = FString::Printf(
						TEXT("Cell %llu has a self or unknown neighbor %llu"),
						static_cast<unsigned long long>(Cell.CellId),
						static_cast<unsigned long long>(NeighborId));
					Reset();
					return false;
				}
				if (!Model.Cells[*NeighborIndex].NeighborCellIds.Contains(Cell.CellId))
				{
					OutError = FString::Printf(
						TEXT("Neighbor relation %llu -> %llu is not symmetric"),
						static_cast<unsigned long long>(Cell.CellId),
						static_cast<unsigned long long>(NeighborId));
					Reset();
					return false;
				}
			}
		}

		Settings = InSettings;
		RuntimeStates.SetNum(Model.Cells.Num());
		FrontierGeneration = 0;
		LastEvaluationTimeSeconds = -DBL_MAX;
		bInitialized = true;
		return true;
	}

	void FValidatedLODSelector::Reset()
	{
		Model.Cells.Reset();
		CellIndexById.Reset();
		RuntimeStates.Reset();
		FrontierGeneration = 0;
		LastEvaluationTimeSeconds = -DBL_MAX;
		bInitialized = false;
	}

	bool FValidatedLODSelector::FindActiveLevel(const uint64 CellId, int32& OutLevel) const
	{
		const int32* CellIndex = CellIndexById.Find(CellId);
		if (!bInitialized || CellIndex == nullptr)
		{
			return false;
		}
		OutLevel = RuntimeStates[*CellIndex].ActiveLevel;
		return true;
	}

	bool FValidatedLODSelector::Evaluate(
		const FValidatedLODSelectionRequest& Request,
		FValidatedLODSelection& OutSelection,
		FString& OutError)
	{
		OutSelection = FValidatedLODSelection();
		OutError.Reset();
		if (!bInitialized)
		{
			OutError = TEXT("ValidatedLOD selector is not initialized");
			return false;
		}
		if (!FMath::IsFinite(Request.TimeSeconds) || Request.TimeSeconds < LastEvaluationTimeSeconds)
		{
			OutError = TEXT("ValidatedLOD evaluation time must be finite and monotonic");
			return false;
		}

		TSet<uint64> ViewIds;
		for (const FValidatedLODView& View : Request.Views)
		{
			if (ViewIds.Contains(View.ViewId))
			{
				OutError = FString::Printf(
					TEXT("ValidatedLOD request repeats view ID %llu"),
					static_cast<unsigned long long>(View.ViewId));
				return false;
			}
			ViewIds.Add(View.ViewId);

			TSet<uint64> ObservedCellIds;
			for (const FValidatedLODCellObservation& Observation : View.CellObservations)
			{
				if (!CellIndexById.Contains(Observation.CellId))
				{
					OutError = FString::Printf(
						TEXT("View %llu observes unknown cell %llu"),
						static_cast<unsigned long long>(View.ViewId),
						static_cast<unsigned long long>(Observation.CellId));
					return false;
				}
				if (ObservedCellIds.Contains(Observation.CellId))
				{
					OutError = FString::Printf(
						TEXT("View %llu repeats cell %llu"),
						static_cast<unsigned long long>(View.ViewId),
						static_cast<unsigned long long>(Observation.CellId));
					return false;
				}
				ObservedCellIds.Add(Observation.CellId);
			}
		}

		const int32 CellCount = Model.Cells.Num();
		TArray<int32> RequiredLevels;
		TArray<int32> ActiveLevels;
		TArray<EValidatedLODDiagnostic> Diagnostics;
		TArray<TArray<FValidatedLODPageKey>> DeferredMissingPages;
		TArray<bool> bObserved;
		RequiredLevels.SetNumUninitialized(CellCount);
		ActiveLevels.SetNumUninitialized(CellCount);
		Diagnostics.Init(EValidatedLODDiagnostic::None, CellCount);
		DeferredMissingPages.SetNum(CellCount);
		bObserved.Init(false, CellCount);
		for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
		{
			RequiredLevels[CellIndex] = RuntimeStates[CellIndex].ActiveLevel;
			ActiveLevels[CellIndex] = RuntimeStates[CellIndex].ActiveLevel;
		}

		// Every view evaluates against the same active frontier. Taking the minimum
		// level number is exactly the finest requirement across all views.
		for (const FValidatedLODView& View : Request.Views)
		{
			for (const FValidatedLODCellObservation& Observation : View.CellObservations)
			{
				const int32 CellIndex = CellIndexById.FindChecked(Observation.CellId);
				EValidatedLODDiagnostic ViewDiagnostics = EValidatedLODDiagnostic::None;
				const int32 ViewRequired = ComputeViewRequiredLevel(
					Model.Cells[CellIndex],
					ActiveLevels[CellIndex],
					Observation,
					Settings,
					ViewDiagnostics);
				if (!bObserved[CellIndex])
				{
					RequiredLevels[CellIndex] = ViewRequired;
					bObserved[CellIndex] = true;
				}
				else
				{
					RequiredLevels[CellIndex] = FMath::Min(RequiredLevels[CellIndex], ViewRequired);
				}
				Diagnostics[CellIndex] |= ViewDiagnostics;
			}
		}

		// Seam equality and neighbor delta form a refinement-only closure. No cell is
		// ever made coarser to satisfy either constraint.
		bool bConstraintsChanged = true;
		while (bConstraintsChanged)
		{
			bConstraintsChanged = false;
			TMap<uint64, int32> FinestBySeamGroup;
			for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
			{
				const uint64 SeamGroupId = Model.Cells[CellIndex].SeamGroupId;
				if (SeamGroupId == InvalidValidatedLODSeamGroupId)
				{
					continue;
				}
				int32* Existing = FinestBySeamGroup.Find(SeamGroupId);
				if (Existing == nullptr)
				{
					FinestBySeamGroup.Add(SeamGroupId, RequiredLevels[CellIndex]);
				}
				else
				{
					*Existing = FMath::Min(*Existing, RequiredLevels[CellIndex]);
				}
			}
			for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
			{
				const uint64 SeamGroupId = Model.Cells[CellIndex].SeamGroupId;
				if (SeamGroupId == InvalidValidatedLODSeamGroupId)
				{
					continue;
				}
				const int32 GroupLevel = FinestBySeamGroup.FindChecked(SeamGroupId);
				if (RequiredLevels[CellIndex] > GroupLevel)
				{
					RequiredLevels[CellIndex] = GroupLevel;
					Diagnostics[CellIndex] |= EValidatedLODDiagnostic::SeamGroupRefinement;
					bConstraintsChanged = true;
				}
			}

			for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
			{
				for (const uint64 NeighborId : Model.Cells[CellIndex].NeighborCellIds)
				{
					const int32 NeighborIndex = CellIndexById.FindChecked(NeighborId);
					const int32 FinestAllowed = RequiredLevels[NeighborIndex] + Settings.MaximumNeighborLevelDelta;
					if (RequiredLevels[CellIndex] > FinestAllowed)
					{
						RequiredLevels[CellIndex] = FinestAllowed;
						Diagnostics[CellIndex] |= EValidatedLODDiagnostic::NeighborRefinement;
						bConstraintsChanged = true;
					}
				}
			}
		}

		TArray<FAtomicGroup> Groups;
		TArray<int32> CellToGroup;
		TMap<uint64, int32> SeamGroupToAtomicGroup;
		CellToGroup.SetNumUninitialized(CellCount);
		for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
		{
			const uint64 SeamGroupId = Model.Cells[CellIndex].SeamGroupId;
			int32 GroupIndex = INDEX_NONE;
			if (SeamGroupId != InvalidValidatedLODSeamGroupId)
			{
				if (const int32* Existing = SeamGroupToAtomicGroup.Find(SeamGroupId))
				{
					GroupIndex = *Existing;
				}
			}
			if (GroupIndex == INDEX_NONE)
			{
				GroupIndex = Groups.AddDefaulted();
				if (SeamGroupId != InvalidValidatedLODSeamGroupId)
				{
					SeamGroupToAtomicGroup.Add(SeamGroupId, GroupIndex);
				}
			}
			Groups[GroupIndex].CellIndices.Add(CellIndex);
			CellToGroup[CellIndex] = GroupIndex;
		}

		for (FAtomicGroup& Group : Groups)
		{
			check(!Group.CellIndices.IsEmpty());
			Group.TargetLevel = RequiredLevels[Group.CellIndices[0]];
			Group.bTargetPagesReady = true;
			Group.bSwitchIntervalElapsed = true;
			for (const int32 CellIndex : Group.CellIndices)
			{
				check(RequiredLevels[CellIndex] == Group.TargetLevel);
				Group.bHasRefinement |= Group.TargetLevel < ActiveLevels[CellIndex];
				Group.bHasCoarsening |= Group.TargetLevel > ActiveLevels[CellIndex];
				TArray<FValidatedLODPageKey> CellMissing;
				if (!GetMissingPages(
					Model.Cells[CellIndex].Levels[Group.TargetLevel],
					Request.ReadyPages,
					CellMissing))
				{
					Group.bTargetPagesReady = false;
					Group.MissingPages.Append(CellMissing);
				}
				if (Group.TargetLevel != ActiveLevels[CellIndex]
					&& Request.TimeSeconds - RuntimeStates[CellIndex].LastSwitchTimeSeconds
						< Settings.MinimumSwitchIntervalSeconds)
				{
					Group.bSwitchIntervalElapsed = false;
				}
			}
			SortAndUniquePageKeys(Group.MissingPages);
		}

		TArray<int32> CandidateLevels = ActiveLevels;
		TArray<bool> bAppliedRefinement;
		bAppliedRefinement.Init(false, Groups.Num());
		for (int32 GroupIndex = 0; GroupIndex < Groups.Num(); ++GroupIndex)
		{
			const FAtomicGroup& Group = Groups[GroupIndex];
			if (!Group.bHasRefinement)
			{
				continue;
			}
			if (Group.bTargetPagesReady && Group.bSwitchIntervalElapsed)
			{
				bAppliedRefinement[GroupIndex] = true;
				for (const int32 CellIndex : Group.CellIndices)
				{
					CandidateLevels[CellIndex] = Group.TargetLevel;
				}
			}
			else
			{
				for (const int32 CellIndex : Group.CellIndices)
				{
					Diagnostics[CellIndex] |= EValidatedLODDiagnostic::RequiredRefinementPending;
					if (!Group.bTargetPagesReady)
					{
						Diagnostics[CellIndex] |= EValidatedLODDiagnostic::TargetPagesNotReady;
						DeferredMissingPages[CellIndex].Append(Group.MissingPages);
					}
					if (!Group.bSwitchIntervalElapsed)
					{
						Diagnostics[CellIndex] |= EValidatedLODDiagnostic::MinimumSwitchInterval;
					}
				}
			}
		}

		// Apply all ready refinement groups together, then remove any fine-end group
		// whose absent coarse-end dependency would make the active frontier invalid.
		bool bRemovedProposal = true;
		while (bRemovedProposal)
		{
			bRemovedProposal = false;
			for (int32 CellIndex = 0; CellIndex < CellCount && !bRemovedProposal; ++CellIndex)
			{
				for (const uint64 NeighborId : Model.Cells[CellIndex].NeighborCellIds)
				{
					const int32 NeighborIndex = CellIndexById.FindChecked(NeighborId);
					if (FMath::Abs(CandidateLevels[CellIndex] - CandidateLevels[NeighborIndex])
						<= Settings.MaximumNeighborLevelDelta)
					{
						continue;
					}
					const int32 FineIndex = CandidateLevels[CellIndex] < CandidateLevels[NeighborIndex]
						? CellIndex
						: NeighborIndex;
					const int32 FineGroupIndex = CellToGroup[FineIndex];
					if (!bAppliedRefinement[FineGroupIndex])
					{
						continue;
					}
					bAppliedRefinement[FineGroupIndex] = false;
					for (const int32 MemberIndex : Groups[FineGroupIndex].CellIndices)
					{
						CandidateLevels[MemberIndex] = ActiveLevels[MemberIndex];
						Diagnostics[MemberIndex] |= EValidatedLODDiagnostic::RequiredRefinementPending;
						Diagnostics[MemberIndex] |= EValidatedLODDiagnostic::NeighborTransitionBlocked;
					}
					bRemovedProposal = true;
					break;
				}
			}
		}

		const TArray<int32> AfterRefinementLevels = CandidateLevels;
		TArray<bool> bAppliedCoarsening;
		bAppliedCoarsening.Init(false, Groups.Num());
		for (int32 GroupIndex = 0; GroupIndex < Groups.Num(); ++GroupIndex)
		{
			const FAtomicGroup& Group = Groups[GroupIndex];
			if (Group.bHasRefinement || !Group.bHasCoarsening)
			{
				continue;
			}
			if (Group.bTargetPagesReady && Group.bSwitchIntervalElapsed)
			{
				bAppliedCoarsening[GroupIndex] = true;
				for (const int32 CellIndex : Group.CellIndices)
				{
					CandidateLevels[CellIndex] = Group.TargetLevel;
				}
			}
			else
			{
				for (const int32 CellIndex : Group.CellIndices)
				{
					Diagnostics[CellIndex] |= EValidatedLODDiagnostic::CoarsenDeferred;
					if (!Group.bTargetPagesReady)
					{
						Diagnostics[CellIndex] |= EValidatedLODDiagnostic::TargetPagesNotReady;
						DeferredMissingPages[CellIndex].Append(Group.MissingPages);
					}
					if (!Group.bSwitchIntervalElapsed)
					{
						Diagnostics[CellIndex] |= EValidatedLODDiagnostic::MinimumSwitchInterval;
					}
				}
			}
		}

		bRemovedProposal = true;
		while (bRemovedProposal)
		{
			bRemovedProposal = false;
			for (int32 CellIndex = 0; CellIndex < CellCount && !bRemovedProposal; ++CellIndex)
			{
				for (const uint64 NeighborId : Model.Cells[CellIndex].NeighborCellIds)
				{
					const int32 NeighborIndex = CellIndexById.FindChecked(NeighborId);
					if (FMath::Abs(CandidateLevels[CellIndex] - CandidateLevels[NeighborIndex])
						<= Settings.MaximumNeighborLevelDelta)
					{
						continue;
					}
					const int32 CoarseIndex = CandidateLevels[CellIndex] > CandidateLevels[NeighborIndex]
						? CellIndex
						: NeighborIndex;
					const int32 CoarseGroupIndex = CellToGroup[CoarseIndex];
					if (!bAppliedCoarsening[CoarseGroupIndex])
					{
						continue;
					}
					bAppliedCoarsening[CoarseGroupIndex] = false;
					for (const int32 MemberIndex : Groups[CoarseGroupIndex].CellIndices)
					{
						CandidateLevels[MemberIndex] = AfterRefinementLevels[MemberIndex];
						Diagnostics[MemberIndex] |= EValidatedLODDiagnostic::CoarsenDeferred;
						Diagnostics[MemberIndex] |= EValidatedLODDiagnostic::NeighborTransitionBlocked;
					}
					bRemovedProposal = true;
					break;
				}
			}
		}

		if (!FrontierObeysNeighbors(Model, CellIndexById, CandidateLevels, Settings.MaximumNeighborLevelDelta))
		{
			OutError = TEXT("ValidatedLOD could not produce a neighbor-safe atomic frontier");
			return false;
		}

		bool bFrontierChanged = false;
		for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
		{
			bFrontierChanged |= CandidateLevels[CellIndex] != ActiveLevels[CellIndex];
		}
		if (bFrontierChanged && FrontierGeneration == MAX_uint64)
		{
			OutError = TEXT("ValidatedLOD frontier generation overflow");
			return false;
		}
		if (bFrontierChanged)
		{
			++FrontierGeneration;
			for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
			{
				if (RuntimeStates[CellIndex].ActiveLevel != CandidateLevels[CellIndex])
				{
					RuntimeStates[CellIndex].ActiveLevel = CandidateLevels[CellIndex];
					RuntimeStates[CellIndex].LastSwitchTimeSeconds = Request.TimeSeconds;
				}
			}
		}
		LastEvaluationTimeSeconds = Request.TimeSeconds;

		OutSelection.Generation = FrontierGeneration;
		OutSelection.bFrontierChanged = bFrontierChanged;
		OutSelection.bPreviewReady = true;
		OutSelection.bStrictReady = true;
		OutSelection.Cells.Reserve(CellCount);
		for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
		{
			FValidatedLODCellSelection& CellResult = OutSelection.Cells.AddDefaulted_GetRef();
			CellResult.CellId = Model.Cells[CellIndex].CellId;
			CellResult.ActiveLevel = RuntimeStates[CellIndex].ActiveLevel;
			CellResult.RequiredLevel = RequiredLevels[CellIndex];
			CellResult.Diagnostics = Diagnostics[CellIndex];

			TArray<FValidatedLODPageKey> ActiveMissing;
			CellResult.bActivePagesReady = GetMissingPages(
				Model.Cells[CellIndex].Levels[CellResult.ActiveLevel],
				Request.ReadyPages,
				ActiveMissing);
			if (!CellResult.bActivePagesReady)
			{
				CellResult.Diagnostics |= EValidatedLODDiagnostic::ActivePagesMissing;
				CellResult.MissingPages.Append(ActiveMissing);
			}

			CellResult.bQualityReady = CellResult.ActiveLevel <= CellResult.RequiredLevel;
			if (!CellResult.bQualityReady)
			{
				CellResult.Diagnostics |= EValidatedLODDiagnostic::RequiredRefinementPending;
				TArray<FValidatedLODPageKey> RequiredMissing;
				GetMissingPages(
					Model.Cells[CellIndex].Levels[CellResult.RequiredLevel],
					Request.ReadyPages,
					RequiredMissing);
				if (!RequiredMissing.IsEmpty())
				{
					CellResult.Diagnostics |= EValidatedLODDiagnostic::TargetPagesNotReady;
					CellResult.MissingPages.Append(RequiredMissing);
				}
			}
			else if (CellResult.ActiveLevel < CellResult.RequiredLevel)
			{
				CellResult.Diagnostics |= EValidatedLODDiagnostic::CoarsenDeferred;
				TArray<FValidatedLODPageKey> CoarseMissing;
				GetMissingPages(
					Model.Cells[CellIndex].Levels[CellResult.RequiredLevel],
					Request.ReadyPages,
					CoarseMissing);
				if (!CoarseMissing.IsEmpty())
				{
					CellResult.Diagnostics |= EValidatedLODDiagnostic::TargetPagesNotReady;
					CellResult.MissingPages.Append(CoarseMissing);
				}
			}
			CellResult.MissingPages.Append(DeferredMissingPages[CellIndex]);
			SortAndUniquePageKeys(CellResult.MissingPages);

			const bool bCellStrictReady = CellResult.bActivePagesReady && CellResult.bQualityReady;
			OutSelection.bPreviewReady &= CellResult.bActivePagesReady;
			OutSelection.bStrictReady &= bCellStrictReady;
			if (!bCellStrictReady)
			{
				FValidatedLODNotReadyDiagnostic& NotReady = OutSelection.NotReadyDiagnostics.AddDefaulted_GetRef();
				NotReady.CellId = CellResult.CellId;
				NotReady.ActiveLevel = CellResult.ActiveLevel;
				NotReady.RequiredLevel = CellResult.RequiredLevel;
				NotReady.Reasons = CellResult.Diagnostics;
				NotReady.MissingPages = CellResult.MissingPages;
				NotReady.Message = FString::Printf(
					TEXT("Cell %llu NotReady: active L%d, required L%d, missing pages=%d, reasons=0x%08x"),
					static_cast<unsigned long long>(CellResult.CellId),
					CellResult.ActiveLevel,
					CellResult.RequiredLevel,
					CellResult.MissingPages.Num(),
					static_cast<uint32>(CellResult.Diagnostics));
			}
		}
		return true;
	}
}
