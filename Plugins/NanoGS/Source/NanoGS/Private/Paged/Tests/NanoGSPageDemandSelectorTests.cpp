// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Paged/NanoGSPageDemandSelector.h"

#include "Misc/AutomationTest.h"

#include <limits>

namespace NanoGS::Paged::Tests
{
	namespace
	{
		FPageRecord MakePage(
			const uint64 PageId,
			const FVector3d& SupportMin,
			const FVector3d& SupportMax,
			const double MaxScaleCm = 1.0)
		{
			FPageRecord Page;
			Page.PageId = PageId;
			Page.SplatCount = 1;
			const FVector3d Center = (SupportMin + SupportMax) * 0.5;
			Page.CenterBoundsMin = Center;
			Page.CenterBoundsMax = Center;
			Page.SupportBoundsMin = SupportMin;
			Page.SupportBoundsMax = SupportMax;
			Page.SupportSigma = 2.0;
			Page.MaxScaleCm = MaxScaleCm;
			Page.Flags = EPageFlags::RotatedGaussianAABB2Sigma;
			return Page;
		}

		FExactHQPageDemandView MakeBoxView(
			const uint64 ViewId,
			const FVector3d& Min,
			const FVector3d& Max,
			const double MaxWorldCmPerPixel = 0.1)
		{
			FExactHQPageDemandView View;
			View.ViewId = ViewId;
			View.MaxWorldCentimetersPerPixel = MaxWorldCmPerPixel;
			View.FrustumPlanes = {
				{{1.0, 0.0, 0.0}, -Min.X},
				{{-1.0, 0.0, 0.0}, Max.X},
				{{0.0, 1.0, 0.0}, -Min.Y},
				{{0.0, -1.0, 0.0}, Max.Y},
				{{0.0, 0.0, 1.0}, -Min.Z},
				{{0.0, 0.0, -1.0}, Max.Z},
			};
			return View;
		}

		bool TestEquivalentResults(
			FAutomationTestBase& Test,
			const FString& Label,
			const FExactHQPageDemandResult& Linear,
			const FExactHQPageDemandResult& Hierarchical)
		{
			bool bEquivalent = true;
			auto Check = [&Test, &Label, &bEquivalent](const bool bCondition, const TCHAR* Detail)
			{
				bEquivalent &= Test.TestTrue(
					FString::Printf(TEXT("%s: %s"), *Label, Detail),
					bCondition);
			};

			Check(
				Linear.FailOpenReasons == Hierarchical.FailOpenReasons,
				TEXT("aggregate fail-open reasons match"));
			Check(
				Linear.UnionRequiredPageIds == Hierarchical.UnionRequiredPageIds,
				TEXT("sorted union PageIds match"));
			Check(
				Linear.PerView.Num() == Hierarchical.PerView.Num(),
				TEXT("view result count matches"));
			const int32 CommonViewCount = FMath::Min(
				Linear.PerView.Num(), Hierarchical.PerView.Num());
			for (int32 ViewIndex = 0; ViewIndex < CommonViewCount; ++ViewIndex)
			{
				const FExactHQPageDemandViewResult& A = Linear.PerView[ViewIndex];
				const FExactHQPageDemandViewResult& B = Hierarchical.PerView[ViewIndex];
				Check(A.ViewId == B.ViewId, TEXT("ViewId matches"));
				Check(
					A.FailOpenReasons == B.FailOpenReasons,
					TEXT("per-view fail-open reasons match"));
				Check(
					A.RequiredPageIds == B.RequiredPageIds,
					TEXT("per-view sorted PageIds match"));
			}
			return bEquivalent;
		}
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageDemandExactExpansionTest,
		"NanoGS.Paged.DemandSelector.ExactSigmaScaleAndPixelGuard",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageDemandExactExpansionTest::RunTest(const FString& Parameters)
	{
		const uint64 LargePageId = static_cast<uint64>(MAX_uint32) + 17ull;
		const TArray<FPageRecord> Pages = {
			MakePage(LargePageId, {-2.0, -2.0, -2.0}, {2.0, 2.0, 2.0}),
			MakePage(3, {20.0, -1.0, -1.0}, {22.0, 1.0, 1.0}),
		};
		const TArray<FExactHQPageDemandView> Views = {
			MakeBoxView(100, {3.0, -5.0, -5.0}, {5.0, 5.0, 5.0}),
		};

		const FExactHQPageDemandResult DefaultResult =
			FPageDemandSelector::SelectExactHQPages(Pages, FTransform::Identity, Views);
		TestFalse(TEXT("Valid default selection does not fail open"), DefaultResult.HasFailOpenSelection());
		TestEqual(TEXT("One view result is returned"), DefaultResult.PerView.Num(), 1);
		TestEqual(TEXT("2-sigma metadata expands to the Exact 4-sigma footprint"), DefaultResult.PerView[0].RequiredPageIds.Num(), 1);
		if (DefaultResult.PerView[0].RequiredPageIds.Num() == 1)
		{
			TestEqual(TEXT("uint64 PageId is preserved"), DefaultResult.PerView[0].RequiredPageIds[0], LargePageId);
		}

		FExactHQPageDemandConfig ScaleConfig;
		ScaleConfig.SplatScale = 2.0;
		const TArray<FExactHQPageDemandView> ScaledViews = {
			MakeBoxView(101, {7.0, -5.0, -5.0}, {9.0, 5.0, 5.0}),
		};
		const FExactHQPageDemandResult ScaleResult =
			FPageDemandSelector::SelectExactHQPages(Pages, FTransform::Identity, ScaledViews, ScaleConfig);
		TestTrue(
			TEXT("SplatScale enlarges target sigma support before selection"),
			ScaleResult.PerView[0].RequiredPageIds.Contains(LargePageId));

		const TArray<FPageRecord> PointPages = {
			MakePage(55, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, 0.0),
		};
		const TArray<FExactHQPageDemandView> PixelGuardViews = {
			MakeBoxView(102, {0.75, -1.0, -1.0}, {2.0, 1.0, 1.0}, 0.5),
		};
		const FExactHQPageDemandResult PixelGuardResult =
			FPageDemandSelector::SelectExactHQPages(PointPages, FTransform::Identity, PixelGuardViews);
		TestTrue(
			TEXT("Default two-pixel raster guard prevents a boundary false negative"),
			PixelGuardResult.PerView[0].RequiredPageIds.Contains(55));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageDemandMultiViewTransformTest,
		"NanoGS.Paged.DemandSelector.MultiViewTransformAndSortedUnion",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageDemandMultiViewTransformTest::RunTest(const FString& Parameters)
	{
		const TArray<FPageRecord> Pages = {
			MakePage(90, {1.0, -0.5, -0.5}, {2.0, 0.5, 0.5}, 0.0),
			MakePage(7, {-6.0, -0.5, -0.5}, {-5.0, 0.5, 0.5}, 0.0),
			MakePage(42, {50.0, -0.5, -0.5}, {51.0, 0.5, 0.5}, 0.0),
		};

		// A mirrored local X maps page 90 into world [6,8], and page 7 into [20,22].
		const FTransform LocalToWorld(FQuat::Identity, FVector(10.0, 0.0, 0.0), FVector(-2.0, 1.0, 1.0));
		const TArray<FExactHQPageDemandView> Views = {
			MakeBoxView(1, {6.25, -2.0, -2.0}, {7.75, 2.0, 2.0}, 0.01),
			MakeBoxView(2, {19.5, -2.0, -2.0}, {22.5, 2.0, 2.0}, 0.01),
		};

		const FExactHQPageDemandResult Result =
			FPageDemandSelector::SelectExactHQPages(Pages, LocalToWorld, Views);
		TestEqual(TEXT("Both input views are represented"), Result.PerView.Num(), 2);
		TestEqual(TEXT("First mirrored page is selected by view one"), Result.PerView[0].RequiredPageIds.Num(), 1);
		TestEqual(TEXT("Second mirrored page is selected by view two"), Result.PerView[1].RequiredPageIds.Num(), 1);
		if (Result.PerView[0].RequiredPageIds.Num() == 1)
		{
			TestEqual(TEXT("View one selects PageId 90"), Result.PerView[0].RequiredPageIds[0], 90ull);
		}
		if (Result.PerView[1].RequiredPageIds.Num() == 1)
		{
			TestEqual(TEXT("View two selects PageId 7"), Result.PerView[1].RequiredPageIds[0], 7ull);
		}
		TestEqual(TEXT("Union contains both view demands"), Result.UnionRequiredPageIds.Num(), 2);
		if (Result.UnionRequiredPageIds.Num() == 2)
		{
			TestEqual(TEXT("Union is sorted deterministically"), Result.UnionRequiredPageIds[0], 7ull);
			TestEqual(TEXT("Union is sorted deterministically"), Result.UnionRequiredPageIds[1], 90ull);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageDemandFailOpenTest,
		"NanoGS.Paged.DemandSelector.InvalidInputsFailOpen",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageDemandFailOpenTest::RunTest(const FString& Parameters)
	{
		FPageRecord InvalidPage = MakePage(9, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
		InvalidPage.SupportBoundsMin.X = 2.0;
		const TArray<FPageRecord> Pages = {
			MakePage(7, {100.0, 100.0, 100.0}, {101.0, 101.0, 101.0}, 0.0),
			InvalidPage,
		};
		FExactHQPageDemandView InvalidView = MakeBoxView(2, {-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0});
		InvalidView.FrustumPlanes[0].Normal = FVector3d::ZeroVector;
		const TArray<FExactHQPageDemandView> Views = {
			MakeBoxView(1, {-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}),
			InvalidView,
		};

		const FExactHQPageDemandResult Result =
			FPageDemandSelector::SelectExactHQPages(Pages, FTransform::Identity, Views);
		TestTrue(
			TEXT("Invalid page is included even when valid page is outside"),
			Result.PerView[0].RequiredPageIds.Contains(9));
		TestFalse(
			TEXT("A valid separated page remains culled for a valid view"),
			Result.PerView[0].RequiredPageIds.Contains(7));
		TestEqual(TEXT("Invalid view includes every page"), Result.PerView[1].RequiredPageIds.Num(), 2);
		TestTrue(TEXT("Invalid page reason is reported"), EnumHasAnyFlags(Result.FailOpenReasons, EPageDemandFailOpenReason::InvalidPage));
		TestTrue(TEXT("Invalid view reason is reported"), EnumHasAnyFlags(Result.FailOpenReasons, EPageDemandFailOpenReason::InvalidView));

		FTransform InvalidTransform = FTransform::Identity;
		const double NaN = std::numeric_limits<double>::quiet_NaN();
		InvalidTransform.SetTranslation(FVector(NaN, NaN, NaN));
		const FExactHQPageDemandResult TransformResult =
			FPageDemandSelector::SelectExactHQPages(Pages, InvalidTransform, Views);
		TestEqual(TEXT("Invalid transform includes all pages in union"), TransformResult.UnionRequiredPageIds.Num(), 2);
		TestTrue(
			TEXT("Invalid transform is reported"),
			EnumHasAnyFlags(TransformResult.FailOpenReasons, EPageDemandFailOpenReason::InvalidTransform));

		FExactHQPageDemandConfig InvalidConfig;
		InvalidConfig.ExactSupportSigma = 2.0;
		const FExactHQPageDemandResult ConfigResult =
			FPageDemandSelector::SelectExactHQPages(Pages, FTransform::Identity, Views, InvalidConfig);
		TestEqual(TEXT("Unsafe sub-4-sigma config includes all pages"), ConfigResult.UnionRequiredPageIds.Num(), 2);
		TestTrue(
			TEXT("Invalid config is reported"),
			EnumHasAnyFlags(ConfigResult.FailOpenReasons, EPageDemandFailOpenReason::InvalidConfig));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageDemandDynamicResolutionGuardTest,
		"NanoGS.Paged.DemandSelector.DynamicResolutionPixelGuard",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageDemandDynamicResolutionGuardTest::RunTest(const FString& Parameters)
	{
		double MaxCentimetersPerPixel = 0.0;
		TestTrue(
			TEXT("Perspective pixel bound accepts valid actual and unscaled rectangles"),
			FPageDemandSelector::CalculateConservativeMaxWorldCentimetersPerPixel(
				FIntPoint(1920, 1080),
				FIntPoint(960, 540),
				1.0,
				1.0,
				true,
				1000.0,
				MaxCentimetersPerPixel));
		TestTrue(
			TEXT("50 percent raster size doubles the conservative cm-per-pixel bound"),
			FMath::IsNearlyEqual(MaxCentimetersPerPixel, 1000.0 / 270.0, 1.e-12));

		TestTrue(
			TEXT("Orthographic bound uses the same smaller actual raster focal count"),
			FPageDemandSelector::CalculateConservativeMaxWorldCentimetersPerPixel(
				FIntPoint(1920, 1080),
				FIntPoint(960, 540),
				2.0,
				2.0,
				false,
				0.0,
				MaxCentimetersPerPixel));
		TestTrue(
			TEXT("Orthographic result is derived without a depth guess"),
			FMath::IsNearlyEqual(MaxCentimetersPerPixel, 1.0 / 540.0, 1.e-12));

		TestFalse(
			TEXT("Missing actual raster size fails instead of underestimating the guard"),
			FPageDemandSelector::CalculateConservativeMaxWorldCentimetersPerPixel(
				FIntPoint(1920, 1080),
				FIntPoint::ZeroValue,
				1.0,
				1.0,
				true,
				1000.0,
				MaxCentimetersPerPixel));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageDemandConservativeSceneBoundsTest,
		"NanoGS.Paged.DemandSelector.ConservativeSceneBounds",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageDemandConservativeSceneBoundsTest::RunTest(const FString& Parameters)
	{
		FVector3d ExpandedMin;
		FVector3d ExpandedMax;
		TestTrue(
			TEXT("Negative SplatScale expands by its magnitude"),
			FPageDemandSelector::CalculateConservativeLocalSupportBounds(
				FVector3d(-10.0, -20.0, -30.0),
				FVector3d(10.0, 20.0, 30.0),
				2.0,
				5.0,
				4.0,
				-2.0,
				ExpandedMin,
				ExpandedMax));
		TestTrue(TEXT("Effective 8-sigma adds six maximum scales to min"), ExpandedMin.Equals(FVector3d(-40.0, -50.0, -60.0), 1.e-12));
		TestTrue(TEXT("Effective 8-sigma adds six maximum scales to max"), ExpandedMax.Equals(FVector3d(40.0, 50.0, 60.0), 1.e-12));

		TestTrue(
			TEXT("A small runtime scale never shrinks validated stored support"),
			FPageDemandSelector::CalculateConservativeLocalSupportBounds(
				FVector3d(-1.0), FVector3d(1.0), 2.0, 5.0, 4.0, 0.25,
				ExpandedMin, ExpandedMax));
		TestTrue(TEXT("Stored min remains unchanged"), ExpandedMin.Equals(FVector3d(-1.0), 1.e-12));
		TestTrue(TEXT("Stored max remains unchanged"), ExpandedMax.Equals(FVector3d(1.0), 1.e-12));

		TestFalse(
			TEXT("Non-finite runtime scale fails instead of under-bounding"),
			FPageDemandSelector::CalculateConservativeLocalSupportBounds(
				FVector3d(-1.0), FVector3d(1.0), 2.0, 5.0, 4.0,
				std::numeric_limits<double>::quiet_NaN(), ExpandedMin, ExpandedMax));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSPageDemandHierarchyEquivalenceTest,
		"NanoGS.Paged.DemandSelector.HierarchyExactEquivalence",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSPageDemandHierarchyEquivalenceTest::RunTest(const FString& Parameters)
	{
		TArray<FPageRecord> Pages;
		Pages.Reserve(4099);
		FRandomStream Random(0x4e475348);
		for (int32 PageIndex = 0; PageIndex < 4096; ++PageIndex)
		{
			const FVector3d Center(
				Random.FRandRange(-50000.0f, 50000.0f),
				Random.FRandRange(-50000.0f, 50000.0f),
				Random.FRandRange(-5000.0f, 5000.0f));
			const FVector3d Extent(
				Random.FRandRange(0.0f, 80.0f),
				Random.FRandRange(0.0f, 80.0f),
				Random.FRandRange(0.0f, 30.0f));
			// Deliberately reverse PageIds so hierarchy topology/traversal order cannot
			// accidentally masquerade as the required deterministic sorted output.
			Pages.Add(MakePage(
				900000ull - static_cast<uint64>(PageIndex) * 7ull,
				Center - Extent,
				Center + Extent,
				Random.FRandRange(0.0f, 12.0f)));
		}

		// The hierarchy must preserve duplicate-id unique sorting and every fail-open
		// path, including a valid record whose runtime expansion overflows.
		Pages.Add(MakePage(Pages[17].PageId, {-2.0, -2.0, -2.0}, {2.0, 2.0, 2.0}, 0.0));
		FPageRecord InvalidPage = MakePage(17, {100000.0, 100000.0, 100000.0}, {100001.0, 100001.0, 100001.0});
		InvalidPage.SupportBoundsMin.X = InvalidPage.SupportBoundsMax.X + 1.0;
		Pages.Add(InvalidPage);
		Pages.Add(MakePage(
			static_cast<uint64>(MAX_uint32) + 91ull,
			{-1.0, -1.0, -1.0},
			{1.0, 1.0, 1.0},
			TNumericLimits<double>::Max()));

		FString Error;
		FPageDemandHierarchy Hierarchy;
		TestTrue(TEXT("Hierarchy initializes over borrowed immutable Page metadata"), Hierarchy.Initialize(Pages, Error));
		TestTrue(TEXT("Hierarchy initialization returns no error"), Error.IsEmpty());

		TArray<FExactHQPageDemandView> Views = {
			MakeBoxView(101, {-12000.0, -8000.0, -1000.0}, {14000.0, 9000.0, 2000.0}, 0.25),
			MakeBoxView(102, {25000.0, -45000.0, -3500.0}, {49000.0, -12000.0, 3500.0}, 0.5),
			MakeBoxView(103, {-60000.0, -60000.0, -7000.0}, {60000.0, 60000.0, 7000.0}, 0.05),
		};
		FExactHQPageDemandView InvalidView = MakeBoxView(
			104, FVector3d(-1.0), FVector3d(1.0));
		InvalidView.FrustumPlanes[2].Offset = std::numeric_limits<double>::quiet_NaN();
		Views.Add(InvalidView);

		FExactHQPageDemandConfig Config;
		Config.SplatScale = -1.75;
		Config.AdditionalLocalGuardCentimeters = 0.125;
		Config.NumericalGuardCentimeters = 1.e-3;
		const FTransform Transform(
			FQuat(FVector::UpVector, FMath::DegreesToRadians(31.0)),
			FVector(12345.0, -4567.0, 890.0),
			FVector(-1.5, 0.75, 2.0));

		auto Compare = [this, &Hierarchy, &Pages](
			const FString& Label,
			const FTransform& QueryTransform,
			const TArray<FExactHQPageDemandView>& QueryViews,
			const FExactHQPageDemandConfig& QueryConfig)
		{
			const FExactHQPageDemandResult Linear =
				FPageDemandSelector::SelectExactHQPages(
					Pages, QueryTransform, QueryViews, QueryConfig);
			const FExactHQPageDemandResult Accelerated =
				Hierarchy.SelectExactHQPages(QueryTransform, QueryViews, QueryConfig);
			return TestEquivalentResults(*this, Label, Linear, Accelerated);
		};

		Compare(TEXT("first transformed multi-view query"), Transform, Views, Config);
		Compare(TEXT("identical query reuses prepared bounds"), Transform, Views, Config);

		TArray<FExactHQPageDemandView> NarrowViews = {
			MakeBoxView(201, {-100.0, -100.0, -100.0}, {100.0, 100.0, 100.0}, 0.01),
		};
		Compare(TEXT("new view reuses hierarchy bounds"), Transform, NarrowViews, Config);

		FExactHQPageDemandConfig ChangedConfig = Config;
		ChangedConfig.SplatScale = 0.25;
		Compare(TEXT("scale change refits exact page bounds"), Transform, Views, ChangedConfig);

		const FTransform ChangedTransform(
			FQuat(FVector::RightVector, FMath::DegreesToRadians(-17.0)),
			FVector(-9000.0, 7000.0, 1200.0),
			FVector(0.5, -2.0, 1.25));
		Compare(TEXT("transform change refits exact page bounds"), ChangedTransform, Views, ChangedConfig);

		const TArray<FExactHQPageDemandView> EmptyViews;
		Compare(TEXT("empty view list preserves fail-open union semantics"), Transform, EmptyViews, Config);

		FExactHQPageDemandConfig InvalidConfig = Config;
		InvalidConfig.ExactSupportSigma = 3.0;
		Compare(TEXT("invalid config matches linear fail-open"), Transform, Views, InvalidConfig);

		FTransform InvalidTransform = Transform;
		InvalidTransform.SetScale3D(FVector(
			std::numeric_limits<double>::quiet_NaN(), 1.0, 1.0));
		Compare(TEXT("invalid transform matches linear fail-open"), InvalidTransform, Views, Config);
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
