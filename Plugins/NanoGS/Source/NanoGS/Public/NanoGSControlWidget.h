#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "NanoGSControlWidget.generated.h"

class STextBlock;
class SVerticalBox;
class UGaussianSplatComponent;

/** Compact runtime controls for the user-facing NanoGS quality settings. */
UCLASS()
class NANOGS_API UNanoGSControlWidget final : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry,
		const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry,
		const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry,
		const FPointerEvent& InMouseEvent) override;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UGaussianSplatComponent* FindActiveComponent() const;
	FReply ToggleMinimized();
	FReply ResetDefaults();
	void ApplyWindowSize();
	void RefreshStatus();

	ECheckBoxState GetAfterTonemapState() const;
	ECheckBoxState GetForceLOD0State() const;
	void OnAfterTonemapChanged(ECheckBoxState NewState);
	void OnForceLOD0Changed(ECheckBoxState NewState);

	TOptional<float> GetStableBudgetMillions() const;
	TOptional<float> GetMovingBudgetMillions() const;
	TOptional<float> GetLODDetail() const;
	TOptional<float> GetOpacity() const;
	void OnStableBudgetCommitted(float Value, ETextCommit::Type CommitType);
	void OnMovingBudgetCommitted(float Value, ETextCommit::Type CommitType);
	void OnLODDetailCommitted(float Value, ETextCommit::Type CommitType);
	void OnOpacityCommitted(float Value, ETextCommit::Type CommitType);

	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<STextBlock> DetailStatusText;
	TSharedPtr<STextBlock> MinimizeText;
	TSharedPtr<SVerticalBox> DetailBox;

	bool bMinimized = false;
	bool bDragging = false;
	FVector2D DragOffset = FVector2D::ZeroVector;
	FVector2D WindowPosition = FVector2D(24.0f, 24.0f);
	float RefreshAccumulator = 0.0f;
};
