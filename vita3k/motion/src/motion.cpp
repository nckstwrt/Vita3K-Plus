// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <motion/event_handler.h>
#include <motion/functions.h>
#include <motion/state.h>

#include <config/state.h>
#include <ctrl/state.h>

#include <util/log.h>

#include <SDL3/SDL_gamepad.h>
#include <algorithm>
#include <cmath>
#include <numbers>

void set_display_rotation(MotionState &state, int rotation) {
    state.device_native_rotation = static_cast<DeviceRotation>(rotation);
}

static void detect_device_motion_support(MotionState &state) {
    state.device_accel_id = 0;
    state.device_gyro_id = 0;

    int num_sensors;
    auto sensors_id = SDL_GetSensors(&num_sensors);
    if (!sensors_id || (num_sensors <= 0)) {
        state.has_device_motion_support = false;
        return;
    }

    for (int idx = 0; idx < num_sensors; idx++) {
        switch (SDL_GetSensorTypeForID(sensors_id[idx])) {
        case SDL_SENSOR_ACCEL:
            state.device_accel_id = sensors_id[idx];
            break;
        case SDL_SENSOR_GYRO:
            state.device_gyro_id = sensors_id[idx];
            break;
        default:
            break;
        }

        if ((state.device_accel_id != 0) && (state.device_gyro_id != 0))
            break;
    }

    SDL_free(sensors_id);

    state.has_device_motion_support = ((state.device_accel_id != 0) && (state.device_gyro_id != 0));
}

static std::string device_rotation_to_string(DeviceRotation rotation) {
    switch (rotation) {
    case ROTATION_0:
        return "ROTATION_0";
    case ROTATION_90:
        return "ROTATION_90";
    case ROTATION_180:
        return "ROTATION_180";
    case ROTATION_270:
        return "ROTATION_270";
    default:
        return "UNKNOWN_ROTATION";
    }
}

void MotionState::init() {
    reset_runtime();
    has_device_motion_support = false;
    device_accel_id = 0;
    device_gyro_id = 0;
}

void MotionState::clear_device_motion_support() {
    stop_sensor_sampling();
    has_device_motion_support = false;
    device_accel_id = 0;
    device_gyro_id = 0;
}

void MotionState::refresh_device_motion_support() {
    clear_device_motion_support();
    detect_device_motion_support(*this);

    if (has_device_motion_support)
        LOG_INFO("Device has a built-in accelerometer and gyroscope (native display rotation: {}).", device_rotation_to_string(device_native_rotation));
}

void MotionState::stop_sensor_sampling() {
    is_sampling = false;
    device_accel.reset();
    device_gyro.reset();
}

void MotionState::start_sensor_sampling() {
    is_sampling = true;
    if (!has_device_motion_support)
        return;

    const auto open_sensor = [](SDL_SensorPtr &sensor, uint32_t sensor_id, const char *name) {
        if (sensor_id == 0) {
            LOG_ERROR("No {} sensor ID available to open.", name);
            return false;
        }

        SDL_Sensor *raw_sensor = SDL_OpenSensor(sensor_id);
        if (!raw_sensor) {
            LOG_ERROR("Failed to open {} sensor with ID {}: {}", name, sensor_id, SDL_GetError());
            return false;
        }

        sensor.reset(raw_sensor);
        return true;
    };

    if (!open_sensor(device_accel, device_accel_id, "accelerometer") || !open_sensor(device_gyro, device_gyro_id, "gyroscope"))
        clear_device_motion_support();
}

void MotionState::reset_runtime() {
    stop_sensor_sampling();
    motion_data.ResetQuaternion();
    motion_data.ResetRotations();
    last_counter = 0;
    last_gyro_timestamp = 0;
    last_accel_timestamp = 0;
    last_updated_gyro_timestamp = 0;
    last_updated_accel_timestamp = 0;
    last_virtual_tilt_timestamp = 0;
    last_virtual_tilt_log_timestamp = 0;
    virtual_tilt_roll_radians = 0.0f;
    virtual_tilt_velocity_radians = 0.0f;
    virtual_tilt_left_trigger = 0.0f;
    virtual_tilt_right_trigger = 0.0f;
    virtual_tilt_target_radians = 0.0f;
    has_virtual_tilt_motion_support = false;
}

SceFVector3 get_acceleration(const MotionState &state) {
    Util::Vec3f accelerometer = state.motion_data.GetAcceleration();
    return {
        accelerometer.x,
        accelerometer.y,
        accelerometer.z,
    };
}

SceFVector3 get_gyroscope(const MotionState &state) {
    if (state.has_virtual_tilt_motion_support)
        return { 0.0f, state.virtual_tilt_velocity_radians, 0.0f };

    Util::Vec3f gyroscope = state.motion_data.GetGyroscope() * 2.f * std::numbers::pi_v<float>;
    return {
        gyroscope.x,
        gyroscope.y,
        gyroscope.z,
    };
}

Util::Quaternion<SceFloat> get_orientation(const MotionState &state) {
    auto quat = state.motion_data.GetOrientation();
    return {
        { -quat.xyz[1], -quat.w, quat.xyz[0] },
        -quat.xyz[2],
    };
}

SceBool get_gyro_bias_correction(const MotionState &state) {
    return state.motion_data.IsGyroBiasEnabled();
}

void set_gyro_bias_correction(MotionState &state, SceBool setValue) {
    state.motion_data.EnableGyroBias(setValue);
}

SceBool get_tilt_correction(MotionState &state) {
    return state.motion_data.IsTiltCorrectionEnabled();
}

void set_tilt_correction(MotionState &state, SceBool setValue) {
    state.motion_data.EnableTiltCorrection(setValue);
}

SceBool get_deadband(MotionState &state) {
    return state.motion_data.IsDeadbandEnabled();
}

void set_deadband(MotionState &state, SceBool setValue) {
    state.motion_data.EnableDeadband(setValue);
}

SceFloat get_angle_threshold(const MotionState &state) {
    return state.motion_data.GetAngleThreshold();
}

void set_angle_threshold(MotionState &state, SceFloat setValue) {
    state.motion_data.SetAngleThreshold(setValue);
}

SceFVector3 get_basic_orientation(const MotionState &state) {
    return state.motion_data.GetBasicOrientation();
}

constexpr uint64_t to_microseconds(uint64_t ns) {
    return ns / 1000;
}

template <typename SensorEvent>
static void handle_motion_event(EmuEnvState &emuenv, int32_t sensor_type, const SensorEvent &sensor) {
    if (!emuenv.motion.is_sampling)
        return;

    if (!emuenv.ctrl.has_motion_support && !emuenv.motion.has_device_motion_support)
        return;

    const auto get_processed_sensor_data = [&]() {
        Util::Vec3f data = { sensor.data[0], sensor.data[1], sensor.data[2] };
        const auto from_gamepad = sensor.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
        if (!from_gamepad) {
            switch (emuenv.motion.device_native_rotation) {
            case ROTATION_90: // portrait -> landscape left
                std::tie(data.x, data.y, data.z) = std::make_tuple(-data.y, data.x, data.z);
                break;
            case ROTATION_180: // upside-down
                data.x *= -1.f;
                data.y *= -1.f;
                break;
            case ROTATION_270: // portrait -> landscape right
                std::tie(data.x, data.y, data.z) = std::make_tuple(data.y, -data.x, data.z);
                break;
            default: // ROTATION_0 or unknown: no transform
                break;
            }
        } else
            std::tie(data.x, data.y, data.z) = std::make_tuple(data.x, -data.z, data.y);

        return data;
    };

    const uint64_t sensor_timestamp = (sensor.sensor_timestamp > 0)
        ? to_microseconds(sensor.sensor_timestamp) // convert ns -> us
        : std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    auto sensor_data = get_processed_sensor_data();

    if (sensor_type == SDL_SENSOR_ACCEL) {
        sensor_data /= -SDL_STANDARD_GRAVITY;
        emuenv.motion.motion_data.SetAcceleration(sensor_data);
        emuenv.motion.last_updated_accel_timestamp = sensor_timestamp;
    } else if (sensor_type == SDL_SENSOR_GYRO) {
        sensor_data /= 2.f * std::numbers::pi_v<float>;
        emuenv.motion.motion_data.SetGyroscope(sensor_data);
        emuenv.motion.last_updated_gyro_timestamp = sensor_timestamp;
    }
}

void handle_motion_event(EmuEnvState &emuenv, int32_t sensor_type, const SDL_SensorEvent &sensor) {
    handle_motion_event<SDL_SensorEvent>(emuenv, sensor_type, sensor);
}

void handle_motion_event(EmuEnvState &emuenv, int32_t sensor_type, const SDL_GamepadSensorEvent &sensor) {
    handle_motion_event<SDL_GamepadSensorEvent>(emuenv, sensor_type, sensor);
}

static float trigger_axis_to_unit(const Sint16 axis) {
    return std::clamp(static_cast<float>(axis) / 32767.0f, 0.0f, 1.0f);
}

static float apply_trigger_deadzone(float tilt, const float deadzone) {
    const float magnitude = std::abs(tilt);
    if (magnitude <= deadzone)
        return 0.0f;

    return std::copysign((magnitude - deadzone) / (1.0f - deadzone), tilt);
}

static void update_virtual_trigger_tilt(MotionState &state, CtrlState &ctrl_state, const Config &config, const uint64_t timestamp) {
    const auto &settings = config.current_config;
    const float deadzone = std::clamp(settings.trigger_tilt_deadzone, 0.0f, 0.95f);
    const float sensitivity = std::clamp(settings.trigger_tilt_sensitivity, 0.0f, 4.0f);
    const float max_roll = std::clamp(settings.trigger_tilt_max_angle_degrees, 0.0f, 85.0f) * std::numbers::pi_v<float> / 180.0f;
    const float smoothing = std::clamp(settings.trigger_tilt_smoothing, 0.0f, 60.0f);

    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
    {
        const std::lock_guard lock(ctrl_state.mutex);
        const auto &axis_binds = config.controller_axis_binds;
        if (axis_binds.size() >= 6) {
            for (const auto &[_, controller] : ctrl_state.controllers) {
                left_trigger = std::max(left_trigger, trigger_axis_to_unit(SDL_GetGamepadAxis(controller.controller.get(), static_cast<SDL_GamepadAxis>(axis_binds[4]))));
                right_trigger = std::max(right_trigger, trigger_axis_to_unit(SDL_GetGamepadAxis(controller.controller.get(), static_cast<SDL_GamepadAxis>(axis_binds[5]))));
            }
        }
    }

    const float elapsed_seconds = state.last_virtual_tilt_timestamp == 0
        ? (1.0f / 60.0f)
        : std::clamp(static_cast<float>(timestamp - state.last_virtual_tilt_timestamp) / 1'000'000.0f, 0.0f, 0.1f);
    state.last_virtual_tilt_timestamp = timestamp;

    float tilt = apply_trigger_deadzone((right_trigger - left_trigger) * sensitivity, deadzone);
    tilt = std::clamp(tilt, -1.0f, 1.0f);
    if (settings.trigger_tilt_invert)
        tilt = -tilt;

    state.virtual_tilt_left_trigger = left_trigger;
    state.virtual_tilt_right_trigger = right_trigger;
    state.virtual_tilt_target_radians = tilt * max_roll;

    const float smoothing_alpha = smoothing == 0.0f ? 1.0f : 1.0f - std::exp(-smoothing * elapsed_seconds);
    const float previous_roll = state.virtual_tilt_roll_radians;
    state.virtual_tilt_roll_radians += (state.virtual_tilt_target_radians - state.virtual_tilt_roll_radians) * smoothing_alpha;
    state.virtual_tilt_velocity_radians = elapsed_seconds > 0.0f
        ? (state.virtual_tilt_roll_radians - previous_roll) / elapsed_seconds
        : 0.0f;

    // Roll is about Vita Y: gravity holds the tilt; gyro is its rate of change.
    // Use the existing quaternion coordinate conversion in get_orientation().
    state.motion_data.SetAcceleration({ std::sin(state.virtual_tilt_roll_radians), 0.0f, -std::cos(state.virtual_tilt_roll_radians) });
    state.motion_data.SetQuaternion({ { 0.0f, 0.0f, -std::cos(state.virtual_tilt_roll_radians / 2.0f) }, -std::sin(state.virtual_tilt_roll_radians / 2.0f) });
    state.last_updated_accel_timestamp = timestamp;
    state.last_updated_gyro_timestamp = timestamp;
}

void refresh_motion(MotionState &state, CtrlState &ctrl_state, const Config &config) {
    const std::lock_guard lock(state.mutex);
    if (!state.is_sampling)
        return;

    const bool has_physical_motion_support = ctrl_state.has_motion_support || state.has_device_motion_support;
    state.has_virtual_tilt_motion_support = config.current_config.trigger_tilt_motion && !config.disable_motion && !has_physical_motion_support;
    if (!has_physical_motion_support && !state.has_virtual_tilt_motion_support)
        return;

    if (state.has_virtual_tilt_motion_support) {
        const uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        update_virtual_trigger_tilt(state, ctrl_state, config, timestamp);
    }

    if (!state.has_virtual_tilt_motion_support)
        state.motion_data.UpdateOrientation(state.last_updated_accel_timestamp - state.last_accel_timestamp);
    state.motion_data.UpdateBasicOrientation();
    if (!state.has_virtual_tilt_motion_support)
        state.motion_data.UpdateRotation(state.last_updated_gyro_timestamp - state.last_gyro_timestamp);

    state.last_accel_timestamp = state.last_updated_accel_timestamp;
    state.last_gyro_timestamp = state.last_updated_gyro_timestamp;

    if (state.has_virtual_tilt_motion_support && (state.last_accel_timestamp - state.last_virtual_tilt_log_timestamp) >= 500'000) {
        const auto acceleration = state.motion_data.GetAcceleration();
        const auto gyro_radians = get_gyroscope(state);
        LOG_INFO("Virtual trigger tilt: LT={:.3f} RT={:.3f} target_roll={:.3f}rad roll={:.3f}rad accel=({:.3f}, {:.3f}, {:.3f}) gyro=({:.3f}, {:.3f}, {:.3f}) rad/s",
            state.virtual_tilt_left_trigger, state.virtual_tilt_right_trigger, state.virtual_tilt_target_radians, state.virtual_tilt_roll_radians,
            acceleration.x, acceleration.y, acceleration.z, gyro_radians.x, gyro_radians.y, gyro_radians.z);
        state.last_virtual_tilt_log_timestamp = state.last_accel_timestamp;
    }

    state.last_counter++;
}
