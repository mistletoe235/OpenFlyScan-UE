#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Vehicles/DjiHil/OpenFlyHilProtocol.h"

#include <cstring>
#include <limits>

namespace
{
    template <typename T>
    void AppendUnsigned(TArray<uint8>& Bytes, T Value)
    {
        for (int32 Shift = (static_cast<int32>(sizeof(T)) - 1) * 8; Shift >= 0; Shift -= 8)
            Bytes.Add(static_cast<uint8>((Value >> Shift) & 0xffu));
    }

    void AppendF32(TArray<uint8>& Bytes, float Value)
    {
        uint32 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Value));
        AppendUnsigned(Bytes, Bits);
    }

    void AppendF64(TArray<uint8>& Bytes, double Value)
    {
        uint64 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Value));
        AppendUnsigned(Bytes, Bits);
    }

    TArray<uint8> MakeHeader(openfly::hil::EUdpPacketType Type,
        uint64 SessionId, uint64 Sequence, uint32 PayloadBytes)
    {
        TArray<uint8> Bytes;
        AppendUnsigned(Bytes, openfly::hil::UdpMagic);
        AppendUnsigned(Bytes, openfly::hil::ProtocolVersion);
        AppendUnsigned(Bytes, static_cast<uint16>(Type));
        AppendUnsigned(Bytes, uint32(0));
        AppendUnsigned(Bytes, SessionId);
        AppendUnsigned(Bytes, Sequence);
        AppendUnsigned(Bytes, PayloadBytes);
        return Bytes;
    }

    TArray<uint8> MakeHello()
    {
        TArray<uint8> Bytes = MakeHeader(openfly::hil::EUdpPacketType::Hello, 0x1020304050607080ull, 9, 28);
        AppendUnsigned(Bytes, uint64(123456789));
        AppendUnsigned(Bytes, uint32(50));
        AppendUnsigned(Bytes, uint32(100));
        AppendUnsigned(Bytes, uint32(30022));
        AppendUnsigned(Bytes, uint32(3));
        AppendUnsigned(Bytes, uint32(0));
        return Bytes;
    }

    TArray<uint8> MakePose(double EastM, double NorthM, double UpM,
        double HeadingDeg, float MeasuredHz)
    {
        TArray<uint8> Bytes = MakeHeader(openfly::hil::EUdpPacketType::Pose, 77, 42, 152);
        AppendUnsigned(Bytes, uint64(987654321));
        AppendF64(Bytes, 22.542813);
        AppendF64(Bytes, 113.958902);
        AppendF64(Bytes, EastM);
        AppendF64(Bytes, NorthM);
        AppendF64(Bytes, UpM);
        AppendF64(Bytes, 1.25);
        AppendF64(Bytes, -3.5);
        AppendF64(Bytes, HeadingDeg);
        AppendF64(Bytes, 4.0);
        AppendF64(Bytes, 5.0);
        AppendF64(Bytes, 6.0);
        AppendF64(Bytes, -20.0);
        AppendF64(Bytes, 0.1);
        AppendF64(Bytes, 0.2);
        AppendF64(Bytes, 0.3);
        AppendF64(Bytes, 2.0);
        AppendUnsigned(Bytes, uint32(17));
        AppendF32(Bytes, MeasuredHz);
        AppendUnsigned(Bytes, uint32(7));
        AppendUnsigned(Bytes, uint32(0));
        return Bytes;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenFlyHilProtocolCodecTest,
    "AirSim.DjiHil.OpenFly.ProtocolCodec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOpenFlyHilProtocolCodecTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace openfly::hil;

    FString Error;
    FDecodedDatagram Packet;
    const TArray<uint8> Hello = MakeHello();
    TestTrue(TEXT("Decode valid HELLO"), DecodeAndroidDatagram(Hello, Packet, Error));
    TestTrue(TEXT("Valid HELLO has no error"), Error.IsEmpty());
    TestEqual(TEXT("HELLO session preserves big-endian uint64"),
        Packet.Header.SessionId, 0x1020304050607080ull);
    TestEqual(TEXT("HELLO pose rate"), Packet.Hello.RequestedPoseHz, uint32(50));
    TestEqual(TEXT("HELLO frame port"), Packet.Hello.FrameTcpPort, uint32(30022));

    const TArray<uint8> PoseBytes = MakePose(12.5, -6.25, 3.75, 271.0, 99.5f);
    TestTrue(TEXT("Decode valid POSE"), DecodeAndroidDatagram(PoseBytes, Packet, Error));
    TestEqual(TEXT("POSE sequence"), Packet.Header.Sequence, uint64(42));
    TestTrue(TEXT("POSE east"), FMath::IsNearlyEqual(Packet.Pose.EastM, 12.5));
    TestTrue(TEXT("POSE north"), FMath::IsNearlyEqual(Packet.Pose.NorthM, -6.25));
    TestTrue(TEXT("POSE up"), FMath::IsNearlyEqual(Packet.Pose.UpM, 3.75));
    TestTrue(TEXT("POSE measured Hz"), FMath::IsNearlyEqual(Packet.Pose.MeasuredSimulatorStateHz, 99.5f));

    TArray<uint8> Truncated = Hello;
    Truncated.Pop();
    TestFalse(TEXT("Reject truncated datagram"), DecodeAndroidDatagram(Truncated, Packet, Error));
    TestTrue(TEXT("Truncated datagram reports error"), !Error.IsEmpty());

    TArray<uint8> WrongPayloadLength = Hello;
    WrongPayloadLength[31] = 27;
    TestFalse(TEXT("Reject mismatched payload_bytes"),
        DecodeAndroidDatagram(WrongPayloadLength, Packet, Error));

    TArray<uint8> ZeroSession = Hello;
    for (int32 Index = 12; Index < 20; ++Index)
        ZeroSession[Index] = 0;
    TestFalse(TEXT("Reject zero session"), DecodeAndroidDatagram(ZeroSession, Packet, Error));

    const TArray<uint8> NanPose = MakePose(
        std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0, 50.0f);
    TestFalse(TEXT("Reject non-finite POSE"), DecodeAndroidDatagram(NanPose, Packet, Error));

    FHeartbeatPayload Heartbeat;
    Heartbeat.SenderMonotonicNs = 1000;
    Heartbeat.LastReceivedSequence = 42;
    Heartbeat.StateFlags = 5;
    TArray<uint8> Response;
    TestTrue(TEXT("Encode HEARTBEAT"), EncodeHeartbeat(77, 2, Heartbeat, Response, Error));
    TestEqual(TEXT("HEARTBEAT wire bytes"), Response.Num(), UdpHeaderBytes + 24);
    TestTrue(TEXT("Decode encoded HEARTBEAT"), DecodeAndroidDatagram(Response, Packet, Error));
    TestEqual(TEXT("HEARTBEAT last sequence"), Packet.Heartbeat.LastReceivedSequence, uint64(42));

    FPongPayload Pong;
    Pong.EchoedSenderMonotonicNs = 2000;
    Pong.PeerMonotonicNs = 2100;
    TestTrue(TEXT("Encode PONG"), EncodePong(77, 3, Pong, Response, Error));
    TestTrue(TEXT("Decode encoded PONG"), DecodeAndroidDatagram(Response, Packet, Error));
    TestEqual(TEXT("PONG echo"), Packet.Pong.EchoedSenderMonotonicNs, uint64(2000));

    FEventPayload Event;
    Event.PeerMonotonicNs = 3000;
    Event.Kind = EEventKind::Collision;
    Event.PoseSequence = 42;
    Event.Reason = TEXT("碰撞 collision");
    TestTrue(TEXT("Encode UTF-8 EVENT"), EncodeEvent(77, 4, Event, Response, Error));
    TestTrue(TEXT("EVENT is bounded"), Response.Num() <= MaximumUdpDatagramBytes);

    Event.Kind = EEventKind::Stop;
    TestFalse(TEXT("Reject STOP without finite score"),
        EncodeEvent(77, 5, Event, Response, Error));
    Event.StopScore = 0.75;
    TestTrue(TEXT("Encode STOP with finite score"),
        EncodeEvent(77, 6, Event, Response, Error));
    Event.Kind = EEventKind::Emergency;
    TestFalse(TEXT("Reject non-STOP with finite score"),
        EncodeEvent(77, 7, Event, Response, Error));
    Event.StopScore = std::numeric_limits<double>::quiet_NaN();

    Event.PoseSequence = 0;
    TestFalse(TEXT("Reject EVENT without pose sequence"),
        EncodeEvent(77, 8, Event, Response, Error));
    Event.PoseSequence = 42;

    Event.Reason = FString::ChrN(MaximumEventReasonBytes + 1, TEXT('x'));
    TestFalse(TEXT("Reject oversized EVENT reason"), EncodeEvent(77, 9, Event, Response, Error));

    FFrameHeader Frame;
    Frame.Format = EFrameFormat::Jpeg;
    Frame.PayloadBytes = 1234;
    Frame.FrameId = 9;
    Frame.PoseSequence = 42;
    Frame.CapturePeerMonotonicNs = 5000;
    Frame.Width = 1440;
    Frame.Height = 1080;
    Frame.PayloadCrc32 = 0xaabbccdd;
    TestTrue(TEXT("Encode OFFR frame header"), EncodeFrameHeader(Frame, Response, Error));
    TestEqual(TEXT("OFFR header is exactly 56 bytes"), Response.Num(), 56);
    TestEqual(TEXT("OFFR magic O"), Response[0], uint8('O'));
    TestEqual(TEXT("OFFR magic F"), Response[1], uint8('F'));
    TestEqual(TEXT("OFFR magic F second"), Response[2], uint8('F'));
    TestEqual(TEXT("OFFR magic R"), Response[3], uint8('R'));
    Frame.PayloadBytes = MaximumFramePayloadBytes + 1;
    TestFalse(TEXT("Reject oversized OFFR payload"), EncodeFrameHeader(Frame, Response, Error));
    return true;
}

#endif
