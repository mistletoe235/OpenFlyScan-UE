#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Paged/NanoGSCaptureReadiness.h"
#include "RenderRequest.h"

namespace
{
	NanoGS::Paged::FCaptureReadinessSnapshot MakeReadySnapshot(const uint32 Generation)
	{
		using namespace NanoGS::Paged;
		FCaptureSourceSnapshot Source;
		Source.SourceIdentity = 17;
		Source.StreamingState = ECaptureStreamingState::Ready;
		Source.ResidentGeneration = Generation;
		Source.RequestedPageCount = 2;
		Source.PublishedPageCount = 2;
		Source.ActiveSplatCount = 128;
		Source.bBounded = true;
		Source.bUploadComplete = true;
		Source.bStableAtomicSample = true;

		FCaptureReadinessSnapshot Snapshot;
		Snapshot.bQueryAvailable = true;
		Snapshot.Sources.Add(Source);
		return Snapshot;
	}

	void SeedResult(RenderRequest::RenderResult& Result, const int32 Width)
	{
		Result.width = Width;
		Result.height = 480;
		Result.time_stamp = 123;
		Result.bmp.Add(FColor::Red);
		Result.bmp_float.Add(FFloat16Color(FLinearColor::Red));
		Result.image_data_uint8.Add(1);
		Result.image_data_float.Add(1.0f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRenderRequestCaptureReadinessHelpersTest,
	"AirSim.Capture.Readiness.FailClosedHelpers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderRequestCaptureReadinessHelpersTest::RunTest(const FString& Parameters)
{
	std::vector<std::shared_ptr<RenderRequest::RenderResult>> Results;
	Results.push_back(std::make_shared<RenderRequest::RenderResult>());
	RenderRequest::RenderResult& Result = *Results[0];
	SeedResult(Result, 640);

	const FString Diagnostic = TEXT("nanogs_not_ready reason=unsafe_path_disabled");
	RenderRequest::InvalidateCaptureResults(Results, 1, Diagnostic);
	TestEqual(TEXT("Invalid capture width is zero"), Result.width, 0);
	TestEqual(TEXT("Invalid capture height is zero"), Result.height, 0);
	TestEqual(TEXT("Invalid capture timestamp is zero"), Result.time_stamp,
		static_cast<msr::airlib::TTimePoint>(0));
	TestTrue(TEXT("Invalid uint8 pixels are empty"), Result.image_data_uint8.IsEmpty());
	TestTrue(TEXT("Invalid float pixels are empty"), Result.image_data_float.IsEmpty());
	TestTrue(TEXT("Invalid color readback is empty"), Result.bmp.IsEmpty());
	TestTrue(TEXT("Invalid float readback is empty"), Result.bmp_float.IsEmpty());
	TestFalse(TEXT("Invalid capture is marked invalid"), Result.capture_is_valid);
	TestEqual(TEXT("Invalid capture preserves machine diagnostic"),
		Result.capture_diagnostic, Diagnostic);

	TestTrue(TEXT("Known Page sources disable unsafe capture"),
		RenderRequest::ShouldDisableUnsafePath(true, 1));
	TestFalse(TEXT("Known no-Page scene preserves legacy unsafe capture"),
		RenderRequest::ShouldDisableUnsafePath(true, 0));
	TestTrue(TEXT("Unavailable query disables unsafe capture fail-closed"),
		RenderRequest::ShouldDisableUnsafePath(false, 1));
	TestTrue(TEXT("Unavailable empty query also disables unsafe capture fail-closed"),
		RenderRequest::ShouldDisableUnsafePath(false, 0));
	TestTrue(TEXT("A successful exact-size readback is accepted"),
		RenderRequest::IsReadbackComplete(true, 640 * 480, 640 * 480));
	TestFalse(TEXT("A failed readback is rejected even if the array size matches"),
		RenderRequest::IsReadbackComplete(false, 640 * 480, 640 * 480));
	TestFalse(TEXT("A short readback is rejected before conversion"),
		RenderRequest::IsReadbackComplete(true, 640 * 480, 640 * 480 - 1));
	TestFalse(TEXT("An empty target is rejected"),
		RenderRequest::IsReadbackComplete(true, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRenderRequestPerCaptureReadinessIsolationTest,
	"AirSim.Capture.Readiness.PerCaptureIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderRequestPerCaptureReadinessIsolationTest::RunTest(const FString& Parameters)
{
	using namespace NanoGS::Paged;
	RenderRequest::RenderResult FirstCamera;
	RenderRequest::RenderResult SecondCamera;
	SeedResult(FirstCamera, 640);
	SeedResult(SecondCamera, 800);

	const FCaptureReadinessSnapshot Generation10 = MakeReadySnapshot(10);
	const FCaptureReadinessSnapshot Generation11 = MakeReadySnapshot(11);
	const FCaptureReadinessValidation FirstValidation =
		ValidateCaptureSnapshots(Generation10, Generation11);
	const FCaptureReadinessValidation SecondValidation =
		ValidateCaptureSnapshots(Generation11, Generation11);

	RenderRequest::ApplyCaptureValidation(FirstCamera, FirstValidation);
	RenderRequest::ApplyCaptureValidation(SecondCamera, SecondValidation);

	TestFalse(TEXT("A camera whose publication changed is rejected"),
		FirstCamera.capture_is_valid);
	TestEqual(TEXT("Rejected camera pixels are cleared"), FirstCamera.width, 0);
	TestTrue(TEXT("Rejected camera retains a machine-readable reason"),
		FirstCamera.capture_diagnostic.Contains(TEXT("reason=publication_changed")));

	TestTrue(TEXT("A sibling camera with a stable bracket remains valid"),
		SecondCamera.capture_is_valid);
	TestEqual(TEXT("Stable sibling dimensions remain intact"), SecondCamera.width, 800);
	TestFalse(TEXT("Stable sibling readback remains intact"), SecondCamera.bmp.IsEmpty());
	TestTrue(TEXT("Stable sibling diagnostic remains empty"),
		SecondCamera.capture_diagnostic.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
