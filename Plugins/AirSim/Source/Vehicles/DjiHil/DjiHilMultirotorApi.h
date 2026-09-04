#pragma once

#include "Vehicles/DjiHil/DjiHilStateProvider.h"
#include "vehicles/multirotor/api/MultirotorApiBase.hpp"

#include <atomic>

class DjiHilMultirotorApi final : public msr::airlib::MultirotorApiBase
{
public:
    DjiHilMultirotorApi(IDjiHilStateProvider* StateProvider,
        const msr::airlib::SensorCollection* Sensors,
        const msr::airlib::GeoPoint& HomeGeoPoint,
        std::function<std::string()> TimingProvider,
        std::function<bool(uint32_t, double, const std::string&)> SafetyEventSender = {});

    virtual void enableApiControl(bool IsEnabled) override;
    virtual bool isApiControlEnabled() const override;
    virtual bool armDisarm(bool Arm) override;
    virtual msr::airlib::GeoPoint getHomeGeoPoint() const override;
    virtual bool isReady(std::string& Message) const override;
    virtual bool canArm() const override;
    virtual const msr::airlib::SensorCollection& getSensors() const override;
    virtual msr::airlib::RCData getRCData() const override;
    virtual msr::airlib::MultirotorState getMultirotorState() const override;
    virtual std::string getDjiHilStatus() const override;
    virtual std::string getDjiHilCapabilities() const override;
    virtual std::string getDjiHilTiming() const override;
    virtual std::string getDjiHilDecodedState() const override;
    virtual bool sendDjiHilSafetyEvent(uint32_t Kind, double Score,
        const std::string& Reason) override;
    virtual void resetImplementation() override;

protected:
    virtual void commandMotorPWMs(float FrontRightPwm, float RearLeftPwm,
        float FrontLeftPwm, float RearRightPwm) override;
    virtual void commandRollPitchYawrateThrottle(float Roll, float Pitch,
        float YawRate, float Throttle) override;
    virtual void commandRollPitchYawZ(float Roll, float Pitch, float Yaw, float Z) override;
    virtual void commandRollPitchYawThrottle(float Roll, float Pitch,
        float Yaw, float Throttle) override;
    virtual void commandRollPitchYawrateZ(float Roll, float Pitch,
        float YawRate, float Z) override;
    virtual void commandAngleRatesZ(float RollRate, float PitchRate,
        float YawRate, float Z) override;
    virtual void commandAngleRatesThrottle(float RollRate, float PitchRate,
        float YawRate, float Throttle) override;
    virtual void commandVelocity(float Vx, float Vy, float Vz,
        const msr::airlib::YawMode& YawMode) override;
    virtual void commandVelocityZ(float Vx, float Vy, float Z,
        const msr::airlib::YawMode& YawMode) override;
    virtual void commandPosition(float X, float Y, float Z,
        const msr::airlib::YawMode& YawMode) override;
    virtual void setControllerGains(uint8_t ControllerType,
        const std::vector<float>& Kp, const std::vector<float>& Ki,
        const std::vector<float>& Kd) override;
    virtual msr::airlib::Kinematics::State getKinematicsEstimated() const override;
    virtual msr::airlib::LandedState getLandedState() const override;
    virtual msr::airlib::GeoPoint getGpsLocation() const override;
    virtual const msr::airlib::MultirotorApiParams& getMultirotorApiParams() const override;
    virtual float getCommandPeriod() const override;
    virtual float getTakeoffZ() const override;
    virtual float getDistanceAccuracy() const override;
    virtual void beforeTask() override;

private:
    [[noreturn]] static void RejectCommand();
    bool ReadState(msr::airlib::djihil::State& OutState) const;

    IDjiHilStateProvider* StateProvider = nullptr;
    const msr::airlib::SensorCollection* Sensors = nullptr;
    msr::airlib::GeoPoint HomeGeoPoint;
    msr::airlib::MultirotorApiParams ApiParams;
    std::function<std::string()> TimingProvider;
    std::function<bool(uint32_t, double, const std::string&)> SafetyEventSender;
    std::atomic_bool bApiControlEnabled{false};
};
