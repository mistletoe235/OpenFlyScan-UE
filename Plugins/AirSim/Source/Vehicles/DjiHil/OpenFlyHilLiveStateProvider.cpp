#include "Vehicles/DjiHil/OpenFlyHilLiveStateProvider.h"

#include "Misc/ScopeLock.h"
#include "common/VectorMath.hpp"

namespace openfly::hil
{
bool FOpenFlyHilLiveStateProvider::PublishPose(uint64 SessionId, uint64 PoseSequence,
    const FPosePayload& Pose, uint64 ReceiveHostMonotonicNs)
{
    if (SessionId == 0 || PoseSequence == 0)
        return false;

    FScopeLock Lock(&Mutex);
    if (bHasState && Metadata.SessionId == SessionId
        && PoseSequence <= Metadata.PoseSequence)
        return false;

    msr::airlib::djihil::State State;
    State.protocol_version = ProtocolVersion;
    State.session_id = SessionId;
    State.frame_id = PoseSequence;
    State.source_time_ns = Pose.SampleMonotonicNs;
    State.host_monotonic_ns = ReceiveHostMonotonicNs;
    State.receive_sequence = PoseSequence;
    State.valid_fields =
        msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::Position)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::Orientation)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::LinearVelocity)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::LandedState);

    // Wire ENU -> AirSim NED. NedTransform performs NED metres -> UE centimetres later.
    State.kinematics.pose.position = msr::airlib::Vector3r(
        static_cast<msr::airlib::real_T>(Pose.NorthM),
        static_cast<msr::airlib::real_T>(Pose.EastM),
        static_cast<msr::airlib::real_T>(-Pose.UpM));
    State.kinematics.twist.linear = msr::airlib::Vector3r(
        static_cast<msr::airlib::real_T>(Pose.VelocityNorthMps),
        static_cast<msr::airlib::real_T>(Pose.VelocityEastMps),
        static_cast<msr::airlib::real_T>(-Pose.VelocityUpMps));

    const msr::airlib::real_T PitchRad = FMath::DegreesToRadians(
        static_cast<msr::airlib::real_T>(Pose.PitchDeg));
    const msr::airlib::real_T RollRad = FMath::DegreesToRadians(
        static_cast<msr::airlib::real_T>(Pose.RollDeg));
    const msr::airlib::real_T HeadingRad = FMath::DegreesToRadians(
        static_cast<msr::airlib::real_T>(FMath::Fmod(Pose.HeadingDeg + 360.0, 360.0)));
    State.kinematics.pose.orientation = msr::airlib::VectorMath::toQuaternion(
        PitchRad, RollRad, HeadingRad).normalized();
    State.landed_state = (Pose.StateFlags & (1u << 1u)) != 0
        ? msr::airlib::LandedState::Flying : msr::airlib::LandedState::Landed;

    LatestState = State;
    Metadata.SessionId = SessionId;
    Metadata.PoseSequence = PoseSequence;
    Metadata.SampleMonotonicNs = Pose.SampleMonotonicNs;
    Metadata.ReceiveHostMonotonicNs = ReceiveHostMonotonicNs;
    Metadata.FlightControllerStateAgeMs = Pose.FlightControllerStateAgeMs;
    Metadata.MeasuredSimulatorStateHz = Pose.MeasuredSimulatorStateHz;
    Metadata.StateFlags = Pose.StateFlags;
    Metadata.GimbalPitchDeg = Pose.GimbalPitchDeg;
    Metadata.CommandForwardMps = Pose.CommandForwardMps;
    Metadata.CommandRightMps = Pose.CommandRightMps;
    Metadata.CommandUpMps = Pose.CommandUpMps;
    Metadata.CommandYawRateDegPerSec = Pose.CommandYawRateDegPerSec;
    Metadata.bFresh = true;
    bHasState = true;
    return true;
}

void FOpenFlyHilLiveStateProvider::MarkLinkLost(uint64 SessionId)
{
    FScopeLock Lock(&Mutex);
    if (bHasState && (SessionId == 0 || Metadata.SessionId == SessionId))
        Metadata.bFresh = false;
}

void FOpenFlyHilLiveStateProvider::Reset()
{
    FScopeLock Lock(&Mutex);
    LatestState = msr::airlib::djihil::State{};
    Metadata = FLivePoseMetadata{};
    bHasState = false;
}

bool FOpenFlyHilLiveStateProvider::GetLatestState(
    msr::airlib::djihil::State& OutState) const
{
    FScopeLock Lock(&Mutex);
    if (!bHasState || !Metadata.bFresh)
        return false;
    OutState = LatestState;
    return true;
}

bool FOpenFlyHilLiveStateProvider::GetLatestMetadata(
    FLivePoseMetadata& OutMetadata) const
{
    FScopeLock Lock(&Mutex);
    if (!bHasState)
        return false;
    OutMetadata = Metadata;
    return true;
}
}
