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

#include <ngs/modules/envelope.h>
#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ngs {

// Multi-point amplitude automation

static float point_amplitude(const SceNgsEnvelopeParams *params, const int32_t index) {
    const int32_t count = static_cast<int32_t>(std::min<uint32_t>(params->uNumPoints, SCE_NGS_ENVELOPE_MAX_POINTS));
    if (count == 0)
        return 1.0f;
    const int32_t clamped = std::clamp(index, 0, count - 1);
    const float amp = params->envelopePoints[clamped].fAmplitude;
    return std::isfinite(amp) ? std::clamp(amp, 0.0f, 16.0f) : 1.0f;
}

static void restart_envelope(EnvelopeLogicalState *logical) {
    logical->position_in_segment_ms = 0.0;
    logical->total_position_ms = 0.0;
    logical->current_point = 0;
    logical->releasing = false;
}

void EnvelopeModule::on_param_change(const MemState &mem, ModuleData &data) {
    if (envelope_write_keeps_position)
        return;
    EnvelopeLogicalState *logical = data.get_logical_state<EnvelopeLogicalState>();
    if (!logical->releasing && data.parent->state != VOICE_STATE_FINALIZING)
        restart_envelope(logical);
}

void EnvelopeModule::on_key_on(const MemState &mem, ModuleData &data) {
    restart_envelope(data.get_logical_state<EnvelopeLogicalState>());
}

bool EnvelopeModule::can_release(const MemState &mem, ModuleData &data) {
    if (data.is_bypassed)
        return false;
    const SceNgsEnvelopeParams *params = data.get_parameters<SceNgsEnvelopeParams>(mem);
    return params && params->desc.id == SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID && params->uNumPoints > 0;
}

bool EnvelopeModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    // a keyed-off voice whose envelope cannot release ends here
    const bool finalizing = data.parent->state == VOICE_STATE_FINALIZING;
    if (data.is_bypassed)
        return finalizing;

    const SceNgsEnvelopeParams *params = data.get_parameters<SceNgsEnvelopeParams>(mem);
    if (!params)
        return finalizing;

    if (params->desc.id != SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID) {
        if (params->desc.id != 0)
            LOG_WARN_ONCE("[NGSDIAG] envelope module driven with unrecognised params struct id {:#010x} (passthrough)", params->desc.id);
        return finalizing;
    }

    const int32_t point_count = static_cast<int32_t>(std::min<uint32_t>(params->uNumPoints, SCE_NGS_ENVELOPE_MAX_POINTS));
    if (point_count == 0)
        return finalizing;

    Voice *voice = data.parent;
    const int32_t granularity = voice->rack->system->granularity;
    const int32_t sample_rate = voice->rack->system->sample_rate;
    if (granularity <= 0 || sample_rate <= 0)
        return false;

    // the buffer this voice is carrying: a source rack's product, or a bus rack's patched input
    float *signal = nullptr;
    if (voice->products[0].data)
        signal = reinterpret_cast<float *>(voice->products[0].data);
    else if (!voice->inputs.inputs.empty())
        signal = reinterpret_cast<float *>(voice->inputs.inputs[0].data());

    EnvelopeLogicalState *logical = data.get_logical_state<EnvelopeLogicalState>();
    SceNgsEnvelopeStates *state = data.get_state<SceNgsEnvelopeStates>();

    const double tick_ms = static_cast<double>(granularity) * 1000.0 / static_cast<double>(sample_rate);

    const auto height_at_current = [&]() -> float {
        if (logical->current_point >= point_count - 1)
            return point_amplitude(params, point_count - 1);
        const float from = point_amplitude(params, logical->current_point);
        const float to = point_amplitude(params, logical->current_point + 1);
        const double seg_ms = std::max<double>(params->envelopePoints[logical->current_point].uMsecsToNextPoint, 0.0);
        if (seg_ms <= 0.0)
            return to;
        double t = std::clamp(logical->position_in_segment_ms / seg_ms, 0.0, 1.0);
        if (params->envelopePoints[logical->current_point].eCurveType == SCE_NGS_ENVELOPE_CURVED)
            t = t * t * (3.0 - 2.0 * t); // smoothstep for the curved segments
        return static_cast<float>(from + (to - from) * t);
    };

    const bool keyed_off = voice->is_keyed_off || voice->state == VOICE_STATE_FINALIZING;
    if (!logical->releasing && keyed_off) {
        const float from = height_at_current();
        logical->releasing = true;
        logical->release_height = from;
        logical->release_start_height = from;
        logical->release_position_ms = 0.0;
    }
    const float start_gain = logical->releasing ? logical->release_height : height_at_current();

    float end_gain;
    bool terminate = false;
    if (logical->releasing) {
        logical->release_position_ms += tick_ms;
        const double rel_ms = std::max<double>(params->uReleaseMsecs, 0.0);
        const double t = rel_ms <= 0.0 ? 1.0 : std::clamp(logical->release_position_ms / rel_ms, 0.0, 1.0);
        end_gain = static_cast<float>(logical->release_start_height * (1.0 - t));
        logical->release_height = end_gain;
    } else {
        logical->position_in_segment_ms += tick_ms;
        logical->total_position_ms += tick_ms;
        // consume completed segments (0 length segments advance instantly)
        while (logical->current_point < point_count - 1) {
            const double seg_ms = std::max<double>(params->envelopePoints[logical->current_point].uMsecsToNextPoint, 0.0);
            if (logical->position_in_segment_ms < seg_ms)
                break;
            logical->position_in_segment_ms -= seg_ms;
            logical->current_point++;
            const int32_t loop_end = params->nLoopEnd;
            const int32_t loop_start = static_cast<int32_t>(params->uLoopStart);
            if (loop_end >= 0 && loop_end < point_count && loop_start <= loop_end
                && logical->current_point > loop_end) {
                logical->current_point = loop_start;
            }
        }
        end_gain = height_at_current();
        if (logical->current_point >= point_count - 1) {
            const bool timed = params->nLoopEnd == SCE_NGS_ENVELOPE_NO_LOOP_TERMINATE || params->nLoopEnd == SCE_NGS_ENVELOPE_NO_LOOP_RELEASE;
            if (timed && logical->position_in_segment_ms >= static_cast<double>(params->envelopePoints[point_count - 1].uMsecsToNextPoint)) {
                if (params->nLoopEnd == SCE_NGS_ENVELOPE_NO_LOOP_TERMINATE) {
                    terminate = true;
                } else {
                    logical->releasing = true;
                    logical->release_height = end_gain;
                    logical->release_start_height = end_gain;
                    logical->release_position_ms = 0.0;
                    voice->rack->system->voice_scheduler.off(mem, voice);
                }
            }
        }
    }

    // guest-visible states (games poll these to sequence fades)
    if (state) {
        state->fCurrentHeight = end_gain;
        state->fPosition = static_cast<float>(logical->releasing ? logical->release_position_ms : logical->total_position_ms);
        state->fReleaseScale = logical->releasing ? logical->release_start_height : 1.0f;
        state->nCurrentPoint = logical->current_point;
        state->nReleasing = logical->releasing ? 1 : 0;
    }

    if (signal) {
        // per-sample ramp across the tick to avoid zipper noise
        const float step = (end_gain - start_gain) / static_cast<float>(granularity);
        float gain = start_gain;
        for (int32_t frame = 0; frame < granularity; frame++) {
            signal[frame * 2 + 0] *= gain;
            signal[frame * 2 + 1] *= gain;
            gain += step;
        }
    }

    if (terminate) {
        static std::atomic<uint32_t> terminations{ 0 };
        const uint32_t n = terminations.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0)
            LOG_INFO("[NGSLIFE] envelope NO_LOOP_TERMINATE finished voice {} (callback {}) - {} so far", fmt::ptr(voice), voice->finished_callback ? "yes" : "none", n);
        return true;
    }

    if (logical->releasing && logical->release_position_ms >= std::max<double>(params->uReleaseMsecs, 0.0)) {
        return true;
    }

    return false;
}
} // namespace ngs
