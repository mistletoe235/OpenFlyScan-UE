#include "RenderRequest.h"
#include "TextureResource.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Paged/NanoGSCaptureReadiness.h"
#include "AirBlueprintLib.h"
#include "Async/Async.h"

RenderRequest::RenderRequest(UGameViewportClient* game_viewport, std::function<void()>&& query_camera_pose_cb)
    : params_(nullptr), results_(nullptr), req_size_(0), wait_signal_(new msr::airlib::WorkerThreadSignal), game_viewport_(game_viewport), query_camera_pose_cb_(std::move(query_camera_pose_cb))
{
}

RenderRequest::~RenderRequest()
{
}

void RenderRequest::InvalidateCaptureResult(
	RenderResult& Result,
	const FString& Diagnostic)
{
	Result.bmp.Reset();
	Result.bmp_float.Reset();
	Result.image_data_uint8.Reset();
	Result.image_data_float.Reset();
	Result.width = 0;
	Result.height = 0;
	Result.time_stamp = 0;
	Result.capture_is_valid = false;
	Result.capture_diagnostic = Diagnostic;
}

void RenderRequest::InvalidateCaptureResults(
	std::vector<std::shared_ptr<RenderResult>>& Results,
	const unsigned int RequestCount,
	const FString& Diagnostic)
{
	for (unsigned int Index = 0; Index < RequestCount && Index < Results.size(); ++Index) {
		if (!Results[Index])
			continue;
		InvalidateCaptureResult(*Results[Index], Diagnostic);
	}
}

void RenderRequest::ApplyCaptureValidation(
	RenderResult& Result,
	const NanoGS::Paged::FCaptureReadinessValidation& Validation)
{
	if (!Validation.bAccepted)
	{
		InvalidateCaptureResult(Result, Validation.Diagnostic);
	}
}

// read pixels from render target using render thread, then compress the result into PNG
// argument on the thread that calls this method.
void RenderRequest::getScreenshot(std::shared_ptr<RenderParams> params[], std::vector<std::shared_ptr<RenderResult>>& results, unsigned int req_size, bool use_safe_method)
{
    //TODO: is below really needed?
    for (unsigned int i = 0; i < req_size; ++i) {
        results.push_back(std::make_shared<RenderResult>());

        if (!params[i]->pixels_as_float)
            results[i]->bmp.Reset();
        else
            results[i]->bmp_float.Reset();
        results[i]->time_stamp = 0;
    }

    //make sure we are not on the rendering thread
    CheckNotBlockedOnRenderThread();

	bool unsafe_path_disabled = false;
	if (!use_safe_method) {
		// Keep the legacy path for non-Page scenes, but never let a future caller
		// bypass the generation-stable Page contract by selecting unsafe readback.
		UAirBlueprintLib::RunCommandOnGameThread([params, req_size, &unsafe_path_disabled]() {
			const FSceneInterface* CaptureSceneInterface = nullptr;
			bool has_renderable_capture = false;
			bool has_mixed_or_missing_scenes = false;
			for (unsigned int i = 0; i < req_size; ++i) {
				if (params[i] == nullptr || params[i]->render_component == nullptr || params[i]->render_target == nullptr)
					continue;
				has_renderable_capture = true;
				const UWorld* World = params[i]->render_component->GetWorld();
				const FSceneInterface* ItemScene = World != nullptr ? World->Scene : nullptr;
				if (ItemScene == nullptr || (CaptureSceneInterface != nullptr && CaptureSceneInterface != ItemScene))
					has_mixed_or_missing_scenes = true;
				else
					CaptureSceneInterface = ItemScene;
			}

			if (has_renderable_capture) {
				if (has_mixed_or_missing_scenes) {
					unsafe_path_disabled = true;
				}
				else {
					const NanoGS::Paged::FCaptureReadinessSnapshot Snapshot =
						NanoGS::Paged::QueryCaptureReadiness(CaptureSceneInterface);
					unsafe_path_disabled = RenderRequest::ShouldDisableUnsafePath(
						Snapshot.bQueryAvailable, Snapshot.Sources.Num());
				}
			}
		}, true);
	}

	if (use_safe_method) {
		auto CaptureAndReadback = [this, params, &results, req_size]() {
			check(IsInGameThread());
			// Capture pose and external-state metadata on the same Game Thread
			// turn immediately before enqueuing CaptureScene.
			if (query_camera_pose_cb_)
				query_camera_pose_cb_();

			for (unsigned int i = 0; i < req_size; ++i) {
				if (params[i] == nullptr || params[i]->render_component == nullptr || params[i]->render_target == nullptr) {
					continue;
				}

				// A bounded Page frontier is view-dependent. Bracket each capture component
				// independently so one camera cannot mask or invalidate a sibling camera's
				// result, and read back before a shared target can be overwritten.
				const UWorld* World = params[i]->render_component->GetWorld();
				const FSceneInterface* CaptureSceneInterface = World != nullptr ? World->Scene : nullptr;
				FTextureRenderTargetResource* rt_resource =
					params[i]->render_target->GameThread_GetRenderTargetResource();
				if (rt_resource == nullptr) {
					InvalidateCaptureResult(
						*results[i], TEXT("nanogs_not_ready reason=capture_target_unavailable"));
					continue;
				}
				const NanoGS::Paged::FCaptureReadinessSnapshot BeforeNanoGS =
					NanoGS::Paged::QueryCaptureReadiness(CaptureSceneInterface);
				const NanoGS::Paged::FCaptureDrawTicketHandle DrawTicket =
					NanoGS::Paged::BeginCaptureDrawTicket(
						CaptureSceneInterface,
						params[i]->render_component,
						rt_resource,
						BeforeNanoGS);

				// When AirSim cameras are configured as nodisplay, the capture components do not
				// update every frame. Force a fresh capture before reading the render target so
					// Python image requests do not see an uninitialized / stale black frame.
					params[i]->render_component->TextureTarget = params[i]->render_target;
					{
						// UE does not retain the component pointer in FSceneView. This scope
						// authenticates the synchronous SetupView/BeginRenderViewFamily chain
						// and is intentionally limited to this exact CaptureScene invocation.
						NanoGS::Paged::FScopedCaptureDrawTicketComponent ComponentScope(
							DrawTicket, params[i]->render_component);
						params[i]->render_component->CaptureScene();
					}

					FIntPoint img_size;
					bool bReadbackSucceeded = false;
					if (!params[i]->pixels_as_float) {
						auto flags = setupRenderResource(rt_resource, params[i].get(), results[i].get(), img_size);
					// ReadPixels enqueues its read after CaptureScene's commands and
					// performs the one required flush itself. An explicit flush here
					// causes nested FlushRenderingCommands calls when an RPC task is
					// pumped by a render-thread wait on Linux/Vulkan.
						bReadbackSucceeded = rt_resource->ReadPixels(results[i]->bmp, flags);
					}
					else {
						setupRenderResource(rt_resource, params[i].get(), results[i].get(), img_size);
						bReadbackSucceeded = rt_resource->ReadFloat16Pixels(results[i]->bmp_float);
					}
					const int64 ExpectedPixelCount =
						static_cast<int64>(img_size.X) * static_cast<int64>(img_size.Y);
					const int64 ActualPixelCount = params[i]->pixels_as_float
						? static_cast<int64>(results[i]->bmp_float.Num())
						: static_cast<int64>(results[i]->bmp.Num());
					const bool bReadbackComplete = IsReadbackComplete(
						bReadbackSucceeded, ExpectedPixelCount, ActualPixelCount);
					if (!bReadbackSucceeded)
					{
						InvalidateCaptureResult(
							*results[i], TEXT("capture_invalid reason=readback_failed"));
					}
					else if (!bReadbackComplete)
					{
						InvalidateCaptureResult(
							*results[i],
							FString::Printf(
								TEXT("capture_invalid reason=readback_size_mismatch expected=%lld actual=%lld"),
								static_cast<long long>(ExpectedPixelCount),
								static_cast<long long>(ActualPixelCount)));
					}
					else
					{
						results[i]->time_stamp = msr::airlib::ClockFactory::get()->nowNanos();
					}

				// ReadPixels/ReadFloat16Pixels flushes this CaptureScene before returning.
				// The pair therefore describes the exact publication used by this result.
				const NanoGS::Paged::FCaptureReadinessSnapshot AfterNanoGS =
					NanoGS::Paged::QueryCaptureReadiness(CaptureSceneInterface);
					const NanoGS::Paged::FCaptureReadinessValidation CaptureValidation =
						NanoGS::Paged::ValidateCaptureTransaction(
							DrawTicket, BeforeNanoGS, AfterNanoGS);
					if (results[i]->capture_is_valid)
					{
						ApplyCaptureValidation(*results[i], CaptureValidation);
					}
			}
		};

        if (UAirBlueprintLib::IsInGameThread()) {
            CaptureAndReadback();
        }
        else {
            // A task-graph callback may be pumped while the game thread is already
            // inside FlushRenderingCommands. Calling ReadPixels there recursively
            // is unsafe on Linux/Vulkan. Register a one-shot ticker from the game
            // thread, then do the capture/readback on the next normal frame tick.
            FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
            UAirBlueprintLib::RunCommandOnGameThread([CaptureAndReadback, CompletionEvent]() mutable {
                FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                    [CaptureAndReadback, CompletionEvent](float) mutable {
                        CaptureAndReadback();
                        CompletionEvent->Trigger();
                        return false;
                    }));
            }, true);
            CompletionEvent->Wait();
            FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
        }
    }
    else if (unsafe_path_disabled) {
		InvalidateCaptureResults(
			results, req_size, TEXT("nanogs_not_ready reason=unsafe_path_disabled"));
	}
	else {
        //wait for render thread to pick up our task
        params_ = params;
        results_ = results.data();
        req_size_ = req_size;

        // Queue up the task of querying camera pose in the game thread and synchronizing render thread with camera pose
        AsyncTask(ENamedThreads::GameThread, [this]() {
            check(IsInGameThread());

            saved_DisableWorldRendering_ = game_viewport_->bDisableWorldRendering;
            game_viewport_->bDisableWorldRendering = 0;
            end_draw_handle_ = game_viewport_->OnEndDraw().AddLambda([this] {
                check(IsInGameThread());

                // capture CameraPose for this frame
                query_camera_pose_cb_();

                // The completion is called immeidately after GameThread sends the
                // rendering commands to RenderThread. Hence, our ExecuteTask will
                // execute *immediately* after RenderThread renders the scene!
                RenderRequest* This = this;
                ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)
                (
                    [This](FRHICommandListImmediate& RHICmdList) {
                        This->ExecuteTask();
                    });

                game_viewport_->bDisableWorldRendering = saved_DisableWorldRendering_;

                assert(end_draw_handle_.IsValid());
                game_viewport_->OnEndDraw().Remove(end_draw_handle_);
            });

            // while we're still on GameThread, enqueue request for capture the scene!
            for (unsigned int i = 0; i < req_size_; ++i) {
                params_[i]->render_component->CaptureSceneDeferred();
            }
        });

        // wait for this task to complete
        while (!wait_signal_->waitFor(5)) {
            // log a message and continue wait
            // lamda function still references a few objects for which there is no refcount.
            // Walking away will cause memory corruption, which is much more difficult to debug.
            UE_LOG(LogTemp, Warning, TEXT("Failed: timeout waiting for screenshot"));
        }
    }

    for (unsigned int i = 0; i < req_size; ++i) {
        if (!params[i]->pixels_as_float) {
            if (results[i]->width != 0 && results[i]->height != 0) {
                if (params[i]->compress) {
                    UAirBlueprintLib::CompressImageArray(results[i]->width, results[i]->height, results[i]->bmp, results[i]->image_data_uint8);
                }
                else {
                    results[i]->image_data_uint8.SetNumUninitialized(
                        results[i]->width * results[i]->height * 3, EAllowShrinking::No);
                    uint8* ptr = results[i]->image_data_uint8.GetData();
                    for (const auto& item : results[i]->bmp) {
                        *ptr++ = item.B;
                        *ptr++ = item.G;
                        *ptr++ = item.R;
                    }
                }
            }
        }
        else {
            results[i]->image_data_float.SetNumUninitialized(results[i]->width * results[i]->height);
            float* ptr = results[i]->image_data_float.GetData();
            for (const auto& item : results[i]->bmp_float) {
                *ptr++ = item.R.GetFloat();
            }
        }
    }
}

FReadSurfaceDataFlags RenderRequest::setupRenderResource(const FTextureRenderTargetResource* rt_resource, const RenderParams* params, RenderResult* result, FIntPoint& size)
{
    size = rt_resource->GetSizeXY();
    result->width = size.X;
    result->height = size.Y;
    FReadSurfaceDataFlags flags(RCM_UNorm, CubeFace_MAX);
    flags.SetLinearToGamma(false);

    return flags;
}

void RenderRequest::ExecuteTask()
{
    if (params_ != nullptr && req_size_ > 0) {
        for (unsigned int i = 0; i < req_size_; ++i) {
            FRHICommandListImmediate& RHICmdList = GetImmediateCommandList_ForRenderCommand();
            auto rt_resource = params_[i]->render_target->GetRenderTargetResource();
            if (rt_resource != nullptr) {
                const FTextureRHIRef& rhi_texture = rt_resource->GetRenderTargetTexture();
                FIntPoint size;
                auto flags = setupRenderResource(rt_resource, params_[i].get(), results_[i].get(), size);

                //should we be using ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER which was in original commit by @saihv
                //https://github.com/Microsoft/AirSim/pull/162/commits/63e80c43812300a8570b04ed42714a3f6949e63f#diff-56b790f9394f7ca1949ddbb320d8456fR64
                if (!params_[i]->pixels_as_float) {
                    //below is undocumented method that avoids flushing, but it seems to segfault every 2000 or so calls
                    RHICmdList.ReadSurfaceData(
                        rhi_texture,
                        FIntRect(0, 0, size.X, size.Y),
                        results_[i]->bmp,
                        flags);
                }
                else {
                    RHICmdList.ReadSurfaceFloatData(
                        rhi_texture,
                        FIntRect(0, 0, size.X, size.Y),
                        results_[i]->bmp_float,
                        CubeFace_PosX,
                        0,
                        0);
                }
            }

            results_[i]->time_stamp = msr::airlib::ClockFactory::get()->nowNanos();
        }

        req_size_ = 0;
        params_ = nullptr;
        results_ = nullptr;

        wait_signal_->signal();
    }
}
