// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace NanoGS::Paged
{
	enum class ESceneActiveBudgetLane : uint8
	{
		Main,
		Capture,
	};

	struct NANOGS_API FSceneBudgetLimits
	{
		uint64 PoolBytes = 0;
		uint64 ResidentSlots = 0;
		uint64 InFlightPages = 0;
		uint64 MainActiveSplats = 0;
		uint64 CaptureActiveSplats = 0;
	};

	struct NANOGS_API FSceneBudgetSnapshot
	{
		uint64 PoolBytes = 0;
		uint64 ResidentSlots = 0;
		uint64 InFlightPages = 0;
		uint64 MainActiveSplats = 0;
		uint64 CaptureActiveSplats = 0;
		uint32 OwnerCount = 0;
	};

	/**
	 * Thread-safe scene-level accounting for independently owned Page render data.
	 * A zero limit means unlimited. Failed reservations never modify existing state.
	 */
	class NANOGS_API FSceneBudgetRegistry final
	{
	public:
		static FSceneBudgetRegistry& Get();

		bool TrySetPoolBytes(uint64 SceneIdentity, uint64 OwnerIdentity, uint64 Bytes,
			uint64 Limit, FString& OutError);
		bool TrySetResidentSlots(uint64 SceneIdentity, uint64 OwnerIdentity, uint64 Slots,
			uint64 Limit, FString& OutError);
		bool TrySetActiveSplats(uint64 SceneIdentity, uint64 OwnerIdentity,
			ESceneActiveBudgetLane Lane, uint64 Splats, uint64 Limit, FString& OutError);
		bool TryAcquireInFlightPages(uint64 SceneIdentity, uint64 OwnerIdentity,
			uint64 Pages, uint64 Limit);
		void ReleaseInFlightPages(uint64 SceneIdentity, uint64 OwnerIdentity, uint64 Pages);
		void ReleaseOwner(uint64 SceneIdentity, uint64 OwnerIdentity);
		FSceneBudgetSnapshot GetSnapshot(uint64 SceneIdentity) const;

#if WITH_DEV_AUTOMATION_TESTS
		void ResetForTests();
#endif

	private:
		struct FSceneState
		{
			TMap<uint64, uint64> PoolBytesByOwner;
			TMap<uint64, uint64> ResidentSlotsByOwner;
			TMap<uint64, uint64> InFlightPagesByOwner;
			TMap<uint64, uint64> MainActiveSplatsByOwner;
			TMap<uint64, uint64> CaptureActiveSplatsByOwner;
			FSceneBudgetSnapshot Totals;
		};

		bool TrySetReservation(TMap<uint64, uint64>& Reservations, uint64 OwnerIdentity,
			uint64 Value, uint64 Limit, uint64& Total, const TCHAR* Label, FString& OutError);
		void RemoveSceneIfEmpty(uint64 SceneIdentity);

		mutable FCriticalSection Lock;
		TMap<uint64, FSceneState> Scenes;
	};
}
