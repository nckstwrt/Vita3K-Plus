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

#include "audio/impl/cubeb_audio.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cstring>

static uint64_t steady_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int queued_frames(const CubebAudioOutPort &port) {
    if (port.nb_buffers_ready <= 0 || port.frame_bytes == 0)
        return 0;
    return (port.nb_buffers_ready * port.len_bytes - port.head_position) / port.frame_bytes;
}

static int unplayed_frames(const CubebAudioOutPort &port, const uint64_t now) {
    return port.play_end_us > now ? static_cast<int>((port.play_end_us - now) * port.spec.rate / 1000000) : 0;
}

static int pacing_threshold(const CubebAudioOutPort &port) {
    const int callback = static_cast<int>(std::max(port.latency_frames, port.max_callback_frames));
    const int wanted = std::max(3 * callback, AUDIO_MIN_QUEUE_MS * static_cast<int>(port.spec.rate) / 1000);
    const int capacity = static_cast<int>(port.audio_buffers.size() - 1) * port.len_bytes / std::max(port.frame_bytes, 1);
    return std::min(wanted, capacity);
}

static long impl_cubeb_audio_callback(cubeb_stream *stream, void *user_data, const void *input, void *output, long nframes) {
    assert(user_data != nullptr);
    assert(stream != nullptr);
    CubebAudioOutPort *port = static_cast<CubebAudioOutPort *>(user_data);
    uint8_t *output_buffer = static_cast<uint8_t *>(output);
    const uint64_t now = steady_now_us();

    std::unique_lock<std::mutex> lock(port->mutex);
    const int ready = port->nb_buffers_ready;
    int next = port->next_audio_buffer;
    lock.unlock();

    // the guest only fills buffers that are not ready, so the ready ones can be read without the lock
    int bytes_given = 0;
    int released = 0;
    const int bytes_to_give = nframes * port->spec.channels * sizeof(uint16_t);
    while (bytes_given < bytes_to_give && released < ready) {
        AudioBuffer &audio_buffer = port->audio_buffers[next];
        // compute the number of bytes we can copy from this buffer to the output
        const int bytes_to_copy = std::min(bytes_to_give - bytes_given, port->len_bytes - audio_buffer.buffer_position);
        memcpy(&output_buffer[bytes_given], &audio_buffer.buffer[audio_buffer.buffer_position], bytes_to_copy);
        audio_buffer.buffer_position += bytes_to_copy;
        bytes_given += bytes_to_copy;
        if (audio_buffer.buffer_position == port->len_bytes) {
            next = (next + 1) % port->audio_buffers.size();
            released++;
        }
    }

    // ran dry so play silence instead of whatever the device buffer held before
    if (bytes_given < bytes_to_give)
        memset(&output_buffer[bytes_given], 0, bytes_to_give - bytes_given);

    lock.lock();
    port->next_audio_buffer = next;
    port->nb_buffers_ready -= released;
    port->head_position = port->nb_buffers_ready > 0 ? port->audio_buffers[next].buffer_position : 0;
    if (port->callbacks >= 8)
        port->max_callback_frames = std::max(port->max_callback_frames, static_cast<uint32_t>(nframes));
    const uint64_t callback_us = static_cast<uint64_t>(nframes) * 1000000 / std::max<uint32_t>(port->spec.rate, 1);
    port->play_end_us = std::min(std::max(port->play_end_us, now) + callback_us, now + 2 * callback_us);
    port->callbacks++;
    lock.unlock();
    port->cond_var.notify_all();

    return nframes;
}

static void impl_cubeb_state_callback(cubeb_stream *stm, void *user, cubeb_state state) {
    // we must give this function as a parameter to cubeb, but we don't care about it
}

CubebAudioOutPort::~CubebAudioOutPort() {
    if (out_stream) {
        cubeb_stream_stop(out_stream);
        cubeb_stream_destroy(out_stream);
    }
}

CubebAudioAdapter::CubebAudioAdapter(AudioState &audio_state)
    : AudioAdapter(audio_state) {}

CubebAudioAdapter::~CubebAudioAdapter() {
    if (cubeb_ctx)
        cubeb_destroy(cubeb_ctx);
}

bool CubebAudioAdapter::init() {
    if (cubeb_init(&cubeb_ctx, "Vita3K audio", nullptr) != CUBEB_OK) {
        LOG_ERROR("Could not initialize cubeb context");
        return false;
    }

    return true;
}

AudioOutPortPtr CubebAudioAdapter::open_port(int nb_channels, int freq, int nb_sample) {
    std::shared_ptr<CubebAudioOutPort> port = std::make_shared<CubebAudioOutPort>();
    port->spec = {
        // all the ps vita samples are signed 16 bits low edian
        .format = CUBEB_SAMPLE_S16LE,
        .rate = static_cast<uint32_t>(freq),
        .channels = static_cast<uint32_t>(nb_channels),
        .layout = CUBEB_LAYOUT_UNDEFINED,
        // we could use the params of sceAudioOutOpenPort to select some prefs, although I don't think it will change anything
        .prefs = CUBEB_STREAM_PREF_NONE
    };

    uint32_t latency = 0;
    cubeb_get_min_latency(cubeb_ctx, &port->spec, &latency);

    if (cubeb_stream_init(cubeb_ctx, &port->out_stream, "Vita3K audio out", nullptr, nullptr, nullptr,
            &port->spec, latency, impl_cubeb_audio_callback, impl_cubeb_state_callback, port.get())
        != CUBEB_OK) {
        LOG_ERROR("Could not initialize cubeb stream");
        return nullptr;
    }

    port->len_bytes = nb_sample * nb_channels * sizeof(uint16_t);
    port->len_microseconds = static_cast<uint64_t>(nb_sample) * 1'000'000ULL / freq;
    port->frame_bytes = nb_channels * sizeof(uint16_t);
    port->latency_frames = latency;

    const uint32_t sample_count = static_cast<uint32_t>(nb_sample);
    const uint32_t min_queue_frames = static_cast<uint32_t>(freq) * AUDIO_MIN_QUEUE_MS / 1000;
    const uint32_t threshold = std::max(3 * latency, min_queue_frames);
    const int nb_buffers = static_cast<int>((2 * threshold + sample_count - 1) / sample_count) + 1;
    port->audio_buffers.resize(nb_buffers);
    for (AudioBuffer &audio_buffer : port->audio_buffers) {
        // initialize all the buffers
        audio_buffer.buffer.resize(port->len_bytes);
        audio_buffer.buffer_position = 0;
    }

    cubeb_stream_start(port->out_stream);
    return port;
}

void CubebAudioAdapter::audio_output(AudioOutPort &out_port, const void *buffer) {
    CubebAudioOutPort &port = static_cast<CubebAudioOutPort &>(out_port);

    std::unique_lock<std::mutex> lock(port.mutex);

    if (out_port.stopping)
        return;

    const uint64_t deadline = steady_now_us() + 2 * port.len_microseconds;
    for (;;) {
        const uint64_t now = steady_now_us();
        const int pending = queued_frames(port) + unplayed_frames(port, now);
        const int threshold = pacing_threshold(port);
        if (pending <= threshold || now >= deadline)
            break;
        const uint64_t wait_us = std::min<uint64_t>(deadline - now, static_cast<uint64_t>(pending - threshold) * 1000000 / std::max<uint32_t>(port.spec.rate, 1) + 1);
        port.cond_var.wait_for(lock, std::chrono::microseconds(wait_us));
        if (out_port.stopping)
            return;
    }
    port.cond_var.wait(lock, [&] { return port.nb_buffers_ready < static_cast<int>(port.audio_buffers.size()) || out_port.stopping; });
    if (out_port.stopping)
        return;

    assert(port.nb_buffers_ready < port.audio_buffers.size());
    if (buffer) {
        // the buffer can be empty to drain the port
        int next_buffer_pos = (port.next_audio_buffer + port.nb_buffers_ready) % port.audio_buffers.size();
        // we could unlock the lock here and re-lock it right after, but will this be faster?
        memcpy(port.audio_buffers[next_buffer_pos].buffer.data(), buffer, port.len_bytes);
        port.audio_buffers[next_buffer_pos].buffer_position = 0;
        port.nb_buffers_ready++;
    }

    lock.unlock();
    port.cond_var.notify_one();
}

void CubebAudioAdapter::set_volume(AudioOutPort &out_port, float volume) {
    CubebAudioOutPort &port = static_cast<CubebAudioOutPort &>(out_port);
    cubeb_stream_set_volume(port.out_stream, volume);
}

int CubebAudioAdapter::get_rest_sample(AudioOutPort &out_port) {
    CubebAudioOutPort &port = static_cast<CubebAudioOutPort &>(out_port);
    const std::lock_guard<std::mutex> lock(port.mutex);
    return queued_frames(port);
}

void CubebAudioAdapter::switch_state(const bool pause) {
    for (auto &[_, out_port] : state.out_ports) {
        CubebAudioOutPort &port = static_cast<CubebAudioOutPort &>(*out_port);
        if (pause)
            cubeb_stream_stop(port.out_stream);
        else
            cubeb_stream_start(port.out_stream);
    }
}

void CubebAudioAdapter::wake_all_ports() {
    for (auto &[_, port_ptr] : state.out_ports) {
        auto &port = static_cast<CubebAudioOutPort &>(*port_ptr);
        {
            std::lock_guard<std::mutex> lock(port.mutex);
        }
        port.cond_var.notify_all();
    }
}
