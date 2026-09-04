// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"

#include "NanoGSPagedSourceAssetFactory.generated.h"

/** Imports a .ngsp reference asset without copying its page payload into the UObject. */
UCLASS(hidecategories = Object)
class NANOGSEDITOR_API UNanoGSPagedSourceAssetFactory : public UFactory, public FReimportHandler
{
	GENERATED_BODY()

public:
	UNanoGSPagedSourceAssetFactory();

	//~ Begin UFactory Interface
	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;
	virtual bool CanCreateNew() const override { return false; }
	virtual FText GetDisplayName() const override;
	//~ End UFactory Interface

	//~ Begin FReimportHandler Interface
	virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
	virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
	virtual EReimportResult::Type Reimport(UObject* Obj) override;
	//~ End FReimportHandler Interface
};
