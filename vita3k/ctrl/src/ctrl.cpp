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

#include <ctrl/ctrl.h>
#include <ctrl/functions.h>
#include <ctrl/state.h>

#include <config/state.h>
#include <display/functions.h>
#include <display/state.h>
#include <kernel/state.h>
#include <util/log.h>

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

static bool is_virtual_pad(const Controller &controller) {
    return SDL_IsJoystickVirtual(SDL_GetGamepadID(controller.controller.get()));
}

static std::map<SDL_JoystickID, std::string> joystick_outcomes;

constexpr uint32_t DIRECT_PAD_LEFT_TRIGGER = 1u << 30;
constexpr uint32_t DIRECT_PAD_RIGHT_TRIGGER = 1u << 31;

struct DirectPad {
    std::string name;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t crc = 0;
    uint32_t buttons = 0;
    float axes[6] = {};
    int verdict = -1;
};
static std::map<int, DirectPad> direct_pads;

static int reserve_port(CtrlState &state) {
    for (int i = 0; i < SCE_CTRL_MAX_WIRELESS_NUM; i++) {
        if (state.free_ports[i]) {
            state.free_ports[i] = false;
            return i;
        }
    }

    // No free port found.
    return -1;
}

SceCtrlExternalInputMode get_type_of_controller(CtrlState &state, int port) {
    for (const auto &[guid, controller] : state.controllers) {
        if (controller.port == port) {
            const auto type = SDL_GetGamepadType(controller.controller.get());
            return (type == SDL_GAMEPAD_TYPE_PS4) || (type == SDL_GAMEPAD_TYPE_PS5) ? SCE_CTRL_TYPE_DS4 : SCE_CTRL_TYPE_DS3;
        }
    }
    return SCE_CTRL_TYPE_DS3;
}

#ifdef __ANDROID__
static void map_android_half_controllers() {
    constexpr uint16_t FACE_BUTTONS = (1 << SDL_GAMEPAD_BUTTON_SOUTH) | (1 << SDL_GAMEPAD_BUTTON_EAST) | (1 << SDL_GAMEPAD_BUTTON_WEST) | (1 << SDL_GAMEPAD_BUTTON_NORTH);
    static const std::pair<SDL_GamepadButton, const char *> BUTTONS[] = {
        { SDL_GAMEPAD_BUTTON_BACK, "back:b4," },
        { SDL_GAMEPAD_BUTTON_START, "start:b6," },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, "leftstick:b7," },
        { SDL_GAMEPAD_BUTTON_RIGHT_STICK, "rightstick:b8," },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "leftshoulder:b9," },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "rightshoulder:b10," },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, "dpup:b11," },
        { SDL_GAMEPAD_BUTTON_DPAD_DOWN, "dpdown:b12," },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, "dpleft:b13," },
        { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "dpright:b14," },
    };
    static std::set<SDL_JoystickID> reported;

    int count = 0;
    SDL_JoystickID *joysticks = SDL_GetJoysticks(&count);
    if (!joysticks)
        return;
    for (int i = 0; i < count; i++) {
        const SDL_JoystickID id = joysticks[i];
        if (SDL_IsGamepad(id))
            continue;
        const char *id_name = SDL_GetJoystickNameForID(id);
        std::string name = (id_name && *id_name) ? id_name : "unnamed";
        if (name.starts_with("uinput-") || name.starts_with("gf_"))
            continue;

        const SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
        const uint16_t buttons = static_cast<uint16_t>(guid.data[12] | (guid.data[13] << 8));
        const uint16_t axes = static_cast<uint16_t>(guid.data[14] | (guid.data[15] << 8));
        char guid_string[33];
        SDL_GUIDToString(guid, guid_string, sizeof(guid_string));

        std::string controls;
        for (const auto &[button, entry] : BUTTONS) {
            if (buttons & (1 << button))
                controls += entry;
        }
        if (!(buttons & FACE_BUTTONS) && !controls.empty() && (axes & 0x7FFF) == 0x0003) {
            if ((buttons & (1 << SDL_GAMEPAD_BUTTON_GUIDE)) && SDL_GetAndroidSDKVersion() >= 30)
                controls += "guide:b5,";
            std::replace(name.begin(), name.end(), ',', ' ');
            const std::string mapping = fmt::format("{},{},{}leftx:a0,lefty:a1,", guid_string, name, controls);
            const bool added = SDL_AddGamepadMapping(mapping.c_str()) >= 0;
            LOG_INFO("[CTRL] joystick '{}' (id {}, buttons 0x{:04X}, axes 0x{:04X}) has no face buttons, so SDL left it out; mapped from its reported controls{}: {}",
                name, id, buttons, axes, added ? std::string() : fmt::format(" FAILED ({})", SDL_GetError()), mapping);
        } else if (reported.insert(id).second) {
            LOG_INFO("[CTRL] joystick '{}' (id {}, guid {}, buttons 0x{:04X}, axes 0x{:04X}) is not a gamepad and is not used", name, id, guid_string, buttons, axes);
        }
    }
    SDL_free(joysticks);
}

static void map_android_digital_triggers(SDL_Gamepad *gamepad, SDL_JoystickID id, std::string_view name) {
    if (SDL_IsJoystickVirtual(id))
        return;
    const int buttons = SDL_GetNumJoystickButtons(SDL_GetGamepadJoystick(gamepad));
    const bool add_left = !SDL_GamepadHasAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) && buttons > 15;
    const bool add_right = !SDL_GamepadHasAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) && buttons > 16;
    if (!add_left && !add_right)
        return;
    char *current = SDL_GetGamepadMapping(gamepad);
    if (!current)
        return;
    std::string mapping = current;
    SDL_free(current);
    if (!mapping.empty() && mapping.back() != ',')
        mapping += ',';
    if (add_left)
        mapping += "lefttrigger:b15,";
    if (add_right)
        mapping += "righttrigger:b16,";
    const bool added = SDL_AddGamepadMapping(mapping.c_str()) >= 0;
    LOG_INFO("[CTRL] '{}' (gamepad {}, {} buttons) has no analog {}, its L2/R2 keys now act as the triggers{}: left {}, right {}; {}",
        name, id, buttons, add_left && add_right ? "L2/R2" : (add_left ? "L2" : "R2"), added ? std::string() : fmt::format(" FAILED ({})", SDL_GetError()),
        SDL_GamepadHasAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) ? "mapped" : "none", SDL_GamepadHasAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) ? "mapped" : "none", mapping);
}

static bool is_pad_part(std::string_view name) {
    return name.ends_with(" Touchpad") || name.ends_with(" Motion Sensors") || name.ends_with(" IMU");
}

static bool is_part_of_listed_pad(SDL_JoystickID id) {
    const char *name = SDL_GetJoystickNameForID(id);
    if (!name || !is_pad_part(name))
        return false;
    const Uint16 vendor = SDL_GetJoystickVendorForID(id);
    const Uint16 product = SDL_GetJoystickProductForID(id);
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    bool listed = false;
    for (int i = 0; ids && i < count && !listed; i++) {
        const char *other = SDL_GetJoystickNameForID(ids[i]);
        listed = ids[i] != id && other && !is_pad_part(other) && SDL_GetJoystickVendorForID(ids[i]) == vendor && SDL_GetJoystickProductForID(ids[i]) == product;
    }
    SDL_free(ids);
    return listed;
}

static void log_joystick_census(const CtrlState &state, bool force) {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (!ids)
        return;
    std::string signature;
    std::vector<std::string> lines;
    for (int i = 0; i < count; i++) {
        const SDL_JoystickID id = ids[i];
        const char *joystick_name = SDL_GetJoystickNameForID(id);
        const bool gamepad = SDL_IsGamepad(id);
        const char *gamepad_name = gamepad ? SDL_GetGamepadNameForID(id) : nullptr;
        const SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
        char guid_string[33];
        SDL_GUIDToString(guid, guid_string, sizeof(guid_string));
        std::string where;
        for (const auto &[_, controller] : state.controllers) {
            if (SDL_GetGamepadID(controller.controller.get()) == id)
                where = fmt::format("opened on port {}", controller.port);
        }
        if (where.empty()) {
            const auto outcome = joystick_outcomes.find(id);
            where = outcome != joystick_outcomes.end() ? outcome->second : (gamepad ? "not opened" : "not used (not a gamepad)");
        }
        char *mapping = gamepad ? SDL_GetGamepadMappingForID(id) : nullptr;
        lines.push_back(fmt::format("[CTRL] SDL joystick {} '{}'{} | guid {} (crc 0x{:04X}, buttons 0x{:04X}, axes 0x{:04X}), vendor 0x{:04X} product 0x{:04X}{} | {} | mapping: {}",
            id, joystick_name ? joystick_name : "", gamepad ? fmt::format(" = gamepad '{}'", gamepad_name ? gamepad_name : "") : std::string(" (not a gamepad)"),
            guid_string, guid.data[2] | (guid.data[3] << 8), guid.data[12] | (guid.data[13] << 8), guid.data[14] | (guid.data[15] << 8),
            SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id), SDL_IsJoystickVirtual(id) ? ", virtual" : "", where, mapping ? mapping : "none"));
        if (mapping)
            SDL_free(mapping);
        signature += fmt::format("{}|{}|{}|{};", id, joystick_name ? joystick_name : "", gamepad_name ? gamepad_name : "-", where);
    }
    SDL_free(ids);
    static std::string last_signature;
    if (!force && signature == last_signature)
        return;
    last_signature = signature;
    LOG_INFO("[CTRL] SDL sees {} joysticks{}", count, force ? " at this launch" : " (changed)");
    for (const auto &line : lines)
        LOG_INFO("{}", line);
}
#endif

void refresh_controllers(CtrlState &state, EmuEnvState &emuenv) {
    // Remove disconnected controllers
    bool found_gyro = false;
    bool found_accel = false;
    const std::lock_guard lock(state.mutex);
    for (ControllerList::iterator controller = state.controllers.begin(); controller != state.controllers.end();) {
        if (SDL_GamepadConnected(controller->second.controller.get())) {
            found_accel |= controller->second.has_accel;
            found_gyro |= controller->second.has_gyro;

            ++controller;
        } else {
            LOG_INFO("[CTRL] '{}' disconnected from port {}", controller->second.name, controller->second.port);
            state.free_ports[controller->second.port] = true;
            controller = state.controllers.erase(controller);
            state.controllers_num--;
        }
    }

    // gamepads left out are reported once each
    static std::set<SDL_JoystickID> reported_skips;

#ifdef __ANDROID__
    map_android_half_controllers();
#endif

    // Add new controllers
    int num_gamepads = 0;
    const auto gamepads = SDL_GetGamepads(&num_gamepads);
    for (int gamepad_index = 0; gamepad_index < num_gamepads; ++gamepad_index) {
        const auto gamepad_id = gamepads[gamepad_index];
        const char *id_name = SDL_GetGamepadNameForID(gamepad_id);
        const std::string_view name = id_name ? id_name : "unnamed";
        if (state.controllers_num >= SCE_CTRL_MAX_WIRELESS_NUM) {
            joystick_outcomes[gamepad_id] = fmt::format("not opened: {} controllers are already connected", state.controllers_num);
            if (reported_skips.insert(gamepad_id).second)
                LOG_WARN("[CTRL] '{}' (gamepad {}) ignored, {} controllers are already connected", name, gamepad_id, state.controllers_num);
            break;
        }

#ifdef __ANDROID__
        // for whatever reasons, fingerprint sensors are detected as controllers, filter them out
        if (name.starts_with("uinput-") || name.starts_with("gf_")) {
            joystick_outcomes[gamepad_id] = "not opened: a fingerprint sensor name";
            continue;
        }
        if (is_part_of_listed_pad(gamepad_id)) {
            joystick_outcomes[gamepad_id] = "not opened: a pad's touchpad or motion part";
            if (reported_skips.insert(gamepad_id).second)
                LOG_INFO("[CTRL] '{}' (gamepad {}) not used, it is a pad's touchpad or motion part", SDL_GetJoystickNameForID(gamepad_id), gamepad_id);
            continue;
        }
#endif

        const SDL_GUID guid = SDL_GetJoystickGUIDForID(gamepad_id);
        const auto existing = state.controllers.find(guid);
        if (existing != state.controllers.end()) {
            const SDL_JoystickID existing_id = SDL_GetGamepadID(existing->second.controller.get());
            if (existing_id != gamepad_id)
                joystick_outcomes[gamepad_id] = fmt::format("not opened: same GUID as gamepad {}", existing_id);
            if (existing_id != gamepad_id && reported_skips.insert(gamepad_id).second)
                LOG_WARN("[CTRL] '{}' (gamepad {}) not used, it shares its GUID with connected '{}' (gamepad {})", name, gamepad_id, existing->second.name, existing_id);
            continue;
        }

        const GamepadPtr controller(SDL_OpenGamepad(gamepad_id), SDL_CloseGamepad);
        if (controller == nullptr) {
            joystick_outcomes[gamepad_id] = fmt::format("not opened: SDL_OpenGamepad failed ({})", SDL_GetError());
            LOG_WARN("[CTRL] could not open '{}' (gamepad {}): {}", name, gamepad_id, SDL_GetError());
            continue;
        }
        auto controller_name = SDL_GetGamepadName(controller.get());
        if (controller_name == nullptr) {
            joystick_outcomes[gamepad_id] = "not opened: no name";
            LOG_WARN("[CTRL] gamepad {} has no name, ignored", gamepad_id);
            continue;
        }

#ifdef __ANDROID__
        map_android_digital_triggers(controller.get(), gamepad_id, controller_name);
#endif

        // reserved after the open so a failed open holds no port
        Controller new_controller;
        new_controller.port = reserve_port(state);
        if (new_controller.port == -1) {
            joystick_outcomes[gamepad_id] = "not opened: no free port";
            LOG_WARN("[CTRL] no free port for '{}' (gamepad {})", controller_name, gamepad_id);
            break;
        }
        if (!SDL_IsJoystickVirtual(gamepad_id)) {
            for (auto &[_, other] : state.controllers) {
                if (other.port < new_controller.port && is_virtual_pad(other)) {
                    LOG_INFO("[CTRL] '{}' moved from port {} to port {} so '{}' gets port {}", other.name, other.port, new_controller.port, controller_name, other.port);
                    std::swap(other.port, new_controller.port);
                    SDL_SetGamepadPlayerIndex(other.controller.get(), other.port);
                }
            }
        }
        new_controller.controller = controller;
        SDL_SetGamepadPlayerIndex(controller.get(), new_controller.port);
        new_controller.name = controller_name;

        new_controller.has_gyro = SDL_GamepadHasSensor(controller.get(), SDL_SENSOR_GYRO);
        if (new_controller.has_gyro)
            SDL_SetGamepadSensorEnabled(controller.get(), SDL_SENSOR_GYRO, true);
        new_controller.has_accel = SDL_GamepadHasSensor(controller.get(), SDL_SENSOR_ACCEL);
        if (new_controller.has_accel)
            SDL_SetGamepadSensorEnabled(controller.get(), SDL_SENSOR_ACCEL, true);

        new_controller.has_led = SDL_GetBooleanProperty(SDL_GetGamepadProperties(controller.get()), SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
        if (new_controller.has_led) {
            auto &color = emuenv.cfg.controller_led_color;
            if (!color.empty()) {
                color.resize(3);
                SDL_SetGamepadLED(controller.get(), color[0], color[1], color[2]);
            }
        }

        found_gyro |= new_controller.has_gyro;
        found_accel |= new_controller.has_accel;

        LOG_INFO("[CTRL] '{}' connected on port {} (gamepad {}{}{})", controller_name, new_controller.port, gamepad_id,
            new_controller.has_gyro ? ", gyro" : "", new_controller.has_accel ? ", accelerometer" : "");
        joystick_outcomes.erase(gamepad_id);
        state.controllers.emplace(guid, new_controller);
        state.controllers_num++;
    }
    SDL_free(gamepads);

    for (auto &[_, controller] : state.controllers) {
        if (!is_virtual_pad(controller))
            continue;
        for (int port = 0; port < controller.port; port++) {
            if (state.free_ports[port]) {
                LOG_INFO("[CTRL] '{}' moved from port {} to free port {}", controller.name, controller.port, port);
                state.free_ports[controller.port] = true;
                state.free_ports[port] = false;
                controller.port = port;
                SDL_SetGamepadPlayerIndex(controller.controller.get(), port);
                break;
            }
        }
    }
    state.has_motion_support = found_gyro && found_accel;
#ifdef __ANDROID__
    log_joystick_census(state, false);
#endif
}

static void apply_keyboard(uint32_t *buttons, float axes[4], bool ext, EmuEnvState &emuenv) {
    const auto &kb = emuenv.ctrl.keyboard_state;
    const uint32_t kb_buttons = ext ? kb.buttons_ext : kb.buttons;

    *buttons |= kb_buttons;
    axes[0] += kb.axes[0];
    axes[1] += kb.axes[1];
    axes[2] += kb.axes[2];
    axes[3] += kb.axes[3];
}

static std::array<ControllerBinding, 13> get_controller_bindings(EmuEnvState &emuenv) {
    return { {
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_BACK]), SCE_CTRL_SELECT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_START]), SCE_CTRL_START },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_UP]), SCE_CTRL_UP },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]), SCE_CTRL_RIGHT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_DOWN]), SCE_CTRL_DOWN },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_LEFT]), SCE_CTRL_LEFT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]), SCE_CTRL_L },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER]), SCE_CTRL_R },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_NORTH]), SCE_CTRL_TRIANGLE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_EAST]), SCE_CTRL_CIRCLE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_SOUTH]), SCE_CTRL_CROSS },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_WEST]), SCE_CTRL_SQUARE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_GUIDE]), SCE_CTRL_PSBUTTON },
    } };
}

std::array<ControllerBinding, 15> get_controller_bindings_ext(EmuEnvState &emuenv) {
    return { {
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_BACK]), SCE_CTRL_SELECT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_LEFT_STICK]), SCE_CTRL_L3 },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_RIGHT_STICK]), SCE_CTRL_R3 },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_START]), SCE_CTRL_START },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_UP]), SCE_CTRL_UP },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]), SCE_CTRL_RIGHT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_DOWN]), SCE_CTRL_DOWN },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_LEFT]), SCE_CTRL_LEFT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]), SCE_CTRL_L1 },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER]), SCE_CTRL_R1 },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_NORTH]), SCE_CTRL_TRIANGLE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_EAST]), SCE_CTRL_CIRCLE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_SOUTH]), SCE_CTRL_CROSS },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_WEST]), SCE_CTRL_SQUARE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_GUIDE]), SCE_CTRL_PSBUTTON },
    } };
}

static float axis_to_axis(int16_t axis, const auto &mult) {
    const auto unsigned_axis = static_cast<float>(axis - INT16_MIN);
    assert(unsigned_axis >= 0);
    assert(unsigned_axis <= UINT16_MAX);

    const auto f = unsigned_axis / UINT16_MAX;

    const auto output = std::clamp(((f * 2) - 1) * mult, -1.0f, 1.f);

    return output;
}

static uint8_t float_to_byte(float f) {
    const auto clamped_f = std::clamp(f, -1.0f, 1.0f);

    return static_cast<uint8_t>(std::lround((clamped_f + 1.0f) * 127.5f));
}

// the triggers press L1/R1 (the Vita's L/R) and the shoulders L2/R2; non-ext reads have no L2/R2, so there the shoulders press nothing
static uint32_t swap_shoulders_and_triggers(uint32_t pressed, bool ext, bool left_trigger, bool right_trigger) {
    const uint32_t vita_left = ext ? SCE_CTRL_L1 : SCE_CTRL_L;
    const uint32_t vita_right = ext ? SCE_CTRL_R1 : SCE_CTRL_R;
    const bool left_shoulder = pressed & vita_left;
    const bool right_shoulder = pressed & vita_right;
    pressed &= ~(vita_left | vita_right);
    if (ext)
        pressed &= ~(SCE_CTRL_L2 | SCE_CTRL_R2);
    if (left_trigger)
        pressed |= vita_left;
    if (right_trigger)
        pressed |= vita_right;
    if (ext && left_shoulder)
        pressed |= SCE_CTRL_L2;
    if (ext && right_shoulder)
        pressed |= SCE_CTRL_R2;
    return pressed;
}

static void apply_controller(EmuEnvState &emuenv, uint32_t *buttons, float axes[4], SDL_Gamepad *controller, bool ext) {
    const auto &axis_binds = emuenv.cfg.controller_axis_binds;
    const bool left_trigger = SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[4])) > 0x3FFF;
    const bool right_trigger = SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[5])) > 0x3FFF;

    uint32_t pressed = 0;
    if (ext) {
        for (const auto &binding : get_controller_bindings_ext(emuenv)) {
            if (SDL_GetGamepadButton(controller, binding.controller)) {
                pressed |= binding.button;
            }
        }

        if (left_trigger) {
            pressed |= SCE_CTRL_L2;
        }
        if (right_trigger) {
            pressed |= SCE_CTRL_R2;
        }
    } else {
        for (const auto &binding : get_controller_bindings(emuenv)) {
            if (SDL_GetGamepadButton(controller, binding.controller)) {
                pressed |= binding.button;
            }
        }
    }
    if (emuenv.cfg.controller_swap_triggers) {
        LOG_INFO_ONCE("[CTRL] L1/R1 and L2/R2 swapped: the controller's triggers press L1/R1 (the Vita's L and R) and its shoulder buttons L2/R2");
        pressed = swap_shoulders_and_triggers(pressed, ext, left_trigger, right_trigger);
    }
    *buttons |= pressed;

    auto &analog_multiplier = emuenv.cfg.controller_analog_multiplier;
    axes[0] += axis_to_axis(SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[0])), analog_multiplier);
    axes[1] += axis_to_axis(SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[1])), analog_multiplier);
    axes[2] += axis_to_axis(SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[2])), analog_multiplier);
    axes[3] += axis_to_axis(SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[3])), analog_multiplier);
}

static const Controller *sdl_pad_for(const CtrlState &state, const DirectPad &pad) {
    for (const auto &[guid, controller] : state.controllers) {
        if (is_virtual_pad(controller))
            continue;
        const auto crc = static_cast<uint16_t>(guid.data[2] | (guid.data[3] << 8));
        const auto vendor = static_cast<uint16_t>(guid.data[4] | (guid.data[5] << 8));
        const auto product = static_cast<uint16_t>(guid.data[8] | (guid.data[9] << 8));
        if (SDL_GetGamepadPath(controller.controller.get())) {
            if (pad.vendor != 0 && vendor == pad.vendor && product == pad.product)
                return &controller;
        } else if (crc == pad.crc && (pad.vendor == 0 || (vendor == pad.vendor && product == pad.product))) {
            return &controller;
        }
    }
    return nullptr;
}

static void apply_direct_pad(EmuEnvState &emuenv, uint32_t *buttons, float axes[4], const DirectPad &pad, bool ext) {
    const auto &axis_binds = emuenv.cfg.controller_axis_binds;
    const auto axis = [&](int index) { return (index >= 0 && index < 6) ? pad.axes[index] : 0.f; };
    const auto held = [&](SDL_GamepadButton button) { return button >= 0 && button < 30 && (pad.buttons & (1u << button)); };
    const bool left_trigger = (pad.buttons & DIRECT_PAD_LEFT_TRIGGER) || axis(axis_binds[4]) > 0.5f;
    const bool right_trigger = (pad.buttons & DIRECT_PAD_RIGHT_TRIGGER) || axis(axis_binds[5]) > 0.5f;

    uint32_t pressed = 0;
    if (ext) {
        for (const auto &binding : get_controller_bindings_ext(emuenv)) {
            if (held(binding.controller))
                pressed |= binding.button;
        }
        if (left_trigger)
            pressed |= SCE_CTRL_L2;
        if (right_trigger)
            pressed |= SCE_CTRL_R2;
    } else {
        for (const auto &binding : get_controller_bindings(emuenv)) {
            if (held(binding.controller))
                pressed |= binding.button;
        }
    }
    if (emuenv.cfg.controller_swap_triggers)
        pressed = swap_shoulders_and_triggers(pressed, ext, left_trigger, right_trigger);
    *buttons |= pressed;

    const float multiplier = emuenv.cfg.controller_analog_multiplier;
    for (int i = 0; i < 4; i++)
        axes[i] += std::clamp(axis(axis_binds[i]) * multiplier, -1.0f, 1.0f);
}

static void retrieve_ctrl_data(EmuEnvState &emuenv, int port, bool is_v2, bool negative, bool from_ext_function, SceUInt32 &buttons, SceUInt8 &lx, SceUInt8 &ly, SceUInt8 &rx, SceUInt8 &ry) {
    std::lock_guard<std::mutex> guard(emuenv.ctrl.mutex);

    if (port == 0) {
        port++;
    }
    CtrlState &state = emuenv.ctrl;

    std::array<float, 4> axes{};
    axes.fill(0);

    const auto reset_axes = [&]() {
        // Re-center joysticks to (128,128). Range is (0-255,0-255).
        SceCtrlPadInputMode mode = from_ext_function ? state.input_mode_ext : state.input_mode;
        if (mode == SCE_CTRL_MODE_DIGITAL) {
            lx = 0x80;
            ly = 0x80;
            rx = 0x80;
            ry = 0x80;
        } else {
            lx = float_to_byte(axes[0]);
            ly = float_to_byte(axes[1]);
            rx = float_to_byte(axes[2]);
            ry = float_to_byte(axes[3]);
        }
        if (negative)
            buttons ^= ~0;
    };

    if (state.input_intercepted() || emuenv.drop_inputs) {
        reset_axes();
        return;
    }

    if (emuenv.cfg.current_config.pstv_mode) {
        if (port == 1) {
            apply_keyboard(&buttons, axes.data(), is_v2, emuenv);
        }
        for (const auto &[_, controller] : state.controllers) {
            const int controller_port = is_virtual_pad(controller) ? 1 : controller.port + 1;
            if (controller_port == port) {
                // sceCtrl ports are 1-based and SDL_GameController index is 0-based. Need to convert.
                apply_controller(emuenv, &buttons, axes.data(), controller.controller.get(), is_v2);
            }
        }
    } else if (port == 1) {
        // If not in PSTV mode, every controller input is considered as a port 1 input
        apply_keyboard(&buttons, axes.data(), is_v2, emuenv);
        for (const auto &[_, controller] : state.controllers) {
            apply_controller(emuenv, &buttons, axes.data(), controller.controller.get(), is_v2);
        }
    }

    if (port == 1) {
        for (auto &[device_id, pad] : direct_pads) {
            const Controller *sdl_pad = sdl_pad_for(state, pad);
            const int verdict = sdl_pad ? 0 : 1;
            if (verdict != pad.verdict) {
                pad.verdict = verdict;
                LOG_INFO("[CTRL] direct pad: device {} '{}': {}", device_id, pad.name,
                    sdl_pad ? fmt::format("SDL has gamepad '{}' for it, left to SDL", sdl_pad->name) : std::string("SDL has no gamepad for it, its Android input goes to the game"));
            }
            if (sdl_pad)
                continue;
            uint32_t pressed = 0;
            float moved[4] = {};
            apply_direct_pad(emuenv, &pressed, moved, pad, is_v2);
            buttons |= pressed;
            for (int i = 0; i < 4; i++) {
                if (std::fabs(moved[i]) > std::fabs(axes[i]))
                    axes[i] = moved[i];
            }
        }
    }

    reset_axes();
}

static void report_extra_buttons(EmuEnvState &emuenv, int port, uint32_t buttons) {
    static constexpr std::pair<uint32_t, const char *> EXTRA_BUTTONS[] = { { SCE_CTRL_L2, "L2" }, { SCE_CTRL_R2, "R2" }, { SCE_CTRL_L3, "L3" }, { SCE_CTRL_R3, "R3" } };
    static std::atomic<uint32_t> reported{ 0 };
    for (const auto &[bit, name] : EXTRA_BUTTONS) {
        if ((buttons & bit) && !(reported.fetch_or(bit, std::memory_order_relaxed) & bit))
            LOG_INFO("[CTRL] {} reached the game for the first time (port {}, PS TV mode {})", name, port, emuenv.cfg.current_config.pstv_mode ? "on" : "off");
    }
}

SceUInt32 ctrl_peek_buttons(EmuEnvState &emuenv, int port) {
    SceUInt32 buttons = 0;
    SceUInt8 lx, ly, rx, ry;
    retrieve_ctrl_data(emuenv, port, false, false, false, buttons, lx, ly, rx, ry);
    return buttons;
}

int ctrl_get(const SceUID thread_id, EmuEnvState &emuenv, int port, SceCtrlData2 *pData, SceUInt32 count, bool negative, bool is_peek, bool is_v2, bool from_ext) {
    emuenv.display.guest_input_reads.fetch_add(1, std::memory_order_relaxed);
    if (port > 1 && !emuenv.cfg.current_config.pstv_mode) {
        const char *export_name = "sceCtrl*Buffer*";
        return RET_ERROR(SCE_CTRL_ERROR_NO_DEVICE);
    }

    memset(pData, 0, sizeof(SceCtrlData2) * count);

    CtrlState &state = emuenv.ctrl;

    int nb_returned_data = 1;
    if (is_peek) {
        nb_returned_data = count;
    } else {
        const uint64_t current_vcount = emuenv.display.vblank_count.load();
        if (current_vcount <= state.last_vcount[port]) {
            // sceCtrlRead is blocking, wait for the next vsync for the buffer to be updated
            // (also mid-vblank under the fps hack so a game built for 30 fps keeps two samples per frame)
            auto thread = emuenv.kernel.get_thread(thread_id);

            wait_vblank(emuenv.display, emuenv.kernel, thread, state.last_vcount[port] + 1, false, emuenv.display.fps_hack);
        }
        uint64_t vblank_count = emuenv.display.vblank_count.load();
        nb_returned_data = std::min<int32_t>(count, vblank_count - state.last_vcount[port]);
        state.last_vcount[port] = vblank_count;
        if ((count > 0) && (nb_returned_data < 1))
            nb_returned_data = 1;
    }

    std::chrono::time_point<std::chrono::steady_clock> ts = std::chrono::steady_clock::now();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(ts.time_since_epoch()).count();
    pData->timeStamp = timestamp;
    retrieve_ctrl_data(emuenv, port, is_v2, negative, from_ext, pData->buttons, pData->lx, pData->ly, pData->rx, pData->ry);
    if (is_v2)
        report_extra_buttons(emuenv, port, negative ? ~pData->buttons : pData->buttons);

    for (int i = 1; i < nb_returned_data; i++) {
        memcpy(&pData[i], &pData[0], sizeof(SceCtrlData2));

        // update the timestamp, 1 vsync = 1/60 sec = 16 667 us earlier
        pData[i].timeStamp -= i * 16667ULL;
    }

    return nb_returned_data;
}

void ctrl_log_joystick_census(EmuEnvState &emuenv) {
    CtrlState &state = emuenv.ctrl;
    const std::lock_guard lock(state.mutex);
    std::string binds;
    for (const short bind : emuenv.cfg.controller_binds)
        binds += fmt::format("{}{}", binds.empty() ? "" : ",", bind);
    std::string axis_binds;
    for (const short bind : emuenv.cfg.controller_axis_binds)
        axis_binds += fmt::format("{}{}", axis_binds.empty() ? "" : ",", bind);
    LOG_INFO("[CTRL] pad settings: {} button binds [{}], {} axis binds [{}], swap triggers {}, analog multiplier {}", emuenv.cfg.controller_binds.size(), binds,
        emuenv.cfg.controller_axis_binds.size(), axis_binds, emuenv.cfg.controller_swap_triggers, emuenv.cfg.controller_analog_multiplier);
#ifdef __ANDROID__
    log_joystick_census(state, true);
#endif
}

void ctrl_set_direct_pad(EmuEnvState &emuenv, int device_id, const std::string &name, uint16_t vendor, uint16_t product, uint16_t crc, uint32_t buttons, const float axes[6]) {
    const std::lock_guard lock(emuenv.ctrl.mutex);
    const auto [entry, added] = direct_pads.try_emplace(device_id);
    DirectPad &pad = entry->second;
    if (added) {
        pad.name = name;
        pad.vendor = vendor;
        pad.product = product;
        pad.crc = crc;
        LOG_INFO("[CTRL] direct pad: device {} '{}' (vendor 0x{:04X} product 0x{:04X}, SDL GUID crc 0x{:04X}) sends Android input", device_id, name, vendor, product, crc);
    }
    pad.buttons = buttons;
    std::copy(axes, axes + 6, pad.axes);
}

void ctrl_remove_direct_pad(EmuEnvState &emuenv, int device_id) {
    const std::lock_guard lock(emuenv.ctrl.mutex);
    if (device_id < 0) {
        if (!direct_pads.empty())
            LOG_INFO("[CTRL] direct pads cleared ({})", direct_pads.size());
        direct_pads.clear();
    } else if (direct_pads.erase(device_id)) {
        LOG_INFO("[CTRL] direct pad: device {} removed", device_id);
    }
}
