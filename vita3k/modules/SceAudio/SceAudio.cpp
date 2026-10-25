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

#include <module/module.h>

#include <audio/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <util/lock_and_find.h>
#include <util/tracy.h>

TRACY_MODULE_NAME(SceAudio);

enum SceAudioOutMode {
    SCE_AUDIO_OUT_MODE_MONO = 0,
    SCE_AUDIO_OUT_MODE_STEREO = 1
};

enum SceAudioOutAlcMode {
    SCE_AUDIO_ALC_OFF,
    SCE_AUDIO_ALC_MODE1,
    SCE_AUDIO_ALC_MODE_MAX
};

enum SceAudioOutPortType {
    //! Used for main audio output, freq must be set to 48000 Hz
    SCE_AUDIO_OUT_PORT_TYPE_MAIN = 0,
    //! Used for Background Music port
    SCE_AUDIO_OUT_PORT_TYPE_BGM = 1,
    //! Used for voice chat port
    SCE_AUDIO_OUT_PORT_TYPE_VOICE = 2
};

enum SceAudioOutErrorCode : uint32_t {
    SCE_AUDIO_OUT_ERROR_NOT_OPENED = 0x80260001,
    SCE_AUDIO_OUT_ERROR_BUSY = 0x80260002,
    SCE_AUDIO_OUT_ERROR_INVALID_PORT = 0x80260003,
    SCE_AUDIO_OUT_ERROR_INVALID_POINTER = 0x80260004,
    SCE_AUDIO_OUT_ERROR_PORT_FULL = 0x80260005,
    SCE_AUDIO_OUT_ERROR_INVALID_SIZE = 0x80260006,
    SCE_AUDIO_OUT_ERROR_INVALID_FORMAT = 0x80260007,
    SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ = 0x80260008,
    SCE_AUDIO_OUT_ERROR_INVALID_VOLUME = 0x80260009,
    SCE_AUDIO_OUT_ERROR_INVALID_PORT_TYPE = 0x8026000A,
    SCE_AUDIO_OUT_ERROR_INVALID_FX_TYPE = 0x8026000B,
    SCE_AUDIO_OUT_ERROR_INVALID_CONF_TYPE = 0x8026000C,
    SCE_AUDIO_OUT_ERROR_OUT_OF_MEMORY = 0x8026000D
};

enum SceAudioOutChannelFlag {
    SCE_AUDIO_VOLUME_FLAG_L_CH = 1, //!< Left Channel
    SCE_AUDIO_VOLUME_FLAG_R_CH = 2 //!< Right Channel
};

enum SceAudioOutConfigType {
    SCE_AUDIO_OUT_CONFIG_TYPE_LEN,
    SCE_AUDIO_OUT_CONFIG_TYPE_FREQ,
    SCE_AUDIO_OUT_CONFIG_TYPE_MODE
};

template <>
std::string to_debug_str<SceAudioOutPortType>(const MemState &mem, SceAudioOutPortType type) {
    switch (type) {
    case SCE_AUDIO_OUT_PORT_TYPE_MAIN:
        return "SCE_AUDIO_OUT_PORT_TYPE_MAIN";
    case SCE_AUDIO_OUT_PORT_TYPE_BGM:
        return "SCE_AUDIO_OUT_PORT_TYPE_BGM";
    case SCE_AUDIO_OUT_PORT_TYPE_VOICE:
        return "SCE_AUDIO_OUT_PORT_TYPE_VOICE";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceAudioOutConfigType>(const MemState &mem, SceAudioOutConfigType type) {
    switch (type) {
    case SCE_AUDIO_OUT_CONFIG_TYPE_LEN:
        return "SCE_AUDIO_OUT_CONFIG_TYPE_LEN";
    case SCE_AUDIO_OUT_CONFIG_TYPE_FREQ:
        return "SCE_AUDIO_OUT_CONFIG_TYPE_FREQ";
    case SCE_AUDIO_OUT_CONFIG_TYPE_MODE:
        return "SCE_AUDIO_OUT_CONFIG_TYPE_MODE";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceAudioOutMode>(const MemState &mem, SceAudioOutMode type) {
    switch (type) {
    case SCE_AUDIO_OUT_MODE_MONO:
        return "SCE_AUDIO_OUT_MODE_MONO";
    case SCE_AUDIO_OUT_MODE_STEREO:
        return "SCE_AUDIO_OUT_MODE_STEREO";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceAudioOutAlcMode>(const MemState &mem, SceAudioOutAlcMode type) {
    switch (type) {
    case SCE_AUDIO_ALC_OFF:
        return "SCE_AUDIO_ALC_OFF";
    case SCE_AUDIO_ALC_MODE1:
        return "SCE_AUDIO_ALC_MODE1";
    case SCE_AUDIO_ALC_MODE_MAX:
        return "SCE_AUDIO_ALC_MODE_MAX";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceAudioOutChannelFlag>(const MemState &mem, SceAudioOutChannelFlag type) {
    switch (static_cast<uint32_t>(type)) {
    case SCE_AUDIO_VOLUME_FLAG_L_CH:
        return "SCE_AUDIO_VOLUME_FLAG_L_CH";
    case SCE_AUDIO_VOLUME_FLAG_R_CH:
        return "SCE_AUDIO_VOLUME_FLAG_R_CH";
        // Third case for special flag SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH
        // passed by games to set the volume of both channels at the same time
    case (SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH):
        return "SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH";
    }
    return std::to_string(type);
}

static bool audio_cfg_traced(std::atomic<uint32_t> &calls, uint32_t &n) {
    n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    return n <= 16 || (n & (n - 1)) == 0;
}

constexpr int MAIN_PORT_SLOTS = 8;

static int port_key(int port) {
    const uint32_t value = static_cast<uint32_t>(port);
    const uint32_t group = (value >> 8) & 0xFF;
    const int slot = static_cast<int>(value & 0xFF);
    if (group == SCE_AUDIO_OUT_PORT_TYPE_MAIN)
        return slot < MAIN_PORT_SLOTS ? slot : -1;
    if (group == SCE_AUDIO_OUT_PORT_TYPE_BGM || group == SCE_AUDIO_OUT_PORT_TYPE_VOICE)
        return static_cast<int>(group << 8);
    return -1;
}

EXPORT(int, sceAudioOutGetAdopt, SceAudioOutPortType type) {
    TRACY_FUNC(sceAudioOutGetAdopt, type);
    // this is used in case the vita is using voice chat, not useful for us
    // all types of audio ports are always enabled
    return 1;
}

EXPORT(int, sceAudioOutGetConfig, int port, SceAudioOutConfigType type) {
    TRACY_FUNC(sceAudioOutGetConfig, port, type);
    const AudioOutPortPtr prt = lock_and_find(port_key(port), emuenv.audio.out_ports, emuenv.audio.mutex);
    if (!prt)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    int value;
    switch (type) {
    case SCE_AUDIO_OUT_CONFIG_TYPE_LEN:
        value = prt->len;
        break;
    case SCE_AUDIO_OUT_CONFIG_TYPE_FREQ:
        value = prt->freq;
        break;
    case SCE_AUDIO_OUT_CONFIG_TYPE_MODE:
        value = prt->mode;
        break;
    default:
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_CONF_TYPE);
    }

    static std::atomic<uint32_t> calls{ 0 };
    uint32_t n;
    if (audio_cfg_traced(calls, n))
        LOG_INFO("[AUDIOCFG] {}: thread {} port {} type {} config type {} = {} (#{})", export_name, thread_id, port, prt->type, static_cast<int>(type), value, n);
    return value;
}

EXPORT(int, sceAudioOutGetPortVolume_forUser) {
    TRACY_FUNC(sceAudioOutGetPortVolume_forUser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutOpenPort, SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode) {
    TRACY_FUNC(sceAudioOutOpenPort, type, len, freq, mode);
    if (type < SCE_AUDIO_OUT_PORT_TYPE_MAIN || type > SCE_AUDIO_OUT_PORT_TYPE_VOICE) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT_TYPE);
    }
    if (type == SCE_AUDIO_OUT_PORT_TYPE_MAIN && freq != 48000) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ);
    }
    if ((mode != SCE_AUDIO_OUT_MODE_MONO) && (mode != SCE_AUDIO_OUT_MODE_STEREO)) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_FORMAT);
    }
    if (len <= 0) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_SIZE);
    }

    const int channels = (mode == SCE_AUDIO_OUT_MODE_MONO) ? 1 : 2;

    const std::lock_guard<std::mutex> lock(emuenv.audio.mutex);
    int port_id = -1;
    if (type == SCE_AUDIO_OUT_PORT_TYPE_MAIN) {
        for (int slot = MAIN_PORT_SLOTS - 1; slot >= 0 && port_id < 0; --slot)
            if (!emuenv.audio.out_ports.contains(slot))
                port_id = slot;
    } else if (!emuenv.audio.out_ports.contains(static_cast<int>(type) << 8)) {
        port_id = static_cast<int>(type) << 8;
    }
    if (port_id < 0)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_PORT_FULL);

    AudioOutPortPtr port = emuenv.audio.open_port(channels, freq, len);
    if (!port)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_NOT_OPENED);

    // Save the port configuration
    port->type = type;
    port->len = len;
    port->freq = freq;
    port->mode = mode;

    emuenv.audio.out_ports.emplace(port_id, port);

    return port_id;
}

EXPORT(int, sceAudioOutOutput, int port, const void *buf) {
    TRACY_FUNC(sceAudioOutOutput, port, buf);
    const AudioOutPortPtr prt = lock_and_find(port_key(port), emuenv.audio.out_ports, emuenv.audio.mutex);
    if (!prt) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    }

    // Empty "buf" variable is valid. It mean wait until sound output is completed.
    // Because this function always returns when all sound is out, then on empty buf it returns immediately.
    // Return value is the number of samples (value of 0 or greater) registered to the audio driver for normal termination.
    if (!buf)
        return 0;

    note_audio_thread(emuenv.kernel, thread_id, export_name);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    }
    // is it really useful to update the thread status?
    thread->update_status(ThreadStatus::wait);
    emuenv.audio.audio_output(*prt, buf);
    thread->update_status(ThreadStatus::run);

    return prt->len;
}

EXPORT(int, sceAudioOutGetRestSample, int port) {
    TRACY_FUNC(sceAudioOutGetRestSample, port);
    const AudioOutPortPtr prt = lock_and_find(port_key(port), emuenv.audio.out_ports, emuenv.audio.mutex);
    if (!prt) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    }
    int samples_available = emuenv.audio.get_rest_sample(*prt);
    if (prt->type == SCE_AUDIO_OUT_PORT_TYPE_MAIN) {
        samples_available = std::clamp(samples_available, 0, prt->len);
    } else {
        // for other port types only the granularity value set for the len argument or 0 can be returned.
        if (samples_available < prt->len / 2)
            samples_available = 0;
        else
            samples_available = prt->len;
    }
    return samples_available;
}

EXPORT(int, sceAudioOutOpenExtPort) {
    TRACY_FUNC(sceAudioOutOpenExtPort);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutReleasePort, int port) {
    TRACY_FUNC(sceAudioOutReleasePort, port);
    const std::lock_guard<std::mutex> guard(emuenv.audio.mutex);
    if (!emuenv.audio.out_ports.erase(port_key(port))) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    }

    return 0;
}

EXPORT(int, sceAudioOutSetAdoptMode) {
    TRACY_FUNC(sceAudioOutSetAdoptMode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetAdopt_forUser) {
    TRACY_FUNC(sceAudioOutSetAdopt_forUser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetAlcMode, SceAudioOutAlcMode mode) {
    TRACY_FUNC(sceAudioOutSetAlcMode, mode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetCompress) {
    TRACY_FUNC(sceAudioOutSetCompress);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetConfig, int port, int len, int freq, int mode) {
    TRACY_FUNC(sceAudioOutSetConfig, port, len, freq, mode);
    if (len == 0)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_SIZE);

    if ((mode >= 0) && (mode != SCE_AUDIO_OUT_MODE_MONO) && (mode != SCE_AUDIO_OUT_MODE_STEREO))
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_FORMAT);

    AudioOutPortPtr prt = lock_and_find(port_key(port), emuenv.audio.out_ports, emuenv.audio.mutex);
    if (!prt)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    if ((freq >= 0) && (prt->type == SCE_AUDIO_OUT_PORT_TYPE_MAIN) && (freq != 48000))
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ);

    const auto set_len = len > 0 ? len : prt->len;
    const auto set_freq = freq >= 0 ? freq : prt->freq;
    const auto set_mode = mode >= 0 ? mode : prt->mode;

    static std::atomic<uint32_t> calls{ 0 };
    uint32_t n;
    const bool traced = audio_cfg_traced(calls, n);

    if ((set_len == prt->len) && (set_freq == prt->freq) && (set_mode == prt->mode)) {
        if (traced)
            LOG_INFO("[AUDIOCFG] {}: thread {} port {} type {} unchanged len {} freq {} mode {} (#{})", export_name, thread_id, port, prt->type, set_len, set_freq, set_mode, n);
        return 0;
    }

    const int type = prt->type;
    const int left_volume = prt->left_channel_volume;
    const int right_volume = prt->right_channel_volume;
    const float volume = prt->volume;
    const int old_len = prt->len;
    const int old_freq = prt->freq;
    const int old_mode = prt->mode;

    if (CALL_EXPORT(sceAudioOutReleasePort, port) != 0)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    const auto channels = (set_mode == SCE_AUDIO_OUT_MODE_MONO) ? 1 : 2;

    prt = emuenv.audio.open_port(channels, set_freq, set_len);
    if (!prt)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_NOT_OPENED);

    // Save the port configuration
    prt->type = type;
    prt->freq = set_freq;
    prt->len = set_len;
    prt->mode = set_mode;
    prt->left_channel_volume = left_volume;
    prt->right_channel_volume = right_volume;
    emuenv.audio.set_volume(*prt, volume);

    if (traced)
        LOG_INFO("[AUDIOCFG] {}: thread {} port {} type {} rebuilt len {} -> {} freq {} -> {} mode {} -> {}, kept volume L {} R {} ({:.3f}) (#{})",
            export_name, thread_id, port, type, old_len, set_len, old_freq, set_freq, old_mode, set_mode, left_volume, right_volume, volume, n);

    const std::lock_guard<std::mutex> lock(emuenv.audio.mutex);
    emuenv.audio.out_ports.emplace(port_key(port), prt);

    return 0;
}

EXPORT(int, sceAudioOutSetEffectType) {
    TRACY_FUNC(sceAudioOutSetEffectType);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetPortVolume_forUser) {
    TRACY_FUNC(sceAudioOutSetPortVolume_forUser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetVolume, int port, SceAudioOutChannelFlag ch, int *vol) {
    TRACY_FUNC(sceAudioOutSetVolume, port, ch, vol[0], vol[1]);
    if (ch == 0) // no channel selected, no changes
        return 0;

    const AudioOutPortPtr prt = lock_and_find(port_key(port), emuenv.audio.out_ports, emuenv.audio.mutex);

    if (!prt)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);

    const int left = (ch & SCE_AUDIO_VOLUME_FLAG_L_CH) ? vol[0] : prt->left_channel_volume;
    const int right = (ch & SCE_AUDIO_VOLUME_FLAG_R_CH) ? vol[1] : prt->right_channel_volume;
    const float volume_level = (static_cast<float>(left + right) / static_cast<float>(SCE_AUDIO_VOLUME_0DB * 2));

    // then update channel volumes in case there was a change
    prt->left_channel_volume = left;
    prt->right_channel_volume = right;
    emuenv.audio.set_volume(*prt, volume_level);

    return 0;
}
