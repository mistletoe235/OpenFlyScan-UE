#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Paged/NanoGSCaptureReadiness.h"
#include "SceneView.h"

namespace NanoGS::Paged::Tests
{
	namespace
	{
		FCaptureSourceSnapshot MakeBoundedReady(
			const uint64 Identity = 1,
			const uint32 Generation = 7,
			const uint32 PageCount = 3,
			const uint32 ActiveCount = 192)
		{
			FCaptureSourceSnapshot Source;
			Source.SourceIdentity = Identity;
			Source.StreamingState = ECaptureStreamingState::Ready;
			Source.ResidentGeneration = Generation;
			Source.RequestedPageCount = PageCount;
			Source.PublishedPageCount = PageCount;
			Source.ActiveSplatCount = ActiveCount;
			Source.bBounded = true;
			Source.bUploadComplete = true;
			Source.bStableAtomicSample = true;
			return Source;
		}

		FCaptureReadinessSnapshot MakeSnapshot(const FCaptureSourceSnapshot& Source)
		{
			FCaptureReadinessSnapshot Snapshot;
			Snapshot.bQueryAvailable = true;
			Snapshot.Sources.Add(Source);
			return Snapshot;
		}

		FCaptureViewSignature MakeViewSignature(const double X = 0.0)
		{
			FCaptureViewSignature Signature;
			Signature.ViewMatrix = FMatrix::Identity;
			Signature.ViewMatrix.M[3][0] = X;
			Signature.ProjectionNoAAMatrix = FMatrix::Identity;
			Signature.UnscaledViewRect = FIntRect(0, 0, 640, 480);
			Signature.SceneCaptureSource = 1;
			Signature.bIsSceneCapture = true;
			return Signature;
		}
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSCaptureReadinessReadyTest,
		"NanoGS.Paged.CaptureReadiness.ReadyAndEmptyFrontier",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSCaptureReadinessReadyTest::RunTest(const FString& Parameters)
	{
		FCaptureReadinessSnapshot NoSources;
		NoSources.bQueryAvailable = true;
		TestTrue(TEXT("A scene with no Page source bypasses the gate"),
			ValidateCaptureSnapshots(NoSources, NoSources).bAccepted);

		const FCaptureReadinessSnapshot Bounded = MakeSnapshot(MakeBoundedReady());
		TestTrue(TEXT("A stable bounded publication is accepted"),
			ValidateCaptureSnapshots(Bounded, Bounded).bAccepted);

		const FCaptureReadinessSnapshot Empty = MakeSnapshot(MakeBoundedReady(1, 8, 0, 0));
		TestTrue(TEXT("A stable zero-page frontier is a valid view away from the scene"),
			ValidateCaptureSnapshots(Empty, Empty).bAccepted);

		FCaptureSourceSnapshot InvalidEmpty = MakeBoundedReady(1, 8, 0, 1);
		const FCaptureReadinessSnapshot InvalidEmptySnapshot = MakeSnapshot(InvalidEmpty);
		TestFalse(TEXT("A zero-page frontier cannot expose an active splat"),
			ValidateCaptureSnapshots(InvalidEmptySnapshot, InvalidEmptySnapshot).bAccepted);

		FCaptureSourceSnapshot AllResident = MakeBoundedReady(2, 1, 354, 23135444);
		AllResident.bBounded = false;
		AllResident.StreamingState = ECaptureStreamingState::AllResident;
		const FCaptureReadinessSnapshot AllResidentSnapshot = MakeSnapshot(AllResident);
		TestTrue(TEXT("A complete atomic all-resident source is accepted"),
			ValidateCaptureSnapshots(AllResidentSnapshot, AllResidentSnapshot).bAccepted);

		AllResident.bUploadComplete = false;
		const FCaptureReadinessSnapshot PartialAllResident = MakeSnapshot(AllResident);
		TestFalse(TEXT("A partially uploaded all-resident source is rejected"),
			ValidateCaptureSnapshots(PartialAllResident, PartialAllResident).bAccepted);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSCaptureReadinessStateTest,
		"NanoGS.Paged.CaptureReadiness.NonReadyStates",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSCaptureReadinessStateTest::RunTest(const FString& Parameters)
	{
		const ECaptureStreamingState RejectedStates[] = {
			ECaptureStreamingState::NotRequested,
			ECaptureStreamingState::Loading,
			ECaptureStreamingState::Draining,
			ECaptureStreamingState::OutOfCapacity,
			ECaptureStreamingState::Failed,
			ECaptureStreamingState::Unavailable,
		};
		for (const ECaptureStreamingState State : RejectedStates)
		{
			FCaptureSourceSnapshot Source = MakeBoundedReady();
			Source.StreamingState = State;
			const FCaptureReadinessSnapshot Snapshot = MakeSnapshot(Source);
			const FCaptureReadinessValidation Result = ValidateCaptureSnapshots(Snapshot, Snapshot);
			TestFalse(TEXT("Every non-Ready bounded state is rejected"), Result.bAccepted);
			TestTrue(TEXT("Failure is machine recognizable"),
				Result.Diagnostic.StartsWith(TEXT("nanogs_not_ready")));
		}

		FCaptureReadinessSnapshot UnavailableQuery;
		const FCaptureReadinessValidation QueryResult =
			ValidateCaptureSnapshots(UnavailableQuery, UnavailableQuery);
		TestFalse(TEXT("An unavailable state query fails closed"), QueryResult.bAccepted);
		TestTrue(TEXT("Unavailable query reports its reason"),
			QueryResult.Diagnostic.Contains(TEXT("reason=query_unavailable")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSCaptureReadinessPublicationTest,
		"NanoGS.Paged.CaptureReadiness.PublicationMustRemainIdentical",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSCaptureReadinessPublicationTest::RunTest(const FString& Parameters)
	{
		const FCaptureReadinessSnapshot Before = MakeSnapshot(MakeBoundedReady());

		auto ExpectPublicationRejected = [this, &Before](
			const TCHAR* What,
			const FCaptureSourceSnapshot& Changed)
		{
			const FCaptureReadinessValidation Result =
				ValidateCaptureSnapshots(Before, MakeSnapshot(Changed));
			TestFalse(What, Result.bAccepted);
			TestTrue(TEXT("A changed publication reports a stable diagnostic"),
				Result.Diagnostic.Contains(TEXT("reason=publication_changed")));
		};

		FCaptureSourceSnapshot Changed = MakeBoundedReady();
		Changed.ResidentGeneration++;
		ExpectPublicationRejected(TEXT("Resident generation changes are rejected"), Changed);

		Changed = MakeBoundedReady();
		Changed.RequestedPageCount++;
		Changed.PublishedPageCount++;
		Changed.ActiveSplatCount++;
		ExpectPublicationRejected(TEXT("A different complete target is rejected"), Changed);

		Changed = MakeBoundedReady();
		Changed.SourceIdentity++;
		ExpectPublicationRejected(TEXT("Source replacement is rejected"), Changed);

		FCaptureReadinessSnapshot AddedSource = Before;
		AddedSource.Sources.Add(MakeBoundedReady(2));
		const FCaptureReadinessValidation AddedResult =
			ValidateCaptureSnapshots(Before, AddedSource);
		TestFalse(TEXT("A changed source set is rejected"), AddedResult.bAccepted);
		TestTrue(TEXT("Source set change is diagnosed"),
			AddedResult.Diagnostic.Contains(TEXT("reason=source_set_changed")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSCaptureDrawTicketSuccessTest,
		"NanoGS.Paged.CaptureReadiness.DrawTicketExactViewSuccess",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSCaptureDrawTicketSuccessTest::RunTest(const FString& Parameters)
	{
		FCaptureDrawTicketTracker Tracker;
		const FCaptureReadinessSnapshot Stable = MakeSnapshot(MakeBoundedReady());
		const FCaptureViewSignature Signature = MakeViewSignature();
		const FCaptureDrawTicketHandle Handle = Tracker.Begin(1, 11, 101, true);

		Tracker.BindView(Handle.TicketId, 11, 1, 101, Signature);
		TestEqual(TEXT("Bound ticket is selected for the exact view"),
			Tracker.PrepareActivation(Handle.TicketId, 11, 1, 101, Signature), Handle.TicketId);
		TestFalse(TEXT("A queued older view cannot claim before the RT marker"),
			Tracker.Claim_RenderThread(1, 101, Signature).IsValid());
		Tracker.Activate_RenderThread(Handle.TicketId, 11);
		const FCaptureDrawTicketClaim Claim =
			Tracker.Claim_RenderThread(1, 101, Signature);
		TestTrue(TEXT("The exact target and no-jitter signature claim the ticket"), Claim.IsValid());
		TestEqual(TEXT("The authenticated component identity reaches the RT claim"),
			Claim.ComponentIdentity, static_cast<uint64>(11));
		Tracker.Succeed_RenderThread(Claim, Stable);
		TestTrue(TEXT("A completed draw with the same publication is accepted"),
			Tracker.Consume(Handle, Stable, Stable).bAccepted);

		FCaptureReadinessSnapshot NoSources;
		NoSources.bQueryAvailable = true;
		const FCaptureDrawTicketHandle Bypass = Tracker.Begin(1, 12, 102, false);
		TestTrue(TEXT("A no-Page capture remains compatible without a draw ticket"),
			Tracker.Consume(Bypass, NoSources, NoSources).bAccepted);

		const FCaptureReadinessSnapshot EmptyFrontier =
			MakeSnapshot(MakeBoundedReady(1, 8, 0, 0));
		const FCaptureDrawTicketHandle EmptyHandle = Tracker.Begin(1, 13, 103, true);
		const FCaptureViewSignature EmptyView = MakeViewSignature(30.0);
		Tracker.BindView(EmptyHandle.TicketId, 13, 1, 103, EmptyView);
		Tracker.Activate_RenderThread(
			Tracker.PrepareActivation(EmptyHandle.TicketId, 13, 1, 103, EmptyView), 13);
		const FCaptureDrawTicketClaim EmptyClaim =
			Tracker.Claim_RenderThread(1, 103, EmptyView);
		Tracker.Succeed_RenderThread(EmptyClaim, EmptyFrontier);
		TestTrue(TEXT("A renderer-completed bounded empty frontier is a valid background capture"),
			Tracker.Consume(EmptyHandle, EmptyFrontier, EmptyFrontier).bAccepted);

		const FCaptureDrawTicketHandle MainRenderer =
			Tracker.Begin(1, 14, 104, true, false);
		const FCaptureReadinessValidation MainRendererResult =
			Tracker.Consume(MainRenderer, Stable, Stable);
		TestFalse(TEXT("A main-renderer SceneCapture fails closed"), MainRendererResult.bAccepted);
		TestTrue(TEXT("The unsupported capture mode is diagnosed explicitly"),
			MainRendererResult.Diagnostic.Contains(TEXT("non_independent_scene_capture")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSCaptureDrawTicketIsolationTest,
		"NanoGS.Paged.CaptureReadiness.DrawTicketMultiCameraIsolation",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSCaptureDrawTicketIsolationTest::RunTest(const FString& Parameters)
	{
		FCaptureDrawTicketTracker Tracker;
		const FCaptureReadinessSnapshot Stable = MakeSnapshot(MakeBoundedReady());
		const FCaptureDrawTicketHandle CameraA = Tracker.Begin(1, 11, 101, true);
		const FCaptureDrawTicketHandle CameraB = Tracker.Begin(1, 12, 102, true);
		const FCaptureViewSignature ViewA = MakeViewSignature(10.0);
		const FCaptureViewSignature ViewB = MakeViewSignature(20.0);
		Tracker.BindView(CameraA.TicketId, 11, 1, 101, ViewA);
		Tracker.BindView(CameraB.TicketId, 12, 1, 102, ViewB);
		Tracker.Activate_RenderThread(
			Tracker.PrepareActivation(CameraA.TicketId, 11, 1, 101, ViewA), 11);
		Tracker.Activate_RenderThread(
			Tracker.PrepareActivation(CameraB.TicketId, 12, 1, 102, ViewB), 12);

		const FCaptureDrawTicketClaim ClaimA = Tracker.Claim_RenderThread(1, 101, ViewA);
		Tracker.Succeed_RenderThread(ClaimA, Stable);
		const FCaptureDrawTicketClaim ClaimB = Tracker.Claim_RenderThread(1, 102, ViewB);
		Tracker.Fail_RenderThread(ClaimB, TEXT("max_render_budget"));

		TestTrue(TEXT("Camera A succeeds independently"),
			Tracker.Consume(CameraA, Stable, Stable).bAccepted);
		const FCaptureReadinessValidation CameraBResult =
			Tracker.Consume(CameraB, Stable, Stable);
		TestFalse(TEXT("Camera B renderer early-out fails closed"), CameraBResult.bAccepted);
		TestTrue(TEXT("Camera B reports its renderer failure"),
			CameraBResult.Diagnostic.Contains(TEXT("reason=render_failed")) &&
			CameraBResult.Diagnostic.Contains(TEXT("max_render_budget")));

		const FCaptureDrawTicketHandle CameraC = Tracker.Begin(1, 13, 103, true);
		Tracker.BindView(CameraC.TicketId, 13, 1, 103, ViewA);
		Tracker.Activate_RenderThread(
			Tracker.PrepareActivation(CameraC.TicketId, 13, 1, 103, ViewA), 13);
		TestFalse(TEXT("A different view cannot claim Camera C"),
			Tracker.Claim_RenderThread(1, 103, ViewB).IsValid());
		TestTrue(TEXT("A signature mismatch is retained as a fail-closed result"),
			Tracker.Consume(CameraC, Stable, Stable).Diagnostic.Contains(
				TEXT("render_view_signature_mismatch")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSCaptureProjectionPhaseTest,
		"NanoGS.Paged.CaptureReadiness.ProjectionPhaseContract",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSCaptureProjectionPhaseTest::RunTest(const FString& Parameters)
	{
		FMatrix Projection = FMatrix::Identity;
		Projection.M[0][0] = 1.75;
		Projection.M[1][1] = 2.25;
		Projection.M[2][0] = 0.125;
		FViewMatrices::FMinimalInitializer Initializer;
		Initializer.ProjectionMatrix = Projection;
		Initializer.ConstrainedViewRect = FIntRect(0, 0, 640, 480);
		FViewMatrices Matrices(Initializer);

		const FMatrix GameThreadProjection = Matrices.GetProjectionMatrix();
		TestFalse(TEXT("Stored ProjectionNoAA is not initialized to the GT projection"),
			Matrices.GetProjectionNoAAMatrix().Equals(GameThreadProjection, 0.0));
		Matrices.SaveProjectionNoAAMatrix();
		TestTrue(TEXT("RT saved ProjectionNoAA is bit-exact with the GT projection"),
			GameThreadProjection.Equals(Matrices.GetProjectionNoAAMatrix(), 0.0));

		FCaptureViewSignature Bound = MakeViewSignature();
		FCaptureViewSignature Rendered = Bound;
		Rendered.ProjectionNoAAMatrix.M[2][0] += 0.01;
		const FString Detail = Bound.DescribeMismatch(Rendered);
		TestTrue(TEXT("Projection mismatch diagnostics identify the field"),
			Detail.Contains(TEXT("projection_no_aa")));
		TestTrue(TEXT("Projection mismatch diagnostics include the maximum delta"),
			Detail.Contains(TEXT("projection_max_abs=")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FNanoGSCaptureComponentIdentityTest,
		"NanoGS.Paged.CaptureReadiness.ComponentIdentityAuthentication",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FNanoGSCaptureComponentIdentityTest::RunTest(const FString& Parameters)
	{
		FCaptureDrawTicketTracker Tracker;
		const FCaptureReadinessSnapshot Stable = MakeSnapshot(MakeBoundedReady());
		const FCaptureViewSignature View = MakeViewSignature();

		const FCaptureDrawTicketHandle BindMismatch = Tracker.Begin(1, 11, 101, true);
		Tracker.BindView(BindMismatch.TicketId, 12, 1, 101, View);
		const FCaptureReadinessValidation BindResult =
			Tracker.Consume(BindMismatch, Stable, Stable);
		TestFalse(TEXT("A different component cannot bind the ticket"), BindResult.bAccepted);
		TestTrue(TEXT("Bind mismatch retains an explicit diagnostic"),
			BindResult.Diagnostic.Contains(TEXT("component_identity_mismatch_during_bind")));

		const FCaptureDrawTicketHandle ActivationMismatch = Tracker.Begin(1, 21, 201, true);
		Tracker.BindView(ActivationMismatch.TicketId, 21, 1, 201, View);
		const uint64 ActivationId = Tracker.PrepareActivation(
			ActivationMismatch.TicketId, 21, 1, 201, View);
		Tracker.Activate_RenderThread(ActivationId, 22);
		const FCaptureReadinessValidation ActivationResult =
			Tracker.Consume(ActivationMismatch, Stable, Stable);
		TestFalse(TEXT("A different component cannot activate the RT ticket"),
			ActivationResult.bAccepted);
		TestTrue(TEXT("RT component mismatch retains an explicit diagnostic"),
			ActivationResult.Diagnostic.Contains(TEXT("component_identity_mismatch_on_render_thread")));
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
