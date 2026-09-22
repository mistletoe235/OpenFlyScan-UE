#include "NanoGSControlWidget.h"

#include "GaussianSplatComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Input/Reply.h"
#include "Styling/CoreStyle.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
constexpr float WindowWidth = 370.0f;
constexpr float ExpandedHeight = 350.0f;
constexpr float MinimizedHeight = 42.0f;
constexpr int32 DefaultStableBudget = 10000000;
constexpr int32 DefaultMovingBudget = 4000000;
constexpr float DefaultLODDetail = 3.0f;
constexpr float DefaultOpacity = 1.0f;

const FLinearColor PanelColor(0.025f, 0.035f, 0.055f, 0.94f);
const FLinearColor HeaderColor(0.055f, 0.075f, 0.11f, 0.98f);
const FLinearColor PrimaryText(0.90f, 0.94f, 1.0f, 1.0f);
const FLinearColor SecondaryText(0.58f, 0.66f, 0.76f, 1.0f);
const FLinearColor HealthyColor(0.22f, 0.90f, 0.58f, 1.0f);
const FLinearColor WaitingColor(1.0f, 0.72f, 0.24f, 1.0f);

IConsoleVariable* GetAfterTonemapVariable()
{
	static IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(TEXT("gs.CompositeAfterTonemap"));
	return Variable;
}

TSharedRef<SWidget> MakeLabel(const TCHAR* Text)
{
	return SNew(STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10))
		.ColorAndOpacity(SecondaryText)
		.Text(FText::FromString(Text));
}

TSharedRef<SWidget> MakeRow(const TCHAR* Label, const TSharedRef<SWidget>& Control)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[ MakeLabel(Label) ]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[ Control ];
}

template <typename CallbackType>
void ForEachActiveComponent(const UWorld* World, CallbackType&& Callback)
{
	if (World == nullptr)
		return;
	for (TObjectIterator<UGaussianSplatComponent> It; It; ++It)
	{
		UGaussianSplatComponent* Component = *It;
		if (Component != nullptr && Component->GetWorld() == World && Component->IsRegistered())
			Callback(*Component);
	}
}
}

TSharedRef<SWidget> UNanoGSControlWidget::RebuildWidget()
{
	return SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
		.BorderBackgroundColor(PanelColor)
		.Padding(0.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
				.BorderBackgroundColor(HeaderColor)
				.Padding(FMargin(12.0f, 8.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
						.ColorAndOpacity(PrimaryText)
						.Text(FText::FromString(TEXT("NANOGS RENDER")))
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(12.0f, 0.0f).VAlign(VAlign_Center)
					[
						SAssignNew(StatusText, STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10))
						.ColorAndOpacity(WaitingColor)
                        .Text(FText::FromString(TEXT("Waiting for scene")))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.ButtonColorAndOpacity(FLinearColor(0.12f, 0.15f, 0.21f, 1.0f))
						.ContentPadding(FMargin(10.0f, 1.0f))
						.OnClicked(FOnClicked::CreateUObject(this, &UNanoGSControlWidget::ToggleMinimized))
						[
							SAssignNew(MinimizeText, STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
							.ColorAndOpacity(PrimaryText)
							.Text(FText::FromString(TEXT("-")))
						]
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(DetailBox, SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 10.0f, 12.0f, 5.0f)
				[
					MakeRow(TEXT("After Tonemap"),
						SNew(SCheckBox)
						.IsChecked(TAttribute<ECheckBoxState>::Create(
							TAttribute<ECheckBoxState>::FGetter::CreateUObject(
								this, &UNanoGSControlWidget::GetAfterTonemapState)))
						.OnCheckStateChanged(FOnCheckStateChanged::CreateUObject(
							this, &UNanoGSControlWidget::OnAfterTonemapChanged)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 4.0f)
				[
					MakeRow(TEXT("Force full LOD0"),
						SNew(SCheckBox)
						.IsChecked(TAttribute<ECheckBoxState>::Create(
							TAttribute<ECheckBoxState>::FGetter::CreateUObject(
								this, &UNanoGSControlWidget::GetForceLOD0State)))
						.OnCheckStateChanged(FOnCheckStateChanged::CreateUObject(
							this, &UNanoGSControlWidget::OnForceLOD0Changed)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 4.0f)
				[
					MakeRow(TEXT("Static budget (M)"),
						SNew(SNumericEntryBox<float>)
						.MinValue(0.1f).MaxValue(30.0f).AllowSpin(false)
						.MinDesiredValueWidth(90.0f)
						.Value(TAttribute<TOptional<float>>::Create(
							TAttribute<TOptional<float>>::FGetter::CreateUObject(
								this, &UNanoGSControlWidget::GetStableBudgetMillions)))
						.OnValueCommitted(SNumericEntryBox<float>::FOnValueCommitted::CreateUObject(
							this, &UNanoGSControlWidget::OnStableBudgetCommitted)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 4.0f)
				[
					MakeRow(TEXT("Moving budget (M)"),
						SNew(SNumericEntryBox<float>)
						.MinValue(0.0f).MaxValue(30.0f).AllowSpin(false)
						.MinDesiredValueWidth(90.0f)
						.Value(TAttribute<TOptional<float>>::Create(
							TAttribute<TOptional<float>>::FGetter::CreateUObject(
								this, &UNanoGSControlWidget::GetMovingBudgetMillions)))
						.OnValueCommitted(SNumericEntryBox<float>::FOnValueCommitted::CreateUObject(
							this, &UNanoGSControlWidget::OnMovingBudgetCommitted)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 4.0f)
				[
					MakeRow(TEXT("LOD detail scale"),
						SNew(SNumericEntryBox<float>)
						.MinValue(0.1f).MaxValue(4.0f).AllowSpin(false)
						.MinDesiredValueWidth(90.0f)
						.Value(TAttribute<TOptional<float>>::Create(
							TAttribute<TOptional<float>>::FGetter::CreateUObject(
								this, &UNanoGSControlWidget::GetLODDetail)))
						.OnValueCommitted(SNumericEntryBox<float>::FOnValueCommitted::CreateUObject(
							this, &UNanoGSControlWidget::OnLODDetailCommitted)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 4.0f)
				[
					MakeRow(TEXT("Opacity"),
						SNew(SNumericEntryBox<float>)
						.MinValue(0.0f).MaxValue(2.0f).AllowSpin(false)
						.MinDesiredValueWidth(90.0f)
						.Value(TAttribute<TOptional<float>>::Create(
							TAttribute<TOptional<float>>::FGetter::CreateUObject(
								this, &UNanoGSControlWidget::GetOpacity)))
						.OnValueCommitted(SNumericEntryBox<float>::FOnValueCommitted::CreateUObject(
							this, &UNanoGSControlWidget::OnOpacityCommitted)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(12.0f, 8.0f, 12.0f, 4.0f)
				[
					SAssignNew(DetailStatusText, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9))
					.ColorAndOpacity(SecondaryText)
					.AutoWrapText(true)
					.Text(FText::FromString(TEXT("--")))
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(12.0f, 6.0f, 12.0f, 10.0f)
				[
					SNew(SButton)
					.ContentPadding(FMargin(12.0f, 3.0f))
					.OnClicked(FOnClicked::CreateUObject(this, &UNanoGSControlWidget::ResetDefaults))
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9))
						.Text(FText::FromString(TEXT("Restore defaults")))
					]
				]
			]
		];
}

void UNanoGSControlWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetAlignmentInViewport(FVector2D::ZeroVector);
	SetPositionInViewport(WindowPosition, false);
	ApplyWindowSize();
	RefreshStatus();
}

void UNanoGSControlWidget::NativeTick(const FGeometry& MyGeometry, const float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshAccumulator += InDeltaTime;
	if (RefreshAccumulator >= 0.25f)
	{
		RefreshAccumulator = 0.0f;
		RefreshStatus();
	}
}

FReply UNanoGSControlWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton &&
		InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition()).Y <= 40.0f)
	{
		bDragging = true;
		DragOffset = InMouseEvent.GetScreenSpacePosition() - WindowPosition;
		return FReply::Handled().CaptureMouse(GetCachedWidget().ToSharedRef());
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UNanoGSControlWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if (bDragging && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bDragging = false;
		return FReply::Handled().ReleaseMouseCapture();
	}
	return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
}

FReply UNanoGSControlWidget::NativeOnMouseMove(const FGeometry& InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if (bDragging)
	{
		FVector2D Position = InMouseEvent.GetScreenSpacePosition() - DragOffset;
		Position.X = FMath::Max(0.0f, Position.X);
		Position.Y = FMath::Max(0.0f, Position.Y);
		WindowPosition = Position;
		SetPositionInViewport(WindowPosition, false);
		return FReply::Handled();
	}
	return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
}

UGaussianSplatComponent* UNanoGSControlWidget::FindActiveComponent() const
{
	UGaussianSplatComponent* Fallback = nullptr;
	for (TObjectIterator<UGaussianSplatComponent> It; It; ++It)
	{
		UGaussianSplatComponent* Component = *It;
		if (Component == nullptr || Component->GetWorld() != GetWorld() || !Component->IsRegistered())
			continue;
		if (Component->GetTreeSourceAsset() != nullptr)
			return Component;
		Fallback = Component;
	}
	return Fallback;
}

FReply UNanoGSControlWidget::ToggleMinimized()
{
	bMinimized = !bMinimized;
	if (DetailBox.IsValid())
		DetailBox->SetVisibility(bMinimized ? EVisibility::Collapsed : EVisibility::Visible);
	if (MinimizeText.IsValid())
		MinimizeText->SetText(FText::FromString(bMinimized ? TEXT("+") : TEXT("-")));
	ApplyWindowSize();
	return FReply::Handled();
}

void UNanoGSControlWidget::ApplyWindowSize()
{
	SetDesiredSizeInViewport(FVector2D(WindowWidth, bMinimized ? MinimizedHeight : ExpandedHeight));
}

void UNanoGSControlWidget::RefreshStatus()
{
	UGaussianSplatComponent* Component = FindActiveComponent();
	if (Component == nullptr)
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::FromString(TEXT("Waiting for scene")));
			StatusText->SetColorAndOpacity(WaitingColor);
		}
		return;
	}
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(Component->bForceFullDetailLOD0
			? TEXT("Full LOD0") : TEXT("Dynamic LOD")));
		StatusText->SetColorAndOpacity(HealthyColor);
	}
	if (DetailStatusText.IsValid())
	{
		DetailStatusText->SetText(FText::FromString(FString::Printf(
			TEXT("Active %d splats · %s\n%s"),
			Component->ActiveSplats, *Component->ResidentPages, *Component->TreeLODStatus)));
	}
}

ECheckBoxState UNanoGSControlWidget::GetAfterTonemapState() const
{
	const IConsoleVariable* Variable = GetAfterTonemapVariable();
	return Variable != nullptr && Variable->GetInt() >= 1 ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

ECheckBoxState UNanoGSControlWidget::GetForceLOD0State() const
{
	const UGaussianSplatComponent* Component = FindActiveComponent();
	return Component != nullptr && Component->bForceFullDetailLOD0
		? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void UNanoGSControlWidget::OnAfterTonemapChanged(const ECheckBoxState NewState)
{
	if (IConsoleVariable* Variable = GetAfterTonemapVariable())
		Variable->Set(NewState == ECheckBoxState::Checked ? 1 : 0, ECVF_SetByConsole);
}

void UNanoGSControlWidget::OnForceLOD0Changed(const ECheckBoxState NewState)
{
	const bool bEnabled = NewState == ECheckBoxState::Checked;
	ForEachActiveComponent(GetWorld(), [bEnabled](UGaussianSplatComponent& Component)
	{
		Component.SetForceFullDetailLOD0(bEnabled);
	});
}

TOptional<float> UNanoGSControlWidget::GetStableBudgetMillions() const
{
	const UGaussianSplatComponent* Component = FindActiveComponent();
	return Component != nullptr ? TOptional<float>(Component->TreeLODSplatBudget / 1000000.0f) : TOptional<float>();
}

TOptional<float> UNanoGSControlWidget::GetMovingBudgetMillions() const
{
	const UGaussianSplatComponent* Component = FindActiveComponent();
	return Component != nullptr ? TOptional<float>(Component->TreeLODProgressiveSplatBudget / 1000000.0f) : TOptional<float>();
}

TOptional<float> UNanoGSControlWidget::GetLODDetail() const
{
	const UGaussianSplatComponent* Component = FindActiveComponent();
	return Component != nullptr ? TOptional<float>(Component->TreeLODDetailScale) : TOptional<float>();
}

TOptional<float> UNanoGSControlWidget::GetOpacity() const
{
	const UGaussianSplatComponent* Component = FindActiveComponent();
	return Component != nullptr ? TOptional<float>(Component->OpacityScale) : TOptional<float>();
}

void UNanoGSControlWidget::OnStableBudgetCommitted(const float Value, ETextCommit::Type)
{
	const int32 Budget = FMath::RoundToInt(FMath::Clamp(Value, 0.1f, 30.0f) * 1000000.0f);
	ForEachActiveComponent(GetWorld(), [Budget](UGaussianSplatComponent& Component)
	{
		Component.SetTreeLODSplatBudget(Budget);
	});
}

void UNanoGSControlWidget::OnMovingBudgetCommitted(const float Value, ETextCommit::Type)
{
	const int32 Budget = FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 30.0f) * 1000000.0f);
	ForEachActiveComponent(GetWorld(), [Budget](UGaussianSplatComponent& Component)
	{
		Component.SetTreeLODProgressiveSplatBudget(Budget);
	});
}

void UNanoGSControlWidget::OnLODDetailCommitted(const float Value, ETextCommit::Type)
{
	ForEachActiveComponent(GetWorld(), [Value](UGaussianSplatComponent& Component)
	{
		Component.SetTreeLODDetailScale(Value);
	});
}

void UNanoGSControlWidget::OnOpacityCommitted(const float Value, ETextCommit::Type)
{
	ForEachActiveComponent(GetWorld(), [Value](UGaussianSplatComponent& Component)
	{
		Component.SetOpacityScale(Value);
	});
}

FReply UNanoGSControlWidget::ResetDefaults()
{
	if (IConsoleVariable* Variable = GetAfterTonemapVariable())
		Variable->Set(1, ECVF_SetByConsole);
	ForEachActiveComponent(GetWorld(), [](UGaussianSplatComponent& Component)
	{
		Component.SetForceFullDetailLOD0(false);
		Component.SetTreeLODSplatBudget(DefaultStableBudget);
		Component.SetTreeLODProgressiveSplatBudget(DefaultMovingBudget);
		Component.SetTreeLODDetailScale(DefaultLODDetail);
		Component.SetOpacityScale(DefaultOpacity);
	});
	RefreshStatus();
	return FReply::Handled();
}
