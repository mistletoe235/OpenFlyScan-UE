#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "common/WorkerThread.hpp"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/GameViewportClient.h"
#include <memory>
#include "common/Common.hpp"

namespace NanoGS::Paged
{
	struct FCaptureReadinessValidation;
}

class RenderRequest : public FRenderCommand
{
public:
    struct RenderParams
    {
        USceneCaptureComponent2D* const render_component;
        UTextureRenderTarget2D* render_target;
        bool pixels_as_float;
        bool compress;

        RenderParams(USceneCaptureComponent2D* render_component_val, UTextureRenderTarget2D* render_target_val, bool pixels_as_float_val, bool compress_val)
            : render_component(render_component_val), render_target(render_target_val), pixels_as_float(pixels_as_float_val), compress(compress_val)
        {
        }
    };
    struct RenderResult
    {
        TArray<uint8> image_data_uint8;
        TArray<float> image_data_float;

        TArray<FColor> bmp;
        TArray<FFloat16Color> bmp_float;

        int width = 0;
        int height = 0;

        msr::airlib::TTimePoint time_stamp;

		// A strict external renderer gate may invalidate pixels after the GPU readback.
		bool capture_is_valid = true;
		FString capture_diagnostic;
    };

private:
    static FReadSurfaceDataFlags setupRenderResource(const FTextureRenderTargetResource* rt_resource, const RenderParams* params, RenderResult* result, FIntPoint& size);

    std::shared_ptr<RenderParams>* params_;
    std::shared_ptr<RenderResult>* results_;
    unsigned int req_size_;

    std::shared_ptr<msr::airlib::WorkerThreadSignal> wait_signal_;

    bool saved_DisableWorldRendering_ = false;
    UGameViewportClient* const game_viewport_;
    FDelegateHandle end_draw_handle_;
    std::function<void()> query_camera_pose_cb_;

public:
    RenderRequest(UGameViewportClient* game_viewport, std::function<void()>&& query_camera_pose_cb);
    ~RenderRequest();

	/** Deterministic fail-closed helper shared by safe and disabled unsafe capture paths. */
	static void InvalidateCaptureResult(
		RenderResult& Result,
		const FString& Diagnostic);

	static void InvalidateCaptureResults(
		std::vector<std::shared_ptr<RenderResult>>& Results,
		unsigned int RequestCount,
		const FString& Diagnostic);

	/** Apply one camera/component's readiness decision without affecting its siblings. */
	static void ApplyCaptureValidation(
		RenderResult& Result,
		const NanoGS::Paged::FCaptureReadinessValidation& Validation);

	/** Pure policy helper: preserve legacy unsafe behavior only when no Page source is known. */
	static bool ShouldDisableUnsafePath(bool bQueryAvailable, int32 PagedSourceCount)
	{
		return !bQueryAvailable || PagedSourceCount > 0;
	}

	/** Pure guard preventing short/failed GPU readbacks from reaching encoders. */
	static bool IsReadbackComplete(
		bool bReadbackSucceeded,
		int64 ExpectedPixelCount,
		int64 ActualPixelCount)
	{
		return bReadbackSucceeded &&
			ExpectedPixelCount > 0 &&
			ExpectedPixelCount <= static_cast<int64>(MAX_int32) &&
			ActualPixelCount == ExpectedPixelCount;
	}

    void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
    {
        ExecuteTask();
    }

    FORCEINLINE TStatId GetStatId() const
    {
        RETURN_QUICK_DECLARE_CYCLE_STAT(RenderRequest, STATGROUP_RenderThreadCommands);
    }

    // read pixels from render target using render thread, then compress the result into PNG
    // argument on the thread that calls this method.
    void getScreenshot(
        std::shared_ptr<RenderParams> params[], std::vector<std::shared_ptr<RenderResult>>& results, unsigned int req_size, bool use_safe_method);

    void ExecuteTask();
};
