#pragma once

#include "common/Common.hpp"
#include "common/CommonStructs.hpp"
#include "physics/Kinematics.hpp"
#include "vehicles/multirotor/api/MultirotorCommon.hpp"

#include <cmath>

namespace msr
{
namespace airlib
{
namespace djihil
{
    enum class ValidField : uint64_t
    {
        None = 0,
        Position = 1ull << 0,
        Orientation = 1ull << 1,
        LinearVelocity = 1ull << 2,
        AngularVelocity = 1ull << 3,
        LinearAcceleration = 1ull << 4,
        AngularAcceleration = 1ull << 5,
        Gps = 1ull << 6,
        LandedState = 1ull << 7,
        Rc = 1ull << 8
    };

    inline uint64_t mask(ValidField field)
    {
        return static_cast<uint64_t>(field);
    }

    struct State
    {
        uint32_t protocol_version = 1;
        uint64_t session_id = 0;
        uint64_t frame_id = 0;
        uint64_t source_time_ns = 0;
        uint64_t host_monotonic_ns = 0;
        uint64_t receive_sequence = 0;
        uint64_t valid_fields = 0;
        Kinematics::State kinematics = Kinematics::State::zero();
        GeoPoint gps;
        LandedState landed_state = LandedState::Landed;

        bool has(ValidField field) const
        {
            return (valid_fields & mask(field)) != 0;
        }

        bool hasFiniteCore() const
        {
            const auto& position = kinematics.pose.position;
            const auto& orientation = kinematics.pose.orientation;
            return (!has(ValidField::Position)
                    || (std::isfinite(position.x()) && std::isfinite(position.y()) && std::isfinite(position.z())))
                && (!has(ValidField::Orientation)
                    || (std::isfinite(orientation.w()) && std::isfinite(orientation.x())
                        && std::isfinite(orientation.y()) && std::isfinite(orientation.z())
                        && orientation.squaredNorm() > 1.0e-8f));
        }
    };

    inline State interpolate(const State& a, const State& b, real_T alpha)
    {
        if (a.session_id != b.session_id)
            return alpha < 1.0f ? a : b;

        alpha = std::max(real_T(0), std::min(real_T(1), alpha));
        State result = alpha < 1.0f ? a : b;
        result.frame_id = alpha < 1.0f ? a.frame_id : b.frame_id;
        result.source_time_ns = static_cast<uint64_t>(
            static_cast<long double>(a.source_time_ns)
            + (static_cast<long double>(b.source_time_ns) - a.source_time_ns) * alpha);
        result.host_monotonic_ns = static_cast<uint64_t>(
            static_cast<long double>(a.host_monotonic_ns)
            + (static_cast<long double>(b.host_monotonic_ns) - a.host_monotonic_ns) * alpha);
        result.valid_fields = a.valid_fields & b.valid_fields;

        if (result.has(ValidField::Position))
            result.kinematics.pose.position = a.kinematics.pose.position
                + (b.kinematics.pose.position - a.kinematics.pose.position) * alpha;
        if (result.has(ValidField::Orientation))
            result.kinematics.pose.orientation = a.kinematics.pose.orientation.slerp(alpha, b.kinematics.pose.orientation).normalized();
        if (result.has(ValidField::LinearVelocity))
            result.kinematics.twist.linear = a.kinematics.twist.linear
                + (b.kinematics.twist.linear - a.kinematics.twist.linear) * alpha;
        if (result.has(ValidField::AngularVelocity))
            result.kinematics.twist.angular = a.kinematics.twist.angular
                + (b.kinematics.twist.angular - a.kinematics.twist.angular) * alpha;
        if (result.has(ValidField::LinearAcceleration))
            result.kinematics.accelerations.linear = a.kinematics.accelerations.linear
                + (b.kinematics.accelerations.linear - a.kinematics.accelerations.linear) * alpha;
        if (result.has(ValidField::AngularAcceleration))
            result.kinematics.accelerations.angular = a.kinematics.accelerations.angular
                + (b.kinematics.accelerations.angular - a.kinematics.accelerations.angular) * alpha;
        if (result.has(ValidField::Gps)) {
            result.gps.latitude = a.gps.latitude + (b.gps.latitude - a.gps.latitude) * alpha;
            result.gps.longitude = a.gps.longitude + (b.gps.longitude - a.gps.longitude) * alpha;
            result.gps.altitude = a.gps.altitude + (b.gps.altitude - a.gps.altitude) * alpha;
        }

        return result;
    }
}
}
}
