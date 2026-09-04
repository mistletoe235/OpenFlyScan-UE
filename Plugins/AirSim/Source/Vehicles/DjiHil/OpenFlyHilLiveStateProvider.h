#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Vehicles/DjiHil/DjiHilStateProvider.h"
#include "Vehicles/DjiHil/OpenFlyHilProtocol.h"

namespace openfly::hil
{
    struct FLivePoseMetadata
    {
        uint64 SessionId = 0;
        uint64 PoseSequence = 0;
        uint64 SampleMonotonicNs = 0;
        uint64 ReceiveHostMonotonicNs = 0;
        uint32 FlightControllerStateAgeMs = 0;
        float MeasuredSimulatorStateHz = 0.0f;
        uint32 StateFlags = 0;
        double GimbalPitchDeg = 0.0;
        double CommandForwardMps = 0.0;
        double CommandRightMps = 0.0;
        double CommandUpMps = 0.0;
        double CommandYawRateDegPerSec = 0.0;
        bool bFresh = false;
    };

    class FOpenFlyHilLiveStateProvider final : public IDjiHilStateProvider
    {
    public:
        bool PublishPose(uint64 SessionId, uint64 PoseSequence,
            const FPosePayload& Pose, uint64 ReceiveHostMonotonicNs);
        void MarkLinkLost(uint64 SessionId);

        virtual void Reset() override;
        virtual bool GetLatestState(msr::airlib::djihil::State& OutState) const override;
        virtual const TCHAR* GetBackendName() const override { return TEXT("openfly_android"); }

        bool GetLatestMetadata(FLivePoseMetadata& OutMetadata) const;

    private:
        mutable FCriticalSection Mutex;
        msr::airlib::djihil::State LatestState;
        FLivePoseMetadata Metadata;
        bool bHasState = false;
    };
}
