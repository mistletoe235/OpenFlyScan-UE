// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Paged/NanoGSSceneBudget.h"
#include "Misc/AutomationTest.h"

namespace NanoGS::Paged::Tests
{
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSSceneBudgetTransactionalTest,
		"NanoGS.Paged.SceneBudget.TransactionalReservations",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSSceneBudgetTransactionalTest::RunTest(const FString& Parameters)
	{
		FSceneBudgetRegistry& Registry = FSceneBudgetRegistry::Get();
		Registry.ResetForTests();
		FString Error;
		TestFalse(TEXT("Zero owner cannot acquire IO"), Registry.TryAcquireInFlightPages(99, 0, 1, 1));
		TestEqual(TEXT("Rejected first reservation leaves no owner"), Registry.GetSnapshot(99).OwnerCount, 0u);
		TestFalse(TEXT("Rejected first pool reservation is empty"), Registry.TrySetPoolBytes(98, 10, 101, 100, Error));
		TestEqual(TEXT("Rejected first pool reservation leaves no owner"), Registry.GetSnapshot(98).OwnerCount, 0u);
		TestTrue(TEXT("First owner reserves pool"), Registry.TrySetPoolBytes(1, 10, 60, 100, Error));
		TestFalse(TEXT("Second owner cannot exceed pool"), Registry.TrySetPoolBytes(1, 20, 50, 100, Error));
		TestEqual(TEXT("Failed reservation is transactional"), Registry.GetSnapshot(1).PoolBytes, 60ull);
		TestTrue(TEXT("Owner can shrink then peer reserve"), Registry.TrySetPoolBytes(1, 10, 40, 100, Error));
		TestTrue(TEXT("Peer reserve succeeds after shrink"), Registry.TrySetPoolBytes(1, 20, 60, 100, Error));
		TestEqual(TEXT("Pool reaches exact limit"), Registry.GetSnapshot(1).PoolBytes, 100ull);
		TestTrue(TEXT("First owner reserves resident slots"), Registry.TrySetResidentSlots(1, 10, 4, 6, Error));
		TestFalse(TEXT("Resident slot overage is rejected"), Registry.TrySetResidentSlots(1, 20, 3, 6, Error));
		TestEqual(TEXT("Failed resident reservation is transactional"), Registry.GetSnapshot(1).ResidentSlots, 4ull);
		TestTrue(TEXT("Another scene has an independent resident budget"), Registry.TrySetResidentSlots(2, 20, 6, 6, Error));
		TestEqual(TEXT("Other scene reaches its own limit"), Registry.GetSnapshot(2).ResidentSlots, 6ull);

		TestTrue(TEXT("Main active lane reserves"), Registry.TrySetActiveSplats(
			1, 10, ESceneActiveBudgetLane::Main, 70, 100, Error));
		TestTrue(TEXT("Capture lane has independent budget"), Registry.TrySetActiveSplats(
			1, 10, ESceneActiveBudgetLane::Capture, 90, 100, Error));
		TestFalse(TEXT("Peer main active overage rejected"), Registry.TrySetActiveSplats(
			1, 20, ESceneActiveBudgetLane::Main, 31, 100, Error));
		TestEqual(TEXT("Main total remains unchanged"), Registry.GetSnapshot(1).MainActiveSplats, 70ull);
		TestEqual(TEXT("Capture total remains independent"), Registry.GetSnapshot(1).CaptureActiveSplats, 90ull);

		TestTrue(TEXT("Acquire first IO pages"), Registry.TryAcquireInFlightPages(1, 10, 3, 4));
		TestFalse(TEXT("Global IO overage rejected"), Registry.TryAcquireInFlightPages(1, 20, 2, 4));
		Registry.ReleaseInFlightPages(1, 10, 2);
		TestTrue(TEXT("Released IO capacity is reusable"), Registry.TryAcquireInFlightPages(1, 20, 2, 4));
		TestEqual(TEXT("IO total is exact"), Registry.GetSnapshot(1).InFlightPages, 3ull);

		Registry.ReleaseOwner(1, 10);
		const FSceneBudgetSnapshot Remaining = Registry.GetSnapshot(1);
		TestEqual(TEXT("Owner release preserves peer pool"), Remaining.PoolBytes, 60ull);
		TestEqual(TEXT("Owner release clears its active lanes"), Remaining.MainActiveSplats, 0ull);
		TestEqual(TEXT("Owner release clears its capture lane"), Remaining.CaptureActiveSplats, 0ull);
		Registry.ReleaseOwner(1, 20);
		TestEqual(TEXT("Empty scene accounting is removed"), Registry.GetSnapshot(1).OwnerCount, 0u);
		TestEqual(TEXT("Other scene remains isolated"), Registry.GetSnapshot(2).ResidentSlots, 6ull);
		Registry.ReleaseOwner(2, 20);
		return true;
	}
}

#endif
