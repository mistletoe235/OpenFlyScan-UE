// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "Paged/NanoGSPagedSourceAsset.h"
#include "Paged/NanoGSSpatialLODSourceAsset.h"
#include "Paged/NanoGSTreeSourceAsset.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/World.h"

UGaussianSplatComponent::UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TreeRuntimeStats = MakeShared<FNanoGSTreeRuntimeStats, ESPMode::ThreadSafe>();
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	bTickInEditor = true;
	bAllowReregistration = false;

	bUseAsOccluder = false;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);

	// Enable dynamic rendering
	Mobility = EComponentMobility::Movable;
}

void UGaussianSplatComponent::PostLoad()
{
	Super::PostLoad();
}

#if WITH_EDITOR
void UGaussianSplatComponent::PreEditChange(FProperty* PropertyThatWillChange)
{
	if (PropertyThatWillChange)
	{
		const FName PropertyName = PropertyThatWillChange->GetFName();
		if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, OpacityScale) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TreeLODSplatBudget) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TreeLODProgressiveSplatBudget) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TreeLODDetailScale))
		{
			return;
		}
	}

	Super::PreEditChange(PropertyThatWillChange);
}

void UGaussianSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	const bool bOpacityOnlyChange =
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, OpacityScale);
	const bool bTreeLODControlChange =
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TreeLODSplatBudget) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TreeLODProgressiveSplatBudget) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TreeLODDetailScale);

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatAsset))
	{
		if (SplatAsset)
		{
			PagedSourceAsset = nullptr;
			SpatialLODSourceAsset = nullptr;
			TreeSourceAsset = nullptr;
		}
		OnAssetChanged();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PagedSourceAsset))
	{
		if (PagedSourceAsset)
		{
			UnsubscribeFromAssetChanges();
			SplatAsset = nullptr;
			TreeSourceAsset = nullptr;
		}
		OnAssetChanged();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialLODSourceAsset))
	{
		if (SpatialLODSourceAsset)
		{
			UnsubscribeFromAssetChanges();
			SplatAsset = nullptr;
			PagedSourceAsset = nullptr;
			TreeSourceAsset = nullptr;
		}
		OnAssetChanged();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TreeSourceAsset))
	{
		if (TreeSourceAsset)
		{
			UnsubscribeFromAssetChanges();
			SplatAsset = nullptr;
			PagedSourceAsset = nullptr;
			SpatialLODSourceAsset = nullptr;
		}
		OnAssetChanged();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SHOrder) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, OpacityScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LODErrorThreshold) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bForceFullDetailLOD0) ||
			 bTreeLODControlChange)
	{
		if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale))
		{
			bBoundsCached = false;
			UpdateBounds();
		}
		MarkRenderDynamicDataDirty();
	}

	if (bOpacityOnlyChange || bTreeLODControlChange)
	{
		return;
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

void UGaussianSplatComponent::OnRegister()
{
	Super::OnRegister();

	if (SplatAsset || (PagedSourceAsset && PagedSourceAsset->HasValidMetadata()) ||
		(SpatialLODSourceAsset && SpatialLODSourceAsset->HasValidMetadata()) ||
		(TreeSourceAsset && TreeSourceAsset->HasValidMetadata()))
	{
		bBoundsCached = false;
		SubscribeToAssetChanges();
	}
}

void UGaussianSplatComponent::OnUnregister()
{
	UnsubscribeFromAssetChanges();
	Super::OnUnregister();
}

void UGaussianSplatComponent::SetOpacityScale(const float NewOpacityScale)
{
	const float ClampedOpacityScale = FMath::Clamp(NewOpacityScale, 0.0f, 2.0f);
	if (FMath::IsNearlyEqual(OpacityScale, ClampedOpacityScale))
		return;
	OpacityScale = ClampedOpacityScale;
	MarkRenderDynamicDataDirty();
}

void UGaussianSplatComponent::SetTreeLODSplatBudget(const int32 NewBudget)
{
	const int32 ClampedBudget = FMath::Clamp(NewBudget, 100000, 30000000);
	if (TreeLODSplatBudget == ClampedBudget)
		return;
	TreeLODSplatBudget = ClampedBudget;
	MarkRenderDynamicDataDirty();
}

void UGaussianSplatComponent::SetTreeLODProgressiveSplatBudget(const int32 NewBudget)
{
	const int32 ClampedBudget = FMath::Clamp(NewBudget, 0, 30000000);
	if (TreeLODProgressiveSplatBudget == ClampedBudget)
		return;
	TreeLODProgressiveSplatBudget = ClampedBudget;
	MarkRenderDynamicDataDirty();
}

void UGaussianSplatComponent::SetTreeLODDetailScale(const float NewScale)
{
	const float ClampedScale = FMath::Clamp(NewScale, 0.1f, 4.0f);
	if (FMath::IsNearlyEqual(TreeLODDetailScale, ClampedScale))
		return;
	TreeLODDetailScale = ClampedScale;
	MarkRenderDynamicDataDirty();
}

void UGaussianSplatComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();
	if (!SceneProxy)
		return;
	FGaussianSplatSceneProxy* Proxy = static_cast<FGaussianSplatSceneProxy*>(SceneProxy);
	const int32 NewSHOrder = SHOrder;
	const float NewOpacityScale = OpacityScale;
	const float NewSplatScale = SplatScale;
	const float NewLODErrorThreshold = LODErrorThreshold;
	const bool bNewForceFullDetailLOD0 = bForceFullDetailLOD0;
	const int32 NewTreeLODSplatBudget = TreeLODSplatBudget;
	const int32 NewTreeLODProgressiveSplatBudget = TreeLODProgressiveSplatBudget;
	const float NewTreeLODDetailScale = TreeLODDetailScale;
	ENQUEUE_RENDER_COMMAND(UpdateNanoGSRenderingParameters)(
		[Proxy, NewSHOrder, NewOpacityScale, NewSplatScale, NewLODErrorThreshold,
		 bNewForceFullDetailLOD0, NewTreeLODSplatBudget,
		 NewTreeLODProgressiveSplatBudget, NewTreeLODDetailScale](FRHICommandListImmediate& RHICmdList)
		{
			Proxy->SetRenderingParameters_RenderThread(
				NewSHOrder, NewOpacityScale, NewSplatScale, NewLODErrorThreshold,
				bNewForceFullDetailLOD0,
				NewTreeLODSplatBudget, NewTreeLODProgressiveSplatBudget,
				NewTreeLODDetailScale);
		});
}

void UGaussianSplatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (TreeSourceAsset && TreeRuntimeStats.IsValid())
	{
		SelectedTreeNodes = TreeRuntimeStats->SelectedNodeCount.load(std::memory_order_acquire);
		SelectedLOD0Nodes = TreeRuntimeStats->ExactLeafNodeCount.load(std::memory_order_acquire);
		ActiveSplats = TreeRuntimeStats->ActiveSplatCount.load(std::memory_order_acquire);
		const int32 MinDepth = TreeRuntimeStats->MinTreeDepth.load(std::memory_order_acquire);
		const int32 MaxDepth = TreeRuntimeStats->MaxTreeDepth.load(std::memory_order_acquire);
		const int32 LoadedPages = TreeRuntimeStats->LoadedPageCount.load(std::memory_order_acquire);
		const int32 RequestedPages = TreeRuntimeStats->RequestedPageCount.load(std::memory_order_acquire);
		const bool bFullDetailActive = TreeRuntimeStats->bFullDetailActive.load(std::memory_order_acquire);
		TreeLODStatus = bFullDetailActive
			? TEXT("Full Detail LOD0 (all exact leaves)")
			: (bForceFullDetailLOD0
				? TEXT("Full Detail LOD0 (loading)")
				: (MinDepth >= 0 ? FString::Printf(TEXT("Dynamic Tree LOD (depth %d-%d)"), MinDepth, MaxDepth) : TEXT("Dynamic Tree LOD (waiting)")));
		ResidentPages = FString::Printf(TEXT("%d loaded / %d requested"), LoadedPages, RequestedPages);
	}
	else
	{
		TreeLODStatus = TEXT("Inactive");
	}
	if (SpatialLODSourceAsset && FParse::Param(FCommandLine::Get(), TEXT("NanoGSSpatialLODPerf")))
	{
		SpatialPerfElapsedSeconds += DeltaTime;
		++SpatialPerfFrames;
		if (SpatialPerfElapsedSeconds >= 2.0)
		{
			UE_LOG(LogTemp, Log, TEXT("NanoGS SpatialLOD perf: game_fps=%.2f frames=%d seconds=%.3f"),
				SpatialPerfFrames / SpatialPerfElapsedSeconds, SpatialPerfFrames, SpatialPerfElapsedSeconds);
			SpatialPerfElapsedSeconds = 0.0;
			SpatialPerfFrames = 0;
		}
	}
	else if (TreeSourceAsset && FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
	{
		SpatialPerfElapsedSeconds += DeltaTime;
		++SpatialPerfFrames;
		if (SpatialPerfElapsedSeconds >= 2.0)
		{
			UE_LOG(LogTemp, Log, TEXT("NanoGS TreeLOD perf: game_fps=%.2f frames=%d seconds=%.3f"),
				SpatialPerfFrames / SpatialPerfElapsedSeconds, SpatialPerfFrames, SpatialPerfElapsedSeconds);
			SpatialPerfElapsedSeconds = 0.0;
			SpatialPerfFrames = 0;
		}
	}
}

void UGaussianSplatComponent::SetForceFullDetailLOD0(bool bEnabled)
{
	if (bForceFullDetailLOD0 == bEnabled)
		return;
	bForceFullDetailLOD0 = bEnabled;
	if (TreeRuntimeStats.IsValid())
		TreeRuntimeStats->bFullDetailActive.store(false, std::memory_order_release);
	MarkRenderDynamicDataDirty();
}

FPrimitiveSceneProxy* UGaussianSplatComponent::CreateSceneProxy()
{
	const bool bHasPagedSource = PagedSourceAsset && PagedSourceAsset->HasValidMetadata();
	const bool bHasSpatialSource = SpatialLODSourceAsset && SpatialLODSourceAsset->HasValidMetadata();
	const bool bHasTreeSource = TreeSourceAsset && TreeSourceAsset->HasValidMetadata();
	const bool bHasMonolithicSource = SplatAsset && SplatAsset->IsValid();
	if (!bHasPagedSource && !bHasSpatialSource && !bHasTreeSource && !bHasMonolithicSource)
	{
		return nullptr;
	}

	// Don't create proxy for preview worlds (Blueprint editor, etc.)
	UWorld* World = GetWorld();
	if (World)
	{
		EWorldType::Type WorldType = World->WorldType;
		if (WorldType == EWorldType::EditorPreview || WorldType == EWorldType::GamePreview)
		{
			return nullptr;
		}
	}

	return new FGaussianSplatSceneProxy(this);
}

FBoxSphereBounds UGaussianSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (SpatialLODSourceAsset && SpatialLODSourceAsset->HasValidMetadata())
	{
		return FBoxSphereBounds(SpatialLODSourceAsset->GetSceneBounds().TransformBy(LocalToWorld));
	}
	if (TreeSourceAsset && TreeSourceAsset->HasValidMetadata())
	{
		return FBoxSphereBounds(TreeSourceAsset->GetSceneBounds().TransformBy(LocalToWorld));
	}
	if (PagedSourceAsset && PagedSourceAsset->HasValidMetadata())
	{
		FBox ConservativeLocalBounds;
		if (PagedSourceAsset->GetConservativeExactSupportBounds(
			static_cast<double>(SplatScale), ConservativeLocalBounds))
		{
			return FBoxSphereBounds(ConservativeLocalBounds.TransformBy(LocalToWorld));
		}
		// Invalid runtime scale/metadata will also make rendering invalid. Preserve
		// the validated stored support box rather than returning an empty bound.
		return FBoxSphereBounds(PagedSourceAsset->GetSceneBounds().TransformBy(LocalToWorld));
	}
	if (SplatAsset && SplatAsset->IsValid())
	{
		FBox LocalBox = SplatAsset->GetBounds();

		// Transform to world space
		FBox WorldBox = LocalBox.TransformBy(LocalToWorld);

		return FBoxSphereBounds(WorldBox);
	}

	// Return small default bounds if no asset
	return FBoxSphereBounds(FVector::ZeroVector, FVector(100.0f), 100.0f);
}

void UGaussianSplatComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	// Gaussian splatting doesn't use traditional materials
	// But we might add a material for composite pass later
}

void UGaussianSplatComponent::SetSplatAsset(UGaussianSplatAsset* NewAsset)
{
	if (SplatAsset != NewAsset)
	{
		// Unsubscribe from old asset
		UnsubscribeFromAssetChanges();

		SplatAsset = NewAsset;
		if (NewAsset)
		{
			PagedSourceAsset = nullptr;
			SpatialLODSourceAsset = nullptr;
			TreeSourceAsset = nullptr;
		}

		// Subscribe to new asset
		if (IsRegistered())
		{
			SubscribeToAssetChanges();
		}

		OnAssetChanged();
	}
}

void UGaussianSplatComponent::SetPagedSourceAsset(UNanoGSPagedSourceAsset* NewAsset)
{
	if (PagedSourceAsset != NewAsset)
	{
		UnsubscribeFromAssetChanges();
		PagedSourceAsset = NewAsset;
		if (NewAsset)
		{
			SplatAsset = nullptr;
			SpatialLODSourceAsset = nullptr;
			TreeSourceAsset = nullptr;
		}
		OnAssetChanged();
	}
}

void UGaussianSplatComponent::SetSpatialLODSourceAsset(UNanoGSSpatialLODSourceAsset* NewAsset)
{
	if (SpatialLODSourceAsset != NewAsset)
	{
		UnsubscribeFromAssetChanges();
		SpatialLODSourceAsset = NewAsset;
		if (NewAsset)
		{
			SplatAsset = nullptr;
			PagedSourceAsset = nullptr;
			TreeSourceAsset = nullptr;
		}
		OnAssetChanged();
	}
}

void UGaussianSplatComponent::SetTreeSourceAsset(UNanoGSTreeSourceAsset* NewAsset)
{
	if (TreeSourceAsset != NewAsset)
	{
		UnsubscribeFromAssetChanges();
		TreeSourceAsset = NewAsset;
		if (NewAsset)
		{
			SplatAsset = nullptr;
			PagedSourceAsset = nullptr;
			SpatialLODSourceAsset = nullptr;
		}
		OnAssetChanged();
	}
}

int32 UGaussianSplatComponent::GetSplatCount() const
{
	if (SpatialLODSourceAsset && SpatialLODSourceAsset->HasValidMetadata())
	{
		const TArray<int64>& Counts = SpatialLODSourceAsset->GetLevelSplatCounts();
		return !Counts.IsEmpty() && Counts[0] <= MAX_int32 ? static_cast<int32>(Counts[0]) : 0;
	}
	if (TreeSourceAsset && TreeSourceAsset->HasValidMetadata())
	{
		return TreeSourceAsset->GetNodeCount() <= MAX_int32
			? static_cast<int32>(TreeSourceAsset->GetNodeCount())
			: 0;
	}
	if (PagedSourceAsset && PagedSourceAsset->HasValidMetadata())
	{
		return PagedSourceAsset->GetTotalSplatCount() <= static_cast<uint64>(MAX_int32)
			? static_cast<int32>(PagedSourceAsset->GetTotalSplatCount())
			: 0;
	}
	return SplatAsset ? SplatAsset->GetSplatCount() : 0;
}

void UGaussianSplatComponent::OnAssetChanged()
{
	bBoundsCached = false;
	UpdateBounds();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::MarkRenderStateDirty()
{
	MarkRenderDynamicDataDirty();

	if (IsRegistered())
	{
		Super::MarkRenderStateDirty();
	}
}

void UGaussianSplatComponent::OnAssetDataChanged(UGaussianSplatAsset* ChangedAsset)
{
	// Only respond if this is our asset
	if (ChangedAsset == SplatAsset)
	{
		UE_LOG(LogTemp, Log, TEXT("GaussianSplat: Asset data changed (Nanite state), recreating scene proxy"));

		// Invalidate cached bounds since splat count may have changed
		bBoundsCached = false;
		UpdateBounds();

		// Recreate the scene proxy with updated asset data
		// This will cause CreateSceneProxy to be called again with the new Nanite state
		MarkRenderStateDirty();
	}
}

void UGaussianSplatComponent::SubscribeToAssetChanges()
{
	if (SplatAsset && !AssetChangedDelegateHandle.IsValid())
	{
		AssetChangedDelegateHandle = SplatAsset->OnAssetChanged.AddUObject(this, &UGaussianSplatComponent::OnAssetDataChanged);
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplat: Subscribed to asset change notifications"));
	}
}

void UGaussianSplatComponent::UnsubscribeFromAssetChanges()
{
	if (SplatAsset && AssetChangedDelegateHandle.IsValid())
	{
		SplatAsset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
		AssetChangedDelegateHandle.Reset();
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplat: Unsubscribed from asset change notifications"));
	}
}
