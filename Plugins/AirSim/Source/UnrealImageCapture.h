#pragma once

#include "CoreMinimal.h"
#include "PIPCamera.h"
#include "common/ImageCaptureBase.hpp"
#include "common/common_utils/UniqueValueMap.hpp"
#include "HAL/CriticalSection.h"
#include "Templates/SharedPointer.h"

class UnrealImageCapture : public msr::airlib::ImageCaptureBase
{
public:
    typedef msr::airlib::ImageCaptureBase::ImageType ImageType;

    UnrealImageCapture(const common_utils::UniqueValueMap<std::string, APIPCamera*>* cameras,
                       std::function<std::string()> capture_metadata_callback = {});
    virtual ~UnrealImageCapture();

    virtual void getImages(const std::vector<ImageRequest>& requests, std::vector<ImageResponse>& responses) const override;

    struct AsyncImageFrame
    {
        std::vector<uint8_t> image_data_uint8;
        int32 width = 0;
        int32 height = 0;
        msr::airlib::TTimePoint time_stamp = 0;
        double capture_started_seconds = 0.0;
        double capture_completed_seconds = 0.0;
        std::string message;
    };

    bool tryGetAsyncSceneCapture(const std::string& camera_name, AsyncImageFrame& out_frame) const;

private:
    struct FAsyncSceneCaptureState;

    void startAsyncSceneCapture(const std::string& camera_name,
                                uint64 capture_sequence,
                                uint32 capture_slot) const;
    void getSceneCaptureImage(const std::vector<msr::airlib::ImageCaptureBase::ImageRequest>& requests,
                              std::vector<msr::airlib::ImageCaptureBase::ImageResponse>& responses, bool use_safe_method) const;

    bool updateCameraVisibility(APIPCamera* camera, const msr::airlib::ImageCaptureBase::ImageRequest& request);

private:
    const common_utils::UniqueValueMap<std::string, APIPCamera*>* cameras_;
    std::function<std::string()> capture_metadata_callback_;
    TSharedPtr<FAsyncSceneCaptureState, ESPMode::ThreadSafe> async_scene_capture_state_;
};
