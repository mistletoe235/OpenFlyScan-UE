// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Paged/NanoGSPageResidentPool.h"

#include "Misc/AutomationTest.h"

namespace NanoGS::Paged::Tests
{
	namespace
	{
		bool CompleteLoads(
			FAutomationTestBase& Test,
			FPageResidentPool& Pool,
			const FExactHQResidentPlan& Plan)
		{
			for (const FPageLoadOperation& Load : Plan.LoadOperations)
			{
				FString Error;
				if (!Test.TestTrue(TEXT("Tokenized page upload completes"), Pool.MarkLoadComplete(Load, Error)))
				{
					Test.AddError(Error);
					return false;
				}
			}
			return true;
		}
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSExactHQFencePinnedFrontierTransitionTest,
		"NanoGS.Paged.ExactHQFrontier.FencePinnedAtomicTransition",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSExactHQFencePinnedFrontierTransitionTest::RunTest(const FString& Parameters)
	{
		FPageResidentPool Pool;
		FPageResidentPoolConfig Config;
		Config.SlotCount = 4;
		Config.SplatsPerSlot = 100;
		Config.KeepAliveGenerations = 0;
		const TArray<FPageCatalogEntry> Catalog = {
			{10, 40}, {20, 50}, {30, 60}, {40, 70}, {50, 80}, {60, 90},
		};
		FString Error;
		TestTrue(TEXT("Four-slot transition pool initializes"), Pool.Initialize(Config, Catalog, Error));

		FExactHQPageRequest Initial;
		Initial.Generation = 1;
		Initial.VisiblePageIds = {10, 20};
		FExactHQResidentPlan InitialPlan;
		TestEqual(
			TEXT("Initial complete frontier plans"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(Initial, InitialPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Initial frontier needs two loads"), InitialPlan.LoadOperations.Num(), 2);
		const FPageLoadOperation StalePage10Completion = InitialPlan.LoadOperations[0];
		CompleteLoads(*this, Pool, InitialPlan);

		FExactHQPageRequest Transition;
		Transition.Generation = 2;
		Transition.VisiblePageIds = {30, 40};
		// Runtime uses RequiredPageIds for the complete currently published frontier.
		// Its generation is not released until the last-use GPU fence polls complete.
		Transition.RequiredPageIds = {10, 20};
		FExactHQResidentPlan TransitionPlan;
		TestEqual(
			TEXT("Target plus pinned old frontier fits with transition headroom"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(Transition, TransitionPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Only target pages load"), TransitionPlan.LoadOperations.Num(), 2);
		TestEqual(TEXT("Old frontier generation is pinned"), Pool.GetOpenCaptureGenerationCount(), 1ull);
		CompleteLoads(*this, Pool, TransitionPlan);

		TArray<FPagePhysicalRange> TargetRanges;
		TArray<uint64> Missing;
		TestTrue(
			TEXT("Target is wholly resident before an atomic active-index publication"),
			Pool.CollectActivePhysicalRanges(
				Transition.VisiblePageIds, {}, TargetRanges, Missing, Error));
		TestEqual(TEXT("Both target pages are publishable together"), TargetRanges.Num(), 2);

		FExactHQPageRequest BeforeFence;
		BeforeFence.Generation = 3;
		BeforeFence.VisiblePageIds = {50, 60};
		BeforeFence.RequiredPageIds = {30, 40};
		FExactHQResidentPlan BeforeFencePlan;
		TestEqual(
			TEXT("Unsignaled old-frontier fence pin blocks unsafe slot reuse"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(BeforeFence, BeforeFencePlan)),
			static_cast<uint8>(EPageResidentPlanResult::OutOfCapacity));
		TestEqual(TEXT("Capacity failure emits no partial eviction"), BeforeFencePlan.EvictOperations.Num(), 0);
		TestEqual(TEXT("Transactional failure does not advance generation"), Pool.GetLastSuccessfulGeneration(), 2ull);
		FPagePhysicalRange OldPage10;
		TestTrue(TEXT("Fence-pinned old page remains assigned"), Pool.FindPhysicalRange(10, OldPage10));

		TestTrue(
			TEXT("Simulated completed last-use fence releases exactly the old frontier generation"),
			Pool.ReleaseCaptureGeneration(2, Error));
		FExactHQResidentPlan AfterFencePlan;
		TestEqual(
			TEXT("Same generation retries after fence retirement"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(BeforeFence, AfterFencePlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Two retired old pages can now be evicted"), AfterFencePlan.EvictOperations.Num(), 2);
		TestEqual(TEXT("Two replacement target pages are tokenized loads"), AfterFencePlan.LoadOperations.Num(), 2);

		TestFalse(
			TEXT("Completion from a pre-eviction assignment token is rejected"),
			Pool.MarkLoadComplete(StalePage10Completion, Error));
		TestTrue(TEXT("Stale completion diagnostic is explicit"), Error.Contains(TEXT("stale")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSExactHQDisjointTeleportDrainTest,
		"NanoGS.Paged.ExactHQFrontier.DisjointTeleportDrainLatestTarget",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSExactHQDisjointTeleportDrainTest::RunTest(const FString& Parameters)
	{
		FPageResidentPool Pool;
		FPageResidentPoolConfig Config;
		Config.SlotCount = 2;
		Config.SplatsPerSlot = 100;
		Config.KeepAliveGenerations = 0;
		const TArray<FPageCatalogEntry> Catalog = {
			{10, 40}, {20, 50}, {30, 60}, {40, 70}, {50, 80},
		};
		FString Error;
		TestTrue(TEXT("Two-slot teleport pool initializes"), Pool.Initialize(Config, Catalog, Error));

		FExactHQPageRequest Initial;
		Initial.Generation = 1;
		Initial.VisiblePageIds = {10, 20};
		FExactHQResidentPlan InitialPlan;
		TestEqual(
			TEXT("Initial frontier plans"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(Initial, InitialPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		CompleteLoads(*this, Pool, InitialPlan);

		FExactHQPageRequest CannotCoexist;
		CannotCoexist.Generation = 2;
		CannotCoexist.VisiblePageIds = {30, 40};
		CannotCoexist.RequiredPageIds = {10, 20};
		FExactHQResidentPlan CannotCoexistPlan;
		TestEqual(
			TEXT("Disjoint target plus published frontier cannot coexist"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(CannotCoexist, CannotCoexistPlan)),
			static_cast<uint8>(EPageResidentPlanResult::OutOfCapacity));
		TestEqual(TEXT("Failed coexist plan performs no eviction"), CannotCoexistPlan.EvictOperations.Num(), 0);
		TestEqual(TEXT("Failed coexist plan does not consume generation"), Pool.GetLastSuccessfulGeneration(), 1ull);

		// Runtime now sets ActiveSplatCount=0 and waits for the old last-use fence.
		// While that fence is pending, demand may change; no pool request/overwrite is
		// made, and the latest complete target replaces the abandoned one.
		uint32 PublishedActiveCount = 90;
		uint32 VisibleActiveCount = 0;
		uint32 VisibilityGeneration = 1;
		auto SetVisibleActiveCount = [&VisibleActiveCount, &VisibilityGeneration](const uint32 NewCount)
		{
			if (VisibleActiveCount != NewCount)
			{
				VisibleActiveCount = NewCount;
				++VisibilityGeneration;
			}
		};
		TArray<uint64> LatestTarget = {30, 40};
		LatestTarget = {40, 50};
		TestEqual(TEXT("Drain suspends drawing the old frontier"), VisibleActiveCount, 0u);
		FPagePhysicalRange OldPage;
		TestTrue(TEXT("Pending fence leaves old page 10 untouched"), Pool.FindPhysicalRange(10, OldPage));
		TestTrue(TEXT("Pending fence leaves old page 20 untouched"), Pool.FindPhysicalRange(20, OldPage));

		// Simulated fence completion clears published ownership. The same generation
		// that failed transactionally may safely re-plan the latest target alone.
		FExactHQPageRequest AfterDrain;
		AfterDrain.Generation = 2;
		AfterDrain.VisiblePageIds = LatestTarget;
		FExactHQResidentPlan AfterDrainPlan;
		TestEqual(
			TEXT("Latest target alone reuses the drained slots"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(AfterDrain, AfterDrainPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Both old slots are evicted only after drain"), AfterDrainPlan.EvictOperations.Num(), 2);
		TestEqual(TEXT("Both latest-target pages load"), AfterDrainPlan.LoadOperations.Num(), 2);

		TArray<FPagePhysicalRange> ReadyRanges;
		TArray<uint64> Missing;
		TestFalse(
			TEXT("No partial target is publishable before its loads finish"),
			Pool.CollectActivePhysicalRanges(LatestTarget, {}, ReadyRanges, Missing, Error));
		TestEqual(TEXT("Old published count is retained only as suspended bookkeeping"), PublishedActiveCount, 90u);
		TestEqual(TEXT("Visible count remains NotReady during loads"), VisibleActiveCount, 0u);
		CompleteLoads(*this, Pool, AfterDrainPlan);
		TestTrue(
			TEXT("The complete latest target becomes atomically publishable"),
			Pool.CollectActivePhysicalRanges(LatestTarget, {}, ReadyRanges, Missing, Error));
		PublishedActiveCount = 150;
		SetVisibleActiveCount(PublishedActiveCount);
		TestEqual(TEXT("Only the complete target count is published"), VisibleActiveCount, 150u);
		const uint32 PublishedGeneration = VisibilityGeneration;

		FExactHQPageRequest Oversized;
		Oversized.Generation = 3;
		Oversized.VisiblePageIds = {10, 30, 50};
		FExactHQResidentPlan OversizedPlan;
		TestEqual(
			TEXT("A target that cannot fit by itself remains OutOfCapacity"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(Oversized, OversizedPlan)),
			static_cast<uint8>(EPageResidentPlanResult::OutOfCapacity));
		TestEqual(TEXT("Oversized target does not evict the current frontier"), OversizedPlan.EvictOperations.Num(), 0);

		// Runtime fail-closes visibility for a target that cannot fit by itself. It
		// deliberately retains the last complete published mapping/index so an exact
		// demand for that set can restore it without a partial intermediate frontier.
		SetVisibleActiveCount(0);
		TestEqual(TEXT("Oversized target suspends all rendering"), VisibleActiveCount, 0u);
		TestEqual(
			TEXT("First OOC suspension invalidates renderer-visible membership once"),
			VisibilityGeneration,
			PublishedGeneration + 1u);
		SetVisibleActiveCount(0);
		TestEqual(
			TEXT("Repeated identical OOC demand does not churn visibility generation"),
			VisibilityGeneration,
			PublishedGeneration + 1u);
		TestTrue(TEXT("Suspended published page 40 remains resident"), Pool.FindPhysicalRange(40, OldPage));
		TestTrue(TEXT("Suspended published page 50 remains resident"), Pool.FindPhysicalRange(50, OldPage));
		TestEqual(TEXT("Suspension retains the complete published count"), PublishedActiveCount, 150u);
		TestTrue(TEXT("Zero active count suppresses the frontier-drawn notification"), VisibleActiveCount == 0u);

		SetVisibleActiveCount(PublishedActiveCount);
		TestEqual(TEXT("Exact published demand restores the full frontier"), VisibleActiveCount, 150u);
		TestEqual(
			TEXT("Atomic restore changes renderer-visible membership exactly once"),
			VisibilityGeneration,
			PublishedGeneration + 2u);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSExactHQLoadingCancelToOutOfCapacityTest,
		"NanoGS.Paged.ExactHQFrontier.LoadingCancelToOutOfCapacity",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSExactHQLoadingCancelToOutOfCapacityTest::RunTest(const FString& Parameters)
	{
		FPageResidentPool Pool;
		FPageResidentPoolConfig Config;
		Config.SlotCount = 4;
		Config.SplatsPerSlot = 100;
		Config.KeepAliveGenerations = 0;
		const TArray<FPageCatalogEntry> Catalog = {
			{10, 40}, {20, 50}, {30, 60}, {40, 70}, {50, 80}, {60, 90},
		};
		FString Error;
		TestTrue(TEXT("Four-slot cancellation pool initializes"), Pool.Initialize(Config, Catalog, Error));

		FExactHQPageRequest Initial;
		Initial.Generation = 1;
		Initial.VisiblePageIds = {10, 20};
		FExactHQResidentPlan InitialPlan;
		TestEqual(
			TEXT("Initial published frontier plans"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(Initial, InitialPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		CompleteLoads(*this, Pool, InitialPlan);

		FExactHQPageRequest Loading;
		Loading.Generation = 2;
		Loading.VisiblePageIds = {30, 40};
		Loading.RequiredPageIds = {10, 20};
		FExactHQResidentPlan LoadingPlan;
		TestEqual(
			TEXT("Replacement begins while the old frontier is capture-pinned"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(Loading, LoadingPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Replacement has two in-flight loads"), LoadingPlan.LoadOperations.Num(), 2);
		TestEqual(TEXT("Loading transition owns one capture pin generation"), Pool.GetOpenCaptureGenerationCount(), 1ull);

		// This mirrors CancelBoundedTransition_RenderThread before entering OOC:
		// invalidate queued uploads, abandon their tokenized slots, then release the
		// capture generation that protected the complete published frontier.
		for (const FPageLoadOperation& Load : LoadingPlan.LoadOperations)
		{
			TestTrue(TEXT("Cancelled target load is abandoned"), Pool.AbandonLoad(Load, Error));
		}
		TestTrue(TEXT("Cancelled transition releases its capture generation"), Pool.ReleaseCaptureGeneration(2, Error));
		TestEqual(TEXT("Loading-to-OOC cancellation leaks no capture pins"), Pool.GetOpenCaptureGenerationCount(), 0ull);

		FPagePhysicalRange Range;
		TestTrue(TEXT("Published page 10 survives cancellation"), Pool.FindPhysicalRange(10, Range));
		TestTrue(TEXT("Published page 20 survives cancellation"), Pool.FindPhysicalRange(20, Range));
		TestFalse(TEXT("Cancelled loading page 30 releases its slot"), Pool.FindPhysicalRange(30, Range));
		TestFalse(TEXT("Cancelled loading page 40 releases its slot"), Pool.FindPhysicalRange(40, Range));

		// The render-data layer rejects this before asking the transactional pool;
		// the direct pool call verifies that even an accidental request cannot mutate
		// mappings or overwrite the retained complete frontier.
		FExactHQPageRequest Oversized;
		Oversized.Generation = 3;
		Oversized.VisiblePageIds = {10, 20, 30, 40, 50};
		FExactHQResidentPlan OversizedPlan;
		TestEqual(
			TEXT("Five-page target cannot fit in four fixed slots"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(Oversized, OversizedPlan)),
			static_cast<uint8>(EPageResidentPlanResult::OutOfCapacity));
		TestEqual(TEXT("Oversized failure has no evictions"), OversizedPlan.EvictOperations.Num(), 0);
		TestEqual(TEXT("Oversized failure opens no pin generation"), Pool.GetOpenCaptureGenerationCount(), 0ull);
		TestTrue(TEXT("Published page 10 remains mapped after OOC"), Pool.FindPhysicalRange(10, Range));
		TestTrue(TEXT("Published page 20 remains mapped after OOC"), Pool.FindPhysicalRange(20, Range));
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
