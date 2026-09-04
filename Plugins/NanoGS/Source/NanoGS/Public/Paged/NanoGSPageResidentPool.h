// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Renderer-independent CPU state machine for a fixed-slot NanoGS Page v1 pool.
 *
 * This class deliberately does not perform file IO or allocate GPU resources. It
 * gives an eventual renderer a transactional Exact-HQ residency plan and stable
 * physical destinations. A load operation must be acknowledged with
 * MarkLoadComplete before its range is returned as active.
 */
namespace NanoGS::Paged
{
	enum class EPageSlotState : uint8
	{
		Empty,
		Loading,
		Resident,
	};

	enum class EPageResidentPlanResult : uint8
	{
		Success,
		NotInitialized,
		InvalidRequest,
		UnknownPage,
		OutOfCapacity,
		ArithmeticOverflow,
	};

	enum class EPageRetainFlags : uint8
	{
		None = 0,
		Visible = 1u << 0u,
		Required = 1u << 1u,
		KeepAlive = 1u << 2u,
		CapturePinned = 1u << 3u,
		LoadInFlight = 1u << 4u,
		SpareCapacity = 1u << 5u,
	};

	ENUM_CLASS_FLAGS(EPageRetainFlags);

	struct NANOGS_API FPageResidentPoolConfig
	{
		/** The pool never grows beyond this many physical slots. */
		uint64 SlotCount = 0;

		/** Fixed address stride and maximum splat count of one Page v1 payload. */
		uint64 SplatsPerSlot = 0;

		/** Unrequested pages younger than this are preferred over old LRU victims. */
		uint64 KeepAliveGenerations = 2;
	};

	struct NANOGS_API FPageCatalogEntry
	{
		uint64 PageId = 0;
		uint64 SplatCount = 0;
	};

	struct NANOGS_API FExactHQPageRequest
	{
		/** Strictly increasing successful planner generation; zero is invalid. */
		uint64 Generation = 0;

		/** Every visible page is mandatory in Exact-HQ; no partial subset is returned. */
		TArray<uint64> VisiblePageIds;

		/** Mandatory pages additionally pinned until ReleaseCaptureGeneration. */
		TArray<uint64> RequiredPageIds;

		/** Optional deterministic load priority; omitted pages retain ascending PageId order. */
		TArray<uint64> PreferredLoadPageIds;
	};

	struct NANOGS_API FPagePhysicalRange
	{
		uint64 PageId = 0;
		uint64 SlotIndex = 0;
		uint64 FirstSplat = 0;
		uint64 SplatCount = 0;
		uint64 AssignmentToken = 0;
		EPageSlotState State = EPageSlotState::Empty;
	};

	struct NANOGS_API FPageLoadOperation
	{
		FPagePhysicalRange Destination;
	};

	struct NANOGS_API FPageRetainOperation
	{
		FPagePhysicalRange Range;
		EPageRetainFlags Flags = EPageRetainFlags::None;
	};

	struct NANOGS_API FPageEvictOperation
	{
		FPagePhysicalRange PreviousRange;
	};

	struct NANOGS_API FExactHQResidentPlan
	{
		EPageResidentPlanResult Result = EPageResidentPlanResult::InvalidRequest;
		FString Error;
		uint64 Generation = 0;

		TArray<FPageLoadOperation> LoadOperations;
		TArray<FPageRetainOperation> RetainOperations;
		TArray<FPageEvictOperation> EvictOperations;

		/** Final destinations for visible U required pages, including in-flight loads. */
		TArray<FPagePhysicalRange> PlannedPhysicalRanges;

		/** Requested ranges safe to consume now. Loading pages are intentionally absent. */
		TArray<FPagePhysicalRange> ActivePhysicalRanges;

		uint64 RequestedPageCount = 0;
		uint64 FixedSlotCount = 0;
		uint64 OccupiedSlotCount = 0;
		uint64 PinnedSlotCount = 0;
		uint64 LoadingSlotCount = 0;
		bool bAllRequestedPagesResident = false;
	};

	class NANOGS_API FPageResidentPool
	{
	public:
		/**
		 * Replace the current pool with a validated fixed-size pool and immutable page catalog.
		 * Failure leaves the previous pool untouched.
		 */
		bool Initialize(
			const FPageResidentPoolConfig& InConfig,
			TConstArrayView<FPageCatalogEntry> InCatalog,
			FString& OutError)
		{
			OutError.Reset();
			if (InConfig.SlotCount == 0 || InConfig.SplatsPerSlot == 0)
			{
				OutError = TEXT("SlotCount and SplatsPerSlot must both be non-zero");
				return false;
			}
			if (InConfig.SlotCount > static_cast<uint64>(MAX_int32))
			{
				OutError = TEXT("SlotCount exceeds the TArray/int32 addressable limit");
				return false;
			}

			uint64 TotalPhysicalSplats = 0;
			if (!TryMultiply(InConfig.SlotCount, InConfig.SplatsPerSlot, TotalPhysicalSplats))
			{
				OutError = TEXT("Fixed physical pool size overflows uint64");
				return false;
			}

			TMap<uint64, uint64> NewCatalog;
			NewCatalog.Reserve(InCatalog.Num());
			for (const FPageCatalogEntry& Entry : InCatalog)
			{
				if (Entry.SplatCount == 0)
				{
					OutError = TEXT("Page catalog contains a zero-splat page");
					return false;
				}
				if (Entry.SplatCount > InConfig.SplatsPerSlot)
				{
					OutError = TEXT("Page catalog contains a page larger than one fixed slot");
					return false;
				}
				if (NewCatalog.Contains(Entry.PageId))
				{
					OutError = TEXT("Page catalog contains a duplicate PageId");
					return false;
				}
				NewCatalog.Add(Entry.PageId, Entry.SplatCount);
			}

			TArray<FSlot> NewSlots;
			NewSlots.SetNum(static_cast<int32>(InConfig.SlotCount));

			Config = InConfig;
			Catalog = MoveTemp(NewCatalog);
			Slots = MoveTemp(NewSlots);
			PageToSlot.Reset();
			OpenCaptureGenerations.Reset();
			TotalPhysicalSplatCapacity = TotalPhysicalSplats;
			LastSuccessfulGeneration = 0;
			NextAssignmentToken = 1;
			bInitialized = true;
			return true;
		}

		void Reset()
		{
			Config = FPageResidentPoolConfig();
			Catalog.Reset();
			Slots.Reset();
			PageToSlot.Reset();
			OpenCaptureGenerations.Reset();
			TotalPhysicalSplatCapacity = 0;
			LastSuccessfulGeneration = 0;
			NextAssignmentToken = 1;
			bInitialized = false;
		}

		/**
		 * Atomically apply an Exact-HQ request. On every non-Success result, pool mappings,
		 * pins, LRU ages, and the successful-generation counter remain unchanged.
		 */
		EPageResidentPlanResult ApplyExactHQRequest(
			const FExactHQPageRequest& Request,
			FExactHQResidentPlan& OutPlan)
		{
			OutPlan = FExactHQResidentPlan();
			OutPlan.Generation = Request.Generation;
			OutPlan.FixedSlotCount = static_cast<uint64>(Slots.Num());

			auto Fail = [&OutPlan](const EPageResidentPlanResult Result, const TCHAR* Message)
			{
				OutPlan.Result = Result;
				OutPlan.Error = Message;
				return Result;
			};

			if (!bInitialized)
			{
				return Fail(EPageResidentPlanResult::NotInitialized, TEXT("Resident pool is not initialized"));
			}
			OutPlan.OccupiedSlotCount = GetOccupiedSlotCount();
			OutPlan.PinnedSlotCount = GetPinnedSlotCount();
			OutPlan.LoadingSlotCount = GetLoadingSlotCount();
			if (Request.Generation == 0 || Request.Generation <= LastSuccessfulGeneration)
			{
				return Fail(
					EPageResidentPlanResult::InvalidRequest,
					TEXT("Request generation must be non-zero and strictly newer than the last successful request"));
			}

			TSet<uint64> VisibleSet;
			TSet<uint64> RequiredSet;
			TSet<uint64> DesiredSet;
			VisibleSet.Reserve(Request.VisiblePageIds.Num());
			RequiredSet.Reserve(Request.RequiredPageIds.Num());
			DesiredSet.Reserve(Request.VisiblePageIds.Num() + Request.RequiredPageIds.Num());

			auto ValidateIds = [this, &DesiredSet, &OutPlan](
				const TArray<uint64>& Ids,
				TSet<uint64>& Destination,
				const TCHAR* SetName)
			{
				for (const uint64 PageId : Ids)
				{
					if (Destination.Contains(PageId))
					{
						OutPlan.Result = EPageResidentPlanResult::InvalidRequest;
						OutPlan.Error = FString::Printf(TEXT("%s contains a duplicate PageId"), SetName);
						return false;
					}
					if (!Catalog.Contains(PageId))
					{
						OutPlan.Result = EPageResidentPlanResult::UnknownPage;
						OutPlan.Error = FString::Printf(TEXT("%s references an unknown PageId"), SetName);
						return false;
					}
					Destination.Add(PageId);
					DesiredSet.Add(PageId);
				}
				return true;
			};

			if (!ValidateIds(Request.VisiblePageIds, VisibleSet, TEXT("VisiblePageIds")) ||
				!ValidateIds(Request.RequiredPageIds, RequiredSet, TEXT("RequiredPageIds")))
			{
				return OutPlan.Result;
			}

			OutPlan.RequestedPageCount = static_cast<uint64>(DesiredSet.Num());
			if (OutPlan.RequestedPageCount > Config.SlotCount)
			{
				return Fail(
					EPageResidentPlanResult::OutOfCapacity,
					TEXT("Exact-HQ visible/required union exceeds the fixed slot budget"));
			}

			TArray<uint64> PagesToLoad;
			PagesToLoad.Reserve(DesiredSet.Num());
			for (const uint64 PageId : DesiredSet)
			{
				if (!PageToSlot.Contains(PageId))
				{
					PagesToLoad.Add(PageId);
				}
			}
			PagesToLoad.Sort();
			if (!Request.PreferredLoadPageIds.IsEmpty())
			{
				TMap<uint64, int32> PreferredRanks;
				PreferredRanks.Reserve(Request.PreferredLoadPageIds.Num());
				for (int32 PriorityIndex = 0; PriorityIndex < Request.PreferredLoadPageIds.Num(); ++PriorityIndex)
				{
					PreferredRanks.FindOrAdd(Request.PreferredLoadPageIds[PriorityIndex], PriorityIndex);
				}
				PagesToLoad.Sort([&PreferredRanks](const uint64 A, const uint64 B)
				{
					const int32* RankA = PreferredRanks.Find(A);
					const int32* RankB = PreferredRanks.Find(B);
					const int32 EffectiveRankA = RankA != nullptr ? *RankA : MAX_int32;
					const int32 EffectiveRankB = RankB != nullptr ? *RankB : MAX_int32;
					return EffectiveRankA != EffectiveRankB ? EffectiveRankA < EffectiveRankB : A < B;
				});
			}

			TArray<int32> EmptySlots;
			TArray<int32> VictimSlots;
			EmptySlots.Reserve(Slots.Num());
			VictimSlots.Reserve(Slots.Num());
			for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
			{
				const FSlot& Slot = Slots[SlotIndex];
				if (Slot.State == EPageSlotState::Empty)
				{
					EmptySlots.Add(SlotIndex);
					continue;
				}

				if (!DesiredSet.Contains(Slot.PageId) &&
					Slot.State == EPageSlotState::Resident &&
					Slot.PinGenerations.IsEmpty())
				{
					VictimSlots.Add(SlotIndex);
				}
			}

			VictimSlots.Sort([this, &Request](const int32 A, const int32 B)
			{
				const FSlot& SlotA = Slots[A];
				const FSlot& SlotB = Slots[B];
				const uint64 AgeA = Request.Generation - SlotA.LastRequestedGeneration;
				const uint64 AgeB = Request.Generation - SlotB.LastRequestedGeneration;
				const bool bExpiredA = AgeA > Config.KeepAliveGenerations;
				const bool bExpiredB = AgeB > Config.KeepAliveGenerations;
				if (bExpiredA != bExpiredB)
				{
					return bExpiredA;
				}
				if (SlotA.LastRequestedGeneration != SlotB.LastRequestedGeneration)
				{
					return SlotA.LastRequestedGeneration < SlotB.LastRequestedGeneration;
				}
				return A < B;
			});

			const uint64 ImmediatelyAvailable =
				static_cast<uint64>(EmptySlots.Num()) + static_cast<uint64>(VictimSlots.Num());
			if (static_cast<uint64>(PagesToLoad.Num()) > ImmediatelyAvailable)
			{
				return Fail(
					EPageResidentPlanResult::OutOfCapacity,
					TEXT("Exact-HQ request cannot fit because remaining slots are capture-pinned or loading"));
			}

			// Keep MAX_uint64 unused so incrementing the allocator can never wrap to token zero.
			if (static_cast<uint64>(PagesToLoad.Num()) > MAX_uint64 - NextAssignmentToken)
			{
				return Fail(
					EPageResidentPlanResult::ArithmeticOverflow,
					TEXT("Assignment token space is exhausted"));
			}

			TArray<int32> AssignmentSlots = EmptySlots;
			const int32 VictimsNeeded = PagesToLoad.Num() - FMath::Min(PagesToLoad.Num(), EmptySlots.Num());
			for (int32 VictimIndex = 0; VictimIndex < VictimsNeeded; ++VictimIndex)
			{
				AssignmentSlots.Add(VictimSlots[VictimIndex]);
			}
			AssignmentSlots.SetNum(PagesToLoad.Num(), EAllowShrinking::No);

			TSet<int32> NewlyAssignedSlotSet;
			NewlyAssignedSlotSet.Reserve(PagesToLoad.Num());

			for (int32 AssignmentIndex = 0; AssignmentIndex < PagesToLoad.Num(); ++AssignmentIndex)
			{
				const int32 SlotIndex = AssignmentSlots[AssignmentIndex];
				FSlot& Slot = Slots[SlotIndex];
				if (Slot.State != EPageSlotState::Empty)
				{
					FPageEvictOperation& Evict = OutPlan.EvictOperations.AddDefaulted_GetRef();
					Evict.PreviousRange = MakeRange(SlotIndex);
					PageToSlot.Remove(Slot.PageId);
					Slot = FSlot();
				}

				const uint64 PageId = PagesToLoad[AssignmentIndex];
				const uint64* SplatCount = Catalog.Find(PageId);
				check(SplatCount != nullptr);
				Slot.PageId = PageId;
				Slot.SplatCount = *SplatCount;
				Slot.AssignmentToken = NextAssignmentToken++;
				Slot.LastRequestedGeneration = Request.Generation;
				Slot.State = EPageSlotState::Loading;
				PageToSlot.Add(PageId, SlotIndex);
				NewlyAssignedSlotSet.Add(SlotIndex);

				FPageLoadOperation& Load = OutPlan.LoadOperations.AddDefaulted_GetRef();
				Load.Destination = MakeRange(SlotIndex);
			}

			for (const uint64 PageId : DesiredSet)
			{
				const int32* SlotIndex = PageToSlot.Find(PageId);
				check(SlotIndex != nullptr);
				Slots[*SlotIndex].LastRequestedGeneration = Request.Generation;
			}

			if (!RequiredSet.IsEmpty())
			{
				OpenCaptureGenerations.Add(Request.Generation);
				for (const uint64 PageId : RequiredSet)
				{
					const int32* SlotIndex = PageToSlot.Find(PageId);
					check(SlotIndex != nullptr);
					Slots[*SlotIndex].PinGenerations.Add(Request.Generation);
				}
			}

			for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
			{
				const FSlot& Slot = Slots[SlotIndex];
				if (Slot.State == EPageSlotState::Empty || NewlyAssignedSlotSet.Contains(SlotIndex))
				{
					continue;
				}

				FPageRetainOperation& Retain = OutPlan.RetainOperations.AddDefaulted_GetRef();
				Retain.Range = MakeRange(SlotIndex);
				if (VisibleSet.Contains(Slot.PageId))
				{
					Retain.Flags |= EPageRetainFlags::Visible;
				}
				if (RequiredSet.Contains(Slot.PageId))
				{
					Retain.Flags |= EPageRetainFlags::Required;
				}
				if (!Slot.PinGenerations.IsEmpty())
				{
					Retain.Flags |= EPageRetainFlags::CapturePinned;
				}
				if (Slot.State == EPageSlotState::Loading)
				{
					Retain.Flags |= EPageRetainFlags::LoadInFlight;
				}
				if (!DesiredSet.Contains(Slot.PageId))
				{
					const uint64 Age = Request.Generation - Slot.LastRequestedGeneration;
					Retain.Flags |= Age <= Config.KeepAliveGenerations
						? EPageRetainFlags::KeepAlive
						: EPageRetainFlags::SpareCapacity;
				}
			}

			for (const uint64 PageId : DesiredSet)
			{
				const int32* SlotIndex = PageToSlot.Find(PageId);
				check(SlotIndex != nullptr);
				const FPagePhysicalRange Range = MakeRange(*SlotIndex);
				OutPlan.PlannedPhysicalRanges.Add(Range);
				if (Range.State == EPageSlotState::Resident)
				{
					OutPlan.ActivePhysicalRanges.Add(Range);
				}
			}
			SortRangesByPhysicalAddress(OutPlan.PlannedPhysicalRanges);
			SortRangesByPhysicalAddress(OutPlan.ActivePhysicalRanges);
			OutPlan.bAllRequestedPagesResident =
				OutPlan.ActivePhysicalRanges.Num() == DesiredSet.Num();
			OutPlan.OccupiedSlotCount = GetOccupiedSlotCount();
			OutPlan.PinnedSlotCount = GetPinnedSlotCount();
			OutPlan.LoadingSlotCount = GetLoadingSlotCount();
			OutPlan.Result = EPageResidentPlanResult::Success;
			OutPlan.Error.Reset();
			LastSuccessfulGeneration = Request.Generation;
			return OutPlan.Result;
		}

		/** Mark exactly one tokenized destination ready; stale IO completions are rejected. */
		bool MarkLoadComplete(const FPageLoadOperation& Operation, FString& OutError)
		{
			OutError.Reset();
			int32 SlotIndex = INDEX_NONE;
			if (!ValidateCurrentAssignment(Operation.Destination, EPageSlotState::Loading, SlotIndex, OutError))
			{
				return false;
			}
			Slots[SlotIndex].State = EPageSlotState::Resident;
			return true;
		}

		/** Drop a failed in-flight load only after every capture pin on it has been released. */
		bool AbandonLoad(const FPageLoadOperation& Operation, FString& OutError)
		{
			OutError.Reset();
			int32 SlotIndex = INDEX_NONE;
			if (!ValidateCurrentAssignment(Operation.Destination, EPageSlotState::Loading, SlotIndex, OutError))
			{
				return false;
			}
			FSlot& Slot = Slots[SlotIndex];
			if (!Slot.PinGenerations.IsEmpty())
			{
				OutError = TEXT("Cannot abandon a load while an unfinished capture generation pins it");
				return false;
			}
			PageToSlot.Remove(Slot.PageId);
			Slot = FSlot();
			return true;
		}

		/** Release all page pins owned by a completed or cancelled capture generation. */
		bool ReleaseCaptureGeneration(const uint64 Generation, FString& OutError)
		{
			OutError.Reset();
			if (!bInitialized)
			{
				OutError = TEXT("Resident pool is not initialized");
				return false;
			}
			if (!OpenCaptureGenerations.Contains(Generation))
			{
				OutError = TEXT("Capture generation is not open or was already released");
				return false;
			}
			for (FSlot& Slot : Slots)
			{
				Slot.PinGenerations.Remove(Generation);
			}
			OpenCaptureGenerations.Remove(Generation);
			return true;
		}

		/** Query the current assignment even while its load is in flight. */
		bool FindPhysicalRange(const uint64 PageId, FPagePhysicalRange& OutRange) const
		{
			const int32* SlotIndex = PageToSlot.Find(PageId);
			if (SlotIndex == nullptr)
			{
				OutRange = FPagePhysicalRange();
				return false;
			}
			OutRange = MakeRange(*SlotIndex);
			return true;
		}

		/**
		 * Return only resident ranges from the requested ID union and report loading/missing
		 * IDs separately. This is the post-IO counterpart of a plan's ActivePhysicalRanges.
		 */
		bool CollectActivePhysicalRanges(
			TConstArrayView<uint64> VisiblePageIds,
			TConstArrayView<uint64> RequiredPageIds,
			TArray<FPagePhysicalRange>& OutActiveRanges,
			TArray<uint64>& OutNotResidentPageIds,
			FString& OutError) const
		{
			OutActiveRanges.Reset();
			OutNotResidentPageIds.Reset();
			OutError.Reset();
			if (!bInitialized)
			{
				OutError = TEXT("Resident pool is not initialized");
				return false;
			}

			TSet<uint64> Desired;
			Desired.Reserve(VisiblePageIds.Num() + RequiredPageIds.Num());
			auto AddIds = [this, &Desired, &OutError](TConstArrayView<uint64> Ids)
			{
				for (const uint64 PageId : Ids)
				{
					if (!Catalog.Contains(PageId))
					{
						OutError = TEXT("Active-range query references an unknown PageId");
						return false;
					}
					Desired.Add(PageId);
				}
				return true;
			};
			if (!AddIds(VisiblePageIds) || !AddIds(RequiredPageIds))
			{
				return false;
			}

			for (const uint64 PageId : Desired)
			{
				const int32* SlotIndex = PageToSlot.Find(PageId);
				if (SlotIndex == nullptr || Slots[*SlotIndex].State != EPageSlotState::Resident)
				{
					OutNotResidentPageIds.Add(PageId);
					continue;
				}
				OutActiveRanges.Add(MakeRange(*SlotIndex));
			}
			SortRangesByPhysicalAddress(OutActiveRanges);
			OutNotResidentPageIds.Sort();
			return OutNotResidentPageIds.IsEmpty();
		}

		bool IsInitialized() const { return bInitialized; }
		uint64 GetFixedSlotCount() const { return static_cast<uint64>(Slots.Num()); }
		uint64 GetTotalPhysicalSplatCapacity() const { return TotalPhysicalSplatCapacity; }
		uint64 GetLastSuccessfulGeneration() const { return LastSuccessfulGeneration; }
		uint64 GetOpenCaptureGenerationCount() const { return static_cast<uint64>(OpenCaptureGenerations.Num()); }

	private:
		struct FSlot
		{
			uint64 PageId = 0;
			uint64 SplatCount = 0;
			uint64 AssignmentToken = 0;
			uint64 LastRequestedGeneration = 0;
			EPageSlotState State = EPageSlotState::Empty;
			TSet<uint64> PinGenerations;
		};

		static bool TryMultiply(const uint64 A, const uint64 B, uint64& OutValue)
		{
			if (A != 0 && B > MAX_uint64 / A)
			{
				OutValue = 0;
				return false;
			}
			OutValue = A * B;
			return true;
		}

		FPagePhysicalRange MakeRange(const int32 SlotIndex) const
		{
			check(Slots.IsValidIndex(SlotIndex));
			const FSlot& Slot = Slots[SlotIndex];
			check(Slot.State != EPageSlotState::Empty);
			FPagePhysicalRange Range;
			Range.PageId = Slot.PageId;
			Range.SlotIndex = static_cast<uint64>(SlotIndex);
			Range.FirstSplat = static_cast<uint64>(SlotIndex) * Config.SplatsPerSlot;
			Range.SplatCount = Slot.SplatCount;
			Range.AssignmentToken = Slot.AssignmentToken;
			Range.State = Slot.State;
			return Range;
		}

		static void SortRangesByPhysicalAddress(TArray<FPagePhysicalRange>& Ranges)
		{
			Ranges.Sort([](const FPagePhysicalRange& A, const FPagePhysicalRange& B)
			{
				return A.SlotIndex < B.SlotIndex;
			});
		}

		bool ValidateCurrentAssignment(
			const FPagePhysicalRange& Range,
			const EPageSlotState ExpectedState,
			int32& OutSlotIndex,
			FString& OutError) const
		{
			if (!bInitialized || Range.SlotIndex >= static_cast<uint64>(Slots.Num()))
			{
				OutError = TEXT("Load completion references an invalid resident slot");
				return false;
			}
			const int32 SlotIndex = static_cast<int32>(Range.SlotIndex);
			const FSlot& Slot = Slots[SlotIndex];
			if (Slot.State != ExpectedState ||
				Slot.PageId != Range.PageId ||
				Slot.SplatCount != Range.SplatCount ||
				Slot.AssignmentToken != Range.AssignmentToken ||
				Range.FirstSplat != static_cast<uint64>(SlotIndex) * Config.SplatsPerSlot)
			{
				OutError = TEXT("Load completion is stale or does not match the current slot assignment");
				return false;
			}
			OutSlotIndex = SlotIndex;
			return true;
		}

		uint64 GetOccupiedSlotCount() const
		{
			uint64 Count = 0;
			for (const FSlot& Slot : Slots)
			{
				Count += Slot.State != EPageSlotState::Empty ? 1ull : 0ull;
			}
			return Count;
		}

		uint64 GetPinnedSlotCount() const
		{
			uint64 Count = 0;
			for (const FSlot& Slot : Slots)
			{
				Count += !Slot.PinGenerations.IsEmpty() ? 1ull : 0ull;
			}
			return Count;
		}

		uint64 GetLoadingSlotCount() const
		{
			uint64 Count = 0;
			for (const FSlot& Slot : Slots)
			{
				Count += Slot.State == EPageSlotState::Loading ? 1ull : 0ull;
			}
			return Count;
		}

		FPageResidentPoolConfig Config;
		TMap<uint64, uint64> Catalog;
		TArray<FSlot> Slots;
		TMap<uint64, int32> PageToSlot;
		TSet<uint64> OpenCaptureGenerations;
		uint64 TotalPhysicalSplatCapacity = 0;
		uint64 LastSuccessfulGeneration = 0;
		uint64 NextAssignmentToken = 1;
		bool bInitialized = false;
	};
}
