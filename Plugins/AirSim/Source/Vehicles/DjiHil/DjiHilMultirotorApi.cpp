#include "Vehicles/DjiHil/DjiHilMultirotorApi.h"

#include <sstream>

using namespace msr::airlib;

DjiHilMultirotorApi::DjiHilMultirotorApi(IDjiHilStateProvider* InStateProvider,
    const SensorCollection* InSensors, const GeoPoint& InHomeGeoPoint,
    std::function<std::string()> InTimingProvider,
    std::function<bool(uint32_t, double, const std::string&)> InSafetyEventSender)
    : StateProvider(InStateProvider), Sensors(InSensors), HomeGeoPoint(InHomeGeoPoint),
      TimingProvider(std::move(InTimingProvider)),
      SafetyEventSender(std::move(InSafetyEventSender))
{
}

void DjiHilMultirotorApi::enableApiControl(bool IsEnabled)
{
    bApiControlEnabled.store(IsEnabled, std::memory_order_relaxed);
}

bool DjiHilMultirotorApi::isApiControlEnabled() const
{
    return bApiControlEnabled.load(std::memory_order_relaxed);
}

bool DjiHilMultirotorApi::armDisarm(bool Arm)
{
    unused(Arm);
    RejectCommand();
}

GeoPoint DjiHilMultirotorApi::getHomeGeoPoint() const
{
    return HomeGeoPoint;
}

bool DjiHilMultirotorApi::isReady(std::string& Message) const
{
    djihil::State State;
    if (!ReadState(State)) {
        Message = "DJI HIL state provider has no fresh state";
        return false;
    }
    if (!State.has(djihil::ValidField::Position)
        || !State.has(djihil::ValidField::Orientation) || !State.hasFiniteCore()) {
        Message = "DJI HIL core pose fields are invalid";
        return false;
    }
    Message = "DJI HIL observation-only backend is ready";
    return true;
}

bool DjiHilMultirotorApi::canArm() const
{
    return false;
}

const SensorCollection& DjiHilMultirotorApi::getSensors() const
{
    check(Sensors != nullptr);
    return *Sensors;
}

RCData DjiHilMultirotorApi::getRCData() const
{
    RCData Result;
    Result.is_valid = false;
    Result.is_initialized = false;
    return Result;
}

MultirotorState DjiHilMultirotorApi::getMultirotorState() const
{
    djihil::State State;
    MultirotorState Result;
    Result.can_arm = false;
    Result.rc_data = getRCData();
    Result.ready = isReady(Result.ready_message);
    if (ReadState(State)) {
        Result.kinematics_estimated = State.kinematics;
        Result.gps_location = State.has(djihil::ValidField::Gps) ? State.gps : HomeGeoPoint;
        Result.timestamp = State.source_time_ns;
        Result.landed_state = State.has(djihil::ValidField::LandedState)
            ? State.landed_state : LandedState::Landed;
    }
    else {
        Result.kinematics_estimated = Kinematics::State::zero();
        Result.gps_location = HomeGeoPoint;
        Result.timestamp = 0;
        Result.landed_state = LandedState::Landed;
    }
    return Result;
}

std::string DjiHilMultirotorApi::getDjiHilStatus() const
{
    std::string Message;
    const bool bReady = isReady(Message);
    djihil::State State;
    const bool bHasState = ReadState(State);
    std::ostringstream Stream;
    const FString Backend = StateProvider != nullptr
        ? StateProvider->GetBackendName() : TEXT("unavailable");
    Stream << "{\"available\":true,\"backend\":\"" << TCHAR_TO_UTF8(*Backend) << "\""
           << ",\"observation_only\":true"
           << ",\"ready\":" << (bReady ? "true" : "false")
           << ",\"can_arm\":false"
           << ",\"api_control_enabled\":"
           << (bApiControlEnabled.load(std::memory_order_relaxed) ? "true" : "false")
           << ",\"session_id\":" << (bHasState ? State.session_id : 0)
           << ",\"frame_id\":" << (bHasState ? State.frame_id : 0)
           << ",\"source_time_ns\":" << (bHasState ? State.source_time_ns : 0)
           << ",\"message\":\"" << Message << "\"}";
    return Stream.str();
}

std::string DjiHilMultirotorApi::getDjiHilCapabilities() const
{
    const FString Backend = StateProvider != nullptr
        ? StateProvider->GetBackendName() : TEXT("unavailable");
    std::ostringstream Stream;
    Stream << "{\"available\":true,\"backend\":\"" << TCHAR_TO_UTF8(*Backend)
           << "\",\"observation_only\":true,"
              "\"state\":[\"pose\",\"kinematics\",\"gps\",\"landed_state\"],"
              "\"sensors\":[\"camera\",\"virtual_sensors\"],\"commands\":[]}";
    return Stream.str();
}

std::string DjiHilMultirotorApi::getDjiHilTiming() const
{
    if (!TimingProvider)
        return "{\"available\":false}";
    const std::string Timing = TimingProvider();
    if (Timing.empty() || Timing.front() != '{')
        return "{\"available\":false}";
    return "{\"available\":true," + Timing.substr(1);
}

std::string DjiHilMultirotorApi::getDjiHilDecodedState() const
{
    djihil::State State;
    if (!ReadState(State))
        return "{\"available\":false}";
    const auto& Position = State.kinematics.pose.position;
    const auto& Orientation = State.kinematics.pose.orientation;
    std::ostringstream Stream;
    Stream << "{\"available\":true"
           << ",\"protocol_version\":" << State.protocol_version
           << ",\"session_id\":" << State.session_id
           << ",\"frame_id\":" << State.frame_id
           << ",\"source_time_ns\":" << State.source_time_ns
           << ",\"host_monotonic_ns\":" << State.host_monotonic_ns
           << ",\"receive_sequence\":" << State.receive_sequence
           << ",\"valid_fields\":" << State.valid_fields
           << ",\"position_ned_m\":[" << Position.x() << ',' << Position.y() << ',' << Position.z() << ']'
           << ",\"orientation_wxyz\":[" << Orientation.w() << ',' << Orientation.x()
           << ',' << Orientation.y() << ',' << Orientation.z() << ']'
           << ",\"gps_lla\":[" << State.gps.latitude << ',' << State.gps.longitude
           << ',' << State.gps.altitude << "]}";
    return Stream.str();
}

bool DjiHilMultirotorApi::sendDjiHilSafetyEvent(uint32_t Kind, double Score,
    const std::string& Reason)
{
    if (!SafetyEventSender)
        return false;
    return SafetyEventSender(Kind, Score, Reason);
}

void DjiHilMultirotorApi::resetImplementation()
{
    MultirotorApiBase::resetImplementation();
    if (StateProvider != nullptr)
        StateProvider->Reset();
    bApiControlEnabled.store(false, std::memory_order_relaxed);
}

void DjiHilMultirotorApi::commandMotorPWMs(float, float, float, float) { RejectCommand(); }
void DjiHilMultirotorApi::commandRollPitchYawrateThrottle(float, float, float, float) { RejectCommand(); }
void DjiHilMultirotorApi::commandRollPitchYawZ(float, float, float, float) { RejectCommand(); }
void DjiHilMultirotorApi::commandRollPitchYawThrottle(float, float, float, float) { RejectCommand(); }
void DjiHilMultirotorApi::commandRollPitchYawrateZ(float, float, float, float) { RejectCommand(); }
void DjiHilMultirotorApi::commandAngleRatesZ(float, float, float, float) { RejectCommand(); }
void DjiHilMultirotorApi::commandAngleRatesThrottle(float, float, float, float) { RejectCommand(); }
void DjiHilMultirotorApi::commandVelocity(float, float, float, const YawMode&) { RejectCommand(); }
void DjiHilMultirotorApi::commandVelocityZ(float, float, float, const YawMode&) { RejectCommand(); }
void DjiHilMultirotorApi::commandPosition(float, float, float, const YawMode&) { RejectCommand(); }
void DjiHilMultirotorApi::setControllerGains(uint8_t, const std::vector<float>&,
    const std::vector<float>&, const std::vector<float>&) { RejectCommand(); }

Kinematics::State DjiHilMultirotorApi::getKinematicsEstimated() const
{
    djihil::State State;
    return ReadState(State) ? State.kinematics : Kinematics::State::zero();
}

LandedState DjiHilMultirotorApi::getLandedState() const
{
    djihil::State State;
    return ReadState(State) && State.has(djihil::ValidField::LandedState)
        ? State.landed_state : LandedState::Landed;
}

GeoPoint DjiHilMultirotorApi::getGpsLocation() const
{
    djihil::State State;
    return ReadState(State) && State.has(djihil::ValidField::Gps) ? State.gps : HomeGeoPoint;
}

const MultirotorApiParams& DjiHilMultirotorApi::getMultirotorApiParams() const
{
    return ApiParams;
}

float DjiHilMultirotorApi::getCommandPeriod() const { return 0.02f; }
float DjiHilMultirotorApi::getTakeoffZ() const { return -3.0f; }
float DjiHilMultirotorApi::getDistanceAccuracy() const { return ApiParams.distance_accuracy; }
void DjiHilMultirotorApi::beforeTask() { RejectCommand(); }

void DjiHilMultirotorApi::RejectCommand()
{
    throw VehicleCommandNotImplementedException(
        "DjiHil backend is observation-only; vehicle commands are disabled");
}

bool DjiHilMultirotorApi::ReadState(djihil::State& OutState) const
{
    return StateProvider != nullptr && StateProvider->GetLatestState(OutState);
}
