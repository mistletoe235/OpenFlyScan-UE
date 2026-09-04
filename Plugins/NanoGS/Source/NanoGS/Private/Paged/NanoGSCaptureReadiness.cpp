// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSCaptureReadiness.h"

#include "GaussianSplatViewExtension.h"
#include "Components/SceneCaptureComponent2D.h"
#include "RenderingThread.h"
#include "SceneView.h"

namespace NanoGS::Paged
{
		namespace
		{
			FCaptureDrawTicketTracker& GetDrawTicketTracker()
			{
				static FCaptureDrawTicketTracker Tracker;
				return Tracker;
			}

			uint64 PointerIdentity(const void* Pointer)
			{
				return static_cast<uint64>(reinterpret_cast<UPTRINT>(Pointer));
			}

			struct FActiveCaptureComponentScope
			{
				uint64 TicketId = 0;
				uint64 ComponentIdentity = 0;
			};

			thread_local FActiveCaptureComponentScope ActiveCaptureComponentScope;

			double MaxAbsMatrixDifference(const FMatrix& A, const FMatrix& B)
			{
				double MaxDifference = 0.0;
				for (int32 Row = 0; Row < 4; ++Row)
				{
					for (int32 Column = 0; Column < 4; ++Column)
					{
						MaxDifference = FMath::Max(
							MaxDifference,
							FMath::Abs(A.M[Row][Column] - B.M[Row][Column]));
					}
				}
				return MaxDifference;
			}

		const TCHAR* LexToString(const ECaptureStreamingState State)
			{
			switch (State)
			{
			case ECaptureStreamingState::AllResident:
				return TEXT("AllResident");
			case ECaptureStreamingState::NotRequested:
				return TEXT("NotRequested");
			case ECaptureStreamingState::Loading:
				return TEXT("Loading");
			case ECaptureStreamingState::Draining:
				return TEXT("Draining");
			case ECaptureStreamingState::Ready:
				return TEXT("Ready");
			case ECaptureStreamingState::OutOfCapacity:
				return TEXT("OutOfCapacity");
			case ECaptureStreamingState::Failed:
				return TEXT("Failed");
			case ECaptureStreamingState::Unavailable:
			default:
				return TEXT("Unavailable");
				}
			}

		FCaptureReadinessValidation Reject(const TCHAR* Reason, const FString& Detail)
		{
			FCaptureReadinessValidation Result;
			Result.Diagnostic = FString::Printf(
				TEXT("nanogs_not_ready reason=%s%s%s"),
				Reason,
				Detail.IsEmpty() ? TEXT("") : TEXT(" detail="),
				*Detail);
			return Result;
		}
	}

	bool FCaptureViewSignature::IsValid() const
	{
		if (!bIsSceneCapture || UnscaledViewRect.Width() <= 0 || UnscaledViewRect.Height() <= 0)
		{
			return false;
		}
		for (int32 Row = 0; Row < 4; ++Row)
		{
			for (int32 Column = 0; Column < 4; ++Column)
			{
				if (!FMath::IsFinite(ViewMatrix.M[Row][Column]) ||
					!FMath::IsFinite(ProjectionNoAAMatrix.M[Row][Column]))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool FCaptureViewSignature::IsEquivalent(const FCaptureViewSignature& Other) const
	{
		return IsValid() && Other.IsValid() &&
			bIsSceneCapture == Other.bIsSceneCapture &&
			SceneCaptureSource == Other.SceneCaptureSource &&
			UnscaledViewRect == Other.UnscaledViewRect &&
			ViewMatrix.Equals(Other.ViewMatrix, 0.0) &&
			ProjectionNoAAMatrix.Equals(Other.ProjectionNoAAMatrix, 0.0);
	}

	FString FCaptureViewSignature::DescribeMismatch(const FCaptureViewSignature& Other) const
	{
		TArray<FString> Fields;
		if (bIsSceneCapture != Other.bIsSceneCapture)
			Fields.Add(TEXT("is_scene_capture"));
		if (SceneCaptureSource != Other.SceneCaptureSource)
			Fields.Add(TEXT("capture_source"));
		if (UnscaledViewRect != Other.UnscaledViewRect)
			Fields.Add(TEXT("view_rect"));
		if (!ViewMatrix.Equals(Other.ViewMatrix, 0.0))
			Fields.Add(TEXT("view_matrix"));
		if (!ProjectionNoAAMatrix.Equals(Other.ProjectionNoAAMatrix, 0.0))
			Fields.Add(TEXT("projection_no_aa"));

		return FString::Printf(
			TEXT("fields=%s,bound_valid=%u,render_valid=%u,bound_rect=[%d,%d,%d,%d],render_rect=[%d,%d,%d,%d],bound_source=%u,render_source=%u,view_max_abs=%.17g,projection_max_abs=%.17g"),
			Fields.IsEmpty() ? TEXT("none") : *FString::Join(Fields, TEXT("|")),
			IsValid() ? 1u : 0u,
			Other.IsValid() ? 1u : 0u,
			UnscaledViewRect.Min.X,
			UnscaledViewRect.Min.Y,
			UnscaledViewRect.Max.X,
			UnscaledViewRect.Max.Y,
			Other.UnscaledViewRect.Min.X,
			Other.UnscaledViewRect.Min.Y,
			Other.UnscaledViewRect.Max.X,
			Other.UnscaledViewRect.Max.Y,
			static_cast<uint32>(SceneCaptureSource),
			static_cast<uint32>(Other.SceneCaptureSource),
			MaxAbsMatrixDifference(ViewMatrix, Other.ViewMatrix),
			MaxAbsMatrixDifference(ProjectionNoAAMatrix, Other.ProjectionNoAAMatrix));
	}

	FCaptureDrawTicketHandle FCaptureDrawTicketTracker::Begin(
		const uint64 SceneIdentity,
		const uint64 ComponentIdentity,
		const uint64 RenderTargetIdentity,
		const bool bRequired,
		const bool bIndependentSceneCapture)
	{
		FCaptureDrawTicketHandle Handle;
		Handle.bRequired = bRequired;
		if (!bRequired)
		{
			return Handle;
		}

		FScopeLock Guard(&Lock);
		Handle.TicketId = NextTicketId++;
		if (Handle.TicketId == 0)
		{
			Handle.TicketId = NextTicketId++;
		}

		FRecord Record;
		Record.TicketId = Handle.TicketId;
		Record.SceneIdentity = SceneIdentity;
		Record.ComponentIdentity = ComponentIdentity;
		Record.RenderTargetIdentity = RenderTargetIdentity;
		if (SceneIdentity == 0 || ComponentIdentity == 0 || RenderTargetIdentity == 0)
		{
			Record.State = EState::Failed;
			Record.FailureReason = TEXT("invalid_ticket_identity");
		}
		else if (!bIndependentSceneCapture)
		{
			// Main-renderer SceneCapture views do not preserve the one-component / one-
			// render-target family identity required by this exact capture proof.
			Record.State = EState::Failed;
			Record.FailureReason = TEXT("non_independent_scene_capture");
		}
		else
		{
			for (const TPair<uint64, FRecord>& Pair : Records)
			{
				if (Pair.Value.RenderTargetIdentity == RenderTargetIdentity)
				{
					Record.State = EState::Failed;
					Record.FailureReason = TEXT("render_target_already_pending");
					break;
				}
			}
		}
		Records.Add(Handle.TicketId, MoveTemp(Record));
		return Handle;
	}

	void FCaptureDrawTicketTracker::BindView(
		const uint64 TicketId,
		const uint64 ComponentIdentity,
		const uint64 SceneIdentity,
		const uint64 RenderTargetIdentity,
		const FCaptureViewSignature& Signature)
	{
		FScopeLock Guard(&Lock);
		FRecord* Record = Records.Find(TicketId);
		if (Record == nullptr || Record->State == EState::Failed)
		{
			return;
		}
		if (Record->ComponentIdentity != ComponentIdentity)
		{
			Record->State = EState::Failed;
			Record->FailureReason = TEXT("component_identity_mismatch_during_bind");
			return;
		}
		// Other view families can be constructed re-entrantly while CaptureScene
		// flushes end-of-frame work. Ignore them unless both stable family identities
		// match; a second matching SetupView fails below instead of being misaccepted.
		if (Record->SceneIdentity != SceneIdentity ||
			Record->RenderTargetIdentity != RenderTargetIdentity)
		{
			return;
		}
		if (Record->State == EState::ViewBound)
		{
			Record->State = EState::Failed;
			Record->FailureReason = TEXT("multiple_views_for_capture");
			return;
		}
		if (Record->State != EState::AwaitingView)
		{
			Record->State = EState::Failed;
			Record->FailureReason = TEXT("invalid_bind_order");
			return;
		}
		if (!Signature.IsValid())
		{
			Record->State = EState::Failed;
			Record->FailureReason = TEXT("invalid_view_signature");
			return;
		}
		Record->ViewSignature = Signature;
		Record->State = EState::ViewBound;
	}

	uint64 FCaptureDrawTicketTracker::PrepareActivation(
		const uint64 TicketId,
		const uint64 ComponentIdentity,
		const uint64 SceneIdentity,
		const uint64 RenderTargetIdentity,
		const FCaptureViewSignature& Signature)
	{
		FScopeLock Guard(&Lock);
		FRecord* Record = Records.Find(TicketId);
		if (Record == nullptr || Record->State == EState::Failed)
		{
			return 0;
		}
		if (Record->ComponentIdentity != ComponentIdentity)
		{
			Record->State = EState::Failed;
			Record->FailureReason = TEXT("component_identity_mismatch_during_activation");
			return 0;
		}
		if (Record->SceneIdentity != SceneIdentity ||
			Record->RenderTargetIdentity != RenderTargetIdentity)
		{
			return 0;
		}
		if (Record->State != EState::ViewBound)
		{
			Record->State = EState::Failed;
			Record->FailureReason = TEXT("invalid_prepare_order");
			return 0;
		}
		if (!Record->ViewSignature.IsEquivalent(Signature))
		{
			Record->State = EState::Failed;
			Record->FailureReason = FString::Printf(
				TEXT("view_signature_changed_before_enqueue %s"),
				*Record->ViewSignature.DescribeMismatch(Signature));
			return 0;
		}
		return Record->TicketId;
	}

	void FCaptureDrawTicketTracker::Activate_RenderThread(
		const uint64 TicketId,
		const uint64 ComponentIdentity)
	{
		FScopeLock Guard(&Lock);
		if (FRecord* Record = Records.Find(TicketId))
		{
			if (Record->ComponentIdentity != ComponentIdentity)
			{
				Record->State = EState::Failed;
				Record->FailureReason = TEXT("component_identity_mismatch_on_render_thread");
				return;
			}
			if (Record->State == EState::ViewBound)
			{
				Record->State = EState::RenderArmed;
			}
			else if (Record->State != EState::Failed)
			{
				Record->State = EState::Failed;
				Record->FailureReason = TEXT("invalid_activation_order");
			}
		}
	}

	FCaptureDrawTicketClaim FCaptureDrawTicketTracker::Claim_RenderThread(
		const uint64 SceneIdentity,
		const uint64 RenderTargetIdentity,
		const FCaptureViewSignature& Signature)
	{
		FScopeLock Guard(&Lock);
		for (TPair<uint64, FRecord>& Pair : Records)
		{
			FRecord& Record = Pair.Value;
			if (Record.SceneIdentity != SceneIdentity ||
				Record.RenderTargetIdentity != RenderTargetIdentity ||
				Record.State != EState::RenderArmed)
			{
				continue;
			}
			if (!Record.ViewSignature.IsEquivalent(Signature))
			{
				Record.State = EState::Failed;
				Record.FailureReason = FString::Printf(
					TEXT("render_view_signature_mismatch %s"),
					*Record.ViewSignature.DescribeMismatch(Signature));
				return FCaptureDrawTicketClaim();
			}
			Record.State = EState::Claimed;
			FCaptureDrawTicketClaim Claim;
			Claim.TicketId = Record.TicketId;
			Claim.ComponentIdentity = Record.ComponentIdentity;
			return Claim;
		}
		return FCaptureDrawTicketClaim();
	}

	void FCaptureDrawTicketTracker::Fail_RenderThread(
		const FCaptureDrawTicketClaim& Claim,
		const FString& Reason)
	{
		if (!Claim.IsValid())
		{
			return;
		}
		FScopeLock Guard(&Lock);
		if (FRecord* Record = Records.Find(Claim.TicketId))
		{
			if (Record->ComponentIdentity != Claim.ComponentIdentity)
			{
				Record->State = EState::Failed;
				Record->FailureReason = TEXT("component_identity_mismatch_during_failure");
				return;
			}
			if (Record->State != EState::Succeeded)
			{
				Record->State = EState::Failed;
				Record->FailureReason = Reason;
			}
		}
	}

	void FCaptureDrawTicketTracker::Succeed_RenderThread(
		const FCaptureDrawTicketClaim& Claim,
		const FCaptureReadinessSnapshot& DrawPublication)
	{
		if (!Claim.IsValid())
		{
			return;
		}
		FScopeLock Guard(&Lock);
		if (FRecord* Record = Records.Find(Claim.TicketId))
		{
			if (Record->ComponentIdentity != Claim.ComponentIdentity)
			{
				Record->State = EState::Failed;
				Record->FailureReason = TEXT("component_identity_mismatch_during_success");
				return;
			}
			if (Record->State == EState::Claimed)
			{
				Record->DrawPublication = DrawPublication;
				Record->State = EState::Succeeded;
			}
		}
	}

	FCaptureReadinessValidation FCaptureDrawTicketTracker::Consume(
		const FCaptureDrawTicketHandle& Handle,
		const FCaptureReadinessSnapshot& Before,
		const FCaptureReadinessSnapshot& After)
	{
		const FCaptureReadinessValidation SnapshotValidation =
			ValidateCaptureSnapshots(Before, After);
		if (!Handle.bRequired)
		{
			return SnapshotValidation;
		}

		FRecord Record;
		bool bFound = false;
		{
			FScopeLock Guard(&Lock);
			if (const FRecord* Found = Records.Find(Handle.TicketId))
			{
				Record = *Found;
				bFound = true;
				Records.Remove(Handle.TicketId);
			}
		}

		if (!bFound)
		{
			return Reject(TEXT("draw_ticket_missing"), FString::Printf(
				TEXT("ticket=%llu"), static_cast<unsigned long long>(Handle.TicketId)));
		}
		if (!SnapshotValidation.bAccepted)
		{
			return SnapshotValidation;
		}
		if (Record.State != EState::Succeeded)
		{
			const TCHAR* StateReason = TEXT("draw_not_completed");
			switch (Record.State)
			{
			case EState::AwaitingView:
				StateReason = TEXT("view_not_bound");
				break;
			case EState::ViewBound:
				StateReason = TEXT("render_marker_missing");
				break;
			case EState::RenderArmed:
				StateReason = TEXT("render_callback_missing");
				break;
			case EState::Claimed:
				StateReason = TEXT("draw_not_completed");
				break;
			case EState::Failed:
				StateReason = TEXT("render_failed");
				break;
			default:
				break;
			}
			return Reject(StateReason, Record.FailureReason);
		}

		const FCaptureReadinessValidation DrawValidation =
			ValidateCaptureSnapshots(Record.DrawPublication, After);
		if (!DrawValidation.bAccepted)
		{
			return Reject(TEXT("draw_publication_mismatch"), DrawValidation.Diagnostic);
		}
		FCaptureReadinessValidation Result;
		Result.bAccepted = true;
		return Result;
	}

	void FCaptureDrawTicketTracker::ResetForTests()
	{
		FScopeLock Guard(&Lock);
		Records.Reset();
		NextTicketId = 1;
	}

	bool FCaptureSourceSnapshot::IsReady() const
	{
		if (!bStableAtomicSample || bUploadFailed || !bUploadComplete)
		{
			return false;
		}

		if (bBounded)
		{
			if (StreamingState != ECaptureStreamingState::Ready ||
				RequestedPageCount != PublishedPageCount)
			{
				return false;
			}
			// Looking completely away from the source is a valid empty frontier.
			return PublishedPageCount == 0 ? ActiveSplatCount == 0 : ActiveSplatCount > 0;
		}

		return StreamingState == ECaptureStreamingState::AllResident &&
			PublishedPageCount > 0 && ActiveSplatCount > 0;
	}

	bool FCaptureSourceSnapshot::HasSamePublication(const FCaptureSourceSnapshot& Other) const
	{
		return SourceIdentity == Other.SourceIdentity &&
			bBounded == Other.bBounded &&
			StreamingState == Other.StreamingState &&
			ResidentGeneration == Other.ResidentGeneration &&
			RequestedPageCount == Other.RequestedPageCount &&
			PublishedPageCount == Other.PublishedPageCount &&
			ActiveSplatCount == Other.ActiveSplatCount &&
			bUploadComplete == Other.bUploadComplete &&
			bUploadFailed == Other.bUploadFailed &&
			bStableAtomicSample == Other.bStableAtomicSample;
	}

	FString FCaptureSourceSnapshot::Describe() const
	{
		return FString::Printf(
			TEXT("source=%llu,mode=%s,state=%s,generation=%u,requested=%u,published=%u,active=%u,complete=%u,failed=%u,stable=%u"),
			static_cast<unsigned long long>(SourceIdentity),
			bBounded ? TEXT("bounded") : TEXT("all_resident"),
			LexToString(StreamingState),
			ResidentGeneration,
			RequestedPageCount,
			PublishedPageCount,
			ActiveSplatCount,
			bUploadComplete ? 1u : 0u,
			bUploadFailed ? 1u : 0u,
			bStableAtomicSample ? 1u : 0u);
	}

	bool FCaptureReadinessSnapshot::AreAllSourcesReady() const
	{
		if (!bQueryAvailable)
		{
			return false;
		}
		for (const FCaptureSourceSnapshot& Source : Sources)
		{
			if (!Source.IsReady())
			{
				return false;
			}
		}
		return true;
	}

	void FCaptureReadinessSnapshot::SortBySourceIdentity()
	{
		Sources.Sort([](const FCaptureSourceSnapshot& A, const FCaptureSourceSnapshot& B)
		{
			return A.SourceIdentity < B.SourceIdentity;
		});
	}

	FString FCaptureReadinessSnapshot::Describe() const
	{
		if (!bQueryAvailable)
		{
			return TEXT("query=unavailable");
		}
		if (Sources.IsEmpty())
		{
			return TEXT("sources=none");
		}

		TArray<FString> Descriptions;
		Descriptions.Reserve(Sources.Num());
		for (const FCaptureSourceSnapshot& Source : Sources)
		{
			Descriptions.Add(Source.Describe());
		}
		return FString::Join(Descriptions, TEXT("|"));
	}

	FCaptureReadinessValidation ValidateCaptureSnapshots(
		const FCaptureReadinessSnapshot& Before,
		const FCaptureReadinessSnapshot& After)
	{
		if (!Before.bQueryAvailable || !After.bQueryAvailable)
		{
			return Reject(TEXT("query_unavailable"),
				FString::Printf(TEXT("before={%s},after={%s}"), *Before.Describe(), *After.Describe()));
		}
		if (!Before.AreAllSourcesReady())
		{
			return Reject(TEXT("before_unready"), Before.Describe());
		}
		if (!After.AreAllSourcesReady())
		{
			return Reject(TEXT("after_unready"), After.Describe());
		}
		if (Before.Sources.Num() != After.Sources.Num())
		{
			return Reject(TEXT("source_set_changed"),
				FString::Printf(TEXT("before=%d,after=%d"), Before.Sources.Num(), After.Sources.Num()));
		}

		for (int32 Index = 0; Index < Before.Sources.Num(); ++Index)
		{
			if (!Before.Sources[Index].HasSamePublication(After.Sources[Index]))
			{
				return Reject(TEXT("publication_changed"), FString::Printf(
					TEXT("before={%s},after={%s}"),
					*Before.Sources[Index].Describe(),
					*After.Sources[Index].Describe()));
			}
		}

		FCaptureReadinessValidation Result;
		Result.bAccepted = true;
		return Result;
	}

	namespace
	{
		FCaptureViewSignature BuildCaptureViewSignatureForProjection(
			const FSceneView& View,
			const FMatrix& ProjectionNoAAMatrix)
		{
			FCaptureViewSignature Signature;
			Signature.ViewMatrix = View.ViewMatrices.GetViewMatrix();
			Signature.ProjectionNoAAMatrix = ProjectionNoAAMatrix;
			Signature.UnscaledViewRect = View.UnscaledViewRect;
			Signature.bIsSceneCapture = View.bIsSceneCapture;
			if (View.Family != nullptr)
			{
				Signature.SceneCaptureSource = static_cast<uint8>(View.Family->SceneCaptureSource);
			}
			return Signature;
		}
	}

	FCaptureViewSignature BuildCaptureViewSignature_GameThread(const FSceneView& View)
	{
		check(IsInGameThread());
		// At SetupView/BeginRenderViewFamily time UE has not applied temporal jitter;
		// the stored ProjectionNoAA member is still its constructor Identity value.
		return BuildCaptureViewSignatureForProjection(
			View, View.ViewMatrices.GetProjectionMatrix());
	}

	FCaptureViewSignature BuildCaptureViewSignature_RenderThread(const FSceneView& View)
	{
		check(IsInRenderingThread());
		// SceneVisibility saves the exact pre-jitter projection before PostOpaque.
		// Reading that saved matrix avoids an add/subtract floating-point round trip.
		return BuildCaptureViewSignatureForProjection(
			View, View.ViewMatrices.GetProjectionNoAAMatrix());
	}

	FScopedCaptureDrawTicketComponent::FScopedCaptureDrawTicketComponent(
		const FCaptureDrawTicketHandle& Handle,
		const USceneCaptureComponent2D* CaptureComponent)
	{
		check(IsInGameThread());
		PreviousTicketId = ActiveCaptureComponentScope.TicketId;
		PreviousComponentIdentity = ActiveCaptureComponentScope.ComponentIdentity;
		TicketId = Handle.TicketId;
		ComponentIdentity = PointerIdentity(CaptureComponent);
		const bool bHasAuthenticatedTicket =
			Handle.bRequired && TicketId != 0 && ComponentIdentity != 0;
		// Always shadow an outer scope. An invalid/non-Page nested CaptureScene must
		// never expose the outer component ticket to its view-extension callbacks.
		ActiveCaptureComponentScope.TicketId = bHasAuthenticatedTicket ? TicketId : 0;
		ActiveCaptureComponentScope.ComponentIdentity =
			bHasAuthenticatedTicket ? ComponentIdentity : 0;
		bActive = true;
	}

	FScopedCaptureDrawTicketComponent::~FScopedCaptureDrawTicketComponent()
	{
		check(IsInGameThread());
		check(bActive);
		ActiveCaptureComponentScope.TicketId = PreviousTicketId;
		ActiveCaptureComponentScope.ComponentIdentity = PreviousComponentIdentity;
	}

	FCaptureDrawTicketHandle BeginCaptureDrawTicket(
		const FSceneInterface* Scene,
		const USceneCaptureComponent2D* CaptureComponent,
		const FRenderTarget* RenderTarget,
		const FCaptureReadinessSnapshot& Before)
	{
		return GetDrawTicketTracker().Begin(
			PointerIdentity(Scene),
			PointerIdentity(CaptureComponent),
			PointerIdentity(RenderTarget),
			Before.bQueryAvailable && Before.HasPagedSources(),
			CaptureComponent != nullptr && !CaptureComponent->bRenderInMainRenderer);
	}

	void BindCaptureDrawTicketView_GameThread(
		const FSceneViewFamily& ViewFamily,
		const FSceneView& View)
	{
		check(IsInGameThread());
		if (ActiveCaptureComponentScope.TicketId == 0 ||
			ActiveCaptureComponentScope.ComponentIdentity == 0)
		{
			return;
		}
		GetDrawTicketTracker().BindView(
			ActiveCaptureComponentScope.TicketId,
			ActiveCaptureComponentScope.ComponentIdentity,
			PointerIdentity(ViewFamily.Scene),
			PointerIdentity(ViewFamily.RenderTarget),
			BuildCaptureViewSignature_GameThread(View));
	}

	void ArmCaptureDrawTicketForViewFamily_GameThread(const FSceneViewFamily& ViewFamily)
	{
		check(IsInGameThread());
		if (ActiveCaptureComponentScope.TicketId == 0 ||
			ActiveCaptureComponentScope.ComponentIdentity == 0 ||
			ViewFamily.Views.Num() != 1 || ViewFamily.Views[0] == nullptr)
		{
			return;
		}

		const uint64 TicketId = GetDrawTicketTracker().PrepareActivation(
			ActiveCaptureComponentScope.TicketId,
			ActiveCaptureComponentScope.ComponentIdentity,
			PointerIdentity(ViewFamily.Scene),
			PointerIdentity(ViewFamily.RenderTarget),
			BuildCaptureViewSignature_GameThread(*ViewFamily.Views[0]));
		if (TicketId == 0)
		{
			return;
		}

		const uint64 ComponentIdentity = ActiveCaptureComponentScope.ComponentIdentity;
		ENQUEUE_RENDER_COMMAND(ActivateNanoGSCaptureDrawTicket)(
			[TicketId, ComponentIdentity](FRHICommandListImmediate&)
			{
				GetDrawTicketTracker().Activate_RenderThread(TicketId, ComponentIdentity);
			});
	}

	FCaptureDrawTicketClaim ClaimCaptureDrawTicket_RenderThread(const FSceneView& View)
	{
		check(IsInRenderingThread());
		if (!View.bIsSceneCapture || View.Family == nullptr)
		{
			return FCaptureDrawTicketClaim();
		}
		return GetDrawTicketTracker().Claim_RenderThread(
			PointerIdentity(View.Family->Scene),
			PointerIdentity(View.Family->RenderTarget),
			BuildCaptureViewSignature_RenderThread(View));
	}

	void FailCaptureDrawTicket_RenderThread(
		const FCaptureDrawTicketClaim& Claim,
		const FString& Reason)
	{
		GetDrawTicketTracker().Fail_RenderThread(Claim, Reason);
	}

	void SucceedCaptureDrawTicket_RenderThread(
		const FCaptureDrawTicketClaim& Claim,
		const FCaptureReadinessSnapshot& DrawPublication)
	{
		GetDrawTicketTracker().Succeed_RenderThread(Claim, DrawPublication);
	}

	FCaptureReadinessValidation ValidateCaptureTransaction(
		const FCaptureDrawTicketHandle& Handle,
		const FCaptureReadinessSnapshot& Before,
		const FCaptureReadinessSnapshot& After)
	{
		return GetDrawTicketTracker().Consume(Handle, Before, After);
	}

	FCaptureReadinessSnapshot QueryCaptureReadiness(const FSceneInterface* Scene)
	{
		FCaptureReadinessSnapshot Snapshot;
		if (Scene == nullptr)
		{
			return Snapshot;
		}

		FGaussianSplatViewExtension* ViewExtension = FGaussianSplatViewExtension::Get();
		if (ViewExtension == nullptr)
		{
			return Snapshot;
		}

		Snapshot.bQueryAvailable = true;
		ViewExtension->BuildPagedCaptureReadinessSnapshot(Scene, Snapshot);
		Snapshot.SortBySourceIdentity();
		return Snapshot;
	}
}
