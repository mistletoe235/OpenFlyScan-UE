#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Templates/Atomic.h"
#include "Vehicles/DjiHil/OpenFlyHilLiveStateProvider.h"
#include "Vehicles/DjiHil/OpenFlyHilPeerState.h"

class FRunnableThread;
class FSocket;

namespace openfly::hil
{
    struct FUdpServerConfig
    {
        FString BindAddress = TEXT("0.0.0.0");
        uint16 BindPort = 30020;
        uint16 AndroidReplyPort = 30021;
        uint32 PeerTimeoutMs = 1000;
        uint32 HeartbeatIntervalMs = 250;
    };

    struct FUdpServerStats
    {
        uint64 ReceivedDatagrams = 0;
        uint64 InvalidDatagrams = 0;
        uint64 AcceptedHello = 0;
        uint64 AcceptedPose = 0;
        uint64 RejectedPackets = 0;
        uint64 SentHeartbeat = 0;
        uint64 SentPong = 0;
        uint64 SentEvent = 0;
        uint64 SendFailures = 0;
        uint64 LinkLossCount = 0;
        uint64 LastReceiveHostNs = 0;
    };

    class FOpenFlyHilUdpServer final : public FRunnable
    {
    public:
        FOpenFlyHilUdpServer(const FUdpServerConfig& InConfig,
            FOpenFlyHilLiveStateProvider& InStateProvider);
        virtual ~FOpenFlyHilUdpServer() override;

        bool Start(FString& OutError);
        void Shutdown();
        // Events are observations only. They are accepted only for the currently
        // live peer and are pinned to that session so a reconnect cannot receive
        // an event produced for an older Android episode.
        bool QueueEvent(const FEventPayload& Event);
        FUdpServerStats GetStats() const;
        FPeerSnapshot GetPeerSnapshot() const;

        virtual uint32 Run() override;
        virtual void Stop() override;

    private:
        uint64 MonotonicNowNs() const;
        void ReceiveAvailable(uint64 NowNs);
        void ProcessDatagram(TArrayView<const uint8> Bytes,
            const FString& SourceIp, uint64 NowNs);
        void ServiceTimersAndEvents(uint64 NowNs);
        bool SendHeartbeat(const FPeerSnapshot& Peer, uint64 NowNs);
        bool SendPong(const FPeerSnapshot& Peer, uint64 EchoedNs, uint64 NowNs);
        bool SendEvent(const FPeerSnapshot& Peer, const FEventPayload& Event);
        bool SendToPeer(const FPeerSnapshot& Peer, TArrayView<const uint8> Datagram);

        FUdpServerConfig Config;
        FOpenFlyHilLiveStateProvider& StateProvider;
        FOpenFlyHilPeerState PeerState;
        FSocket* Socket = nullptr;
        TUniquePtr<FRunnableThread> Thread;
        TAtomic<bool> bStopRequested{false};
        uint64 ResponseSequence = 1;
        uint64 LastHeartbeatHostNs = 0;
        mutable FCriticalSection PendingEventsMutex;
        struct FQueuedEvent
        {
            uint64 SessionId = 0;
            FEventPayload Payload;
        };
        TArray<FQueuedEvent> PendingEvents;
        mutable FCriticalSection StatsMutex;
        FUdpServerStats Stats;
    };
}
