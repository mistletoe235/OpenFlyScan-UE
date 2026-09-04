#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Vehicles/DjiHil/DjiHilStateProvider.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDjiHilReplayProviderTest,
    "AirSim.DjiHil.ReplayProvider",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDjiHilReplayProviderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString ReplayPath = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Tests/DjiHil/replay_provider_test.jsonl"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReplayPath), true);

    const FString Replay =
        TEXT("{\"protocol_version\":1,\"session_id\":\"7\",\"frame_id\":\"10\",\"source_time_ns\":\"1000000000\",\"host_monotonic_ns\":\"5000000000\",\"receive_sequence\":\"1\",\"valid_fields\":\"3\",\"position_ned_m\":[0,0,-2],\"orientation_wxyz\":[1,0,0,0]}\n")
        TEXT("{\"protocol_version\":1,\"session_id\":\"7\",\"frame_id\":\"11\",\"source_time_ns\":\"2000000000\",\"host_monotonic_ns\":\"6000000000\",\"receive_sequence\":\"2\",\"valid_fields\":\"3\",\"position_ned_m\":[10,4,-6],\"orientation_wxyz\":[0.70710678,0,0,0.70710678]}\n");
    if (!FFileHelper::SaveStringToFile(Replay, *ReplayPath)) {
        AddError(FString::Printf(TEXT("Failed to create replay fixture: %s"), *ReplayPath));
        return false;
    }

    FDjiHilReplayStateProvider Provider;
    FString Error;
    TestTrue(TEXT("Load valid JSONL replay"), Provider.LoadJsonLines(ReplayPath, Error));
    TestTrue(TEXT("Valid replay has no error"), Error.IsEmpty());
    TestEqual(TEXT("Replay frame count"), Provider.GetFrameCount(), 2);
    uint64 FirstSourceTimeNs = 0;
    uint64 LastSourceTimeNs = 0;
    TestTrue(TEXT("Read replay source-time range"),
        Provider.GetSourceTimeRange(FirstSourceTimeNs, LastSourceTimeNs));
    TestEqual(TEXT("First replay source time"), FirstSourceTimeNs, 1000000000ull);
    TestEqual(TEXT("Last replay source time"), LastSourceTimeNs, 2000000000ull);

    msr::airlib::djihil::State Sample;
    TestTrue(TEXT("Sample midpoint"), Provider.SampleRenderState(1500000000ull, Sample));
    TestEqual(TEXT("Midpoint source time"), Sample.source_time_ns, 1500000000ull);
    TestTrue(TEXT("Midpoint X"), FMath::IsNearlyEqual(Sample.kinematics.pose.position.x(), 5.0f));
    TestTrue(TEXT("Midpoint Y"), FMath::IsNearlyEqual(Sample.kinematics.pose.position.y(), 2.0f));
    TestTrue(TEXT("Midpoint Z"), FMath::IsNearlyEqual(Sample.kinematics.pose.position.z(), -4.0f));
    TestTrue(TEXT("Interpolated quaternion remains normalized"),
        FMath::IsNearlyEqual(Sample.kinematics.pose.orientation.norm(), 1.0f, 1.0e-5f));

    TestTrue(TEXT("Advance to second source frame"), Provider.AdvanceToSourceTime(2000000000ull));
    msr::airlib::djihil::State Latest;
    TestTrue(TEXT("Read latest source frame"), Provider.GetLatestState(Latest));
    TestEqual(TEXT("Latest frame id"), Latest.frame_id, 11ull);

    const FString NonMonotonicReplay =
        TEXT("{\"protocol_version\":1,\"session_id\":\"8\",\"frame_id\":\"2\",\"source_time_ns\":\"200\",\"valid_fields\":\"3\",\"position_ned_m\":[0,0,0],\"orientation_wxyz\":[1,0,0,0]}\n")
        TEXT("{\"protocol_version\":1,\"session_id\":\"8\",\"frame_id\":\"1\",\"source_time_ns\":\"100\",\"valid_fields\":\"3\",\"position_ned_m\":[0,0,0],\"orientation_wxyz\":[1,0,0,0]}\n");
    TestTrue(TEXT("Write non-monotonic replay fixture"),
        FFileHelper::SaveStringToFile(NonMonotonicReplay, *ReplayPath));
    FDjiHilReplayStateProvider InvalidOrderProvider;
    Error.Reset();
    TestFalse(TEXT("Reject non-monotonic frame/time"),
        InvalidOrderProvider.LoadJsonLines(ReplayPath, Error));
    TestTrue(TEXT("Non-monotonic replay reports an error"), !Error.IsEmpty());

    const FString InvalidQuaternionReplay =
        TEXT("{\"protocol_version\":1,\"session_id\":\"9\",\"frame_id\":\"1\",\"source_time_ns\":\"100\",\"valid_fields\":\"3\",\"position_ned_m\":[0,0,0],\"orientation_wxyz\":[0,0,0,0]}\n");
    TestTrue(TEXT("Write invalid quaternion replay fixture"),
        FFileHelper::SaveStringToFile(InvalidQuaternionReplay, *ReplayPath));
    FDjiHilReplayStateProvider InvalidQuaternionProvider;
    Error.Reset();
    TestFalse(TEXT("Reject zero-length quaternion"),
        InvalidQuaternionProvider.LoadJsonLines(ReplayPath, Error));
    TestTrue(TEXT("Invalid quaternion reports an error"), !Error.IsEmpty());

    const FString InvalidGpsReplay =
        TEXT("{\"protocol_version\":1,\"session_id\":\"10\",\"frame_id\":\"1\",\"source_time_ns\":\"100\",\"valid_fields\":\"67\",\"position_ned_m\":[0,0,0],\"orientation_wxyz\":[1,0,0,0],\"gps_lla\":[91,181,0]}\n");
    TestTrue(TEXT("Write invalid GPS replay fixture"),
        FFileHelper::SaveStringToFile(InvalidGpsReplay, *ReplayPath));
    FDjiHilReplayStateProvider InvalidGpsProvider;
    Error.Reset();
    TestFalse(TEXT("Reject out-of-range GPS"),
        InvalidGpsProvider.LoadJsonLines(ReplayPath, Error));
    TestTrue(TEXT("Invalid GPS reports an error"), !Error.IsEmpty());

    const FString FractionalIdentifierReplay =
        TEXT("{\"protocol_version\":1,\"session_id\":\"10\",\"frame_id\":1.5,\"source_time_ns\":\"100\",\"valid_fields\":\"3\",\"position_ned_m\":[0,0,0],\"orientation_wxyz\":[1,0,0,0]}\n");
    TestTrue(TEXT("Write fractional identifier replay fixture"),
        FFileHelper::SaveStringToFile(FractionalIdentifierReplay, *ReplayPath));
    FDjiHilReplayStateProvider FractionalIdentifierProvider;
    Error.Reset();
    TestFalse(TEXT("Reject fractional frame identifier"),
        FractionalIdentifierProvider.LoadJsonLines(ReplayPath, Error));
    TestTrue(TEXT("Fractional identifier reports an error"), !Error.IsEmpty());

    const FString UnsupportedValidityReplay =
        TEXT("{\"protocol_version\":1,\"session_id\":\"10\",\"frame_id\":\"1\",\"source_time_ns\":\"100\",\"valid_fields\":\"259\",\"position_ned_m\":[0,0,0],\"orientation_wxyz\":[1,0,0,0]}\n");
    TestTrue(TEXT("Write unsupported validity replay fixture"),
        FFileHelper::SaveStringToFile(UnsupportedValidityReplay, *ReplayPath));
    FDjiHilReplayStateProvider UnsupportedValidityProvider;
    Error.Reset();
    TestFalse(TEXT("Reject unsupported RC validity bit"),
        UnsupportedValidityProvider.LoadJsonLines(ReplayPath, Error));
    TestTrue(TEXT("Unsupported validity reports an error"), !Error.IsEmpty());

    const FString MissingProtocolVersionReplay =
        TEXT("{\"session_id\":\"10\",\"frame_id\":\"1\",\"source_time_ns\":\"100\",\"valid_fields\":\"3\",\"position_ned_m\":[0,0,0],\"orientation_wxyz\":[1,0,0,0]}\n");
    TestTrue(TEXT("Write missing protocol version replay fixture"),
        FFileHelper::SaveStringToFile(MissingProtocolVersionReplay, *ReplayPath));
    FDjiHilReplayStateProvider MissingProtocolVersionProvider;
    Error.Reset();
    TestFalse(TEXT("Reject missing protocol version"),
        MissingProtocolVersionProvider.LoadJsonLines(ReplayPath, Error));
    TestTrue(TEXT("Missing protocol version reports an error"), !Error.IsEmpty());

    const FString MultipleSessionReplay =
        TEXT("{\"protocol_version\":1,\"session_id\":\"11\",\"frame_id\":\"1\",\"source_time_ns\":\"100\",\"valid_fields\":\"3\",\"position_ned_m\":[0,0,0],\"orientation_wxyz\":[1,0,0,0]}\n")
        TEXT("{\"protocol_version\":1,\"session_id\":\"12\",\"frame_id\":\"2\",\"source_time_ns\":\"200\",\"valid_fields\":\"3\",\"position_ned_m\":[0,0,0],\"orientation_wxyz\":[1,0,0,0]}\n");
    TestTrue(TEXT("Write multiple-session replay fixture"),
        FFileHelper::SaveStringToFile(MultipleSessionReplay, *ReplayPath));
    FDjiHilReplayStateProvider MultipleSessionProvider;
    Error.Reset();
    TestFalse(TEXT("Reject multiple sessions in one replay file"),
        MultipleSessionProvider.LoadJsonLines(ReplayPath, Error));
    TestTrue(TEXT("Multiple sessions report an error"), !Error.IsEmpty());

    IFileManager::Get().Delete(*ReplayPath, false, true);
    return true;
}

#endif
