#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Templates/Atomic.h"
#include "Vehicles/DjiHil/OpenFlyHilProtocol.h"

class FRunnableThread;
class FSocket;
class UnrealImageCapture;

namespace openfly::hil
{
    struct FPeerSnapshot;
    class FOpenFlyHilUdpServer;

    struct FFrameServerConfig
    {
        FString CameraName = TEXT("front_custom");
        EFrameFormat Format = EFrameFormat::Jpeg;
        uint32 Width = 1440;
        uint32 Height = 1080;
        uint32 JpegQuality = 92;
        uint32 MaximumPayloadBytes = MaximumFramePayloadBytes;
        uint32 MaximumFrameRate = 30;
    };

    struct FFrameServerStats
    {
        bool bClientConnected = false;
        uint64 AcceptedConnections = 0;
        uint64 RejectedConnections = 0;
        uint64 CompletedFrames = 0;
        uint64 CaptureFailures = 0;
        uint64 EncodeFailures = 0;
        uint64 SendFailures = 0;
        uint64 BytesSent = 0;
        uint64 LastFrameId = 0;
        uint64 LastPoseSequence = 0;
        uint64 CaptureSamples = 0;
        uint64 CaptureTotalMicros = 0;
        uint64 CaptureMaximumMicros = 0;
        uint64 EncodeSamples = 0;
        uint64 EncodeTotalMicros = 0;
        uint64 EncodeMaximumMicros = 0;
        uint64 SendSamples = 0;
        uint64 SendTotalMicros = 0;
        uint64 SendMaximumMicros = 0;
    };

    class FOpenFlyHilFrameServer final : public FRunnable
    {
    public:
        FOpenFlyHilFrameServer(const FFrameServerConfig& InConfig,
            const UnrealImageCapture& InImageCapture,
            const FOpenFlyHilUdpServer& InUdpServer);
        virtual ~FOpenFlyHilFrameServer() override;

        bool Start(FString& OutError);
        void Shutdown();
        FFrameServerStats GetStats() const;

        virtual uint32 Run() override;
        virtual void Stop() override;

    private:
        bool ConnectToPeer(const FPeerSnapshot& Peer);
        void CloseClient();
        bool ClientClosedOrSentUnexpectedData() const;
        bool CaptureAndSendFrame(bool& bOutCaptureCompleted);
        bool EncodeJpeg(const std::vector<uint8>& Bgra, int32 Width, int32 Height,
            TArray<uint8>& OutJpeg) const;
        bool EncodePng(const std::vector<uint8>& Bgra, int32 Width, int32 Height,
            TArray<uint8>& OutPng) const;
        bool SendAll(TArrayView<const uint8> Bytes);
        static bool ExtractUnsignedJsonField(const std::string& Json,
            const char* Field, uint64& OutValue);
        uint64 MonotonicNowNs() const;

        FFrameServerConfig Config;
        const UnrealImageCapture& ImageCapture;
        const FOpenFlyHilUdpServer& UdpServer;
        FSocket* ClientSocket = nullptr;
        FString ClientIp;
        uint64 ClientSessionId = 0;
        uint16 ClientPort = 0;
        double NextConnectAttemptSeconds = 0.0;
        uint64 NextFrameId = 1;
        bool bLoggedBootstrapFrame = false;
        TUniquePtr<FRunnableThread> Thread;
        TAtomic<bool> bStopRequested{false};
        mutable FCriticalSection StatsMutex;
        FFrameServerStats Stats;
    };
}
