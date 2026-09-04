#include "Vehicles/DjiHil/OpenFlyHilFrameServer.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "IPAddress.h"
#include "Misc/Crc.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UnrealImageCapture.h"
#include "Vehicles/DjiHil/OpenFlyHilUdpServer.h"

#include <cctype>

namespace openfly::hil
{
FOpenFlyHilFrameServer::FOpenFlyHilFrameServer(const FFrameServerConfig& InConfig,
    const UnrealImageCapture& InImageCapture, const FOpenFlyHilUdpServer& InUdpServer)
    : Config(InConfig), ImageCapture(InImageCapture), UdpServer(InUdpServer)
{
}

FOpenFlyHilFrameServer::~FOpenFlyHilFrameServer()
{
    Shutdown();
}

bool FOpenFlyHilFrameServer::Start(FString& OutError)
{
    OutError.Reset();
    if (Thread.IsValid() || ClientSocket != nullptr) {
        OutError = TEXT("openfly.hil frame sender is already started");
        return false;
    }
    if (Config.Width == 0 || Config.Height == 0 || Config.MaximumPayloadBytes == 0
        || Config.MaximumPayloadBytes > MaximumFramePayloadBytes
        || Config.JpegQuality < 1 || Config.JpegQuality > 100
        || Config.MaximumFrameRate == 0 || Config.MaximumFrameRate > 240) {
        OutError = TEXT("invalid openfly.hil frame sender limits");
        return false;
    }
    bStopRequested.Store(false);
    Thread.Reset(FRunnableThread::Create(this, TEXT("OpenFlyHilFrames"),
        0, TPri_BelowNormal));
    if (!Thread.IsValid()) {
        OutError = TEXT("failed to create openfly.hil frame worker thread");
        return false;
    }
    return true;
}

void FOpenFlyHilFrameServer::Shutdown()
{
    Stop();
    if (Thread.IsValid()) {
        Thread->WaitForCompletion();
        Thread.Reset();
    }
    CloseClient();
}

FFrameServerStats FOpenFlyHilFrameServer::GetStats() const
{
    FScopeLock Lock(&StatsMutex);
    return Stats;
}

uint32 FOpenFlyHilFrameServer::Run()
{
    const double MinimumFrameSeconds = 1.0 / static_cast<double>(Config.MaximumFrameRate);
    while (!bStopRequested.Load()) {
        const FPeerSnapshot Peer = UdpServer.GetPeerSnapshot();
        if (ClientSocket == nullptr) {
            const double NowSeconds = FPlatformTime::Seconds();
            if (Peer.bHasPeer && Peer.FrameTcpPort != 0
                && NowSeconds >= NextConnectAttemptSeconds) {
                if (!ConnectToPeer(Peer))
                    NextConnectAttemptSeconds = NowSeconds + 0.25;
            }
            FPlatformProcess::SleepNoStats(0.005f);
            continue;
        }

        if (!Peer.bHasPeer || Peer.PeerIp != ClientIp
            || Peer.SessionId != ClientSessionId || Peer.FrameTcpPort != ClientPort) {
            UE_LOG(LogTemp, Warning,
                TEXT("OpenFly HIL frame TCP closing: peer changed fresh=%d ip=%s/%s session=%llu/%llu port=%u/%u"),
                Peer.bHasPeer ? 1 : 0, *ClientIp, *Peer.PeerIp,
                ClientSessionId, Peer.SessionId, ClientPort, Peer.FrameTcpPort);
            CloseClient();
            NextConnectAttemptSeconds = FPlatformTime::Seconds() + 0.5;
            continue;
        }
        if (ClientClosedOrSentUnexpectedData()) {
            UE_LOG(LogTemp, Warning,
                TEXT("OpenFly HIL frame TCP closing: Android closed the stream or sent unexpected data"));
            CloseClient();
            NextConnectAttemptSeconds = FPlatformTime::Seconds() + 0.5;
            continue;
        }

        const double Started = FPlatformTime::Seconds();
        bool bCaptureCompleted = false;
        if (!CaptureAndSendFrame(bCaptureCompleted)) {
            UE_LOG(LogTemp, Warning,
                TEXT("OpenFly HIL frame TCP closing: frame send failed or timed out"));
            CloseClient();
            NextConnectAttemptSeconds = FPlatformTime::Seconds() + 0.5;
            continue;
        }
        if (bCaptureCompleted) {
            const double Remaining = MinimumFrameSeconds
                - (FPlatformTime::Seconds() - Started);
            if (Remaining > 0.0)
                FPlatformProcess::SleepNoStats(static_cast<float>(Remaining));
        }
        else {
            // A GPU readback that finishes just after the frame-rate sleep used
            // to remain unobserved until the following 33 ms tick.  At 1440p a
            // capture slightly slower than one 30 Hz interval was therefore
            // quantized all the way down to 15 Hz.  Poll pending readbacks at a
            // low cost while retaining the configured limit after every
            // completed capture (including rejected frames).
            FPlatformProcess::SleepNoStats(0.001f);
        }
    }
    return 0;
}

void FOpenFlyHilFrameServer::Stop()
{
    bStopRequested.Store(true);
}

bool FOpenFlyHilFrameServer::ConnectToPeer(const FPeerSnapshot& Peer)
{
    if (!Peer.bHasPeer || Peer.PeerIp.IsEmpty() || Peer.FrameTcpPort == 0)
        return false;

    bool bValidIp = false;
    TSharedRef<FInternetAddr> Remote =
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
    Remote->SetIp(*Peer.PeerIp, bValidIp);
    Remote->SetPort(Peer.FrameTcpPort);
    if (!bValidIp)
        return false;

    FSocket* Candidate = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateSocket(
        NAME_Stream, TEXT("OpenFlyHilAndroidFrames"), false);
    if (Candidate == nullptr)
        return false;
    Candidate->SetNonBlocking(false);
    Candidate->SetNoDelay(true);
    int32 ActualSendBufferBytes = 0;
    Candidate->SetSendBufferSize(2 * 1024 * 1024, ActualSendBufferBytes);
    if (!Candidate->Connect(*Remote)) {
        Candidate->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Candidate);
        FScopeLock Lock(&StatsMutex);
        ++Stats.RejectedConnections;
        return false;
    }
    Candidate->SetNonBlocking(true);
    ClientSocket = Candidate;
    ClientIp = Peer.PeerIp;
    ClientSessionId = Peer.SessionId;
    ClientPort = Peer.FrameTcpPort;
    NextFrameId = 1;
    {
        FScopeLock Lock(&StatsMutex);
        Stats.bClientConnected = true;
        ++Stats.AcceptedConnections;
    }
    UE_LOG(LogTemp, Display, TEXT("OpenFly HIL frame TCP connected to Android %s:%u session=%llu"),
        *ClientIp, ClientPort, ClientSessionId);
    return true;
}

void FOpenFlyHilFrameServer::CloseClient()
{
    if (ClientSocket != nullptr) {
        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
        ClientSocket = nullptr;
    }
    ClientIp.Reset();
    ClientSessionId = 0;
    ClientPort = 0;
    NextFrameId = 1;
    FScopeLock Lock(&StatsMutex);
    Stats.bClientConnected = false;
}

bool FOpenFlyHilFrameServer::ClientClosedOrSentUnexpectedData() const
{
    if (ClientSocket == nullptr
        || ClientSocket->GetConnectionState() != SCS_Connected)
        return true;
    // OFFR is server-to-client only. Read readiness means FIN/RST or protocol
    // input that v1 never permits; either case releases the single-client slot
    // immediately instead of waiting for the send buffer to fill.
    if (!ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
        return false;
    uint8 Byte = 0;
    int32 ReadBytes = 0;
    ClientSocket->Recv(&Byte, 1, ReadBytes, ESocketReceiveFlags::Peek);
    return true;
}

bool FOpenFlyHilFrameServer::CaptureAndSendFrame(bool& bOutCaptureCompleted)
{
    bOutCaptureCompleted = false;
    UnrealImageCapture::AsyncImageFrame Frame;
    if (!ImageCapture.tryGetAsyncSceneCapture(
            TCHAR_TO_UTF8(*Config.CameraName), Frame))
        return true;
    bOutCaptureCompleted = true;

    if (Frame.capture_started_seconds > 0.0
        && Frame.capture_completed_seconds >= Frame.capture_started_seconds) {
        const uint64 CaptureMicros = static_cast<uint64>(
            (Frame.capture_completed_seconds - Frame.capture_started_seconds) * 1000000.0);
        FScopeLock Lock(&StatsMutex);
        ++Stats.CaptureSamples;
        Stats.CaptureTotalMicros += CaptureMicros;
        Stats.CaptureMaximumMicros = FMath::Max(Stats.CaptureMaximumMicros, CaptureMicros);
    }

    const uint64 ExpectedBgraBytes = static_cast<uint64>(Config.Width)
        * static_cast<uint64>(Config.Height) * 4;
    if (Frame.width != static_cast<int32>(Config.Width)
        || Frame.height != static_cast<int32>(Config.Height)
        || Frame.image_data_uint8.size() != ExpectedBgraBytes) {
        FScopeLock Lock(&StatsMutex);
        ++Stats.CaptureFailures;
        if (Stats.CaptureFailures <= 3 || Stats.CaptureFailures % 300 == 0) {
            UE_LOG(LogTemp, Warning,
                TEXT("OpenFly HIL async frame capture invalid: size=%dx%d expected=%ux%u bytes=%llu message=%s failures=%llu"),
                Frame.width, Frame.height, Config.Width, Config.Height,
                static_cast<uint64>(Frame.image_data_uint8.size()),
                UTF8_TO_TCHAR(Frame.message.c_str()), Stats.CaptureFailures);
        }
        return true;
    }

    uint64 PoseSequence = 0;
    const bool bHasRenderPose = ExtractUnsignedJsonField(Frame.message,
        "render_frame_id", PoseSequence) && PoseSequence != 0;
    if (!bHasRenderPose) {
        const FPeerSnapshot Peer = UdpServer.GetPeerSnapshot();
        if (Peer.LastPoseSequence != 0) {
            FScopeLock Lock(&StatsMutex);
            ++Stats.CaptureFailures;
            if (Stats.CaptureFailures <= 3 || Stats.CaptureFailures % 300 == 0) {
                UE_LOG(LogTemp, Warning,
                    TEXT("OpenFly HIL async frame metadata missing render_frame_id after POSE: message=%s failures=%llu"),
                    UTF8_TO_TCHAR(Frame.message.c_str()), Stats.CaptureFailures);
            }
            return true;
        }
        PoseSequence = 1;
        if (!bLoggedBootstrapFrame) {
            UE_LOG(LogTemp, Warning,
                TEXT("OpenFly HIL sending bootstrap frames before the first Android POSE; pose_sequence=1 until render state is available"));
            bLoggedBootstrapFrame = true;
        }
    }

    TArray<uint8> Encoded;
    const double EncodeStarted = FPlatformTime::Seconds();
    const bool bEncoded = Config.Format == EFrameFormat::Png
        ? EncodePng(Frame.image_data_uint8, Frame.width, Frame.height, Encoded)
        : EncodeJpeg(Frame.image_data_uint8, Frame.width, Frame.height, Encoded);
    if (!bEncoded || Encoded.IsEmpty()
        || Encoded.Num() > static_cast<int32>(Config.MaximumPayloadBytes)) {
        const uint64 EncodeMicros = static_cast<uint64>(
            (FPlatformTime::Seconds() - EncodeStarted) * 1000000.0);
        FScopeLock Lock(&StatsMutex);
        ++Stats.EncodeSamples;
        Stats.EncodeTotalMicros += EncodeMicros;
        Stats.EncodeMaximumMicros = FMath::Max(Stats.EncodeMaximumMicros, EncodeMicros);
        ++Stats.EncodeFailures;
        return true;
    }
    {
        const uint64 EncodeMicros = static_cast<uint64>(
            (FPlatformTime::Seconds() - EncodeStarted) * 1000000.0);
        FScopeLock Lock(&StatsMutex);
        ++Stats.EncodeSamples;
        Stats.EncodeTotalMicros += EncodeMicros;
        Stats.EncodeMaximumMicros = FMath::Max(Stats.EncodeMaximumMicros, EncodeMicros);
    }

    FFrameHeader Header;
    Header.Format = Config.Format;
    Header.PayloadBytes = static_cast<uint32>(Encoded.Num());
    Header.FrameId = NextFrameId;
    Header.PoseSequence = PoseSequence;
    const uint64 CaptureHostMonotonicNs = MonotonicNowNs();
    const FPeerSnapshot CapturePeer = UdpServer.GetPeerSnapshot();
    Header.CapturePeerMonotonicNs = CaptureHostMonotonicNs;
    if (CapturePeer.bHasPeerMonotonicReference
        && CaptureHostMonotonicNs >= CapturePeer.PeerMonotonicReferenceHostNs) {
        Header.CapturePeerMonotonicNs = CapturePeer.PeerMonotonicReferenceNs
            + (CaptureHostMonotonicNs - CapturePeer.PeerMonotonicReferenceHostNs);
    }
    Header.Width = Config.Width;
    Header.Height = Config.Height;
    Header.PayloadCrc32 = FCrc::MemCrc32(Encoded.GetData(), Encoded.Num());
    TArray<uint8> HeaderBytes;
    FString Error;
    if (!EncodeFrameHeader(Header, HeaderBytes, Error)) {
        FScopeLock Lock(&StatsMutex);
        ++Stats.EncodeFailures;
        return true;
    }
    const double SendStarted = FPlatformTime::Seconds();
    if (!SendAll(HeaderBytes) || !SendAll(Encoded)) {
        const uint64 SendMicros = static_cast<uint64>(
            (FPlatformTime::Seconds() - SendStarted) * 1000000.0);
        FScopeLock Lock(&StatsMutex);
        ++Stats.SendSamples;
        Stats.SendTotalMicros += SendMicros;
        Stats.SendMaximumMicros = FMath::Max(Stats.SendMaximumMicros, SendMicros);
        ++Stats.SendFailures;
        return false;
    }
    const uint64 SendMicros = static_cast<uint64>(
        (FPlatformTime::Seconds() - SendStarted) * 1000000.0);

    ++NextFrameId;
    {
        FScopeLock Lock(&StatsMutex);
        ++Stats.CompletedFrames;
        Stats.BytesSent += HeaderBytes.Num() + Encoded.Num();
        Stats.LastFrameId = Header.FrameId;
        Stats.LastPoseSequence = PoseSequence;
        ++Stats.SendSamples;
        Stats.SendTotalMicros += SendMicros;
        Stats.SendMaximumMicros = FMath::Max(Stats.SendMaximumMicros, SendMicros);
    }
    return true;
}

bool FOpenFlyHilFrameServer::EncodeJpeg(const std::vector<uint8>& Bgra,
    int32 Width, int32 Height, TArray<uint8>& OutJpeg) const
{
    OutJpeg.Reset();
    if (Bgra.size() != static_cast<size_t>(Width) * Height * 4)
        return false;
    IImageWrapperModule& Module =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
    if (!Wrapper.IsValid() || !Wrapper->SetRaw(Bgra.data(), Bgra.size(),
            Width, Height, ERGBFormat::BGRA, 8))
        return false;
    OutJpeg = Wrapper->GetCompressed(static_cast<int32>(Config.JpegQuality));
    return !OutJpeg.IsEmpty();
}

bool FOpenFlyHilFrameServer::EncodePng(const std::vector<uint8>& Bgra,
    int32 Width, int32 Height, TArray<uint8>& OutPng) const
{
    OutPng.Reset();
    if (Bgra.size() != static_cast<size_t>(Width) * Height * 4)
        return false;
    IImageWrapperModule& Module =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
    if (!Wrapper.IsValid() || !Wrapper->SetRaw(Bgra.data(), Bgra.size(),
            Width, Height, ERGBFormat::BGRA, 8))
        return false;
    OutPng = Wrapper->GetCompressed();
    return !OutPng.IsEmpty();
}

bool FOpenFlyHilFrameServer::SendAll(TArrayView<const uint8> Bytes)
{
    int32 Offset = 0;
    const double Deadline = FPlatformTime::Seconds() + 1.0;
    while (Offset < Bytes.Num() && !bStopRequested.Load()) {
        int32 Sent = 0;
        if (ClientSocket->Send(Bytes.GetData() + Offset, Bytes.Num() - Offset, Sent)
            && Sent > 0) {
            Offset += Sent;
            continue;
        }
        if (ClientSocket->GetConnectionState() != SCS_Connected)
            return false;
        if (FPlatformTime::Seconds() >= Deadline)
            return false;

        // A non-blocking socket can remain temporarily non-writable while the
        // peer drains its receive buffer.  A single 20 ms wait timeout is only
        // transient backpressure; the one-second aggregate deadline above is
        // the actual failure boundary.
        ClientSocket->Wait(ESocketWaitConditions::WaitForWrite,
            FTimespan::FromMilliseconds(20));
    }
    return Offset == Bytes.Num();
}

bool FOpenFlyHilFrameServer::ExtractUnsignedJsonField(const std::string& Json,
    const char* Field, uint64& OutValue)
{
    const std::string Needle = std::string("\"") + Field + "\":";
    size_t Offset = Json.find(Needle);
    if (Offset == std::string::npos)
        return false;
    Offset += Needle.size();
    if (Offset >= Json.size() || !std::isdigit(static_cast<unsigned char>(Json[Offset])))
        return false;
    uint64 Value = 0;
    while (Offset < Json.size() && std::isdigit(static_cast<unsigned char>(Json[Offset]))) {
        const uint32 Digit = static_cast<uint32>(Json[Offset++] - '0');
        if (Value > (MAX_uint64 - Digit) / 10)
            return false;
        Value = Value * 10 + Digit;
    }
    OutValue = Value;
    return true;
}

uint64 FOpenFlyHilFrameServer::MonotonicNowNs() const
{
    return static_cast<uint64>(FPlatformTime::Seconds() * 1000000000.0);
}
}
