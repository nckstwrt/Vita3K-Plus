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

#include <renderer/functions.h>
#include <renderer/state.h>
#include <renderer/types.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>

#include <dialog/state.h>
#include <overlay/common_dialog.h>
#include <overlay/display_manager.h>
#include <overlay/font.h>
#include <overlay/pause_overlay.h>
#include <overlay/perf_overlay.h>
#include <overlay/shader_compile_notice.h>
#include <renderer/gl/state.h>
#include <renderer/gl/types.h>
#include <renderer/vulkan/functions.h>

#include <gxm/functions.h>
#include <mem/functions.h>
#include <util/log.h>

namespace renderer {

void State::run_render_thread_tasks() {
    std::vector<std::function<void()>> tasks;
    {
        const std::lock_guard<std::mutex> lock(render_thread_tasks_mutex);
        tasks.swap(render_thread_tasks);
    }
    for (auto &task : tasks)
        task();
}

std::vector<uint32_t> State::dump_frame_on_render_thread(DisplayState &display, uint32_t &width, uint32_t &height) {
    if (!render_thread || render_thread->get_id() == std::this_thread::get_id())
        return dump_frame(display, width, height);

    struct Frame {
        std::vector<uint32_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    auto task = std::make_shared<std::packaged_task<Frame()>>([this, &display]() {
        Frame frame;
        frame.pixels = dump_frame(display, frame.width, frame.height);
        return frame;
    });
    std::future<Frame> result = task->get_future();
    {
        const std::lock_guard<std::mutex> lock(render_thread_tasks_mutex);
        render_thread_tasks.emplace_back([task]() { (*task)(); });
    }

    if (result.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        LOG_ERROR("The render thread did not capture the frame in time");
        return {};
    }
    try {
        Frame frame = result.get();
        width = frame.width;
        height = frame.height;
        return std::move(frame.pixels);
    } catch (const std::exception &e) {
        LOG_ERROR("Frame capture failed: {}", e.what());
        return {};
    }
}

void State::update_overlays() {
    if (!overlay_manager)
        return;

    if (show_compile_shaders) {
        const auto now = std::chrono::steady_clock::now();
        const uint32_t newly_compiled = shaders_count_compiled;
        if (newly_compiled > 0) {
            m_shaders_compiled_count += newly_compiled;
            shaders_count_compiled = 0;
            m_shaders_compiled_time = now;
            if (current_backend == Backend::Vulkan) {
                constexpr bool log_pipeline_bursts = false;
                if constexpr (log_pipeline_bursts)
                    LOG_INFO("[PIPELINE] {} pipeline(s) compiled in this burst ({} since this notice began); key normalisation has avoided {} redundant compile(s) so far this session", newly_compiled, m_shaders_compiled_count, pipelines_redundant_avoided);
            }

            auto notice = overlay_manager->get<overlay::shader_compile_notice>();
            if (!notice)
                notice = overlay_manager->create<overlay::shader_compile_notice>();
            notice->update_count(m_shaders_compiled_count, current_backend == Backend::Vulkan);
        } else if (m_shaders_compiled_count > 0) {
            auto notice = overlay_manager->get<overlay::shader_compile_notice>();
            if (notice && notice->should_hide()) {
                overlay_manager->remove<overlay::shader_compile_notice>();
                m_shaders_compiled_count = 0;
            }
        }
    }

    {
        const bool is_paused = paused.load(std::memory_order_relaxed);
        overlay_manager->set_paused(is_paused);
        if (is_paused) {
            if (!overlay_manager->get<overlay::pause_overlay>())
                overlay_manager->create<overlay::pause_overlay>();
        } else {
            if (overlay_manager->get<overlay::pause_overlay>())
                overlay_manager->remove<overlay::pause_overlay>();
        }
    }

    if (perf_overlay.enabled && perf_overlay.fps > 0) {
        auto perf = overlay_manager->get<overlay::perf_overlay>();
        if (!perf)
            perf = overlay_manager->create<overlay::perf_overlay>();

        perf->set_position(static_cast<overlay::screen_quadrant>(perf_overlay.position));
        perf->set_detail_level(static_cast<overlay::perf_detail_level>(perf_overlay.detail));
        perf->set_fps_data(perf_overlay.fps, perf_overlay.avg_fps, perf_overlay.min_fps,
            perf_overlay.max_fps, perf_overlay.ms_per_frame,
            perf_overlay.fps_values.data(), perf_overlay.fps_values_count,
            perf_overlay.current_fps_offset);
    } else {
        auto perf = overlay_manager->get<overlay::perf_overlay>();
        if (perf)
            overlay_manager->remove<overlay::perf_overlay>();
    }

    if (common_dialog) {
        auto dlg = overlay_manager->get<overlay::common_dialog_overlay>();
        if (common_dialog->type != NO_DIALOG && common_dialog->status == SCE_COMMON_DIALOG_STATUS_RUNNING) {
            bool just_created = false;
            if (!dlg) {
                dlg = overlay_manager->create<overlay::common_dialog_overlay>();
                just_created = true;
            }
            if (dlg->poll_dialog(*common_dialog, sys_date_format, sys_button)
                && common_dialog->type != TROPHY_SETUP_DIALOG) {
                if (just_created || dlg->input_loop_exited()) {
                    dlg->reset_input_loop();
                    overlay_manager->attach_thread_input("common_dialog", dlg);
                }
            }
        } else {
            if (dlg)
                overlay_manager->remove<overlay::common_dialog_overlay>();
        }
    }
}

void State::init_overlay_font_dirs() {
    overlay::fontmgr::set_system_lang(sys_lang);

    if (frame) {
        overlay::fontmgr::set_system_font_dirs(frame->font_dirs());
    }

    if (!vita_fs_path.empty()) {
        auto fw_dir = fs_utils::path_to_utf8(vita_fs_path / "sa0" / "data" / "font" / "pvf");
        if (!fw_dir.empty()) {
            if (fw_dir.back() != '/' && fw_dir.back() != '\\')
                fw_dir += '/';
            overlay::fontmgr::set_firmware_font_dir(fw_dir);
        }
    }

    {
        auto icons_dir = fs_utils::path_to_utf8(static_assets / "icons");
        if (!icons_dir.empty()) {
            if (icons_dir.back() != '/' && icons_dir.back() != '\\')
                icons_dir += '/';
            overlay::resource_config::set_icons_dir(icons_dir);
        }
    }

    LOG_INFO("Overlay font firmware dir: {}", overlay::fontmgr::get_firmware_font_dir().empty() ? "(none)" : overlay::fontmgr::get_firmware_font_dir());
    LOG_INFO("Overlay font system dirs: {} entries", overlay::fontmgr::get_system_font_dirs().size());
    for (const auto &d : overlay::fontmgr::get_system_font_dirs())
        LOG_DEBUG("  system font dir: {}", d);
}

void set_depth_bias(State &state, Context *ctx, bool is_front, int factor, int units) {
    renderer::add_state_set_command(ctx, renderer::GXMState::DepthBias, is_front, factor, units);
}

void set_depth_func(State &state, Context *ctx, bool is_front, SceGxmDepthFunc depth_func) {
    renderer::add_state_set_command(ctx, renderer::GXMState::DepthFunc, is_front, depth_func);
}

void set_depth_write_enable_mode(State &state, Context *ctx, bool is_front, SceGxmDepthWriteMode enable) {
    renderer::add_state_set_command(ctx, renderer::GXMState::DepthWriteEnable, is_front, enable);
}

void set_point_line_width(State &state, Context *ctx, bool is_front, unsigned int width) {
    renderer::add_state_set_command(ctx, renderer::GXMState::PointLineWidth, is_front, width);
}

void set_polygon_mode(State &state, Context *ctx, bool is_front, SceGxmPolygonMode mode) {
    renderer::add_state_set_command(ctx, renderer::GXMState::PolygonMode, is_front, mode);
}

void set_stencil_func(State &state, Context *ctx, bool is_front, SceGxmStencilFunc func, SceGxmStencilOp stencilFail, SceGxmStencilOp depthFail, SceGxmStencilOp depthPass, unsigned char compareMask, unsigned char writeMask) {
    renderer::add_state_set_command(ctx, renderer::GXMState::StencilFunc, is_front, func, stencilFail, depthFail, depthPass, compareMask, writeMask);
}

void set_stencil_ref(State &state, Context *ctx, bool is_front, unsigned char sref) {
    renderer::add_state_set_command(ctx, renderer::GXMState::StencilRef, is_front, sref);
}

void set_program(State &state, Context *ctx, Ptr<const void> program, const std::shared_ptr<ProgramBinding> &binding, bool is_fragment) {
    auto *binding_payload = new std::shared_ptr<ProgramBinding>(binding);
    if (!renderer::add_state_set_command(ctx, renderer::GXMState::Program, program, binding_payload, is_fragment))
        delete binding_payload;
}

void set_cull_mode(State &state, Context *ctx, SceGxmCullMode cull) {
    renderer::add_state_set_command(ctx, renderer::GXMState::CullMode, cull);
}

void set_texture(State &state, Context *ctx, const std::uint32_t tex_index, const SceGxmTexture tex) {
    renderer::add_state_set_command(ctx, renderer::GXMState::Texture, tex_index, tex);
}

void set_viewport_real(State &state, Context *ctx, float xOffset, float yOffset, float zOffset, float xScale, float yScale, float zScale) {
    renderer::add_state_set_command(ctx, renderer::GXMState::Viewport, false, xOffset, yOffset,
        zOffset, xScale, yScale, zScale);
}

void set_viewport_flat(State &state, Context *ctx) {
    renderer::add_state_set_command(ctx, renderer::GXMState::Viewport, true);
}

void set_region_clip(State &state, Context *ctx, SceGxmRegionClipMode mode, unsigned int xMin, unsigned int xMax, unsigned int yMin, unsigned int yMax) {
    renderer::add_state_set_command(ctx, renderer::GXMState::RegionClip, mode, xMin, xMax, yMin, yMax);
}

void set_two_sided_enable(State &state, Context *ctx, SceGxmTwoSidedMode mode) {
    renderer::add_state_set_command(ctx, renderer::GXMState::TwoSided, mode);
}

void set_side_fragment_program_enable(State &state, Context *ctx, const bool is_front, SceGxmFragmentProgramMode mode) {
    renderer::add_state_set_command(ctx, renderer::GXMState::FragmentProgramEnable, is_front, mode);
}

void set_context(State &state, Context *ctx, RenderTarget *target, SceGxmColorSurface *color_surface, SceGxmDepthStencilSurface *depth_stencil_surface) {
    renderer::add_command(ctx, renderer::CommandOpcode::SetContext, nullptr, target, color_surface, depth_stencil_surface);
}

namespace {
constexpr bool GUEST_STREAM_SNAPSHOT = true;
constexpr size_t SNAP_RING_SIZE = 64u * 1024u * 1024u;
// only a small stream is copied (every stream a game has been seen to recycle is a small per-draw block)
// a game that indexes deep into a large shared buffer which would overlap the ring every frame
constexpr uint32_t SNAP_MAX_BYTES = 16u * 1024u;

struct SnapHeader {
    uint64_t handle;
    uint32_t size;
    uint32_t reserved;
};

std::mutex g_snap_mutex;
std::vector<uint8_t> g_snap_ring;
uint64_t g_snap_cursor = 0;
uint64_t g_snap_copied = 0;
uint64_t g_snap_bound = 0;
uint64_t g_snap_stale = 0;
uint64_t g_snap_mismatch = 0;
int64_t g_snap_next_report = 0;
std::atomic<bool> g_snap_disabled{ false };

uint32_t snap_align(const uint32_t size) {
    return (size + 63u) & ~63u;
}

// copies one stream into a new slot of the ring and returns its handle
uint64_t snap_copy_locked(MemState &mem, const uint32_t addr, const uint32_t size) {
    if (!addr || !size || !is_valid_addr_range(mem, addr, addr + size))
        return ~0ull;
    if (g_snap_ring.empty())
        g_snap_ring.resize(SNAP_RING_SIZE);

    const uint32_t slot = snap_align(static_cast<uint32_t>(sizeof(SnapHeader)) + size);
    uint64_t handle = g_snap_cursor;
    size_t offset = static_cast<size_t>(handle % SNAP_RING_SIZE);
    if (offset + slot > SNAP_RING_SIZE) {
        // never let a slot straddle the end of the ring
        handle += SNAP_RING_SIZE - offset;
        offset = 0;
    }
    g_snap_cursor = handle + slot;

    uint8_t *const at = g_snap_ring.data() + offset;
    const SnapHeader header = { handle, size, 0 };
    memcpy(at, &header, sizeof(header));
    memcpy_from_guest(mem, at + sizeof(SnapHeader), addr, size);
    g_snap_copied++;
    return handle;
}
} // namespace

void stream_snapshot_kick(MemState &mem, CommandList &list) {
    if (!GUEST_STREAM_SNAPSHOT || g_snap_disabled.load(std::memory_order_relaxed) || !list.first)
        return;

    Command *from = list.first;
    for (Command *cmd = list.first; cmd; cmd = cmd == list.last ? nullptr : cmd->next) {
        if (cmd->opcode == CommandOpcode::MidSceneFlush)
            from = cmd == list.last ? nullptr : cmd->next;
    }

    const std::lock_guard<std::mutex> lock(g_snap_mutex);
    if (g_snap_disabled.load(std::memory_order_relaxed))
        return;
    for (Command *cmd = from; cmd; cmd = cmd == list.last ? nullptr : cmd->next) {
        if (cmd->opcode != CommandOpcode::SetState)
            continue;
        CommandHelper helper(cmd);
        if (helper.pop<GXMState>() != GXMState::VertexStream)
            continue;
        const Ptr<const uint8_t> stream = helper.pop<Ptr<const uint8_t>>();
        helper.pop<std::size_t>();
        const std::size_t length = helper.pop<std::size_t>();
        uint64_t handle = length <= SNAP_MAX_BYTES ? snap_copy_locked(mem, stream.address(), static_cast<uint32_t>(length)) : ~0ull;
        helper.push(handle);
    }
}

const uint8_t *stream_snapshot_get(const uint64_t handle, const uint32_t size) {
    if (!GUEST_STREAM_SNAPSHOT || handle == ~0ull || !size)
        return nullptr;

    const std::lock_guard<std::mutex> lock(g_snap_mutex);
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now >= g_snap_next_report) {
        g_snap_next_report = now + 10000000;
        LOG_INFO("[SNAPSHOT] copied {} bound {} stale {} mismatch {}", g_snap_copied, g_snap_bound, g_snap_stale, g_snap_mismatch);
    }

    if (g_snap_ring.empty() || g_snap_cursor < handle || g_snap_cursor - handle > SNAP_RING_SIZE) {
        g_snap_stale++;
        return nullptr;
    }
    const uint8_t *const at = g_snap_ring.data() + static_cast<size_t>(handle % SNAP_RING_SIZE);
    SnapHeader header;
    memcpy(&header, at, sizeof(header));
    if (header.handle != handle || size > header.size) {
        g_snap_mismatch++;
        return nullptr;
    }
    g_snap_bound++;

    thread_local std::vector<uint8_t> scratch;
    scratch.assign(at + sizeof(SnapHeader), at + sizeof(SnapHeader) + size);
    return scratch.data();
}

void stream_snapshot_reset() {
    const std::lock_guard<std::mutex> lock(g_snap_mutex);
    g_snap_disabled = false;
    std::vector<uint8_t>().swap(g_snap_ring);
    g_snap_copied = 0;
    g_snap_bound = 0;
    g_snap_stale = 0;
    g_snap_mismatch = 0;
}

void stream_snapshot_disable_for_program(const uint32_t program_addr, const uint32_t program_flags) {
    if (g_snap_disabled.exchange(true))
        return;
    {
        const std::lock_guard<std::mutex> lock(g_snap_mutex);
        std::vector<uint8_t>().swap(g_snap_ring);
    }
    LOG_INFO("[SNAPSHOT] disabled for this game: the program at 0x{:08X} (flags 0x{:X}) writes memory from the GPU, so a copy of a vertex stream taken on the guest thread could miss what the GPU writes into it later", program_addr, program_flags);
}

void set_vertex_stream(State &state, Context *ctx, const std::size_t index, const std::size_t data_len, const Ptr<const void> stream) {
    renderer::add_state_set_command(ctx, renderer::GXMState::VertexStream, stream, index, data_len, static_cast<uint64_t>(~0ull));
}

void draw(State &state, Context *ctx, SceGxmPrimitiveType prim_type, SceGxmIndexFormat index_type, Ptr<const void> index_data, const std::uint32_t index_count, const std::uint32_t instance_count) {
    renderer::add_command(ctx, renderer::CommandOpcode::Draw, nullptr, prim_type, index_type, index_data, index_count, instance_count);
}

void transfer_copy(State &state, uint32_t colorKeyValue, uint32_t colorKeyMask, SceGxmTransferColorKeyMode colorKeyMode, const SceGxmTransferImage *images, SceGxmTransferType srcType, SceGxmTransferType destType) {
    renderer::send_single_command(state, nullptr, renderer::CommandOpcode::TransferCopy, false, colorKeyValue, colorKeyMask, colorKeyMode, images, srcType, destType);
}

void transfer_downscale(State &state, const SceGxmTransferImage *src, const SceGxmTransferImage *dest) {
    renderer::send_single_command(state, nullptr, renderer::CommandOpcode::TransferDownscale, false, src, dest);
}

void transfer_fill(State &state, uint32_t fillColor, const SceGxmTransferImage *dest) {
    renderer::send_single_command(state, nullptr, renderer::CommandOpcode::TransferFill, false, fillColor, dest);
}

void sync_surface_data(State &state, Context *ctx, const SceGxmNotification vertex_notification, const SceGxmNotification fragment_notification) {
    renderer::add_command(ctx, renderer::CommandOpcode::SyncSurfaceData, nullptr, vertex_notification, fragment_notification);
}

int sync_guest_range(State &state, Address address, uint32_t size) {
    return renderer::send_single_command(state, nullptr, renderer::CommandOpcode::SyncGuestRange, true, address, size);
}

bool create_context(State &state, std::unique_ptr<Context> &context) {
    return renderer::send_single_command(state, nullptr, renderer::CommandOpcode::CreateContext, true, &context) > CommandErrorCodeNone;
}

void destroy_context(State &state, std::unique_ptr<Context> &context) {
    renderer::send_single_command(state, nullptr, renderer::CommandOpcode::DestroyContext, true, &context);
}

void destroy_context_during_shutdown(State &state, std::unique_ptr<Context> &context) {
    assert(!state.render_thread);

    if (state.current_backend == Backend::OpenGL) {
        state.set_current();
    }

    if (state.context == context.get()) {
        state.context = nullptr;
    }

    context.reset();
}

bool create_render_target(State &state, std::unique_ptr<RenderTarget> &rt, const SceGxmRenderTargetParams *params) {
    return renderer::send_single_command(state, nullptr, renderer::CommandOpcode::CreateRenderTarget, true, &rt, params) > CommandErrorCodeNone;
}

void destroy_render_target(State &state, std::unique_ptr<RenderTarget> &rt) {
    renderer::send_single_command(state, nullptr, renderer::CommandOpcode::DestroyRenderTarget, true, &rt);
}

void destroy_render_target_during_shutdown(State &state, std::unique_ptr<RenderTarget> &rt) {
    assert(!state.render_thread);
    if (!rt)
        return;

    switch (state.current_backend) {
    case Backend::OpenGL:
        state.set_current();
        break;

    case Backend::Vulkan:
        vulkan::destroy(dynamic_cast<vulkan::VKState &>(state), rt);
        break;
    }

    rt.reset();
}

void set_uniform_buffer(State &state, Context *ctx, const bool is_vertex_uniform, const int block_number, const std::uint16_t block_size, const Ptr<const void> buffer) {
    // Calculate the number of bytes
    std::uint32_t bytes_to_copy_and_pad = ((block_size + 15) / 16) * 16;

    renderer::add_state_set_command(ctx, renderer::GXMState::UniformBuffer, buffer, is_vertex_uniform, block_number, bytes_to_copy_and_pad);
}

void set_visibility_buffer(State &state, Context *ctx, Ptr<uint32_t> visibility_address, uint32_t visibility_stride) {
    renderer::add_state_set_command(ctx, renderer::GXMState::VisibilityBuffer, visibility_address, visibility_stride);
}

void set_visibility_index(State &state, Context *ctx, bool enable, uint32_t index, bool is_increment) {
    renderer::add_state_set_command(ctx, renderer::GXMState::VisibilityIndex, index, enable, is_increment);
}

} // namespace renderer
