// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetFactory.h"
#include "GaussianSplatAsset.h"
#include "PLYFileReader.h"
#include "EditorFramework/AssetImportData.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/App.h"
#include "Misc/FeedbackContext.h"
#include "Misc/ScopedSlowTask.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	constexpr int64 MaxRuntimeByteArraySize = MAX_int32;

	FString DescribeSHBands(const int32 SHBands)
	{
		if (SHBands < 0)
		{
			return TEXT("Auto");
		}

		return FString::Printf(TEXT("SH%d"), SHBands);
	}

	int32 GetEffectiveSHBands(const int32 DetectedSHBands, const int32 RequestedMaxSHBands)
	{
		return (RequestedMaxSHBands < 0)
			? DetectedSHBands
			: FMath::Clamp(FMath::Min(DetectedSHBands, RequestedMaxSHBands), 0, GaussianSplattingConstants::MaxSHOrder);
	}

	int32 GetStoredSHBytesPerSplat(int32 SHBands)
	{
		switch (SHBands)
		{
		case 0: return 0;
		case 1: return 24;
		case 2: return 54;
		case 3: return 96;
		default: return 96;
		}
	}

	FString BuildOptionLabel(const int32 OptionMaxSHBands, const int32 DetectedSHBands)
	{
		if (OptionMaxSHBands < 0)
		{
			return FString::Printf(TEXT("Auto (use detected %s)"), *DescribeSHBands(DetectedSHBands));
		}

		if (OptionMaxSHBands >= DetectedSHBands)
		{
			return FString::Printf(TEXT("%s (keep all available coefficients)"), *DescribeSHBands(OptionMaxSHBands));
		}

		return FString::Printf(TEXT("%s (drop higher-order coefficients)"), *DescribeSHBands(OptionMaxSHBands));
	}

	bool ValidateProbeForImport(const FPLYFileInfo& FileInfo, const int32 EffectiveSHBands, FString& OutError)
	{
		const int64 SplatCount = FileInfo.VertexCount;
		const int64 RawImportBytes = SplatCount * static_cast<int64>(sizeof(FGaussianSplatData));
		const int64 PackedSplatBytes = SplatCount * GaussianSplattingConstants::PackedSplatStride;
		const int64 PositionBytes = SplatCount * 12ll;
		const int64 OtherBytes = SplatCount * 28ll;
		const int64 SHBytes = SplatCount * static_cast<int64>(GetStoredSHBytesPerSplat(EffectiveSHBands));

		const int32 ChosenWidth = GaussianSplattingUtils::ChooseColorTextureWidthForSplats(SplatCount);
		if (ChosenWidth == 0)
		{
			OutError = FString::Printf(
				TEXT("File contains %lld splats. Current NanoGS color texture layout cannot represent that many splats within the %dx%d texture limit."),
				SplatCount,
				GaussianSplattingConstants::MaxColorTextureDimension,
				GaussianSplattingConstants::MaxColorTextureDimension);
			return false;
		}

		const int32 ColorHeight = GaussianSplattingUtils::GetColorTextureHeightForSplats(SplatCount, ChosenWidth);
		const int64 ColorTextureBytes = static_cast<int64>(ChosenWidth) * ColorHeight * static_cast<int64>(sizeof(FFloat16Color));

		auto MakeArraySizeError = [&](const TCHAR* BufferName, int64 BufferBytes) -> bool
		{
			if (BufferBytes > MaxRuntimeByteArraySize)
			{
				OutError = FString::Printf(
					TEXT("File contains %lld splats. The %s buffer would be %.2f GiB, exceeding the current NanoGS/UE runtime array limit of 2.00 GiB. ")
					TEXT("Detected SH bands: %d, import SH bands: %d, estimated raw import memory: %.2f GiB. ")
					TEXT("Please split the GS file or reduce SH order / splat count before import."),
					SplatCount,
					BufferName,
					BufferBytes / (1024.0 * 1024.0 * 1024.0),
					FileInfo.SHBands,
					EffectiveSHBands,
					RawImportBytes / (1024.0 * 1024.0 * 1024.0));
				return false;
			}
			return true;
		};

		if (!MakeArraySizeError(TEXT("packed splat"), PackedSplatBytes) ||
			!MakeArraySizeError(TEXT("position"), PositionBytes) ||
			!MakeArraySizeError(TEXT("rotation/scale"), OtherBytes) ||
			!MakeArraySizeError(TEXT("SH"), SHBytes) ||
			!MakeArraySizeError(TEXT("color texture"), ColorTextureBytes))
		{
			return false;
		}

		UE_LOG(LogTemp, Log,
			TEXT("NanoGS preflight: %lld splats, file %.2f GiB, raw import %.2f GiB, color texture %dx%d, detected SH bands %d, import SH bands %d"),
			SplatCount,
			FileInfo.FileSize / (1024.0 * 1024.0 * 1024.0),
			RawImportBytes / (1024.0 * 1024.0 * 1024.0),
			ChosenWidth,
			ColorHeight,
			FileInfo.SHBands,
			EffectiveSHBands);

		return true;
	}

	class SNanoGSImportOptionsDialog : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SNanoGSImportOptionsDialog) {}
			SLATE_ARGUMENT(FPLYFileInfo, FileInfo)
			SLATE_ARGUMENT(FString, FilePath)
			SLATE_ARGUMENT(int32, InitialMaxSHBands)
			SLATE_ARGUMENT(TSharedPtr<SWindow>, OwnerWindow)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			FileInfo = InArgs._FileInfo;
			FilePath = InArgs._FilePath;
			OwnerWindow = InArgs._OwnerWindow;
			SelectedMaxSHBands = InArgs._InitialMaxSHBands;

			Options.Add(MakeShared<int32>(INDEX_NONE));
			for (int32 SHBands = GaussianSplattingConstants::MaxSHOrder; SHBands >= 0; --SHBands)
			{
				Options.Add(MakeShared<int32>(SHBands));
			}

			SelectedItem = Options[0];
			for (const TSharedPtr<int32>& Option : Options)
			{
				if (*Option == SelectedMaxSHBands)
				{
					SelectedItem = Option;
					break;
				}
			}

			ChildSlot
			[
				SNew(SBox)
				.WidthOverride(540.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(12.0f, 12.0f, 12.0f, 8.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Choose the maximum SH level to keep for this import.")))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(12.0f, 0.0f, 12.0f, 8.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(
							TEXT("File: %s\nSplats: %lld\nDetected SH: %s\nLower SH reduces SH buffer size but also reduces view-dependent lighting detail."),
							*FPaths::GetCleanFilename(FilePath),
							FileInfo.VertexCount,
							*DescribeSHBands(FileInfo.SHBands))))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(12.0f, 0.0f, 12.0f, 12.0f)
					[
						SAssignNew(ComboBox, SComboBox<TSharedPtr<int32>>)
						.OptionsSource(&Options)
						.InitiallySelectedItem(SelectedItem)
						.OnSelectionChanged(this, &SNanoGSImportOptionsDialog::OnSelectionChanged)
						.OnGenerateWidget(this, &SNanoGSImportOptionsDialog::GenerateOptionWidget)
						[
							SNew(STextBlock)
							.Text(this, &SNanoGSImportOptionsDialog::GetSelectedOptionText)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(12.0f, 0.0f, 12.0f, 12.0f)
					[
						SNew(STextBlock)
						.Text(this, &SNanoGSImportOptionsDialog::GetSummaryText)
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Right)
					.Padding(12.0f)
					[
						SNew(SUniformGridPanel)
						.SlotPadding(FMargin(8.0f, 0.0f))
						+ SUniformGridPanel::Slot(0, 0)
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("Import")))
							.OnClicked(this, &SNanoGSImportOptionsDialog::OnImportClicked)
						]
						+ SUniformGridPanel::Slot(1, 0)
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("Cancel")))
							.OnClicked(this, &SNanoGSImportOptionsDialog::OnCancelClicked)
						]
					]
				]
			];
		}

		bool WasAccepted() const
		{
			return bAccepted;
		}

		int32 GetSelectedMaxSHBands() const
		{
			return SelectedMaxSHBands;
		}

	private:
		TSharedRef<SWidget> GenerateOptionWidget(TSharedPtr<int32> InOption) const
		{
			return SNew(STextBlock)
				.Text(FText::FromString(BuildOptionLabel(*InOption, FileInfo.SHBands)));
		}

		void OnSelectionChanged(TSharedPtr<int32> InOption, ESelectInfo::Type)
		{
			if (InOption.IsValid())
			{
				SelectedItem = InOption;
				SelectedMaxSHBands = *InOption;
			}
		}

		FText GetSelectedOptionText() const
		{
			return FText::FromString(BuildOptionLabel(SelectedMaxSHBands, FileInfo.SHBands));
		}

		FText GetSummaryText() const
		{
			const int32 EffectiveSHBands = GetEffectiveSHBands(FileInfo.SHBands, SelectedMaxSHBands);
			const double SHGiB = static_cast<double>(FileInfo.VertexCount) * static_cast<double>(GetStoredSHBytesPerSplat(EffectiveSHBands)) / (1024.0 * 1024.0 * 1024.0);
			return FText::FromString(FString::Printf(
				TEXT("Current choice: %s. Effective import level: %s. Estimated SH bulk size: %.2f GiB."),
				*DescribeSHBands(SelectedMaxSHBands),
				*DescribeSHBands(EffectiveSHBands),
				SHGiB));
		}

		FReply OnImportClicked()
		{
			bAccepted = true;
			if (OwnerWindow.IsValid())
			{
				OwnerWindow.Pin()->RequestDestroyWindow();
			}
			return FReply::Handled();
		}

		FReply OnCancelClicked()
		{
			bAccepted = false;
			if (OwnerWindow.IsValid())
			{
				OwnerWindow.Pin()->RequestDestroyWindow();
			}
			return FReply::Handled();
		}

		FPLYFileInfo FileInfo;
		FString FilePath;
		TWeakPtr<SWindow> OwnerWindow;
		TArray<TSharedPtr<int32>> Options;
		TSharedPtr<int32> SelectedItem;
		TSharedPtr<SComboBox<TSharedPtr<int32>>> ComboBox;
		int32 SelectedMaxSHBands = INDEX_NONE;
		bool bAccepted = false;
	};

	bool PromptForImportOptions(const FPLYFileInfo& FileInfo, const FString& FilePath, int32 InitialMaxSHBands, int32& OutRequestedMaxSHBands)
	{
		if (FApp::IsUnattended() || IsRunningCommandlet() || !FSlateApplication::IsInitialized())
		{
			OutRequestedMaxSHBands = InitialMaxSHBands;
			UE_LOG(LogTemp, Log, TEXT("NanoGS unattended import: automatically selecting %s"),
				*DescribeSHBands(InitialMaxSHBands));
			return true;
		}

		TSharedRef<SWindow> DialogWindow = SNew(SWindow)
			.Title(FText::FromString(TEXT("Gaussian Splat Import Options")))
			.SizingRule(ESizingRule::Autosized)
			.SupportsMaximize(false)
			.SupportsMinimize(false);

		TSharedRef<SNanoGSImportOptionsDialog> DialogContent =
			SNew(SNanoGSImportOptionsDialog)
			.FileInfo(FileInfo)
			.FilePath(FilePath)
			.InitialMaxSHBands(InitialMaxSHBands)
			.OwnerWindow(DialogWindow);

		DialogWindow->SetContent(DialogContent);
		FSlateApplication::Get().AddModalWindow(DialogWindow, FSlateApplication::Get().GetActiveTopLevelWindow(), false);

		if (!DialogContent->WasAccepted())
		{
			return false;
		}

		OutRequestedMaxSHBands = DialogContent->GetSelectedMaxSHBands();
		return true;
	}

	bool FindHighestImportableSHBands(const FPLYFileInfo& FileInfo, int32& OutSHBands, FString& OutError)
	{
		for (int32 CandidateSHBands = FileInfo.SHBands; CandidateSHBands >= 0; --CandidateSHBands)
		{
			FString CandidateError;
			if (ValidateProbeForImport(FileInfo, CandidateSHBands, CandidateError))
			{
				OutSHBands = CandidateSHBands;
				OutError = CandidateError;
				return true;
			}
		}

		OutSHBands = INDEX_NONE;
		return false;
	}
}

UGaussianSplatAssetFactory::UGaussianSplatAssetFactory()
{
	bCreateNew = false;
	bEditorImport = true;
	bText = false;

	SupportedClass = UGaussianSplatAsset::StaticClass();

	Formats.Add(TEXT("ply;PLY Gaussian Splatting File"));
}

bool UGaussianSplatAssetFactory::ConfigureProperties()
{
	bImportOptionsCanceled = false;
	RequestedMaxSHBands = INDEX_NONE;
	return true;
}

bool UGaussianSplatAssetFactory::FactoryCanImport(const FString& Filename)
{
	const FString Extension = FPaths::GetExtension(Filename);
	return Extension.Equals(TEXT("ply"), ESearchCase::IgnoreCase) && FPLYFileReader::IsValidPLYFile(Filename);
}

UObject* UGaussianSplatAssetFactory::FactoryCreateFile(
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

	if (bImportOptionsCanceled)
	{
		bOutOperationCanceled = true;
		return nullptr;
	}

	UGaussianSplatAsset* NewAsset = ImportPLYFile(Filename, InParent, InName, Flags, nullptr);

	if (!NewAsset)
	{
		if (bImportOptionsCanceled)
		{
			bOutOperationCanceled = true;
			return nullptr;
		}

		if (Warn)
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("Failed to import Gaussian Splat from: %s"), *Filename);
		}
	}

	return NewAsset;
}

FText UGaussianSplatAssetFactory::GetDisplayName() const
{
	return FText::FromString(TEXT("Gaussian Splat Asset"));
}

bool UGaussianSplatAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
	if (Asset && !Asset->SourceFilePath.IsEmpty())
	{
		OutFilenames.Add(Asset->GetResolvedSourceFilePath());
		return true;
	}
	return false;
}

void UGaussianSplatAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
	if (Asset && NewReimportPaths.Num() > 0)
	{
		Asset->SetSourceFilePathForReimport(NewReimportPaths[0]);
	}
}

EReimportResult::Type UGaussianSplatAssetFactory::Reimport(UObject* Obj)
{
	UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
	if (!Asset)
	{
		return EReimportResult::Failed;
	}

	if (Asset->SourceFilePath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Cannot reimport: source file path is empty"));
		return EReimportResult::Failed;
	}

	const FString ResolvedSourceFilePath = Asset->GetResolvedSourceFilePath();
	if (!FPaths::FileExists(ResolvedSourceFilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("Cannot reimport: source file not found: %s"), *ResolvedSourceFilePath);
		return EReimportResult::Failed;
	}

	// Preserve Nanite setting before reimport
	const bool bWasNaniteEnabled = Asset->IsNaniteEnabled();

	// Use the original quality level
	QualityLevel = Asset->ImportQuality;
	RequestedMaxSHBands = Asset->SHBands;
	bImportOptionsCanceled = false;

	UGaussianSplatAsset* ReimportedAsset = ImportPLYFile(
		ResolvedSourceFilePath,
		Asset->GetOuter(),
		Asset->GetFName(),
		Asset->GetFlags(),
		Asset
	);

	if (ReimportedAsset)
	{
		// If Nanite was enabled before reimport, rebuild the cluster hierarchy
		if (bWasNaniteEnabled)
		{
			UE_LOG(LogTemp, Log, TEXT("Reimport: Rebuilding Nanite cluster hierarchy (was enabled before reimport)"));
			if (!ReimportedAsset->BuildNaniteClusterHierarchy())
			{
				UE_LOG(LogTemp, Warning, TEXT("Reimport: Failed to rebuild Nanite cluster hierarchy"));
			}
		}
		return EReimportResult::Succeeded;
	}

	return EReimportResult::Failed;
}

UGaussianSplatAsset* UGaussianSplatAssetFactory::ImportPLYFile(
	const FString& FilePath,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	UGaussianSplatAsset* ExistingAsset)
{
	FScopedSlowTask SlowTask(100.0f, FText::FromString(TEXT("Importing Gaussian Splat...")));
	if (!FApp::IsUnattended() && !IsRunningCommandlet())
	{
		SlowTask.MakeDialog(true);
	}

	// Read PLY file
	SlowTask.EnterProgressFrame(30.0f, FText::FromString(TEXT("Reading PLY file...")));

	FString ErrorMessage;
	FPLYFileInfo FileInfo;
	if (!FPLYFileReader::ProbePLYFile(FilePath, FileInfo, ErrorMessage))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to probe PLY file: %s"), *ErrorMessage);
		return nullptr;
	}

	int32 EffectiveSHBands = FileInfo.SHBands;
	if (ExistingAsset)
	{
		EffectiveSHBands = GetEffectiveSHBands(FileInfo.SHBands, ExistingAsset->SHBands);
	}
	else
	{
		FString AutoImportError;
		if (!ValidateProbeForImport(FileInfo, EffectiveSHBands, AutoImportError))
		{
			int32 HighestImportableSHBands = INDEX_NONE;
			FString FallbackValidationError;
			if (!FindHighestImportableSHBands(FileInfo, HighestImportableSHBands, FallbackValidationError))
			{
				UE_LOG(LogTemp, Error, TEXT("Gaussian Splat import rejected: %s"), *AutoImportError);
				return nullptr;
			}

			int32 SelectedMaxSHBands = HighestImportableSHBands;
			if (!PromptForImportOptions(FileInfo, FilePath, HighestImportableSHBands, SelectedMaxSHBands))
			{
				bImportOptionsCanceled = true;
				UE_LOG(LogTemp, Log, TEXT("Gaussian Splat import cancelled by user: %s"), *FilePath);
				return nullptr;
			}

			RequestedMaxSHBands = SelectedMaxSHBands;
			EffectiveSHBands = GetEffectiveSHBands(FileInfo.SHBands, RequestedMaxSHBands);
		}
	}

	if (!ValidateProbeForImport(FileInfo, EffectiveSHBands, ErrorMessage))
	{
		UE_LOG(LogTemp, Error, TEXT("Gaussian Splat import rejected: %s"), *ErrorMessage);
		return nullptr;
	}

	TArray<FGaussianSplatData> SplatData;
	int32 DetectedSHBands = 0;

	if (!FPLYFileReader::ReadPLYFile(FilePath, SplatData, ErrorMessage, &DetectedSHBands, EffectiveSHBands))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to read PLY file: %s"), *ErrorMessage);
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("Read %d splats from PLY file (detected SH bands: %d, import SH bands: %d)"),
		SplatData.Num(), DetectedSHBands, EffectiveSHBands);

	// Create or reuse asset
	SlowTask.EnterProgressFrame(10.0f, FText::FromString(TEXT("Creating asset...")));

	UGaussianSplatAsset* Asset = ExistingAsset;
	if (!Asset)
	{
		Asset = NewObject<UGaussianSplatAsset>(InParent, UGaussianSplatAsset::StaticClass(), InName, Flags);
	}

	if (!Asset)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create Gaussian Splat asset"));
		return nullptr;
	}

	// Store source file path
	Asset->SetSourceFilePathForReimport(FilePath);

	// Set the final SH band count BEFORE initializing (CompressSH uses this)
	Asset->SHBands = EffectiveSHBands;

	// Initialize asset from splat data (NO cluster building - user enables Nanite via Asset Actions)
	SlowTask.EnterProgressFrame(55.0f, FText::FromString(TEXT("Compressing splat data...")));

	Asset->InitializeFromSplatData(SplatData, QualityLevel);

	// NO cluster hierarchy by default - user enables Nanite via Asset Actions > Nanite
	Asset->ClusterHierarchy.Reset();

	// Mark package dirty
	Asset->MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("Successfully imported Gaussian Splat asset: %d splats, %lld bytes, SH bands %d (Nanite disabled by default)"),
		Asset->GetSplatCount(), Asset->GetMemoryUsage(), Asset->SHBands);

	return Asset;
}
