#include <config/state.h>
#include <ctrl/state.h>
#include <motion/functions.h>
#include <motion/state.h>

#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

int main() {
    try {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        CHECK(SDL_Init(SDL_INIT_GAMEPAD));
        SDL_VirtualJoystickDesc desc{};
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
        desc.name = "Trigger tilt test";
        const auto id = SDL_AttachVirtualJoystick(&desc);
        CHECK(id != 0);
        auto *joystick = SDL_OpenJoystick(id);
        CHECK(joystick != nullptr);
        GamepadPtr gamepad(SDL_OpenGamepad(id), SDL_CloseGamepad);
        CHECK(gamepad != nullptr);

        CtrlState ctrl;
        ctrl.controllers.emplace(SDL_GetGamepadGUIDForID(id), Controller{ gamepad, 1, false, false, false, desc.name });
        Config cfg;
        cfg.controller_axis_binds = { 0, 1, 2, 3, 4, 5 };
        cfg.current_config.trigger_tilt_motion = true;
        cfg.current_config.trigger_tilt_smoothing = 0.0f;
        MotionState state;
        state.init();
        state.start_sensor_sampling();

        const auto sample = [&](float left, float right) {
            CHECK(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, static_cast<Sint16>(std::lround(left * 65535.0f - 32768.0f))));
            CHECK(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, static_cast<Sint16>(std::lround(right * 65535.0f - 32768.0f))));
            SDL_UpdateJoysticks();
            state.last_virtual_tilt_timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - 16667;
            refresh_motion(state, ctrl, cfg);
            const auto accel = get_acceleration(state);
            const auto quat = get_orientation(state);
            const auto gravity = Util::QuaternionRotate(quat.Inverse(), Util::Vec3f{ 0.0f, 0.0f, -1.0f });
            CHECK(std::abs(accel.x - gravity.x) < 0.0001f);
            CHECK(std::abs(accel.y - gravity.y) < 0.0001f);
            CHECK(std::abs(accel.z - gravity.z) < 0.0001f);
            CHECK(std::abs(quat.xyz.Length2() + quat.w * quat.w - 1.0f) < 0.0001f);
            CHECK(state.has_virtual_tilt_motion_support);
        };

        sample(0.0f, 0.0f);
        CHECK(std::abs(get_acceleration(state).z + 1.0f) < 0.0001f);
        CHECK(std::abs(get_gyroscope(state).y) < 0.0001f);
        sample(1.0f, 0.0f);
        CHECK(SDL_GetGamepadAxis(gamepad.get(), SDL_GAMEPAD_AXIS_LEFT_TRIGGER) == 32767);
        CHECK(get_acceleration(state).x < -0.4f);
        CHECK(get_gyroscope(state).y < 0.0f);
        sample(1.0f, 0.0f);
        CHECK(std::abs(get_gyroscope(state).y) < 0.0001f); // Holding tilt is not endless rotation.
        sample(0.0f, 1.0f);
        CHECK(get_acceleration(state).x > 0.4f);
        CHECK(get_gyroscope(state).y > 0.0f);
        sample(0.0f, 0.5f);
        const float half_roll = state.virtual_tilt_roll_radians;
        CHECK(half_roll > 0.1f && half_roll < 0.3f);
        sample(0.8f, 0.8f);
        CHECK(std::abs(state.virtual_tilt_roll_radians) < 0.0001f);
        sample(0.0f, 0.01f);
        CHECK(std::abs(state.virtual_tilt_roll_radians) < 0.0001f);
        cfg.current_config.trigger_tilt_invert = true;
        sample(0.0f, 1.0f);
        CHECK(state.virtual_tilt_roll_radians < 0.0f);
        cfg.current_config.trigger_tilt_invert = false;
        sample(0.0f, 1.0f);
        cfg.current_config.trigger_tilt_smoothing = 10.0f;
        const float full_roll = state.virtual_tilt_roll_radians;
        sample(0.0f, 0.0f);
        CHECK(state.virtual_tilt_roll_radians > 0.0f && state.virtual_tilt_roll_radians < full_roll);
        for (int i = 0; i < 100; ++i)
            sample(0.0f, 0.0f);
        CHECK(std::abs(state.virtual_tilt_roll_radians) < 0.0001f);
        CHECK(std::abs(get_gyroscope(state).y) < 0.0001f);
        ctrl.has_motion_support = true;
        refresh_motion(state, ctrl, cfg);
        CHECK(!state.has_virtual_tilt_motion_support);
        ctrl.has_motion_support = false;
        cfg.disable_motion = true;
        refresh_motion(state, ctrl, cfg);
        CHECK(!state.has_virtual_tilt_motion_support);

        ctrl.controllers.clear();
        gamepad.reset();
        SDL_CloseJoystick(joystick);
        CHECK(SDL_DetachVirtualJoystick(id));
        SDL_Quit();
        std::cout << "PASS: SDL trigger input, analog tilt, deadzone, equal triggers, inversion, return to center, zero held gyro, gravity/quaternion agreement, sensor priority, disabled motion.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "; SDL: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
}
