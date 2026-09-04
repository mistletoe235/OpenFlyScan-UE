#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Vehicles/DjiHil/OpenFlyHilProtocol.h"

namespace openfly::hil
{
    enum class EPeerPacketResult : uint8
    {
        AcceptedHello,
        AcceptedPose,
        AcceptedHeartbeat,
        AcceptedPong,
        ReplyPong,
        RejectedNoHello,
        RejectedPeerMismatch,
        RejectedStaleSequence,
        RejectedOriginChanged
    };

    struct FPeerSnapshot
    {
        bool bHasPeer = false;
        FString PeerIp;
        uint16 AndroidReplyPort = 30021;
        uint16 FrameTcpPort = 30022;
        uint64 SessionId = 0;
        uint64 LastPacketSequence = 0;
        uint64 LastPoseSequence = 0;
        uint64 LastValidPacketHostNs = 0;
        bool bHasPeerMonotonicReference = false;
        uint64 PeerMonotonicReferenceNs = 0;
        uint64 PeerMonotonicReferenceHostNs = 0;
        uint64 AcceptedPoseCount = 0;
        uint64 RejectedPacketCount = 0;
        bool bHasPose = false;
        FPosePayload LatestPose;
    };

    class FOpenFlyHilPeerState
    {
    public:
        explicit FOpenFlyHilPeerState(uint32 InTimeoutMs = 1000,
            uint16 InAndroidReplyPort = 30021);

        EPeerPacketResult ProcessPacket(const FDecodedDatagram& Packet,
            const FString& SourceIp, uint64 HostMonotonicNs);
        bool ExpireIfStale(uint64 HostMonotonicNs);
        void Reset();
        FPeerSnapshot Snapshot() const;

    private:
        void ResetLocked();
        void UpdatePeerMonotonicReferenceLocked(uint64 PeerMonotonicNs,
            uint64 HostMonotonicNs);
        bool OriginMatchesLocked(const FPosePayload& Pose) const;

        mutable FCriticalSection Mutex;
        FPeerSnapshot State;
        uint64 TimeoutNs = 1000000000ull;
        uint16 ConfiguredAndroidReplyPort = 30021;
        bool bHasOrigin = false;
        double OriginLatitudeDeg = 0.0;
        double OriginLongitudeDeg = 0.0;
    };
}
