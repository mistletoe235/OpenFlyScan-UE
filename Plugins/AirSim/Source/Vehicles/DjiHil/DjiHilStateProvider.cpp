#include "Vehicles/DjiHil/DjiHilStateProvider.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "String/LexFromString.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    bool TryGetUInt64(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, uint64_t& OutValue)
    {
        const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
        if (!Value.IsValid())
            return false;

        if (Value->Type == EJson::String) {
            FString Text;
            if (!Value->TryGetString(Text))
                return false;
            uint64 ParsedValue = 0;
            if (!LexTryParseString(ParsedValue, *Text))
                return false;
            OutValue = static_cast<uint64_t>(ParsedValue);
            return true;
        }

        if (Value->Type != EJson::Number)
            return false;
        double Number = 0.0;
        // JSON numbers are IEEE-754 doubles. Require string encoding for values
        // above the exact integer range so frame/time identifiers cannot round.
        constexpr double MaxExactJsonInteger = 9007199254740991.0;
        if (!Value->TryGetNumber(Number) || Number < 0.0
            || Number > MaxExactJsonInteger || !FMath::IsFinite(Number)
            || Number != FMath::FloorToDouble(Number))
            return false;
        OutValue = static_cast<uint64_t>(Number);
        return true;
    }

    bool TryGetVector3(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, msr::airlib::Vector3r& OutValue)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object->TryGetArrayField(Name, Values) || Values == nullptr || Values->Num() != 3)
            return false;
        const double X = (*Values)[0]->AsNumber();
        const double Y = (*Values)[1]->AsNumber();
        const double Z = (*Values)[2]->AsNumber();
        if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
            return false;
        OutValue = msr::airlib::Vector3r(static_cast<msr::airlib::real_T>(X),
            static_cast<msr::airlib::real_T>(Y), static_cast<msr::airlib::real_T>(Z));
        return true;
    }

    bool TryGetQuaternionWxyz(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name,
        msr::airlib::Quaternionr& OutValue)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object->TryGetArrayField(Name, Values) || Values == nullptr || Values->Num() != 4)
            return false;
        const double W = (*Values)[0]->AsNumber();
        const double X = (*Values)[1]->AsNumber();
        const double Y = (*Values)[2]->AsNumber();
        const double Z = (*Values)[3]->AsNumber();
        if (!FMath::IsFinite(W) || !FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
            return false;
        OutValue = msr::airlib::Quaternionr(static_cast<msr::airlib::real_T>(W),
            static_cast<msr::airlib::real_T>(X), static_cast<msr::airlib::real_T>(Y),
            static_cast<msr::airlib::real_T>(Z));
        if (OutValue.squaredNorm() <= 1.0e-8f)
            return false;
        OutValue.normalize();
        return true;
    }
}

bool FDjiHilReplayStateProvider::LoadJsonLines(const FString& ReplayPath, FString& OutError)
{
    TArray<FString> Lines;
    if (!FFileHelper::LoadFileToStringArray(Lines, *ReplayPath)) {
        OutError = FString::Printf(TEXT("Unable to read DJI HIL replay: %s"), *ReplayPath);
        return false;
    }

    TArray<msr::airlib::djihil::State> ParsedFrames;
    ParsedFrames.Reserve(Lines.Num());
    for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex) {
        const FString Line = Lines[LineIndex].TrimStartAndEnd();
        if (Line.IsEmpty())
            continue;
        if (Line.Len() > 65536) {
            OutError = FString::Printf(TEXT("DJI HIL replay line %d exceeds 64 KiB"), LineIndex + 1);
            return false;
        }

        msr::airlib::djihil::State State;
        if (!ParseStateLine(Line, LineIndex + 1, State, OutError))
            return false;
        if (!ParsedFrames.IsEmpty()) {
            const auto& Previous = ParsedFrames.Last();
            if (State.session_id != Previous.session_id) {
                OutError = FString::Printf(
                    TEXT("DJI HIL replay line %d changes session_id; use one session per replay file"),
                    LineIndex + 1);
                return false;
            }
            if (State.frame_id <= Previous.frame_id || State.source_time_ns < Previous.source_time_ns) {
                OutError = FString::Printf(TEXT("DJI HIL replay line %d is not monotonic"), LineIndex + 1);
                return false;
            }
        }
        ParsedFrames.Add(State);
    }

    if (ParsedFrames.IsEmpty()) {
        OutError = TEXT("DJI HIL replay contains no state frames");
        return false;
    }

    FScopeLock Lock(&Mutex);
    Frames = MoveTemp(ParsedFrames);
    LatestFrameIndex = 0;
    OutError.Reset();
    return true;
}

void FDjiHilReplayStateProvider::Reset()
{
    FScopeLock Lock(&Mutex);
    LatestFrameIndex = Frames.IsEmpty() ? INDEX_NONE : 0;
}

bool FDjiHilReplayStateProvider::AdvanceToSourceTime(uint64 TargetSourceTimeNs)
{
    FScopeLock Lock(&Mutex);
    if (Frames.IsEmpty())
        return false;
    if (LatestFrameIndex == INDEX_NONE || TargetSourceTimeNs < Frames[LatestFrameIndex].source_time_ns)
        LatestFrameIndex = 0;
    while (LatestFrameIndex + 1 < Frames.Num()
        && Frames[LatestFrameIndex + 1].source_time_ns <= TargetSourceTimeNs)
        ++LatestFrameIndex;
    return true;
}

bool FDjiHilReplayStateProvider::GetLatestState(msr::airlib::djihil::State& OutState) const
{
    FScopeLock Lock(&Mutex);
    if (!Frames.IsValidIndex(LatestFrameIndex))
        return false;
    OutState = Frames[LatestFrameIndex];
    return true;
}

bool FDjiHilReplayStateProvider::SampleRenderState(uint64 TargetSourceTimeNs,
    msr::airlib::djihil::State& OutState) const
{
    FScopeLock Lock(&Mutex);
    if (Frames.IsEmpty())
        return false;
    if (TargetSourceTimeNs <= Frames[0].source_time_ns) {
        OutState = Frames[0];
        return true;
    }
    int32 Low = 1;
    int32 High = Frames.Num() - 1;
    while (Low < High) {
        const int32 Mid = Low + (High - Low) / 2;
        if (Frames[Mid].source_time_ns < TargetSourceTimeNs)
            Low = Mid + 1;
        else
            High = Mid;
    }

    const auto& A = Frames[Low - 1];
    const auto& B = Frames[Low];
    if (A.session_id != B.session_id || B.source_time_ns == A.source_time_ns) {
        OutState = B;
        return true;
    }
    const long double Numerator = TargetSourceTimeNs - A.source_time_ns;
    const long double Denominator = B.source_time_ns - A.source_time_ns;
    OutState = msr::airlib::djihil::interpolate(A, B,
        static_cast<msr::airlib::real_T>(Numerator / Denominator));
    return true;
}

bool FDjiHilReplayStateProvider::GetSourceTimeRange(uint64& OutFirstSourceTimeNs,
    uint64& OutLastSourceTimeNs) const
{
    FScopeLock Lock(&Mutex);
    if (Frames.IsEmpty())
        return false;
    OutFirstSourceTimeNs = Frames[0].source_time_ns;
    OutLastSourceTimeNs = Frames.Last().source_time_ns;
    return true;
}

int32 FDjiHilReplayStateProvider::GetFrameCount() const
{
    FScopeLock Lock(&Mutex);
    return Frames.Num();
}

bool FDjiHilReplayStateProvider::ParseStateLine(const FString& Line, int32 LineNumber,
    msr::airlib::djihil::State& OutState, FString& OutError)
{
    TSharedPtr<FJsonObject> Object;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
    if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid()) {
        OutError = FString::Printf(TEXT("DJI HIL replay line %d is not valid JSON"), LineNumber);
        return false;
    }

    uint64_t ProtocolVersion = 0;
    if (!TryGetUInt64(Object, TEXT("protocol_version"), ProtocolVersion)
        || ProtocolVersion != 1
        || !TryGetUInt64(Object, TEXT("session_id"), OutState.session_id)
        || !TryGetUInt64(Object, TEXT("frame_id"), OutState.frame_id)
        || !TryGetUInt64(Object, TEXT("source_time_ns"), OutState.source_time_ns)) {
        OutError = FString::Printf(TEXT("DJI HIL replay line %d has invalid protocol/session/frame/time"), LineNumber);
        return false;
    }
    OutState.protocol_version = static_cast<uint32>(ProtocolVersion);

    if ((Object->HasField(TEXT("host_monotonic_ns"))
            && !TryGetUInt64(Object, TEXT("host_monotonic_ns"), OutState.host_monotonic_ns))
        || (Object->HasField(TEXT("receive_sequence"))
            && !TryGetUInt64(Object, TEXT("receive_sequence"), OutState.receive_sequence))) {
        OutError = FString::Printf(TEXT("DJI HIL replay line %d has invalid host/receive timing"), LineNumber);
        return false;
    }
    if (!TryGetUInt64(Object, TEXT("valid_fields"), OutState.valid_fields)) {
        OutError = FString::Printf(TEXT("DJI HIL replay line %d has invalid valid_fields"), LineNumber);
        return false;
    }
    const uint64 SupportedFields =
        msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::Position)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::Orientation)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::LinearVelocity)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::AngularVelocity)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::LinearAcceleration)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::AngularAcceleration)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::Gps)
        | msr::airlib::djihil::mask(msr::airlib::djihil::ValidField::LandedState);
    if ((OutState.valid_fields & ~SupportedFields) != 0) {
        OutError = FString::Printf(
            TEXT("DJI HIL replay line %d requests unsupported validity fields"), LineNumber);
        return false;
    }

    const auto RequireVector = [&](msr::airlib::djihil::ValidField Field, const TCHAR* Name,
                                   msr::airlib::Vector3r& Value) -> bool {
        return !OutState.has(Field) || TryGetVector3(Object, Name, Value);
    };
    if (!RequireVector(msr::airlib::djihil::ValidField::Position, TEXT("position_ned_m"), OutState.kinematics.pose.position)
        || (OutState.has(msr::airlib::djihil::ValidField::Orientation)
            && !TryGetQuaternionWxyz(Object, TEXT("orientation_wxyz"), OutState.kinematics.pose.orientation))
        || !RequireVector(msr::airlib::djihil::ValidField::LinearVelocity, TEXT("linear_velocity_ned_mps"), OutState.kinematics.twist.linear)
        || !RequireVector(msr::airlib::djihil::ValidField::AngularVelocity, TEXT("angular_velocity_body_rps"), OutState.kinematics.twist.angular)
        || !RequireVector(msr::airlib::djihil::ValidField::LinearAcceleration, TEXT("linear_acceleration_ned_mps2"), OutState.kinematics.accelerations.linear)
        || !RequireVector(msr::airlib::djihil::ValidField::AngularAcceleration, TEXT("angular_acceleration_body_rps2"), OutState.kinematics.accelerations.angular)) {
        OutError = FString::Printf(TEXT("DJI HIL replay line %d is missing a valid vector/quaternion field"), LineNumber);
        return false;
    }

    if (OutState.has(msr::airlib::djihil::ValidField::Gps)) {
        const TArray<TSharedPtr<FJsonValue>>* Gps = nullptr;
        if (!Object->TryGetArrayField(TEXT("gps_lla"), Gps) || Gps == nullptr || Gps->Num() != 3) {
            OutError = FString::Printf(TEXT("DJI HIL replay line %d is missing gps_lla"), LineNumber);
            return false;
        }
        const double Latitude = (*Gps)[0]->AsNumber();
        const double Longitude = (*Gps)[1]->AsNumber();
        const double Altitude = (*Gps)[2]->AsNumber();
        if (!FMath::IsFinite(Latitude) || !FMath::IsFinite(Longitude) || !FMath::IsFinite(Altitude)
            || Latitude < -90.0 || Latitude > 90.0 || Longitude < -180.0 || Longitude > 180.0) {
            OutError = FString::Printf(TEXT("DJI HIL replay line %d has invalid gps_lla"), LineNumber);
            return false;
        }
        OutState.gps.set(Latitude, Longitude, static_cast<float>(Altitude));
    }

    if (OutState.has(msr::airlib::djihil::ValidField::LandedState)) {
        int32 Landed = 0;
        if (!Object->TryGetNumberField(TEXT("landed_state"), Landed) || (Landed != 0 && Landed != 1)) {
            OutError = FString::Printf(TEXT("DJI HIL replay line %d has invalid landed_state"), LineNumber);
            return false;
        }
        OutState.landed_state = Landed == 0 ? msr::airlib::LandedState::Landed : msr::airlib::LandedState::Flying;
    }

    if (!OutState.hasFiniteCore()) {
        OutError = FString::Printf(TEXT("DJI HIL replay line %d contains non-finite core state"), LineNumber);
        return false;
    }
    return true;
}
