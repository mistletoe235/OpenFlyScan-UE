// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GaussianDataTypes.h"
#include <atomic>
#include "GaussianSplatComponent.generated.h"

class UGaussianSplatAsset;
class UNanoGSPagedSourceAsset;
class UNanoGSSpatialLODSourceAsset;
class UNanoGSTreeSourceAsset;
class FGaussianSplatSceneProxy;

struct FNanoGSTreeRuntimeStats
{
	std::atomic<int32> SelectedNodeCount{0};
	std::atomic<int32> ExactLeafNodeCount{0};
	std::atomic<int32> MinTreeDepth{-1};
	std::atomic<int32> MaxTreeDepth{-1};
	std::atomic<int32> ActiveSplatCount{0};
	std::atomic<int32> LoadedPageCount{0};
	std::atomic<int32> RequestedPageCount{0};
	std::atomic<bool> bFullDetailActive{false};
};

/**
 * Component for rendering Gaussian Splatting assets in the scene
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent), hidecategories = (Collision, Physics, Navigation))
class NANOGS_API UGaussianSplatComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer);

	//~ Begin UObject Interface
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyThatWillChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UObject Interface

	//~ Begin UActorComponent Interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void SendRenderDynamicData_Concurrent() override;
	//~ End UActorComponent Interface

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	//~ End UPrimitiveComponent Interface

	/** Set the Gaussian Splat asset to render */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	void SetSplatAsset(UGaussianSplatAsset* NewAsset);

	/** Get the currently assigned asset */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	UGaussianSplatAsset* GetSplatAsset() const { return SplatAsset; }

	/** Select a Page v1 external source. This clears the monolithic UAsset source. */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Paged")
	void SetPagedSourceAsset(UNanoGSPagedSourceAsset* NewAsset);

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Paged")
	UNanoGSPagedSourceAsset* GetPagedSourceAsset() const { return PagedSourceAsset; }

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Spatial LOD")
	void SetSpatialLODSourceAsset(UNanoGSSpatialLODSourceAsset* NewAsset);

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Spatial LOD")
	UNanoGSSpatialLODSourceAsset* GetSpatialLODSourceAsset() const { return SpatialLODSourceAsset; }

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Tree LOD")
	void SetTreeSourceAsset(UNanoGSTreeSourceAsset* NewAsset);

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Tree LOD")
	UNanoGSTreeSourceAsset* GetTreeSourceAsset() const { return TreeSourceAsset; }

	/** Get the number of splats being rendered */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	int32 GetSplatCount() const;

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Rendering")
	void SetOpacityScale(float NewOpacityScale);

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Tree LOD")
	void SetForceFullDetailLOD0(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Tree LOD")
	void SetTreeLODSplatBudget(int32 NewBudget);

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Tree LOD")
	void SetTreeLODProgressiveSplatBudget(int32 NewBudget);

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Tree LOD")
	void SetTreeLODDetailScale(float NewScale);

	TSharedPtr<FNanoGSTreeRuntimeStats, ESPMode::ThreadSafe> GetTreeRuntimeStats() const { return TreeRuntimeStats; }

public:
	/** The Gaussian Splat asset to render */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting")
	TObjectPtr<UGaussianSplatAsset> SplatAsset;

	/**
	 * Metadata-only reference to an external .ngsp Page v1 container. When set,
	 * this Exact-HQ source takes precedence over SplatAsset and is uploaded a
	 * complete page at a time without constructing a multi-gigabyte UObject.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Paged")
	TObjectPtr<UNanoGSPagedSourceAsset> PagedSourceAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Spatial LOD")
	TObjectPtr<UNanoGSSpatialLODSourceAsset> SpatialLODSourceAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Tree LOD")
	TObjectPtr<UNanoGSTreeSourceAsset> TreeSourceAsset;

	/** Spherical Harmonic order to use for rendering (0-3). Higher = more color detail but slower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Quality", meta = (ClampMin = "0", ClampMax = "3", DisplayName = "SH Order"))
	int32 SHOrder = 3;

	/** Global opacity multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetOpacityScale, Category = "Gaussian Splatting|Rendering", meta = (ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0", NoSpinbox = "true", DisplayName = "Opacity"))
	float OpacityScale = 1.0f;

	/** Scale multiplier for splat sizes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Rendering", meta = (ClampMin = "0.1", ClampMax = "10.0", UIMin = "0.25", UIMax = "3.0", DisplayName = "Splat Size"))
	float SplatScale = 1.0f;

	/** Projected error threshold for LOD selection (resolution-independent, like Nanite).
	 *  Lower values = more conservative (keep detail longer, less LOD savings)
	 *  Higher values = more aggressive (switch to LOD sooner, better performance)
	 *  Uses projection-space units. ~0.03 ≈ 32 pixels at 1080p with 90° FOV. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Performance", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float LODErrorThreshold = 0.03f;

	/** Debug/quality comparison: request every exact leaf (LOD0), ignoring the dynamic Tree LOD budget and frustum. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetForceFullDetailLOD0, Category = "Gaussian Splatting|Tree LOD", meta = (DisplayName = "Force Full Detail LOD0"))
	bool bForceFullDetailLOD0 = false;

	/** Maximum regular-view Tree frontier size. Matches Spark's Splat Budget control. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetTreeLODSplatBudget, Category = "Gaussian Splatting|Tree LOD", meta = (ClampMin = "100000", ClampMax = "30000000", UIMin = "100000", UIMax = "10000000", Delta = "100000", NoSpinbox = "true", DisplayName = "Splat Budget"))
	int32 TreeLODSplatBudget = 10000000;

	/** First exact frontier shown while the camera is moving. 0 disables progressive publication. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetTreeLODProgressiveSplatBudget, Category = "Gaussian Splatting|Tree LOD", meta = (ClampMin = "0", ClampMax = "30000000", UIMin = "0", UIMax = "10000000", Delta = "100000", NoSpinbox = "true", DisplayName = "Moving Progressive Budget"))
	int32 TreeLODProgressiveSplatBudget = 4000000;

	/** Relative Tree refinement detail. Higher values retain finer nodes, like Spark's LoD Detail control. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetTreeLODDetailScale, Category = "Gaussian Splatting|Tree LOD", meta = (ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.1", UIMax = "3.0", Delta = "0.05", NoSpinbox = "true", DisplayName = "LOD Detail Scale"))
	float TreeLODDetailScale = 3.0f;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Gaussian Splatting|Tree LOD Status", meta = (DisplayName = "Current LOD"))
	FString TreeLODStatus = TEXT("Inactive");

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Gaussian Splatting|Tree LOD Status")
	int32 SelectedTreeNodes = 0;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Gaussian Splatting|Tree LOD Status", meta = (DisplayName = "Exact LOD0 Nodes"))
	int32 SelectedLOD0Nodes = 0;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Gaussian Splatting|Tree LOD Status")
	int32 ActiveSplats = 0;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Gaussian Splatting|Tree LOD Status")
	FString ResidentPages = TEXT("0 / 0 requested");

protected:
	/** Called when the asset changes */
	void OnAssetChanged();

	/** Mark the render state as dirty */
	void MarkRenderStateDirty();

	/** Called when the asset's data changes (e.g., Nanite enabled/disabled) */
	void OnAssetDataChanged(class UGaussianSplatAsset* ChangedAsset);

	/** Subscribe to asset change notifications */
	void SubscribeToAssetChanges();

	/** Unsubscribe from asset change notifications */
	void UnsubscribeFromAssetChanges();

private:
	/** Cached bounds */
	mutable FBoxSphereBounds CachedBounds;
	mutable bool bBoundsCached = false;

	/** Delegate handle for asset change subscription */
	FDelegateHandle AssetChangedDelegateHandle;
	TSharedPtr<FNanoGSTreeRuntimeStats, ESPMode::ThreadSafe> TreeRuntimeStats;
	double SpatialPerfElapsedSeconds = 0.0;
	int32 SpatialPerfFrames = 0;
};
