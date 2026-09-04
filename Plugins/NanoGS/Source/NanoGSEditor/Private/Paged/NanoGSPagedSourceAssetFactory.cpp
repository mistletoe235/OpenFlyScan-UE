// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSPagedSourceAssetFactory.h"

#include "Paged/NanoGSPagedSourceAsset.h"

#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "NanoGSPagedSourceAssetFactory"

UNanoGSPagedSourceAssetFactory::UNanoGSPagedSourceAssetFactory()
{
	bCreateNew = false;
	bEditorImport = true;
	bText = false;
	SupportedClass = UNanoGSPagedSourceAsset::StaticClass();
	Formats.Add(TEXT("ngsp;NanoGS Page v1 Container"));
}

bool UNanoGSPagedSourceAssetFactory::FactoryCanImport(const FString& Filename)
{
	return FPaths::GetExtension(Filename).Equals(TEXT("ngsp"), ESearchCase::IgnoreCase);
}

UObject* UNanoGSPagedSourceAssetFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	bOutOperationCanceled = false;
	UClass* AssetClass = InClass ? InClass : SupportedClass.Get();
	if (!AssetClass || !AssetClass->IsChildOf(UNanoGSPagedSourceAsset::StaticClass()))
	{
		if (Warn)
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("Invalid class supplied to NanoGS Page source factory"));
		}
		return nullptr;
	}
	UNanoGSPagedSourceAsset* Asset = NewObject<UNanoGSPagedSourceAsset>(InParent, AssetClass, InName, Flags);
	FString Error;
	if (!Asset->InitializeFromContainer(Filename, Error, true))
	{
		if (Warn)
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("Failed to import NanoGS Page container '%s': %s"), *Filename, *Error);
		}
		return nullptr;
	}

	if (!Asset->IsInRecommendedStagingDirectory() && Warn)
	{
		Warn->Logf(
			ELogVerbosity::Warning,
			TEXT("NanoGS Page asset '%s' references '%s'. Move the container under Content/NanoGSData before packaging."),
			*InName.ToString(),
			*Asset->GetProjectRelativeContainerPath());
	}
	return Asset;
}

FText UNanoGSPagedSourceAssetFactory::GetDisplayName() const
{
	return LOCTEXT("DisplayName", "NanoGS Paged Source Asset");
}

bool UNanoGSPagedSourceAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	const UNanoGSPagedSourceAsset* Asset = Cast<UNanoGSPagedSourceAsset>(Obj);
	if (!Asset)
	{
		return false;
	}

	const FString ResolvedPath = Asset->GetResolvedContainerPath();
	if (ResolvedPath.IsEmpty())
	{
		return false;
	}
	OutFilenames.Add(ResolvedPath);
	return true;
}

void UNanoGSPagedSourceAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	UNanoGSPagedSourceAsset* Asset = Cast<UNanoGSPagedSourceAsset>(Obj);
	if (!Asset || NewReimportPaths.IsEmpty())
	{
		return;
	}

	FString Error;
	if (!Asset->InitializeFromContainer(NewReimportPaths[0], Error, true))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to change NanoGS Page reimport path: %s"), *Error);
	}
}

EReimportResult::Type UNanoGSPagedSourceAssetFactory::Reimport(UObject* Obj)
{
	UNanoGSPagedSourceAsset* Asset = Cast<UNanoGSPagedSourceAsset>(Obj);
	if (!Asset)
	{
		return EReimportResult::Failed;
	}

	FString Error;
	if (!Asset->RefreshFromContainer(Error, true))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to reimport NanoGS Page asset '%s': %s"), *Asset->GetPathName(), *Error);
		return EReimportResult::Failed;
	}

	Asset->MarkPackageDirty();
	return EReimportResult::Succeeded;
}

#undef LOCTEXT_NAMESPACE
