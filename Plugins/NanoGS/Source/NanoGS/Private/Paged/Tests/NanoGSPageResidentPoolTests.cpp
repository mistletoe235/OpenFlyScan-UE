// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Paged/NanoGSPageResidentPool.h"

#include "Misc/AutomationTest.h"

namespace NanoGS::Paged::Tests
{
	namespace
	{
		TArray<FPageCatalogEntry> MakeCatalog()
		{
			return {
				{10, 40},
				{20, 60},
				{30, 70},
				{40, 80},
			};
		}

		bool CompleteAllLoads(
			FAutomationTestBase& Test,
			FPageResidentPool& Pool,
			const FExactHQResidentPlan& Plan)
		{
			bool bAllSucceeded = true;
			for (const FPageLoadOperation& Load : Plan.LoadOperations)
			{
				FString Error;
				const bool bSucceeded = Pool.MarkLoadComplete(Load, Error);
				Test.TestTrue(FString::Printf(TEXT("Load completion succeeds: %s"), *Error), bSucceeded);
				bAllSucceeded &= bSucceeded;
			}
			return bAllSucceeded;
		}

		const FPageRetainOperation* FindRetain(
			const FExactHQResidentPlan& Plan,
			const uint64 PageId)
		{
			return Plan.RetainOperations.FindByPredicate([PageId](const FPageRetainOperation& Retain)
			{
				return Retain.Range.PageId == PageId;
			});
		}
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageResidentStableMappingTest,
		"NanoGS.Paged.ResidentPool.StableMappingAndActiveRanges",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageResidentStableMappingTest::RunTest(const FString& Parameters)
	{
		FPageResidentPool Pool;
		FPageResidentPoolConfig Config;
		Config.SlotCount = 3;
		Config.SplatsPerSlot = 100;
		Config.KeepAliveGenerations = 2;
		const TArray<FPageCatalogEntry> Catalog = MakeCatalog();
		FString Error;
		TestTrue(TEXT("Pool initializes"), Pool.Initialize(Config, Catalog, Error));
		TestEqual(TEXT("Physical capacity is checked uint64 multiplication"), Pool.GetTotalPhysicalSplatCapacity(), 300ull);

		FExactHQPageRequest FirstRequest;
		FirstRequest.Generation = 1;
		FirstRequest.VisiblePageIds = {20, 10};
		FExactHQResidentPlan FirstPlan;
		TestEqual(
			TEXT("Initial request succeeds"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(FirstRequest, FirstPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Two deterministic loads are emitted"), FirstPlan.LoadOperations.Num(), 2);
		TestEqual(TEXT("Loading ranges are not active"), FirstPlan.ActivePhysicalRanges.Num(), 0);
		TestFalse(TEXT("Initial request waits for IO"), FirstPlan.bAllRequestedPagesResident);
		TestTrue(TEXT("Both initial loads complete"), CompleteAllLoads(*this, Pool, FirstPlan));

		FPagePhysicalRange Page10Before;
		FPagePhysicalRange Page20Before;
		TestTrue(TEXT("Page 10 has an assignment"), Pool.FindPhysicalRange(10, Page10Before));
		TestTrue(TEXT("Page 20 has an assignment"), Pool.FindPhysicalRange(20, Page20Before));
		TestEqual(TEXT("Sorted PageId gets deterministic slot zero"), Page10Before.SlotIndex, 0ull);
		TestEqual(TEXT("Page 20 gets slot one"), Page20Before.SlotIndex, 1ull);
		TestEqual(TEXT("Slot stride produces a stable physical offset"), Page20Before.FirstSplat, 100ull);

		TArray<FPagePhysicalRange> Active;
		TArray<uint64> Missing;
		TestTrue(
			TEXT("Completed requested pages are active"),
			Pool.CollectActivePhysicalRanges(FirstRequest.VisiblePageIds, {}, Active, Missing, Error));
		TestEqual(TEXT("Two active ranges returned"), Active.Num(), 2);
		TestEqual(TEXT("First range has exact page splat count"), Active[0].SplatCount, 40ull);
		TestEqual(TEXT("Second range has exact page splat count"), Active[1].SplatCount, 60ull);

		FExactHQPageRequest SecondRequest = FirstRequest;
		SecondRequest.Generation = 2;
		SecondRequest.VisiblePageIds = {10, 20};
		FExactHQResidentPlan SecondPlan;
		TestEqual(
			TEXT("Repeat request succeeds"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(SecondRequest, SecondPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Repeat request performs no loads"), SecondPlan.LoadOperations.Num(), 0);
		TestEqual(TEXT("Repeat request performs no evictions"), SecondPlan.EvictOperations.Num(), 0);
		TestEqual(TEXT("Both ranges are immediately active"), SecondPlan.ActivePhysicalRanges.Num(), 2);
		TestTrue(TEXT("Repeat request is ready"), SecondPlan.bAllRequestedPagesResident);

		FPagePhysicalRange Page10After;
		FPagePhysicalRange Page20After;
		Pool.FindPhysicalRange(10, Page10After);
		Pool.FindPhysicalRange(20, Page20After);
		TestEqual(TEXT("Page 10 keeps its slot"), Page10After.SlotIndex, Page10Before.SlotIndex);
		TestEqual(TEXT("Page 20 keeps its slot"), Page20After.SlotIndex, Page20Before.SlotIndex);
		TestEqual(TEXT("Page 10 keeps its assignment token"), Page10After.AssignmentToken, Page10Before.AssignmentToken);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageResidentPreferredLoadOrderTest,
		"NanoGS.Paged.ResidentPool.PreferredLoadOrder",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageResidentPreferredLoadOrderTest::RunTest(const FString& Parameters)
	{
		FPageResidentPool Pool;
		FPageResidentPoolConfig Config;
		Config.SlotCount = 4;
		Config.SplatsPerSlot = 100;
		const TArray<FPageCatalogEntry> Catalog = MakeCatalog();
		FString Error;
		TestTrue(TEXT("Pool initializes"), Pool.Initialize(Config, Catalog, Error));

		FExactHQPageRequest Request;
		Request.Generation = 1;
		Request.VisiblePageIds = {10, 20, 30, 40};
		Request.PreferredLoadPageIds = {40, 20};
		FExactHQResidentPlan Plan;
		TestEqual(
			TEXT("Priority request succeeds"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(Request, Plan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("All loads emitted"), Plan.LoadOperations.Num(), 4);
		if (Plan.LoadOperations.Num() == 4)
		{
			TestEqual(TEXT("First preferred page loads first"), Plan.LoadOperations[0].Destination.PageId, 40ull);
			TestEqual(TEXT("Second preferred page loads second"), Plan.LoadOperations[1].Destination.PageId, 20ull);
			TestEqual(TEXT("Unranked pages retain ascending order"), Plan.LoadOperations[2].Destination.PageId, 10ull);
			TestEqual(TEXT("Last unranked page"), Plan.LoadOperations[3].Destination.PageId, 30ull);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageResidentLRUTest,
		"NanoGS.Paged.ResidentPool.KeepAliveAndLRU",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageResidentLRUTest::RunTest(const FString& Parameters)
	{
		FPageResidentPool Pool;
		FPageResidentPoolConfig Config;
		Config.SlotCount = 3;
		Config.SplatsPerSlot = 100;
		Config.KeepAliveGenerations = 2;
		const TArray<FPageCatalogEntry> Catalog = MakeCatalog();
		FString Error;
		TestTrue(TEXT("Pool initializes"), Pool.Initialize(Config, Catalog, Error));

		auto ApplyAndComplete = [this, &Pool](const uint64 Generation, TArray<uint64> Visible, FExactHQResidentPlan& OutPlan)
		{
			FExactHQPageRequest Request;
			Request.Generation = Generation;
			Request.VisiblePageIds = MoveTemp(Visible);
			const EPageResidentPlanResult Result = Pool.ApplyExactHQRequest(Request, OutPlan);
			TestEqual(TEXT("LRU setup request succeeds"), static_cast<uint8>(Result), static_cast<uint8>(EPageResidentPlanResult::Success));
			return CompleteAllLoads(*this, Pool, OutPlan);
		};

		FExactHQResidentPlan Plan1;
		FExactHQResidentPlan Plan2;
		FExactHQResidentPlan Plan3;
		FExactHQResidentPlan Plan4;
		ApplyAndComplete(1, {10, 20}, Plan1);
		ApplyAndComplete(2, {10}, Plan2);
		const FPageRetainOperation* Page20Retain = FindRetain(Plan2, 20);
		TestNotNull(TEXT("Unrequested page remains while a spare slot exists"), Page20Retain);
		if (Page20Retain != nullptr)
		{
			TestTrue(
				TEXT("Young unrequested page is marked keepalive"),
				EnumHasAnyFlags(Page20Retain->Flags, EPageRetainFlags::KeepAlive));
		}

		ApplyAndComplete(3, {30}, Plan3);
		FPagePhysicalRange Page10Before;
		FPagePhysicalRange Page30Before;
		Pool.FindPhysicalRange(10, Page10Before);
		Pool.FindPhysicalRange(30, Page30Before);
		ApplyAndComplete(4, {40}, Plan4);
		TestEqual(TEXT("One victim is selected at full capacity"), Plan4.EvictOperations.Num(), 1);
		if (!Plan4.EvictOperations.IsEmpty())
		{
			TestEqual(TEXT("Expired oldest LRU page is evicted first"), Plan4.EvictOperations[0].PreviousRange.PageId, 20ull);
		}
		FPagePhysicalRange Page10After;
		FPagePhysicalRange Page30After;
		Pool.FindPhysicalRange(10, Page10After);
		Pool.FindPhysicalRange(30, Page30After);
		TestEqual(TEXT("Younger page 10 retains its stable slot"), Page10After.SlotIndex, Page10Before.SlotIndex);
		TestEqual(TEXT("Newest page 30 retains its stable slot"), Page30After.SlotIndex, Page30Before.SlotIndex);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageResidentCapturePinTest,
		"NanoGS.Paged.ResidentPool.CapturePinAndTransactionalCapacity",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageResidentCapturePinTest::RunTest(const FString& Parameters)
	{
		FPageResidentPool Pool;
		FPageResidentPoolConfig Config;
		Config.SlotCount = 1;
		Config.SplatsPerSlot = 100;
		const TArray<FPageCatalogEntry> Catalog = MakeCatalog();
		FString Error;
		TestTrue(TEXT("Pool initializes"), Pool.Initialize(Config, Catalog, Error));

		FExactHQPageRequest CaptureRequest;
		CaptureRequest.Generation = 1;
		CaptureRequest.VisiblePageIds = {10};
		CaptureRequest.RequiredPageIds = {10};
		FExactHQResidentPlan CapturePlan;
		TestEqual(
			TEXT("Capture request succeeds"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(CaptureRequest, CapturePlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		CompleteAllLoads(*this, Pool, CapturePlan);
		TestEqual(TEXT("One capture generation is open"), Pool.GetOpenCaptureGenerationCount(), 1ull);

		FExactHQPageRequest OverlappingCaptureRequest = CaptureRequest;
		OverlappingCaptureRequest.Generation = 2;
		FExactHQResidentPlan OverlappingCapturePlan;
		TestEqual(
			TEXT("A second capture generation can pin the same stable page"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(OverlappingCaptureRequest, OverlappingCapturePlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Two capture generations are independently open"), Pool.GetOpenCaptureGenerationCount(), 2ull);

		FPagePhysicalRange OriginalAssignment;
		Pool.FindPhysicalRange(10, OriginalAssignment);
		FExactHQPageRequest BlockedRequest;
		BlockedRequest.Generation = 3;
		BlockedRequest.VisiblePageIds = {20};
		FExactHQResidentPlan BlockedPlan;
		TestTrue(TEXT("First completed capture releases only its own generation"), Pool.ReleaseCaptureGeneration(1, Error));
		TestEqual(TEXT("Second capture generation remains open"), Pool.GetOpenCaptureGenerationCount(), 1ull);
		TestEqual(
			TEXT("Still-pinned slot returns explicit OutOfCapacity"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(BlockedRequest, BlockedPlan)),
			static_cast<uint8>(EPageResidentPlanResult::OutOfCapacity));
		TestEqual(TEXT("Failed plan emits no partial eviction"), BlockedPlan.EvictOperations.Num(), 0);
		TestEqual(TEXT("Failed plan does not advance generation"), Pool.GetLastSuccessfulGeneration(), 2ull);
		FPagePhysicalRange StillAssigned;
		TestTrue(TEXT("Pinned page remains mapped"), Pool.FindPhysicalRange(10, StillAssigned));
		TestEqual(TEXT("Pinned page retains exact assignment"), StillAssigned.AssignmentToken, OriginalAssignment.AssignmentToken);

		TestTrue(TEXT("Second completed capture releases its generation"), Pool.ReleaseCaptureGeneration(2, Error));
		FExactHQResidentPlan RetryPlan;
		TestEqual(
			TEXT("Same generation can be retried after transactional failure"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(BlockedRequest, RetryPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Unpinned page is now evicted"), RetryPlan.EvictOperations.Num(), 1);
		TestEqual(TEXT("Replacement page is loaded"), RetryPlan.LoadOperations.Num(), 1);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageResidentSafetyTest,
		"NanoGS.Paged.ResidentPool.OverflowBudgetAndLoadSafety",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageResidentSafetyTest::RunTest(const FString& Parameters)
	{
		FString Error;
		const TArray<FPageCatalogEntry> Catalog = MakeCatalog();

		FPageResidentPool OverflowPool;
		FPageResidentPoolConfig OverflowConfig;
		OverflowConfig.SlotCount = 2;
		OverflowConfig.SplatsPerSlot = MAX_uint64;
		TestFalse(TEXT("uint64 physical-capacity overflow is rejected"), OverflowPool.Initialize(OverflowConfig, Catalog, Error));
		TestTrue(TEXT("Overflow error is explicit"), Error.Contains(TEXT("overflows uint64")));

		FPageResidentPool OversizedPagePool;
		FPageResidentPoolConfig SmallSlotConfig;
		SmallSlotConfig.SlotCount = 2;
		SmallSlotConfig.SplatsPerSlot = 50;
		TestFalse(TEXT("Page larger than a fixed slot is rejected"), OversizedPagePool.Initialize(SmallSlotConfig, Catalog, Error));

		FPageResidentPool Pool;
		FPageResidentPoolConfig Config;
		Config.SlotCount = 2;
		Config.SplatsPerSlot = 100;
		TestTrue(TEXT("Safety-test pool initializes"), Pool.Initialize(Config, Catalog, Error));

		FExactHQPageRequest OverBudget;
		OverBudget.Generation = 1;
		OverBudget.VisiblePageIds = {10, 20};
		OverBudget.RequiredPageIds = {30};
		FExactHQResidentPlan OverBudgetPlan;
		TestEqual(
			TEXT("Visible/required union over fixed budget is explicit OutOfCapacity"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(OverBudget, OverBudgetPlan)),
			static_cast<uint8>(EPageResidentPlanResult::OutOfCapacity));
		TestEqual(TEXT("Over-budget failure does not occupy slots"), OverBudgetPlan.OccupiedSlotCount, 0ull);

		FExactHQPageRequest First;
		First.Generation = 1;
		First.VisiblePageIds = {10, 20};
		FExactHQResidentPlan FirstPlan;
		TestEqual(
			TEXT("Two-page request succeeds"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(First, FirstPlan)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("Both slots are loading"), FirstPlan.LoadingSlotCount, 2ull);

		FExactHQPageRequest WhileLoading;
		WhileLoading.Generation = 2;
		WhileLoading.VisiblePageIds = {30};
		FExactHQResidentPlan WhileLoadingPlan;
		TestEqual(
			TEXT("In-flight loads cannot be evicted"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(WhileLoading, WhileLoadingPlan)),
			static_cast<uint8>(EPageResidentPlanResult::OutOfCapacity));

		const FPageLoadOperation StaleLoad = FirstPlan.LoadOperations[0];
		CompleteAllLoads(*this, Pool, FirstPlan);
		FExactHQResidentPlan Retry;
		TestEqual(
			TEXT("Request succeeds after IO completes"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(WhileLoading, Retry)),
			static_cast<uint8>(EPageResidentPlanResult::Success));
		TestEqual(TEXT("One LRU page is evicted"), Retry.EvictOperations.Num(), 1);
		TestFalse(TEXT("Stale completion token is rejected after slot reuse"), Pool.MarkLoadComplete(StaleLoad, Error));

		FExactHQPageRequest Unknown;
		Unknown.Generation = 3;
		Unknown.VisiblePageIds = {999};
		FExactHQResidentPlan UnknownPlan;
		TestEqual(
			TEXT("Unknown PageId is explicit"),
			static_cast<uint8>(Pool.ApplyExactHQRequest(Unknown, UnknownPlan)),
			static_cast<uint8>(EPageResidentPlanResult::UnknownPage));
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
