// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FSceneInterface;
class FRenderTarget;
class FSceneView;
class FSceneViewFamily;
class USceneCaptureComponent2D;

namespace NanoGS::Paged
{
	/** Stable, public capture-facing copy of the internal Page streaming state. */
	enum class ECaptureStreamingState : uint8
	{
		AllResident = 0,
		NotRequested,
		Loading,
		Draining,
		Ready,
		OutOfCapacity,
		Failed,
		Unavailable,
	};

	/**
	 * One atomically sampled Page source. SourceIdentity is process-local and is
	 * used only to compare the two snapshots surrounding one image readback.
	 */
	struct NANOGS_API FCaptureSourceSnapshot
	{
		uint64 SourceIdentity = 0;
		ECaptureStreamingState StreamingState = ECaptureStreamingState::Unavailable;
		uint32 ResidentGeneration = 0;
		uint32 RequestedPageCount = 0;
		uint32 PublishedPageCount = 0;
		uint32 ActiveSplatCount = 0;
		bool bBounded = false;
		bool bUploadComplete = false;
		bool bUploadFailed = false;
		bool bStableAtomicSample = false;

		bool IsReady() const;
		bool HasSamePublication(const FCaptureSourceSnapshot& Other) const;
		FString Describe() const;
	};

	/** All Page sources registered in one FSceneInterface, sorted by identity. */
	struct NANOGS_API FCaptureReadinessSnapshot
	{
		bool bQueryAvailable = false;
		TArray<FCaptureSourceSnapshot> Sources;

		bool HasPagedSources() const { return !Sources.IsEmpty(); }
		bool AreAllSourcesReady() const;
		void SortBySourceIdentity();
		FString Describe() const;
	};

	struct NANOGS_API FCaptureReadinessValidation
	{
		bool bAccepted = false;
		FString Diagnostic;
	};

	/** Exact no-jitter identity copied from the GT SceneCapture view into FViewInfo. */
	struct NANOGS_API FCaptureViewSignature
	{
		FMatrix ViewMatrix = FMatrix::Identity;
		FMatrix ProjectionNoAAMatrix = FMatrix::Identity;
		FIntRect UnscaledViewRect;
		uint8 SceneCaptureSource = 0;
		bool bIsSceneCapture = false;

		bool IsValid() const;
		bool IsEquivalent(const FCaptureViewSignature& Other) const;
		FString DescribeMismatch(const FCaptureViewSignature& Other) const;
	};

	struct NANOGS_API FCaptureDrawTicketHandle
	{
		uint64 TicketId = 0;
		bool bRequired = false;
	};

	struct NANOGS_API FCaptureDrawTicketClaim
	{
		uint64 TicketId = 0;
		uint64 ComponentIdentity = 0;
		bool IsValid() const { return TicketId != 0 && ComponentIdentity != 0; }
	};

	/**
	 * Thread-safe, identity-specific SceneCapture proof tracker. Public inputs are
	 * pointer identities so automation can exercise the state machine without UObjects.
	 */
	class NANOGS_API FCaptureDrawTicketTracker
	{
	public:
		FCaptureDrawTicketHandle Begin(
			uint64 SceneIdentity,
			uint64 ComponentIdentity,
			uint64 RenderTargetIdentity,
			bool bRequired,
			bool bIndependentSceneCapture = true);
		void BindView(
			uint64 TicketId,
			uint64 ComponentIdentity,
			uint64 SceneIdentity,
			uint64 RenderTargetIdentity,
			const FCaptureViewSignature& Signature);
		uint64 PrepareActivation(
			uint64 TicketId,
			uint64 ComponentIdentity,
			uint64 SceneIdentity,
			uint64 RenderTargetIdentity,
			const FCaptureViewSignature& Signature);
		void Activate_RenderThread(uint64 TicketId, uint64 ComponentIdentity);
		FCaptureDrawTicketClaim Claim_RenderThread(
			uint64 SceneIdentity,
			uint64 RenderTargetIdentity,
			const FCaptureViewSignature& Signature);
		void Fail_RenderThread(const FCaptureDrawTicketClaim& Claim, const FString& Reason);
		void Succeed_RenderThread(
			const FCaptureDrawTicketClaim& Claim,
			const FCaptureReadinessSnapshot& DrawPublication);
		FCaptureReadinessValidation Consume(
			const FCaptureDrawTicketHandle& Handle,
			const FCaptureReadinessSnapshot& Before,
			const FCaptureReadinessSnapshot& After);
		void ResetForTests();

	private:
		enum class EState : uint8
		{
			AwaitingView,
			ViewBound,
			RenderArmed,
			Claimed,
			Succeeded,
			Failed,
		};

		struct FRecord
		{
			uint64 TicketId = 0;
			uint64 SceneIdentity = 0;
			uint64 ComponentIdentity = 0;
			uint64 RenderTargetIdentity = 0;
			FCaptureViewSignature ViewSignature;
			FCaptureReadinessSnapshot DrawPublication;
			EState State = EState::AwaitingView;
			FString FailureReason;
		};

		FCriticalSection Lock;
		TMap<uint64, FRecord> Records;
		uint64 NextTicketId = 1;
	};

	/**
	 * Authenticates view-family construction as causally belonging to one explicit
	 * USceneCaptureComponent2D::CaptureScene call. UE does not retain the component
	 * pointer in FSceneView, so SetupView/BeginRenderViewFamily consume this GT-only
	 * scope and the RT activation marker carries the verified component identity.
	 */
	class NANOGS_API FScopedCaptureDrawTicketComponent
	{
	public:
		FScopedCaptureDrawTicketComponent(
			const FCaptureDrawTicketHandle& Handle,
			const USceneCaptureComponent2D* CaptureComponent);
		~FScopedCaptureDrawTicketComponent();

		FScopedCaptureDrawTicketComponent(const FScopedCaptureDrawTicketComponent&) = delete;
		FScopedCaptureDrawTicketComponent& operator=(const FScopedCaptureDrawTicketComponent&) = delete;

	private:
		uint64 TicketId = 0;
		uint64 ComponentIdentity = 0;
		uint64 PreviousTicketId = 0;
		uint64 PreviousComponentIdentity = 0;
		bool bActive = false;
	};

	/** Pure helper used by AirSim after readback and by automation tests. */
	NANOGS_API FCaptureReadinessValidation ValidateCaptureSnapshots(
		const FCaptureReadinessSnapshot& Before,
		const FCaptureReadinessSnapshot& After);

	NANOGS_API FCaptureViewSignature BuildCaptureViewSignature_GameThread(const FSceneView& View);
	NANOGS_API FCaptureViewSignature BuildCaptureViewSignature_RenderThread(const FSceneView& View);
	NANOGS_API FCaptureDrawTicketHandle BeginCaptureDrawTicket(
		const FSceneInterface* Scene,
		const USceneCaptureComponent2D* CaptureComponent,
		const FRenderTarget* RenderTarget,
		const FCaptureReadinessSnapshot& Before);
	NANOGS_API void BindCaptureDrawTicketView_GameThread(
		const FSceneViewFamily& ViewFamily,
		const FSceneView& View);
	NANOGS_API void ArmCaptureDrawTicketForViewFamily_GameThread(
		const FSceneViewFamily& ViewFamily);
	NANOGS_API FCaptureDrawTicketClaim ClaimCaptureDrawTicket_RenderThread(
		const FSceneView& View);
	NANOGS_API void FailCaptureDrawTicket_RenderThread(
		const FCaptureDrawTicketClaim& Claim,
		const FString& Reason);
	NANOGS_API void SucceedCaptureDrawTicket_RenderThread(
		const FCaptureDrawTicketClaim& Claim,
		const FCaptureReadinessSnapshot& DrawPublication);
	NANOGS_API FCaptureReadinessValidation ValidateCaptureTransaction(
		const FCaptureDrawTicketHandle& Handle,
		const FCaptureReadinessSnapshot& Before,
		const FCaptureReadinessSnapshot& After);

	/**
	 * Read the current Page state without a render-thread flush. The view-extension
	 * proxy lock protects proxy lifetime; individual Page fields are atomics.
	 */
	NANOGS_API FCaptureReadinessSnapshot QueryCaptureReadiness(const FSceneInterface* Scene);
}
