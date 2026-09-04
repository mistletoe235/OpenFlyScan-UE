// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSPageDemandSelector.h"

#include "Algo/Sort.h"

#include <cmath>
#include <limits>

namespace NanoGS::Paged
{
	namespace
	{
		constexpr double MinimumExactSupportSigma = 4.0;
		constexpr double MinimumNormalSquared = 1.e-24;
		constexpr double DoubleMachineEpsilon = 2.2204460492503131e-16;

		bool IsFiniteVector(const FVector3d& Value)
		{
			return FMath::IsFinite(Value.X) &&
				FMath::IsFinite(Value.Y) &&
				FMath::IsFinite(Value.Z);
		}

		bool IsFiniteOrderedBounds(const FVector3d& Min, const FVector3d& Max)
		{
			return IsFiniteVector(Min) &&
				IsFiniteVector(Max) &&
				Min.X <= Max.X &&
				Min.Y <= Max.Y &&
				Min.Z <= Max.Z;
		}

		bool IsConfigValid(const FExactHQPageDemandConfig& Config)
		{
			return FMath::IsFinite(Config.ExactSupportSigma) &&
				Config.ExactSupportSigma >= MinimumExactSupportSigma &&
				FMath::IsFinite(Config.SplatScale) &&
				FMath::IsFinite(Config.RasterGuardPixels) &&
				Config.RasterGuardPixels > 0.0 &&
				FMath::IsFinite(Config.AdditionalLocalGuardCentimeters) &&
				Config.AdditionalLocalGuardCentimeters >= 0.0 &&
				FMath::IsFinite(Config.NumericalGuardCentimeters) &&
				Config.NumericalGuardCentimeters >= 0.0;
		}

		bool IsTransformValid(const FTransform& Transform)
		{
			const FVector Translation = Transform.GetTranslation();
			const FVector Scale = Transform.GetScale3D();
			const FQuat Rotation = Transform.GetRotation();
			return IsFiniteVector(FVector3d(Translation)) &&
				IsFiniteVector(FVector3d(Scale)) &&
				FMath::IsFinite(Rotation.X) &&
				FMath::IsFinite(Rotation.Y) &&
				FMath::IsFinite(Rotation.Z) &&
				FMath::IsFinite(Rotation.W) &&
				Rotation.IsNormalized();
		}

		bool IsPageValid(const FPageRecord& Page)
		{
			const bool bCenterBoundsValid =
				IsFiniteOrderedBounds(Page.CenterBoundsMin, Page.CenterBoundsMax);
			const bool bSupportBoundsValid =
				IsFiniteOrderedBounds(Page.SupportBoundsMin, Page.SupportBoundsMax);
			const bool bSupportContainsCenters = bCenterBoundsValid && bSupportBoundsValid &&
				Page.SupportBoundsMin.X <= Page.CenterBoundsMin.X &&
				Page.SupportBoundsMin.Y <= Page.CenterBoundsMin.Y &&
				Page.SupportBoundsMin.Z <= Page.CenterBoundsMin.Z &&
				Page.SupportBoundsMax.X >= Page.CenterBoundsMax.X &&
				Page.SupportBoundsMax.Y >= Page.CenterBoundsMax.Y &&
				Page.SupportBoundsMax.Z >= Page.CenterBoundsMax.Z;
			return Page.SplatCount > 0 &&
				bSupportContainsCenters &&
				FMath::IsFinite(Page.SupportSigma) &&
				FMath::IsNearlyEqual(Page.SupportSigma, 2.0, 1.e-12) &&
				FMath::IsFinite(Page.MaxScaleCm) &&
				Page.MaxScaleCm >= 0.0 &&
				EnumHasAnyFlags(Page.Flags, EPageFlags::RotatedGaussianAABB2Sigma);
		}

		bool IsViewValid(
			const FExactHQPageDemandView& View,
			const FExactHQPageDemandConfig& Config)
		{
			if (View.FrustumPlanes.IsEmpty() ||
				!FMath::IsFinite(View.MaxWorldCentimetersPerPixel) ||
				View.MaxWorldCentimetersPerPixel <= 0.0 ||
				!FMath::IsFinite(View.AdditionalWorldGuardCentimeters) ||
				View.AdditionalWorldGuardCentimeters < 0.0)
			{
				return false;
			}

			const double RasterGuard =
				Config.RasterGuardPixels * View.MaxWorldCentimetersPerPixel;
			if (!FMath::IsFinite(RasterGuard))
			{
				return false;
			}

			for (const FPageDemandPlane& Plane : View.FrustumPlanes)
			{
				if (!IsFiniteVector(Plane.Normal) ||
					!FMath::IsFinite(Plane.Offset))
				{
					return false;
				}

				const double NormalSquared = Plane.Normal.SizeSquared();
				if (!FMath::IsFinite(NormalSquared) || NormalSquared <= MinimumNormalSquared)
				{
					return false;
				}
			}
			return true;
		}

		void SortUnique(TArray<uint64>& PageIds)
		{
			PageIds.Sort();
			if (PageIds.Num() < 2)
			{
				return;
			}

			int32 WriteIndex = 1;
			for (int32 ReadIndex = 1; ReadIndex < PageIds.Num(); ++ReadIndex)
			{
				if (PageIds[ReadIndex] != PageIds[WriteIndex - 1])
				{
					PageIds[WriteIndex++] = PageIds[ReadIndex];
				}
			}
			PageIds.SetNum(WriteIndex, EAllowShrinking::No);
		}

		TArray<uint64> CollectAllPageIds(TConstArrayView<FPageRecord> Pages)
		{
			TArray<uint64> Result;
			Result.Reserve(Pages.Num());
			for (const FPageRecord& Page : Pages)
			{
				Result.Add(Page.PageId);
			}
			SortUnique(Result);
			return Result;
		}

		bool CalculateExpandedWorldBounds(
			const FPageRecord& Page,
			const FTransform& LocalToWorld,
			const FExactHQPageDemandConfig& Config,
			FVector3d& OutWorldMin,
			FVector3d& OutWorldMax)
		{
			FVector3d LocalMin;
			FVector3d LocalMax;
			if (!FPageDemandSelector::CalculateConservativeLocalSupportBounds(
				Page.SupportBoundsMin,
				Page.SupportBoundsMax,
				Page.SupportSigma,
				Page.MaxScaleCm,
				Config.ExactSupportSigma,
				Config.SplatScale,
				LocalMin,
				LocalMax))
			{
				return false;
			}

			if (!FMath::IsFinite(Config.AdditionalLocalGuardCentimeters))
			{
				return false;
			}
			LocalMin -= FVector3d(Config.AdditionalLocalGuardCentimeters);
			LocalMax += FVector3d(Config.AdditionalLocalGuardCentimeters);
			if (!IsFiniteOrderedBounds(LocalMin, LocalMax))
			{
				return false;
			}

			OutWorldMin = FVector3d(TNumericLimits<double>::Max());
			OutWorldMax = FVector3d(TNumericLimits<double>::Lowest());
			for (uint32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
			{
				const FVector LocalCorner(
					(CornerIndex & 1u) != 0 ? LocalMax.X : LocalMin.X,
					(CornerIndex & 2u) != 0 ? LocalMax.Y : LocalMin.Y,
					(CornerIndex & 4u) != 0 ? LocalMax.Z : LocalMin.Z);
				const FVector3d WorldCorner(LocalToWorld.TransformPosition(LocalCorner));
				if (!IsFiniteVector(WorldCorner))
				{
					return false;
				}
				OutWorldMin.X = FMath::Min(OutWorldMin.X, WorldCorner.X);
				OutWorldMin.Y = FMath::Min(OutWorldMin.Y, WorldCorner.Y);
				OutWorldMin.Z = FMath::Min(OutWorldMin.Z, WorldCorner.Z);
				OutWorldMax.X = FMath::Max(OutWorldMax.X, WorldCorner.X);
				OutWorldMax.Y = FMath::Max(OutWorldMax.Y, WorldCorner.Y);
				OutWorldMax.Z = FMath::Max(OutWorldMax.Z, WorldCorner.Z);
			}
			return IsFiniteOrderedBounds(OutWorldMin, OutWorldMax);
		}

		bool IntersectsView(
			const FVector3d& WorldMin,
			const FVector3d& WorldMax,
			const FExactHQPageDemandView& View,
			const FExactHQPageDemandConfig& Config,
			bool& bOutCalculationValid)
		{
			bOutCalculationValid = true;
			const FVector3d Center = (WorldMin + WorldMax) * 0.5;
			const FVector3d Extent = (WorldMax - WorldMin) * 0.5;
			const double WorldGuard =
				Config.RasterGuardPixels * View.MaxWorldCentimetersPerPixel +
				View.AdditionalWorldGuardCentimeters +
				Config.NumericalGuardCentimeters;
			if (!IsFiniteVector(Center) || !IsFiniteVector(Extent) || !FMath::IsFinite(WorldGuard))
			{
				bOutCalculationValid = false;
				return true;
			}

			for (const FPageDemandPlane& Plane : View.FrustumPlanes)
			{
				const double NormalLength = Plane.Normal.Size();
				const double ProjectedExtent =
					FMath::Abs(Plane.Normal.X) * Extent.X +
					FMath::Abs(Plane.Normal.Y) * Extent.Y +
					FMath::Abs(Plane.Normal.Z) * Extent.Z;
				const double MaximumPlaneValue =
					FVector3d::DotProduct(Plane.Normal, Center) + Plane.Offset + ProjectedExtent;
				const double PlaneGuard = WorldGuard * NormalLength;
				// Bound accumulated double-precision dot/add cancellation as well as the
				// explicit geometric guard. A large-coordinate world must over-select,
				// never flip a touching box to the outside half-space.
				const double PlaneExpressionMagnitude =
					FMath::Abs(Plane.Normal.X * Center.X) +
					FMath::Abs(Plane.Normal.Y * Center.Y) +
					FMath::Abs(Plane.Normal.Z * Center.Z) +
					FMath::Abs(Plane.Offset) + ProjectedExtent + PlaneGuard;
				const double RoundoffGuard =
					16.0 * DoubleMachineEpsilon * PlaneExpressionMagnitude;
				const double SeparationGuard = PlaneGuard + RoundoffGuard;
				if (!FMath::IsFinite(NormalLength) ||
					!FMath::IsFinite(ProjectedExtent) ||
					!FMath::IsFinite(MaximumPlaneValue) ||
					!FMath::IsFinite(PlaneGuard) ||
					!FMath::IsFinite(PlaneExpressionMagnitude) ||
					!FMath::IsFinite(RoundoffGuard) ||
					!FMath::IsFinite(SeparationGuard))
				{
					bOutCalculationValid = false;
					return true;
				}

				// Equality is an intersection. Only a strict, guarded separation is culled.
				if (MaximumPlaneValue < -SeparationGuard)
				{
					return false;
				}
			}
			return true;
		}
	}

	bool FPageDemandSelector::CalculateConservativeMaxWorldCentimetersPerPixel(
		const FIntPoint UnscaledViewSize,
		const FIntPoint RasterViewSize,
		const double ProjectionScaleX,
		const double ProjectionScaleY,
		const bool bPerspective,
		const double MaxPositiveDepthCentimeters,
		double& OutMaxWorldCentimetersPerPixel)
	{
		OutMaxWorldCentimetersPerPixel = 0.0;
		if (UnscaledViewSize.X <= 0 || UnscaledViewSize.Y <= 0 ||
			RasterViewSize.X <= 0 || RasterViewSize.Y <= 0 ||
			!FMath::IsFinite(ProjectionScaleX) || ProjectionScaleX <= 0.0 ||
			!FMath::IsFinite(ProjectionScaleY) || ProjectionScaleY <= 0.0 ||
			(bPerspective && (!FMath::IsFinite(MaxPositiveDepthCentimeters) ||
				MaxPositiveDepthCentimeters <= 0.0)))
		{
			return false;
		}

		const double MinFocalPixels = FMath::Min(
			FMath::Min(
				0.5 * static_cast<double>(UnscaledViewSize.X) * ProjectionScaleX,
				0.5 * static_cast<double>(UnscaledViewSize.Y) * ProjectionScaleY),
			FMath::Min(
				0.5 * static_cast<double>(RasterViewSize.X) * ProjectionScaleX,
				0.5 * static_cast<double>(RasterViewSize.Y) * ProjectionScaleY));
		if (!FMath::IsFinite(MinFocalPixels) || MinFocalPixels <= 0.0)
		{
			return false;
		}

		OutMaxWorldCentimetersPerPixel = bPerspective
			? MaxPositiveDepthCentimeters / MinFocalPixels
			: 1.0 / MinFocalPixels;
		return FMath::IsFinite(OutMaxWorldCentimetersPerPixel) &&
			OutMaxWorldCentimetersPerPixel > 0.0;
	}

	bool FPageDemandSelector::CalculateConservativeLocalSupportBounds(
		const FVector3d& StoredSupportMin,
		const FVector3d& StoredSupportMax,
		const double StoredSupportSigma,
		const double MaxScaleCentimeters,
		const double ExactSupportSigma,
		const double SplatScale,
		FVector3d& OutSupportMin,
		FVector3d& OutSupportMax)
	{
		OutSupportMin = FVector3d::ZeroVector;
		OutSupportMax = FVector3d::ZeroVector;
		if (!IsFiniteOrderedBounds(StoredSupportMin, StoredSupportMax) ||
			!FMath::IsFinite(StoredSupportSigma) || StoredSupportSigma <= 0.0 ||
			!FMath::IsFinite(MaxScaleCentimeters) || MaxScaleCentimeters < 0.0 ||
			!FMath::IsFinite(ExactSupportSigma) || ExactSupportSigma < 0.0 ||
			!FMath::IsFinite(SplatScale))
		{
			return false;
		}

		const double EffectiveSigma = ExactSupportSigma * FMath::Abs(SplatScale);
		const double Expansion =
			FMath::Max(0.0, EffectiveSigma - StoredSupportSigma) * MaxScaleCentimeters;
		if (!FMath::IsFinite(EffectiveSigma) || !FMath::IsFinite(Expansion))
		{
			return false;
		}
		OutSupportMin = StoredSupportMin - FVector3d(Expansion);
		OutSupportMax = StoredSupportMax + FVector3d(Expansion);
		return IsFiniteOrderedBounds(OutSupportMin, OutSupportMax);
	}

	FExactHQPageDemandResult FPageDemandSelector::SelectExactHQPages(
		TConstArrayView<FPageRecord> Pages,
		const FTransform& LocalToWorld,
		TConstArrayView<FExactHQPageDemandView> Views,
		const FExactHQPageDemandConfig& Config)
	{
		FExactHQPageDemandResult Result;
		Result.PerView.Reserve(Views.Num());
		for (const FExactHQPageDemandView& View : Views)
		{
			FExactHQPageDemandViewResult& ViewResult = Result.PerView.AddDefaulted_GetRef();
			ViewResult.ViewId = View.ViewId;
			ViewResult.RequiredPageIds.Reserve(Pages.Num());
		}

		const TArray<uint64> AllPageIds = CollectAllPageIds(Pages);
		const bool bConfigValid = IsConfigValid(Config);
		const bool bTransformValid = IsTransformValid(LocalToWorld);
		if (!bConfigValid || !bTransformValid)
		{
			EPageDemandFailOpenReason Reason = EPageDemandFailOpenReason::None;
			if (!bConfigValid)
			{
				Reason |= EPageDemandFailOpenReason::InvalidConfig;
			}
			if (!bTransformValid)
			{
				Reason |= EPageDemandFailOpenReason::InvalidTransform;
			}
			Result.FailOpenReasons |= Reason;
			Result.UnionRequiredPageIds = AllPageIds;
			for (FExactHQPageDemandViewResult& ViewResult : Result.PerView)
			{
				ViewResult.FailOpenReasons |= Reason;
				ViewResult.RequiredPageIds = AllPageIds;
			}
			return Result;
		}

		TArray<bool> ValidViews;
		ValidViews.Reserve(Views.Num());
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			const bool bViewValid = IsViewValid(Views[ViewIndex], Config);
			ValidViews.Add(bViewValid);
			if (!bViewValid)
			{
				FExactHQPageDemandViewResult& ViewResult = Result.PerView[ViewIndex];
				ViewResult.FailOpenReasons |= EPageDemandFailOpenReason::InvalidView;
				ViewResult.RequiredPageIds = AllPageIds;
				Result.FailOpenReasons |= EPageDemandFailOpenReason::InvalidView;
			}
		}

		for (const FPageRecord& Page : Pages)
		{
			if (!IsPageValid(Page))
			{
				Result.FailOpenReasons |= EPageDemandFailOpenReason::InvalidPage;
				Result.UnionRequiredPageIds.Add(Page.PageId);
				for (FExactHQPageDemandViewResult& ViewResult : Result.PerView)
				{
					ViewResult.FailOpenReasons |= EPageDemandFailOpenReason::InvalidPage;
					ViewResult.RequiredPageIds.Add(Page.PageId);
				}
				continue;
			}

			FVector3d WorldMin;
			FVector3d WorldMax;
			if (!CalculateExpandedWorldBounds(Page, LocalToWorld, Config, WorldMin, WorldMax))
			{
				Result.FailOpenReasons |= EPageDemandFailOpenReason::NonFiniteCalculation;
				Result.UnionRequiredPageIds.Add(Page.PageId);
				for (FExactHQPageDemandViewResult& ViewResult : Result.PerView)
				{
					ViewResult.FailOpenReasons |= EPageDemandFailOpenReason::NonFiniteCalculation;
					ViewResult.RequiredPageIds.Add(Page.PageId);
				}
				continue;
			}

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if (!ValidViews[ViewIndex])
				{
					continue;
				}

				bool bCalculationValid = true;
				if (IntersectsView(WorldMin, WorldMax, Views[ViewIndex], Config, bCalculationValid))
				{
					Result.PerView[ViewIndex].RequiredPageIds.Add(Page.PageId);
					Result.UnionRequiredPageIds.Add(Page.PageId);
				}
				if (!bCalculationValid)
				{
					Result.PerView[ViewIndex].FailOpenReasons |=
						EPageDemandFailOpenReason::NonFiniteCalculation;
					Result.FailOpenReasons |= EPageDemandFailOpenReason::NonFiniteCalculation;
				}
			}
		}

		// Invalid views already require every page; merge that requirement into the union.
		for (const FExactHQPageDemandViewResult& ViewResult : Result.PerView)
		{
			Result.UnionRequiredPageIds.Append(ViewResult.RequiredPageIds);
		}

		for (FExactHQPageDemandViewResult& ViewResult : Result.PerView)
		{
			SortUnique(ViewResult.RequiredPageIds);
		}
		SortUnique(Result.UnionRequiredPageIds);
		return Result;
	}

	bool FPageDemandHierarchy::Initialize(
		const TConstArrayView<FPageRecord> Pages,
		FString& OutError)
	{
		Reset();
		OutError.Reset();
		BorrowedPages = Pages;
		SortedAllPageIds = CollectAllPageIds(Pages);
		ValidPageOrdinals.Reserve(Pages.Num());
		InvalidPageOrdinals.Reserve(Pages.Num());
		PreparedPageBounds.SetNum(Pages.Num());
		for (int32 PageOrdinal = 0; PageOrdinal < Pages.Num(); ++PageOrdinal)
		{
			if (IsPageValid(Pages[PageOrdinal]))
			{
				ValidPageOrdinals.Add(PageOrdinal);
			}
			else
			{
				InvalidPageOrdinals.Add(PageOrdinal);
			}
		}

		if (!ValidPageOrdinals.IsEmpty())
		{
			// Median splits stop at eight pages.  Reserving roughly N/2 nodes covers
			// the worst balanced 4-page leaves without a 2N memory spike on huge files.
			Nodes.Reserve(ValidPageOrdinals.Num() / 2 + 1);
			BuildNode(0, ValidPageOrdinals.Num());
		}
		bInitialized = true;
		return true;
	}

	void FPageDemandHierarchy::Reset()
	{
		BorrowedPages = TConstArrayView<FPageRecord>();
		SortedAllPageIds.Reset();
		ValidPageOrdinals.Reset();
		InvalidPageOrdinals.Reset();
		Nodes.Reset();
		PreparedPageBounds.Reset();
		NonFinitePageOrdinals.Reset();
		PreparedLocalToWorld = FTransform::Identity;
		PreparedConfig = FExactHQPageDemandConfig();
		for (FVector3d& Corner : PreparedSceneWorldCorners)
		{
			Corner = FVector3d::ZeroVector;
		}
		bInitialized = false;
		bPrepared = false;
		bPreparedSceneWorldCornersValid = false;
	}

	int32 FPageDemandHierarchy::BuildNode(
		const int32 FirstPageOrdinal,
		const int32 PageOrdinalCount)
	{
		constexpr int32 PagesPerLeaf = 8;
		const int32 NodeIndex = Nodes.AddDefaulted();
		Nodes[NodeIndex].FirstPageOrdinal = FirstPageOrdinal;
		Nodes[NodeIndex].PageOrdinalCount = PageOrdinalCount;
		if (PageOrdinalCount <= PagesPerLeaf)
		{
			return NodeIndex;
		}

		FVector3d CentroidMin(TNumericLimits<double>::Max());
		FVector3d CentroidMax(TNumericLimits<double>::Lowest());
		for (int32 Index = FirstPageOrdinal;
			Index < FirstPageOrdinal + PageOrdinalCount;
			++Index)
		{
			const FPageRecord& Page = BorrowedPages[ValidPageOrdinals[Index]];
			// Half-before-add avoids overflowing for same-sign finite endpoints.
			const FVector3d Centroid =
				Page.SupportBoundsMin * 0.5 + Page.SupportBoundsMax * 0.5;
			CentroidMin.X = FMath::Min(CentroidMin.X, Centroid.X);
			CentroidMin.Y = FMath::Min(CentroidMin.Y, Centroid.Y);
			CentroidMin.Z = FMath::Min(CentroidMin.Z, Centroid.Z);
			CentroidMax.X = FMath::Max(CentroidMax.X, Centroid.X);
			CentroidMax.Y = FMath::Max(CentroidMax.Y, Centroid.Y);
			CentroidMax.Z = FMath::Max(CentroidMax.Z, Centroid.Z);
		}

		const FVector3d CentroidExtent = CentroidMax - CentroidMin;
		int32 SplitAxis = 0;
		if (CentroidExtent.Y > CentroidExtent.X)
		{
			SplitAxis = 1;
		}
		if (CentroidExtent.Z > CentroidExtent[SplitAxis])
		{
			SplitAxis = 2;
		}

		TArrayView<int32> PageRange(
			ValidPageOrdinals.GetData() + FirstPageOrdinal,
			PageOrdinalCount);
		Algo::Sort(PageRange, [this, SplitAxis](const int32 A, const int32 B)
		{
			const FPageRecord& PageA = BorrowedPages[A];
			const FPageRecord& PageB = BorrowedPages[B];
			const double CenterA =
				PageA.SupportBoundsMin[SplitAxis] * 0.5 +
				PageA.SupportBoundsMax[SplitAxis] * 0.5;
			const double CenterB =
				PageB.SupportBoundsMin[SplitAxis] * 0.5 +
				PageB.SupportBoundsMax[SplitAxis] * 0.5;
			return CenterA == CenterB ? A < B : CenterA < CenterB;
		});

		const int32 LeftCount = PageOrdinalCount / 2;
		const int32 LeftChild = BuildNode(FirstPageOrdinal, LeftCount);
		const int32 RightChild = BuildNode(
			FirstPageOrdinal + LeftCount,
			PageOrdinalCount - LeftCount);
		Nodes[NodeIndex].LeftChild = LeftChild;
		Nodes[NodeIndex].RightChild = RightChild;
		return NodeIndex;
	}

	bool FPageDemandHierarchy::CacheKeyEquals(
		const FTransform& LocalToWorld,
		const FExactHQPageDemandConfig& Config) const
	{
		if (!bPrepared)
		{
			return false;
		}

		const FVector TranslationA = PreparedLocalToWorld.GetTranslation();
		const FVector TranslationB = LocalToWorld.GetTranslation();
		const FVector ScaleA = PreparedLocalToWorld.GetScale3D();
		const FVector ScaleB = LocalToWorld.GetScale3D();
		const FQuat RotationA = PreparedLocalToWorld.GetRotation();
		const FQuat RotationB = LocalToWorld.GetRotation();
		return TranslationA.X == TranslationB.X &&
			TranslationA.Y == TranslationB.Y &&
			TranslationA.Z == TranslationB.Z &&
			ScaleA.X == ScaleB.X &&
			ScaleA.Y == ScaleB.Y &&
			ScaleA.Z == ScaleB.Z &&
			RotationA.X == RotationB.X &&
			RotationA.Y == RotationB.Y &&
			RotationA.Z == RotationB.Z &&
			RotationA.W == RotationB.W &&
			PreparedConfig.ExactSupportSigma == Config.ExactSupportSigma &&
			PreparedConfig.SplatScale == Config.SplatScale &&
			PreparedConfig.RasterGuardPixels == Config.RasterGuardPixels &&
			PreparedConfig.AdditionalLocalGuardCentimeters ==
				Config.AdditionalLocalGuardCentimeters &&
			PreparedConfig.NumericalGuardCentimeters == Config.NumericalGuardCentimeters;
	}

	bool FPageDemandHierarchy::Prepare(
		const FTransform& LocalToWorld,
		const FExactHQPageDemandConfig& Config)
	{
		if (!bInitialized || !IsConfigValid(Config) || !IsTransformValid(LocalToWorld))
		{
			return false;
		}
		if (CacheKeyEquals(LocalToWorld, Config))
		{
			return true;
		}

		NonFinitePageOrdinals.Reset();
		for (FPreparedPageBounds& Bounds : PreparedPageBounds)
		{
			Bounds = FPreparedPageBounds();
		}

		FVector3d SceneLocalMin(TNumericLimits<double>::Max());
		FVector3d SceneLocalMax(TNumericLimits<double>::Lowest());
		bool bSceneBoundsValid = !BorrowedPages.IsEmpty();
		for (int32 PageOrdinal = 0; PageOrdinal < BorrowedPages.Num(); ++PageOrdinal)
		{
			const FPageRecord& Page = BorrowedPages[PageOrdinal];
			FVector3d LocalMin;
			FVector3d LocalMax;
			if (!FPageDemandSelector::CalculateConservativeLocalSupportBounds(
				Page.SupportBoundsMin,
				Page.SupportBoundsMax,
				Page.SupportSigma,
				Page.MaxScaleCm,
				Config.ExactSupportSigma,
				Config.SplatScale,
				LocalMin,
				LocalMax))
			{
				bSceneBoundsValid = false;
			}
			else
			{
				LocalMin -= FVector3d(Config.AdditionalLocalGuardCentimeters);
				LocalMax += FVector3d(Config.AdditionalLocalGuardCentimeters);
				if (!IsFiniteOrderedBounds(LocalMin, LocalMax))
				{
					bSceneBoundsValid = false;
				}
				else
				{
					SceneLocalMin.X = FMath::Min(SceneLocalMin.X, LocalMin.X);
					SceneLocalMin.Y = FMath::Min(SceneLocalMin.Y, LocalMin.Y);
					SceneLocalMin.Z = FMath::Min(SceneLocalMin.Z, LocalMin.Z);
					SceneLocalMax.X = FMath::Max(SceneLocalMax.X, LocalMax.X);
					SceneLocalMax.Y = FMath::Max(SceneLocalMax.Y, LocalMax.Y);
					SceneLocalMax.Z = FMath::Max(SceneLocalMax.Z, LocalMax.Z);
				}
			}

			if (!IsPageValid(Page))
			{
				continue;
			}
			FPreparedPageBounds& PreparedBounds = PreparedPageBounds[PageOrdinal];
			PreparedBounds.bValid = CalculateExpandedWorldBounds(
				Page,
				LocalToWorld,
				Config,
				PreparedBounds.WorldMin,
				PreparedBounds.WorldMax);
			if (!PreparedBounds.bValid)
			{
				NonFinitePageOrdinals.Add(PageOrdinal);
			}
		}

		bPreparedSceneWorldCornersValid = bSceneBoundsValid &&
			IsFiniteOrderedBounds(SceneLocalMin, SceneLocalMax);
		if (bPreparedSceneWorldCornersValid)
		{
			for (uint32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
			{
				const FVector LocalCorner(
					(CornerIndex & 1u) != 0 ? SceneLocalMax.X : SceneLocalMin.X,
					(CornerIndex & 2u) != 0 ? SceneLocalMax.Y : SceneLocalMin.Y,
					(CornerIndex & 4u) != 0 ? SceneLocalMax.Z : SceneLocalMin.Z);
				PreparedSceneWorldCorners[CornerIndex] =
					FVector3d(LocalToWorld.TransformPosition(LocalCorner));
				if (!IsFiniteVector(PreparedSceneWorldCorners[CornerIndex]))
				{
					bPreparedSceneWorldCornersValid = false;
					break;
				}
			}
		}

		if (!Nodes.IsEmpty())
		{
			RefitNode(0);
		}
		PreparedLocalToWorld = LocalToWorld;
		PreparedConfig = Config;
		bPrepared = true;
		return true;
	}

	void FPageDemandHierarchy::RefitNode(const int32 NodeIndex)
	{
		FNode& Node = Nodes[NodeIndex];
		Node.WorldMin = FVector3d(TNumericLimits<double>::Max());
		Node.WorldMax = FVector3d(TNumericLimits<double>::Lowest());
		Node.bHasSelectableBounds = false;
		Node.bCanCull = true;

		if (Node.IsLeaf())
		{
			for (int32 Index = Node.FirstPageOrdinal;
				Index < Node.FirstPageOrdinal + Node.PageOrdinalCount;
				++Index)
			{
				const int32 PageOrdinal = ValidPageOrdinals[Index];
				const FPreparedPageBounds& PageBounds = PreparedPageBounds[PageOrdinal];
				if (!PageBounds.bValid)
				{
					continue;
				}
				Node.bHasSelectableBounds = true;
				Node.WorldMin.X = FMath::Min(Node.WorldMin.X, PageBounds.WorldMin.X);
				Node.WorldMin.Y = FMath::Min(Node.WorldMin.Y, PageBounds.WorldMin.Y);
				Node.WorldMin.Z = FMath::Min(Node.WorldMin.Z, PageBounds.WorldMin.Z);
				Node.WorldMax.X = FMath::Max(Node.WorldMax.X, PageBounds.WorldMax.X);
				Node.WorldMax.Y = FMath::Max(Node.WorldMax.Y, PageBounds.WorldMax.Y);
				Node.WorldMax.Z = FMath::Max(Node.WorldMax.Z, PageBounds.WorldMax.Z);
			}
		}
		else
		{
			RefitNode(Node.LeftChild);
			RefitNode(Node.RightChild);
			const FNode& Left = Nodes[Node.LeftChild];
			const FNode& Right = Nodes[Node.RightChild];
			for (const FNode* Child : {&Left, &Right})
			{
				if (!Child->bHasSelectableBounds)
				{
					continue;
				}
				Node.bHasSelectableBounds = true;
				Node.bCanCull = Node.bCanCull && Child->bCanCull;
				Node.WorldMin.X = FMath::Min(Node.WorldMin.X, Child->WorldMin.X);
				Node.WorldMin.Y = FMath::Min(Node.WorldMin.Y, Child->WorldMin.Y);
				Node.WorldMin.Z = FMath::Min(Node.WorldMin.Z, Child->WorldMin.Z);
				Node.WorldMax.X = FMath::Max(Node.WorldMax.X, Child->WorldMax.X);
				Node.WorldMax.Y = FMath::Max(Node.WorldMax.Y, Child->WorldMax.Y);
				Node.WorldMax.Z = FMath::Max(Node.WorldMax.Z, Child->WorldMax.Z);
			}
		}

		if (!Node.bHasSelectableBounds)
		{
			Node.bCanCull = false;
			return;
		}

		// Outward ULP inflation makes every hierarchy rejection strictly more
		// conservative than testing the exact child AABBs cached above.
		const double NegativeInfinity = -std::numeric_limits<double>::infinity();
		const double PositiveInfinity = std::numeric_limits<double>::infinity();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double ExpandedMin = std::nextafter(Node.WorldMin[Axis], NegativeInfinity);
			const double ExpandedMax = std::nextafter(Node.WorldMax[Axis], PositiveInfinity);
			if (!FMath::IsFinite(ExpandedMin) || !FMath::IsFinite(ExpandedMax))
			{
				Node.bCanCull = false;
				continue;
			}
			Node.WorldMin[Axis] = ExpandedMin;
			Node.WorldMax[Axis] = ExpandedMax;
		}
	}

	bool FPageDemandHierarchy::GetPreparedSceneWorldCorners(
		FVector3d OutWorldCorners[8]) const
	{
		if (!bPrepared || !bPreparedSceneWorldCornersValid || OutWorldCorners == nullptr)
		{
			return false;
		}
		for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			OutWorldCorners[CornerIndex] = PreparedSceneWorldCorners[CornerIndex];
		}
		return true;
	}

	FExactHQPageDemandResult FPageDemandHierarchy::SelectExactHQPages(
		const FTransform& LocalToWorld,
		const TConstArrayView<FExactHQPageDemandView> Views,
		const FExactHQPageDemandConfig& Config)
	{
		if (!bInitialized)
		{
			return FPageDemandSelector::SelectExactHQPages(
				BorrowedPages, LocalToWorld, Views, Config);
		}

		FExactHQPageDemandResult Result;
		Result.PerView.Reserve(Views.Num());
		for (const FExactHQPageDemandView& View : Views)
		{
			FExactHQPageDemandViewResult& ViewResult = Result.PerView.AddDefaulted_GetRef();
			ViewResult.ViewId = View.ViewId;
			ViewResult.RequiredPageIds.Reserve(FMath::Min(BorrowedPages.Num(), 1024));
		}

		const TArray<uint64>& AllPageIds = SortedAllPageIds;
		const bool bConfigValid = IsConfigValid(Config);
		const bool bTransformValid = IsTransformValid(LocalToWorld);
		if (!bConfigValid || !bTransformValid)
		{
			EPageDemandFailOpenReason Reason = EPageDemandFailOpenReason::None;
			if (!bConfigValid)
			{
				Reason |= EPageDemandFailOpenReason::InvalidConfig;
			}
			if (!bTransformValid)
			{
				Reason |= EPageDemandFailOpenReason::InvalidTransform;
			}
			Result.FailOpenReasons |= Reason;
			Result.UnionRequiredPageIds = AllPageIds;
			for (FExactHQPageDemandViewResult& ViewResult : Result.PerView)
			{
				ViewResult.FailOpenReasons |= Reason;
				ViewResult.RequiredPageIds = AllPageIds;
			}
			return Result;
		}

		if (!Prepare(LocalToWorld, Config))
		{
			// This path is only possible for an internal hierarchy lifecycle error.
			// The linear selector remains the fail-safe authority.
			return FPageDemandSelector::SelectExactHQPages(
				BorrowedPages, LocalToWorld, Views, Config);
		}

		TArray<bool> ValidViews;
		ValidViews.Reserve(Views.Num());
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			const bool bViewValid = IsViewValid(Views[ViewIndex], Config);
			ValidViews.Add(bViewValid);
			if (!bViewValid)
			{
				FExactHQPageDemandViewResult& ViewResult = Result.PerView[ViewIndex];
				ViewResult.FailOpenReasons |= EPageDemandFailOpenReason::InvalidView;
				ViewResult.RequiredPageIds = AllPageIds;
				Result.FailOpenReasons |= EPageDemandFailOpenReason::InvalidView;
			}
		}

		auto AddFailOpenPages = [&Result, this](
			const TConstArrayView<int32> PageOrdinals,
			const EPageDemandFailOpenReason Reason)
		{
			if (PageOrdinals.IsEmpty())
			{
				return;
			}
			Result.FailOpenReasons |= Reason;
			for (FExactHQPageDemandViewResult& ViewResult : Result.PerView)
			{
				ViewResult.FailOpenReasons |= Reason;
			}
			for (const int32 PageOrdinal : PageOrdinals)
			{
				const uint64 PageId = BorrowedPages[PageOrdinal].PageId;
				Result.UnionRequiredPageIds.Add(PageId);
				for (FExactHQPageDemandViewResult& ViewResult : Result.PerView)
				{
					ViewResult.RequiredPageIds.Add(PageId);
				}
			}
		};
		AddFailOpenPages(InvalidPageOrdinals, EPageDemandFailOpenReason::InvalidPage);
		AddFailOpenPages(NonFinitePageOrdinals, EPageDemandFailOpenReason::NonFiniteCalculation);

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if (!ValidViews[ViewIndex] || Nodes.IsEmpty())
			{
				continue;
			}

			TArray<int32, TInlineAllocator<64>> NodeStack;
			NodeStack.Add(0);
			while (!NodeStack.IsEmpty())
			{
				const int32 NodeIndex = NodeStack.Pop(EAllowShrinking::No);
				const FNode& Node = Nodes[NodeIndex];
				if (!Node.bHasSelectableBounds)
				{
					continue;
				}

				if (Node.bCanCull)
				{
					bool bHierarchyCalculationValid = true;
					if (!IntersectsView(
						Node.WorldMin,
						Node.WorldMax,
						Views[ViewIndex],
						Config,
						bHierarchyCalculationValid) &&
						bHierarchyCalculationValid)
					{
						continue;
					}
				}

				if (!Node.IsLeaf())
				{
					// Push right first so deterministic traversal visits the left child first.
					NodeStack.Add(Node.RightChild);
					NodeStack.Add(Node.LeftChild);
					continue;
				}

				for (int32 Index = Node.FirstPageOrdinal;
					Index < Node.FirstPageOrdinal + Node.PageOrdinalCount;
					++Index)
				{
					const int32 PageOrdinal = ValidPageOrdinals[Index];
					const FPreparedPageBounds& PageBounds = PreparedPageBounds[PageOrdinal];
					if (!PageBounds.bValid)
					{
						continue;
					}

					bool bCalculationValid = true;
					if (IntersectsView(
						PageBounds.WorldMin,
						PageBounds.WorldMax,
						Views[ViewIndex],
						Config,
						bCalculationValid))
					{
						Result.PerView[ViewIndex].RequiredPageIds.Add(
							BorrowedPages[PageOrdinal].PageId);
					}
					if (!bCalculationValid)
					{
						Result.PerView[ViewIndex].FailOpenReasons |=
							EPageDemandFailOpenReason::NonFiniteCalculation;
						Result.FailOpenReasons |=
							EPageDemandFailOpenReason::NonFiniteCalculation;
					}
				}
			}
		}

		for (const FExactHQPageDemandViewResult& ViewResult : Result.PerView)
		{
			Result.UnionRequiredPageIds.Append(ViewResult.RequiredPageIds);
		}
		for (FExactHQPageDemandViewResult& ViewResult : Result.PerView)
		{
			SortUnique(ViewResult.RequiredPageIds);
		}
		SortUnique(Result.UnionRequiredPageIds);
		return Result;
	}
}
