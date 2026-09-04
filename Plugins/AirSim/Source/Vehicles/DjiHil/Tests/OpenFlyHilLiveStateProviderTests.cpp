#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Vehicles/DjiHil/OpenFlyHilLiveStateProvider.h"
#include "common/VectorMath.hpp"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenFlyHilLiveStateProviderTest,
    "AirSim.DjiHil.OpenFly.LiveStateProvider",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOpenFlyHilLiveStateProviderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace openfly::hil;
    using namespace msr::airlib;

    FOpenFlyHilLiveStateProvider Provider;
    FPosePayload Pose;
    Pose.SampleMonotonicNs = 123456789;
    Pose.EastM = 10.0;
    Pose.NorthM = 20.0;
    Pose.UpM = 3.0;
    Pose.HeadingDeg = 90.0;
    Pose.VelocityNorthMps = 1.0;
    Pose.VelocityEastMps = 2.0;
    Pose.VelocityUpMps = 0.5;
    Pose.GimbalPitchDeg = -30.0;
    Pose.FlightControllerStateAgeMs = 7;
    Pose.MeasuredSimulatorStateHz = 99.5f;
    Pose.StateFlags = 7;
    Pose.CommandForwardMps = 1.25;
    Pose.CommandRightMps = -0.5;
    Pose.CommandUpMps = 0.75;
    Pose.CommandYawRateDegPerSec = 12.0;

    TestTrue(TEXT("Publish first live pose"), Provider.PublishPose(8, 10, Pose, 200000000));
    djihil::State State;
    TestTrue(TEXT("Fresh provider returns state"), Provider.GetLatestState(State));
    TestTrue(TEXT("North maps to AirSim X"), FMath::IsNearlyEqual(State.kinematics.pose.position.x(), 20.0f));
    TestTrue(TEXT("East maps to AirSim Y"), FMath::IsNearlyEqual(State.kinematics.pose.position.y(), 10.0f));
    TestTrue(TEXT("Up maps to negative AirSim Z"), FMath::IsNearlyEqual(State.kinematics.pose.position.z(), -3.0f));
    TestTrue(TEXT("Velocity north maps X"), FMath::IsNearlyEqual(State.kinematics.twist.linear.x(), 1.0f));
    TestTrue(TEXT("Velocity east maps Y"), FMath::IsNearlyEqual(State.kinematics.twist.linear.y(), 2.0f));
    TestTrue(TEXT("Velocity up maps negative Z"), FMath::IsNearlyEqual(State.kinematics.twist.linear.z(), -0.5f));
    TestEqual(TEXT("Flying flag maps to Flying"), State.landed_state, LandedState::Flying);

    real_T Pitch = 0;
    real_T Roll = 0;
    real_T Yaw = 0;
    VectorMath::toEulerianAngle(State.kinematics.pose.orientation, Pitch, Roll, Yaw);
    TestTrue(TEXT("Heading 90 maps to NED yaw +90"),
        FMath::IsNearlyEqual(FMath::RadiansToDegrees(Yaw), 90.0f, 1.0e-3f));

    FLivePoseMetadata Metadata;
    TestTrue(TEXT("Read live metadata"), Provider.GetLatestMetadata(Metadata));
    TestEqual(TEXT("Metadata pose sequence"), Metadata.PoseSequence, uint64(10));
    TestEqual(TEXT("Metadata state flags"), Metadata.StateFlags, uint32(7));
    TestTrue(TEXT("Metadata command forward"), FMath::IsNearlyEqual(Metadata.CommandForwardMps, 1.25));
    TestTrue(TEXT("Metadata command right"), FMath::IsNearlyEqual(Metadata.CommandRightMps, -0.5));
    TestTrue(TEXT("Metadata command up"), FMath::IsNearlyEqual(Metadata.CommandUpMps, 0.75));
    TestTrue(TEXT("Metadata yaw rate"), FMath::IsNearlyEqual(Metadata.CommandYawRateDegPerSec, 12.0));
    TestTrue(TEXT("Metadata starts fresh"), Metadata.bFresh);
    TestFalse(TEXT("Reject duplicate pose"), Provider.PublishPose(8, 10, Pose, 210000000));

    Provider.MarkLinkLost(7);
    TestTrue(TEXT("Wrong session cannot stale state"), Provider.GetLatestState(State));
    Provider.MarkLinkLost(8);
    TestFalse(TEXT("Matching link loss makes state unavailable"), Provider.GetLatestState(State));
    TestTrue(TEXT("Metadata remains observable after loss"), Provider.GetLatestMetadata(Metadata));
    TestFalse(TEXT("Metadata reports stale"), Metadata.bFresh);

    Pose.StateFlags = 0;
    TestTrue(TEXT("New session can publish after loss"), Provider.PublishPose(9, 1, Pose, 300000000));
    TestTrue(TEXT("New session state available"), Provider.GetLatestState(State));
    TestEqual(TEXT("No flying flag maps Landed"), State.landed_state, LandedState::Landed);
    return true;
}

#endif
