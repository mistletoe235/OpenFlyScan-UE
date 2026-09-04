// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSTreeSourceAssetFactory.h"
#include "Paged/NanoGSTreeSourceAsset.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "NanoGSTreeSourceAssetFactory"

UNanoGSTreeSourceAssetFactory::UNanoGSTreeSourceAssetFactory()
{
	bCreateNew = false;
	bEditorImport = true;
	bText = true;
	SupportedClass = UNanoGSTreeSourceAsset::StaticClass();
	Formats.Add(TEXT("ngst;NanoGS Tree v2"));
	Formats.Add(TEXT("ngstree;NanoGS Tree v2 Package Descriptor"));
}

bool UNanoGSTreeSourceAssetFactory::FactoryCanImport(const FString& Filename)
{
	const FString Extension = FPaths::GetExtension(Filename);
	return Extension.Equals(TEXT("ngst"), ESearchCase::IgnoreCase) ||
		Extension.Equals(TEXT("ngstree"), ESearchCase::IgnoreCase);
}

UObject* UNanoGSTreeSourceAssetFactory::FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
	const FString& Filename, const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled)
{
	bOutOperationCanceled = false;
	UNanoGSTreeSourceAsset* Asset = NewObject<UNanoGSTreeSourceAsset>(InParent, InClass ? InClass : SupportedClass.Get(), InName, Flags);
	FString Error;
	if (!Asset->InitializeFromDirectory(FPaths::GetPath(Filename), Error))
	{
		if (Warn)
			Warn->Logf(ELogVerbosity::Error, TEXT("Failed to import Tree v2 package '%s': %s"), *Filename, *Error);
		return nullptr;
	}
	return Asset;
}

FText UNanoGSTreeSourceAssetFactory::GetDisplayName() const { return LOCTEXT("DisplayName", "NanoGS Tree v2 Source Asset"); }

bool UNanoGSTreeSourceAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	const UNanoGSTreeSourceAsset* Asset = Cast<UNanoGSTreeSourceAsset>(Obj);
	if (!Asset)
		return false;
	OutFilenames.Add(FPaths::Combine(Asset->GetResolvedDirectory(), TEXT("tree.ngst")));
	return true;
}

void UNanoGSTreeSourceAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	UNanoGSTreeSourceAsset* Asset = Cast<UNanoGSTreeSourceAsset>(Obj);
	if (!Asset || NewReimportPaths.IsEmpty())
		return;
	FString Error;
	if (!Asset->InitializeFromDirectory(FPaths::GetPath(NewReimportPaths[0]), Error))
		UE_LOG(LogTemp, Error, TEXT("Failed to change Tree v2 reimport path: %s"), *Error);
}

EReimportResult::Type UNanoGSTreeSourceAssetFactory::Reimport(UObject* Obj)
{
	UNanoGSTreeSourceAsset* Asset = Cast<UNanoGSTreeSourceAsset>(Obj);
	if (!Asset)
		return EReimportResult::Failed;
	FString Error;
	if (!Asset->Refresh(Error))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to reimport Tree v2 source '%s': %s"), *Asset->GetPathName(), *Error);
		return EReimportResult::Failed;
	}
	Asset->MarkPackageDirty();
	return EReimportResult::Succeeded;
}

#undef LOCTEXT_NAMESPACE
