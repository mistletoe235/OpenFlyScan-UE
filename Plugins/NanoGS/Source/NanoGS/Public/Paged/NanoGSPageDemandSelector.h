// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Paged/NanoGSPageFormat.h"

/**
 * Renderer-independent, conservative view-demand selection for Exact-HQ Page v1 data.
 *
 * Page v1 stores the union of the rotated per-splat 2-sigma AABBs. Those bounds
 * are not sufficient for Exact-HQ frustum demand on their own: the rasterizer
 * uses a wider footprint, SplatScale can enlarge it, and its screen-space
 * covariance floor needs a pixel guard. This selector expands the local bounds
 * for all three effects before testing them against each view.
 *
 * This is deliberately only a demand selector. It performs no IO, residency,
 * occlusion, ordering, or LOD selection.
 */
namespace NanoGS::Paged
{
	/**
	 * One world-space half-space in the unambiguous form
	 * Dot(Normal, WorldPosition) + Offset >= 0.
	 *
	 * Normal need not be normalized. An invalid or zero normal makes the entire
	 * view fail open.
	 */
	struct NANOGS_API FPageDemandPlane
	{
		FVector3d Normal = FVector3d::ZeroVector;
		double Offset = 0.0;
	};

	/**
	 * A conservative convex world-space view volume.
	 *
	 * FrustumPlanes may contain five planes for an infinite-far projection or six
	 * for a finite frustum. More generally, any non-empty set of half-spaces is
	 * accepted if their intersection conservatively contains the raster domain.
	 * Omitting a plane only over-selects pages. Supplying an incorrectly oriented
	 * plane violates the caller contract and cannot be inferred by this class.
	 */
	struct NANOGS_API FExactHQPageDemandView
	{
		uint64 ViewId = 0;
		TArray<FPageDemandPlane> FrustumPlanes;

		/**
		 * Conservative upper bound on world centimeters covered by one pixel over
		 * this view's complete demand domain. For a perspective view the caller can
		 * derive this from the farthest permitted demand distance. Required to be
		 * finite and positive when RasterGuardPixels is positive.
		 */
		double MaxWorldCentimetersPerPixel = 0.0;

		/** Extra world-space guard for caller-specific reconstruction/filter support. */
		double AdditionalWorldGuardCentimeters = 0.0;
	};

	struct NANOGS_API FExactHQPageDemandConfig
	{
		/** Page v1 stores 2 sigma; Exact-HQ defaults to a conservative 4-sigma support. */
		double ExactSupportSigma = 4.0;

		/** Runtime component SplatScale. Its magnitude scales Gaussian standard deviations. */
		double SplatScale = 1.0;

		/** Guard for raster covariance filtering and boundary round-off. */
		double RasterGuardPixels = 2.0;

		/** Optional additional local-space guard applied before LocalToWorld. */
		double AdditionalLocalGuardCentimeters = 0.0;

		/** Small final world-space guard against floating-point boundary cancellation. */
		double NumericalGuardCentimeters = 1.e-4;
	};

	enum class EPageDemandFailOpenReason : uint8
	{
		None = 0,
		InvalidConfig = 1u << 0u,
		InvalidTransform = 1u << 1u,
		InvalidView = 1u << 2u,
		InvalidPage = 1u << 3u,
		NonFiniteCalculation = 1u << 4u,
	};

	ENUM_CLASS_FLAGS(EPageDemandFailOpenReason);

	struct NANOGS_API FExactHQPageDemandViewResult
	{
		uint64 ViewId = 0;
		EPageDemandFailOpenReason FailOpenReasons = EPageDemandFailOpenReason::None;

		/** Sorted unique uint64 PageIds required by this view. */
		TArray<uint64> RequiredPageIds;
	};

	struct NANOGS_API FExactHQPageDemandResult
	{
		/** Results preserve input-view order; every PageId array is sorted and unique. */
		TArray<FExactHQPageDemandViewResult> PerView;

		/** Sorted unique union of every view's requirement. */
		TArray<uint64> UnionRequiredPageIds;

		EPageDemandFailOpenReason FailOpenReasons = EPageDemandFailOpenReason::None;

		bool HasFailOpenSelection() const
		{
			return FailOpenReasons != EPageDemandFailOpenReason::None;
		}
	};

	class NANOGS_API FPageDemandSelector
	{
	public:
		/**
		 * Derive a conservative world-centimeters-per-pixel upper bound. Dynamic
		 * resolution is handled by using the smaller focal-pixel count from the
		 * unscaled and actual raster rectangles. Returns false instead of guessing.
		 */
		static bool CalculateConservativeMaxWorldCentimetersPerPixel(
			FIntPoint UnscaledViewSize,
			FIntPoint RasterViewSize,
			double ProjectionScaleX,
			double ProjectionScaleY,
			bool bPerspective,
			double MaxPositiveDepthCentimeters,
			double& OutMaxWorldCentimetersPerPixel);

		/** Expand stored local support bounds without ever shrinking them. */
		static bool CalculateConservativeLocalSupportBounds(
			const FVector3d& StoredSupportMin,
			const FVector3d& StoredSupportMax,
			double StoredSupportSigma,
			double MaxScaleCentimeters,
			double ExactSupportSigma,
			double SplatScale,
			FVector3d& OutSupportMin,
			FVector3d& OutSupportMax);

		/**
		 * Select every Page v1 payload whose conservatively expanded support may
		 * intersect any supplied view.
		 *
		 * Safety policy:
		 * - invalid config or LocalToWorld includes every page for every view;
		 * - an invalid view includes every page for that view;
		 * - an invalid page record is included in every view;
		 * - a non-finite intermediate calculation includes the affected page.
		 *
		 * Consequently bad metadata can reduce streaming efficiency but cannot
		 * silently create an Exact-HQ hole.
		 */
		static FExactHQPageDemandResult SelectExactHQPages(
			TConstArrayView<FPageRecord> Pages,
			const FTransform& LocalToWorld,
			TConstArrayView<FExactHQPageDemandView> Views,
			const FExactHQPageDemandConfig& Config = FExactHQPageDemandConfig());
	};

	/**
	 * Runtime hierarchy for repeated Exact-HQ demand queries over immutable Page v1
	 * metadata.
	 *
	 * Initialize builds only deterministic topology.  The first query for a
	 * LocalToWorld/config pair computes the exact same expanded world AABB used by
	 * FPageDemandSelector for every valid page and refits the hierarchy.  Later
	 * queries with the identical pair use hierarchy bounds for rejection, then run
	 * the same per-page intersection test at every reached leaf.  Hierarchy bounds
	 * may therefore over-traverse, but can never intentionally alter the selected
	 * PageId set or its deterministic ordering.
	 *
	 * The page storage is borrowed and must remain at the same address, with the
	 * same contents, until Reset or destruction.  Query/prepare operations mutate
	 * a cache and are intentionally not thread-safe; the renderer owns one instance
	 * and accesses it only on the render thread.
	 */
	class NANOGS_API FPageDemandHierarchy
	{
	public:
		FPageDemandHierarchy() = default;

		bool Initialize(TConstArrayView<FPageRecord> Pages, FString& OutError);
		void Reset();

		bool IsInitialized() const { return bInitialized; }

		/**
		 * Prepare/cache the linear selector's exact per-page world bounds.  This also
		 * caches the eight transformed corners of the legacy conservative scene box
		 * used to derive the perspective pixel guard in FPageRenderData.
		 */
		bool Prepare(
			const FTransform& LocalToWorld,
			const FExactHQPageDemandConfig& Config);

		/** Copy the prepared legacy scene corners; returns false if they are unsafe. */
		bool GetPreparedSceneWorldCorners(FVector3d OutWorldCorners[8]) const;

		/** Hierarchy-accelerated equivalent of FPageDemandSelector::SelectExactHQPages. */
		FExactHQPageDemandResult SelectExactHQPages(
			const FTransform& LocalToWorld,
			TConstArrayView<FExactHQPageDemandView> Views,
			const FExactHQPageDemandConfig& Config = FExactHQPageDemandConfig());

	private:
		struct FNode
		{
			FVector3d WorldMin = FVector3d::ZeroVector;
			FVector3d WorldMax = FVector3d::ZeroVector;
			int32 FirstPageOrdinal = 0;
			int32 PageOrdinalCount = 0;
			int32 LeftChild = INDEX_NONE;
			int32 RightChild = INDEX_NONE;
			bool bHasSelectableBounds = false;
			bool bCanCull = false;

			bool IsLeaf() const { return LeftChild == INDEX_NONE; }
		};

		struct FPreparedPageBounds
		{
			FVector3d WorldMin = FVector3d::ZeroVector;
			FVector3d WorldMax = FVector3d::ZeroVector;
			bool bValid = false;
		};

		int32 BuildNode(int32 FirstPageOrdinal, int32 PageOrdinalCount);
		void RefitNode(int32 NodeIndex);
		bool CacheKeyEquals(
			const FTransform& LocalToWorld,
			const FExactHQPageDemandConfig& Config) const;

		TConstArrayView<FPageRecord> BorrowedPages;
		TArray<uint64> SortedAllPageIds;
		TArray<int32> ValidPageOrdinals;
		TArray<int32> InvalidPageOrdinals;
		TArray<FNode> Nodes;
		TArray<FPreparedPageBounds> PreparedPageBounds;
		TArray<int32> NonFinitePageOrdinals;

		FTransform PreparedLocalToWorld = FTransform::Identity;
		FExactHQPageDemandConfig PreparedConfig;
		FVector3d PreparedSceneWorldCorners[8]{};
		bool bInitialized = false;
		bool bPrepared = false;
		bool bPreparedSceneWorldCornersValid = false;
	};
}
