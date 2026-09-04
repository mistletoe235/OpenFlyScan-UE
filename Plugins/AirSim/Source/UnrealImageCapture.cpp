#include "UnrealImageCapture.h"
#include "Engine/World.h"
#include "Async/Async.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "HAL/PlatformProcess.h"

#include "Paged/NanoGSCaptureReadiness.h"

#include "AirBlueprintLib.h"
#include "RenderRequest.h"
#include "common/ClockFactory.hpp"

#include <chrono>
#include <thread>

namespace
{
	APIPCamera* FindCamera(
		const common_utils::UniqueValueMap<std::string, APIPCamera*>* Cameras,
		const std::string& CameraName)
	{
		if (Cameras == nullptr)
			return nullptr;

		const auto It = Cameras->find(CameraName);
		if (It == Cameras->getMap().end() || !IsValid(It->second))
			return nullptr;

		return It->second;
	}

	std::string MakeCameraUnavailableMessage(const std::string& CameraName)
	{
		return std::string("camera is not set: ") + CameraName;
	}

	void InitializeImageResponse(
		const msr::airlib::ImageCaptureBase::ImageRequest& Request,
		msr::airlib::ImageCaptureBase::ImageResponse& Response)
	{
		Response.camera_name = Request.camera_name;
		Response.pixels_as_float = Request.pixels_as_float;
		Response.compress = Request.compress;
		Response.image_type = Request.image_type;
	}
}

struct UnrealImageCapture::FAsyncSceneCaptureState
{
    FCriticalSection Mutex;
    bool bShuttingDown = false;
    bool bPipelineReady = false;
    bool bPipelineDisabled = false;
    uint32 InFlightCount = 0;
    bool bCaptureSlotInFlight[2] = {false, false};
    uint64 NextCaptureSequence = 1;
    uint64 CompletedSequence = 0;
    TOptional<AsyncImageFrame> CompletedFrame;

    void FinishCaptureSlot_AssumesLocked(uint32 CaptureSlot)
    {
        check(CaptureSlot < UE_ARRAY_COUNT(bCaptureSlotInFlight));
        check(InFlightCount > 0);
        check(bCaptureSlotInFlight[CaptureSlot]);
        --InFlightCount;
        bCaptureSlotInFlight[CaptureSlot] = false;
    }

    void PublishFailure_AssumesLocked(uint64 CaptureSequence,
                                      const FString& Diagnostic)
    {
        if (bShuttingDown || CaptureSequence <= CompletedSequence)
            return;

        AsyncImageFrame Frame;
        Frame.message = TCHAR_TO_UTF8(*Diagnostic);
        CompletedFrame = MoveTemp(Frame);
        CompletedSequence = CaptureSequence;
    }
};

UnrealImageCapture::UnrealImageCapture(
    const common_utils::UniqueValueMap<std::string, APIPCamera*>* cameras,
    std::function<std::string()> capture_metadata_callback)
    : cameras_(cameras), capture_metadata_callback_(std::move(capture_metadata_callback)),
      async_scene_capture_state_(MakeShared<FAsyncSceneCaptureState, ESPMode::ThreadSafe>())
{
}

UnrealImageCapture::~UnrealImageCapture()
{
    FScopeLock Lock(&async_scene_capture_state_->Mutex);
    async_scene_capture_state_->bShuttingDown = true;
    async_scene_capture_state_->CompletedFrame.Reset();
}

bool UnrealImageCapture::tryGetAsyncSceneCapture(
    const std::string& camera_name, AsyncImageFrame& out_frame) const
{
    bool bStartCapture = false;
    bool bHasFrame = false;
    uint64 CaptureSequence = 0;
    uint32 CaptureSlot = 0;
    {
        FScopeLock Lock(&async_scene_capture_state_->Mutex);
        if (async_scene_capture_state_->CompletedFrame.IsSet()) {
            out_frame = MoveTemp(async_scene_capture_state_->CompletedFrame.GetValue());
            async_scene_capture_state_->CompletedFrame.Reset();
            bHasFrame = true;
        }
        const uint32 MaximumInFlight = async_scene_capture_state_->bPipelineReady
            && !async_scene_capture_state_->bPipelineDisabled ? 2u : 1u;
        bool bHasFreeCaptureSlot = false;
        for (uint32 Slot = 0; Slot < MaximumInFlight; ++Slot) {
            if (!async_scene_capture_state_->bCaptureSlotInFlight[Slot]) {
                CaptureSlot = Slot;
                bHasFreeCaptureSlot = true;
                break;
            }
        }
        if (!async_scene_capture_state_->bShuttingDown && bHasFreeCaptureSlot
            && async_scene_capture_state_->InFlightCount < MaximumInFlight) {
            ++async_scene_capture_state_->InFlightCount;
            async_scene_capture_state_->bCaptureSlotInFlight[CaptureSlot] = true;
            CaptureSequence = async_scene_capture_state_->NextCaptureSequence++;
            bStartCapture = true;
        }
    }
    if (bStartCapture)
        startAsyncSceneCapture(camera_name, CaptureSequence, CaptureSlot);
    return bHasFrame;
}

void UnrealImageCapture::startAsyncSceneCapture(const std::string& camera_name,
    uint64 capture_sequence, uint32 capture_slot) const
{
    const double CaptureStartedSeconds = FPlatformTime::Seconds();
    const TSharedPtr<FAsyncSceneCaptureState, ESPMode::ThreadSafe> State =
        async_scene_capture_state_;
    const auto* Cameras = cameras_;
    const std::function<std::string()> MetadataCallback = capture_metadata_callback_;

    UAirBlueprintLib::RunCommandOnGameThread(
        [State, Cameras, CameraName = camera_name, MetadataCallback,
            CaptureStartedSeconds, CaptureSequence = capture_sequence,
            CaptureSlot = capture_slot]() mutable {
            const auto CompleteFailure = [State, CaptureSequence, CaptureSlot](
                const FString& Diagnostic) {
                FScopeLock Lock(&State->Mutex);
                State->PublishFailure_AssumesLocked(CaptureSequence, Diagnostic);
                State->FinishCaptureSlot_AssumesLocked(CaptureSlot);
            };
            {
                FScopeLock Lock(&State->Mutex);
                if (State->bShuttingDown) {
                    State->FinishCaptureSlot_AssumesLocked(CaptureSlot);
                    return;
                }
            }

            APIPCamera* Camera = FindCamera(Cameras, CameraName);
            if (Camera == nullptr) {
                CompleteFailure(FString::Printf(TEXT("camera is not set: %s"),
                    UTF8_TO_TCHAR(CameraName.c_str())));
                return;
            }

            USceneCaptureComponent2D* Capture = Camera->getCaptureComponent(
                UnrealImageCapture::ImageType::Scene, false);
            UTextureRenderTarget2D* RenderTarget = CaptureSlot == 0
                ? Camera->getRenderTarget(UnrealImageCapture::ImageType::Scene, false)
                : Camera->getSecondaryRenderTarget(UnrealImageCapture::ImageType::Scene);
            if (!IsValid(Capture) || !IsValid(RenderTarget)) {
                CompleteFailure(TEXT("async capture component or render target is unavailable"));
                return;
            }

            Capture->TextureTarget = RenderTarget;
            if (!Capture->IsActive())
                Capture->Activate();

            FTextureRenderTargetResource* RenderResource =
                RenderTarget->GameThread_GetRenderTargetResource();
            UWorld* World = Capture->GetWorld();
            const FSceneInterface* Scene = World != nullptr ? World->Scene : nullptr;
            if (RenderResource == nullptr || Scene == nullptr) {
                CompleteFailure(TEXT("async capture render resource or scene is unavailable"));
                return;
            }

            const NanoGS::Paged::FCaptureReadinessSnapshot BeforeNanoGS =
                NanoGS::Paged::QueryCaptureReadiness(Scene);
            const NanoGS::Paged::FCaptureDrawTicketHandle DrawTicket =
                NanoGS::Paged::BeginCaptureDrawTicket(
                    Scene, Capture, RenderResource, BeforeNanoGS);
            const std::string Metadata = MetadataCallback
                ? MetadataCallback() : std::string();

            {
                NanoGS::Paged::FScopedCaptureDrawTicketComponent ComponentScope(
                    DrawTicket, Capture);
                Capture->CaptureScene();
            }

            ENQUEUE_RENDER_COMMAND(OpenFlyAsyncSceneReadback)(
                [State, RenderResource, Scene, BeforeNanoGS, DrawTicket, Metadata,
                    CaptureStartedSeconds, CaptureSequence, CaptureSlot](
                    FRHICommandListImmediate& RHICmdList) mutable {
                    const auto CompleteRenderFailure = [State, CaptureSequence, CaptureSlot](
                        const FString& Diagnostic) {
                        FScopeLock Lock(&State->Mutex);
                        State->PublishFailure_AssumesLocked(CaptureSequence, Diagnostic);
                        State->FinishCaptureSlot_AssumesLocked(CaptureSlot);
                    };
                    const FTextureRHIRef Texture =
                        RenderResource->GetRenderTargetTexture();
                    if (!Texture.IsValid()) {
                        CompleteRenderFailure(TEXT("async capture texture is unavailable"));
                        return;
                    }

                    const FIntPoint Size = Texture->GetSizeXY();
                    const EPixelFormat Format = Texture->GetFormat();
                    TUniquePtr<FRHIGPUTextureReadback> Readback =
                        MakeUnique<FRHIGPUTextureReadback>(
                            TEXT("OpenFlyHilAsyncReadback"));
                    RHICmdList.Transition(FRHITransitionInfo(
                        Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
                    Readback->EnqueueCopy(RHICmdList, Texture,
                        FResolveRect(0, 0, Size.X, Size.Y));
                    RHICmdList.Transition(FRHITransitionInfo(
                        Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));

                    AsyncTask(ENamedThreads::AnyBackgroundHiPriTask,
                        [State, Scene, BeforeNanoGS, DrawTicket, Metadata, Size, Format,
                            CaptureStartedSeconds, CaptureSequence, CaptureSlot,
                            Readback = MoveTemp(Readback)]() mutable {
                            while (!Readback->IsReady()) {
                                {
                                    FScopeLock Lock(&State->Mutex);
                                    if (State->bShuttingDown) {
                                        State->FinishCaptureSlot_AssumesLocked(CaptureSlot);
                                        return;
                                    }
                                }
                                FPlatformProcess::SleepNoStats(0.0005f);
                            }

                            int32 RowPitchPixels = 0;
                            int32 BufferHeight = 0;
                            const void* Mapped = Readback->Lock(
                                RowPitchPixels, &BufferHeight);
                            if (Mapped == nullptr || RowPitchPixels < Size.X
                                || BufferHeight < Size.Y
                                || (Format != PF_B8G8R8A8 && Format != PF_R8G8B8A8)) {
                                if (Mapped != nullptr)
                                    Readback->Unlock();
                                FScopeLock Lock(&State->Mutex);
                                State->PublishFailure_AssumesLocked(
                                    CaptureSequence,
                                    FString::Printf(
                                        TEXT("async readback invalid format=%d pitch=%d height=%d size=%dx%d"),
                                        static_cast<int32>(Format), RowPitchPixels, BufferHeight,
                                        Size.X, Size.Y));
                                State->FinishCaptureSlot_AssumesLocked(CaptureSlot);
                                return;
                            }

                            AsyncImageFrame Frame;
                            Frame.width = Size.X;
                            Frame.height = Size.Y;
                            Frame.time_stamp = msr::airlib::ClockFactory::get()->nowNanos();
                            Frame.capture_started_seconds = CaptureStartedSeconds;
                            Frame.message = Metadata;
                            // Keep the GPU readback in the BGRA layout consumed by
                            // IImageWrapper.  The HIL sender used to collapse this
                            // buffer to BGR here and immediately expand it back to
                            // BGRA before JPEG/PNG compression.  Apart from moving
                            // roughly 11 MB per 1440x1080 frame, the scalar nested
                            // loops serialized the otherwise asynchronous capture
                            // pipeline on one worker core.
                            Frame.image_data_uint8.resize(
                                static_cast<size_t>(Size.X) * Size.Y * 4);
                            const uint8* Source = static_cast<const uint8*>(Mapped);
                            uint8* Dest = Frame.image_data_uint8.data();
                            for (int32 Y = 0; Y < Size.Y; ++Y) {
                                const uint8* Row = Source
                                    + static_cast<int64>(Y) * RowPitchPixels * 4;
                                uint8* DestRow = Dest
                                    + static_cast<int64>(Y) * Size.X * 4;
                                if (Format == PF_B8G8R8A8) {
                                    FMemory::Memcpy(DestRow, Row,
                                        static_cast<SIZE_T>(Size.X) * 4);
                                    continue;
                                }
                                for (int32 X = 0; X < Size.X; ++X) {
                                    const uint8* Pixel = Row + X * 4;
                                    uint8* DestPixel = DestRow + X * 4;
                                    DestPixel[0] = Pixel[2];
                                    DestPixel[1] = Pixel[1];
                                    DestPixel[2] = Pixel[0];
                                    DestPixel[3] = Pixel[3];
                                }
                            }
                            Readback->Unlock();
                            Frame.capture_completed_seconds = FPlatformTime::Seconds();

                            const NanoGS::Paged::FCaptureReadinessSnapshot AfterNanoGS =
                                NanoGS::Paged::QueryCaptureReadiness(Scene);
                            const NanoGS::Paged::FCaptureReadinessValidation Validation =
                                NanoGS::Paged::ValidateCaptureTransaction(
                                    DrawTicket, BeforeNanoGS, AfterNanoGS);
                            if (!Validation.bAccepted) {
                                Frame.image_data_uint8.clear();
                                Frame.width = 0;
                                Frame.height = 0;
                                Frame.message = TCHAR_TO_UTF8(*Validation.Diagnostic);
                            }

                            FScopeLock Lock(&State->Mutex);
                            if (!Validation.bAccepted && State->bPipelineReady
                                && !State->bPipelineDisabled) {
                                State->bPipelineDisabled = true;
                                UE_LOG(LogTemp, Warning,
                                    TEXT("OpenFly HIL capture pipeline falling back to single in-flight: %s"),
                                    *Validation.Diagnostic);
                            }
                            else if (Validation.bAccepted && !State->bPipelineReady) {
                                State->bPipelineReady = true;
                                UE_LOG(LogTemp, Display,
                                    TEXT("OpenFly HIL capture pipeline enabled with up to two in-flight readbacks"));
                            }
                            if (!State->bShuttingDown
                                && CaptureSequence > State->CompletedSequence) {
                                State->CompletedFrame = MoveTemp(Frame);
                                State->CompletedSequence = CaptureSequence;
                            }
                            State->FinishCaptureSlot_AssumesLocked(CaptureSlot);
                        });
                });
        });
}

void UnrealImageCapture::getImages(const std::vector<msr::airlib::ImageCaptureBase::ImageRequest>& requests,
                                   std::vector<msr::airlib::ImageCaptureBase::ImageResponse>& responses) const
{
    if (cameras_ == nullptr || cameras_->valsSize() == 0) {
        for (unsigned int i = 0; i < requests.size(); ++i) {
            responses.push_back(ImageResponse());
			ImageResponse& Response = responses.back();
			InitializeImageResponse(requests[i], Response);
			Response.message = MakeCameraUnavailableMessage(requests[i].camera_name);
        }
    }
    else
        // UE5 Vulkan hits a texture layout ensure on the legacy async render-thread path.
        // Use the safe readback path for stability in the fused CARLA + AirSim runtime.
        getSceneCaptureImage(requests, responses, true);
}

void UnrealImageCapture::getSceneCaptureImage(const std::vector<msr::airlib::ImageCaptureBase::ImageRequest>& requests,
                                              std::vector<msr::airlib::ImageCaptureBase::ImageResponse>& responses, bool use_safe_method) const
{
    std::vector<std::shared_ptr<RenderRequest::RenderParams>> render_params;
    std::vector<std::shared_ptr<RenderRequest::RenderResult>> render_results;
	const size_t response_offset = responses.size();
	for (size_t i = 0; i < requests.size(); ++i) {
		responses.push_back(ImageResponse());
		InitializeImageResponse(requests[i], responses.back());
	}

    bool visibilityChanged = false;
	// AirSim RPC handlers may call this concurrently. Camera enable state and every
	// UObject/component lookup must be serialized on the Game Thread, before the
	// per-component capture transactions are queued there.
	UAirBlueprintLib::RunCommandOnGameThread([this, &requests, &responses, response_offset, &visibilityChanged]() {
			check(IsInGameThread());
			for (unsigned int i = 0; i < requests.size(); ++i) {
				APIPCamera* camera = FindCamera(cameras_, requests[i].camera_name);
				if (camera == nullptr) {
					responses.at(response_offset + i).message =
						MakeCameraUnavailableMessage(requests[i].camera_name);
					continue;
				}
				//TODO: may be we should have these methods non-const?
				visibilityChanged = const_cast<UnrealImageCapture*>(this)->updateCameraVisibility(camera, requests[i]) || visibilityChanged;
		}
	}, true);

    if (use_safe_method && visibilityChanged) {
		// Preserve AirSim's activation settling delay after the synchronized
		// Game Thread enable operation.
        std::this_thread::sleep_for(std::chrono::duration<double>(0.2));
    }

    UGameViewportClient* gameViewport = nullptr;
	UAirBlueprintLib::RunCommandOnGameThread(
		[this, &requests, &responses, response_offset, &render_params, &gameViewport]() {
				check(IsInGameThread());
				for (unsigned int i = 0; i < requests.size(); ++i) {
					ImageResponse& response = responses.at(response_offset + i);
					UTextureRenderTarget2D* textureTarget = nullptr;
					USceneCaptureComponent2D* capture = nullptr;
					APIPCamera* camera = FindCamera(cameras_, requests[i].camera_name);
					if (camera == nullptr) {
						response.message = MakeCameraUnavailableMessage(requests[i].camera_name);
					}
					else {
						UWorld* World = camera->GetWorld();
						if (!IsValid(World)) {
							response.message = "Can't take screenshot because camera world is null";
						}
						else {
							UGameViewportClient* CameraViewport = World->GetGameViewport();
							if (!IsValid(CameraViewport)) {
								response.message = "Can't take screenshot because game viewport is null";
							}
							else if (gameViewport == nullptr) {
								gameViewport = CameraViewport;
							}

							capture = camera->getCaptureComponent(requests[i].image_type, false);
						}
					}

					if (camera != nullptr && response.message.empty() && !IsValid(capture)) {
						response.message = "Can't take screenshot because none camera type is not active";
					}
					else if (IsValid(capture) && !IsValid(capture->TextureTarget)) {
						response.message = "Can't take screenshot because texture target is null";
					}
					else if (IsValid(capture)) {
						textureTarget = capture->TextureTarget;
					}

				render_params.push_back(std::make_shared<RenderRequest::RenderParams>(
					capture, textureTarget, requests[i].pixels_as_float, requests[i].compress));
			}
		}, true);

    if (nullptr == gameViewport) {
        return;
    }

		auto query_camera_pose_cb = [this, &requests, &responses, response_offset]() {
        const std::string CaptureMetadata = capture_metadata_callback_
            ? capture_metadata_callback_() : std::string();
        size_t count = requests.size();
		for (size_t i = 0; i < count; i++) {
			const ImageRequest& request = requests.at(i);
			ImageResponse& response = responses.at(response_offset + i);
			APIPCamera* camera = FindCamera(cameras_, request.camera_name);
			if (camera == nullptr) {
				if (response.message.empty())
					response.message = MakeCameraUnavailableMessage(request.camera_name);
				continue;
			}
			auto camera_pose = camera->getPose();
            response.camera_position = camera_pose.position;
            response.camera_orientation = camera_pose.orientation;
            if (!CaptureMetadata.empty() && response.message.empty())
                response.message = CaptureMetadata;
        }
    };
    RenderRequest render_request{ gameViewport, std::move(query_camera_pose_cb) };

    render_request.getScreenshot(render_params.data(), render_results, render_params.size(), use_safe_method);

	for (unsigned int i = 0; i < requests.size(); ++i) {
		const ImageRequest& request = requests.at(i);
		ImageResponse& response = responses.at(response_offset + i);
		if (i >= render_results.size() || render_results[i] == nullptr) {
			if (response.message.empty())
				response.message = "Can't take screenshot because render result is unavailable";
			continue;
		}

		response.time_stamp = render_results[i]->time_stamp;
		if (!render_results[i]->image_data_uint8.IsEmpty())
			response.image_data_uint8 = std::vector<uint8_t>(
				render_results[i]->image_data_uint8.GetData(),
				render_results[i]->image_data_uint8.GetData() + render_results[i]->image_data_uint8.Num());
		if (!render_results[i]->image_data_float.IsEmpty())
			response.image_data_float = std::vector<float>(
				render_results[i]->image_data_float.GetData(),
				render_results[i]->image_data_float.GetData() + render_results[i]->image_data_float.Num());

		response.width = render_results[i]->width;
		response.height = render_results[i]->height;
		if (!render_results[i]->capture_is_valid) {
			const std::string CaptureDiagnostic = TCHAR_TO_UTF8(*render_results[i]->capture_diagnostic);
			response.message = CaptureDiagnostic +
				(response.message.empty() ? std::string() : std::string(";") + response.message);
		}
    }
}

bool UnrealImageCapture::updateCameraVisibility(APIPCamera* camera, const msr::airlib::ImageCaptureBase::ImageRequest& request)
{
	check(IsInGameThread());
	if (!IsValid(camera))
		return false;

    bool visibilityChanged = false;
    if (!camera->getCameraTypeEnabled(request.image_type)) {
        camera->setCameraTypeEnabled(request.image_type, true);
        visibilityChanged = true;
    }

    return visibilityChanged;
}
