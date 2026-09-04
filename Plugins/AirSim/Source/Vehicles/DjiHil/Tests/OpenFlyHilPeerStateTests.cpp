#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Vehicles/DjiHil/OpenFlyHilPeerState.h"

namespace
{
    openfly::hil::FDecodedDatagram MakePacket(openfly::hil::EUdpPacketType Type,
        uint64 SessionId, uint64 Sequence)
    {
        openfly::hil::FDecodedDatagram Packet;
        Packet.Header.Type = Type;
        Packet.Header.SessionId = SessionId;
        Packet.Header.Sequence = Sequence;
        Packet.Hello.FrameTcpPort = 30022;
        Packet.Hello.AndroidMonotonicNs = 10000 + Sequence;
        Packet.Pose.SampleMonotonicNs = 20000 + Sequence;
        Packet.Heartbeat.SenderMonotonicNs = 30000 + Sequence;
        Packet.Ping.SenderMonotonicNs = 40000 + Sequence;
        Packet.Pong.PeerMonotonicNs = 50000 + Sequence;
        Packet.Pose.OriginLatitudeDeg = 22.0;
        Packet.Pose.OriginLongitudeDeg = 113.0;
        Packet.Pose.EastM = static_cast<double>(Sequence);
        return Packet;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenFlyHilPeerStateTest,
    "AirSim.DjiHil.OpenFly.PeerState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOpenFlyHilPeerStateTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace openfly::hil;

    FOpenFlyHilPeerState Peer(1000, 30021);
    TestEqual(TEXT("POSE cannot establish peer"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Pose, 7, 1), TEXT("192.168.1.2"), 100),
        EPeerPacketResult::RejectedNoHello);
    TestFalse(TEXT("No peer after pre-HELLO POSE"), Peer.Snapshot().bHasPeer);

    TestEqual(TEXT("HELLO establishes peer"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Hello, 7, 2), TEXT("192.168.1.2"), 200),
        EPeerPacketResult::AcceptedHello);
    FPeerSnapshot Snapshot = Peer.Snapshot();
    TestTrue(TEXT("Peer is locked"), Snapshot.bHasPeer);
    TestEqual(TEXT("Peer IP"), Snapshot.PeerIp, FString(TEXT("192.168.1.2")));
    TestEqual(TEXT("Session"), Snapshot.SessionId, uint64(7));
    TestTrue(TEXT("HELLO establishes peer clock reference"),
        Snapshot.bHasPeerMonotonicReference);
    TestEqual(TEXT("HELLO peer clock"), Snapshot.PeerMonotonicReferenceNs,
        uint64(10002));
    TestEqual(TEXT("HELLO host clock"), Snapshot.PeerMonotonicReferenceHostNs,
        uint64(200));

    TestEqual(TEXT("Reject another IP"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Pose, 7, 3), TEXT("192.168.1.3"), 300),
        EPeerPacketResult::RejectedPeerMismatch);
    TestEqual(TEXT("Reject another session"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Pose, 8, 3), TEXT("192.168.1.2"), 300),
        EPeerPacketResult::RejectedPeerMismatch);
    TestEqual(TEXT("Mismatches do not refresh freshness"),
        Peer.Snapshot().LastValidPacketHostNs, uint64(200));

    TestEqual(TEXT("Accept first POSE"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Pose, 7, 3), TEXT("192.168.1.2"), 400),
        EPeerPacketResult::AcceptedPose);
    TestEqual(TEXT("Accept independent high HEARTBEAT sequence"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Heartbeat, 7, 1000), TEXT("192.168.1.2"), 450),
        EPeerPacketResult::AcceptedHeartbeat);
    TestEqual(TEXT("Accept next POSE below HEARTBEAT sequence"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Pose, 7, 4), TEXT("192.168.1.2"), 475),
        EPeerPacketResult::AcceptedPose);
    TestEqual(TEXT("Reject duplicate sequence"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Pose, 7, 4), TEXT("192.168.1.2"), 500),
        EPeerPacketResult::RejectedStaleSequence);
    TestEqual(TEXT("Duplicate does not refresh freshness"),
        Peer.Snapshot().LastValidPacketHostNs, uint64(475));

    FDecodedDatagram JitteredOrigin = MakePacket(EUdpPacketType::Pose, 7, 5);
    JitteredOrigin.Pose.OriginLatitudeDeg += 4.0e-9;
    JitteredOrigin.Pose.OriginLongitudeDeg -= 4.0e-9;
    TestEqual(TEXT("Accept sub-centimeter origin jitter"),
        Peer.ProcessPacket(JitteredOrigin, TEXT("192.168.1.2"), 550),
        EPeerPacketResult::AcceptedPose);

    FDecodedDatagram ChangedOrigin = MakePacket(EUdpPacketType::Pose, 7, 6);
    ChangedOrigin.Pose.OriginLatitudeDeg = 23.0;
    TestEqual(TEXT("Reject origin change"),
        Peer.ProcessPacket(ChangedOrigin, TEXT("192.168.1.2"), 600),
        EPeerPacketResult::RejectedOriginChanged);
    TestEqual(TEXT("Origin rejection does not replace pose"),
        Peer.Snapshot().LastPoseSequence, uint64(5));

    TestEqual(TEXT("PING requests immediate PONG"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Ping, 7, 1), TEXT("192.168.1.2"), 700),
        EPeerPacketResult::ReplyPong);
    Snapshot = Peer.Snapshot();
    TestEqual(TEXT("PING refreshes peer clock"), Snapshot.PeerMonotonicReferenceNs,
        uint64(40001));
    TestEqual(TEXT("PING refreshes host clock"), Snapshot.PeerMonotonicReferenceHostNs,
        uint64(700));
    TestFalse(TEXT("Exactly one second remains fresh"), Peer.ExpireIfStale(1000000700ull));
    TestTrue(TEXT("More than one second expires"), Peer.ExpireIfStale(1000000701ull));
    TestFalse(TEXT("Peer marked stale after timeout"), Peer.Snapshot().bHasPeer);
    TestEqual(TEXT("Different session still requires HELLO"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Pose, 8, 1), TEXT("192.168.1.2"), 1100000800ull),
        EPeerPacketResult::RejectedNoHello);
    TestEqual(TEXT("Same peer resumes without discovery delay"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Pose, 7, 6), TEXT("192.168.1.2"), 1100000900ull),
        EPeerPacketResult::AcceptedPose);
    TestTrue(TEXT("Same peer is fresh after resume"), Peer.Snapshot().bHasPeer);
    TestTrue(TEXT("Resumed peer expires again"), Peer.ExpireIfStale(2100000901ull));

    TestEqual(TEXT("New session can establish after timeout"),
        Peer.ProcessPacket(MakePacket(EUdpPacketType::Hello, 8, 1), TEXT("10.0.0.5"), 2200000000ull),
        EPeerPacketResult::AcceptedHello);
    TestEqual(TEXT("New session locked"), Peer.Snapshot().SessionId, uint64(8));
    return true;
}

#endif
