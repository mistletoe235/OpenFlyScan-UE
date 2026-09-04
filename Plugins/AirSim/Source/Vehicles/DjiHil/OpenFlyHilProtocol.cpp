#include "Vehicles/DjiHil/OpenFlyHilProtocol.h"

#include "Containers/StringConv.h"

#include <cmath>
#include <cstring>

namespace openfly::hil
{
namespace
{
    class FBigEndianReader
    {
    public:
        explicit FBigEndianReader(TArrayView<const uint8> InBytes)
            : Bytes(InBytes)
        {
        }

        bool ReadU16(uint16& Out) { return ReadUnsigned(Out); }
        bool ReadU32(uint32& Out) { return ReadUnsigned(Out); }
        bool ReadU64(uint64& Out) { return ReadUnsigned(Out); }

        bool ReadF32(float& Out)
        {
            uint32 Bits = 0;
            if (!ReadU32(Bits))
                return false;
            static_assert(sizeof(Bits) == sizeof(Out));
            FMemory::Memcpy(&Out, &Bits, sizeof(Out));
            return true;
        }

        bool ReadF64(double& Out)
        {
            uint64 Bits = 0;
            if (!ReadU64(Bits))
                return false;
            static_assert(sizeof(Bits) == sizeof(Out));
            FMemory::Memcpy(&Out, &Bits, sizeof(Out));
            return true;
        }

        int32 Remaining() const { return Bytes.Num() - Offset; }

    private:
        template <typename T>
        bool ReadUnsigned(T& Out)
        {
            if (Remaining() < static_cast<int32>(sizeof(T)))
                return false;
            T Value = 0;
            for (int32 Index = 0; Index < static_cast<int32>(sizeof(T)); ++Index)
                Value = static_cast<T>((Value << 8u) | Bytes[Offset++]);
            Out = Value;
            return true;
        }

        TArrayView<const uint8> Bytes;
        int32 Offset = 0;
    };

    class FBigEndianWriter
    {
    public:
        explicit FBigEndianWriter(TArray<uint8>& InBytes)
            : Bytes(InBytes)
        {
        }

        void WriteU16(uint16 Value) { WriteUnsigned(Value); }
        void WriteU32(uint32 Value) { WriteUnsigned(Value); }
        void WriteU64(uint64 Value) { WriteUnsigned(Value); }

        void WriteF64(double Value)
        {
            uint64 Bits = 0;
            static_assert(sizeof(Bits) == sizeof(Value));
            FMemory::Memcpy(&Bits, &Value, sizeof(Value));
            WriteU64(Bits);
        }

        void WriteBytes(TArrayView<const uint8> Value)
        {
            Bytes.Append(Value.GetData(), Value.Num());
        }

    private:
        template <typename T>
        void WriteUnsigned(T Value)
        {
            for (int32 Shift = (static_cast<int32>(sizeof(T)) - 1) * 8; Shift >= 0; Shift -= 8)
                Bytes.Add(static_cast<uint8>((Value >> Shift) & 0xffu));
        }

        TArray<uint8>& Bytes;
    };

    bool Fail(FString& OutError, const TCHAR* Message)
    {
        OutError = Message;
        return false;
    }

    bool IsKnownAndroidType(EUdpPacketType Type)
    {
        return Type == EUdpPacketType::Hello || Type == EUdpPacketType::Pose
            || Type == EUdpPacketType::Heartbeat || Type == EUdpPacketType::Ping
            || Type == EUdpPacketType::Pong;
    }

    bool IsFinitePose(const FPosePayload& Pose)
    {
        const double Values[] = {
            Pose.OriginLatitudeDeg, Pose.OriginLongitudeDeg,
            Pose.EastM, Pose.NorthM, Pose.UpM,
            Pose.RollDeg, Pose.PitchDeg, Pose.HeadingDeg,
            Pose.VelocityNorthMps, Pose.VelocityEastMps, Pose.VelocityUpMps,
            Pose.GimbalPitchDeg, Pose.CommandForwardMps, Pose.CommandRightMps,
            Pose.CommandUpMps, Pose.CommandYawRateDegPerSec
        };
        for (double Value : Values) {
            if (!std::isfinite(Value))
                return false;
        }
        return std::isfinite(Pose.MeasuredSimulatorStateHz);
    }

    bool EncodeHeader(EUdpPacketType Type, uint64 SessionId, uint64 Sequence,
        uint32 PayloadBytes, TArray<uint8>& OutDatagram, FString& OutError)
    {
        OutError.Reset();
        OutDatagram.Reset(UdpHeaderBytes + static_cast<int32>(PayloadBytes));
        if (SessionId == 0)
            return Fail(OutError, TEXT("openfly.hil.v1 response requires non-zero session_id"));
        if (UdpHeaderBytes + PayloadBytes > MaximumUdpDatagramBytes)
            return Fail(OutError, TEXT("openfly.hil.v1 response exceeds UDP size limit"));
        FBigEndianWriter Writer(OutDatagram);
        Writer.WriteU32(UdpMagic);
        Writer.WriteU16(ProtocolVersion);
        Writer.WriteU16(static_cast<uint16>(Type));
        Writer.WriteU32(0);
        Writer.WriteU64(SessionId);
        Writer.WriteU64(Sequence);
        Writer.WriteU32(PayloadBytes);
        return true;
    }
}

bool DecodeAndroidDatagram(TArrayView<const uint8> Datagram,
    FDecodedDatagram& OutPacket, FString& OutError)
{
    OutError.Reset();
    OutPacket = FDecodedDatagram{};
    if (Datagram.Num() < UdpHeaderBytes)
        return Fail(OutError, TEXT("openfly.hil.v1 UDP datagram is shorter than its header"));
    if (Datagram.Num() > MaximumUdpDatagramBytes)
        return Fail(OutError, TEXT("openfly.hil.v1 UDP datagram exceeds 1400 bytes"));

    FBigEndianReader Reader(Datagram);
    uint32 Magic = 0;
    uint16 Version = 0;
    uint16 RawType = 0;
    if (!Reader.ReadU32(Magic) || !Reader.ReadU16(Version) || !Reader.ReadU16(RawType)
        || !Reader.ReadU32(OutPacket.Header.Flags)
        || !Reader.ReadU64(OutPacket.Header.SessionId)
        || !Reader.ReadU64(OutPacket.Header.Sequence)
        || !Reader.ReadU32(OutPacket.Header.PayloadBytes))
        return Fail(OutError, TEXT("openfly.hil.v1 UDP header is truncated"));

    OutPacket.Header.Type = static_cast<EUdpPacketType>(RawType);
    if (Magic != UdpMagic)
        return Fail(OutError, TEXT("openfly.hil.v1 UDP magic mismatch"));
    if (Version != ProtocolVersion)
        return Fail(OutError, TEXT("unsupported openfly.hil UDP version"));
    if (!IsKnownAndroidType(OutPacket.Header.Type))
        return Fail(OutError, TEXT("invalid Android-to-UE openfly.hil packet type"));
    if (OutPacket.Header.Flags != 0)
        return Fail(OutError, TEXT("openfly.hil.v1 packet has unsupported flags"));
    if (OutPacket.Header.SessionId == 0)
        return Fail(OutError, TEXT("openfly.hil.v1 session_id must be non-zero"));
    if (OutPacket.Header.PayloadBytes != static_cast<uint32>(Reader.Remaining()))
        return Fail(OutError, TEXT("openfly.hil.v1 payload_bytes does not match datagram length"));

    switch (OutPacket.Header.Type) {
    case EUdpPacketType::Hello:
    {
        uint32 Reserved = 0;
        if (OutPacket.Header.PayloadBytes != 28
            || !Reader.ReadU64(OutPacket.Hello.AndroidMonotonicNs)
            || !Reader.ReadU32(OutPacket.Hello.RequestedPoseHz)
            || !Reader.ReadU32(OutPacket.Hello.SimulatorStateHz)
            || !Reader.ReadU32(OutPacket.Hello.FrameTcpPort)
            || !Reader.ReadU32(OutPacket.Hello.Capabilities)
            || !Reader.ReadU32(Reserved))
            return Fail(OutError, TEXT("invalid openfly.hil.v1 HELLO payload"));
        if (Reserved != 0 || OutPacket.Hello.FrameTcpPort == 0
            || OutPacket.Hello.FrameTcpPort > 65535)
            return Fail(OutError, TEXT("openfly.hil.v1 HELLO has invalid reserved/port fields"));
        break;
    }
    case EUdpPacketType::Pose:
    {
        uint32 Reserved = 0;
        FPosePayload& Pose = OutPacket.Pose;
        if (OutPacket.Header.PayloadBytes != 152
            || !Reader.ReadU64(Pose.SampleMonotonicNs)
            || !Reader.ReadF64(Pose.OriginLatitudeDeg)
            || !Reader.ReadF64(Pose.OriginLongitudeDeg)
            || !Reader.ReadF64(Pose.EastM) || !Reader.ReadF64(Pose.NorthM)
            || !Reader.ReadF64(Pose.UpM) || !Reader.ReadF64(Pose.RollDeg)
            || !Reader.ReadF64(Pose.PitchDeg) || !Reader.ReadF64(Pose.HeadingDeg)
            || !Reader.ReadF64(Pose.VelocityNorthMps)
            || !Reader.ReadF64(Pose.VelocityEastMps)
            || !Reader.ReadF64(Pose.VelocityUpMps)
            || !Reader.ReadF64(Pose.GimbalPitchDeg)
            || !Reader.ReadF64(Pose.CommandForwardMps)
            || !Reader.ReadF64(Pose.CommandRightMps)
            || !Reader.ReadF64(Pose.CommandUpMps)
            || !Reader.ReadF64(Pose.CommandYawRateDegPerSec)
            || !Reader.ReadU32(Pose.FlightControllerStateAgeMs)
            || !Reader.ReadF32(Pose.MeasuredSimulatorStateHz)
            || !Reader.ReadU32(Pose.StateFlags) || !Reader.ReadU32(Reserved))
            return Fail(OutError, TEXT("invalid openfly.hil.v1 POSE payload"));
        if (Reserved != 0 || !IsFinitePose(Pose)
            || Pose.OriginLatitudeDeg < -90.0 || Pose.OriginLatitudeDeg > 90.0
            || Pose.OriginLongitudeDeg < -180.0 || Pose.OriginLongitudeDeg > 180.0)
            return Fail(OutError, TEXT("openfly.hil.v1 POSE contains invalid numeric fields"));
        break;
    }
    case EUdpPacketType::Heartbeat:
    {
        uint32 Reserved = 0;
        if (OutPacket.Header.PayloadBytes != 24
            || !Reader.ReadU64(OutPacket.Heartbeat.SenderMonotonicNs)
            || !Reader.ReadU64(OutPacket.Heartbeat.LastReceivedSequence)
            || !Reader.ReadU32(OutPacket.Heartbeat.StateFlags)
            || !Reader.ReadU32(Reserved) || Reserved != 0)
            return Fail(OutError, TEXT("invalid openfly.hil.v1 HEARTBEAT payload"));
        break;
    }
    case EUdpPacketType::Ping:
        if (OutPacket.Header.PayloadBytes != 8
            || !Reader.ReadU64(OutPacket.Ping.SenderMonotonicNs))
            return Fail(OutError, TEXT("invalid openfly.hil.v1 PING payload"));
        break;
    case EUdpPacketType::Pong:
        if (OutPacket.Header.PayloadBytes != 16
            || !Reader.ReadU64(OutPacket.Pong.EchoedSenderMonotonicNs)
            || !Reader.ReadU64(OutPacket.Pong.PeerMonotonicNs))
            return Fail(OutError, TEXT("invalid openfly.hil.v1 PONG payload"));
        break;
    default:
        return Fail(OutError, TEXT("unreachable openfly.hil packet type"));
    }
    return Reader.Remaining() == 0
        || Fail(OutError, TEXT("openfly.hil.v1 payload decoder left trailing bytes"));
}

bool EncodeHeartbeat(uint64 SessionId, uint64 Sequence,
    const FHeartbeatPayload& Payload, TArray<uint8>& OutDatagram, FString& OutError)
{
    if (!EncodeHeader(EUdpPacketType::Heartbeat, SessionId, Sequence, 24, OutDatagram, OutError))
        return false;
    FBigEndianWriter Writer(OutDatagram);
    Writer.WriteU64(Payload.SenderMonotonicNs);
    Writer.WriteU64(Payload.LastReceivedSequence);
    Writer.WriteU32(Payload.StateFlags);
    Writer.WriteU32(0);
    return true;
}

bool EncodePong(uint64 SessionId, uint64 Sequence,
    const FPongPayload& Payload, TArray<uint8>& OutDatagram, FString& OutError)
{
    if (!EncodeHeader(EUdpPacketType::Pong, SessionId, Sequence, 16, OutDatagram, OutError))
        return false;
    FBigEndianWriter Writer(OutDatagram);
    Writer.WriteU64(Payload.EchoedSenderMonotonicNs);
    Writer.WriteU64(Payload.PeerMonotonicNs);
    return true;
}

bool EncodeEvent(uint64 SessionId, uint64 Sequence,
    const FEventPayload& Payload, TArray<uint8>& OutDatagram, FString& OutError)
{
    OutError.Reset();
    if (Payload.PeerMonotonicNs == 0 || Payload.PoseSequence == 0)
        return Fail(OutError, TEXT("openfly.hil.v1 EVENT timestamps/sequences must be non-zero"));
    if (Payload.Kind == EEventKind::Stop && !FMath::IsFinite(Payload.StopScore))
        return Fail(OutError, TEXT("openfly.hil.v1 STOP EVENT requires a finite score"));
    if (Payload.Kind != EEventKind::Stop && FMath::IsFinite(Payload.StopScore))
        return Fail(OutError, TEXT("openfly.hil.v1 non-STOP EVENT score must be NaN"));
    FTCHARToUTF8 ReasonUtf8(*Payload.Reason);
    const int32 ReasonBytes = ReasonUtf8.Length();
    if (ReasonBytes > MaximumEventReasonBytes)
        return Fail(OutError, TEXT("openfly.hil.v1 EVENT reason exceeds 512 UTF-8 bytes"));
    if (static_cast<uint32>(Payload.Kind) > static_cast<uint32>(EEventKind::Emergency))
        return Fail(OutError, TEXT("openfly.hil.v1 EVENT kind is invalid"));
    const uint32 PayloadBytes = 32u + static_cast<uint32>(ReasonBytes);
    if (!EncodeHeader(EUdpPacketType::Event, SessionId, Sequence,
            PayloadBytes, OutDatagram, OutError))
        return false;
    FBigEndianWriter Writer(OutDatagram);
    Writer.WriteU64(Payload.PeerMonotonicNs);
    Writer.WriteU32(static_cast<uint32>(Payload.Kind));
    Writer.WriteF64(Payload.StopScore);
    Writer.WriteU64(Payload.PoseSequence);
    Writer.WriteU32(static_cast<uint32>(ReasonBytes));
    Writer.WriteBytes(MakeArrayView(
        reinterpret_cast<const uint8*>(ReasonUtf8.Get()), ReasonBytes));
    return true;
}

bool EncodeFrameHeader(const FFrameHeader& Header,
    TArray<uint8>& OutHeaderBytes, FString& OutError)
{
    OutError.Reset();
    OutHeaderBytes.Reset(56);
    if (Header.Format != EFrameFormat::Jpeg && Header.Format != EFrameFormat::Png)
        return Fail(OutError, TEXT("openfly.hil.v1 frame format is invalid"));
    if (Header.PayloadBytes == 0 || Header.PayloadBytes > MaximumFramePayloadBytes)
        return Fail(OutError, TEXT("openfly.hil.v1 frame payload size is invalid"));
    if (Header.FrameId == 0 || Header.PoseSequence == 0
        || Header.Width == 0 || Header.Height == 0 || Header.Flags != 0)
        return Fail(OutError, TEXT("openfly.hil.v1 frame header has invalid required fields"));

    FBigEndianWriter Writer(OutHeaderBytes);
    Writer.WriteU32(FrameMagic);
    Writer.WriteU16(ProtocolVersion);
    Writer.WriteU16(static_cast<uint16>(Header.Format));
    Writer.WriteU32(56);
    Writer.WriteU32(Header.PayloadBytes);
    Writer.WriteU64(Header.FrameId);
    Writer.WriteU64(Header.PoseSequence);
    Writer.WriteU64(Header.CapturePeerMonotonicNs);
    Writer.WriteU32(Header.Width);
    Writer.WriteU32(Header.Height);
    Writer.WriteU32(Header.Flags);
    Writer.WriteU32(Header.PayloadCrc32);
    check(OutHeaderBytes.Num() == 56);
    return true;
}
}
