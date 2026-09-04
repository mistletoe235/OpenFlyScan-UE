// Copyright Epic Games, Inc. All Rights Reserved.

#include "NanoGS.h"
#include "GaussianSplatViewExtension.h"
#include "GaussianSplatRenderer.h"
#include "GaussianSplatShaders.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianGlobalAccumulator.h"
#include "NanoGSRHICompat.h"
#include "NanoGSRuntimeSceneLoader.h"
#include "NanoGSControlWidget.h"
#include "Paged/NanoGSCaptureReadiness.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "SceneViewExtension.h"
#include "Misc/CoreDelegates.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "SceneView.h"
#include "SceneRendering.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#define LOCTEXT_NAMESPACE "FNanoGSModule"

// Pass 2 parameter struct: declares IntermediateTexture as an RDG-tracked shader resource
// so that RDG inserts the proper RTV→SRV barrier between Pass 1 (write) and Pass 2 (read).
BEGIN_SHADER_PARAMETER_STRUCT(FGaussianCompositePassParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, IntermediateTexture)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

namespace
{
	struct FCaptureDrawPassEvidence
	{
		bool bRendererCompleted = false;
		NanoGS::Paged::FCaptureReadinessSnapshot DrawPublication;
	};

	struct FInteractiveViewportTelemetry
	{
		double WindowStartSeconds = 0.0;
		double LastFrameSeconds = 0.0;
		FMatrix LastViewProjection = FMatrix::Identity;
		bool bHasLastViewProjection = false;
		TArray<float> MovingFrameMilliseconds;
		TArray<float> StaticFrameMilliseconds;
		uint32 RecomputeFrames = 0;
		uint32 CacheReuseFrames = 0;
		uint32 DeferredSortFrames = 0;

		static float Percentile(TArray<float> Values, const double Fraction)
		{
			if (Values.IsEmpty())
			{
				return 0.0f;
			}
			Values.Sort();
			const int32 Index = FMath::Clamp(
				FMath::CeilToInt(Fraction * static_cast<double>(Values.Num())) - 1,
				0, Values.Num() - 1);
			return Values[Index];
		}

		void Record(const FMatrix& ViewProjection, const bool bCanSkip,
			const bool bSortDeferred, const uint32 ActiveSplats)
		{
			const double NowSeconds = FPlatformTime::Seconds();
			if (WindowStartSeconds <= 0.0)
			{
				WindowStartSeconds = NowSeconds;
			}
			if (LastFrameSeconds > 0.0 && bHasLastViewProjection)
			{
				const float FrameMilliseconds = static_cast<float>(
					FMath::Clamp((NowSeconds - LastFrameSeconds) * 1000.0, 0.0, 1000.0));
				const bool bCameraMoved = !LastViewProjection.Equals(ViewProjection, 0.0f);
				(bCameraMoved ? MovingFrameMilliseconds : StaticFrameMilliseconds).Add(FrameMilliseconds);
				if (bCanSkip)
				{
					++CacheReuseFrames;
				}
				else
				{
					++RecomputeFrames;
				}
				if (bSortDeferred)
				{
					++DeferredSortFrames;
				}
			}
			LastFrameSeconds = NowSeconds;
			LastViewProjection = ViewProjection;
			bHasLastViewProjection = true;

			if (NowSeconds - WindowStartSeconds < 2.0)
			{
				return;
			}

			const auto MaxValue = [](const TArray<float>& Values)
			{
				float Result = 0.0f;
				for (const float Value : Values)
				{
					Result = FMath::Max(Result, Value);
				}
				return Result;
			};
			UE_LOG(LogTemp, Display,
				TEXT("NANOGS_INTERACTIVE_RT moving=%d p50=%.3f p95=%.3f p99=%.3f max=%.3f static=%d p50=%.3f p95=%.3f p99=%.3f max=%.3f recompute=%u cache-reuse=%u sort-deferred=%u active-splats=%u"),
				MovingFrameMilliseconds.Num(),
				Percentile(MovingFrameMilliseconds, 0.50),
				Percentile(MovingFrameMilliseconds, 0.95),
				Percentile(MovingFrameMilliseconds, 0.99),
				MaxValue(MovingFrameMilliseconds),
				StaticFrameMilliseconds.Num(),
				Percentile(StaticFrameMilliseconds, 0.50),
				Percentile(StaticFrameMilliseconds, 0.95),
				Percentile(StaticFrameMilliseconds, 0.99),
				MaxValue(StaticFrameMilliseconds),
				RecomputeFrames, CacheReuseFrames, DeferredSortFrames, ActiveSplats);

			WindowStartSeconds = NowSeconds;
			MovingFrameMilliseconds.Reset();
			StaticFrameMilliseconds.Reset();
			RecomputeFrames = 0;
			CacheReuseFrames = 0;
			DeferredSortFrames = 0;
		}
	};

	bool PublishGlobalSortKeys(
		FRHICommandListImmediate& RHICmdList,
		FGaussianGlobalAccumulator* Accumulator,
		const uint32 SplatCapacity)
	{
		if (Accumulator == nullptr || SplatCapacity == 0 ||
			!Accumulator->GlobalSortKeysBuffer.IsValid())
		{
			return false;
		}

		const uint64 CopyBytes = static_cast<uint64>(SplatCapacity) * sizeof(uint32);
		if (!Accumulator->GlobalPublishedSortKeysBuffer.IsValid())
		{
			Accumulator->GlobalPublishedSortKeysBuffer = NanoGS::RHICompat::CreateBuffer(
				RHICmdList,
				TEXT("GlobalPublishedSortKeysBuffer"),
				static_cast<uint32>(CopyBytes),
				static_cast<uint32>(sizeof(uint32)),
				BUF_ShaderResource | BUF_StructuredBuffer,
				ERHIAccess::SRVGraphics);
			if (!Accumulator->GlobalPublishedSortKeysBuffer.IsValid())
			{
				return false;
			}
			Accumulator->GlobalPublishedSortKeysBufferSRV = RHICmdList.CreateShaderResourceView(
				Accumulator->GlobalPublishedSortKeysBuffer,
				FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(static_cast<uint32>(sizeof(uint32))));
		}
		RHICmdList.Transition(FRHITransitionInfo(
			Accumulator->GlobalSortKeysBuffer,
			ERHIAccess::SRVGraphics, ERHIAccess::CopySrc));
		RHICmdList.Transition(FRHITransitionInfo(
			Accumulator->GlobalPublishedSortKeysBuffer,
			ERHIAccess::SRVGraphics, ERHIAccess::CopyDest));
		RHICmdList.CopyBufferRegion(
			Accumulator->GlobalPublishedSortKeysBuffer, 0,
			Accumulator->GlobalSortKeysBuffer, 0, CopyBytes);
		RHICmdList.Transition(FRHITransitionInfo(
			Accumulator->GlobalSortKeysBuffer,
			ERHIAccess::CopySrc, ERHIAccess::SRVGraphics));
		RHICmdList.Transition(FRHITransitionInfo(
			Accumulator->GlobalPublishedSortKeysBuffer,
			ERHIAccess::CopyDest, ERHIAccess::SRVGraphics));
		Accumulator->bHasPublishedSortKeys = true;
		return true;
	}

	FInteractiveViewportTelemetry GInteractiveViewportTelemetry;
}

//----------------------------------------------------------------------
// Console Variables for Gaussian Splatting
//----------------------------------------------------------------------

/** Selects the screen-space raster contract without changing the imported data.
 *  Profile 0 follows the SuperSplat application shader used by the browser
 *  reference. Profile 1 follows the recovered Legacy LCC 5 raster contract. */
TAutoConsoleVariable<int32> CVarRasterProfile(
	TEXT("gs.RasterProfile"),
	0,
	TEXT("Gaussian splat raster profile.\n")
	TEXT(" 0: SuperSplat application parity (default)\n")
	TEXT(" 1: Legacy LCC parity (covariance AA, opacity compensation, 1/255 cutoff)\n")
	TEXT("The setting is render-thread safe and can be changed at runtime."),
	ECVF_RenderThreadSafe);

/** Show cluster debug visualization (Nanite-style coloring) */
TAutoConsoleVariable<int32> CVarShowClusterBounds(
	TEXT("gs.ShowClusterBounds"),
	0,
	TEXT("Debug visualization for Gaussian Splat clusters (Nanite-style).\n")
	TEXT("When enabled, shows cluster colors on black background (like Nanite debug view).\n")
	TEXT(" 0: Off (default)\n")
	TEXT(" 1: Show cluster colors (each cluster gets a unique random color)"),
	ECVF_RenderThreadSafe);

/** Maximum number of splats the global accumulator will allocate working buffers for.
 *  Caps VRAM usage for ViewData/sort/histogram buffers. If total visible splats exceed
 *  this budget (after Nanite LOD compaction), excess splats are simply not rendered.
 *  When budget is active, closer assets get priority (farther assets culled first).
 *  Default: 0 (unlimited). Example: 3M budget uses ~195 MB working buffers. */
TAutoConsoleVariable<int32> CVarMaxRenderBudget(
	TEXT("gs.MaxRenderBudget"),
	0,
	TEXT("Maximum number of splats to render per frame (render budget).\n")
	TEXT("Caps global accumulator buffer allocation and GPU-side visible count.\n")
	TEXT("When budget is exceeded, farther assets are culled first (closer assets have priority).\n")
	TEXT("Default: 0 (unlimited). Set to a positive value (e.g. 3000000) to limit splat count."),
	ECVF_RenderThreadSafe);

/** Use the asset's original Float32 positions in CalcViewData. The packed
 *  rotation/scale/color path remains active; setting this to zero also selects
 *  its float16 position for the lower-bandwidth fast mode. */
TAutoConsoleVariable<int32> CVarUseFloat32Position(
	TEXT("gs.UseFloat32Position"),
	1,
	TEXT("Select the Gaussian position source used by CalcViewData.\n")
	TEXT(" 0: Fast packed float16 position plus asset-local origin\n")
	TEXT(" 1: Original Float32 position (default, highest geometry fidelity)\n")
	TEXT("The setting is render-thread safe and can be changed at runtime."),
	ECVF_RenderThreadSafe);

/** Use the asset's original Float32 quaternion and scale payload in CalcViewData.
 *  The packed uint8 octahedral rotation and log-scale representation remains
 *  available as the lower-bandwidth fallback. */
TAutoConsoleVariable<int32> CVarUseFloat32RotationScale(
	TEXT("gs.UseFloat32RotationScale"),
	1,
	TEXT("Select the Gaussian rotation/scale source used by CalcViewData.\n")
	TEXT(" 0: Fast packed uint8 rotation and log-scale\n")
	TEXT(" 1: Original Float32 quaternion and scale (default, highest covariance fidelity)\n")
	TEXT("The setting is render-thread safe and can be changed at runtime."),
	ECVF_RenderThreadSafe);

/** Diagnostic escape hatch for shader hot reloads and cache audits. A failed
 *  shader dispatch cannot currently report success back through the render
 *  pipeline, so forcing recomputation prevents an empty/stale buffer from
 *  being treated as valid while testing a newly compiled shader. */
TAutoConsoleVariable<int32> CVarForceViewRecompute(
	TEXT("gs.ForceViewRecompute"),
	0,
	TEXT("Force CalcViewData and global depth sorting every frame.\n")
	TEXT(" 0: Reuse the camera-static cache (default)\n")
	TEXT(" 1: Recompute every frame; use temporarily after shader hot reload"),
	ECVF_RenderThreadSafe);

/** Emit the exact same sortable depth/index keys from CalcViewData while the
 * clip position is already in registers. Disable only for A/B diagnostics. */
static TAutoConsoleVariable<int32> CVarFuseDepthKeys(
	TEXT("gs.FuseDepthKeys"),
	1,
	TEXT("Fuse radix-sort key generation into CalcViewData.\n")
	TEXT(" 0: Run the legacy standalone CalcDistances pass\n")
	TEXT(" 1: Emit identical keys in CalcViewData (default; exact 32-bit key contract)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInteractivePublishedSort(
	TEXT("gs.InteractivePublishedSort"),
	0,
	TEXT("Spark-style stable published ordering for moving main views.\n")
	TEXT("0: exact synchronous radix sort on every changed view (default)\n")
	TEXT("1: retain the last complete ordering while selected movement frames update ViewData"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInteractiveSortEveryN(
	TEXT("gs.InteractiveSortEveryN"),
	2,
	TEXT("With gs.InteractivePublishedSort=1, publish an exact ordering every N camera-change frames.\n")
	TEXT("After movement settles, one final exact sort is always forced. Range 1-8; default 2."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarInteractiveSortSettleMilliseconds(
	TEXT("gs.InteractiveSortSettleMs"),
	100.0f,
	TEXT("Continuous quiet time before a deferred moving view receives its final exact sort.\n")
	TEXT("Prevents gaps between mouse samples from being mistaken for movement stop. Default 100 ms."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSPassTimingOneShot(
	TEXT("gs.PassTimingOneShot"),
	0,
	TEXT("One-shot diagnostic: on the next resident-generation change at or above "
		 "gs.PassTimingMinSplats, drain the GPU between NanoGS stages and log their isolated times. "
		 "This intentionally stalls and must remain disabled outside diagnosis."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSPassTimingMinSplats(
	TEXT("gs.PassTimingMinSplats"),
	8000000,
	TEXT("Minimum active splat count for gs.PassTimingOneShot (default 8,000,000)."),
	ECVF_RenderThreadSafe);

/** Sensor-server mode mirrors HybridGS' view filtering: SceneCapture output keeps
 * the full renderer while the otherwise unused main viewport skips NanoGS work. */
static TAutoConsoleVariable<int32> CVarNanoGSSensorOnlyMode(
	TEXT("gs.SensorOnlyMode"),
	0,
	TEXT("Render NanoGS only for SceneCapture views.\n")
	TEXT(" 0: Render main views and SceneCapture views (default)\n")
	TEXT(" 1: Render SceneCapture views only; intended for AirSim/CARLA sensor servers\n")
	TEXT("Use -NanoGSSensorOnly to enable this before the first rendered frame."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNanoGSControlUI(
	TEXT("gs.ControlUI"),
	1,
	TEXT("Show the compact NanoGS runtime controls in interactive game/PIE viewports.\n")
	TEXT(" 0: Hidden\n")
	TEXT(" 1: Visible (default). Use -NanoGSNoControlUI to disable at startup."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarNanoGSCompositeAfterTonemap(
	TEXT("gs.CompositeAfterTonemap"),
	1,
	TEXT("Select display-referred Gaussian composition after UE Tonemap.\n")
	TEXT(" 0: Immediate linear SceneColor composition for every view (compatibility fallback)\n")
	TEXT(" 1: After-tonemap composition for main views only (default)\n")
	TEXT(" 2: Also use after-tonemap composition for FinalColorLDR SceneCapture views\n")
	TEXT("Debug visualization and non-LDR SceneCapture views always retain the immediate path."),
	ECVF_RenderThreadSafe);

/** The original NanoGS parent splats are produced by a destructive 4:1 editor
 * merge (identity rotation and no higher-order SH). Keep them behind an
 * explicit opt-in so merely opening such an asset cannot silently reduce
 * quality. Validated streamed LOD will use a separate quality mode/contract. */
static TAutoConsoleVariable<int32> CVarEnableHeuristicLOD(
	TEXT("gs.EnableHeuristicLOD"),
	0,
	TEXT("Allow legacy NanoGS-generated approximate parent splats.\n")
	TEXT(" 0: Exact-HQ: render original L0 splats only (default)\n")
	TEXT(" 1: Enable legacy 4:1 heuristic LOD (experimental and visibly lossy)\n")
	TEXT("This does not control future quality-validated streamed LOD."),
	ECVF_RenderThreadSafe);

/** Debug: Force a specific LOD level for debugging LOD hierarchy */
TAutoConsoleVariable<int32> CVarDebugForceLODLevel(
	TEXT("gs.DebugForceLODLevel"),
	-1,
	TEXT("Force rendering of a specific LOD level for debugging (only affects Nanite-enabled assets).\n")
	TEXT("This ignores normal LOD selection and forces all clusters to use specified level.\n")
	TEXT(" -1: Auto - normal LOD selection based on distance/error (default)\n")
	TEXT("  0: Force leaf clusters only - render original splats (finest detail)\n")
	TEXT("  1+: Force specific LOD level (1 = first parent level, 2 = second, etc.)\n")
	TEXT("Note: Higher levels have fewer, coarser splats. Max level depends on asset size.\n")
	TEXT("Use with gs.ShowClusterBounds 2 to visualize which LOD level is being rendered."),
	ECVF_RenderThreadSafe);

// Export for other modules
int32 GGaussianSplatShowClusterBounds = 0;

// Helper to get the renderer module
static IRendererModule& GetRendererModuleRef()
{
	return FModuleManager::GetModuleChecked<IRendererModule>("Renderer");
}

/**
 * Display-referred compositor. The PostOpaque callback renders Gaussian color
 * into an externalized FP16 intermediate; this extension blends that texture
 * over UE's completed Tonemap output. Compatibility mode limits it to the main
 * view, while the explicit sensor mode also admits FinalColorLDR captures and
 * completes their strict Page draw ticket only after this pass executes.
 */
class FGaussianSplatCompositeViewExtension final : public FSceneViewExtensionBase
{
public:
	FGaussianSplatCompositeViewExtension(const FAutoRegister& AutoRegister)
		: FSceneViewExtensionBase(AutoRegister)
	{
	}

	virtual void SetupViewFamily(FSceneViewFamily&) override {}
	virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}

	void Collect(
		const uint32 ViewKey,
		const TRefCountPtr<IPooledRenderTarget>& Texture,
		const FIntRect& SourceRect,
		const FIntPoint& SourceTextureExtent,
		const NanoGS::Paged::FCaptureDrawTicketClaim& DrawTicketClaim,
		const TSharedRef<FCaptureDrawPassEvidence, ESPMode::ThreadSafe>& CaptureEvidence)
	{
		check(IsInRenderingThread());
		FPendingComposite& Pending = PendingComposites.FindOrAdd(ViewKey);
		Pending.Texture = Texture;
		Pending.SourceRect = SourceRect;
		Pending.SourceTextureExtent = SourceTextureExtent;
		Pending.DrawTicketClaim = DrawTicketClaim;
		Pending.CaptureEvidence = CaptureEvidence;
	}

	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass PassId,
		const FSceneView& InView,
		FAfterPassCallbackDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override
	{
		if (PassId == EPostProcessingPass::Tonemap
			&& bIsPassEnabled
			&& PendingComposites.Contains(InView.GetViewKey()))
		{
			InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
				this, &FGaussianSplatCompositeViewExtension::PostProcessAfterTonemap_RenderThread));
		}
	}

	virtual void PostRenderView_RenderThread(FRDGBuilder&, FSceneView& InView) override
	{
		PendingComposites.Remove(InView.GetViewKey());
	}

private:
	struct FPendingComposite
	{
		TRefCountPtr<IPooledRenderTarget> Texture;
		FIntRect SourceRect;
		FIntPoint SourceTextureExtent = FIntPoint::ZeroValue;
		NanoGS::Paged::FCaptureDrawTicketClaim DrawTicketClaim;
		TSharedPtr<FCaptureDrawPassEvidence, ESPMode::ThreadSafe> CaptureEvidence;
	};

	FScreenPassTexture PostProcessAfterTonemap_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& InView,
		const FPostProcessMaterialInputs& Inputs)
	{
		const FPendingComposite* Pending = PendingComposites.Find(InView.GetViewKey());
		if (!Pending || !Pending->Texture.IsValid())
		{
			return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		}
		const FPendingComposite PendingCopy = *Pending;

		FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(
			GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
		if (!SceneColor.IsValid())
		{
			NanoGS::Paged::FailCaptureDrawTicket_RenderThread(
				PendingCopy.DrawTicketClaim, TEXT("after_tonemap_scene_color_unavailable"));
			return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		}

		FScreenPassRenderTarget Output = Inputs.OverrideOutput;
		if (!Output.IsValid())
		{
			Output = FScreenPassRenderTarget::CreateFromInput(
				GraphBuilder, SceneColor, InView.GetOverwriteLoadAction(),
				TEXT("NanoGSAfterTonemapOutput"));
		}
		if (Output.Texture != SceneColor.Texture)
		{
			AddDrawTexturePass(GraphBuilder, InView, SceneColor, Output);
		}
		Output.LoadAction = ERenderTargetLoadAction::ELoad;

		FRDGTextureRef Intermediate = GraphBuilder.RegisterExternalTexture(
			Pending->Texture, TEXT("NanoGSIntermediateExternal"));
		FGaussianCompositePassParameters* CompositeParams =
			GraphBuilder.AllocParameters<FGaussianCompositePassParameters>();
		CompositeParams->IntermediateTexture = Intermediate;
		CompositeParams->RenderTargets[0] = Output.GetRenderTargetBinding();

		const FIntRect SourceRect = PendingCopy.SourceRect;
		const FIntPoint SourceTextureExtent = PendingCopy.SourceTextureExtent;
		const FIntRect DestinationRect = Output.ViewRect;
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GaussianSplat_CompositeAfterTonemap"),
			CompositeParams,
			ERDGPassFlags::Raster | ERDGPassFlags::NeverParallel,
			[Intermediate, SourceRect, SourceTextureExtent, DestinationRect,
			 DrawTicketClaim = PendingCopy.DrawTicketClaim,
			 CaptureEvidence = PendingCopy.CaptureEvidence]
			(FRHICommandListImmediate& RHICmdList)
			{
				const bool bCompositeSucceeded = FGaussianSplatRenderer::CompositeAfterTonemap(
					RHICmdList, Intermediate->GetRHI(), SourceTextureExtent,
					SourceRect, DestinationRect);
				if (!DrawTicketClaim.IsValid())
				{
					return;
				}
				if (!bCompositeSucceeded)
				{
					NanoGS::Paged::FailCaptureDrawTicket_RenderThread(
						DrawTicketClaim, TEXT("after_tonemap_composite_failed"));
					return;
				}
				if (!CaptureEvidence.IsValid() || !CaptureEvidence->bRendererCompleted)
				{
					NanoGS::Paged::FailCaptureDrawTicket_RenderThread(
						DrawTicketClaim, TEXT("after_tonemap_draw_evidence_missing"));
					return;
				}
				NanoGS::Paged::SucceedCaptureDrawTicket_RenderThread(
					DrawTicketClaim, CaptureEvidence->DrawPublication);
			});

		if (!bLoggedFirstComposite)
		{
			UE_LOG(LogTemp, Log,
				TEXT("NanoGS first after-Tonemap composite: source %dx%d @ (%d,%d), destination %dx%d @ (%d,%d)."),
				SourceRect.Width(), SourceRect.Height(), SourceRect.Min.X, SourceRect.Min.Y,
				DestinationRect.Width(), DestinationRect.Height(),
				DestinationRect.Min.X, DestinationRect.Min.Y);
			bLoggedFirstComposite = true;
		}

		return MoveTemp(Output);
	}

	TMap<uint32, FPendingComposite> PendingComposites;
	bool bLoggedFirstComposite = false;
};

void FNanoGSModule::StartupModule()
{
	// Register the shader directory so we can use our custom shaders
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("NanoGS"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/NanoGS"), PluginShaderDir);

	// Allocate the global accumulator (buffers are created lazily on first render)
	GlobalAccumulator = MakeUnique<FGaussianGlobalAccumulator>();

	if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSSensorOnly")))
	{
		CVarNanoGSSensorOnlyMode.AsVariable()->Set(1, ECVF_SetByCommandline);
		UE_LOG(LogTemp, Log, TEXT("NanoGS sensor-only mode enabled: main-view Gaussian work will be skipped."));
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSNoControlUI")))
	{
		CVarNanoGSControlUI.AsVariable()->Set(0, ECVF_SetByCommandline);
	}
	FString RuntimeUIMode;
	if (FParse::Value(FCommandLine::Get(), TEXT("RuntimeUI="), RuntimeUIMode))
	{
		const bool bShowNanoGS = RuntimeUIMode.Equals(TEXT("All"), ESearchCase::IgnoreCase)
			|| RuntimeUIMode.Equals(TEXT("NanoGS"), ESearchCase::IgnoreCase);
		CVarNanoGSControlUI.AsVariable()->Set(bShowNanoGS ? 1 : 0, ECVF_SetByCommandline);
	}
	bAutoHideControlUIInFullscreen =
		FParse::Param(FCommandLine::Get(), TEXT("AutoHideRuntimeUIInFullscreen"));

	// Register post-opaque render delegate for rendering
	PostOpaqueRenderDelegateHandle = GetRendererModuleRef().RegisterPostOpaqueRenderDelegate(
		FPostOpaqueRenderDelegate::CreateRaw(this, &FNanoGSModule::OnPostOpaqueRender_RenderThread));

	// Defer view extension creation until GEngine is valid
	// (StartupModule runs before GEngine is initialized, causing an ensure failure)
	PostEngineInitDelegateHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FNanoGSModule::OnPostEngineInit);
	WorldInitializedActorsDelegateHandle = FWorldDelegates::OnWorldInitializedActors.AddRaw(
		this, &FNanoGSModule::OnWorldInitializedActors);
	TickDelegateHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FNanoGSModule::TickControlUI), 0.25f);

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatting module started. Shader directory: %s"), *PluginShaderDir);
}

void FNanoGSModule::OnWorldInitializedActors(const FActorsInitializedParams& Params)
{
	NanoGS::RuntimeScene::ApplyConfiguredScene(Params.World);
}

bool FNanoGSModule::TickControlUI(float)
{
	if (IsRunningCommandlet() || FApp::IsUnattended() || GEngine == nullptr)
		return true;

	if (CVarNanoGSControlUI.GetValueOnGameThread() == 0)
	{
		if (ControlWidget.IsValid())
			ControlWidget->RemoveFromParent();
		ControlWidget.Reset();
		ControlWidgetWorld.Reset();
		return true;
	}

	UWorld* InteractiveWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World != nullptr && World->IsGameWorld() && World->GetFirstPlayerController() != nullptr)
		{
			InteractiveWorld = World;
			break;
		}
	}
	if (InteractiveWorld == nullptr)
		return true;

	if (ControlWidget.IsValid() && ControlWidgetWorld.Get() != InteractiveWorld)
	{
		ControlWidget->RemoveFromParent();
		ControlWidget.Reset();
		ControlWidgetWorld.Reset();
	}
	if (ControlWidget.IsValid())
	{
		const bool bShouldShow = !bAutoHideControlUIInFullscreen
			|| GEngine->GameViewport == nullptr
			|| !GEngine->GameViewport->IsFullScreenViewport();
		if (bShouldShow && !ControlWidget->IsInViewport())
			ControlWidget->AddToViewport(45);
		else if (!bShouldShow && ControlWidget->IsInViewport())
			ControlWidget->RemoveFromParent();
		return true;
	}

	APlayerController* PlayerController = InteractiveWorld->GetFirstPlayerController();
	UNanoGSControlWidget* Widget = CreateWidget<UNanoGSControlWidget>(
		PlayerController, UNanoGSControlWidget::StaticClass());
	if (Widget == nullptr)
		return true;

	if (!bAutoHideControlUIInFullscreen || GEngine->GameViewport == nullptr
		|| !GEngine->GameViewport->IsFullScreenViewport())
		Widget->AddToViewport(45);
	ControlWidget = Widget;
	ControlWidgetWorld = InteractiveWorld;
	PlayerController->bShowMouseCursor = true;
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);
	UE_LOG(LogTemp, Display, TEXT("NanoGS runtime control UI created"));
	return true;
}

void FNanoGSModule::OnPostEngineInit()
{
	// Skip view extension creation during cooking/commandlet — GEngine is null in those contexts
	if (IsRunningCommandlet() || !GEngine)
	{
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: Skipping ViewExtension creation (commandlet/cook mode)"));
		return;
	}

	// Now GEngine is valid — safe to create the view extension
	ViewExtension = FSceneViewExtensions::NewExtension<FGaussianSplatViewExtension>();
	CompositeViewExtension =
		FSceneViewExtensions::NewExtension<FGaussianSplatCompositeViewExtension>();

	if (!ViewExtension.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("GaussianSplatting: Failed to create ViewExtension!"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: ViewExtension created successfully (deferred init)"));
	}
	if (!CompositeViewExtension.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("GaussianSplatting: Failed to create after-Tonemap ViewExtension!"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatting: After-Tonemap ViewExtension created successfully"));
	}
}

void FNanoGSModule::OnPostOpaqueRender_RenderThread(FPostOpaqueRenderParameters& Parameters)
{
	const FSceneView* SceneView = reinterpret_cast<const FSceneView*>(Parameters.View);
	const NanoGS::Paged::FCaptureDrawTicketClaim DrawTicketClaim = SceneView != nullptr
		? NanoGS::Paged::ClaimCaptureDrawTicket_RenderThread(*SceneView)
		: NanoGS::Paged::FCaptureDrawTicketClaim();
	const bool bCaptureFrontier = SceneView != nullptr && SceneView->bIsSceneCapture;
	uint64 CaptureViewIdentity = DrawTicketClaim.ComponentIdentity;
	if (bCaptureFrontier && CaptureViewIdentity == 0 && SceneView->Family != nullptr)
	{
		CaptureViewIdentity = static_cast<uint64>(
			reinterpret_cast<UPTRINT>(SceneView->Family->RenderTarget));
		if (CaptureViewIdentity == 0)
		{
			CaptureViewIdentity = static_cast<uint64>(SceneView->GetViewKey()) + 1ull;
		}
	}
	auto FailDrawTicket = [&DrawTicketClaim](const TCHAR* Reason)
	{
		NanoGS::Paged::FailCaptureDrawTicket_RenderThread(DrawTicketClaim, Reason);
	};

	FGaussianSplatViewExtension* Ext = FGaussianSplatViewExtension::Get();
	if (!Ext || !Parameters.GraphBuilder || !Parameters.View)
	{
		FailDrawTicket(TEXT("post_opaque_callback_invalid"));
		return;
	}
	if (SceneView->Family == nullptr)
	{
		FailDrawTicket(TEXT("view_family_unavailable"));
		return;
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("NanoGSTreePerf")))
	{
		static TSet<uint64> LoggedViewTargets;
		const uint64 RenderTargetIdentity = static_cast<uint64>(
			reinterpret_cast<UPTRINT>(SceneView->Family->RenderTarget));
		const uint64 LogIdentity = RenderTargetIdentity ^
			(SceneView->bIsSceneCapture ? 0x8000000000000000ull : 0ull);
		if (!LoggedViewTargets.Contains(LogIdentity))
		{
			LoggedViewTargets.Add(LogIdentity);
			UE_LOG(LogTemp, Display,
				TEXT("NanoGS view context: capture=%d ticket=%d render-target=%llu size=%dx%d"),
				SceneView->bIsSceneCapture ? 1 : 0, DrawTicketClaim.IsValid() ? 1 : 0,
				static_cast<unsigned long long>(RenderTargetIdentity),
				SceneView->UnscaledViewRect.Width(), SceneView->UnscaledViewRect.Height());
		}
	}

	FRDGBuilder& GraphBuilder = *Parameters.GraphBuilder;
	if (CVarNanoGSSensorOnlyMode.GetValueOnRenderThread() != 0 && !SceneView->bIsSceneCapture)
	{
		FailDrawTicket(TEXT("sensor_only_view_rejected"));
		return;
	}

	TArray<FGaussianSplatSceneProxy*> Proxies;
	Ext->GetRegisteredProxies(Proxies);

	if (Proxies.Num() == 0)
	{
		FailDrawTicket(TEXT("no_registered_proxies"));
		return;
	}

	// Sort proxies back-to-front for correct depth ordering
	if (Proxies.Num() > 1)
	{
		FVector CameraPosition = SceneView->ViewMatrices.GetViewOrigin();
		Proxies.Sort([CameraPosition](const FGaussianSplatSceneProxy& A, const FGaussianSplatSceneProxy& B)
		{
			float DistA = FVector::DistSquared(CameraPosition, A.GetBounds().Origin);
			float DistB = FVector::DistSquared(CameraPosition, B.GetBounds().Origin);
			return DistA > DistB;
		});
	}

	FRDGTexture* ColorTexture = Parameters.ColorTexture;
	FRDGTexture* DepthTexture = Parameters.DepthTexture;
	FRDGTexture* VelocityTexture = Parameters.VelocityTexture;
	if (!ColorTexture)
	{
		FailDrawTicket(TEXT("scene_color_unavailable"));
		return;
	}

	int32 DebugMode = CVarShowClusterBounds.GetValueOnRenderThread();

	// Create intermediate render target for sRGB-space alpha blending.
	// Gaussian splatting trains in sRGB space, so blending must happen in sRGB space
	// to produce correct colors. After compositing, we convert sRGB→linear for SceneColor.
	FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
		ColorTexture->Desc.Extent,
		PF_FloatRGBA,  // Need alpha channel for accumulation tracking
		FClearValueBinding(FLinearColor::Transparent),
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTexture* IntermediateTexture = GraphBuilder.CreateTexture(IntermediateDesc, TEXT("GaussianSplatIntermediateRT"));

	// Pass 1: Render splats to intermediate RT (sRGB blending)
	FRenderTargetParameters* Pass1Parameters = GraphBuilder.AllocParameters<FRenderTargetParameters>();
	Pass1Parameters->RenderTargets[0] = FRenderTargetBinding(IntermediateTexture, ERenderTargetLoadAction::EClear);
	// Bind velocity texture for TAA/TSR motion vector output
	if (VelocityTexture)
	{
		Pass1Parameters->RenderTargets[1] = FRenderTargetBinding(VelocityTexture, ERenderTargetLoadAction::ELoad);
	}
	if (DepthTexture)
	{
		// Enable depth writes so TSR/TAA can properly detect disocclusion
		// Splats will write their center depth for each rendered pixel
		Pass1Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			DepthTexture,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilWrite
		);
	}

	if (!GlobalAccumulator.IsValid())
	{
		FailDrawTicket(TEXT("global_accumulator_unavailable"));
		return;
	}

	//------------------------------------------------------------------
	// GLOBAL ACCUMULATOR PATH: Phase 1 (per-proxy CalcViewData) +
	// Phase 2 (single CalcDistances + RadixSort) + single DrawSplats
	//------------------------------------------------------------------

		// Build the list of visible proxies and compute total splat count (CPU-side)
		struct FProxyRenderInfo
		{
			FGaussianSplatSceneProxy* Proxy;
			FMatrix LocalToWorld;
				uint32 GlobalBaseOffset;
				bool bUseLODRendering;
				float DistanceToCamera;  // For budget priority sorting (closer = higher priority)
				uint32 ResidentGeneration; // Invalidates static-view cache when this view frontier changes.
				uint32 ActiveSplatCount;
				bool bCaptureFrontier;
		};
		TArray<FProxyRenderInfo> VisibleProxies;
		uint64 TotalSplatCount64 = 0;
			bool bAllNanite = true;  // True if every visible proxy supports compaction
		bool bSawPagedSourceForScene = false;
		bool bPagedRenderResourceUnavailable = false;

			FVector CameraLocation = SceneView->ViewLocation;
			const uint32 CurrentEnableHeuristicLOD =
				CVarEnableHeuristicLOD.GetValueOnRenderThread() != 0 ? 1u : 0u;

		for (FGaussianSplatSceneProxy* Proxy : Proxies)
		{
			if (!Proxy) continue;
			if (&Proxy->GetScene() != SceneView->Family->Scene) continue;
			const bool bPagedSource = Proxy->IsPagedSource();
			bSawPagedSourceForScene |= bPagedSource;
			if (!Proxy->IsShown(SceneView)) continue;

			const FBoxSphereBounds& Bounds = Proxy->GetBounds();
			FGaussianSplatGPUResources* GPUResources = Proxy->GetGPUResources();
			if (!GPUResources || !GPUResources->IsValid())
			{
				bPagedRenderResourceUnavailable |= bPagedSource;
				continue;
			}
			const bool bBoundedPagedStreaming =
				bPagedSource && GPUResources->IsBoundedPagedStreamingEnabled();
			// Bounded Page streaming needs demand even before its first complete
			// frontier has a non-zero active count. Submit demand before broadphase;
			// CalcBounds now gives every paged source a common 4-sigma, abs(SplatScale)-
			// aware bound, so both bounded and default all-resident avoid 2-sigma holes
			// without forcing an off-screen 23M-splat proxy through the renderer.
			if (bBoundedPagedStreaming)
			{
				Proxy->RequestPagedExactHQForViewFamily_RenderThread(
					*SceneView->Family,
					CaptureViewIdentity);
				GPUResources->RefreshPagedFrontierBinding_RenderThread(bCaptureFrontier);
			}
			if (!SceneView->ViewFrustum.IntersectBox(Bounds.Origin, Bounds.BoxExtent))
			{
				continue;
			}
			const int32 ViewSplatCount = Proxy->GetSplatCountForView(bCaptureFrontier);
			if (ViewSplatCount <= 0) continue;

			FProxyRenderInfo Info;
			Info.Proxy = Proxy;
			Info.LocalToWorld = Proxy->GetLocalToWorld();
			Info.GlobalBaseOffset = 0;  // Will be computed after sorting
				Info.bUseLODRendering = CurrentEnableHeuristicLOD != 0 &&
					GPUResources->bEnableNanite && GPUResources->bHasLODSplats;
			Info.DistanceToCamera = FVector::Dist(Bounds.Origin, CameraLocation);
			Info.ResidentGeneration = GPUResources->GetResidentGeneration(bCaptureFrontier);
			Info.ActiveSplatCount = static_cast<uint32>(ViewSplatCount);
			Info.bCaptureFrontier = bCaptureFrontier;
			VisibleProxies.Add(Info);

			// All proxies must support Nanite compaction for the fast global path
			if (!GPUResources->bEnableNanite || !GPUResources->bHasClusterData || !GPUResources->bSupportsCompaction)
			{
				bAllNanite = false;
			}
		}

		// Sort by distance: closer proxies first (get budget priority when MaxRenderBudget is active)
		VisibleProxies.Sort([](const FProxyRenderInfo& A, const FProxyRenderInfo& B)
		{
			return A.DistanceToCamera < B.DistanceToCamera;
		});

		// Compute GlobalBaseOffset and TotalSplatCount after sorting
		for (FProxyRenderInfo& Info : VisibleProxies)
		{
			const uint64 ProxySplatCount = static_cast<uint64>(Info.ActiveSplatCount);
			if (TotalSplatCount64 > static_cast<uint64>(MAX_uint32) - ProxySplatCount)
			{
					UE_LOG(LogTemp, Error,
						TEXT("NanoGS: Visible splat total exceeds the uint32 shader-index limit while adding a proxy (%llu + %llu). Split the scene into streamed spatial pages."),
						static_cast<unsigned long long>(TotalSplatCount64),
						static_cast<unsigned long long>(ProxySplatCount));
					FailDrawTicket(TEXT("visible_splat_count_overflow"));
					return;
			}
			Info.GlobalBaseOffset = static_cast<uint32>(TotalSplatCount64);
			TotalSplatCount64 += ProxySplatCount;
		}
		const uint32 TotalSplatCount = static_cast<uint32>(TotalSplatCount64);

		// Safety: global accumulator only supports up to MAX_PROXY_COUNT proxies
		if ((uint32)VisibleProxies.Num() > FGaussianGlobalAccumulator::MAX_PROXY_COUNT)
		{
			bAllNanite = false;
		}

			if (TotalSplatCount == 0)
			{
				if (DrawTicketClaim.IsValid())
				{
					if (!bSawPagedSourceForScene)
					{
						FailDrawTicket(TEXT("paged_source_missing_from_scene"));
					}
					else if (bPagedRenderResourceUnavailable)
					{
						FailDrawTicket(TEXT("paged_render_resource_unavailable"));
					}
					else
					{
						// A completed broadphase with no participating splats is a valid
						// background capture (for example a bounded empty frontier).
						NanoGS::Paged::SucceedCaptureDrawTicket_RenderThread(
							DrawTicketClaim,
							NanoGS::Paged::QueryCaptureReadiness(SceneView->Family->Scene));
					}
				}
				return;
			}
			if (bPagedRenderResourceUnavailable)
			{
				// Keep ordinary scene rendering alive, but the strict Page capture may
				// not use pixels produced while one shown Page resource was unavailable.
				FailDrawTicket(TEXT("paged_render_resource_unavailable"));
			}

		// Check camera-static skip: if nothing has changed, skip Phase 1+2 and reuse cached sort
		// Use ProjectionNoAAMatrix to ignore TSR/TAA per-frame jitter that changes every frame
		FMatrix CurrentVP = SceneView->ViewMatrices.GetViewMatrix() * SceneView->ViewMatrices.GetProjectionNoAAMatrix();
		// ViewRect size is not encoded in the projection matrix. A same-aspect viewport
		// resize or high-resolution SceneCapture must still rebuild per-splat pixel
		// filtering, minimum-size culling and viewport-dependent axis caps.
		const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(*SceneView);
		const FIntPoint CurrentViewRectSize = ViewInfo.ViewRect.Size();
		int32 CurrentDebugMode = DebugMode;
		int32 CurrentDebugForceLODLevel = CVarDebugForceLODLevel.GetValueOnRenderThread();
		int32 CurrentUseFloat32Position = CVarUseFloat32Position.GetValueOnRenderThread() != 0 ? 1 : 0;
		int32 CurrentUseFloat32RotationScale = CVarUseFloat32RotationScale.GetValueOnRenderThread() != 0 ? 1 : 0;
		uint32 CurrentRasterProfile = static_cast<uint32>(FMath::Clamp(CVarRasterProfile.GetValueOnRenderThread(), 0, 1));
		uint32 CurrentFuseDepthKeys = CVarFuseDepthKeys.GetValueOnRenderThread() != 0 ? 1u : 0u;
		uint32 CurrentUsePublishedSort =
			CVarInteractivePublishedSort.GetValueOnRenderThread() != 0 && !bAllNanite ? 1u : 0u;
		const uint32 InteractiveSortEveryN = static_cast<uint32>(FMath::Clamp(
			CVarInteractiveSortEveryN.GetValueOnRenderThread(), 1, 8));
		const double InteractiveSortSettleSeconds = static_cast<double>(FMath::Clamp(
			CVarInteractiveSortSettleMilliseconds.GetValueOnRenderThread(), 0.0f, 1000.0f)) /
			1000.0;
		int32 BudgetVal = CVarMaxRenderBudget.GetValueOnRenderThread();
		uint32 CurrentMaxRenderBudget = (BudgetVal > 0) ? (uint32)BudgetVal : 0;
		if (CurrentDebugForceLODLevel >= 0)
		{
			CurrentMaxRenderBudget = 0;
		}

		const double InteractiveNowSeconds = FPlatformTime::Seconds();
		const bool bViewChangedSinceLastFrame =
			!GlobalAccumulator->bHasLastObservedViewProjection ||
			!GlobalAccumulator->LastObservedViewProjectionMatrix.Equals(CurrentVP, 0.0f);
		GlobalAccumulator->LastObservedViewProjectionMatrix = CurrentVP;
		GlobalAccumulator->bHasLastObservedViewProjection = true;
		if (bViewChangedSinceLastFrame)
		{
			++GlobalAccumulator->InteractiveViewChangeSequence;
			GlobalAccumulator->LastInteractiveViewChangeSeconds = InteractiveNowSeconds;
		}
		const bool bPublishedSortSettleDue = CurrentUsePublishedSort != 0u &&
			GlobalAccumulator->bPublishedSortPendingExact &&
			!bViewChangedSinceLastFrame &&
			GlobalAccumulator->LastInteractiveViewChangeSeconds > 0.0 &&
			InteractiveNowSeconds - GlobalAccumulator->LastInteractiveViewChangeSeconds >=
				InteractiveSortSettleSeconds;

		bool bSortMappingCompatible = GlobalAccumulator->bHasCachedSortData &&
			GlobalAccumulator->CachedTotalSplatCount == TotalSplatCount &&
			GlobalAccumulator->CachedViewRectSize == CurrentViewRectSize &&
			GlobalAccumulator->CachedMaxRenderBudget == CurrentMaxRenderBudget &&
			GlobalAccumulator->CachedRasterProfile == CurrentRasterProfile &&
			GlobalAccumulator->CachedFuseDepthKeys == CurrentFuseDepthKeys &&
			GlobalAccumulator->CachedEnableHeuristicLOD == CurrentEnableHeuristicLOD &&
			GlobalAccumulator->CachedUsePublishedSort == CurrentUsePublishedSort &&
			(CurrentUsePublishedSort == 0u || GlobalAccumulator->bHasPublishedSortKeys);

		if (bSortMappingCompatible)
		{
			for (const FProxyRenderInfo& Info : VisibleProxies)
			{
				FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
					// Match DispatchClusterCulling's clamp exactly. Otherwise values in
					// [0.001, 0.1) compare as one cache key even though the shader sees
					// different thresholds, leaving stale LOD selections for a static view.
					float ProxyErrorThreshold = FMath::Max(0.001f, Info.Proxy->GetLODErrorThreshold());
				if (!GPUResources->bHasCachedSortData ||
					GPUResources->CachedViewRectSize != CurrentViewRectSize ||
					!GPUResources->CachedLocalToWorld.Equals(Info.LocalToWorld, 0.0f) ||
					GPUResources->CachedSplatScale != Info.Proxy->GetSplatScale() ||
					GPUResources->CachedErrorThreshold != ProxyErrorThreshold ||
					GPUResources->CachedDebugMode != CurrentDebugMode ||
					GPUResources->CachedDebugForceLODLevel != CurrentDebugForceLODLevel ||
					GPUResources->CachedUseFloat32Position != CurrentUseFloat32Position ||
					GPUResources->CachedUseFloat32RotationScale != CurrentUseFloat32RotationScale ||
					GPUResources->CachedSHOrder != Info.Proxy->GetSHOrder() ||
					GPUResources->CachedResidentGeneration != GPUResources->GetResidentGeneration())
				{
					bSortMappingCompatible = false;
					break;
				}
			}
		}

		bool bCanSkip = bSortMappingCompatible &&
			GlobalAccumulator->CachedViewProjectionMatrix.Equals(CurrentVP, 0.0f);
		if (bCanSkip)
		{
			for (const FProxyRenderInfo& Info : VisibleProxies)
			{
				const FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
				if (GPUResources == nullptr ||
					!GPUResources->CachedViewProjectionMatrix.Equals(CurrentVP, 0.0f))
				{
					bCanSkip = false;
					break;
				}
			}
		}
		if (CVarForceViewRecompute.GetValueOnRenderThread() != 0)
		{
			bCanSkip = false;
			bSortMappingCompatible = false;
		}
		if (bPublishedSortSettleDue)
		{
			bCanSkip = false;
		}

		const bool bDeferPublishedSort = CurrentUsePublishedSort != 0u &&
			bSortMappingCompatible && bViewChangedSinceLastFrame &&
			(GlobalAccumulator->InteractiveViewChangeSequence % InteractiveSortEveryN) != 0u;

		if (!bCaptureFrontier &&
			FParse::Param(FCommandLine::Get(), TEXT("NanoGSInteractivePerf")))
		{
			GInteractiveViewportTelemetry.Record(
				CurrentVP, bCanSkip, bDeferPublishedSort, TotalSplatCount);
		}

		// Grab index buffer from the first proxy (all proxies use identical quad geometry)
		FBufferRHIRef SharedIndexBuffer;
		if (VisibleProxies.Num() > 0)
		{
			FGaussianSplatGPUResources* FirstRes = VisibleProxies[0].Proxy->GetGPUResources();
			SharedIndexBuffer = FirstRes ? FirstRes->IndexBuffer : FBufferRHIRef();
		}

				FGaussianGlobalAccumulator* RawAccumulator = GlobalAccumulator.Get();
			const TSharedRef<FCaptureDrawPassEvidence, ESPMode::ThreadSafe> CaptureEvidence =
				MakeShared<FCaptureDrawPassEvidence, ESPMode::ThreadSafe>();

			// Captured into the render pass after participating in cache invalidation.
		const uint32 MaxRenderBudget = CurrentMaxRenderBudget;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GaussianSplat_RenderToIntermediate"),
			Pass1Parameters,
			ERDGPassFlags::Raster | ERDGPassFlags::Compute | ERDGPassFlags::SkipRenderPass,
			[SceneView, VisibleProxies, TotalSplatCount, bCanSkip, bDeferPublishedSort,
			 bAllNanite, RawAccumulator,
			 SharedIndexBuffer, CurrentVP, CurrentViewRectSize, CurrentDebugMode,
					 CurrentDebugForceLODLevel, CurrentUseFloat32Position, CurrentUseFloat32RotationScale,
					 CurrentRasterProfile, CurrentFuseDepthKeys, CurrentUsePublishedSort,
					 CurrentEnableHeuristicLOD, DebugMode, MaxRenderBudget,
				 Pass1Parameters, DrawTicketClaim, CaptureEvidence](FRHICommandListImmediate& RHICmdList)
			{
					auto FailPass = [&DrawTicketClaim](const TCHAR* Reason)
					{
						NanoGS::Paged::FailCaptureDrawTicket_RenderThread(DrawTicketClaim, Reason);
					};
					if (!SceneView)
					{
						FailPass(TEXT("rdg_scene_view_unavailable"));
						return;
					}
				SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRendering_Global);

				// SAFETY CHECK: Re-validate all proxies before rendering.
				// Proxies may have been destroyed between when we built VisibleProxies
				// and when this lambda executes (RDG deferred execution).
				// We need to rebuild the list with only valid proxies.
					TArray<FProxyRenderInfo> ValidProxies;
					ValidProxies.Reserve(VisibleProxies.Num());
					uint64 NewTotalSplatCount64 = 0;
					bool bCanSkipAdjusted = bCanSkip;
					bool bResidentGenerationChanged = false;
					uint32 PreviousResidentGeneration = MAX_uint32;
					uint32 ProfiledResidentGeneration = MAX_uint32;

				for (const auto& Info : VisibleProxies)
				{
					// Check if proxy is still valid (not destroyed or pending destruction)
					if (Info.Proxy && Info.Proxy->IsValidForRendering())
					{
						FGaussianSplatGPUResources* CurrentResources = Info.Proxy->GetGPUResources();
						if (!CurrentResources)
						{
							continue;
						}
						CurrentResources->RefreshPagedFrontierBinding_RenderThread(Info.bCaptureFrontier);
						const int32 ProxySplatCount = static_cast<int32>(Info.ActiveSplatCount);
						if (ProxySplatCount <= 0)
						{
							continue;
						}
						const uint64 ProxySplatCount64 = static_cast<uint64>(ProxySplatCount);
						if (NewTotalSplatCount64 > static_cast<uint64>(MAX_uint32) - ProxySplatCount64)
						{
							UE_LOG(LogTemp, Error,
								TEXT("NanoGS: Deferred visible splat total exceeds the uint32 shader-index limit (%llu + %llu); skipping this view to prevent offset wrap."),
								static_cast<unsigned long long>(NewTotalSplatCount64),
									static_cast<unsigned long long>(ProxySplatCount64));
								RawAccumulator->bHasCachedSortData = false;
								FailPass(TEXT("deferred_splat_count_overflow"));
								return;
						}
							FProxyRenderInfo ValidInfo = Info;
							const uint32 CurrentResidentGeneration =
								CurrentResources->GetResidentGeneration(Info.bCaptureFrontier);
							if (CurrentResources->CachedResidentGeneration != CurrentResidentGeneration)
							{
								bResidentGenerationChanged = true;
								PreviousResidentGeneration = CurrentResources->CachedResidentGeneration;
								ProfiledResidentGeneration = CurrentResidentGeneration;
							}
							if (CurrentResidentGeneration != Info.ResidentGeneration)
							{
								bCanSkipAdjusted = false;
								RawAccumulator->bHasCachedSortData = false;
							}
							ValidInfo.ResidentGeneration = CurrentResidentGeneration;
							ValidInfo.GlobalBaseOffset = static_cast<uint32>(NewTotalSplatCount64);
						NewTotalSplatCount64 += ProxySplatCount64;
						ValidProxies.Add(ValidInfo);
					}
				}
					const uint32 NewTotalSplatCount = static_cast<uint32>(NewTotalSplatCount64);

				// If no valid proxies remain, skip rendering entirely
					if (ValidProxies.Num() == 0 || NewTotalSplatCount == 0)
				{
						FailPass(TEXT("no_valid_visible_proxies"));
						return;
				}

					// Invalidate cache skip if the proxy list changed (some proxies were destroyed)
					// This ensures we don't use stale cached data when the scene has changed
					if (ValidProxies.Num() != VisibleProxies.Num() || NewTotalSplatCount != TotalSplatCount)
				{
					bCanSkipAdjusted = false;
					// Also invalidate the global accumulator cache since proxy set changed
					RawAccumulator->bHasCachedSortData = false;
				}

				// Ensure global buffers are large enough for all splats
					if (!RawAccumulator->ResizeIfNeeded(RHICmdList, NewTotalSplatCount64, MaxRenderBudget))
					{
						FailPass(TEXT("global_accumulator_resize_failed"));
						return;
				}
				// ResizeIfNeeded releases and recreates the backing buffers. Never reuse
				// a cache decision made before that allocation change.
					if (!RawAccumulator->bHasCachedSortData)
					{
						bCanSkipAdjusted = false;
					}
					RawAccumulator->bUsePublishedSortKeysForDraw =
						CurrentUsePublishedSort != 0u;
					if (CurrentUsePublishedSort == 0u)
					{
						RawAccumulator->bPublishedSortPendingExact = false;
					}

					// Exact-HQ Page sources never silently render a prefix. A user-supplied
					// global budget that cannot contain the published active set is a hard
					// capacity failure; bounded page residency must reduce the active set
					// before it is published.
					if (MaxRenderBudget > 0 && NewTotalSplatCount > MaxRenderBudget)
					{
						for (const FProxyRenderInfo& Info : ValidProxies)
						{
							if (Info.Proxy->IsPagedSource())
							{
								UE_LOG(LogTemp, Error,
									TEXT("NanoGS Page Exact-HQ OutOfCapacity: %u active splats exceed gs.MaxRenderBudget=%u; refusing a lossy prefix."),
										NewTotalSplatCount, MaxRenderBudget);
									RawAccumulator->bHasCachedSortData = false;
									FailPass(TEXT("page_max_render_budget_exceeded"));
									return;
							}
						}
					}

					// Re-check if all valid proxies support Nanite compaction
				bool bAllValidNanite = true;
				for (const auto& Info : ValidProxies)
				{
					FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
					if (!GPUResources || !GPUResources->bEnableNanite || !GPUResources->bHasClusterData || !GPUResources->bSupportsCompaction)
					{
						bAllValidNanite = false;
						break;
					}
					}

				// Cap to MAX_PROXY_COUNT
					if ((uint32)ValidProxies.Num() > FGaussianGlobalAccumulator::MAX_PROXY_COUNT)
					{
						bAllValidNanite = false;
					}
					if (!SharedIndexBuffer.IsValid())
					{
						FailPass(TEXT("draw_index_buffer_unavailable"));
						return;
					}
					TShaderMapRef<FGaussianSplatVS> DrawVertexShader(
						GetGlobalShaderMap(GMaxRHIFeatureLevel));
					TShaderMapRef<FGaussianSplatPS> DrawPixelShader(
						GetGlobalShaderMap(GMaxRHIFeatureLevel));
					if (!DrawVertexShader.IsValid() || !DrawPixelShader.IsValid())
					{
						FailPass(TEXT("draw_shader_unavailable"));
						return;
					}
					if (bAllValidNanite)
				{
					//==================================================
					// GLOBAL + COMPACTION PATH
					// All proxies are Nanite-enabled: GPU compaction
					// reduces working set from TotalSplatCount → TotalVisible
					// (~140x reduction at LOD5 for a 719K-splat tile).
					//==================================================

					// Ensure fixed-size prefix-sum buffers exist (allocated once)
					RawAccumulator->EnsureCompactionBuffersAllocated(RHICmdList);

					if (!bCanSkipAdjusted)
					{
						// --------------------------------------------------
						// Phase 0: Per-proxy culling + compaction + indirect args
						// Early-out: skip proxies once cumulative splat count
						// exceeds MaxRenderBudget (CPU-side estimate using total
						// splat count as conservative upper bound for visible count).
						// Proxies are sorted by distance, so closer ones get priority.
						// --------------------------------------------------
						int32 NumProcessedProxies = 0;
						uint32 CumulativeSplatCount = 0;

						for (const auto& Info : ValidProxies)
						{
							// Budget early-out: if cumulative total already exceeds budget,
							// skip culling/compaction for remaining (farther) proxies
							if (MaxRenderBudget > 0 && CumulativeSplatCount >= MaxRenderBudget)
							{
								break;
							}

							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources) continue;  // Extra safety check
							int32 SplatCount = static_cast<int32>(Info.ActiveSplatCount);
							int32 OriginalSplatCount = SplatCount - GPUResources->LODSplatCount;

							// Cluster culling → fills ClusterVisibilityBitmap
							FGaussianSplatRenderer::DispatchClusterCulling(
								RHICmdList, *SceneView, GPUResources,
								Info.LocalToWorld, Info.Proxy->GetLODErrorThreshold(),
								Info.Proxy->GetSplatScale(), Info.bUseLODRendering);

							// Compact → fills CompactedSplatIndices + VisibleSplatCountBuffer
							FGaussianSplatRenderer::DispatchCompactSplats(
								RHICmdList, GPUResources,
								SplatCount, OriginalSplatCount, Info.bUseLODRendering);

							// PrepareIndirectArgs → fills IndirectDispatchArgsBuffer for CalcViewData
							FGaussianSplatRenderer::DispatchPrepareIndirectArgs(RHICmdList, GPUResources);

							CumulativeSplatCount += (uint32)SplatCount;
							NumProcessedProxies++;
						}

						// --------------------------------------------------
						// Phase 1: Gather visible counts + GPU prefix sum
						// Only gather from proxies that were actually processed
						// --------------------------------------------------
						for (int32 i = 0; i < NumProcessedProxies; i++)
						{
							FGaussianSplatGPUResources* GPUResources = ValidProxies[i].Proxy->GetGPUResources();
							if (!GPUResources) continue;  // Extra safety check
							FGaussianSplatRenderer::DispatchGatherVisibleCount(
								RHICmdList, GPUResources, RawAccumulator, i);
						}

						// Single 1-thread dispatch: computes prefix sums + writes all indirect args
						FGaussianSplatRenderer::DispatchPrefixSumVisibleCounts(
							RHICmdList, RawAccumulator, NumProcessedProxies, MaxRenderBudget);

						// --------------------------------------------------
						// Phase 2: Per-proxy CalcViewData → global buffer
						// (indirect dispatch, only visible splats per proxy)
						// Only process proxies that went through culling/compaction
						// --------------------------------------------------
						for (int32 i = 0; i < NumProcessedProxies; i++)
						{
							const auto& Info = ValidProxies[i];
							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources) continue;  // Extra safety check
							int32 SplatCount = static_cast<int32>(Info.ActiveSplatCount);
							int32 OriginalSplatCount = SplatCount - GPUResources->LODSplatCount;

							FGaussianSplatRenderer::DispatchCalcViewDataCompactedGlobal(
								RHICmdList, *SceneView, GPUResources,
								Info.LocalToWorld,
								SplatCount,
								OriginalSplatCount,
								Info.Proxy->GetSHOrder(),
								Info.Proxy->GetOpacityScale(),
								Info.Proxy->GetSplatScale(),
								i,
								RawAccumulator,
								MaxRenderBudget);
						}

						// --------------------------------------------------
						// Phase 3: Single global RadixSort. CalcViewData writes the
						// identical depth/index keys directly, avoiding a second
						// full read of the 64-byte ViewData records.
						// --------------------------------------------------
						if (CurrentFuseDepthKeys == 0u)
						{
							FGaussianSplatRenderer::DispatchCalcDistancesGlobalIndirect(RHICmdList, RawAccumulator);
						}
						else
						{
							// CalcViewData wrote the primary radix inputs as UAVs.  The
							// fused path has no intervening CalcDistances dispatch, so make
							// those writes explicitly visible before radix reads them.  This
							// is a same-state UAV barrier and does not alter the key values.
							RHICmdList.Transition(FRHITransitionInfo(
								RawAccumulator->GlobalSortDistanceBuffer,
								ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
							RHICmdList.Transition(FRHITransitionInfo(
								RawAccumulator->GlobalSortKeysBuffer,
								ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
						}
						FGaussianSplatRenderer::DispatchRadixSortGlobalIndirect(RHICmdList, RawAccumulator);

						// Update caches — only for processed proxies
						RawAccumulator->bHasCachedSortData = true;
						RawAccumulator->CachedTotalSplatCount = NewTotalSplatCount;
						RawAccumulator->CachedViewProjectionMatrix = CurrentVP;
						RawAccumulator->CachedViewRectSize = CurrentViewRectSize;
						RawAccumulator->CachedMaxRenderBudget = MaxRenderBudget;
						RawAccumulator->CachedRasterProfile = CurrentRasterProfile;
						RawAccumulator->CachedFuseDepthKeys = CurrentFuseDepthKeys;
						RawAccumulator->CachedEnableHeuristicLOD = CurrentEnableHeuristicLOD;
						RawAccumulator->CachedUsePublishedSort = CurrentUsePublishedSort;

						for (int32 i = 0; i < ValidProxies.Num(); i++)
						{
							FGaussianSplatGPUResources* GPUResources = ValidProxies[i].Proxy->GetGPUResources();
							if (!GPUResources) continue;

							if (i < NumProcessedProxies)
							{
								const auto& Info = ValidProxies[i];
								GPUResources->CachedViewProjectionMatrix = CurrentVP;
								GPUResources->CachedViewRectSize = CurrentViewRectSize;
								GPUResources->CachedLocalToWorld = Info.LocalToWorld;
								GPUResources->CachedOpacityScale = Info.Proxy->GetOpacityScale();
								GPUResources->CachedSplatScale = Info.Proxy->GetSplatScale();
									GPUResources->CachedErrorThreshold = FMath::Max(0.001f, Info.Proxy->GetLODErrorThreshold());
								GPUResources->CachedDebugMode = CurrentDebugMode;
								GPUResources->CachedDebugForceLODLevel = CurrentDebugForceLODLevel;
								GPUResources->CachedUseFloat32Position = CurrentUseFloat32Position;
								GPUResources->CachedUseFloat32RotationScale = CurrentUseFloat32RotationScale;
									GPUResources->CachedSHOrder = Info.Proxy->GetSHOrder();
									GPUResources->CachedResidentGeneration = Info.ResidentGeneration;
									GPUResources->bHasCachedSortData = true;
							}
							else
							{
								// Invalidate cache for budget-skipped proxies so they
								// don't block the camera-static skip check
								GPUResources->bHasCachedSortData = false;
							}
						}
					}

					// Single draw call — instance count from GlobalDrawIndirectArgsBuffer
					RHICmdList.BeginRenderPass(
						GetRenderPassInfo(Pass1Parameters),
						TEXT("GaussianSplat_RenderToIntermediate"));
						FGaussianSplatRenderer::DrawSplatsGlobalIndirect(
							RHICmdList, *SceneView, RawAccumulator, SharedIndexBuffer, DebugMode,
							ValidProxies[0].Proxy->GetOpacityScale());
						RHICmdList.EndRenderPass();
						for (const FProxyRenderInfo& Info : ValidProxies)
						{
							if (FGaussianSplatGPUResources* Resources = Info.Proxy->GetGPUResources())
							{
								Resources->NotifyPagedFrontierDrawn_RenderThread(RHICmdList, Info.bCaptureFrontier);
							}
						}
				}
				else
				{
					//==================================================
					// NON-COMPACTION GLOBAL PATH (fallback)
					// Not all proxies are Nanite-enabled.
					// Sorts all NewTotalSplatCount splats (no compaction benefit).
					// Still provides correct cross-tile alpha blending.
					//==================================================

					// Cap splat count to render budget (CPU-side enforcement)
					uint32 CappedTotalSplatCount = NewTotalSplatCount;
					if (MaxRenderBudget > 0 && CappedTotalSplatCount > MaxRenderBudget)
					{
						CappedTotalSplatCount = MaxRenderBudget;
					}
					const uint32 PassTimingMinSplats = static_cast<uint32>(FMath::Max(
						1, CVarNanoGSPassTimingMinSplats.GetValueOnRenderThread()));
					const bool bPassTimingRequested =
						CVarNanoGSPassTimingOneShot.GetValueOnRenderThread() != 0;
					if (!bPassTimingRequested)
					{
						RawAccumulator->bPassTimingOneShotConsumed = false;
					}
					const bool bTimePasses =
						bPassTimingRequested && !RawAccumulator->bPassTimingOneShotConsumed &&
						bResidentGenerationChanged && !bCanSkipAdjusted &&
						CappedTotalSplatCount >= PassTimingMinSplats;
					double PreDrainMilliseconds = 0.0;
					double CalcViewMilliseconds = 0.0;
					double RadixMilliseconds = 0.0;
					double DrawMilliseconds = 0.0;
					double PassTimingStageStartSeconds = 0.0;
					if (bTimePasses)
					{
						const double DrainStartSeconds = FPlatformTime::Seconds();
						RHICmdList.SubmitAndBlockUntilGPUIdle();
						PreDrainMilliseconds =
							(FPlatformTime::Seconds() - DrainStartSeconds) * 1000.0;
						PassTimingStageStartSeconds = FPlatformTime::Seconds();
					}

					if (!bCanSkipAdjusted)
					{
						// --------------------------------------------------
						// Phase 1: Per-proxy ClusterCulling + CalcViewData
						// --------------------------------------------------
						for (const auto& Info : ValidProxies)
						{
							// Stop once this proxy's prefix begins beyond the allocated/output cap.
							if (Info.GlobalBaseOffset >= CappedTotalSplatCount)
							{
								break;  // All subsequent proxies also exceed the budget
							}
							const uint32 RemainingOutputCapacity = CappedTotalSplatCount - Info.GlobalBaseOffset;

							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources)
							{
								RawAccumulator->bHasCachedSortData = false;
								FailPass(TEXT("calc_view_data_resource_unavailable"));
								return;
							}

							// Cluster culling for Nanite-enabled proxies
							if (GPUResources->bEnableNanite && GPUResources->bHasClusterData)
							{
									if (FGaussianSplatRenderer::DispatchClusterCulling(
										RHICmdList, *SceneView, GPUResources,
										Info.LocalToWorld, Info.Proxy->GetLODErrorThreshold(),
										Info.Proxy->GetSplatScale(), Info.bUseLODRendering) <= 0)
									{
										RawAccumulator->bHasCachedSortData = false;
										FailPass(TEXT("cluster_culling_dispatch_failed"));
										return;
									}
								}

								// CalcViewData → writes to GlobalViewDataBuffer at GlobalBaseOffset
								if (!FGaussianSplatRenderer::DispatchCalcViewDataGlobal(
									RHICmdList, *SceneView, GPUResources,
								Info.LocalToWorld,
								Info.Proxy->GetSplatCount(),
								RemainingOutputCapacity,
								Info.Proxy->GetSHOrder(),
								Info.Proxy->GetOpacityScale(),
								Info.Proxy->GetSplatScale(),
									Info.bUseLODRendering,
									Info.GlobalBaseOffset,
									RawAccumulator))
								{
									RawAccumulator->bHasCachedSortData = false;
									FailPass(TEXT("calc_view_data_global_dispatch_failed"));
									return;
								}
						}

						if (bTimePasses)
						{
							RHICmdList.SubmitAndBlockUntilGPUIdle();
							CalcViewMilliseconds =
								(FPlatformTime::Seconds() - PassTimingStageStartSeconds) * 1000.0;
							PassTimingStageStartSeconds = FPlatformTime::Seconds();
						}

						// --------------------------------------------------
						// Phase 2: CalcViewData already emitted identical sort keys;
						// run only the single global RadixSort.
						// --------------------------------------------------
							const bool bDeferSortAdjusted = bDeferPublishedSort &&
								RawAccumulator->bHasCachedSortData &&
								RawAccumulator->bHasPublishedSortKeys;
							if (!bDeferSortAdjusted)
							{
								if (CurrentFuseDepthKeys == 0u)
								{
									if (!FGaussianSplatRenderer::DispatchCalcDistancesGlobal(
										RHICmdList, RawAccumulator, static_cast<int32>(CappedTotalSplatCount)))
									{
										RawAccumulator->bHasCachedSortData = false;
										FailPass(TEXT("calc_distances_global_dispatch_failed"));
										return;
									}
								}
								else
								{
									// Match the compacted path: CalcViewData is the producer and
									// radix is the next UAV consumer on Vulkan.
									RHICmdList.Transition(FRHITransitionInfo(
										RawAccumulator->GlobalSortDistanceBuffer,
										ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
									RHICmdList.Transition(FRHITransitionInfo(
										RawAccumulator->GlobalSortKeysBuffer,
										ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
								}
								if (!FGaussianSplatRenderer::DispatchRadixSortGlobal(
									RHICmdList, RawAccumulator, static_cast<int32>(CappedTotalSplatCount)))
								{
									RawAccumulator->bHasCachedSortData = false;
									FailPass(TEXT("radix_sort_global_dispatch_failed"));
									return;
								}
								if (CurrentUsePublishedSort != 0u &&
									!PublishGlobalSortKeys(
										RHICmdList, RawAccumulator, CappedTotalSplatCount))
								{
									RawAccumulator->bUsePublishedSortKeysForDraw = false;
								}
							}
						if (bTimePasses)
						{
							RHICmdList.SubmitAndBlockUntilGPUIdle();
							RadixMilliseconds =
								(FPlatformTime::Seconds() - PassTimingStageStartSeconds) * 1000.0;
							PassTimingStageStartSeconds = FPlatformTime::Seconds();
						}

						// ViewData is current even when radix publication is deferred. Cache
						// that view so gaps between input samples do no work; the explicit
						// pending flag forces one exact sort after the settle interval.
						RawAccumulator->bHasCachedSortData = true;
						RawAccumulator->CachedTotalSplatCount = NewTotalSplatCount;
						RawAccumulator->CachedViewProjectionMatrix = CurrentVP;
						RawAccumulator->CachedViewRectSize = CurrentViewRectSize;
						RawAccumulator->CachedMaxRenderBudget = MaxRenderBudget;
						RawAccumulator->CachedRasterProfile = CurrentRasterProfile;
						RawAccumulator->CachedFuseDepthKeys = CurrentFuseDepthKeys;
						RawAccumulator->CachedEnableHeuristicLOD = CurrentEnableHeuristicLOD;
						RawAccumulator->CachedUsePublishedSort = CurrentUsePublishedSort;
						RawAccumulator->bPublishedSortPendingExact = bDeferSortAdjusted;

						for (const auto& Info : ValidProxies)
						{
							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources) continue;  // Extra safety check
							GPUResources->CachedViewProjectionMatrix = CurrentVP;
							GPUResources->CachedViewRectSize = CurrentViewRectSize;
							GPUResources->CachedLocalToWorld = Info.LocalToWorld;
							GPUResources->CachedOpacityScale = Info.Proxy->GetOpacityScale();
							GPUResources->CachedSplatScale = Info.Proxy->GetSplatScale();

								GPUResources->CachedErrorThreshold = FMath::Max(0.001f, Info.Proxy->GetLODErrorThreshold());
							GPUResources->CachedDebugMode = CurrentDebugMode;
							GPUResources->CachedDebugForceLODLevel = CurrentDebugForceLODLevel;
							GPUResources->CachedUseFloat32Position = CurrentUseFloat32Position;
							GPUResources->CachedUseFloat32RotationScale = CurrentUseFloat32RotationScale;
							GPUResources->CachedSHOrder = Info.Proxy->GetSHOrder();
							GPUResources->CachedResidentGeneration = Info.ResidentGeneration;
							GPUResources->bHasCachedSortData = true;
						}
					}

					// Single draw call for ALL proxies (capped to render budget)
						RHICmdList.BeginRenderPass(
							GetRenderPassInfo(Pass1Parameters),
							TEXT("GaussianSplat_RenderToIntermediate"));
							const bool bDrawSucceeded = FGaussianSplatRenderer::DrawSplatsGlobal(
								RHICmdList, *SceneView, RawAccumulator,
								SharedIndexBuffer, (int32)CappedTotalSplatCount, DebugMode,
								ValidProxies[0].Proxy->GetOpacityScale());
							RHICmdList.EndRenderPass();
							if (bTimePasses)
							{
								RHICmdList.SubmitAndBlockUntilGPUIdle();
								DrawMilliseconds =
									(FPlatformTime::Seconds() - PassTimingStageStartSeconds) * 1000.0;
								UE_LOG(LogTemp, Display,
									TEXT("NANOGS_PASS_TIMING splats=%u generation=%u->%u pre-drain=%.3f ms calc-view=%.3f ms radix=%.3f ms draw=%.3f ms total=%.3f ms"),
									CappedTotalSplatCount, PreviousResidentGeneration,
									ProfiledResidentGeneration, PreDrainMilliseconds,
									CalcViewMilliseconds, RadixMilliseconds, DrawMilliseconds,
									PreDrainMilliseconds + CalcViewMilliseconds +
									RadixMilliseconds + DrawMilliseconds);
								RawAccumulator->bPassTimingOneShotConsumed = true;
							}
							if (!bDrawSucceeded)
							{
								FailPass(TEXT("draw_splats_global_failed"));
								return;
							}
						for (const FProxyRenderInfo& Info : ValidProxies)
						{
							if (FGaussianSplatGPUResources* Resources = Info.Proxy->GetGPUResources())
							{
								Resources->NotifyPagedFrontierDrawn_RenderThread(RHICmdList, Info.bCaptureFrontier);
								}
							}
					}
					if (DrawTicketClaim.IsValid())
					{
						// Only Page proxies that passed this view's show/frustum/active-count
						// traversal are required to reach the draw. Off-frustum Page sources
						// legitimately contribute a transparent background.
						for (const FProxyRenderInfo& Expected : VisibleProxies)
						{
							if (Expected.Proxy == nullptr || !Expected.Proxy->IsPagedSource())
							{
								continue;
							}

							bool bFound = false;
							for (const FProxyRenderInfo& Drawn : ValidProxies)
							{
								bFound |= Drawn.Proxy == Expected.Proxy;
							}
							if (!bFound)
							{
								FailPass(TEXT("expected_paged_source_not_drawn"));
								return;
							}
						}
					}
					CaptureEvidence->DrawPublication = NanoGS::Paged::QueryCaptureReadiness(
						SceneView->Family != nullptr ? SceneView->Family->Scene : nullptr);
					CaptureEvidence->bRendererCompleted = true;
				}
			);

		const FViewInfo& CompositeViewInfo = static_cast<const FViewInfo&>(*SceneView);
		const int32 CompositeAfterTonemapMode = FMath::Clamp(
			CVarNanoGSCompositeAfterTonemap.GetValueOnRenderThread(), 0, 2);
		const bool bAfterTonemapViewAllowed =
			(!SceneView->bIsSceneCapture && CompositeAfterTonemapMode >= 1)
			|| (SceneView->bIsSceneCapture
				&& CompositeAfterTonemapMode >= 2
				&& SceneView->Family
				&& SceneView->Family->SceneCaptureSource == SCS_FinalColorLDR);
		const bool bCanCompositeAfterTonemap =
			CompositeViewExtension.IsValid()
			&& bAfterTonemapViewAllowed
			&& DebugMode == 0
			&& SceneView->Family
			&& SceneView->Family->EngineShowFlags.PostProcessing
			&& SceneView->Family->EngineShowFlags.Tonemapper;
		if (bCanCompositeAfterTonemap)
		{
			CompositeViewExtension->Collect(
				SceneView->GetViewKey(),
				GraphBuilder.ConvertToExternalTexture(IntermediateTexture),
				CompositeViewInfo.ViewRect,
				IntermediateDesc.Extent,
				DrawTicketClaim,
				CaptureEvidence);
		}
		else
		{
			// Compatibility path: composite the intermediate sRGB RT into linear
			// SceneColor before UE post processing. SceneCapture intentionally stays
			// here until the AirSim/CARLA image contract is regressed independently.
			ERenderTargetLoadAction CompositeColorLoadAction =
				(DebugMode > 0) ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;
			FGaussianCompositePassParameters* Pass2Parameters =
				GraphBuilder.AllocParameters<FGaussianCompositePassParameters>();
			Pass2Parameters->IntermediateTexture = IntermediateTexture;
			Pass2Parameters->RenderTargets[0] =
				FRenderTargetBinding(ColorTexture, CompositeColorLoadAction);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GaussianSplat_CompositeToSceneColor"),
				Pass2Parameters,
				ERDGPassFlags::Raster,
				[SceneView, IntermediateTexture, DrawTicketClaim, CaptureEvidence](FRHICommandListImmediate& RHICmdList)
				{
					if (!SceneView)
					{
						NanoGS::Paged::FailCaptureDrawTicket_RenderThread(
							DrawTicketClaim, TEXT("composite_scene_view_unavailable"));
						return;
					}

					FRHITexture* IntermediateRHI = IntermediateTexture->GetRHI();
					if (!IntermediateRHI)
					{
						NanoGS::Paged::FailCaptureDrawTicket_RenderThread(
							DrawTicketClaim, TEXT("composite_texture_unavailable"));
						return;
					}
					TShaderMapRef<FGaussianSplatCompositeVS> CompositeVertexShader(
						GetGlobalShaderMap(GMaxRHIFeatureLevel));
					TShaderMapRef<FGaussianSplatCompositePS> CompositePixelShader(
						GetGlobalShaderMap(GMaxRHIFeatureLevel));
					if (!CompositeVertexShader.IsValid() || !CompositePixelShader.IsValid())
					{
						NanoGS::Paged::FailCaptureDrawTicket_RenderThread(
							DrawTicketClaim, TEXT("composite_shader_unavailable"));
						return;
					}

					if (!FGaussianSplatRenderer::CompositeToSceneColor(
						RHICmdList, *SceneView, IntermediateRHI))
					{
						NanoGS::Paged::FailCaptureDrawTicket_RenderThread(
							DrawTicketClaim, TEXT("composite_draw_failed"));
						return;
					}
					if (!CaptureEvidence->bRendererCompleted)
					{
						NanoGS::Paged::FailCaptureDrawTicket_RenderThread(
							DrawTicketClaim, TEXT("draw_evidence_missing"));
						return;
					}
					NanoGS::Paged::SucceedCaptureDrawTicket_RenderThread(
						DrawTicketClaim, CaptureEvidence->DrawPublication);
				}
			);
		}
}

void FNanoGSModule::ShutdownModule()
{
	if (TickDelegateHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);
		TickDelegateHandle.Reset();
	}
	if (ControlWidget.IsValid())
		ControlWidget->RemoveFromParent();
	ControlWidget.Reset();
	ControlWidgetWorld.Reset();
	if (WorldInitializedActorsDelegateHandle.IsValid())
	{
		FWorldDelegates::OnWorldInitializedActors.Remove(WorldInitializedActorsDelegateHandle);
		WorldInitializedActorsDelegateHandle.Reset();
	}
	// Remove deferred init delegate
	if (PostEngineInitDelegateHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitDelegateHandle);
		PostEngineInitDelegateHandle.Reset();
	}

	// Unregister post-opaque render delegate
	if (PostOpaqueRenderDelegateHandle.IsValid())
	{
		GetRendererModuleRef().RemovePostOpaqueRenderDelegate(PostOpaqueRenderDelegateHandle);
		PostOpaqueRenderDelegateHandle.Reset();
	}

	// Release global accumulator GPU buffers from the render thread
	if (GlobalAccumulator.IsValid())
	{
		FGaussianGlobalAccumulator* RawAccumulator = GlobalAccumulator.Release();
		ENQUEUE_RENDER_COMMAND(ReleaseGlobalAccumulator)(
			[RawAccumulator](FRHICommandListImmediate& RHICmdList)
			{
				RawAccumulator->Release();
				delete RawAccumulator;
			});
	}

	// Clear the view extension
	ViewExtension.Reset();
	CompositeViewExtension.Reset();
}

FNanoGSModule& FNanoGSModule::Get()
{
	return FModuleManager::LoadModuleChecked<FNanoGSModule>("NanoGS");
}

bool FNanoGSModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded("NanoGS");
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNanoGSModule, NanoGS)
