#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "PIPCamera.h"
#include "SimHUD/SimHUDWidget.h"
#include "SimMode/SimModeBase.h"
#include "common/AirSimSettings.hpp"

#include "SimWorldGameMode.generated.h"

class UOpenFlyHilMonitorWidget;

UCLASS()
class AIRSIM_API ASimWorldGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    using ImageType = msr::airlib::ImageCaptureBase::ImageType;
    using AirSimSettings = msr::airlib::AirSimSettings;

    ASimWorldGameMode(const FObjectInitializer& ObjectInitializer);

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    void InputEventToggleRecording();
    void InputEventToggleReport();
    void InputEventToggleHelp();
    void InputEventToggleTrace();
    void InputEventToggleSubwindow0();
    void InputEventToggleSubwindow1();
    void InputEventToggleSubwindow2();
    void InputEventToggleAll();

protected:
    void SetupAirSimInputBindings();
    void ToggleRecordHandler();
    void UpdateWidgetSubwindowVisibility();
    bool IsWidgetSubwindowVisible(int WindowIndex) const;
    void ToggleSubwindowVisibility(int WindowIndex);

private:
    void EnsureSpectatorAnchor();
    void InitializeAirSimSettings();
    void SetUnrealEngineSettings();
    void CreateSimMode();
    void CreateAirSimWidget();
    void TryCreateOpenFlyHilMonitorWidget();
    void UpdateOpenFlyHilMonitorVisibility();
    void InitializeSubWindows();

    bool GetSettingsText(std::string& SettingsText);
    bool GetSettingsTextFromCommandLine(std::string& SettingsText);
    bool ReadSettingsTextFromFile(const FString& SettingsFilePath, std::string& SettingsText);
    std::string GetSimModeFromUser();

    static FString GetLaunchPath(const std::string& Filename);

    const std::vector<AirSimSettings::SubwindowSetting>& GetSubWindowSettings() const;
    std::vector<AirSimSettings::SubwindowSetting>& GetSubWindowSettings();

private:
    UClass* WidgetClass_ = nullptr;

    UPROPERTY()
    USimHUDWidget* Widget_ = nullptr;

    UPROPERTY()
    UOpenFlyHilMonitorWidget* OpenFlyHilMonitorWidget_ = nullptr;

    bool bAutoHideRuntimeUIInFullscreen_ = false;
    bool bOpenFlyHilMonitorUIEnabled_ = true;

    UPROPERTY()
    ASimModeBase* SimMode_ = nullptr;

    UPROPERTY()
    APawn* CachedSpectator_ = nullptr;

    APIPCamera* SubwindowCameras_[AirSimSettings::kSubwindowCount] = {};
};
