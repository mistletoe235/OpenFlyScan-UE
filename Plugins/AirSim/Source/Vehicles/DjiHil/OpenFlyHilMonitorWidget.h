#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "OpenFlyHilMonitorWidget.generated.h"

class STextBlock;
class SVerticalBox;

UCLASS()
class AIRSIM_API UOpenFlyHilMonitorWidget final : public UUserWidget
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
    FReply ToggleMinimized();
    void RefreshStatus();
    void ApplyWindowSize();
    bool ApplyDefaultWindowPosition();

    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<STextBlock> MinimizeText;
    TSharedPtr<STextBlock> ConnectionText;
    TSharedPtr<STextBlock> PoseText;
    TSharedPtr<STextBlock> TransformText;
    TSharedPtr<STextBlock> AttitudeText;
    TSharedPtr<STextBlock> ControlText;
    TSharedPtr<STextBlock> FrameText;
    TSharedPtr<STextBlock> SafetyText;
    TSharedPtr<SVerticalBox> DetailBox;

    bool bMinimized = false;
    bool bDragging = false;
    bool bDefaultPositionApplied = false;
    FVector2D DragOffset = FVector2D::ZeroVector;
    FVector2D WindowPosition = FVector2D::ZeroVector;
    double RefreshAccumulator = 0.0;
    double LastRateSampleSeconds = 0.0;
    uint64 LastPoseCount = 0;
    uint64 LastFrameCount = 0;
    uint64 LastBytesSent = 0;
    double PoseRateHz = 0.0;
    double FrameRateHz = 0.0;
    double MegabitsPerSecond = 0.0;
};
