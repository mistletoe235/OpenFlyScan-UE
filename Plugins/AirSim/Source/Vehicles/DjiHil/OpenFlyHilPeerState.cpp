#include "Vehicles/DjiHil/OpenFlyHilPeerState.h"

#include "Misc/ScopeLock.h"

namespace openfly::hil
{
FOpenFlyHilPeerState::FOpenFlyHilPeerState(uint32 InTimeoutMs,
    uint16 InAndroidReplyPort)
    : TimeoutNs(static_cast<uint64>(FMath::Max(1u, InTimeoutMs)) * 1000000ull),
      ConfiguredAndroidReplyPort(InAndroidReplyPort == 0 ? 30021 : InAndroidReplyPort)
{
    State.AndroidReplyPort = ConfiguredAndroidReplyPort;
}

EPeerPacketResult FOpenFlyHilPeerState::ProcessPacket(const FDecodedDatagram& Packet,
    const FString& SourceIp, uint64 HostMonotonicNs)
{
    FScopeLock Lock(&Mutex);
    if (!State.bHasPeer) {
        if (Packet.Header.Type == EUdpPacketType::Hello) {
            ResetLocked();
            State.bHasPeer = true;
            State.PeerIp = SourceIp;
            State.SessionId = Packet.Header.SessionId;
            State.LastPacketSequence = Packet.Header.Sequence;
            State.LastValidPacketHostNs = HostMonotonicNs;
            State.AndroidReplyPort = ConfiguredAndroidReplyPort;
            State.FrameTcpPort = static_cast<uint16>(Packet.Hello.FrameTcpPort);
            UpdatePeerMonotonicReferenceLocked(Packet.Hello.AndroidMonotonicNs,
                HostMonotonicNs);
            return EPeerPacketResult::AcceptedHello;
        }
        if (State.PeerIp == SourceIp && State.SessionId == Packet.Header.SessionId) {
            State.bHasPeer = true;
        }
        else {
            ++State.RejectedPacketCount;
            return EPeerPacketResult::RejectedNoHello;
        }
    }

    if (State.PeerIp != SourceIp || State.SessionId != Packet.Header.SessionId) {
        ++State.RejectedPacketCount;
        return EPeerPacketResult::RejectedPeerMismatch;
    }
    if (Packet.Header.Type == EUdpPacketType::Pose
        && State.bHasPose
        && Packet.Header.Sequence <= State.LastPoseSequence) {
        ++State.RejectedPacketCount;
        return EPeerPacketResult::RejectedStaleSequence;
    }

    if (Packet.Header.Type == EUdpPacketType::Pose && !OriginMatchesLocked(Packet.Pose)) {
        ++State.RejectedPacketCount;
        return EPeerPacketResult::RejectedOriginChanged;
    }

    State.LastPacketSequence = FMath::Max(
        State.LastPacketSequence, Packet.Header.Sequence);
    State.LastValidPacketHostNs = HostMonotonicNs;
    switch (Packet.Header.Type) {
    case EUdpPacketType::Hello:
        State.FrameTcpPort = static_cast<uint16>(Packet.Hello.FrameTcpPort);
        UpdatePeerMonotonicReferenceLocked(Packet.Hello.AndroidMonotonicNs,
            HostMonotonicNs);
        return EPeerPacketResult::AcceptedHello;
    case EUdpPacketType::Pose:
        if (!bHasOrigin) {
            bHasOrigin = true;
            OriginLatitudeDeg = Packet.Pose.OriginLatitudeDeg;
            OriginLongitudeDeg = Packet.Pose.OriginLongitudeDeg;
        }
        State.LatestPose = Packet.Pose;
        State.LastPoseSequence = Packet.Header.Sequence;
        State.bHasPose = true;
        ++State.AcceptedPoseCount;
        UpdatePeerMonotonicReferenceLocked(Packet.Pose.SampleMonotonicNs,
            HostMonotonicNs);
        return EPeerPacketResult::AcceptedPose;
    case EUdpPacketType::Heartbeat:
        UpdatePeerMonotonicReferenceLocked(Packet.Heartbeat.SenderMonotonicNs,
            HostMonotonicNs);
        return EPeerPacketResult::AcceptedHeartbeat;
    case EUdpPacketType::Ping:
        UpdatePeerMonotonicReferenceLocked(Packet.Ping.SenderMonotonicNs,
            HostMonotonicNs);
        return EPeerPacketResult::ReplyPong;
    case EUdpPacketType::Pong:
        UpdatePeerMonotonicReferenceLocked(Packet.Pong.PeerMonotonicNs,
            HostMonotonicNs);
        return EPeerPacketResult::AcceptedPong;
    default:
        ++State.RejectedPacketCount;
        return EPeerPacketResult::RejectedPeerMismatch;
    }
}

bool FOpenFlyHilPeerState::ExpireIfStale(uint64 HostMonotonicNs)
{
    FScopeLock Lock(&Mutex);
    if (!State.bHasPeer || HostMonotonicNs < State.LastValidPacketHostNs
        || HostMonotonicNs - State.LastValidPacketHostNs <= TimeoutNs)
        return false;
    State.bHasPeer = false;
    State.bHasPose = false;
    State.LastValidPacketHostNs = 0;
    State.bHasPeerMonotonicReference = false;
    return true;
}

void FOpenFlyHilPeerState::Reset()
{
    FScopeLock Lock(&Mutex);
    ResetLocked();
}

FPeerSnapshot FOpenFlyHilPeerState::Snapshot() const
{
    FScopeLock Lock(&Mutex);
    return State;
}

void FOpenFlyHilPeerState::ResetLocked()
{
    const uint64 RejectedPacketCount = State.RejectedPacketCount;
    State = FPeerSnapshot{};
    State.AndroidReplyPort = ConfiguredAndroidReplyPort;
    State.RejectedPacketCount = RejectedPacketCount;
    bHasOrigin = false;
    OriginLatitudeDeg = 0.0;
    OriginLongitudeDeg = 0.0;
}

void FOpenFlyHilPeerState::UpdatePeerMonotonicReferenceLocked(
    uint64 PeerMonotonicNs, uint64 HostMonotonicNs)
{
    if (PeerMonotonicNs == 0 || HostMonotonicNs == 0)
        return;
    State.bHasPeerMonotonicReference = true;
    State.PeerMonotonicReferenceNs = PeerMonotonicNs;
    State.PeerMonotonicReferenceHostNs = HostMonotonicNs;
}

bool FOpenFlyHilPeerState::OriginMatchesLocked(const FPosePayload& Pose) const
{
    if (!bHasOrigin)
        return true;
    static constexpr double OriginToleranceDeg = 1.0e-7;
    return FMath::IsNearlyEqual(
               OriginLatitudeDeg, Pose.OriginLatitudeDeg, OriginToleranceDeg)
        && FMath::IsNearlyEqual(
               OriginLongitudeDeg, Pose.OriginLongitudeDeg, OriginToleranceDeg);
}
}
