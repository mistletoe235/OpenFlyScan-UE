// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Paged/NanoGSValidatedLODSelector.h"

#include "Misc/AutomationTest.h"

namespace NanoGS::Paged::Tests
{
	namespace
	{
		FValidatedLODPageKey PageKey(const uint64 CellId, const int32 Level, const uint64 Page)
		{
			return {CellId * 10ull + static_cast<uint64>(Level), Page};
		}

		FValidatedLODCell MakeCell(
			const uint64 CellId,
			const int32 LevelCount = 3,
			const uint64 SeamGroupId = InvalidValidatedLODSeamGroupId,
			TArray<uint64> Neighbors = {})
		{
			FValidatedLODCell Cell;
			Cell.CellId = CellId;
			Cell.SeamGroupId = SeamGroupId;
			Cell.NeighborCellIds = MoveTemp(Neighbors);
			for (int32 LevelIndex = 0; LevelIndex < LevelCount; ++LevelIndex)
			{
				FValidatedLODLevel& Level = Cell.Levels.AddDefaulted_GetRef();
				Level.Level = LevelIndex;
				Level.DirectionalErrorWorldCm = {
					static_cast<double>(LevelIndex),
					static_cast<double>(LevelIndex) * 1.1,
				};
				Level.MinimumCertifiedDistanceCm = 0.0;
				Level.bCertificateValid = true;
				Level.Pages = {
					PageKey(CellId, LevelIndex, 0),
					PageKey(CellId, LevelIndex, 1),
				};
			}
			return Cell;
		}

		FValidatedLODModel MakeSingleCellModel(const int32 LevelCount = 3)
		{
			FValidatedLODModel Model;
			Model.Cells.Add(MakeCell(1, LevelCount));
			return Model;
		}

		void AddAllPages(const FValidatedLODModel& Model, TSet<FValidatedLODPageKey>& ReadyPages)
		{
			for (const FValidatedLODCell& Cell : Model.Cells)
			{
				for (const FValidatedLODLevel& Level : Cell.Levels)
				{
					ReadyPages.Append(Level.Pages);
				}
			}
		}

		FValidatedLODCellObservation Observe(
			const uint64 CellId,
			const double DepthCm,
			const bool bDomainValid = true,
			const bool bNearPlane = false)
		{
			FValidatedLODCellObservation Observation;
			Observation.CellId = CellId;
			Observation.ConservativeDepthCm = DepthCm;
			Observation.NearPlaneCm = 1.0;
			Observation.HorizontalFocalPixels = 100.0;
			Observation.VerticalFocalPixels = 90.0;
			Observation.bCertificateDomainValid = bDomainValid;
			Observation.bIntersectsNearPlane = bNearPlane;
			Observation.ConservativeDirectionBins = {0};
			return Observation;
		}

		FValidatedLODSelectionRequest MakeRequest(
			const double TimeSeconds,
			const TSet<FValidatedLODPageKey>& ReadyPages,
			TArray<FValidatedLODCellObservation> Observations)
		{
			FValidatedLODSelectionRequest Request;
			Request.TimeSeconds = TimeSeconds;
			Request.ReadyPages = ReadyPages;
			FValidatedLODView& View = Request.Views.AddDefaulted_GetRef();
			View.ViewId = 100;
			View.CellObservations = MoveTemp(Observations);
			return Request;
		}

		const FValidatedLODCellSelection* FindCell(
			const FValidatedLODSelection& Selection,
			const uint64 CellId)
		{
			return Selection.Cells.FindByPredicate([CellId](const FValidatedLODCellSelection& Cell)
			{
				return Cell.CellId == CellId;
			});
		}

		bool Evaluate(
			FAutomationTestBase& Test,
			FValidatedLODSelector& Selector,
			const FValidatedLODSelectionRequest& Request,
			FValidatedLODSelection& OutSelection)
		{
			FString Error;
			const bool bSucceeded = Selector.Evaluate(Request, OutSelection, Error);
			Test.TestTrue(FString::Printf(TEXT("Selector evaluation succeeds: %s"), *Error), bSucceeded);
			return bSucceeded;
		}

		FValidatedLODSelectorSettings ImmediateSwitchSettings()
		{
			FValidatedLODSelectorSettings Settings;
			Settings.MinimumSwitchIntervalSeconds = 0.0;
			return Settings;
		}
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSValidatedLODModelValidationTest,
		"NanoGS.Paged.ValidatedLOD.ModelValidation",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSValidatedLODModelValidationTest::RunTest(const FString& Parameters)
	{
		FString Error;
		FValidatedLODSelector Selector;
		const FValidatedLODSelectorSettings Settings = ImmediateSwitchSettings();
		const FValidatedLODModel ValidModel = MakeSingleCellModel();
		TestTrue(TEXT("A contiguous exact-L0 model initializes"), Selector.Initialize(ValidModel, Settings, Error));

		FValidatedLODModel NonMonotonicError = ValidModel;
		NonMonotonicError.Cells[0].Levels[2].DirectionalErrorWorldCm[0] = 0.5;
		TestFalse(
			TEXT("Coarser error decreasing below a finer level is rejected"),
			Selector.Initialize(NonMonotonicError, Settings, Error));
		TestTrue(TEXT("Error monotonicity failure is explicit"), Error.Contains(TEXT("error is not monotonic")));

		FValidatedLODModel NonMonotonicDistance = ValidModel;
		NonMonotonicDistance.Cells[0].Levels[1].MinimumCertifiedDistanceCm = 200.0;
		NonMonotonicDistance.Cells[0].Levels[2].MinimumCertifiedDistanceCm = 100.0;
		TestFalse(
			TEXT("Coarser minimum distance decreasing below a finer level is rejected"),
			Selector.Initialize(NonMonotonicDistance, Settings, Error));
		TestTrue(TEXT("Distance monotonicity failure is explicit"), Error.Contains(TEXT("distance is not monotonic")));

		FValidatedLODModel InexactL0 = ValidModel;
		InexactL0.Cells[0].Levels[0].DirectionalErrorWorldCm[1] = 0.01;
		TestFalse(TEXT("L0 with non-zero error is rejected"), Selector.Initialize(InexactL0, Settings, Error));
		TestTrue(TEXT("Exact-L0 failure is explicit"), Error.Contains(TEXT("L0 must be exact")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSValidatedLODHysteresisTest,
		"NanoGS.Paged.ValidatedLOD.SSEHysteresisAndMinimumDistance",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSValidatedLODHysteresisTest::RunTest(const FString& Parameters)
	{
		FValidatedLODModel Model = MakeSingleCellModel(2);
		Model.Cells[0].Levels[1].MinimumCertifiedDistanceCm = 100.0;
		TSet<FValidatedLODPageKey> ReadyPages;
		AddAllPages(Model, ReadyPages);

		FValidatedLODSelector Selector;
		FString Error;
		TestTrue(TEXT("Selector initializes"), Selector.Initialize(Model, ImmediateSwitchSettings(), Error));

		FValidatedLODSelection Selection;
		Evaluate(*this, Selector, MakeRequest(0.0, ReadyPages, {Observe(1, 300.0)}), Selection);
		TestEqual(TEXT("SSE 0.333 is below coarsen 0.35"), Selection.Cells[0].ActiveLevel, 1);
		TestEqual(TEXT("First frontier switch creates generation one"), Selection.Generation, 1ull);

		Evaluate(*this, Selector, MakeRequest(1.0, ReadyPages, {Observe(1, 220.0)}), Selection);
		TestEqual(TEXT("SSE 0.455 remains inside hysteresis band"), Selection.Cells[0].ActiveLevel, 1);
		TestFalse(TEXT("Hysteresis hold does not change the frontier"), Selection.bFrontierChanged);

		Evaluate(*this, Selector, MakeRequest(2.0, ReadyPages, {Observe(1, 200.0)}), Selection);
		TestEqual(TEXT("SSE exactly 0.5 does not refine"), Selection.Cells[0].ActiveLevel, 1);

		Evaluate(*this, Selector, MakeRequest(3.0, ReadyPages, {Observe(1, 190.0)}), Selection);
		TestEqual(TEXT("SSE above 0.5 refines to L0"), Selection.Cells[0].ActiveLevel, 0);
		TestEqual(TEXT("Refinement advances generation once"), Selection.Generation, 2ull);

		Evaluate(*this, Selector, MakeRequest(4.0, ReadyPages, {Observe(1, 250.0)}), Selection);
		TestEqual(TEXT("SSE 0.4 cannot coarsen back across the 0.35 gate"), Selection.Cells[0].ActiveLevel, 0);

		Evaluate(*this, Selector, MakeRequest(5.0, ReadyPages, {Observe(1, 300.0)}), Selection);
		TestEqual(TEXT("Far observation coarsens again"), Selection.Cells[0].ActiveLevel, 1);
		Evaluate(*this, Selector, MakeRequest(6.0, ReadyPages, {Observe(1, 80.0)}), Selection);
		TestEqual(TEXT("Below minimum certified distance falls back to L0"), Selection.Cells[0].ActiveLevel, 0);
		TestTrue(
			TEXT("Minimum-distance fallback is diagnosed"),
			EnumHasAnyFlags(Selection.Cells[0].Diagnostics, EValidatedLODDiagnostic::BelowMinimumCertifiedDistance));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSValidatedLODMultiViewDomainTest,
		"NanoGS.Paged.ValidatedLOD.MultiViewCertificateDomainAndNearPlane",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSValidatedLODMultiViewDomainTest::RunTest(const FString& Parameters)
	{
		const FValidatedLODModel Model = MakeSingleCellModel(2);
		TSet<FValidatedLODPageKey> ReadyPages;
		AddAllPages(Model, ReadyPages);
		FValidatedLODSelector Selector;
		FString Error;
		TestTrue(TEXT("Selector initializes"), Selector.Initialize(Model, ImmediateSwitchSettings(), Error));

		FValidatedLODSelection Selection;
		Evaluate(*this, Selector, MakeRequest(0.0, ReadyPages, {Observe(1, 400.0)}), Selection);
		TestEqual(TEXT("Far view selects L1"), Selection.Cells[0].ActiveLevel, 1);

		FValidatedLODSelectionRequest MultiView = MakeRequest(1.0, ReadyPages, {Observe(1, 400.0)});
		FValidatedLODView& NearView = MultiView.Views.AddDefaulted_GetRef();
		NearView.ViewId = 200;
		NearView.CellObservations = {Observe(1, 190.0)};
		Evaluate(*this, Selector, MultiView, Selection);
		TestEqual(TEXT("Multiple views take the finest requirement"), Selection.Cells[0].ActiveLevel, 0);

		Evaluate(*this, Selector, MakeRequest(2.0, ReadyPages, {Observe(1, 400.0)}), Selection);
		TestEqual(TEXT("Far view restores L1"), Selection.Cells[0].ActiveLevel, 1);
		Evaluate(*this, Selector, MakeRequest(3.0, ReadyPages, {Observe(1, 400.0, false)}), Selection);
		TestEqual(TEXT("Outside certificate domain forces exact L0"), Selection.Cells[0].ActiveLevel, 0);
		TestTrue(
			TEXT("Certificate-domain fallback is diagnosed"),
			EnumHasAnyFlags(Selection.Cells[0].Diagnostics, EValidatedLODDiagnostic::CertificateDomainInvalid));

		Evaluate(*this, Selector, MakeRequest(4.0, ReadyPages, {Observe(1, 400.0)}), Selection);
		Evaluate(*this, Selector, MakeRequest(5.0, ReadyPages, {Observe(1, 400.0, true, true)}), Selection);
		TestEqual(TEXT("Near-plane intersection forces exact L0"), Selection.Cells[0].ActiveLevel, 0);
		TestTrue(
			TEXT("Near-plane fallback is diagnosed"),
			EnumHasAnyFlags(Selection.Cells[0].Diagnostics, EValidatedLODDiagnostic::NearPlaneFallback));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSValidatedLODReadinessTest,
		"NanoGS.Paged.ValidatedLOD.AtomicReadinessAndGeneration",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSValidatedLODReadinessTest::RunTest(const FString& Parameters)
	{
		const FValidatedLODModel Model = MakeSingleCellModel(2);
		TSet<FValidatedLODPageKey> AllReady;
		AddAllPages(Model, AllReady);
		FValidatedLODSelector Selector;
		FString Error;
		TestTrue(TEXT("Selector initializes"), Selector.Initialize(Model, ImmediateSwitchSettings(), Error));

		FValidatedLODSelection Selection;
		Evaluate(*this, Selector, MakeRequest(0.0, AllReady, {Observe(1, 400.0)}), Selection);
		TestEqual(TEXT("Setup commits complete L1"), Selection.Cells[0].ActiveLevel, 1);
		TestEqual(TEXT("Setup generation"), Selection.Generation, 1ull);

		TSet<FValidatedLODPageKey> PartialRefine = AllReady;
		const FValidatedLODPageKey MissingL0 = PageKey(1, 0, 1);
		PartialRefine.Remove(MissingL0);
		Evaluate(*this, Selector, MakeRequest(1.0, PartialRefine, {Observe(1, 190.0)}), Selection);
		TestEqual(TEXT("Partial L0 never replaces the complete old L1"), Selection.Cells[0].ActiveLevel, 1);
		TestTrue(TEXT("Old complete representation remains preview-ready"), Selection.bPreviewReady);
		TestFalse(TEXT("Required refine makes strict capture NotReady"), Selection.bStrictReady);
		TestFalse(TEXT("Incomplete refine does not advance generation"), Selection.bFrontierChanged);
		TestEqual(TEXT("Generation remains stable while waiting"), Selection.Generation, 1ull);
		TestEqual(TEXT("One strict NotReady diagnostic is emitted"), Selection.NotReadyDiagnostics.Num(), 1);
		TestTrue(
			TEXT("The exact missing target page is reported"),
			Selection.NotReadyDiagnostics[0].MissingPages.Contains(MissingL0));

		Evaluate(*this, Selector, MakeRequest(2.0, AllReady, {Observe(1, 190.0)}), Selection);
		TestEqual(TEXT("Complete L0 commits atomically"), Selection.Cells[0].ActiveLevel, 0);
		TestTrue(TEXT("Completed refine is strict-ready"), Selection.bStrictReady);
		TestEqual(TEXT("Completed refine advances generation once"), Selection.Generation, 2ull);

		TSet<FValidatedLODPageKey> PartialCoarsen = AllReady;
		const FValidatedLODPageKey MissingL1 = PageKey(1, 1, 1);
		PartialCoarsen.Remove(MissingL1);
		Evaluate(*this, Selector, MakeRequest(3.0, PartialCoarsen, {Observe(1, 400.0)}), Selection);
		TestEqual(TEXT("Incomplete coarsen keeps finer L0"), Selection.Cells[0].ActiveLevel, 0);
		TestTrue(TEXT("Finer old level remains quality-ready"), Selection.Cells[0].bQualityReady);
		TestTrue(TEXT("Deferred coarsen does not block strict capture"), Selection.bStrictReady);
		TestEqual(TEXT("Deferred coarsen does not advance generation"), Selection.Generation, 2ull);
		TestTrue(
			TEXT("Deferred coarsen is visible in diagnostics"),
			EnumHasAnyFlags(Selection.Cells[0].Diagnostics, EValidatedLODDiagnostic::CoarsenDeferred));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSValidatedLODSeamTest,
		"NanoGS.Paged.ValidatedLOD.SeamGroupFinestAndAtomicSwitch",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSValidatedLODSeamTest::RunTest(const FString& Parameters)
	{
		FValidatedLODModel Model;
		Model.Cells = {
			MakeCell(1, 2, 77),
			MakeCell(2, 2, 77),
		};
		TSet<FValidatedLODPageKey> AllReady;
		AddAllPages(Model, AllReady);
		FValidatedLODSelector Selector;
		FString Error;
		TestTrue(TEXT("Selector initializes"), Selector.Initialize(Model, ImmediateSwitchSettings(), Error));

		FValidatedLODSelection Selection;
		Evaluate(
			*this,
			Selector,
			MakeRequest(0.0, AllReady, {Observe(1, 400.0), Observe(2, 400.0)}),
			Selection);
		TestEqual(TEXT("Both seam cells coarsen together"), FindCell(Selection, 1)->ActiveLevel, 1);
		TestEqual(TEXT("Both seam cells share the same level"), FindCell(Selection, 2)->ActiveLevel, 1);

		TSet<FValidatedLODPageKey> MissingOneMember = AllReady;
		const FValidatedLODPageKey MissingPage = PageKey(2, 0, 1);
		MissingOneMember.Remove(MissingPage);
		Evaluate(
			*this,
			Selector,
			MakeRequest(1.0, MissingOneMember, {Observe(1, 190.0), Observe(2, 400.0)}),
			Selection);
		TestEqual(TEXT("Fine demand does not partially switch seam cell one"), FindCell(Selection, 1)->ActiveLevel, 1);
		TestEqual(TEXT("Missing member keeps the whole seam group old"), FindCell(Selection, 2)->ActiveLevel, 1);
		TestFalse(TEXT("Required seam refinement is strict NotReady"), Selection.bStrictReady);
		TestTrue(
			TEXT("Finest seam requirement is propagated to the far cell"),
			EnumHasAnyFlags(FindCell(Selection, 2)->Diagnostics, EValidatedLODDiagnostic::SeamGroupRefinement));
		TestTrue(
			TEXT("Group-level missing page is diagnosed on both atomic members"),
			FindCell(Selection, 1)->MissingPages.Contains(MissingPage));

		Evaluate(
			*this,
			Selector,
			MakeRequest(2.0, AllReady, {Observe(1, 190.0), Observe(2, 400.0)}),
			Selection);
		TestEqual(TEXT("Complete seam target commits cell one"), FindCell(Selection, 1)->ActiveLevel, 0);
		TestEqual(TEXT("Complete seam target commits cell two"), FindCell(Selection, 2)->ActiveLevel, 0);
		TestTrue(TEXT("Complete seam frontier is strict-ready"), Selection.bStrictReady);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSValidatedLODNeighborTest,
		"NanoGS.Paged.ValidatedLOD.NeighborDeltaRefinesCoarseEnd",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSValidatedLODNeighborTest::RunTest(const FString& Parameters)
	{
		FValidatedLODModel Model;
		Model.Cells = {
			MakeCell(1, 3, InvalidValidatedLODSeamGroupId, {2}),
			MakeCell(2, 3, InvalidValidatedLODSeamGroupId, {1, 3}),
			MakeCell(3, 3, InvalidValidatedLODSeamGroupId, {2}),
		};
		TSet<FValidatedLODPageKey> AllReady;
		AddAllPages(Model, AllReady);
		FValidatedLODSelector Selector;
		FString Error;
		TestTrue(TEXT("Selector initializes"), Selector.Initialize(Model, ImmediateSwitchSettings(), Error));

		FValidatedLODSelection Selection;
		Evaluate(
			*this,
			Selector,
			MakeRequest(0.0, AllReady, {Observe(1, 1000.0), Observe(2, 1000.0), Observe(3, 1000.0)}),
			Selection);
		TestEqual(TEXT("Setup coarsens cell one to L2"), FindCell(Selection, 1)->ActiveLevel, 2);
		TestEqual(TEXT("Setup coarsens cell two to L2"), FindCell(Selection, 2)->ActiveLevel, 2);
		TestEqual(TEXT("Setup coarsens cell three to L2"), FindCell(Selection, 3)->ActiveLevel, 2);

		TSet<FValidatedLODPageKey> MissingBridge = AllReady;
		MissingBridge.Remove(PageKey(2, 1, 1));
		Evaluate(
			*this,
			Selector,
			MakeRequest(1.0, MissingBridge, {Observe(1, 100.0), Observe(2, 1000.0), Observe(3, 1000.0)}),
			Selection);
		TestEqual(TEXT("Fine end waits when the L1 bridge is incomplete"), FindCell(Selection, 1)->ActiveLevel, 2);
		TestEqual(TEXT("Incomplete bridge stays on its complete L2"), FindCell(Selection, 2)->ActiveLevel, 2);
		TestFalse(TEXT("Blocked required refinement is strict NotReady"), Selection.bStrictReady);
		TestTrue(
			TEXT("Fine-end dependency failure is diagnosed"),
			EnumHasAnyFlags(
				FindCell(Selection, 1)->Diagnostics,
				EValidatedLODDiagnostic::NeighborTransitionBlocked));

		Evaluate(
			*this,
			Selector,
			MakeRequest(2.0, AllReady, {Observe(1, 100.0), Observe(2, 1000.0), Observe(3, 1000.0)}),
			Selection);
		TestEqual(TEXT("Near cell commits L0"), FindCell(Selection, 1)->ActiveLevel, 0);
		TestEqual(TEXT("Only coarse endpoint is refined to bridge L1"), FindCell(Selection, 2)->ActiveLevel, 1);
		TestEqual(TEXT("Far neighbor stays L2"), FindCell(Selection, 3)->ActiveLevel, 2);
		TestTrue(TEXT("Neighbor-safe frontier is strict-ready"), Selection.bStrictReady);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSValidatedLODSwitchIntervalTest,
		"NanoGS.Paged.ValidatedLOD.MinimumSwitchInterval",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSValidatedLODSwitchIntervalTest::RunTest(const FString& Parameters)
	{
		const FValidatedLODModel Model = MakeSingleCellModel(2);
		TSet<FValidatedLODPageKey> AllReady;
		AddAllPages(Model, AllReady);
		FValidatedLODSelector Selector;
		FValidatedLODSelectorSettings Settings;
		Settings.MinimumSwitchIntervalSeconds = 0.25;
		FString Error;
		TestTrue(TEXT("Selector initializes"), Selector.Initialize(Model, Settings, Error));

		FValidatedLODSelection Selection;
		Evaluate(*this, Selector, MakeRequest(0.0, AllReady, {Observe(1, 400.0)}), Selection);
		TestEqual(TEXT("Initial complete coarsen is allowed"), Selection.Cells[0].ActiveLevel, 1);
		TestEqual(TEXT("Initial switch generation"), Selection.Generation, 1ull);

		Evaluate(*this, Selector, MakeRequest(0.10, AllReady, {Observe(1, 190.0)}), Selection);
		TestEqual(TEXT("Early refine retains old complete level"), Selection.Cells[0].ActiveLevel, 1);
		TestFalse(TEXT("Rate-limited required refine is strict NotReady"), Selection.bStrictReady);
		TestEqual(TEXT("Rate-limited request does not advance generation"), Selection.Generation, 1ull);
		TestTrue(
			TEXT("Switch interval is diagnosed"),
			EnumHasAnyFlags(Selection.Cells[0].Diagnostics, EValidatedLODDiagnostic::MinimumSwitchInterval));

		Evaluate(*this, Selector, MakeRequest(0.25, AllReady, {Observe(1, 190.0)}), Selection);
		TestEqual(TEXT("Refine commits exactly at minimum interval"), Selection.Cells[0].ActiveLevel, 0);
		TestTrue(TEXT("Committed frontier becomes strict-ready"), Selection.bStrictReady);
		TestEqual(TEXT("Committed refine advances generation"), Selection.Generation, 2ull);
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
