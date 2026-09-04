#pragma once

#include "PawnSimApi.h"
#include "Vehicles/DjiHil/DjiHilMultirotorApi.h"
#include "Vehicles/DjiHil/DjiHilStateProvider.h"
#include "Vehicles/DjiHil/OpenFlyHilLiveStateProvider.h"
#include "Vehicles/DjiHil/OpenFlyHilFrameServer.h"
#include "Vehicles/DjiHil/OpenFlyHilUdpServer.h"
#include "Vehicles/Multirotor/MultirotorPawnEvents.h"
#include "HAL/CriticalSection.h"

struct FOpenFlyHilMonitorSnapshot
{
    bool bBackendActive = false;
    bool bUdpListening = false;
    bool bFrameListening = false;
    bool bPeerConnected = false;
    bool bFrameClientConnected = false;
    FString PeerIp;
    uint64 SessionId = 0;
    uint64 LastPoseSequence = 0;
    uint64 LastValidPacketHostNs = 0;
    uint64 AcceptedPoseCount = 0;
    uint64 RejectedPacketCount = 0;
    uint64 InvalidDatagramCount = 0;
    uint64 LinkLossCount = 0;
    uint64 SentEventCount = 0;
    uint64 CompletedFrameCount = 0;
    uint64 BytesSent = 0;
    uint64 CaptureFailureCount = 0;
    uint32 FrameWidth = 0;
    uint32 FrameHeight = 0;
    uint32 MaximumFrameRate = 0;
    FString FrameFormat;
    float MeasuredSimulatorStateHz = 0.0f;
    uint32 FlightControllerStateAgeMs = 0;
    uint32 StateFlags = 0;
    double CommandForwardMps = 0.0;
    double CommandRightMps = 0.0;
    double CommandUpMps = 0.0;
    double CommandYawRateDegPerSec = 0.0;
    double EastM = 0.0;
    double NorthM = 0.0;
    double UpM = 0.0;
    double RollDeg = 0.0;
    double PitchDeg = 0.0;
    double HeadingDeg = 0.0;
    double GimbalPitchDeg = 0.0;
};

class DjiHilPawnSimApi final : public PawnSimApi
{
public:
    explicit DjiHilPawnSimApi(const Params& Params);
    virtual ~DjiHilPawnSimApi() override;

    virtual void initialize() override;
    virtual void resetImplementation() override;
    virtual void update() override;
    virtual void updateRenderedState(float Dt) override;
    virtual void updateRendering(float Dt) override;
    virtual void setPose(const Pose& PoseValue, bool IgnoreCollision) override;
    virtual void setKinematics(const Kinematics::State& State, bool IgnoreCollision) override;
    virtual void reportState(msr::airlib::StateReporter& Reporter) override;
    virtual msr::airlib::VehicleApiBase* getVehicleApiBase() const override;

    static bool GetActiveOpenFlyMonitorSnapshot(FOpenFlyHilMonitorSnapshot& OutSnapshot);

protected:
    virtual void pawnTick(float Dt) override;
    virtual std::string getImageCaptureMetadata() const override;
    virtual void onCollisionObserved(const CollisionInfo& Collision) override;

private:
    uint64 GetReplayTargetSourceTime() const;
    void ApplySourceState(const msr::airlib::djihil::State& State);
    FString ResolveReplayPath(const std::string& ConfiguredPath) const;
    bool QueueSafetyEvent(uint32 Kind, double Score, const std::string& Reason);
    void ObserveAuthoritativePoseCollision(const Pose& TargetPose);
    void UpdateRotorAnimation();

    FDjiHilReplayStateProvider ReplayProvider;
    openfly::hil::FOpenFlyHilLiveStateProvider LiveProvider;
    IDjiHilStateProvider* ActiveProvider = nullptr;
    MultirotorPawnEvents* PawnEvents = nullptr;
    std::vector<MultirotorPawnEvents::RotorActuatorInfo> RotorAnimation;
    bool bLastRotorMotorStarted = false;
    TUniquePtr<openfly::hil::FOpenFlyHilUdpServer> UdpServer;
    TUniquePtr<openfly::hil::FOpenFlyHilFrameServer> FrameServer;
    msr::airlib::SensorCollection Sensors;
    std::vector<std::shared_ptr<msr::airlib::SensorBase>> SensorStorage;
    std::unique_ptr<DjiHilMultirotorApi> VehicleApi;
    msr::airlib::djihil::State RenderState;
    uint64 ReplayFirstSourceTimeNs = 0;
    uint64 ReplayLastSourceTimeNs = 0;
    uint64 ReplayStartClockNs = 0;
    uint64 InterpolationDelayNs = 0;
    uint64 LastAppliedFrameId = MAX_uint64;
    uint64 LastAppliedSessionId = MAX_uint64;
    bool bIsReplayBackend = true;
    bool bHasRenderState = false;
    FString LiveFrameCameraName;
    FRotator LiveFrameCameraBaseRotation = FRotator::ZeroRotator;
    bool bHasLiveFrameCameraBaseRotation = false;
    TWeakObjectPtr<AActor> LastObservedCollisionActor;
    uint64 LastObservedCollisionHostNs = 0;
    mutable FCriticalSection RenderStateMutex;
    uint32 MonitorFrameWidth = 0;
    uint32 MonitorFrameHeight = 0;
    uint32 MonitorMaximumFrameRate = 0;
    FString MonitorFrameFormat;

    static DjiHilPawnSimApi* ActiveOpenFlyInstance;
};
