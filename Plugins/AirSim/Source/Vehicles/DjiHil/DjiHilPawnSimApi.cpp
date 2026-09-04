#include "Vehicles/DjiHil/DjiHilPawnSimApi.h"

#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformTime.h"
#include "Engine/World.h"
#include "UnrealSensors/UnrealSensorFactory.h"

#include <sstream>

using namespace msr::airlib;

DjiHilPawnSimApi* DjiHilPawnSimApi::ActiveOpenFlyInstance = nullptr;

DjiHilPawnSimApi::DjiHilPawnSimApi(const Params& InParams)
    : PawnSimApi(InParams),
      PawnEvents(static_cast<MultirotorPawnEvents*>(InParams.pawn_events))
{
}

DjiHilPawnSimApi::~DjiHilPawnSimApi()
{
    if (ActiveOpenFlyInstance == this)
        ActiveOpenFlyInstance = nullptr;
    if (FrameServer.IsValid())
        FrameServer->Shutdown();
    if (UdpServer.IsValid())
        UdpServer->Shutdown();
}

void DjiHilPawnSimApi::initialize()
{
    PawnSimApi::initialize();

    RotorAnimation.resize(4);
    const int RotorDirections[4] = { 1, -1, -1, 1 };
    for (int RotorIndex = 0; RotorIndex < 4; ++RotorIndex)
        RotorAnimation[RotorIndex].rotor_direction = RotorDirections[RotorIndex];
    if (PawnEvents != nullptr)
        PawnEvents->getActuatorSignal().emit(RotorAnimation);

    const msr::airlib::AirSimSettings::VehicleSetting* VehicleSetting = getVehicleSetting();
    if (VehicleSetting == nullptr || VehicleSetting->dji_hil == nullptr)
        throw std::runtime_error("DjiHil vehicle is missing its DjiHil settings block");
    const msr::airlib::AirSimSettings::DjiHilSetting& Settings = *VehicleSetting->dji_hil;
    djihil::State FirstState;
    bool bHasFirstState = false;
    if (Settings.backend == "replay") {
        bIsReplayBackend = true;
        ActiveProvider = &ReplayProvider;
        if (Settings.replay_file.empty())
            throw std::runtime_error("DjiHil ReplayFile must be configured");
        if (Settings.replay_loop)
            throw std::runtime_error(
                "DjiHil ReplayLoop is disabled because source time and frame_id must never rewind");
        FString LoadError;
        const FString ReplayPath = ResolveReplayPath(Settings.replay_file);
        if (!ReplayProvider.LoadJsonLines(ReplayPath, LoadError))
            throw std::runtime_error(TCHAR_TO_UTF8(*LoadError));
        if (!ReplayProvider.GetSourceTimeRange(ReplayFirstSourceTimeNs, ReplayLastSourceTimeNs))
            throw std::runtime_error("DjiHil replay has no time range");
        InterpolationDelayNs = static_cast<uint64>(FMath::Max(0, Settings.interpolation_delay_ms)) * 1000000ull;
        ReplayStartClockNs = clock()->nowNanos();
        bHasFirstState = ReplayProvider.GetLatestState(FirstState);
        if (!bHasFirstState)
            throw std::runtime_error("DjiHil replay failed to publish its first state");
        UE_LOG(LogTemp, Display, TEXT("DjiHil Replay loaded %d frames from %s"),
            ReplayProvider.GetFrameCount(), *ReplayPath);
    }
    else if (Settings.backend == "openflyandroid" || Settings.backend == "openfly_android") {
        bIsReplayBackend = false;
        ActiveProvider = &LiveProvider;
        const auto ValidPort = [](int Value) { return Value > 0 && Value <= 65535; };
        if (!ValidPort(Settings.udp_port) || !ValidPort(Settings.android_reply_port)
            || !ValidPort(Settings.frame_tcp_port))
            throw std::runtime_error("DjiHil OpenFlyAndroid ports must be in 1..65535");
        if (Settings.peer_timeout_ms <= 0 || Settings.heartbeat_interval_ms <= 0
            || Settings.heartbeat_interval_ms >= Settings.peer_timeout_ms)
            throw std::runtime_error(
                "DjiHil OpenFlyAndroid heartbeat must be positive and shorter than peer timeout");
        openfly::hil::FUdpServerConfig UdpConfig;
        UdpConfig.BindAddress = UTF8_TO_TCHAR(Settings.udp_bind_address.c_str());
        UdpConfig.BindPort = static_cast<uint16>(Settings.udp_port);
        UdpConfig.AndroidReplyPort = static_cast<uint16>(Settings.android_reply_port);
        UdpConfig.PeerTimeoutMs = static_cast<uint32>(Settings.peer_timeout_ms);
        UdpConfig.HeartbeatIntervalMs = static_cast<uint32>(Settings.heartbeat_interval_ms);
        UdpServer = MakeUnique<openfly::hil::FOpenFlyHilUdpServer>(UdpConfig, LiveProvider);
        FString StartError;
        if (!UdpServer->Start(StartError))
            throw std::runtime_error(TCHAR_TO_UTF8(*StartError));
        UE_LOG(LogTemp, Display,
            TEXT("DjiHil OpenFlyAndroid UDP listening on %s:%d; reply=%d TCP=%d"),
            *UdpConfig.BindAddress, Settings.udp_port, Settings.android_reply_port,
            Settings.frame_tcp_port);
    }
    else {
        throw std::runtime_error("DjiHil Backend must be Replay or OpenFlyAndroid");
    }

    if (bHasFirstState) {
        ApplySourceState(FirstState);
        RenderState = FirstState;
        bHasRenderState = true;
    }

    const auto SensorFactory = std::make_shared<UnrealSensorFactory>(getPawn(), &getNedTransform());
    SensorFactory->createSensorsFromSettings(VehicleSetting->sensors, Sensors, SensorStorage);
    Sensors.initialize(getGroundTruthKinematics(), getGroundTruthEnvironment());

    GeoPoint Home = getGroundTruthEnvironment()->getState().geo_point;
    if (bHasFirstState && FirstState.has(djihil::ValidField::Gps))
        Home = FirstState.gps;
    VehicleApi = std::make_unique<DjiHilMultirotorApi>(
        ActiveProvider, &Sensors, Home, [this]() { return getImageCaptureMetadata(); },
        [this](uint32 Kind, double Score, const std::string& Reason) {
            return QueueSafetyEvent(Kind, Score, Reason);
        });

    if (!bIsReplayBackend) {
        if (Settings.frame_width <= 0 || Settings.frame_height <= 0
            || Settings.maximum_frame_bytes <= 0
            || Settings.maximum_frame_bytes > static_cast<int>(openfly::hil::MaximumFramePayloadBytes)
            || Settings.jpeg_quality < 1 || Settings.jpeg_quality > 100
            || Settings.maximum_frame_rate <= 0 || Settings.maximum_frame_rate > 240
            || Settings.frame_camera_name.empty())
            throw std::runtime_error("DjiHil OpenFlyAndroid frame settings are invalid");
        openfly::hil::FFrameServerConfig FrameConfig;
        FrameConfig.CameraName = UTF8_TO_TCHAR(Settings.frame_camera_name.c_str());
        LiveFrameCameraName = FrameConfig.CameraName;
        if (Settings.frame_format == "jpeg" || Settings.frame_format == "jpg")
            FrameConfig.Format = openfly::hil::EFrameFormat::Jpeg;
        else if (Settings.frame_format == "png")
            FrameConfig.Format = openfly::hil::EFrameFormat::Png;
        else
            throw std::runtime_error("DjiHil OpenFlyAndroid FrameFormat must be JPEG or PNG");
        FrameConfig.Width = static_cast<uint32>(Settings.frame_width);
        FrameConfig.Height = static_cast<uint32>(Settings.frame_height);
        FrameConfig.JpegQuality = static_cast<uint32>(Settings.jpeg_quality);
        FrameConfig.MaximumPayloadBytes = static_cast<uint32>(Settings.maximum_frame_bytes);
        FrameConfig.MaximumFrameRate = static_cast<uint32>(Settings.maximum_frame_rate);
        const UnrealImageCapture* ImageCapture = getImageCapture();
        if (ImageCapture == nullptr)
            throw std::runtime_error("DjiHil OpenFlyAndroid image capture is unavailable");
        if (APIPCamera* LiveCamera = getCamera(Settings.frame_camera_name);
            LiveCamera != nullptr && LiveCamera->GetRootComponent() != nullptr) {
            LiveFrameCameraBaseRotation = LiveCamera->GetRootComponent()->GetRelativeRotation();
            bHasLiveFrameCameraBaseRotation = true;
        }
        else {
            throw std::runtime_error("DjiHil OpenFlyAndroid frame camera was not found");
        }
        FrameServer = MakeUnique<openfly::hil::FOpenFlyHilFrameServer>(
            FrameConfig, *ImageCapture, *UdpServer);
        FString FrameStartError;
        if (!FrameServer->Start(FrameStartError))
            throw std::runtime_error(TCHAR_TO_UTF8(*FrameStartError));
        MonitorFrameWidth = FrameConfig.Width;
        MonitorFrameHeight = FrameConfig.Height;
        MonitorMaximumFrameRate = FrameConfig.MaximumFrameRate;
        MonitorFrameFormat = FrameConfig.Format == openfly::hil::EFrameFormat::Jpeg
            ? TEXT("JPEG") : TEXT("PNG");
        ActiveOpenFlyInstance = this;
        UE_LOG(LogTemp, Display,
            TEXT("DjiHil OpenFlyAndroid frame sender camera=%s %dx%d %s@%d; target comes from HELLO"),
            *FrameConfig.CameraName,
            Settings.frame_width, Settings.frame_height,
            FrameConfig.Format == openfly::hil::EFrameFormat::Jpeg ? TEXT("JPEG") : TEXT("PNG"),
            Settings.maximum_frame_rate);
    }
}

bool DjiHilPawnSimApi::GetActiveOpenFlyMonitorSnapshot(
    FOpenFlyHilMonitorSnapshot& OutSnapshot)
{
    DjiHilPawnSimApi* Instance = ActiveOpenFlyInstance;
    if (Instance == nullptr || Instance->bIsReplayBackend)
        return false;

    OutSnapshot = FOpenFlyHilMonitorSnapshot{};
    OutSnapshot.bBackendActive = true;
    OutSnapshot.bUdpListening = Instance->UdpServer.IsValid();
    OutSnapshot.bFrameListening = Instance->FrameServer.IsValid();
    OutSnapshot.FrameWidth = Instance->MonitorFrameWidth;
    OutSnapshot.FrameHeight = Instance->MonitorFrameHeight;
    OutSnapshot.MaximumFrameRate = Instance->MonitorMaximumFrameRate;
    OutSnapshot.FrameFormat = Instance->MonitorFrameFormat;

    if (Instance->UdpServer.IsValid()) {
        const openfly::hil::FPeerSnapshot Peer = Instance->UdpServer->GetPeerSnapshot();
        const openfly::hil::FUdpServerStats Stats = Instance->UdpServer->GetStats();
        OutSnapshot.bPeerConnected = Peer.bHasPeer;
        OutSnapshot.PeerIp = Peer.PeerIp;
        OutSnapshot.SessionId = Peer.SessionId;
        OutSnapshot.LastPoseSequence = Peer.LastPoseSequence;
        OutSnapshot.LastValidPacketHostNs = Peer.LastValidPacketHostNs;
        OutSnapshot.AcceptedPoseCount = Stats.AcceptedPose;
        OutSnapshot.RejectedPacketCount = Stats.RejectedPackets;
        OutSnapshot.InvalidDatagramCount = Stats.InvalidDatagrams;
        OutSnapshot.LinkLossCount = Stats.LinkLossCount;
        OutSnapshot.SentEventCount = Stats.SentEvent;
        if (Peer.bHasPose) {
            OutSnapshot.EastM = Peer.LatestPose.EastM;
            OutSnapshot.NorthM = Peer.LatestPose.NorthM;
            OutSnapshot.UpM = Peer.LatestPose.UpM;
            OutSnapshot.RollDeg = Peer.LatestPose.RollDeg;
            OutSnapshot.PitchDeg = Peer.LatestPose.PitchDeg;
            OutSnapshot.HeadingDeg = Peer.LatestPose.HeadingDeg;
            OutSnapshot.GimbalPitchDeg = Peer.LatestPose.GimbalPitchDeg;
            OutSnapshot.MeasuredSimulatorStateHz = Peer.LatestPose.MeasuredSimulatorStateHz;
            OutSnapshot.FlightControllerStateAgeMs = Peer.LatestPose.FlightControllerStateAgeMs;
            OutSnapshot.StateFlags = Peer.LatestPose.StateFlags;
            OutSnapshot.CommandForwardMps = Peer.LatestPose.CommandForwardMps;
            OutSnapshot.CommandRightMps = Peer.LatestPose.CommandRightMps;
            OutSnapshot.CommandUpMps = Peer.LatestPose.CommandUpMps;
            OutSnapshot.CommandYawRateDegPerSec = Peer.LatestPose.CommandYawRateDegPerSec;
        }
    }
    if (Instance->FrameServer.IsValid()) {
        const openfly::hil::FFrameServerStats Stats = Instance->FrameServer->GetStats();
        OutSnapshot.bFrameClientConnected = Stats.bClientConnected;
        OutSnapshot.CompletedFrameCount = Stats.CompletedFrames;
        OutSnapshot.BytesSent = Stats.BytesSent;
        OutSnapshot.CaptureFailureCount = Stats.CaptureFailures;
    }
    return true;
}

void DjiHilPawnSimApi::resetImplementation()
{
    PawnSimApi::resetImplementation();
    if (ActiveProvider != nullptr)
        ActiveProvider->Reset();
    if (bIsReplayBackend)
        ReplayStartClockNs = clock()->nowNanos();
    LastAppliedFrameId = MAX_uint64;
    LastAppliedSessionId = MAX_uint64;
    {
        FScopeLock Lock(&RenderStateMutex);
        bHasRenderState = false;
    }
    Sensors.reset();
    if (VehicleApi != nullptr)
        VehicleApi->reset();

    djihil::State FirstState;
    if (ActiveProvider != nullptr && ActiveProvider->GetLatestState(FirstState)) {
        ApplySourceState(FirstState);
        FScopeLock Lock(&RenderStateMutex);
        RenderState = FirstState;
        bHasRenderState = true;
    }
}

void DjiHilPawnSimApi::update()
{
    if (bIsReplayBackend) {
        const uint64 TargetSourceTimeNs = GetReplayTargetSourceTime();
        ReplayProvider.AdvanceToSourceTime(TargetSourceTimeNs);
    }

    djihil::State LatestState;
    if (ActiveProvider != nullptr && ActiveProvider->GetLatestState(LatestState)
        && (LatestState.session_id != LastAppliedSessionId
            || LatestState.frame_id != LastAppliedFrameId))
        ApplySourceState(LatestState);

    PawnSimApi::update();
    Sensors.update();
    if (VehicleApi != nullptr)
        VehicleApi->update();
}

void DjiHilPawnSimApi::updateRenderedState(float Dt)
{
    unused(Dt);
    djihil::State NewRenderState;
    bool bNewRenderState = false;
    if (bIsReplayBackend) {
        const uint64 TargetSourceTimeNs = GetReplayTargetSourceTime();
        const uint64 RenderSourceTimeNs = TargetSourceTimeNs > InterpolationDelayNs
            ? TargetSourceTimeNs - InterpolationDelayNs : ReplayFirstSourceTimeNs;
        bNewRenderState = ReplayProvider.SampleRenderState(RenderSourceTimeNs, NewRenderState);
    }
    else if (ActiveProvider != nullptr) {
        bNewRenderState = ActiveProvider->GetLatestState(NewRenderState);
    }
    FScopeLock Lock(&RenderStateMutex);
    RenderState = NewRenderState;
    bHasRenderState = bNewRenderState;
}

void DjiHilPawnSimApi::updateRendering(float Dt)
{
    unused(Dt);
    djihil::State StateToRender;
    bool bCanRender = false;
    {
        FScopeLock Lock(&RenderStateMutex);
        StateToRender = RenderState;
        bCanRender = bHasRenderState;
    }
    UpdateRotorAnimation();
    if (bCanRender
        && StateToRender.has(djihil::ValidField::Position)
        && StateToRender.has(djihil::ValidField::Orientation)) {
        if (!bIsReplayBackend)
            ObserveAuthoritativePoseCollision(StateToRender.kinematics.pose);
        setPoseInternal(StateToRender.kinematics.pose, true);
        if (!bIsReplayBackend && bHasLiveFrameCameraBaseRotation) {
            openfly::hil::FLivePoseMetadata Metadata;
            if (LiveProvider.GetLatestMetadata(Metadata)
                && Metadata.SessionId == StateToRender.session_id
                && Metadata.PoseSequence == StateToRender.frame_id) {
                if (APIPCamera* LiveCamera = getCamera(
                        std::string(TCHAR_TO_UTF8(*LiveFrameCameraName)));
                    LiveCamera != nullptr && LiveCamera->GetRootComponent() != nullptr) {
                    FRotator GimbalRotation = LiveFrameCameraBaseRotation;
                    GimbalRotation.Pitch += static_cast<float>(Metadata.GimbalPitchDeg);
                    LiveCamera->GetRootComponent()->SetRelativeRotation(GimbalRotation);
                }
            }
        }
    }
}


void DjiHilPawnSimApi::UpdateRotorAnimation()
{
    if (PawnEvents == nullptr || RotorAnimation.size() != 4)
        return;

    openfly::hil::FLivePoseMetadata Metadata;
    const bool bMotorStarted = !bIsReplayBackend
        && LiveProvider.GetLatestMetadata(Metadata)
        && Metadata.bFresh
        && (Metadata.StateFlags & 1u) != 0;
    const bool bInTheAir = bMotorStarted && (Metadata.StateFlags & 2u) != 0;
    const double CommandMagnitude = bMotorStarted
        ? FMath::Clamp(
            FMath::Abs(Metadata.CommandForwardMps)
                + FMath::Abs(Metadata.CommandRightMps)
                + FMath::Abs(Metadata.CommandUpMps)
                + FMath::Abs(Metadata.CommandYawRateDegPerSec) / 90.0,
            0.0, 2.0)
        : 0.0;
    const msr::airlib::real_T RotorSpeed = bMotorStarted
        ? static_cast<msr::airlib::real_T>((bInTheAir ? 520.0 : 320.0)
            + CommandMagnitude * 80.0)
        : 0.0f;
    for (MultirotorPawnEvents::RotorActuatorInfo& Rotor : RotorAnimation)
        Rotor.rotor_speed = RotorSpeed;
    PawnEvents->getActuatorSignal().emit(RotorAnimation);

    if (bMotorStarted != bLastRotorMotorStarted) {
        UE_LOG(LogTemp, Display, TEXT("DjiHil propeller animation %s"),
            bMotorStarted ? TEXT("started") : TEXT("stopped"));
        bLastRotorMotorStarted = bMotorStarted;
    }
}

void DjiHilPawnSimApi::setPose(const Pose& PoseValue, bool IgnoreCollision)
{
    unused(PoseValue);
    unused(IgnoreCollision);
    throw msr::airlib::VehicleApiBase::VehicleCommandNotImplementedException(
        "DjiHil pose is externally owned; simSetVehiclePose is disabled");
}

void DjiHilPawnSimApi::setKinematics(const Kinematics::State& State, bool IgnoreCollision)
{
    unused(State);
    unused(IgnoreCollision);
    throw msr::airlib::VehicleApiBase::VehicleCommandNotImplementedException(
        "DjiHil kinematics are externally owned; simSetKinematics is disabled");
}

void DjiHilPawnSimApi::reportState(StateReporter& Reporter)
{
    PawnSimApi::reportState(Reporter);
    Sensors.reportState(Reporter);
}

VehicleApiBase* DjiHilPawnSimApi::getVehicleApiBase() const
{
    return VehicleApi.get();
}

void DjiHilPawnSimApi::pawnTick(float Dt)
{
    unused(Dt);
    // PhysicsWorld owns update(); SimModeWorldBase owns render-state transfer.
}

void DjiHilPawnSimApi::onCollisionObserved(const CollisionInfo& Collision)
{
    const std::string Reason = "collision object_id=" + std::to_string(Collision.object_id);
    QueueSafetyEvent(static_cast<uint32>(openfly::hil::EEventKind::Collision),
        std::numeric_limits<double>::quiet_NaN(), Reason);
}

bool DjiHilPawnSimApi::QueueSafetyEvent(uint32 Kind, double Score,
    const std::string& Reason)
{
    if (bIsReplayBackend || !UdpServer.IsValid()
        || Kind > static_cast<uint32>(openfly::hil::EEventKind::Emergency))
        return false;
    const openfly::hil::EEventKind EventKind =
        static_cast<openfly::hil::EEventKind>(Kind);
    if ((EventKind == openfly::hil::EEventKind::Stop && !FMath::IsFinite(Score))
        || (EventKind != openfly::hil::EEventKind::Stop && FMath::IsFinite(Score)))
        return false;
    const FString EventReason = UTF8_TO_TCHAR(Reason.c_str());
    if (FTCHARToUTF8(*EventReason).Length() > openfly::hil::MaximumEventReasonBytes)
        return false;
    openfly::hil::FLivePoseMetadata Metadata;
    if (!LiveProvider.GetLatestMetadata(Metadata) || !Metadata.bFresh
        || Metadata.PoseSequence == 0)
        return false;
    openfly::hil::FEventPayload Event;
    Event.PeerMonotonicNs = static_cast<uint64>(FPlatformTime::Seconds() * 1000000000.0);
    Event.Kind = EventKind;
    Event.StopScore = Score;
    Event.PoseSequence = Metadata.PoseSequence;
    Event.Reason = EventReason;
    return UdpServer->QueueEvent(Event);
}

void DjiHilPawnSimApi::ObserveAuthoritativePoseCollision(const Pose& TargetPose)
{
    APawn* Pawn = getPawn();
    UWorld* World = Pawn != nullptr ? Pawn->GetWorld() : nullptr;
    if (World == nullptr)
        return;

    const FVector Start = getUUPosition();
    const FVector End = getNedTransform().fromLocalNed(TargetPose.position);
    const FQuat Orientation = getNedTransform().fromNed(TargetPose.orientation);
    const FBox Bounds = Pawn->GetComponentsBoundingBox(true);
    FVector Extent = Bounds.IsValid ? Bounds.GetExtent() : FVector(25.0f);
    // Exclude degenerate shapes while keeping this observation independent of
    // the authoritative teleport. The sweep result never changes End.
    Extent.X = FMath::Max(Extent.X, 1.0f);
    Extent.Y = FMath::Max(Extent.Y, 1.0f);
    Extent.Z = FMath::Max(Extent.Z, 1.0f);
    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(OpenFlyHilCollision), false, Pawn);
    QueryParams.bFindInitialOverlaps = false;
    FHitResult Hit;
    const bool bHit = World->SweepSingleByChannel(Hit, Start, End, Orientation,
        ECC_Pawn, FCollisionShape::MakeBox(Extent), QueryParams);
    if (!bHit || Hit.GetActor() == nullptr) {
        LastObservedCollisionActor.Reset();
        return;
    }

    const uint64 NowNs = static_cast<uint64>(FPlatformTime::Seconds() * 1000000000.0);
    static constexpr uint64 DuplicateSuppressionNs = 500000000ull;
    if (LastObservedCollisionActor.Get() == Hit.GetActor()
        && NowNs - LastObservedCollisionHostNs < DuplicateSuppressionNs)
        return;
    LastObservedCollisionActor = Hit.GetActor();
    LastObservedCollisionHostNs = NowNs;
    FString Reason = FString::Printf(TEXT("collision actor=%s"),
        *Hit.GetActor()->GetName());
    while (FTCHARToUTF8(*Reason).Length() > openfly::hil::MaximumEventReasonBytes)
        Reason.LeftChopInline(1, EAllowShrinking::No);
    QueueSafetyEvent(static_cast<uint32>(openfly::hil::EEventKind::Collision),
        std::numeric_limits<double>::quiet_NaN(),
        std::string(TCHAR_TO_UTF8(*Reason)));
}

std::string DjiHilPawnSimApi::getImageCaptureMetadata() const
{
    djihil::State LatestState;
    const bool bHasLatestState = ActiveProvider != nullptr
        && ActiveProvider->GetLatestState(LatestState);
    openfly::hil::FLivePoseMetadata LiveMetadata;
    const bool bHasLiveMetadata = !bIsReplayBackend
        && LiveProvider.GetLatestMetadata(LiveMetadata);
    djihil::State CapturedRenderState;
    bool bCapturedRenderState = false;
    {
        FScopeLock Lock(&RenderStateMutex);
        CapturedRenderState = RenderState;
        bCapturedRenderState = bHasRenderState;
    }
    std::ostringstream Stream;
    Stream << "{\"schema\":\"djihil.capture.v1\""
           << ",\"backend\":\"" << (ActiveProvider != nullptr
               ? TCHAR_TO_UTF8(ActiveProvider->GetBackendName()) : "unavailable") << "\""
           << ",\"capture_air_sim_time_ns\":" << clock()->nowNanos()
           << ",\"session_id\":" << (bCapturedRenderState ? CapturedRenderState.session_id : 0)
           << ",\"render_frame_id\":" << (bCapturedRenderState ? CapturedRenderState.frame_id : 0)
           << ",\"render_source_time_ns\":" << (bCapturedRenderState ? CapturedRenderState.source_time_ns : 0)
           << ",\"render_host_monotonic_ns\":" << (bCapturedRenderState ? CapturedRenderState.host_monotonic_ns : 0)
           << ",\"raw_frame_id\":" << (bHasLatestState ? LatestState.frame_id : 0)
           << ",\"raw_source_time_ns\":" << (bHasLatestState ? LatestState.source_time_ns : 0)
           << ",\"raw_host_monotonic_ns\":" << (bHasLatestState ? LatestState.host_monotonic_ns : 0)
           << ",\"gimbal_pitch_deg\":" << (bHasLiveMetadata ? LiveMetadata.GimbalPitchDeg : 0.0)
           << ",\"state_flags\":" << (bHasLiveMetadata ? LiveMetadata.StateFlags : 0)
           << ",\"motor_started\":" << (bHasLiveMetadata && (LiveMetadata.StateFlags & 1u) != 0 ? "true" : "false")
           << ",\"in_the_air\":" << (bHasLiveMetadata && (LiveMetadata.StateFlags & 2u) != 0 ? "true" : "false")
           << ",\"command_forward_mps\":" << (bHasLiveMetadata ? LiveMetadata.CommandForwardMps : 0.0)
           << ",\"command_right_mps\":" << (bHasLiveMetadata ? LiveMetadata.CommandRightMps : 0.0)
           << ",\"command_up_mps\":" << (bHasLiveMetadata ? LiveMetadata.CommandUpMps : 0.0)
           << ",\"command_yaw_rate_deg_s\":" << (bHasLiveMetadata ? LiveMetadata.CommandYawRateDegPerSec : 0.0)
           ;
    if (UdpServer.IsValid()) {
        const openfly::hil::FUdpServerStats UdpStats = UdpServer->GetStats();
        const openfly::hil::FPeerSnapshot Peer = UdpServer->GetPeerSnapshot();
        Stream << ",\"udp_received_datagrams\":" << UdpStats.ReceivedDatagrams
               << ",\"udp_invalid_datagrams\":" << UdpStats.InvalidDatagrams
               << ",\"udp_accepted_pose\":" << UdpStats.AcceptedPose
               << ",\"udp_rejected_packets\":" << UdpStats.RejectedPackets
               << ",\"udp_sent_heartbeat\":" << UdpStats.SentHeartbeat
               << ",\"udp_sent_pong\":" << UdpStats.SentPong
               << ",\"udp_sent_event\":" << UdpStats.SentEvent
               << ",\"udp_send_failures\":" << UdpStats.SendFailures
               << ",\"udp_link_loss_count\":" << UdpStats.LinkLossCount
               << ",\"peer_fresh\":" << (Peer.bHasPeer ? "true" : "false")
               << ",\"peer_accepted_pose_count\":" << Peer.AcceptedPoseCount;
    }
    if (FrameServer.IsValid()) {
        const openfly::hil::FFrameServerStats FrameStats = FrameServer->GetStats();
        Stream << ",\"tcp_accepted_connections\":" << FrameStats.AcceptedConnections
               << ",\"tcp_rejected_connections\":" << FrameStats.RejectedConnections
               << ",\"tcp_completed_frames\":" << FrameStats.CompletedFrames
               << ",\"tcp_capture_failures\":" << FrameStats.CaptureFailures
               << ",\"tcp_encode_failures\":" << FrameStats.EncodeFailures
               << ",\"tcp_send_failures\":" << FrameStats.SendFailures
               << ",\"tcp_bytes_sent\":" << FrameStats.BytesSent
               << ",\"tcp_last_frame_id\":" << FrameStats.LastFrameId
               << ",\"tcp_last_pose_sequence\":" << FrameStats.LastPoseSequence
               << ",\"tcp_capture_samples\":" << FrameStats.CaptureSamples
               << ",\"tcp_capture_total_us\":" << FrameStats.CaptureTotalMicros
               << ",\"tcp_capture_max_us\":" << FrameStats.CaptureMaximumMicros
               << ",\"tcp_encode_samples\":" << FrameStats.EncodeSamples
               << ",\"tcp_encode_total_us\":" << FrameStats.EncodeTotalMicros
               << ",\"tcp_encode_max_us\":" << FrameStats.EncodeMaximumMicros
               << ",\"tcp_send_samples\":" << FrameStats.SendSamples
               << ",\"tcp_send_total_us\":" << FrameStats.SendTotalMicros
               << ",\"tcp_send_max_us\":" << FrameStats.SendMaximumMicros;
    }
    Stream << "}";
    return Stream.str();
}

uint64 DjiHilPawnSimApi::GetReplayTargetSourceTime() const
{
    const uint64 NowNs = clock()->nowNanos();
    const uint64 ElapsedNs = NowNs >= ReplayStartClockNs ? NowNs - ReplayStartClockNs : 0;
    const uint64 DurationNs = ReplayLastSourceTimeNs >= ReplayFirstSourceTimeNs
        ? ReplayLastSourceTimeNs - ReplayFirstSourceTimeNs : 0;
    return ReplayFirstSourceTimeNs + FMath::Min(ElapsedNs, DurationNs);
}

void DjiHilPawnSimApi::ApplySourceState(const djihil::State& State)
{
    Kinematics::State GroundTruth = getKinematics()->getState();
    if (State.has(djihil::ValidField::Position))
        GroundTruth.pose.position = State.kinematics.pose.position;
    if (State.has(djihil::ValidField::Orientation))
        GroundTruth.pose.orientation = State.kinematics.pose.orientation;
    if (State.has(djihil::ValidField::LinearVelocity))
        GroundTruth.twist.linear = State.kinematics.twist.linear;
    if (State.has(djihil::ValidField::AngularVelocity))
        GroundTruth.twist.angular = State.kinematics.twist.angular;
    if (State.has(djihil::ValidField::LinearAcceleration))
        GroundTruth.accelerations.linear = State.kinematics.accelerations.linear;
    if (State.has(djihil::ValidField::AngularAcceleration))
        GroundTruth.accelerations.angular = State.kinematics.accelerations.angular;
    getKinematics()->setState(GroundTruth);
    LastAppliedSessionId = State.session_id;
    LastAppliedFrameId = State.frame_id;
}

FString DjiHilPawnSimApi::ResolveReplayPath(const std::string& ConfiguredPath) const
{
    FString Result = UTF8_TO_TCHAR(ConfiguredPath.c_str());
    if (FPaths::IsRelative(Result))
        Result = FPaths::Combine(FPaths::ProjectDir(), Result);
    return FPaths::ConvertRelativePathToFull(Result);
}
