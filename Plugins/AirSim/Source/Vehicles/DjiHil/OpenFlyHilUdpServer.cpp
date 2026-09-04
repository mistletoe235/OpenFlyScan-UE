#include "Vehicles/DjiHil/OpenFlyHilUdpServer.h"

#include "Common/UdpSocketBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace openfly::hil
{
FOpenFlyHilUdpServer::FOpenFlyHilUdpServer(const FUdpServerConfig& InConfig,
    FOpenFlyHilLiveStateProvider& InStateProvider)
    : Config(InConfig), StateProvider(InStateProvider),
      PeerState(InConfig.PeerTimeoutMs, InConfig.AndroidReplyPort)
{
}

FOpenFlyHilUdpServer::~FOpenFlyHilUdpServer()
{
    Shutdown();
}

bool FOpenFlyHilUdpServer::Start(FString& OutError)
{
    OutError.Reset();
    if (Thread.IsValid() || Socket != nullptr) {
        OutError = TEXT("openfly.hil UDP server is already started");
        return false;
    }
    FIPv4Address BindIp;
    if (!FIPv4Address::Parse(Config.BindAddress, BindIp)) {
        OutError = FString::Printf(TEXT("invalid openfly.hil UDP bind address: %s"),
            *Config.BindAddress);
        return false;
    }
    Socket = FUdpSocketBuilder(TEXT("OpenFlyHilUdpServer"))
        .AsNonBlocking()
        .AsReusable()
        .BoundToEndpoint(FIPv4Endpoint(BindIp, Config.BindPort))
        .WithReceiveBufferSize(256 * 1024)
        .WithSendBufferSize(64 * 1024);
    if (Socket == nullptr) {
        OutError = FString::Printf(TEXT("failed to bind openfly.hil UDP %s:%u"),
            *Config.BindAddress, Config.BindPort);
        return false;
    }
    bStopRequested.Store(false);
    Thread.Reset(FRunnableThread::Create(this, TEXT("OpenFlyHilUdp"),
        0, TPri_AboveNormal));
    if (!Thread.IsValid()) {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
        Socket = nullptr;
        OutError = TEXT("failed to create openfly.hil UDP worker thread");
        return false;
    }
    return true;
}

void FOpenFlyHilUdpServer::Shutdown()
{
    Stop();
    if (Thread.IsValid()) {
        Thread->WaitForCompletion();
        Thread.Reset();
    }
    if (Socket != nullptr) {
        Socket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
        Socket = nullptr;
    }
    const FPeerSnapshot Peer = PeerState.Snapshot();
    StateProvider.MarkLinkLost(Peer.SessionId);
    PeerState.Reset();
}

bool FOpenFlyHilUdpServer::QueueEvent(const FEventPayload& Event)
{
    FScopeLock Lock(&PendingEventsMutex);
    // Snapshot while holding the queue lock. If timeout races this call, the
    // expiry path will clear anything appended here before a future HELLO can
    // reuse even the same numeric session id.
    const FPeerSnapshot Peer = PeerState.Snapshot();
    if (!Peer.bHasPeer)
        return false;
    FQueuedEvent Queued;
    Queued.SessionId = Peer.SessionId;
    Queued.Payload = Event;
    static constexpr int32 MaximumPendingEvents = 64;
    if (PendingEvents.Num() < MaximumPendingEvents) {
        PendingEvents.Add(MoveTemp(Queued));
        return true;
    }
    // Keep memory bounded. An emergency may replace the newest lower-priority item.
    if (Event.Kind == EEventKind::Emergency) {
        PendingEvents.Last() = MoveTemp(Queued);
        return true;
    }
    return false;
}

FUdpServerStats FOpenFlyHilUdpServer::GetStats() const
{
    FScopeLock Lock(&StatsMutex);
    return Stats;
}

FPeerSnapshot FOpenFlyHilUdpServer::GetPeerSnapshot() const
{
    return PeerState.Snapshot();
}

uint32 FOpenFlyHilUdpServer::Run()
{
    while (!bStopRequested.Load()) {
        const uint64 NowNs = MonotonicNowNs();
        ReceiveAvailable(NowNs);
        ServiceTimersAndEvents(NowNs);
        FPlatformProcess::SleepNoStats(0.002f);
    }
    return 0;
}

void FOpenFlyHilUdpServer::Stop()
{
    bStopRequested.Store(true);
}

uint64 FOpenFlyHilUdpServer::MonotonicNowNs() const
{
    return static_cast<uint64>(FPlatformTime::Seconds() * 1000000000.0);
}

void FOpenFlyHilUdpServer::ReceiveAvailable(uint64 NowNs)
{
    if (Socket == nullptr)
        return;
    uint32 PendingBytes = 0;
    while (Socket->HasPendingData(PendingBytes)) {
        const int32 BufferBytes = FMath::Clamp<int32>(
            static_cast<int32>(PendingBytes), 1, MaximumUdpDatagramBytes + 1);
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(BufferBytes);
        TSharedRef<FInternetAddr> Source =
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
        int32 ReadBytes = 0;
        if (!Socket->RecvFrom(Buffer.GetData(), Buffer.Num(), ReadBytes, *Source)
            || ReadBytes <= 0)
            break;
        Buffer.SetNum(ReadBytes, EAllowShrinking::No);
        {
            FScopeLock Lock(&StatsMutex);
            ++Stats.ReceivedDatagrams;
            Stats.LastReceiveHostNs = NowNs;
        }
        ProcessDatagram(Buffer, Source->ToString(false), NowNs);
        NowNs = MonotonicNowNs();
    }
}

void FOpenFlyHilUdpServer::ProcessDatagram(TArrayView<const uint8> Bytes,
    const FString& SourceIp, uint64 NowNs)
{
    FDecodedDatagram Packet;
    FString Error;
    if (!DecodeAndroidDatagram(Bytes, Packet, Error)) {
        FScopeLock Lock(&StatsMutex);
        ++Stats.InvalidDatagrams;
        return;
    }

    const EPeerPacketResult Result = PeerState.ProcessPacket(Packet, SourceIp, NowNs);
    const FPeerSnapshot Peer = PeerState.Snapshot();
    switch (Result) {
    case EPeerPacketResult::AcceptedHello:
        {
            FScopeLock Lock(&StatsMutex);
            ++Stats.AcceptedHello;
        }
        SendHeartbeat(Peer, NowNs);
        break;
    case EPeerPacketResult::AcceptedPose:
        if (StateProvider.PublishPose(Packet.Header.SessionId,
                Packet.Header.Sequence, Packet.Pose, NowNs)) {
            FScopeLock Lock(&StatsMutex);
            ++Stats.AcceptedPose;
        }
        break;
    case EPeerPacketResult::ReplyPong:
        SendPong(Peer, Packet.Ping.SenderMonotonicNs, NowNs);
        break;
    case EPeerPacketResult::AcceptedHeartbeat:
    case EPeerPacketResult::AcceptedPong:
        break;
    default:
        {
            FScopeLock Lock(&StatsMutex);
            const uint64 RejectedCount = ++Stats.RejectedPackets;
            if (RejectedCount <= 8 || RejectedCount % 500 == 0)
                UE_LOG(LogTemp, Warning, TEXT("OpenFly HIL rejected packet result=%d type=%d count=%llu source=%s session=%llu sequence=%llu peer_session=%llu last_packet=%llu last_pose=%llu origin=(%.12f,%.12f) peer_origin=(%.12f,%.12f)"), static_cast<int32>(Result), static_cast<int32>(Packet.Header.Type), RejectedCount, *SourceIp, Packet.Header.SessionId, Packet.Header.Sequence, Peer.SessionId, Peer.LastPacketSequence, Peer.LastPoseSequence, Packet.Pose.OriginLatitudeDeg, Packet.Pose.OriginLongitudeDeg, Peer.LatestPose.OriginLatitudeDeg, Peer.LatestPose.OriginLongitudeDeg);
        }
        break;
    }
}

void FOpenFlyHilUdpServer::ServiceTimersAndEvents(uint64 NowNs)
{
    const FPeerSnapshot BeforeExpire = PeerState.Snapshot();
    if (PeerState.ExpireIfStale(NowNs)) {
        StateProvider.MarkLinkLost(BeforeExpire.SessionId);
        {
            // Never retain observations across link loss, even if Android later
            // happens to reuse the same numeric session id.
            FScopeLock EventLock(&PendingEventsMutex);
            PendingEvents.Reset();
        }
        FScopeLock Lock(&StatsMutex);
        ++Stats.LinkLossCount;
    }

    const FPeerSnapshot Peer = PeerState.Snapshot();
    if (!Peer.bHasPeer)
        return;
    const uint64 HeartbeatIntervalNs =
        static_cast<uint64>(FMath::Max(1u, Config.HeartbeatIntervalMs)) * 1000000ull;
    if (LastHeartbeatHostNs == 0 || NowNs - LastHeartbeatHostNs >= HeartbeatIntervalNs)
        SendHeartbeat(Peer, NowNs);

    TArray<FQueuedEvent> EventsToSend;
    {
        FScopeLock Lock(&PendingEventsMutex);
        EventsToSend = MoveTemp(PendingEvents);
        PendingEvents.Reset();
    }
    for (const FQueuedEvent& Event : EventsToSend) {
        if (Event.SessionId == Peer.SessionId)
            SendEvent(Peer, Event.Payload);
    }
}

bool FOpenFlyHilUdpServer::SendHeartbeat(const FPeerSnapshot& Peer, uint64 NowNs)
{
    FHeartbeatPayload Payload;
    Payload.SenderMonotonicNs = NowNs;
    Payload.LastReceivedSequence = Peer.LastPacketSequence;
    TArray<uint8> Datagram;
    FString Error;
    if (!EncodeHeartbeat(Peer.SessionId, ResponseSequence++,
            Payload, Datagram, Error))
        return false;
    const bool bSent = SendToPeer(Peer, Datagram);
    if (bSent) {
        LastHeartbeatHostNs = NowNs;
        FScopeLock Lock(&StatsMutex);
        ++Stats.SentHeartbeat;
    }
    return bSent;
}

bool FOpenFlyHilUdpServer::SendPong(const FPeerSnapshot& Peer,
    uint64 EchoedNs, uint64 NowNs)
{
    FPongPayload Payload;
    Payload.EchoedSenderMonotonicNs = EchoedNs;
    Payload.PeerMonotonicNs = NowNs;
    TArray<uint8> Datagram;
    FString Error;
    if (!EncodePong(Peer.SessionId, ResponseSequence++,
            Payload, Datagram, Error))
        return false;
    const bool bSent = SendToPeer(Peer, Datagram);
    if (bSent) {
        FScopeLock Lock(&StatsMutex);
        ++Stats.SentPong;
    }
    return bSent;
}

bool FOpenFlyHilUdpServer::SendEvent(const FPeerSnapshot& Peer,
    const FEventPayload& Event)
{
    TArray<uint8> Datagram;
    FString Error;
    if (!EncodeEvent(Peer.SessionId, ResponseSequence++,
            Event, Datagram, Error))
        return false;
    const bool bSent = SendToPeer(Peer, Datagram);
    if (bSent) {
        FScopeLock Lock(&StatsMutex);
        ++Stats.SentEvent;
    }
    return bSent;
}

bool FOpenFlyHilUdpServer::SendToPeer(const FPeerSnapshot& Peer,
    TArrayView<const uint8> Datagram)
{
    bool bValidIp = false;
    TSharedRef<FInternetAddr> Destination =
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
    Destination->SetIp(*Peer.PeerIp, bValidIp);
    Destination->SetPort(Peer.AndroidReplyPort);
    int32 SentBytes = 0;
    const bool bSent = bValidIp && Socket != nullptr
        && Socket->SendTo(Datagram.GetData(), Datagram.Num(), SentBytes, *Destination)
        && SentBytes == Datagram.Num();
    if (!bSent) {
        FScopeLock Lock(&StatsMutex);
        ++Stats.SendFailures;
    }
    return bSent;
}
}
