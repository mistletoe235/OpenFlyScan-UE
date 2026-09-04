// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <cfloat>

namespace NanoGS::Paged
{
	/** A cell that does not belong to a seam group uses this value. */
	inline constexpr uint64 InvalidValidatedLODSeamGroupId = MAX_uint64;

	/** Page IDs are scoped by payload/container; different LOD files may reuse PageId. */
	struct NANOGS_API FValidatedLODPageKey
	{
		uint64 PayloadId = 0;
		uint64 PageId = 0;

		friend bool operator==(const FValidatedLODPageKey& A, const FValidatedLODPageKey& B)
		{
			return A.PayloadId == B.PayloadId && A.PageId == B.PageId;
		}

		friend uint32 GetTypeHash(const FValidatedLODPageKey& Key)
		{
			return HashCombine(GetTypeHash(Key.PayloadId), GetTypeHash(Key.PageId));
		}
	};

	/**
	 * One independently renderable cell representation. Level zero is always the
	 * Exact-HQ representation; increasing level numbers are progressively coarser.
	 */
	struct NANOGS_API FValidatedLODLevel
	{
		int32 Level = 0;

		/** Conservative world-space error for every certificate direction bin. */
		TArray<double> DirectionalErrorWorldCm;

		/** This representation is not certified at smaller support-bound depths. */
		double MinimumCertifiedDistanceCm = 0.0;

		/** Ignored for L0. A false coarse certificate makes that level unavailable. */
		bool bCertificateValid = false;

		/** Every page is required before this representation can be committed. */
		TArray<FValidatedLODPageKey> Pages;
	};

	struct NANOGS_API FValidatedLODCell
	{
		uint64 CellId = 0;
		TArray<FValidatedLODLevel> Levels;
		TArray<uint64> NeighborCellIds;
		uint64 SeamGroupId = InvalidValidatedLODSeamGroupId;
	};

	struct NANOGS_API FValidatedLODModel
	{
		TArray<FValidatedLODCell> Cells;
	};

	/**
	 * Renderer-independent observation of one cell in one view. The caller computes
	 * ConservativeDepthCm from the certified support bounds, not from the cell centre.
	 */
	struct NANOGS_API FValidatedLODCellObservation
	{
		uint64 CellId = 0;
		double ConservativeDepthCm = 0.0;
		double NearPlaneCm = 0.0;
		double HorizontalFocalPixels = 0.0;
		double VerticalFocalPixels = 0.0;
		bool bIntersectsNearPlane = false;
		bool bCertificateDomainValid = true;

		/**
		 * Bins whose maximum error is used for this observation. Passing no bins is
		 * deliberately conservative and uses the maximum over every bin.
		 */
		TArray<int32> ConservativeDirectionBins;
	};

	struct NANOGS_API FValidatedLODView
	{
		uint64 ViewId = 0;
		TArray<FValidatedLODCellObservation> CellObservations;
	};

	struct NANOGS_API FValidatedLODSelectorSettings
	{
		double RefineSSEThreshold = 0.50;
		double CoarsenSSEThreshold = 0.35;
		double MinimumSwitchIntervalSeconds = 0.25;
		int32 MaximumNeighborLevelDelta = 1;
	};

	struct NANOGS_API FValidatedLODSelectionRequest
	{
		/** Monotonic scheduler time; it need not be wall-clock time. */
		double TimeSeconds = 0.0;
		TArray<FValidatedLODView> Views;

		/** Snapshot of pages whose IO, CRC and upload have all completed. */
		TSet<FValidatedLODPageKey> ReadyPages;
	};

	enum class EValidatedLODDiagnostic : uint32
	{
		None = 0,
		CertificateDomainInvalid = 1u << 0,
		NearPlaneFallback = 1u << 1,
		InvalidObservationFallback = 1u << 2,
		CertificateUnavailable = 1u << 3,
		BelowMinimumCertifiedDistance = 1u << 4,
		RefineSSEExceeded = 1u << 5,
		SeamGroupRefinement = 1u << 6,
		NeighborRefinement = 1u << 7,
		TargetPagesNotReady = 1u << 8,
		MinimumSwitchInterval = 1u << 9,
		NeighborTransitionBlocked = 1u << 10,
		ActivePagesMissing = 1u << 11,
		CoarsenDeferred = 1u << 12,
		RequiredRefinementPending = 1u << 13,
	};
	ENUM_CLASS_FLAGS(EValidatedLODDiagnostic);

	struct NANOGS_API FValidatedLODCellSelection
	{
		uint64 CellId = 0;
		int32 ActiveLevel = 0;
		int32 RequiredLevel = 0;
		bool bActivePagesReady = false;
		bool bQualityReady = false;
		EValidatedLODDiagnostic Diagnostics = EValidatedLODDiagnostic::None;
		TArray<FValidatedLODPageKey> MissingPages;
	};

	/** A strict-capture failure for one cell in the returned frontier generation. */
	struct NANOGS_API FValidatedLODNotReadyDiagnostic
	{
		uint64 CellId = 0;
		int32 ActiveLevel = 0;
		int32 RequiredLevel = 0;
		EValidatedLODDiagnostic Reasons = EValidatedLODDiagnostic::None;
		TArray<FValidatedLODPageKey> MissingPages;
		FString Message;
	};

	struct NANOGS_API FValidatedLODSelection
	{
		/** Increments exactly once when an evaluation atomically changes the frontier. */
		uint64 Generation = 0;
		bool bFrontierChanged = false;

		/** True when every currently selected representation is complete. */
		bool bPreviewReady = false;

		/** Preview-ready and no cell is coarser than the multi-view quality requirement. */
		bool bStrictReady = false;
		TArray<FValidatedLODCellSelection> Cells;
		TArray<FValidatedLODNotReadyDiagnostic> NotReadyDiagnostics;
	};

	/**
	 * Stateful, CPU-only ValidatedLOD v1 selector. It owns only logical frontier
	 * state; page IO, GPU resources, sorting and frame-boundary publication remain
	 * the caller's responsibility.
	 */
	class NANOGS_API FValidatedLODSelector
	{
	public:
		/** Strictly validates the model and starts every cell on Exact-HQ L0. */
		bool Initialize(
			const FValidatedLODModel& InModel,
			const FValidatedLODSelectorSettings& InSettings,
			FString& OutError);

		void Reset();

		/**
		 * Evaluates and commits an atomic logical frontier from the supplied readiness
		 * snapshot. A false return denotes malformed input, not ordinary NotReady.
		 */
		bool Evaluate(
			const FValidatedLODSelectionRequest& Request,
			FValidatedLODSelection& OutSelection,
			FString& OutError);

		bool IsInitialized() const { return bInitialized; }
		uint64 GetFrontierGeneration() const { return FrontierGeneration; }
		bool FindActiveLevel(uint64 CellId, int32& OutLevel) const;

	private:
		struct FCellRuntimeState
		{
			int32 ActiveLevel = 0;
			double LastSwitchTimeSeconds = -DBL_MAX;
		};

		FValidatedLODModel Model;
		FValidatedLODSelectorSettings Settings;
		TMap<uint64, int32> CellIndexById;
		TArray<FCellRuntimeState> RuntimeStates;
		uint64 FrontierGeneration = 0;
		double LastEvaluationTimeSeconds = -DBL_MAX;
		bool bInitialized = false;
	};
}
