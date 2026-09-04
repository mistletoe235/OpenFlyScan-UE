#include "SimWorldGameMode.h"

#include "AirBlueprintLib.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Components/PrimitiveComponent.h"
#include "IImageWrapperModule.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "UObject/ConstructorHelpers.h"

#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/PlayerStart.h"
#include "Vehicles/Car/SimModeCar.h"
#include "Vehicles/ComputerVision/SimModeComputerVision.h"
#include "Vehicles/Multirotor/SimModeWorldMultiRotor.h"
#include "Vehicles/DjiHil/DjiHilPawnSimApi.h"
#include "Vehicles/DjiHil/OpenFlyHilMonitorWidget.h"

#include "common/Common.hpp"

#include <stdexcept>

class FSimWorldUnrealLog : public msr::airlib::Utils::Logger
{
public:
    virtual void log(int level, const std::string& message) override
    {
        size_t tab_pos;
        static const std::string delim = ":\t";
        if ((tab_pos = message.find(delim)) != std::string::npos) {
            UAirBlueprintLib::LogMessageString(message.substr(0, tab_pos),
                                               message.substr(tab_pos + delim.size(), std::string::npos),
                                               LogDebugLevel::Informational);
            return;
        }

        if (level == msr::airlib::Utils::kLogLevelError) {
            UE_LOG(LogTemp, Error, TEXT("%s"), *FString(message.c_str()));
        }
        else if (level == msr::airlib::Utils::kLogLevelWarn) {
            UE_LOG(LogTemp, Warning, TEXT("%s"), *FString(message.c_str()));
        }
        else {
            UE_LOG(LogTemp, Log, TEXT("%s"), *FString(message.c_str()));
        }

        msr::airlib::Utils::Logger::log(level, message);
    }
};

static FSimWorldUnrealLog GSimWorldLog;

ASimWorldGameMode::ASimWorldGameMode(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    DefaultPawnClass = nullptr;

    static ConstructorHelpers::FClassFinder<UUserWidget> SimHudWidgetClass(
        TEXT("WidgetBlueprint'/AirSim/Blueprints/BP_SimHUDWidget'"));
    WidgetClass_ = SimHudWidgetClass.Succeeded() ? SimHudWidgetClass.Class : nullptr;
    if (WidgetClass_ == nullptr) {
        UE_LOG(LogTemp, Warning, TEXT("SimWorldGameMode: failed to load AirSim HUD widget blueprint"));
    }

    common_utils::Utils::getSetLogger(&GSimWorldLog);

    static IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    (void)ImageWrapperModule;
}

void ASimWorldGameMode::BeginPlay()
{
    Super::BeginPlay();

    bAutoHideRuntimeUIInFullscreen_ =
        FParse::Param(FCommandLine::Get(), TEXT("AutoHideRuntimeUIInFullscreen"));
    FString RuntimeUIMode;
    if (FParse::Value(FCommandLine::Get(), TEXT("RuntimeUI="), RuntimeUIMode)) {
        bOpenFlyHilMonitorUIEnabled_ =
            RuntimeUIMode.Equals(TEXT("All"), ESearchCase::IgnoreCase)
            || RuntimeUIMode.Equals(TEXT("HIL"), ESearchCase::IgnoreCase);
    }

    EnsureSpectatorAnchor();

    try {
        UAirBlueprintLib::OnBeginPlay();
        InitializeAirSimSettings();
        SetUnrealEngineSettings();
        CreateSimMode();
        CreateAirSimWidget();
        TryCreateOpenFlyHilMonitorWidget();
        SetupAirSimInputBindings();

        if (SimMode_ != nullptr) {
            SimMode_->startApiServer();
        }
    }
    catch (const std::exception& Ex) {
        UAirBlueprintLib::LogMessageString("Error at AirSim startup: ", Ex.what(), LogDebugLevel::Failure);
        UAirBlueprintLib::ShowMessage(
            EAppMsgType::Ok,
            std::string("Error at AirSim startup: ") + Ex.what(),
            "Error");
    }
}

void ASimWorldGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    TryCreateOpenFlyHilMonitorWidget();
    UpdateOpenFlyHilMonitorVisibility();

    if (SimMode_ != nullptr && SimMode_->EnableReport && Widget_ != nullptr) {
        Widget_->updateDebugReport(SimMode_->getDebugReport());
    }
}

void ASimWorldGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (SimMode_ != nullptr) {
        SimMode_->stopApiServer();
    }

    if (OpenFlyHilMonitorWidget_ != nullptr) {
        OpenFlyHilMonitorWidget_->RemoveFromParent();
        OpenFlyHilMonitorWidget_ = nullptr;
    }

    if (Widget_ != nullptr) {
        Widget_->RemoveFromParent();
        Widget_ = nullptr;
    }

    if (SimMode_ != nullptr) {
        SimMode_->Destroy();
        SimMode_ = nullptr;
    }

    UAirBlueprintLib::OnEndPlay();

    Super::EndPlay(EndPlayReason);
}

void ASimWorldGameMode::InputEventToggleRecording()
{
    ToggleRecordHandler();
}

void ASimWorldGameMode::InputEventToggleReport()
{
    if (SimMode_ == nullptr || Widget_ == nullptr) {
        return;
    }

    SimMode_->EnableReport = !SimMode_->EnableReport;
    Widget_->setReportVisible(SimMode_->EnableReport);
}

void ASimWorldGameMode::InputEventToggleHelp()
{
    if (Widget_ != nullptr) {
        Widget_->toggleHelpVisibility();
    }
}

void ASimWorldGameMode::InputEventToggleTrace()
{
    if (SimMode_ != nullptr) {
        SimMode_->toggleTraceAll();
    }
}

void ASimWorldGameMode::InputEventToggleSubwindow0()
{
    ToggleSubwindowVisibility(0);
}

void ASimWorldGameMode::InputEventToggleSubwindow1()
{
    ToggleSubwindowVisibility(1);
}

void ASimWorldGameMode::InputEventToggleSubwindow2()
{
    ToggleSubwindowVisibility(2);
}

void ASimWorldGameMode::InputEventToggleAll()
{
    auto& Settings = GetSubWindowSettings();
    Settings.at(0).visible = !Settings.at(0).visible;
    Settings.at(1).visible = Settings.at(0).visible;
    Settings.at(2).visible = Settings.at(0).visible;
    UpdateWidgetSubwindowVisibility();
}

void ASimWorldGameMode::SetupAirSimInputBindings()
{
    UAirBlueprintLib::EnableInput(this);

    UAirBlueprintLib::BindActionToKey("inputEventToggleRecording", EKeys::R, this, &ASimWorldGameMode::InputEventToggleRecording);
    UAirBlueprintLib::BindActionToKey("InputEventToggleReport", EKeys::Semicolon, this, &ASimWorldGameMode::InputEventToggleReport);
    UAirBlueprintLib::BindActionToKey("InputEventToggleHelp", EKeys::F1, this, &ASimWorldGameMode::InputEventToggleHelp);
    UAirBlueprintLib::BindActionToKey("InputEventToggleTrace", EKeys::T, this, &ASimWorldGameMode::InputEventToggleTrace);
    UAirBlueprintLib::BindActionToKey("InputEventToggleSubwindow0", EKeys::One, this, &ASimWorldGameMode::InputEventToggleSubwindow0);
    UAirBlueprintLib::BindActionToKey("InputEventToggleSubwindow1", EKeys::Two, this, &ASimWorldGameMode::InputEventToggleSubwindow1);
    UAirBlueprintLib::BindActionToKey("InputEventToggleSubwindow2", EKeys::Three, this, &ASimWorldGameMode::InputEventToggleSubwindow2);
    UAirBlueprintLib::BindActionToKey("InputEventToggleAll", EKeys::Zero, this, &ASimWorldGameMode::InputEventToggleAll);
}

void ASimWorldGameMode::ToggleRecordHandler()
{
    if (SimMode_ != nullptr) {
        SimMode_->toggleRecording();
    }
}

void ASimWorldGameMode::UpdateWidgetSubwindowVisibility()
{
    if (Widget_ == nullptr) {
        return;
    }

    for (int WindowIndex = 0; WindowIndex < AirSimSettings::kSubwindowCount; ++WindowIndex) {
        APIPCamera* Camera = SubwindowCameras_[WindowIndex];
        ImageType CameraType = GetSubWindowSettings().at(WindowIndex).image_type;
        bool bIsVisible = GetSubWindowSettings().at(WindowIndex).visible && Camera != nullptr;

        if (Camera != nullptr) {
            Camera->setCameraTypeEnabled(CameraType, bIsVisible);
            Camera->setCameraTypeUpdate(CameraType, false);
        }

        Widget_->setSubwindowVisibility(
            WindowIndex,
            bIsVisible,
            bIsVisible ? Camera->getRenderTarget(CameraType, false) : nullptr);
    }
}

bool ASimWorldGameMode::IsWidgetSubwindowVisible(int WindowIndex) const
{
    return Widget_ != nullptr && Widget_->getSubwindowVisibility(WindowIndex) != 0;
}

void ASimWorldGameMode::ToggleSubwindowVisibility(int WindowIndex)
{
    auto& Settings = GetSubWindowSettings();
    Settings.at(WindowIndex).visible = !Settings.at(WindowIndex).visible;
    UpdateWidgetSubwindowVisibility();
}

void ASimWorldGameMode::EnsureSpectatorAnchor()
{
    if (CachedSpectator_ != nullptr || GetWorld() == nullptr) {
        return;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    // Replaceable NanoGS scenes resolve their tagged PlayerStart before the
    // game mode begins play.  AirSim derives its global NED origin from this
    // spectator, so spawning it at identity would silently ignore the scene's
    // runtime-selected ground location.
    FTransform SpectatorTransform = FTransform::Identity;
    APlayerStart* RuntimePlayerStart = nullptr;
    for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It) {
        if (It->ActorHasTag(TEXT("NanoGS.PlayerStart"))) {
            RuntimePlayerStart = *It;
            break;
        }
    }
    if (RuntimePlayerStart != nullptr) {
        SpectatorTransform = RuntimePlayerStart->GetActorTransform();
    }

    APawn* SpectatorPawn = GetWorld()->SpawnActor<ASpectatorPawn>(
        ASpectatorPawn::StaticClass(), SpectatorTransform, SpawnParams);
    if (SpectatorPawn == nullptr) {
        UE_LOG(LogTemp, Error, TEXT("SimWorldGameMode: failed to create spectator anchor"));
        return;
    }

    // This pawn anchors the global AirSim NED origin. It shares
    // the tagged PlayerStart with the AirSim vehicle, so leaving its default
    // collision enabled can pin the multirotor at spawn and make takeoff or
    // horizontal velocity commands fail while the body merely tilts.
    SpectatorPawn->SetActorEnableCollision(false);
    if (UPrimitiveComponent* RootPrimitive =
            Cast<UPrimitiveComponent>(SpectatorPawn->GetRootComponent())) {
        RootPrimitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        RootPrimitive->SetGenerateOverlapEvents(false);
    }

    CachedSpectator_ = SpectatorPawn;

    if (APlayerController* PlayerController = GetWorld()->GetFirstPlayerController()) {
        PlayerController->Possess(SpectatorPawn);
        PlayerController->SetViewTarget(SpectatorPawn);
    }

    UE_LOG(LogTemp, Log,
        TEXT("NANOGS_AIRSIM_ORIGIN source=%s location=(%.1f,%.1f,%.1f)"),
        RuntimePlayerStart != nullptr ? TEXT("tagged-player-start") : TEXT("world-origin"),
        SpectatorTransform.GetLocation().X,
        SpectatorTransform.GetLocation().Y,
        SpectatorTransform.GetLocation().Z);
    UE_LOG(LogTemp, Log, TEXT("SimWorldGameMode: spectator anchor created"));
}

void ASimWorldGameMode::InitializeAirSimSettings()
{
    std::string SettingsText;
    if (GetSettingsText(SettingsText)) {
        AirSimSettings::initializeSettings(SettingsText);
    }
    else {
        AirSimSettings::createDefaultSettingsFile();
    }

    AirSimSettings::singleton().load(std::bind(&ASimWorldGameMode::GetSimModeFromUser, this));

    for (const auto& Warning : AirSimSettings::singleton().warning_messages) {
        UAirBlueprintLib::LogMessageString(Warning, "", LogDebugLevel::Failure);
    }
    for (const auto& Error : AirSimSettings::singleton().error_messages) {
        UAirBlueprintLib::ShowMessage(EAppMsgType::Ok, Error, "settings.json");
    }
}

void ASimWorldGameMode::SetUnrealEngineSettings()
{
    if (GetWorld() == nullptr || GetWorld()->GetGameViewport() == nullptr) {
        return;
    }

    GetWorld()->GetGameViewport()->GetEngineShowFlags()->SetMotionBlur(false);

    static const auto CustomDepthVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"));
    if (CustomDepthVar != nullptr) {
        CustomDepthVar->Set(3);
    }

    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), FString("r.CustomDepth 3"));

    static const auto RenderTimeoutVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("g.TimeoutForBlockOnRenderFence"));
    if (RenderTimeoutVar != nullptr) {
        RenderTimeoutVar->Set(300000);
    }
}

void ASimWorldGameMode::CreateSimMode()
{
    const std::string SimModeName = AirSimSettings::singleton().simmode_name;

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

    if (SimModeName == AirSimSettings::kSimModeTypeMultirotor) {
        SimMode_ = GetWorld()->SpawnActor<ASimModeWorldMultiRotor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
    }
    else if (SimModeName == AirSimSettings::kSimModeTypeCar) {
        SimMode_ = GetWorld()->SpawnActor<ASimModeCar>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
    }
    else if (SimModeName == AirSimSettings::kSimModeTypeComputerVision) {
        SimMode_ = GetWorld()->SpawnActor<ASimModeComputerVision>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
    }
    else {
        UAirBlueprintLib::ShowMessage(EAppMsgType::Ok, std::string("SimMode is not valid: ") + SimModeName, "Error");
        UAirBlueprintLib::LogMessageString("SimMode is not valid: ", SimModeName, LogDebugLevel::Failure);
    }
}

void ASimWorldGameMode::CreateAirSimWidget()
{
    if (WidgetClass_ == nullptr || GetWorld() == nullptr) {
        UAirBlueprintLib::LogMessage(TEXT("Cannot instantiate BP_SimHUDWidget blueprint!"), TEXT(""), LogDebugLevel::Failure);
        return;
    }

    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (PlayerController == nullptr) {
        UAirBlueprintLib::LogMessage(TEXT("Cannot find player controller for AirSim widget."), TEXT(""), LogDebugLevel::Failure);
        return;
    }

    Widget_ = CreateWidget<USimHUDWidget>(PlayerController, WidgetClass_);
    if (Widget_ == nullptr) {
        UAirBlueprintLib::LogMessage(TEXT("Failed to create AirSim widget instance."), TEXT(""), LogDebugLevel::Failure);
        return;
    }

    InitializeSubWindows();

    Widget_->AddToViewport();
    Widget_->initializeForPlay();
    if (SimMode_ != nullptr) {
        Widget_->setReportVisible(SimMode_->EnableReport);
    }
    Widget_->setOnToggleRecordingHandler(std::bind(&ASimWorldGameMode::ToggleRecordHandler, this));
    Widget_->setRecordButtonVisibility(AirSimSettings::singleton().is_record_ui_visible);
    UpdateWidgetSubwindowVisibility();
}

void ASimWorldGameMode::TryCreateOpenFlyHilMonitorWidget()
{
    if (!bOpenFlyHilMonitorUIEnabled_
        || OpenFlyHilMonitorWidget_ != nullptr || GetWorld() == nullptr)
        return;

    FOpenFlyHilMonitorSnapshot Snapshot;
    if (!DjiHilPawnSimApi::GetActiveOpenFlyMonitorSnapshot(Snapshot))
        return;

    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (PlayerController == nullptr)
        return;

    OpenFlyHilMonitorWidget_ = CreateWidget<UOpenFlyHilMonitorWidget>(
        PlayerController, UOpenFlyHilMonitorWidget::StaticClass());
    if (OpenFlyHilMonitorWidget_ != nullptr) {
        if (!bAutoHideRuntimeUIInFullscreen_
            || GEngine == nullptr || GEngine->GameViewport == nullptr
            || !GEngine->GameViewport->IsFullScreenViewport()) {
            OpenFlyHilMonitorWidget_->AddToViewport(50);
        }
        PlayerController->bShowMouseCursor = true;
        FInputModeGameAndUI InputMode;
        InputMode.SetHideCursorDuringCapture(false);
        InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        PlayerController->SetInputMode(InputMode);
        UE_LOG(LogTemp, Display, TEXT("OpenFly HIL monitor UI created"));
    }
}

void ASimWorldGameMode::UpdateOpenFlyHilMonitorVisibility()
{
    if (OpenFlyHilMonitorWidget_ == nullptr || GEngine == nullptr
        || GEngine->GameViewport == nullptr) {
        return;
    }

    const bool bShouldShow = !bAutoHideRuntimeUIInFullscreen_
        || !GEngine->GameViewport->IsFullScreenViewport();
    if (bShouldShow && !OpenFlyHilMonitorWidget_->IsInViewport()) {
        OpenFlyHilMonitorWidget_->AddToViewport(50);
    }
    else if (!bShouldShow && OpenFlyHilMonitorWidget_->IsInViewport()) {
        OpenFlyHilMonitorWidget_->RemoveFromParent();
    }
}

void ASimWorldGameMode::InitializeSubWindows()
{
    if (SimMode_ == nullptr) {
        return;
    }

    auto DefaultVehicleSimApi = SimMode_->getVehicleSimApi();
    if (DefaultVehicleSimApi != nullptr) {
        const auto CameraCount = DefaultVehicleSimApi->getCameraCount();
        if (CameraCount > 0) {
            SubwindowCameras_[0] = DefaultVehicleSimApi->getCamera("");
            SubwindowCameras_[1] = DefaultVehicleSimApi->getCamera("");
            SubwindowCameras_[2] = DefaultVehicleSimApi->getCamera("");
        }
    }

    for (const auto& Setting : GetSubWindowSettings()) {
        APIPCamera* Camera = SimMode_->getCamera(
            msr::airlib::CameraDetails(Setting.camera_name, Setting.vehicle_name, Setting.external));
        if (Camera != nullptr) {
            SubwindowCameras_[Setting.window_index] = Camera;
        }
        else {
            UAirBlueprintLib::LogMessageString(
                "Invalid Camera settings in <SubWindows> element",
                std::to_string(Setting.window_index),
                LogDebugLevel::Failure);
        }
    }
}

bool ASimWorldGameMode::GetSettingsText(std::string& SettingsText)
{
    return GetSettingsTextFromCommandLine(SettingsText) ||
           ReadSettingsTextFromFile(FString(msr::airlib::Settings::getExecutableFullPath("settings.json").c_str()), SettingsText) ||
           ReadSettingsTextFromFile(GetLaunchPath("settings.json"), SettingsText) ||
           ReadSettingsTextFromFile(FString(msr::airlib::Settings::Settings::getUserDirectoryFullPath("settings.json").c_str()), SettingsText);
}

bool ASimWorldGameMode::GetSettingsTextFromCommandLine(std::string& SettingsText)
{
    const TCHAR* CommandLineArgs = FCommandLine::Get();
    FString SettingsJsonFString;

    if (FParse::Value(CommandLineArgs, TEXT("-settings="), SettingsJsonFString, false)) {
        if (ReadSettingsTextFromFile(SettingsJsonFString, SettingsText)) {
            return true;
        }

        UAirBlueprintLib::LogMessageString("Loaded settings from commandline: ", TCHAR_TO_UTF8(*SettingsJsonFString), LogDebugLevel::Informational);
        SettingsText = TCHAR_TO_UTF8(*SettingsJsonFString);
        return true;
    }

    return false;
}

bool ASimWorldGameMode::ReadSettingsTextFromFile(const FString& SettingsFilePath, std::string& SettingsText)
{
    const bool bFound = FPaths::FileExists(SettingsFilePath);
    if (!bFound) {
        return false;
    }

    FString SettingsTextFStr;
    const bool bReadSuccessful = FFileHelper::LoadFileToString(SettingsTextFStr, *SettingsFilePath);
    if (!bReadSuccessful) {
        UAirBlueprintLib::LogMessageString("Cannot read file ", TCHAR_TO_UTF8(*SettingsFilePath), LogDebugLevel::Failure);
        throw std::runtime_error("Cannot read settings file.");
    }

    UAirBlueprintLib::LogMessageString("Loaded settings from ", TCHAR_TO_UTF8(*SettingsFilePath), LogDebugLevel::Informational);
    SettingsText = TCHAR_TO_UTF8(*SettingsTextFStr);
    return true;
}

std::string ASimWorldGameMode::GetSimModeFromUser()
{
    if (EAppReturnType::No == UAirBlueprintLib::ShowMessage(
                                   EAppMsgType::YesNo,
                                   "Would you like to use car simulation? Choose no to use quadrotor simulation.",
                                   "Choose Vehicle")) {
        return AirSimSettings::kSimModeTypeMultirotor;
    }

    return AirSimSettings::kSimModeTypeCar;
}

FString ASimWorldGameMode::GetLaunchPath(const std::string& Filename)
{
    FString LaunchRelativePath = FPaths::LaunchDir();
    FString AbsolutePath = FPaths::ConvertRelativePathToFull(LaunchRelativePath);
    return FPaths::Combine(AbsolutePath, FString(Filename.c_str()));
}

const std::vector<ASimWorldGameMode::AirSimSettings::SubwindowSetting>& ASimWorldGameMode::GetSubWindowSettings() const
{
    return AirSimSettings::singleton().subwindow_settings;
}

std::vector<ASimWorldGameMode::AirSimSettings::SubwindowSetting>& ASimWorldGameMode::GetSubWindowSettings()
{
    return AirSimSettings::singleton().subwindow_settings;
}
