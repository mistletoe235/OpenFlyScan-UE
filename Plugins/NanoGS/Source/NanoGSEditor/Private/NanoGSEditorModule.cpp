// Copyright Epic Games, Inc. All Rights Reserved.

#include "NanoGSEditorModule.h"
#include "GaussianSplatAssetTypeActions.h"
#include "GaussianSplatThumbnailRenderer.h"
#include "GaussianSplatAsset.h"
#include "NanoGSEditorMouseInputCoalescer.h"
#include "AssetToolsModule.h"
#include "Framework/Application/SlateApplication.h"
#include "GaussianSplatComponentDetails.h"
#include "IAssetTools.h"
#include "PropertyEditorModule.h"
#include "ThumbnailRendering/ThumbnailManager.h"

#define LOCTEXT_NAMESPACE "FNanoGSEditorModule"

void FNanoGSEditorModule::StartupModule()
{
	// Register asset type actions
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Register Gaussian Splat asset type
	TSharedPtr<IAssetTypeActions> GaussianSplatAssetActions = MakeShareable(new FAssetTypeActions_GaussianSplatAsset());
	AssetTools.RegisterAssetTypeActions(GaussianSplatAssetActions.ToSharedRef());
	RegisteredAssetTypeActions.Add(GaussianSplatAssetActions);

	// Register custom thumbnail renderer for Gaussian Splat assets
	UThumbnailManager::Get().RegisterCustomRenderer(
		UGaussianSplatAsset::StaticClass(),
		UGaussianSplatThumbnailRenderer::StaticClass());

	FPropertyEditorModule& PropertyEditor =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	PropertyEditor.RegisterCustomClassLayout(
		TEXT("GaussianSplatComponent"),
		FOnGetDetailCustomizationInstance::CreateStatic(&FGaussianSplatComponentDetails::MakeInstance));
	PropertyEditor.NotifyCustomizationModuleChanged();

	if (FSlateApplication::IsInitialized())
	{
		MouseInputCoalescer = MakeShared<FNanoGSEditorMouseInputCoalescer>();
		FSlateApplication::Get().RegisterInputPreProcessor(MouseInputCoalescer, 0);
	}

	UE_LOG(LogTemp, Log, TEXT("GaussianSplattingEditor module started."));
}

void FNanoGSEditorModule::ShutdownModule()
{
	if (MouseInputCoalescer.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(MouseInputCoalescer);
	}
	MouseInputCoalescer.Reset();

	if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")))
	{
		FPropertyEditorModule& PropertyEditor =
			FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.UnregisterCustomClassLayout(TEXT("GaussianSplatComponent"));
		PropertyEditor.NotifyCustomizationModuleChanged();
	}
	// Unregister asset type actions
	if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		for (auto& Action : RegisteredAssetTypeActions)
		{
			AssetTools.UnregisterAssetTypeActions(Action.ToSharedRef());
		}
	}
	RegisteredAssetTypeActions.Empty();

	UE_LOG(LogTemp, Log, TEXT("GaussianSplattingEditor module shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNanoGSEditorModule, NanoGSEditor)
