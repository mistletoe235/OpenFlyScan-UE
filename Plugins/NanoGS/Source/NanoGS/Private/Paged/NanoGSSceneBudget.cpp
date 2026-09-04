// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSSceneBudget.h"

namespace NanoGS::Paged
{
	FSceneBudgetRegistry& FSceneBudgetRegistry::Get()
	{
		static FSceneBudgetRegistry Instance;
		return Instance;
	}

	bool FSceneBudgetRegistry::TrySetReservation(
		TMap<uint64, uint64>& Reservations,
		const uint64 OwnerIdentity,
		const uint64 Value,
		const uint64 Limit,
		uint64& Total,
		const TCHAR* Label,
		FString& OutError)
	{
		OutError.Reset();
		if (OwnerIdentity == 0)
		{
			OutError = FString::Printf(TEXT("%s reservation requires a non-zero owner identity"), Label);
			return false;
		}
		const uint64 Previous = Reservations.FindRef(OwnerIdentity);
		if (Total < Previous)
		{
			OutError = FString::Printf(TEXT("%s reservation accounting underflow"), Label);
			return false;
		}
		const uint64 WithoutOwner = Total - Previous;
		if (Value > MAX_uint64 - WithoutOwner || (Limit != 0 && WithoutOwner + Value > Limit))
		{
			OutError = FString::Printf(
				TEXT("scene %s budget exceeded: requested=%llu existing-other=%llu limit=%llu"),
				Label,
				static_cast<unsigned long long>(Value),
				static_cast<unsigned long long>(WithoutOwner),
				static_cast<unsigned long long>(Limit));
			return false;
		}
		Total = WithoutOwner + Value;
		if (Value == 0)
		{
			Reservations.Remove(OwnerIdentity);
		}
		else
		{
			Reservations.Add(OwnerIdentity, Value);
		}
		return true;
	}

	bool FSceneBudgetRegistry::TrySetPoolBytes(
		const uint64 SceneIdentity, const uint64 OwnerIdentity, const uint64 Bytes,
		const uint64 Limit, FString& OutError)
	{
		FScopeLock Guard(&Lock);
		FSceneState& Scene = Scenes.FindOrAdd(SceneIdentity);
		const bool bReserved = TrySetReservation(Scene.PoolBytesByOwner, OwnerIdentity, Bytes, Limit,
			Scene.Totals.PoolBytes, TEXT("GPU-pool bytes"), OutError);
		if (!bReserved)
		{
			RemoveSceneIfEmpty(SceneIdentity);
		}
		return bReserved;
	}

	bool FSceneBudgetRegistry::TrySetResidentSlots(
		const uint64 SceneIdentity, const uint64 OwnerIdentity, const uint64 Slots,
		const uint64 Limit, FString& OutError)
	{
		FScopeLock Guard(&Lock);
		FSceneState& Scene = Scenes.FindOrAdd(SceneIdentity);
		const bool bReserved = TrySetReservation(Scene.ResidentSlotsByOwner, OwnerIdentity, Slots, Limit,
			Scene.Totals.ResidentSlots, TEXT("resident-slot"), OutError);
		if (!bReserved)
		{
			RemoveSceneIfEmpty(SceneIdentity);
		}
		return bReserved;
	}

	bool FSceneBudgetRegistry::TrySetActiveSplats(
		const uint64 SceneIdentity, const uint64 OwnerIdentity,
		const ESceneActiveBudgetLane Lane, const uint64 Splats,
		const uint64 Limit, FString& OutError)
	{
		FScopeLock Guard(&Lock);
		FSceneState& Scene = Scenes.FindOrAdd(SceneIdentity);
		const bool bReserved = Lane == ESceneActiveBudgetLane::Capture
			? TrySetReservation(Scene.CaptureActiveSplatsByOwner, OwnerIdentity, Splats, Limit,
				Scene.Totals.CaptureActiveSplats, TEXT("capture active-splat"), OutError)
			: TrySetReservation(Scene.MainActiveSplatsByOwner, OwnerIdentity, Splats, Limit,
				Scene.Totals.MainActiveSplats, TEXT("main active-splat"), OutError);
		if (!bReserved)
		{
			RemoveSceneIfEmpty(SceneIdentity);
		}
		return bReserved;
	}

	bool FSceneBudgetRegistry::TryAcquireInFlightPages(
		const uint64 SceneIdentity, const uint64 OwnerIdentity, const uint64 Pages, const uint64 Limit)
	{
		if (Pages == 0)
		{
			return true;
		}
		if (OwnerIdentity == 0)
		{
			return false;
		}
		FScopeLock Guard(&Lock);
		FSceneState& Scene = Scenes.FindOrAdd(SceneIdentity);
		const uint64 Previous = Scene.InFlightPagesByOwner.FindRef(OwnerIdentity);
		if (Scene.Totals.InFlightPages > MAX_uint64 - Pages ||
			(Limit != 0 && Scene.Totals.InFlightPages + Pages > Limit))
		{
			RemoveSceneIfEmpty(SceneIdentity);
			return false;
		}
		Scene.InFlightPagesByOwner.Add(OwnerIdentity, Previous + Pages);
		Scene.Totals.InFlightPages += Pages;
		return true;
	}

	void FSceneBudgetRegistry::ReleaseInFlightPages(
		const uint64 SceneIdentity, const uint64 OwnerIdentity, const uint64 Pages)
	{
		if (Pages == 0)
		{
			return;
		}
		FScopeLock Guard(&Lock);
		FSceneState* Scene = Scenes.Find(SceneIdentity);
		if (Scene == nullptr)
		{
			return;
		}
		uint64* Previous = Scene->InFlightPagesByOwner.Find(OwnerIdentity);
		if (Previous == nullptr)
		{
			return;
		}
		const uint64 Released = FMath::Min(*Previous, Pages);
		*Previous -= Released;
		Scene->Totals.InFlightPages -= Released;
		if (*Previous == 0)
		{
			Scene->InFlightPagesByOwner.Remove(OwnerIdentity);
		}
		RemoveSceneIfEmpty(SceneIdentity);
	}

	void FSceneBudgetRegistry::ReleaseOwner(const uint64 SceneIdentity, const uint64 OwnerIdentity)
	{
		FScopeLock Guard(&Lock);
		FSceneState* Scene = Scenes.Find(SceneIdentity);
		if (Scene == nullptr)
		{
			return;
		}
		auto Remove = [OwnerIdentity](TMap<uint64, uint64>& Values, uint64& Total)
		{
			const uint64 Previous = Values.FindRef(OwnerIdentity);
			Total = Total >= Previous ? Total - Previous : 0;
			Values.Remove(OwnerIdentity);
		};
		Remove(Scene->PoolBytesByOwner, Scene->Totals.PoolBytes);
		Remove(Scene->ResidentSlotsByOwner, Scene->Totals.ResidentSlots);
		Remove(Scene->InFlightPagesByOwner, Scene->Totals.InFlightPages);
		Remove(Scene->MainActiveSplatsByOwner, Scene->Totals.MainActiveSplats);
		Remove(Scene->CaptureActiveSplatsByOwner, Scene->Totals.CaptureActiveSplats);
		RemoveSceneIfEmpty(SceneIdentity);
	}

	FSceneBudgetSnapshot FSceneBudgetRegistry::GetSnapshot(const uint64 SceneIdentity) const
	{
		FScopeLock Guard(&Lock);
		const FSceneState* Scene = Scenes.Find(SceneIdentity);
		if (Scene == nullptr)
		{
			return FSceneBudgetSnapshot();
		}
		FSceneBudgetSnapshot Result = Scene->Totals;
		TSet<uint64> Owners;
		for (const TPair<uint64, uint64>& Pair : Scene->PoolBytesByOwner) Owners.Add(Pair.Key);
		for (const TPair<uint64, uint64>& Pair : Scene->ResidentSlotsByOwner) Owners.Add(Pair.Key);
		for (const TPair<uint64, uint64>& Pair : Scene->InFlightPagesByOwner) Owners.Add(Pair.Key);
		for (const TPair<uint64, uint64>& Pair : Scene->MainActiveSplatsByOwner) Owners.Add(Pair.Key);
		for (const TPair<uint64, uint64>& Pair : Scene->CaptureActiveSplatsByOwner) Owners.Add(Pair.Key);
		Result.OwnerCount = static_cast<uint32>(Owners.Num());
		return Result;
	}

	void FSceneBudgetRegistry::RemoveSceneIfEmpty(const uint64 SceneIdentity)
	{
		const FSceneState* Scene = Scenes.Find(SceneIdentity);
		if (Scene != nullptr && Scene->PoolBytesByOwner.IsEmpty() &&
			Scene->ResidentSlotsByOwner.IsEmpty() && Scene->InFlightPagesByOwner.IsEmpty() &&
			Scene->MainActiveSplatsByOwner.IsEmpty() && Scene->CaptureActiveSplatsByOwner.IsEmpty())
		{
			Scenes.Remove(SceneIdentity);
		}
	}

#if WITH_DEV_AUTOMATION_TESTS
	void FSceneBudgetRegistry::ResetForTests()
	{
		FScopeLock Guard(&Lock);
		Scenes.Reset();
	}
#endif
}
