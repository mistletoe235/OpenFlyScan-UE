#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "CoreMinimal.h"

#include <limits>

namespace openfly::hil
{
    static constexpr uint32 UdpMagic = 0x4f46484c; // OFHL
    static constexpr uint32 FrameMagic = 0x4f464652; // OFFR
    static constexpr uint16 ProtocolVersion = 1;
    static constexpr int32 UdpHeaderBytes = 32;
    static constexpr int32 MaximumUdpDatagramBytes = 1400;
    static constexpr int32 MaximumEventReasonBytes = 512;
    static constexpr uint32 MaximumFramePayloadBytes = 16u * 1024u * 1024u;

    enum class EUdpPacketType : uint16
    {
        Hello = 1,
        Pose = 2,
        Heartbeat = 3,
        Ping = 4,
        Pong = 5,
        Event = 6
    };

    enum class EEventKind : uint32
    {
        Info = 0,
        Collision = 1,
        Stop = 2,
        Emergency = 3
    };

    enum class EFrameFormat : uint16
    {
        Jpeg = 1,
        Png = 2
    };

    struct FUdpHeader
    {
        EUdpPacketType Type = EUdpPacketType::Hello;
        uint32 Flags = 0;
        uint64 SessionId = 0;
        uint64 Sequence = 0;
        uint32 PayloadBytes = 0;
    };

    struct FHelloPayload
    {
        uint64 AndroidMonotonicNs = 0;
        uint32 RequestedPoseHz = 0;
        uint32 SimulatorStateHz = 0;
        uint32 FrameTcpPort = 0;
        uint32 Capabilities = 0;
    };

    struct FPosePayload
    {
        uint64 SampleMonotonicNs = 0;
        double OriginLatitudeDeg = 0.0;
        double OriginLongitudeDeg = 0.0;
        double EastM = 0.0;
        double NorthM = 0.0;
        double UpM = 0.0;
        double RollDeg = 0.0;
        double PitchDeg = 0.0;
        double HeadingDeg = 0.0;
        double VelocityNorthMps = 0.0;
        double VelocityEastMps = 0.0;
        double VelocityUpMps = 0.0;
        double GimbalPitchDeg = 0.0;
        double CommandForwardMps = 0.0;
        double CommandRightMps = 0.0;
        double CommandUpMps = 0.0;
        double CommandYawRateDegPerSec = 0.0;
        uint32 FlightControllerStateAgeMs = 0;
        float MeasuredSimulatorStateHz = 0.0f;
        uint32 StateFlags = 0;
    };

    struct FHeartbeatPayload
    {
        uint64 SenderMonotonicNs = 0;
        uint64 LastReceivedSequence = 0;
        uint32 StateFlags = 0;
    };

    struct FPingPayload
    {
        uint64 SenderMonotonicNs = 0;
    };

    struct FPongPayload
    {
        uint64 EchoedSenderMonotonicNs = 0;
        uint64 PeerMonotonicNs = 0;
    };

    struct FDecodedDatagram
    {
        FUdpHeader Header;
        FHelloPayload Hello;
        FPosePayload Pose;
        FHeartbeatPayload Heartbeat;
        FPingPayload Ping;
        FPongPayload Pong;
    };

    struct FEventPayload
    {
        uint64 PeerMonotonicNs = 0;
        EEventKind Kind = EEventKind::Info;
        double StopScore = std::numeric_limits<double>::quiet_NaN();
        uint64 PoseSequence = 0;
        FString Reason;
    };

    struct FFrameHeader
    {
        EFrameFormat Format = EFrameFormat::Jpeg;
        uint32 PayloadBytes = 0;
        uint64 FrameId = 0;
        uint64 PoseSequence = 0;
        uint64 CapturePeerMonotonicNs = 0;
        uint32 Width = 0;
        uint32 Height = 0;
        uint32 Flags = 0;
        uint32 PayloadCrc32 = 0;
    };

    bool DecodeAndroidDatagram(TArrayView<const uint8> Datagram,
        FDecodedDatagram& OutPacket, FString& OutError);

    bool EncodeHeartbeat(uint64 SessionId, uint64 Sequence,
        const FHeartbeatPayload& Payload, TArray<uint8>& OutDatagram, FString& OutError);
    bool EncodePong(uint64 SessionId, uint64 Sequence,
        const FPongPayload& Payload, TArray<uint8>& OutDatagram, FString& OutError);
    bool EncodeEvent(uint64 SessionId, uint64 Sequence,
        const FEventPayload& Payload, TArray<uint8>& OutDatagram, FString& OutError);
    bool EncodeFrameHeader(const FFrameHeader& Header,
        TArray<uint8>& OutHeaderBytes, FString& OutError);
}
