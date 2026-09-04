#include "Vehicles/DjiHil/OpenFlyHilMonitorWidget.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Input/Reply.h"
#include "Styling/CoreStyle.h"
#include "Vehicles/DjiHil/DjiHilPawnSimApi.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
constexpr float WindowWidth = 390.0f;
constexpr float ExpandedHeight = 310.0f;
constexpr float MinimizedHeight = 42.0f;
const FLinearColor PanelColor(0.025f, 0.035f, 0.055f, 0.94f);
const FLinearColor HeaderColor(0.055f, 0.075f, 0.11f, 0.98f);
const FLinearColor PrimaryText(0.90f, 0.94f, 1.0f, 1.0f);
const FLinearColor SecondaryText(0.58f, 0.66f, 0.76f, 1.0f);
const FLinearColor HealthyColor(0.22f, 0.90f, 0.58f, 1.0f);
const FLinearColor WaitingColor(1.0f, 0.72f, 0.24f, 1.0f);
const FLinearColor ErrorColor(1.0f, 0.34f, 0.36f, 1.0f);

TSharedRef<STextBlock> MakeValueText(TSharedPtr<STextBlock>& OutText)
{
    return SAssignNew(OutText, STextBlock)
        .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10))
        .ColorAndOpacity(PrimaryText)
        .Text(FText::FromString(TEXT("--")));
}
}

TSharedRef<SWidget> UOpenFlyHilMonitorWidget::RebuildWidget()
{
    TSharedRef<SWidget> Result =
        SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
        .BorderBackgroundColor(PanelColor)
        .Padding(0.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SBorder)
                .BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
                .BorderBackgroundColor(HeaderColor)
                .Padding(FMargin(12.0f, 8.0f))
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
                        .ColorAndOpacity(PrimaryText)
                        .Text(FText::FromString(TEXT("OPENFLY HIL")))
                    ]
                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    .Padding(12.0f, 0.0f)
                    .VAlign(VAlign_Center)
                    [
                        SAssignNew(StatusText, STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10))
                        .ColorAndOpacity(WaitingColor)
                        .Text(FText::FromString(TEXT("正在启动")))
                    ]
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    [
                        SNew(SButton)
                        .ButtonColorAndOpacity(FLinearColor(0.12f, 0.15f, 0.21f, 1.0f))
                        .ContentPadding(FMargin(10.0f, 1.0f))
                        .OnClicked(FOnClicked::CreateUObject(this,
                            &UOpenFlyHilMonitorWidget::ToggleMinimized))
                        [
                            SAssignNew(MinimizeText, STextBlock)
                            .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
                            .ColorAndOpacity(PrimaryText)
                            .Justification(ETextJustify::Center)
                            .Text(FText::FromString(TEXT("-")))
                        ]
                    ]
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SAssignNew(DetailBox, SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(12.0f, 10.0f, 12.0f, 4.0f)
                [ MakeValueText(ConnectionText) ]
                + SVerticalBox::Slot().AutoHeight().Padding(12.0f, 3.0f)
                [ MakeValueText(PoseText) ]
                + SVerticalBox::Slot().AutoHeight().Padding(12.0f, 3.0f)
                [ MakeValueText(TransformText) ]
                + SVerticalBox::Slot().AutoHeight().Padding(12.0f, 3.0f)
                [ MakeValueText(AttitudeText) ]
                + SVerticalBox::Slot().AutoHeight().Padding(12.0f, 3.0f)
                [ MakeValueText(ControlText) ]
                + SVerticalBox::Slot().AutoHeight().Padding(12.0f, 3.0f)
                [ MakeValueText(FrameText) ]
                + SVerticalBox::Slot().AutoHeight().Padding(12.0f, 6.0f, 12.0f, 10.0f)
                [ MakeValueText(SafetyText) ]
            ]
        ];
    return Result;
}

void UOpenFlyHilMonitorWidget::NativeConstruct()
{
    Super::NativeConstruct();
    SetAlignmentInViewport(FVector2D::ZeroVector);
    bDefaultPositionApplied = ApplyDefaultWindowPosition();
    ApplyWindowSize();
    RefreshStatus();
}

void UOpenFlyHilMonitorWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);
    if (!bDefaultPositionApplied && !bDragging)
        bDefaultPositionApplied = ApplyDefaultWindowPosition();
    RefreshAccumulator += InDeltaTime;
    if (RefreshAccumulator >= 0.2) {
        RefreshAccumulator = 0.0;
        RefreshStatus();
    }
}

FReply UOpenFlyHilMonitorWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry,
    const FPointerEvent& InMouseEvent)
{
    if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton
        && InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition()).Y <= 40.0f) {
        bDragging = true;
        DragOffset = InMouseEvent.GetScreenSpacePosition() - WindowPosition;
        return FReply::Handled().CaptureMouse(GetCachedWidget().ToSharedRef());
    }
    return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UOpenFlyHilMonitorWidget::NativeOnMouseButtonUp(const FGeometry& InGeometry,
    const FPointerEvent& InMouseEvent)
{
    if (bDragging && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton) {
        bDragging = false;
        return FReply::Handled().ReleaseMouseCapture();
    }
    return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
}

FReply UOpenFlyHilMonitorWidget::NativeOnMouseMove(const FGeometry& InGeometry,
    const FPointerEvent& InMouseEvent)
{
    if (bDragging) {
        FVector2D Position = InMouseEvent.GetScreenSpacePosition() - DragOffset;
        Position.X = FMath::Max(0.0f, Position.X);
        Position.Y = FMath::Max(0.0f, Position.Y);
        WindowPosition = Position;
        SetPositionInViewport(WindowPosition, false);
        return FReply::Handled();
    }
    return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
}

FReply UOpenFlyHilMonitorWidget::ToggleMinimized()
{
    bMinimized = !bMinimized;
    if (DetailBox.IsValid())
        DetailBox->SetVisibility(bMinimized ? EVisibility::Collapsed : EVisibility::Visible);
    if (MinimizeText.IsValid())
        MinimizeText->SetText(FText::FromString(bMinimized ? TEXT("+") : TEXT("-")));
    ApplyWindowSize();
    return FReply::Handled();
}

bool UOpenFlyHilMonitorWidget::ApplyDefaultWindowPosition()
{
    FVector2D ViewportSize = FVector2D::ZeroVector;
    if (GEngine == nullptr || GEngine->GameViewport == nullptr)
        return false;
    GEngine->GameViewport->GetViewportSize(ViewportSize);
    if (ViewportSize.X <= WindowWidth || ViewportSize.Y <= 0.0f)
        return false;
    constexpr float ViewportMargin = 24.0f;
    WindowPosition = FVector2D(ViewportSize.X - WindowWidth - ViewportMargin, ViewportMargin);
    SetPositionInViewport(WindowPosition, false);
    return true;
}

void UOpenFlyHilMonitorWidget::ApplyWindowSize()
{
    SetDesiredSizeInViewport(FVector2D(WindowWidth,
        bMinimized ? MinimizedHeight : ExpandedHeight));
}

void UOpenFlyHilMonitorWidget::RefreshStatus()
{
    FOpenFlyHilMonitorSnapshot Snapshot;
    if (!DjiHilPawnSimApi::GetActiveOpenFlyMonitorSnapshot(Snapshot)) {
        if (StatusText.IsValid()) {
            StatusText->SetText(FText::FromString(TEXT("等待 OpenFly 后端")));
            StatusText->SetColorAndOpacity(WaitingColor);
        }
        return;
    }

    const double NowSeconds = FPlatformTime::Seconds();
    if (LastRateSampleSeconds <= 0.0) {
        LastRateSampleSeconds = NowSeconds;
        LastPoseCount = Snapshot.AcceptedPoseCount;
        LastFrameCount = Snapshot.CompletedFrameCount;
        LastBytesSent = Snapshot.BytesSent;
    }
    else if (NowSeconds - LastRateSampleSeconds >= 0.5) {
        const double Elapsed = NowSeconds - LastRateSampleSeconds;
        PoseRateHz = static_cast<double>(Snapshot.AcceptedPoseCount - LastPoseCount) / Elapsed;
        FrameRateHz = static_cast<double>(Snapshot.CompletedFrameCount - LastFrameCount) / Elapsed;
        MegabitsPerSecond = static_cast<double>(Snapshot.BytesSent - LastBytesSent)
            * 8.0 / Elapsed / 1000000.0;
        LastRateSampleSeconds = NowSeconds;
        LastPoseCount = Snapshot.AcceptedPoseCount;
        LastFrameCount = Snapshot.CompletedFrameCount;
        LastBytesSent = Snapshot.BytesSent;
    }

    const double FreshnessMs = Snapshot.LastValidPacketHostNs == 0 ? 0.0
        : FMath::Max(0.0, (NowSeconds * 1000000000.0
            - static_cast<double>(Snapshot.LastValidPacketHostNs)) / 1000000.0);
    if (StatusText.IsValid()) {
        if (!Snapshot.bPeerConnected) {
            StatusText->SetText(FText::FromString(TEXT("等待手机连接")));
            StatusText->SetColorAndOpacity(WaitingColor);
        }
        else if (!Snapshot.bFrameClientConnected) {
            StatusText->SetText(FText::FromString(Snapshot.LastPoseSequence == 0
                ? TEXT("等待 DJI Simulator · 图像未连接")
                : TEXT("姿态在线 · 图像未连接")));
            StatusText->SetColorAndOpacity(WaitingColor);
        }
        else if (Snapshot.LastPoseSequence == 0) {
            StatusText->SetText(FText::FromString(TEXT("图像在线 · 等待 DJI Simulator")));
            StatusText->SetColorAndOpacity(WaitingColor);
        }
        else {
            StatusText->SetText(FText::FromString(TEXT("运行正常")));
            StatusText->SetColorAndOpacity(HealthyColor);
        }
    }
    if (ConnectionText.IsValid())
        ConnectionText->SetText(FText::FromString(FString::Printf(
            TEXT("连接  %s   Session %llu   新鲜度 %.0f ms"),
            Snapshot.bPeerConnected ? *Snapshot.PeerIp : TEXT("--"),
            Snapshot.SessionId, FreshnessMs)));
    if (PoseText.IsValid())
        PoseText->SetText(FText::FromString(FString::Printf(
            TEXT("姿态  %.1f Hz   Seq %llu   FC age %u ms"),
            PoseRateHz, Snapshot.LastPoseSequence, Snapshot.FlightControllerStateAgeMs)));
    if (TransformText.IsValid())
        TransformText->SetText(FText::FromString(FString::Printf(
            TEXT("ENU   E %.2f   N %.2f   U %.2f m"),
            Snapshot.EastM, Snapshot.NorthM, Snapshot.UpM)));
    if (AttitudeText.IsValid())
        AttitudeText->SetText(FText::FromString(FString::Printf(
            TEXT("姿态  R %.1f°   P %.1f°   H %.1f°   云台 %.1f°"),
            Snapshot.RollDeg, Snapshot.PitchDeg,
            Snapshot.HeadingDeg, Snapshot.GimbalPitchDeg)));
    if (ControlText.IsValid()) {
        const bool bMotorStarted = (Snapshot.StateFlags & 1u) != 0;
        const bool bInTheAir = (Snapshot.StateFlags & 2u) != 0;
        ControlText->SetText(FText::FromString(FString::Printf(
            TEXT("飞控  %s%s   摇杆 F %.2f R %.2f U %.2f Y %.1f°/s"),
            bMotorStarted ? TEXT("电机启动") : TEXT("电机停止"),
            bInTheAir ? TEXT(" · 飞行中") : TEXT(" · 地面"),
            Snapshot.CommandForwardMps, Snapshot.CommandRightMps,
            Snapshot.CommandUpMps, Snapshot.CommandYawRateDegPerSec)));
        ControlText->SetColorAndOpacity(bInTheAir ? HealthyColor : PrimaryText);
    }
    if (FrameText.IsValid())
        FrameText->SetText(FText::FromString(FString::Printf(
            TEXT("图像  %ux%u %s   %.1f / %u fps   %.1f Mbps"),
            Snapshot.FrameWidth, Snapshot.FrameHeight, *Snapshot.FrameFormat,
            FrameRateHz, Snapshot.MaximumFrameRate, MegabitsPerSecond)));
    if (SafetyText.IsValid()) {
        SafetyText->SetText(FText::FromString(FString::Printf(
            TEXT("安全  EVENT %llu   拒绝 %llu   非法 %llu   断链 %llu   捕获失败 %llu"),
            Snapshot.SentEventCount, Snapshot.RejectedPacketCount,
            Snapshot.InvalidDatagramCount, Snapshot.LinkLossCount,
            Snapshot.CaptureFailureCount)));
        SafetyText->SetColorAndOpacity(
            Snapshot.InvalidDatagramCount > 0 || Snapshot.CaptureFailureCount > 0
                ? ErrorColor : SecondaryText);
    }
}
