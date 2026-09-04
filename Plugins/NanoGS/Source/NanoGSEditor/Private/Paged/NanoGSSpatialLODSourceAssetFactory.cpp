// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSSpatialLODSourceAssetFactory.h"
#include "Paged/NanoGSSpatialLODSourceAsset.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "NanoGSSpatialLODSourceAssetFactory"

UNanoGSSpatialLODSourceAssetFactory::UNanoGSSpatialLODSourceAssetFactory()
{
	bCreateNew = false;
	bEditorImport = true;
	bText = true;
	SupportedClass = UNanoGSSpatialLODSourceAsset::StaticClass();
	Formats.Add(TEXT("spatiallod;NanoGS Spatial LOD Package Descriptor"));
}

bool UNanoGSSpatialLODSourceAssetFactory::FactoryCanImport(const FString& Filename)
{
	return FPaths::GetExtension(Filename).Equals(TEXT("spatiallod"), ESearchCase::IgnoreCase);
}

UObject* UNanoGSSpatialLODSourceAssetFactory::FactoryCreateFile(
	UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
	const FString& Filename, const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled)
{
	bOutOperationCanceled = false;
	UNanoGSSpatialLODSourceAsset* Asset = NewObject<UNanoGSSpatialLODSourceAsset>(
		InParent, InClass ? InClass : SupportedClass.Get(), InName, Flags);
	FString Error;
	if (!Asset->InitializeFromDirectory(FPaths::GetPath(Filename), Error))
	{
		if (Warn)
			Warn->Logf(ELogVerbosity::Error, TEXT("Failed to import SpatialLOD package '%s': %s"), *Filename, *Error);
		return nullptr;
	}
	return Asset;
}

FText UNanoGSSpatialLODSourceAssetFactory::GetDisplayName() const
{
	return LOCTEXT("DisplayName", "NanoGS Spatial LOD Source Asset");
}

bool UNanoGSSpatialLODSourceAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	const UNanoGSSpatialLODSourceAsset* Asset = Cast<UNanoGSSpatialLODSourceAsset>(Obj);
	if (!Asset)
		return false;
	OutFilenames.Add(FPaths::Combine(Asset->GetResolvedDirectory(), TEXT("sjtu_spatial_lod.spatiallod")));
	return true;
}

void UNanoGSSpatialLODSourceAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	UNanoGSSpatialLODSourceAsset* Asset = Cast<UNanoGSSpatialLODSourceAsset>(Obj);
	if (!Asset || NewReimportPaths.IsEmpty())
		return;
	FString Error;
	if (!Asset->InitializeFromDirectory(FPaths::GetPath(NewReimportPaths[0]), Error))
		UE_LOG(LogTemp, Error, TEXT("Failed to change SpatialLOD reimport path: %s"), *Error);
}

EReimportResult::Type UNanoGSSpatialLODSourceAssetFactory::Reimport(UObject* Obj)
{
	UNanoGSSpatialLODSourceAsset* Asset = Cast<UNanoGSSpatialLODSourceAsset>(Obj);
	if (!Asset)
		return EReimportResult::Failed;
	FString Error;
	if (!Asset->Refresh(Error))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to reimport SpatialLOD source '%s': %s"), *Asset->GetPathName(), *Error);
		return EReimportResult::Failed;
	}
	Asset->MarkPackageDirty();
	return EReimportResult::Succeeded;
}

#undef LOCTEXT_NAMESPACE
