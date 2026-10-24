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

#include <renderer/vulkan/pipeline_cache.h>

#include <renderer/functions.h>
#include <renderer/vulkan/functions.h>
#include <renderer/vulkan/gxm_to_vulkan.h>
#include <renderer/vulkan/state.h>
#include <renderer/vulkan/types.h>

#include <gxm/functions.h>
#include <gxm/types.h>
#include <renderer/shaders.h>
#include <shader/spirv_recompiler.h>

#include <util/fs.h>
#include <util/hash.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>

#include <SDL3/SDL_cpuinfo.h>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// don't use the dispatch version, because we always hash a small amount
// with a known size
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace renderer::vulkan {

static constexpr bool probe_before_defer = true;
static constexpr bool warm_at_program_creation = true;
static constexpr bool sync_new_surface_draws = false; // the surface was not drawn to in the last 60 frames (off: the main scene counts as new after a loading screen so a level's first frame froze for seconds on a cold cache)
static constexpr bool sync_depth_stencil_writers = false; // the draw writes depth or stencil (off: nearly every new pipeline of a 3D game does so async would almost never defer)
static constexpr bool save_in_background = true;

namespace {
uint64_t now_s() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t warm_pair_key(const Sha256Hash &vertex, const Sha256Hash &fragment, SceGxmOutputRegisterFormat output_format) {
    std::array<uint8_t, 68> data;
    memcpy(data.data(), vertex.data(), 32);
    memcpy(data.data() + 32, fragment.data(), 32);
    const uint32_t format = static_cast<uint32_t>(output_format);
    memcpy(data.data() + 64, &format, sizeof(format));
    return XXH3_64bits(data.data(), data.size());
}

// a skipped draw that writes depth or stencil changes what the later draws of the frame show
bool writes_depth_or_stencil(const GxmRecordState &record) {
    const bool two_sided = record.two_sided == SCE_GXM_TWO_SIDED_ENABLED;
    const auto stencil_writes = [](const GxmStencilStateOp &op) {
        return op.stencil_fail != SCE_GXM_STENCIL_OP_KEEP || op.depth_fail != SCE_GXM_STENCIL_OP_KEEP || op.depth_pass != SCE_GXM_STENCIL_OP_KEEP;
    };
    return record.front_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED || (two_sided && record.back_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED)
        || stencil_writes(record.front_stencil_state_op) || (two_sided && stencil_writes(record.back_stencil_state_op));
}
} // namespace

// Size of the record containing what is needed for the pipeline construction (what is after is dynamic state)
constexpr size_t record_pipeline_len = offsetof(GxmRecordState, vertex_streams);

static uint64_t hash_pipeline_record(const GxmRecordState &record) {
    alignas(8) uint8_t block[record_pipeline_len];
    memcpy(block, &record, record_pipeline_len);
    const auto zero = [&](const auto &field) {
        const size_t off = reinterpret_cast<const uint8_t *>(&field) - reinterpret_cast<const uint8_t *>(&record);
        memset(block + off, 0, sizeof(field));
    };
    zero(record.region_clip_mode);
    zero(record.back_polygon_mode);
    zero(record.back_depth_func);
    zero(record.back_depth_write_mode);
    zero(record.back_side_fragment_program_mode);
    if (record.two_sided != SCE_GXM_TWO_SIDED_ENABLED)
        zero(record.back_stencil_state_op);
    return XXH3_64bits(block, record_pipeline_len);
}

// structure containing everything needed to compile a pipeline
struct CompileRequest {
    // iterator to the pipeline location
    vk::Pipeline *pipeline;

    // this is everything we need to compile the shader on another thread (as the original data will change)
    SceGxmPrimitiveType type;
    vk::RenderPass render_pass;
    std::shared_ptr<ProgramBinding> vertex_program_binding;
    std::shared_ptr<ProgramBinding> fragment_program_binding;
    bool has_color_surface_data;
    shader::Hints hints;
    bool has_casts;

    // the content of the record useful for the pipeline creation
    alignas(8) uint8_t record_data[record_pipeline_len];

    const GxmRecordState *get_record() {
        // note: this object is only half defined, but we are only looking at the part that's defined
        return reinterpret_cast<const GxmRecordState *>(record_data);
    }
};

PipelineCache::PipelineCache(VKState &state)
    : state(state)
    , pipeline_compile_queue_token(pipeline_compile_queue) {
}

void PipelineCache::init(bool support_rasterized_order_access) {
    vk::PipelineCacheCreateInfo pipeline_info{};
    pipeline_cache = state.device.createPipelineCache(pipeline_info);

    if (state.features.vertex_store_slots > 0) {
        const shader::usse::SpirvCode code = shader::generate_vertex_store_fragment_shader(state.features.vertex_store_slots);
        vertex_store_fragment_module = state.device.createShaderModule(vk::ShaderModuleCreateInfo{ .codeSize = code.size() * sizeof(uint32_t), .pCode = code.data() });
    }

    // the layout for uniforms buffer can be made here as it will always be the same
    {
        std::array<vk::DescriptorSetLayoutBinding, 4> layout_bindings;
        // Our vertex uniform (GXMRenderVertUniformBlock)
        layout_bindings[0] = vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eVertex,
        };
        // Our fragment uniform (GXMRenderFragUniformBlock)
        layout_bindings[1] = vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        };
        // GXM vertex uniform (if no memory mapping)
        layout_bindings[2] = vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eStorageBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eVertex,
        };
        // GXM Fragment uniform (if no memory mapping)
        layout_bindings[3] = vk::DescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = vk::DescriptorType::eStorageBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        };

        vk::DescriptorSetLayoutCreateInfo descriptor_info{
            .bindingCount = state.features.enable_memory_mapping ? 2U : 4U,
            .pBindings = layout_bindings.data()
        };
        uniforms_layout = state.device.createDescriptorSetLayout(descriptor_info);
    }

    {
        // layout for the mask, color attachment as input, being an input attachment or a storage image
        // depending on whether or not we are using shader interlock
        std::array<vk::DescriptorSetLayoutBinding, 3> layout_binding;
        const vk::DescriptorType intput_image_descriptor = state.features.support_shader_interlock
            ? vk::DescriptorType::eStorageImage
            : vk::DescriptorType::eInputAttachment;
        uint32_t binding_count = 0;
        layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = intput_image_descriptor,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment
        };
        if (state.features.use_mask_bit)
            layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment
            };
        if (state.features.preserve_f16_nan_as_u16)
            // raw u16x4 alias of the color attachment (set 1, binding 2 in the shaders)
            layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
                .binding = 2,
                .descriptorType = intput_image_descriptor,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment
            };

        vk::DescriptorSetLayoutCreateInfo descriptor_info{
            .bindingCount = binding_count,
            .pBindings = layout_binding.data()
        };
        attachments_layout = state.device.createDescriptorSetLayout(descriptor_info);
    }

    {
        // texture layout

        // empty descriptor
        {
            vk::DescriptorSetLayoutCreateInfo empty_info{};
            vertex_textures_layout[0] = state.device.createDescriptorSetLayout(empty_info);
            fragment_textures_layout[0] = vertex_textures_layout[0];
        }

        // first vertex
        std::array<vk::DescriptorSetLayoutBinding, 16> layout_bindings;
        for (uint32_t i = 0; i < 16; i++) {
            layout_bindings[i] = {
                .binding = i,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eVertex
            };
        }
        for (uint32_t i = 1; i <= 16; i++) {
            vk::DescriptorSetLayoutCreateInfo descriptor_info{
                .bindingCount = i,
                .pBindings = layout_bindings.data()
            };
            vertex_textures_layout[i] = state.device.createDescriptorSetLayout(descriptor_info);
        }

        // then fragment
        for (uint32_t i = 0; i < 16; i++) {
            layout_bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
        }
        for (uint32_t i = 1; i <= 16; i++) {
            vk::DescriptorSetLayoutCreateInfo descriptor_info{
                .bindingCount = i,
                .pBindings = layout_bindings.data()
            };
            fragment_textures_layout[i] = state.device.createDescriptorSetLayout(descriptor_info);
        }
    }

    // compute all possible pipeline layouts
    for (uint32_t vert_texture_count = 0; vert_texture_count <= 16; vert_texture_count++) {
        for (uint32_t frag_texture_count = 0; frag_texture_count <= 16; frag_texture_count++) {
            vk::PipelineLayoutCreateInfo layout_info{};
            vk::DescriptorSetLayout set_layouts[] = { uniforms_layout, attachments_layout, vertex_textures_layout[vert_texture_count], fragment_textures_layout[frag_texture_count] };
            layout_info.setSetLayouts(set_layouts);
            pipeline_layouts[vert_texture_count][frag_texture_count] = state.device.createPipelineLayout(layout_info);
        }
    }

    {
        // look for rgb vertex attribute support
        // we need to look at each format because it is not the same for all usual 3-component formats (checked on AMD Radeon HD 7800)
        // no need to test for 32-bit types, they are always supported
        vk::Format formats[] = {
            vk::Format::eR16G16B16Unorm, vk::Format::eR16G16B16Snorm,
            vk::Format::eR16G16B16Uscaled, vk::Format::eR16G16B16Sscaled,
            vk::Format::eR16G16B16Uint, vk::Format::eR16G16B16Sint,
            vk::Format::eR16G16B16Sfloat,
            vk::Format::eR8G8B8Unorm, vk::Format::eR8G8B8Snorm,
            vk::Format::eR8G8B8Uscaled, vk::Format::eR8G8B8Sscaled,
            vk::Format::eR8G8B8Uint, vk::Format::eR8G8B8Sint
        };
        for (auto fmt : formats) {
            vk::FormatProperties rgb_property = state.physical_device.getFormatProperties(fmt);
            if (!(rgb_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer)) {
                unsupported_rgb_vertex_attribute_formats.emplace(fmt);
            }
        }

        // same for scaled formats
        const vk::FormatProperties scaled_property = state.physical_device.getFormatProperties(vk::Format::eR8G8B8A8Uscaled);
        support_scaled_vertex_attribute = static_cast<bool>(scaled_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer);
        state.features.support_scaled_attribute_formats = support_scaled_vertex_attribute;

        // handle interactions between these 2 properties, the scaled support will be applied before the 3-component support
        if (!support_scaled_vertex_attribute) {
            vk::Format scaled_fmt[] = { vk::Format::eR16G16B16Uscaled, vk::Format::eR16G16B16Sscaled, vk::Format::eR8G8B8Uscaled, vk::Format::eR8G8B8Sscaled };
            for (auto fmt : scaled_fmt)
                unsupported_rgb_vertex_attribute_formats.erase(fmt);
        }

        state.features.support_rgb_attributes = unsupported_rgb_vertex_attribute_formats.empty();
    }

    support_coherent_framebuffer_fetch = support_rasterized_order_access;

    const int nb_logical_threads = SDL_GetNumLogicalCPUCores();
    // took this from RPCS3 (slightly modified)
    if (nb_logical_threads > 12)
        nb_worker_threads = 6;
    else if (nb_logical_threads > 8)
        nb_worker_threads = 4;
    else if (nb_logical_threads >= 8)
        nb_worker_threads = 3;
    else if (nb_logical_threads >= 6)
        nb_worker_threads = 2;
    else
        nb_worker_threads = 1;

    if (use_async_compilation) {
        // we could not initialize the worker threads previously
        use_async_compilation = false;
        set_async_compilation(true);
    }

    support_cache_only_creation = state.support_pipeline_cache_control;
    if (warm_at_program_creation) {
        warm_render_pass = retrieve_render_pass(vk::Format::eR8G8B8A8Unorm, true, true, true, false);
        if (state.features.support_shader_interlock)
            warm_interlock_render_pass = retrieve_render_pass(vk::Format::eR8G8B8A8Unorm, true, true, true, false, true);
        const int warm_thread_count = std::clamp(nb_worker_threads, 1, 2);
        warm_threads.reserve(warm_thread_count);
        for (int i = 0; i < warm_thread_count; i++)
            warm_threads.emplace_back(&PipelineCache::warm_thread, this, std::ref(*state.mem));
        warm_enabled = true;
    }
}

void PipelineCache::set_async_compilation(bool enable) {
    if (enable == use_async_compilation)
        return;

    use_async_compilation = enable;
    if (nb_worker_threads == 0)
        // not ingame yet
        return;

    if (enable) {
        LOG_INFO("Enabling asynchronous pipeline compilation with {} threads", nb_worker_threads);
        worker_threads.reserve(nb_worker_threads);
        for (int i = 0; i < nb_worker_threads; i++) {
            worker_threads.emplace_back(&PipelineCache::compiler_thread, this, std::ref(*state.mem));
        }
    } else {
        LOG_INFO("Asynchronous pipeline compilation is now disabled");
        stop_compile_workers(true);
    }
}

void PipelineCache::stop_compile_workers(bool drain) {
    if (drain) {
        drain_compile_workers = true;
    } else {
        for (size_t i = 0; i < worker_threads.size(); i++)
            // if a thread receives nullptr, it exits
            pipeline_compile_queue.enqueue(nullptr);
    }

    for (auto &thread : worker_threads) {
        if (thread.joinable())
            thread.join();
    }
    worker_threads.clear();
    drain_compile_workers = false;
}

// magic number put at the beginning of the pipeline cache file
constexpr uint32_t pipeline_cache_magic = 0xBEEF4321;

void PipelineCache::read_pipeline_cache() {
    const std::string pipeline_cache_name = fmt::format("pipeline-cache-vk{}.dat", shader::CURRENT_VERSION);
    const fs::path path = state.shaders_path / pipeline_cache_name;

    fs::ifstream pipeline_cache_file(path, std::ios::in | std::ios::binary);
    if (!pipeline_cache_file.is_open())
        return;

    LOG_INFO("Found pipeline cache, reading...");

    pipeline_cache_file.seekg(0, fs::ifstream::end);
    size_t pipeline_size = pipeline_cache_file.tellg();
    pipeline_cache_file.seekg(0);

    if (pipeline_size < sizeof(uint32_t) + sizeof(size_t))
        return;

    // read the hashes
    auto read_integer = [&]<typename T>(T &val) {
        pipeline_cache_file.read(reinterpret_cast<char *>(&val), sizeof(T));
    };
    uint32_t magic_number;
    read_integer(magic_number);
    size_t nb_hashes;
    read_integer(nb_hashes);
    // safety check
    size_t hashes_size = sizeof(magic_number) + sizeof(nb_hashes) + nb_hashes * sizeof(uint64_t);
    if (magic_number != pipeline_cache_magic || nb_hashes > pipeline_size / sizeof(uint64_t) || pipeline_size < hashes_size) {
        LOG_WARN("Pipeline cache is corrupted, ignoring it.");
        pipeline_cache_file.close();
        return;
    }
    pipeline_size -= hashes_size;

    std::vector<uint64_t> hashes(nb_hashes);
    for (uint64_t &hash : hashes)
        read_integer(hash);

    std::vector<char> pipeline_data(pipeline_size);
    pipeline_cache_file.read(pipeline_data.data(), pipeline_size);
    pipeline_cache_file.close();

    VkPipelineCacheHeaderVersionOne header{};
    if (pipeline_size >= sizeof(header))
        std::memcpy(&header, pipeline_data.data(), sizeof(header));
    const vk::PhysicalDeviceProperties &props = state.physical_device_properties;
    if (pipeline_size < sizeof(header) || header.headerSize < sizeof(header) || header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        || header.vendorID != props.vendorID || header.deviceID != props.deviceID
        || std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID.data(), VK_UUID_SIZE) != 0) {
        LOG_WARN("Pipeline cache was written for another GPU or driver, ignoring it");
        return;
    }

    // insert hashes with null pipeline
    for (const uint64_t hash : hashes)
        pipelines[hash] = nullptr;

    vk::PipelineCacheCreateInfo cache_info{
        .initialDataSize = pipeline_size,
        .pInitialData = pipeline_data.data()
    };

    state.device.destroyPipelineCache(pipeline_cache);
    pipeline_cache = state.device.createPipelineCache(cache_info);
    LOG_INFO("Pipeline cache read and loaded");
}

void PipelineCache::save_pipeline_cache() {
    wait_for_background_save();
    std::vector<uint64_t> keys;
    keys.reserve(pipelines.size());
    for (const auto &[hash, _] : pipelines)
        keys.push_back(hash);
    write_pipeline_cache(keys);
}

void PipelineCache::save_pipeline_cache_async() {
    if (!save_in_background) {
        save_pipeline_cache();
        return;
    }
    if (save_running) {
        // the previous save is still writing, try again shortly
        next_pipeline_cache_save = now_s() + 2;
        return;
    }
    wait_for_background_save();
    std::vector<uint64_t> keys;
    keys.reserve(pipelines.size());
    for (const auto &[hash, _] : pipelines)
        keys.push_back(hash);
    save_running = true;
    save_thread = std::thread([this, keys = std::move(keys)]() {
        try {
            write_pipeline_cache(keys);
        } catch (const std::exception &error) {
            LOG_ERROR("Could not save the pipeline cache: {}", error.what());
        }
        save_running = false;
    });
}

void PipelineCache::wait_for_background_save() {
    if (save_thread.joinable())
        save_thread.join();
}

void PipelineCache::write_pipeline_cache(const std::vector<uint64_t> &keys) {
    // first save the shader hashes
    // do a copy for thread safety
    std::vector<ShadersHash> shader_cache_copy;
    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        shader_cache_copy = state.shaders_cache_hashs;
    }
    renderer::save_shaders_cache_hashs(state, shader_cache_copy);

    const std::vector<uint8_t> pipeline_data = state.device.getPipelineCacheData(pipeline_cache);
    if (pipeline_data.empty())
        // No pipeline was created
        return;

    const std::string pipeline_cache_name = fmt::format("pipeline-cache-vk{}.dat", shader::CURRENT_VERSION);
    const fs::path path = state.shaders_path / pipeline_cache_name;

    fs::ofstream pipeline_cache_file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!pipeline_cache_file.is_open())
        return;

    LOG_INFO("Saving pipeline cache...");

    // first save the hashes of all pipelines
    auto write_integer = [&]<typename T>(T val) {
        pipeline_cache_file.write(reinterpret_cast<const char *>(&val), sizeof(T));
    };
    write_integer(pipeline_cache_magic);
    write_integer(keys.size());
    for (const uint64_t hash : keys) {
        write_integer(hash);
    }

    // then save the cache
    pipeline_cache_file.write(reinterpret_cast<const char *>(pipeline_data.data()), pipeline_data.size());
    pipeline_cache_file.close();
    LOG_INFO("Pipeline cache saved");
}

void PipelineCache::cleanup() {
    stop_warm_threads();
    wait_for_background_save();
    warm_render_pass = nullptr;
    warm_interlock_render_pass = nullptr;
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        warm_pairs_requested.clear();
        pairs_drawn.clear();
        warm_states.clear();
    }
    draw_states.clear();
    draw_states_total = 0;

    // stop threads (queued requests are abandoned at shutdown)
    if (use_async_compilation) {
        use_async_compilation = false;
        stop_compile_workers(false);
    }

    for (auto &[hash, pipeline] : pipelines)
        state.device.destroy(pipeline);
    pipelines.clear();

    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        for (auto &[hash, shader] : shaders)
            state.device.destroy(shader);
        shaders.clear();
    }

    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 2; k++)
                for (int l = 0; l < 2; l++) {
                    for (auto &[fmt, pass] : render_passes[i][j][k][l])
                        state.device.destroy(pass);
                    render_passes[i][j][k][l].clear();
                }

    for (auto &[fmt, pass] : shader_interlock_pass)
        state.device.destroy(pass);
    shader_interlock_pass.clear();

    for (int i = 0; i < 17; i++)
        for (int j = 0; j < 17; j++) {
            state.device.destroy(pipeline_layouts[i][j]);
            pipeline_layouts[i][j] = nullptr;
        }

    state.device.destroy(vertex_store_fragment_module);
    vertex_store_fragment_module = nullptr;
    state.device.destroy(uniforms_layout);
    uniforms_layout = nullptr;
    state.device.destroy(attachments_layout);
    attachments_layout = nullptr;

    state.device.destroy(vertex_textures_layout[0]);
    vertex_textures_layout[0] = nullptr;
    fragment_textures_layout[0] = nullptr;

    for (int i = 1; i <= 16; i++) {
        state.device.destroy(vertex_textures_layout[i]);
        vertex_textures_layout[i] = nullptr;
        state.device.destroy(fragment_textures_layout[i]);
        fragment_textures_layout[i] = nullptr;
    }

    state.device.destroy(pipeline_cache);
    pipeline_cache = nullptr;

    next_pipeline_cache_save = std::numeric_limits<uint64_t>::max();
    nb_worker_threads = 0;
}

// Vulkan structs used to specify the fragment specialization constants
// Also, booleans in SPIRV are 32bit wide
static const vk::SpecializationMapEntry frag_spec_entries[] = {
    { .constantID = shader::GAMMA_CORRECTION_SPECIALIZATION_ID, .offset = 0, .size = sizeof(uint32_t) },
    { .constantID = shader::SURFACE_CAST_SPECIALIZATION_ID, .offset = sizeof(uint32_t), .size = sizeof(uint32_t) },
};

// indexed by (is_srgb << 1) | has_casts
static const uint32_t frag_spec_data[4][2] = {
    { vk::False, vk::False },
    { vk::False, vk::True },
    { vk::True, vk::False },
    { vk::True, vk::True },
};

static const vk::SpecializationInfo frag_spec_infos[4] = {
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[0]), .pData = frag_spec_data[0] },
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[1]), .pData = frag_spec_data[1] },
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[2]), .pData = frag_spec_data[2] },
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[3]), .pData = frag_spec_data[3] },
};

static Sha256Hash shader_cache_key(const SceGxmProgram *program, const Sha256Hash &base_hash, bool is_vertex, const shader::Hints &hints, const FeatureState &features, std::string &version_suffix) {
    // A one-channel surface moves a different component into the stored channel, so it needs its own cache entry
    const int one_channel_source = is_vertex ? -1 : gxm::one_channel_source_component(hints.color_format);
    Sha256Hash hash = base_hash;
    if (one_channel_source > 0) {
        hash[0] ^= static_cast<uint8_t>(0xC0 + one_channel_source);
        version_suffix = fmt::format("c{}", one_channel_source);
    }
    if (!is_vertex && program->is_native_color() && hints.output_register_format != SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED) {
        hash[1] ^= static_cast<uint8_t>(0x80 + hints.output_register_format);
        version_suffix += fmt::format("r{}", static_cast<int>(hints.output_register_format));
    }
    const SceGxmColorBaseFormat surface_base = gxm::get_base_format(hints.color_format);
    if (!is_vertex && (program->is_frag_color_used() || surface_base == SCE_GXM_COLOR_BASE_FORMAT_F32F32)) {
        for (int i = 0; i < 4; i++)
            hash[2 + i] ^= static_cast<uint8_t>(static_cast<uint32_t>(surface_base) >> (8 * i));
        version_suffix += fmt::format("s{:X}", static_cast<uint32_t>(surface_base));
    }
    if (!is_vertex && hints.raw_color_attachment_input) {
        hash[6] ^= 0x75;
        version_suffix += "u";
    }
    if (is_vertex && shader::emulates_vertex_stores(*program, features)) {
        hash[7] ^= hints.vertex_store_pass ? 0x5E : 0x4E;
        version_suffix += hints.vertex_store_pass ? fmt::format("w{}", features.vertex_store_slots) : std::string("n");
    }
    return hash;
}

vk::PipelineShaderStageCreateInfo PipelineCache::retrieve_shader(const SceGxmProgram *program, const Sha256Hash &base_hash, bool is_vertex, bool maskupdate, MemState &mem, const shader::Hints &hints, bool is_srgb, bool has_casts, bool ready_only) {
    if (maskupdate)
        LOG_WARN_ONCE("Mask not implemented in the vulkan renderer!");

    std::string version_suffix;
    const Sha256Hash hash = shader_cache_key(program, base_hash, is_vertex, hints, state.features, version_suffix);

    const vk::ShaderModule shader_compiling = std::bit_cast<vk::ShaderModule>(~0ULL);

    // entries for constants a module does not declare are ignored, and is_srgb is only declared along shader interlock
    const bool srgb = is_srgb && state.features.should_use_shader_interlock() && program->is_frag_color_used();
    const vk::SpecializationInfo *spec_info = is_vertex ? nullptr : &frag_spec_infos[(srgb ? 2 : 0) | (has_casts ? 1 : 0)];

    if (ready_only) {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        const auto found = shaders.find(hash);
        if (found == shaders.end() || found->second == nullptr || found->second == shader_compiling)
            return {};
        return vk::PipelineShaderStageCreateInfo{
            .stage = is_vertex ? vk::ShaderStageFlagBits::eVertex : vk::ShaderStageFlagBits::eFragment,
            .module = found->second,
            .pName = is_vertex ? "main_vs" : "main_fs",
            .pSpecializationInfo = spec_info,
        };
    }

    vk::ShaderModule *shader_module;
    {
        // look if it is in the cache
        std::unique_lock<std::mutex> lock(shaders_mutex);
        shader_module = &shaders.insert({ hash, nullptr }).first->second;
        if (*shader_module == shader_compiling) {
            // another thread is compiling the same exact shader at the same time
            // it's no use re-compiling it, so just wait for the other thread being done
            lock.unlock();

            // we shouldn't need atomics and the compiler shouldn't be able to optimize this
            while (*shader_module == shader_compiling)
                std::this_thread::yield();
        }

        if (*shader_module == nullptr)
            // now mark the shader as compiling so that other threads accessing it won't try to compile it a second time
            *shader_module = shader_compiling;
    }

    if (*shader_module == shader_compiling) {
        precompile_shader(hash, false);
    }

    if (*shader_module != shader_compiling) {
        vk::PipelineShaderStageCreateInfo shader_stage_info{
            .stage = is_vertex ? vk::ShaderStageFlagBits::eVertex : vk::ShaderStageFlagBits::eFragment,
            .module = *shader_module,
            .pName = is_vertex ? "main_vs" : "main_fs",
            .pSpecializationInfo = spec_info,
        };
        return shader_stage_info;
    }

    const std::string hash_text = hex_string(hash);

    LOG_DEBUG("Generating vulkan spv shader {}", hash_text);
    const std::string shader_version = fmt::format("vk{}{}", shader::CURRENT_VERSION, version_suffix);

    // anything thrown here has to hand the slot back first or every thread waiting on this shader spins forever
    try {
        shader::usse::SpirvCode source = load_spirv_shader(*program, state.features, true, hints, maskupdate, state.shaders_path, state.shaders_log_path, shader_version, true);
        if (source.empty())
            throw std::runtime_error("shader translation produced no SPIR-V");

        vk::ShaderModuleCreateInfo shader_info{
            .codeSize = sizeof(uint32_t) * source.size(),
            .pCode = source.data()
        };

        *shader_module = state.device.createShaderModule(shader_info);
    } catch (...) {
        {
            std::lock_guard<std::mutex> guard(shaders_mutex);
            *shader_module = nullptr;
        }
        throw;
    }
    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        // Save shader cache hashes
        // vertex and fragment shaders are not linked together so no need to associate them
        Sha256Hash empty_hash{};
        if (is_vertex) {
            state.shaders_cache_hashs.push_back({ hash, empty_hash });
        } else {
            state.shaders_cache_hashs.push_back({ empty_hash, hash });
        }
    }

    vk::PipelineShaderStageCreateInfo shader_stage_info{
        .stage = is_vertex ? vk::ShaderStageFlagBits::eVertex : vk::ShaderStageFlagBits::eFragment,
        .module = *shader_module,
        .pName = is_vertex ? "main_vs" : "main_fs",
        .pSpecializationInfo = spec_info,
    };

    return shader_stage_info;
}

vk::RenderPass PipelineCache::retrieve_render_pass(vk::Format format, bool depth_load, bool stencil_load, bool force_store, bool is_color_transient, bool no_color, bool has_raw_attachment) {
    const bool with_raw_attachment = has_raw_attachment && !no_color && !is_color_transient && state.features.preserve_f16_nan_as_u16 && format == vk::Format::eR16G16B16A16Sfloat;

    auto &render_passes_map = no_color
        ? shader_interlock_pass
        : (with_raw_attachment
                  ? render_passes_with_raw[depth_load][stencil_load][force_store]
                  : render_passes[is_color_transient][depth_load][stencil_load][force_store]);

    auto it = render_passes_map.find(format);

    if (it != render_passes_map.end())
        return it->second;

    // create a new render pass for this format

    vk::AttachmentReference color_refs[2] = {
        { .attachment = 0, .layout = vk::ImageLayout::eGeneral },
        { .attachment = 1, .layout = vk::ImageLayout::eGeneral }
    };
    vk::AttachmentReference ds_ref{
        .attachment = no_color ? 0U : (with_raw_attachment ? 2U : 1U),
        .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal
    };
    vk::SubpassDescription subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics
    };

    subpass.setPDepthStencilAttachment(&ds_ref);
    if (!no_color) {
        if (support_coherent_framebuffer_fetch)
            subpass.flags = vk::SubpassDescriptionFlagBits::eRasterizationOrderAttachmentColorAccessEXT;

        subpass.colorAttachmentCount = with_raw_attachment ? 2 : 1;
        subpass.pColorAttachments = color_refs;
        subpass.inputAttachmentCount = (with_raw_attachment && state.features.direct_fragcolor) ? 2 : 1;
        subpass.pInputAttachments = color_refs;
    }

    vk::AttachmentDescription color_attachment{
        .format = format,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = is_color_transient ? vk::AttachmentLoadOp::eDontCare : vk::AttachmentLoadOp::eLoad,
        .storeOp = is_color_transient ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
        .initialLayout = is_color_transient ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
        .finalLayout = vk::ImageLayout::eGeneral
    };

    vk::AttachmentDescription raw_attachment{
        .format = vk::Format::eR16G16B16A16Uint,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .initialLayout = vk::ImageLayout::eGeneral,
        .finalLayout = vk::ImageLayout::eGeneral
    };

    vk::AttachmentStoreOp store_op = force_store ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare;
    vk::AttachmentDescription ds_attachment{
        .format = state.deep_stencil_use,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = depth_load ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
        .storeOp = store_op,
        .stencilLoadOp = stencil_load ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
        .stencilStoreOp = store_op,
        // eUndefined would allow discarding a loaded aspect, so use it only when neither aspect is loaded
        .initialLayout = (depth_load || stencil_load) ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal
    };

    std::array<vk::SubpassDependency, 4> dependencies;

    // external dependency
    // we want the previous render pass to be done when we reach the fragment stage / stencil*depth testing
    dependencies[0] = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
        .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite
            | vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eInputAttachmentRead
    };

    // BUG: This is dead code (as immediately overwritten below)
    if (state.features.support_shader_interlock && no_color) {
        // we must wait for the previous shaders to be done
        dependencies[1].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
        dependencies[1].dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    }

    // if an attachment is sampled from, we want it to be done before the next render pass fragment shader
    dependencies[1] = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eFragmentShader,
        .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits::eShaderRead,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite
    };

    if (state.features.support_shader_interlock && !no_color) {
        // we must wait for the shader interlock shader to be done
        dependencies[1].srcAccessMask |= vk::AccessFlagBits::eShaderWrite;
    }

    // self-dependency
    // this allows us to use a pipeline barrier in the render pass for programmable blending
    dependencies[2] = {
        .srcSubpass = 0,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eInputAttachmentRead,
        .dependencyFlags = vk::DependencyFlagBits::eByRegion
    };

    // mid-scene flush
    // unity games use it to write to a buffer in a vertex shader then use it as the vertex input in the next draw
    dependencies[3] = {
        .srcSubpass = 0,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eVertexShader,
        .dstStageMask = vk::PipelineStageFlagBits::eVertexInput,
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eVertexAttributeRead
    };

    vk::RenderPassCreateInfo pass_info{};
    vk::AttachmentDescription attachments[] = { color_attachment, with_raw_attachment ? raw_attachment : ds_attachment, ds_attachment };
    pass_info.setAttachments(attachments);
    pass_info.attachmentCount = with_raw_attachment ? 3 : 2;
    pass_info.setSubpasses(subpass);
    pass_info.setDependencies(dependencies);
    if (no_color) {
        // only add the ds attachment
        pass_info.pAttachments = &attachments[2];
        pass_info.attachmentCount = 1;
        // no need for the self-dependency
        pass_info.setDependencyCount(2);
    }

    render_passes_map[format] = state.device.createRenderPass(pass_info);

    {
        // Remember exactly how this render pass was built. A pipeline is only ever rejected
        // *relative to* its subpass, so a failure report is close to useless without this.
        std::string desc = fmt::format(
            "format={} attachments={} no_color={} raw={} transient={} depth_load={} stencil_load={} force_store={}"
            " subpass_flags=0x{:X} color_att={} input_att={} ds_att={}",
            vk::to_string(format), pass_info.attachmentCount, no_color, with_raw_attachment, is_color_transient,
            depth_load, stencil_load, force_store, static_cast<uint32_t>(static_cast<VkSubpassDescriptionFlags>(subpass.flags)),
            subpass.colorAttachmentCount, subpass.inputAttachmentCount, ds_ref.attachment);
        std::lock_guard<std::mutex> guard(diagnostics_mutex);
        render_pass_descriptions[std::bit_cast<uint64_t>(static_cast<VkRenderPass>(render_passes_map[format]))] = std::move(desc);
    }

    return render_passes_map[format];
}

// --- SPIR-V introspection ---------------------------------------------------------------------
// Just enough of a walker to answer the questions a rejected pipeline raises: what capabilities
// does the module need, and what does its interface look like? A driver that refuses a module
// nearly always does so because of one of those two things.
static const char *spirv_capability_name(uint32_t cap) {
    switch (cap) {
    case 0: return "Matrix";
    case 1: return "Shader";
    case 9: return "Float16";
    case 10: return "Float64";
    case 11: return "Int64";
    case 22: return "Int16";
    case 25: return "ImageGatherExtended";
    case 32: return "ClipDistance";
    case 33: return "CullDistance";
    case 34: return "ImageCubeArray";
    case 35: return "SampleRateShading";
    case 39: return "Int8";
    case 40: return "InputAttachment";
    case 42: return "MinLod";
    case 43: return "Sampled1D";
    case 44: return "Image1D";
    case 45: return "SampledCubeArray";
    case 46: return "SampledBuffer";
    case 47: return "ImageBuffer";
    case 49: return "StorageImageExtendedFormats";
    case 50: return "ImageQuery";
    case 51: return "DerivativeControl";
    case 55: return "StorageImageReadWithoutFormat";
    case 56: return "StorageImageWriteWithoutFormat";
    case 61: return "GroupNonUniform";
    case 4427: return "DrawParameters";
    case 4433: return "StorageBuffer16BitAccess";
    case 4434: return "UniformAndStorageBuffer16BitAccess";
    case 4436: return "StorageInputOutput16";
    case 4448: return "StorageBuffer8BitAccess";
    case 4449: return "UniformAndStorageBuffer8BitAccess";
    case 5301: return "ShaderNonUniform";
    case 5302: return "RuntimeDescriptorArray";
    case 5345: return "VulkanMemoryModel";
    case 5347: return "PhysicalStorageBufferAddresses";
    case 5363: return "FragmentShaderSampleInterlockEXT";
    case 5372: return "FragmentShaderShadingRateInterlockEXT";
    case 5378: return "FragmentShaderPixelInterlockEXT";
    case 5379: return "DemoteToHelperInvocation";
    default: return nullptr;
    }
}

static std::string spirv_literal_string(const uint32_t *words, size_t max_words) {
    std::string out;
    for (size_t i = 0; i < max_words; i++) {
        for (int b = 0; b < 4; b++) {
            const char c = static_cast<char>((words[i] >> (8 * b)) & 0xFF);
            if (c == '\0')
                return out;
            out.push_back(c);
        }
    }
    return out;
}

static std::string describe_spirv_module(const uint32_t *code, size_t word_count) {
    if (word_count < 5 || code[0] != 0x07230203)
        return "<not a valid SPIR-V module>";

    std::vector<std::string> capabilities;
    std::vector<std::string> extensions;
    std::string entry_point;
    uint32_t interface_count = 0;
    std::map<uint32_t, uint32_t> var_storage_class; // variable id -> storage class
    std::map<uint32_t, uint32_t> var_location; // variable/target id -> Location decoration
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> var_set_binding; // id -> (set, binding)
    std::map<uint32_t, uint32_t> var_builtin;
    std::map<uint32_t, std::string> var_name;
    std::vector<uint32_t> spec_const_ids;

    size_t i = 5;
    while (i < word_count) {
        const uint32_t instr = code[i];
        const uint16_t op = instr & 0xFFFF;
        const uint16_t len = instr >> 16;
        if (len == 0 || i + len > word_count)
            break;
        const uint32_t *w = &code[i];

        switch (op) {
        case 17: // OpCapability
            if (len >= 2) {
                const char *name = spirv_capability_name(w[1]);
                capabilities.push_back(name ? name : fmt::format("Cap{}", w[1]));
            }
            break;
        case 10: // OpExtension
            if (len >= 2)
                extensions.push_back(spirv_literal_string(&w[1], len - 1));
            break;
        case 15: // OpEntryPoint: model, id, name..., interface ids
            if (len >= 4) {
                const std::string name = spirv_literal_string(&w[3], len - 3);
                const size_t name_words = (name.size() / 4) + 1;
                interface_count = static_cast<uint32_t>(len - 3 - name_words);
                entry_point = fmt::format("{} (model {}) interface_vars={}", name, w[1], interface_count);
            }
            break;
        case 5: // OpName
            if (len >= 3)
                var_name[w[1]] = spirv_literal_string(&w[2], len - 2);
            break;
        case 71: // OpDecorate: target, decoration, operands...
            if (len >= 3) {
                const uint32_t target = w[1];
                const uint32_t decoration = w[2];
                if (decoration == 30 && len >= 4) // Location
                    var_location[target] = w[3];
                else if (decoration == 11 && len >= 4) // BuiltIn
                    var_builtin[target] = w[3];
                else if (decoration == 34 && len >= 4) // DescriptorSet
                    var_set_binding[target].first = w[3];
                else if (decoration == 33 && len >= 4) // Binding
                    var_set_binding[target].second = w[3];
                else if (decoration == 1 && len >= 4) // SpecId
                    spec_const_ids.push_back(w[3]);
            }
            break;
        case 59: // OpVariable: result type, result id, storage class
            if (len >= 4)
                var_storage_class[w[2]] = w[3];
            break;
        default:
            break;
        }
        i += len;
    }

    auto list_vars = [&](uint32_t storage_class) {
        std::string out;
        for (const auto &[id, sc] : var_storage_class) {
            if (sc != storage_class)
                continue;
            const auto loc = var_location.find(id);
            const auto bi = var_builtin.find(id);
            if (loc == var_location.end() && bi == var_builtin.end())
                continue; // unlocated / unnamed globals are not interesting here
            const auto nm = var_name.find(id);
            if (!out.empty())
                out += ", ";
            if (bi != var_builtin.end())
                out += fmt::format("{}=builtin{}", nm != var_name.end() ? nm->second : fmt::format("%{}", id), bi->second);
            else
                out += fmt::format("{}@loc{}", nm != var_name.end() ? nm->second : fmt::format("%{}", id), loc->second);
        }
        return out.empty() ? std::string("none") : out;
    };

    std::string descriptors;
    for (const auto &[id, sb] : var_set_binding) {
        const auto nm = var_name.find(id);
        if (!descriptors.empty())
            descriptors += ", ";
        descriptors += fmt::format("{}=set{}.binding{}", nm != var_name.end() ? nm->second : fmt::format("%{}", id), sb.first, sb.second);
    }

    auto join = [](const std::vector<std::string> &items) {
        std::string out;
        for (const std::string &item : items)
            out += (out.empty() ? "" : ", ") + item;
        return out.empty() ? std::string("none") : out;
    };
    std::vector<std::string> spec_ids;
    for (uint32_t id : spec_const_ids)
        spec_ids.push_back(std::to_string(id));

    return fmt::format(
        "words={} entry=[{}]\n"
        "      capabilities: {}\n"
        "      extensions: {}\n"
        "      inputs: {}\n"
        "      outputs: {}\n"
        "      uniform_constants: {}\n"
        "      spec_constant_ids: {}",
        word_count, entry_point,
        join(capabilities),
        join(extensions),
        list_vars(1), // Input
        list_vars(3), // Output
        descriptors.empty() ? std::string("none") : descriptors,
        join(spec_ids));
}

std::string PipelineCache::describe_shader(const Sha256Hash &hash) {
    const std::string file_name = fmt::format("vk{}-{}.spv", shader::CURRENT_VERSION, hex_string(hash));
    const std::vector<uint32_t> source = renderer::pre_load_shader_spirv(state.shaders_path / file_name);
    if (source.empty())
        return fmt::format("<could not read {} back from the shader cache>", file_name);

    return describe_spirv_module(source.data(), source.size());
}

namespace {
struct AttributeFetch {
    vk::Format format;
    // TODO: add support for matrix input for non-regformated attributes
    uint32_t array_size = 1;
    uint32_t array_element_size = 0;
    uint32_t component_size = 0;
    uint32_t size = 0;
};

AttributeFetch attribute_fetch(const SceGxmVertexAttribute &attribute, shader::usse::AttributeInformation info, const bool support_scaled_vertex_attribute, const std::set<vk::Format> &unsupported_rgb_vertex_attribute_formats) {
    AttributeFetch fetch{};
    SceGxmAttributeFormat attribute_format = attribute.format;
    uint8_t component_count = attribute.componentCount;
    if (info.regformat) {
        component_count = info.component_count;
        switch (info.gxm_type) {
        case SCE_GXM_PARAMETER_TYPE_U8:
        case SCE_GXM_PARAMETER_TYPE_S8:
        case SCE_GXM_PARAMETER_TYPE_C10:
            attribute_format = SCE_GXM_ATTRIBUTE_FORMAT_U8;
            break;
        case SCE_GXM_PARAMETER_TYPE_U16:
        case SCE_GXM_PARAMETER_TYPE_S16:
        case SCE_GXM_PARAMETER_TYPE_F16:
            attribute_format = SCE_GXM_ATTRIBUTE_FORMAT_U16;
            break;
        default:
            // U32 format
            attribute_format = SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED;
            break;
        }

        if (info.gxm_type == SCE_GXM_PARAMETER_TYPE_C10)
            component_count = (component_count * 10 + 7) / 8;

        if (component_count > 4) {
            fetch.array_size = (component_count + 3) / 4;
            fetch.array_element_size = 4 * gxm::attribute_format_size(attribute_format);
            component_count = 4;
        }

        fetch.format = translate_attribute_format(attribute_format, component_count, true, false);
        if (component_count == 3 && unsupported_rgb_vertex_attribute_formats.contains(fetch.format)) {
            component_count = 4;
            fetch.format = translate_attribute_format(attribute_format, component_count, true, false);
        }
    } else {
        // some Android GPUs do not support scaled attributes (so do the conversion in the GPU instead)
        if (!support_scaled_vertex_attribute)
            info.is_integer = true;

        // some AMD GPUs do not seem to support rgb vertex attributes
        fetch.format = translate_attribute_format(attribute_format, component_count, info.is_integer, info.is_signed);
        if (component_count == 3 && unsupported_rgb_vertex_attribute_formats.contains(fetch.format)) {
            component_count = 4;
            fetch.format = translate_attribute_format(attribute_format, component_count, info.is_integer, info.is_signed);
        }
    }

    fetch.component_size = gxm::attribute_format_size(attribute_format);
    fetch.size = (fetch.array_size - 1) * fetch.array_element_size + component_count * fetch.component_size;
    return fetch;
}
} // namespace

vk::PipelineVertexInputStateCreateInfo PipelineCache::get_vertex_input_state(const ProgramBinding &vertex_program) {
    // pointer to these objects are returned (so it needs to be static)
    // and each thread needs one (hence the thread_local)
    static thread_local std::vector<vk::VertexInputBindingDescription> binding_descr;
    static thread_local std::vector<vk::VertexInputAttributeDescription> attr_descr;
    binding_descr.clear();
    attr_descr.clear();

    // Vertex attributes.
    VertexProgram *vkvert = vertex_program.vertex_program.get();

    uint32_t used_streams = 0;
    for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
        if (vkvert->attribute_infos.contains(attribute.regIndex))
            used_streams |= (1 << attribute.streamIndex);
    }
    std::array<VertexStreamLayout, SCE_GXM_MAX_VERTEX_STREAMS> layouts;
    for (uint32_t stream_index = 0; stream_index < SCE_GXM_MAX_VERTEX_STREAMS; stream_index++) {
        if (used_streams & (1 << stream_index))
            layouts[stream_index] = vertex_stream_layout(vertex_program, stream_index);
    }

    const bool attribute_bindings = needs_attribute_bindings(vertex_program);
    const auto add_binding = [&](uint32_t binding, uint32_t stream_index) {
        const SceGxmVertexStream &stream = vertex_program.streams[stream_index];
        const bool is_instanced = gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(stream.indexSource));
        binding_descr.push_back(vk::VertexInputBindingDescription{
            .binding = binding,
            .stride = layouts[stream_index].stride,
            .inputRate = is_instanced ? vk::VertexInputRate::eInstance : vk::VertexInputRate::eVertex });
    };

    for (size_t a = 0; a < vertex_program.attributes.size(); a++) {
        const SceGxmVertexAttribute &attribute = vertex_program.attributes[a];
        if (!vkvert->attribute_infos.contains(attribute.regIndex))
            continue;

        const shader::usse::AttributeInformation &info = vkvert->attribute_infos.at(attribute.regIndex);
        const AttributeFetch fetch = attribute_fetch(attribute, info, support_scaled_vertex_attribute, unsupported_rgb_vertex_attribute_formats);

        uint32_t binding = attribute.streamIndex;
        if (attribute_bindings) {
            binding = static_cast<uint32_t>(binding_descr.size());
            add_binding(binding, attribute.streamIndex);
        }

        const uint32_t offset = a < VertexStreamLayout::MAX_ATTRIBUTES ? layouts[attribute.streamIndex].offsets[a] : attribute.offset;
        for (uint32_t i = 0; i < fetch.array_size; i++) {
            attr_descr.push_back(vk::VertexInputAttributeDescription{
                .location = info.location + i,
                .binding = binding,
                .format = fetch.format,
                .offset = (attribute_bindings ? 0 : offset) + i * fetch.array_element_size });
        }
    }

    if (!attribute_bindings) {
        for (unsigned int stream_index = 0; stream_index < SCE_GXM_MAX_VERTEX_STREAMS; stream_index++) {
            if (used_streams & (1 << stream_index))
                add_binding(stream_index, stream_index);
        }
    }

    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding_descr);
    vertex_input.setVertexAttributeDescriptions(attr_descr);
    return vertex_input;
}

bool PipelineCache::needs_attribute_bindings(const ProgramBinding &vertex_program) const {
    const VertexProgram *vkvert = vertex_program.vertex_program.get();
    const uint32_t max_offset = state.physical_device_properties.limits.maxVertexInputAttributeOffset;
    for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
        const auto it = vkvert->attribute_infos.find(attribute.regIndex);
        if (it == vkvert->attribute_infos.end())
            continue;

        const shader::usse::AttributeInformation &info = it->second;
        // a matrix attribute is an array of vec4, the last one furthest along
        const uint32_t array_offset = info.regformat && info.component_count > 4 ? ((info.component_count - 1) / 4) * 16 : 0;
        if (attribute.offset + array_offset > max_offset)
            return true;
    }
    return false;
}

VertexStreamLayout PipelineCache::vertex_stream_layout(const ProgramBinding &vertex_program, const uint32_t stream_index) const {
    VertexStreamLayout layout;
    const uint32_t stride = vertex_program.streams[stream_index].stride;
    layout.stride = stride;

    struct Item {
        uint32_t index;
        uint32_t offset;
        uint32_t size;
        uint32_t align;
    };
    std::array<Item, VertexStreamLayout::MAX_ATTRIBUTES> items;
    uint32_t count = 0;
#ifdef __APPLE__
    // Metal only takes a stride that is a multiple of 4
    bool unaligned = stride % 4 != 0;
#else
    bool unaligned = false;
#endif
    const VertexProgram *vkvert = vertex_program.vertex_program.get();
    for (size_t a = 0; a < vertex_program.attributes.size() && a < VertexStreamLayout::MAX_ATTRIBUTES; a++) {
        const SceGxmVertexAttribute &attribute = vertex_program.attributes[a];
        layout.offsets[a] = attribute.offset;
        if (attribute.streamIndex != stream_index)
            continue;

        const auto it = vkvert->attribute_infos.find(attribute.regIndex);
        if (it == vkvert->attribute_infos.end())
            continue;

        const AttributeFetch fetch = attribute_fetch(attribute, it->second, support_scaled_vertex_attribute, unsupported_rgb_vertex_attribute_formats);
        items[count++] = { static_cast<uint32_t>(a), attribute.offset, fetch.size, fetch.component_size };
        unaligned |= attribute.offset % fetch.component_size != 0 || stride % fetch.component_size != 0;
    }
    if (!unaligned)
        return layout;

    std::sort(items.begin(), items.begin() + count, [](const Item &l, const Item &r) { return l.offset != r.offset ? l.offset < r.offset : l.size > r.size; });
    VertexStreamLayout::Copy block{};
    uint32_t delta = 0;
    for (uint32_t k = 0; k < count; k++) {
        const Item &item = items[k];
        if (k > 0 && (item.offset + delta) % item.align == 0) {
            block.size = std::max(block.size, item.offset + item.size - block.src);
        } else {
            if (k > 0)
                layout.copies[layout.copy_count++] = block;
            const uint32_t dst = align(std::max(layout.record_size, static_cast<uint32_t>(item.offset)), item.align);
            delta = dst - item.offset;
            block = { item.offset, dst, item.size };
        }
        layout.offsets[item.index] = item.offset + delta;
        layout.record_size = std::max(layout.record_size, block.dst + block.size);
    }
    if (count > 0)
        layout.copies[layout.copy_count++] = block;
    layout.stride = stride == 0 ? 0 : align(std::max(layout.record_size, stride), 4);
    layout.repack = true;
    return layout;
}

void PipelineCache::compiler_thread(MemState &mem) {
    moodycamel::ConsumerToken consumer_token(pipeline_compile_queue);

    // just a single loop, waiting for a pipeline compile request and compiling it
    CompileRequest *request;
    while (true) {
        if (!pipeline_compile_queue.wait_dequeue_timed(consumer_token, request, std::chrono::milliseconds(100))) {
            if (drain_compile_workers)
                break;
            continue;
        }

        if (request == nullptr)
            // use this as an instruction to stop the thread
            break;

        if (drain_compile_workers) {
            *request->pipeline = nullptr;
            delete request;
            continue;
        }

        vk::Pipeline pipeline = compile_pipeline(request->type, request->render_pass, *request->vertex_program_binding, *request->fragment_program_binding, *request->get_record(), request->has_color_surface_data, request->hints, request->has_casts, mem);
        // mark a refused pipeline as failed rather than leaving it null, which would make every
        // later draw queue the same doomed compilation again
        *request->pipeline = pipeline ? pipeline : std::bit_cast<vk::Pipeline, uint64_t>(~1ULL);

        const auto time_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        next_pipeline_cache_save = time_s + pipeline_cache_save_delay;

        state.shaders_count_compiled++;

        delete request;
    }
}

static vk::StencilOpState convert_op_state(const GxmStencilStateOp &state) {
    return vk::StencilOpState{
        .failOp = translate_stencil_op(state.stencil_fail),
        .passOp = translate_stencil_op(state.depth_pass),
        .depthFailOp = translate_stencil_op(state.depth_fail),
        .compareOp = translate_stencil_func(state.func)
    };
}

vk::Pipeline PipelineCache::compile_pipeline(SceGxmPrimitiveType type, vk::RenderPass render_pass, const ProgramBinding &vertex_program_binding, const ProgramBinding &fragment_program_binding, const GxmRecordState &record, bool has_color_surface_data, const shader::Hints &hints, bool has_casts, MemState &mem, bool cache_only) try {
    const VertexProgram &vertex_program = *vertex_program_binding.vertex_program;
    const SceGxmProgram *gxm_fragment_shader = fragment_program_binding.program();
    const VKFragmentProgram &fragment_program = *reinterpret_cast<VKFragmentProgram *>(
        fragment_program_binding.fragment_program.get());

    // the vertex input state must be computed before shader are retrieved in case symbols are stripped
    const vk::PipelineVertexInputStateCreateInfo vertex_input = get_vertex_input_state(vertex_program_binding);

    const bool use_shader_interlock = state.features.support_shader_interlock && gxm_fragment_shader->is_frag_color_used();
    const bool with_raw_attachment = state.features.preserve_f16_nan_as_u16 && !use_shader_interlock && record.color_base_format == SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16 && has_color_surface_data;

    shader::Hints fragment_hints = hints;
    fragment_hints.raw_color_attachment_input = with_raw_attachment && state.features.direct_fragcolor && gxm_fragment_shader->is_frag_color_used();

    const bool vertex_stores = hints.vertex_store_pass && shader::emulates_vertex_stores(*vertex_program_binding.program(), state.features);
    const vk::PipelineShaderStageCreateInfo vertex_shader = retrieve_shader(vertex_program_binding.program(), vertex_program.hash, true, fragment_program_binding.is_maskupdate, mem, hints, false, false, cache_only);
    const vk::PipelineShaderStageCreateInfo fragment_shader = vertex_stores
        ? vk::PipelineShaderStageCreateInfo{ .stage = vk::ShaderStageFlagBits::eFragment, .module = vertex_store_fragment_module, .pName = "main_fs" }
        : retrieve_shader(gxm_fragment_shader, fragment_program.hash, false, fragment_program_binding.is_maskupdate, mem, fragment_hints, record.is_gamma_corrected, has_casts, cache_only);
    if (cache_only && (!vertex_shader.module || !fragment_shader.module))
        // a cache-only request never translates a shader on the calling thread
        return nullptr;
    const vk::PipelineShaderStageCreateInfo shader_stages[] = { vertex_shader, fragment_shader };
    // disable the fragment shader if gxm asks us to
    const bool is_fragment_disabled = !vertex_stores && (record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED || gxm_fragment_shader->has_no_effect());
    const uint32_t shader_stage_count = is_fragment_disabled ? 1U : 2U;

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vertex_stores ? vk::PrimitiveTopology::ePointList : translate_primitive(type)
    };

    const bool two_sided = (record.two_sided == SCE_GXM_TWO_SIDED_ENABLED);

    const vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = (enable_depth_clamp && state.physical_device_features.depthClamp) ? VK_TRUE : VK_FALSE,
        .polygonMode = vertex_stores ? vk::PolygonMode::eFill : translate_polygon_mode(record.front_polygon_mode),
        .cullMode = vertex_stores ? vk::CullModeFlags() : translate_cull_mode(record.cull_mode),
        // front face is always counter clockwise
        .frontFace = vk::FrontFace::eCounterClockwise,
        .depthBiasEnable = VK_TRUE,
        .lineWidth = 1.0f
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1
    };
    // depth and stencil tests are always enabled on the ps vita as there is almost no cost in doing so
    // on a tiled renderer
    vk::PipelineDepthStencilStateCreateInfo ds_info{
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = (record.front_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED),
        .depthCompareOp = translate_depth_func(record.front_depth_func),
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_TRUE,
        .front = convert_op_state(record.front_stencil_state_op),
        .back = convert_op_state(two_sided ? record.back_stencil_state_op : record.front_stencil_state_op)
    };
    if (fetch_draw_stops_lrz_write(state, record, *gxm_fragment_shader)) {
        ds_info.front.compareOp = vk::CompareOp::eEqual;
        ds_info.front.failOp = vk::StencilOp::eKeep;
    }
    if (vertex_stores)
        ds_info = vk::PipelineDepthStencilStateCreateInfo{};

    vk::PipelineColorBlendStateCreateInfo color_blending{};
    if (support_coherent_framebuffer_fetch)
        color_blending.flags = vk::PipelineColorBlendStateCreateFlagBits::eRasterizationOrderAttachmentAccessEXT;

    const bool frag_has_no_output = gxm_fragment_shader->has_no_color_output();
    std::array<vk::PipelineColorBlendAttachmentState, 2> blend_attachments;
    blend_attachments[1] = vk::PipelineColorBlendAttachmentState{
        .blendEnable = VK_FALSE,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
    };
    if (is_fragment_disabled || frag_has_no_output || use_shader_interlock || vertex_stores) {
        // The write mask must be empty as the lack of a fragment shader results in undefined values
        blend_attachments[0] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = VK_FALSE,
            .colorWriteMask = vk::ColorComponentFlags()
        };
        blend_attachments[1].colorWriteMask = vk::ColorComponentFlags();
    } else {
        blend_attachments[0] = fragment_program.blending;
    }
    color_blending.attachmentCount = with_raw_attachment ? 2 : 1;
    color_blending.pAttachments = blend_attachments.data();

    vk::PipelineLayout pipeline_layout = pipeline_layouts[vertex_program.texture_count][fragment_program.texture_count];

    // all of these can be changed at any time using the vita graphics api (like opengl)
    // Because each one can take a lot of different values, it's better to set them as dynamic
    const std::array dynamic_states = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
        vk::DynamicState::eStencilCompareMask,
        vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eDepthBias,
        vk::DynamicState::eLineWidth,
    };
    vk::PipelineDynamicStateCreateInfo dynamic_info{};
    dynamic_info.setDynamicStates(dynamic_states);
    if (!state.physical_device_features.wideLines)
        dynamic_info.dynamicStateCount--;

    // we still need to specify the viewport and scissor count even though they are dynamic
    vk::PipelineViewportStateCreateInfo viewport{
        .viewportCount = 1,
        .scissorCount = 1
    };

    vk::GraphicsPipelineCreateInfo pipeline_info{
        .stageCount = shader_stage_count,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &ds_info,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .subpass = 0
    };

    // The stock Adreno driver can SIGSEGV inside createGraphicsPipeline
    // If the log ends on this line this exact vert/frag pair killed the driver's compiler
    if (state.is_adreno_stock && !cache_only)
        LOG_INFO("[PIPE] stock-Adreno: compiling pipeline vert={} frag={} ({})", hex_string(vertex_program.hash),
            hex_string(fragment_program.hash), is_fragment_disabled ? "frag-disabled" : "frag-enabled");

    // vulkan.hpp THROWS on an error result here, so a driver refusing the pipeline used to take
    // the whole emulator down before the check below could run. Some drivers (notably Mesa/Turnip)
    // also report an internal shader-compiler rejection as ErrorOutOfHostMemory rather than a
    // specific error.
    if (cache_only)
        pipeline_info.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequiredEXT;
    try {
        const auto result = state.device.createGraphicsPipeline(pipeline_cache, pipeline_info);
        if (result.result == vk::Result::ePipelineCompileRequiredEXT)
            // not in the driver cache
            return nullptr;
        if (result.result != vk::Result::eSuccess) {
            LOG_CRITICAL("Failed to create pipeline: {}", vk::to_string(result.result));
            return nullptr;
        }

        pipelines_created++;
        {
            constexpr bool log_pipeline_keys = false; // ~700 lines per run
            static std::atomic<uint32_t> pipekey_lines{ 0 };
            const uint32_t n = pipekey_lines.fetch_add(1, std::memory_order_relaxed);
            if (log_pipeline_keys && n < 4000) {
                const auto &sf = record.front_stencil_state_op;
                const auto &sb = record.back_stencil_state_op;
                LOG_INFO("[PIPEKEY] #{} vert={} frag={} blend={:016x} layout={:016x} prim={} fmt=0x{:x} cull={} two={} poly={} dfunc={} dwrite={} stF={}/{}/{}/{} stB={}/{}/{}/{} fmode={} fdis={} mask={} gamma={} rp=0x{:x}",
                    n, hex_string(vertex_program.hash).substr(0, 10), hex_string(fragment_program.hash).substr(0, 10),
                    fragment_program.blending_hash, vertex_program_binding.key_hash, static_cast<int>(type),
                    static_cast<uint32_t>(record.color_base_format), static_cast<int>(record.cull_mode), static_cast<int>(record.two_sided),
                    static_cast<int>(record.front_polygon_mode), static_cast<int>(record.front_depth_func), static_cast<int>(record.front_depth_write_mode),
                    static_cast<int>(sf.func), static_cast<int>(sf.stencil_fail), static_cast<int>(sf.depth_fail), static_cast<int>(sf.depth_pass),
                    static_cast<int>(sb.func), static_cast<int>(sb.stencil_fail), static_cast<int>(sb.depth_fail), static_cast<int>(sb.depth_pass),
                    static_cast<int>(record.front_side_fragment_program_mode), is_fragment_disabled ? 1 : 0,
                    record.is_maskupdate ? 1 : 0, record.is_gamma_corrected ? 1 : 0,
                    std::bit_cast<uint64_t>(static_cast<VkRenderPass>(render_pass)));
            }
        }
        return result.value;
    } catch (const vk::SystemError &err) {
        const uint32_t failure_index = pipelines_failed++;
        LOG_CRITICAL("Failed to create pipeline #{} ({} succeeded so far): {}", failure_index,
            pipelines_created.load(), err.what());
        LOG_CRITICAL("  vert shader: {}", hex_string(vertex_program.hash));
        LOG_CRITICAL("  frag shader: {} (fragment {})", hex_string(fragment_program.hash),
            is_fragment_disabled ? "disabled" : "enabled");
        LOG_CRITICAL("  colour base format: 0x{:X} raw_attachment: {} raw_input: {} shader_interlock: {} blend: {} attachments: {}",
            static_cast<uint32_t>(record.color_base_format), with_raw_attachment, fragment_hints.raw_color_attachment_input, use_shader_interlock,
            static_cast<bool>(fragment_program.blending.blendEnable), color_blending.attachmentCount);

        // Dump absolutely everything for the first few failures. Anything less means another
        // round trip, and the state that gets left out is always the state that mattered.
        if (failures_dumped++ < max_failures_dumped) {
            std::string render_pass_desc;
            {
                std::lock_guard<std::mutex> guard(diagnostics_mutex);
                auto it = render_pass_descriptions.find(std::bit_cast<uint64_t>(static_cast<VkRenderPass>(render_pass)));
                render_pass_desc = (it == render_pass_descriptions.end()) ? "<unknown render pass>" : it->second;
            }

            LOG_CRITICAL("=== FULL PIPELINE DUMP (failure #{}) ===", failure_index);
            LOG_CRITICAL("  render pass: {}", render_pass_desc);
            LOG_CRITICAL("  support_coherent_framebuffer_fetch: {} colour blend flags: 0x{:X}",
                support_coherent_framebuffer_fetch,
                static_cast<uint32_t>(static_cast<VkPipelineColorBlendStateCreateFlags>(color_blending.flags)));
            LOG_CRITICAL("  stages: {} (vertex entry main_vs, fragment entry main_fs) vert_spec_info: {} frag_spec_info: {}",
                shader_stage_count, static_cast<bool>(vertex_shader.pSpecializationInfo),
                static_cast<bool>(fragment_shader.pSpecializationInfo));
            LOG_CRITICAL("  pipeline layout: vertex_textures={} fragment_textures={}",
                vertex_program.texture_count, fragment_program.texture_count);
            LOG_CRITICAL("  input assembly: topology={} primitive_restart={}",
                vk::to_string(input_assembly.topology), static_cast<bool>(input_assembly.primitiveRestartEnable));
            LOG_CRITICAL("  rasterizer: depth_clamp={} discard={} polygon={} cull={} front_face={} depth_bias={} line_width={}",
                static_cast<bool>(rasterizer.depthClampEnable), static_cast<bool>(rasterizer.rasterizerDiscardEnable),
                vk::to_string(rasterizer.polygonMode), vk::to_string(rasterizer.cullMode),
                vk::to_string(rasterizer.frontFace), static_cast<bool>(rasterizer.depthBiasEnable), rasterizer.lineWidth);
            LOG_CRITICAL("  multisample: samples={} sample_shading={}",
                vk::to_string(multisampling.rasterizationSamples), static_cast<bool>(multisampling.sampleShadingEnable));
            LOG_CRITICAL("  depth/stencil: test={} write={} compare={} stencil_test={} front(fail={} pass={} depth_fail={} cmp={}) back(fail={} pass={} depth_fail={} cmp={})",
                static_cast<bool>(ds_info.depthTestEnable), static_cast<bool>(ds_info.depthWriteEnable),
                vk::to_string(ds_info.depthCompareOp), static_cast<bool>(ds_info.stencilTestEnable),
                vk::to_string(ds_info.front.failOp), vk::to_string(ds_info.front.passOp),
                vk::to_string(ds_info.front.depthFailOp), vk::to_string(ds_info.front.compareOp),
                vk::to_string(ds_info.back.failOp), vk::to_string(ds_info.back.passOp),
                vk::to_string(ds_info.back.depthFailOp), vk::to_string(ds_info.back.compareOp));
            for (uint32_t att = 0; att < color_blending.attachmentCount; att++) {
                const auto &b = blend_attachments[att];
                LOG_CRITICAL("  blend[{}]: enable={} colour(src={} dst={} op={}) alpha(src={} dst={} op={}) write_mask=0x{:X}",
                    att, static_cast<bool>(b.blendEnable), vk::to_string(b.srcColorBlendFactor),
                    vk::to_string(b.dstColorBlendFactor), vk::to_string(b.colorBlendOp),
                    vk::to_string(b.srcAlphaBlendFactor), vk::to_string(b.dstAlphaBlendFactor),
                    vk::to_string(b.alphaBlendOp), static_cast<uint32_t>(static_cast<VkColorComponentFlags>(b.colorWriteMask)));
            }
            std::string dyn;
            for (uint32_t d = 0; d < dynamic_info.dynamicStateCount; d++)
                dyn += (dyn.empty() ? "" : ", ") + vk::to_string(dynamic_info.pDynamicStates[d]);
            LOG_CRITICAL("  dynamic states ({}): {}", dynamic_info.dynamicStateCount, dyn);
            LOG_CRITICAL("  vertex input: {} bindings, {} attributes",
                vertex_input.vertexBindingDescriptionCount, vertex_input.vertexAttributeDescriptionCount);
            for (uint32_t b = 0; b < vertex_input.vertexBindingDescriptionCount; b++) {
                const auto &bd = vertex_input.pVertexBindingDescriptions[b];
                LOG_CRITICAL("    binding {}: stride={} rate={}", bd.binding, bd.stride, vk::to_string(bd.inputRate));
            }
            for (uint32_t a = 0; a < vertex_input.vertexAttributeDescriptionCount; a++) {
                const auto &ad = vertex_input.pVertexAttributeDescriptions[a];
                LOG_CRITICAL("    attribute loc={} binding={} format={} offset={}", ad.location, ad.binding,
                    vk::to_string(ad.format), ad.offset);
            }
            LOG_CRITICAL("  VERTEX SHADER {}\n      {}", hex_string(vertex_program.hash), describe_shader(vertex_program.hash));
            LOG_CRITICAL("  FRAGMENT SHADER {}\n      {}", hex_string(fragment_program.hash), describe_shader(fragment_program.hash));
            LOG_CRITICAL("  spv files live in: {}", state.shaders_path.string());
            LOG_CRITICAL("=== END PIPELINE DUMP ===");

            // and now find out which single piece of state the driver is objecting to
            bisect_pipeline_failure(pipeline_info);
        }
        return nullptr;
    }
} catch (const std::exception &error) {
    LOG_ERROR("Failed to compile pipeline: {}", error.what());
    return nullptr;
}

// Recreate the same pipeline several times, each time with one aspect neutralised, and report
// which variants the driver accepts. Whatever change makes it succeed is the cause.
void PipelineCache::bisect_pipeline_failure(const vk::GraphicsPipelineCreateInfo &failing_info) {
    LOG_CRITICAL("=== PIPELINE KNOCKOUT BISECT: retrying with individual state neutralised ===");

    auto attempt = [&](const char *what, const vk::GraphicsPipelineCreateInfo &info) {
        try {
            const auto res = state.device.createGraphicsPipeline(pipeline_cache, info);
            if (res.result == vk::Result::eSuccess) {
                LOG_CRITICAL("  [OK  ] {}", what);
                state.device.destroyPipeline(res.value);
            } else {
                LOG_CRITICAL("  [FAIL] {} -> {}", what, vk::to_string(res.result));
            }
        } catch (const vk::SystemError &e) {
            LOG_CRITICAL("  [FAIL] {} -> {}", what, e.what());
        }
    };

    // 1. vertex stage only: isolates "is the vertex shader itself acceptable?"
    {
        vk::GraphicsPipelineCreateInfo info = failing_info;
        vk::PipelineRasterizationStateCreateInfo raster = *failing_info.pRasterizationState;
        raster.rasterizerDiscardEnable = VK_TRUE;
        info.stageCount = 1;
        info.pRasterizationState = &raster;
        attempt("vertex stage only (rasterizer discard)", info);
    }

    // 2. colour blend flags flipped: tests the rasterization-order-attachment-access match
    {
        vk::PipelineColorBlendStateCreateInfo blend = *failing_info.pColorBlendState;
        blend.flags ^= vk::PipelineColorBlendStateCreateFlagBits::eRasterizationOrderAttachmentAccessEXT;
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pColorBlendState = &blend;
        attempt(fmt::format("colour blend flags toggled to 0x{:X}",
                    static_cast<uint32_t>(static_cast<VkPipelineColorBlendStateCreateFlags>(blend.flags)))
                    .c_str(),
            info);
    }

    // 3. no blending at all, full write mask
    {
        vk::PipelineColorBlendAttachmentState plain{
            .blendEnable = VK_FALSE,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
                | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
        };
        std::array<vk::PipelineColorBlendAttachmentState, 2> plains{ plain, plain };
        vk::PipelineColorBlendStateCreateInfo blend = *failing_info.pColorBlendState;
        blend.pAttachments = plains.data();
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pColorBlendState = &blend;
        attempt("blending disabled", info);
    }

    // 4. default rasterization state
    {
        const vk::PipelineRasterizationStateCreateInfo raster{ .lineWidth = 1.0f };
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pRasterizationState = &raster;
        attempt("default rasterization state", info);
    }

    // 5. depth and stencil tests off
    {
        const vk::PipelineDepthStencilStateCreateInfo ds{};
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pDepthStencilState = &ds;
        attempt("depth/stencil tests disabled", info);
    }

    // 6. no dynamic state (static viewport and scissor instead)
    {
        const vk::Viewport vp{ .width = 1.0f, .height = 1.0f, .maxDepth = 1.0f };
        const vk::Rect2D sc{ .extent = { 1, 1 } };
        vk::PipelineViewportStateCreateInfo viewport{};
        viewport.setViewports(vp);
        viewport.setScissors(sc);
        vk::PipelineRasterizationStateCreateInfo raster = *failing_info.pRasterizationState;
        raster.depthBiasEnable = VK_FALSE;
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pDynamicState = nullptr;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        attempt("no dynamic state (static viewport/scissor, no depth bias)", info);
    }

    // 7. empty vertex input
    {
        const vk::PipelineVertexInputStateCreateInfo vertex_input{};
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pVertexInputState = &vertex_input;
        attempt("empty vertex input state", info);
    }

    LOG_CRITICAL("=== END KNOCKOUT BISECT ===");
}

vk::Pipeline PipelineCache::retrieve_pipeline(VKContext &context, SceGxmPrimitiveType &type, bool consider_for_async, MemState &mem, bool vertex_store_pass) {
    const GxmRecordState &record = context.record;
    uint64_t key = hash_pipeline_record(record);
    uint64_t raw_key = XXH3_64bits(&record, record_pipeline_len);

    // add the hash of the blending
    const std::shared_ptr<ProgramBinding> &fragment_program_binding = record.fragment_program_binding;
    const VKFragmentProgram &fragment_program = *reinterpret_cast<VKFragmentProgram *>(
        fragment_program_binding->fragment_program.get());
    key ^= fragment_program.blending_hash;
    raw_key ^= fragment_program.blending_hash;

    // add the hash of the attribute and stream layout
    const std::shared_ptr<ProgramBinding> &vertex_program_binding = record.vertex_program_binding;
    key ^= vertex_program_binding->key_hash;
    raw_key ^= vertex_program_binding->key_hash;

    // and also add the primitive type
    key ^= static_cast<uint64_t>(type);
    raw_key ^= static_cast<uint64_t>(type);

    uint64_t variant_bits = 0;
    const SceGxmProgram *key_fragment_gxp = fragment_program_binding->program();
    if (key_fragment_gxp && key_fragment_gxp->is_native_color() && fragment_program.output_register_format != SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED)
        variant_bits |= static_cast<uint64_t>(fragment_program.output_register_format) << 56;
    const int key_one_channel = gxm::one_channel_source_component(record.color_surface.colorFormat);
    if (key_one_channel > 0)
        variant_bits |= static_cast<uint64_t>(key_one_channel) << 48;
    key ^= variant_bits;
    raw_key ^= variant_bits;
    if (vertex_store_pass) {
        key ^= 0x9E3779B97F4A7C15ull;
        raw_key ^= 0x9E3779B97F4A7C15ull;
    }

    const uint32_t cast_units = context.curr_frag_ublock.cast_sampler_bits | context.curr_frag_ublock.cast_phase_bits | context.curr_frag_ublock.raw_cast_bits;
    const bool has_casts = (cast_units & fragment_program_binding->fragment_program->textures_used.to_ulong()) != 0;
    if (has_casts) {
        key ^= 0xC2B2AE3D27D4EB4Full;
        raw_key ^= 0xC2B2AE3D27D4EB4Full;
    }

    // can't use constexpr because of apple clang...
    const vk::Pipeline pipeline_compiling = std::bit_cast<vk::Pipeline, uint64_t>(~0ULL);
    // a pipeline the driver refused: remembered so we don't recompile (and re-log) it on every
    // single draw, which turns one rejected pipeline into a freeze and a gigabyte of log
    const vk::Pipeline pipeline_failed = std::bit_cast<vk::Pipeline, uint64_t>(~1ULL);
    // if the pipeline is in the pipeline cache, we can expect its creation time to be almost instantaneous
    bool already_in_cache = false;

    auto it = pipelines.find(key);
    if (it != pipelines.end()) {
        if (it->second != nullptr) {
            if (it->second == pipeline_failed)
                return nullptr;
            if (raw_pipeline_keys_seen.insert(raw_key).second)
                state.pipelines_redundant_avoided++;
            if (it->second == pipeline_compiling) {
                // a worker is compiling it for an earlier draw (a draw that must not be skipped waits for it)
                if (!use_async_compilation || !must_not_skip_draw(record, consider_for_async))
                    return nullptr;
                while (it->second == pipeline_compiling)
                    std::this_thread::yield();
                return it->second == pipeline_failed ? nullptr : it->second;
            }
            return it->second;
        }
        already_in_cache = true;
    } else {
        // the pipeline hash was not in the cache;
        it = pipelines.insert({ key, pipeline_compiling }).first;
    }
    raw_pipeline_keys_seen.insert(raw_key);

    // get the correct renderpass here
    const SceGxmProgram *gxm_fragment_shader = fragment_program_binding->program();
    const bool use_shader_interlock = state.features.support_shader_interlock && gxm_fragment_shader->is_frag_color_used();
    const vk::RenderPass render_pass = use_shader_interlock ? context.current_shader_interlock_pass : context.current_render_pass;

    // the warm threads learn from it which draw states new pipelines use
    const WarmState draw_state{
        .color_format = static_cast<uint32_t>(record.color_surface.colorFormat),
        .raw = state.features.preserve_f16_nan_as_u16 && !use_shader_interlock && record.color_base_format == SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16 && static_cast<bool>(record.color_surface.data),
        .casts = has_casts,
        .render_pass = context.current_render_pass,
        .interlock_render_pass = context.current_shader_interlock_pass,
    };
    note_draw_state(draw_state);
    const uint64_t draw_pair = warm_pair_key(vertex_program_binding->vertex_program->hash, fragment_program.hash, fragment_program.output_register_format);
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        pairs_drawn.insert(draw_pair);
    }
    // update the shader hints
    context.shader_hints.color_format = record.color_surface.colorFormat;
    context.shader_hints.attributes = &vertex_program_binding->attributes;
    context.shader_hints.vertex_store_pass = vertex_store_pass;
    context.shader_hints.output_register_format = fragment_program_binding->fragment_program
        ? fragment_program_binding->fragment_program->output_register_format
        : SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED;
    // a native-colour program's register format has to agree with the surface it is drawn to
    if (gxm::LOG_FRAGOUT && context.shader_hints.output_register_format != SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED) {
        static std::mutex fragout_mutex;
        static std::set<std::tuple<const SceGxmProgram *, uint32_t, int>> fragout_seen;
        const std::lock_guard<std::mutex> fragout_lock(fragout_mutex);
        if (fragout_seen.emplace(gxm_fragment_shader, static_cast<uint32_t>(record.color_surface.colorFormat), static_cast<int>(context.shader_hints.output_register_format)).second)
            LOG_INFO("[FRAGOUT] draw: fragment program {} register format {} on colour surface format 0x{:X} (msaa mode {}, native_color={})",
                hex_string(fragment_program_binding->fragment_program->hash).substr(0, 12), static_cast<int>(context.shader_hints.output_register_format),
                static_cast<uint32_t>(record.color_surface.colorFormat), static_cast<int>(fragment_program_binding->fragment_program->multisample_mode), gxm_fragment_shader->is_native_color());
    }

    constexpr bool log_texture_hint_changes = false; // costs a mutex + map lookup per draw
    if constexpr (log_texture_hint_changes) {
        static std::mutex texhint_mutex;
        static std::unordered_map<const SceGxmProgram *, std::array<SceGxmTextureFormat, SCE_GXM_MAX_TEXTURE_UNITS>> texhint_seen;
        static uint32_t texhint_stale_count = 0;
        std::array<SceGxmTextureFormat, SCE_GXM_MAX_TEXTURE_UNITS> now{};
        for (uint32_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++)
            now[i] = context.shader_hints.fragment_textures[i];

        std::lock_guard<std::mutex> texhint_lock(texhint_mutex);
        auto [texhint_it, texhint_inserted] = texhint_seen.try_emplace(gxm_fragment_shader, now);
        if (texhint_inserted) {
            std::string fmts;
            for (uint32_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++)
                fmts += fmt::format(" [{}]=0x{:08X}", i, static_cast<uint32_t>(now[i]));
            LOG_INFO("[TEXHINT] first draw of fragment program {}:{}", fmt::ptr(gxm_fragment_shader), fmts);
        } else if (texhint_it->second != now) {
            texhint_stale_count++;
            if (texhint_stale_count <= 500) {
                std::string diff;
                for (uint32_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++)
                    if (texhint_it->second[i] != now[i])
                        diff += fmt::format(" [{}] 0x{:08X} -> 0x{:08X}", i,
                            static_cast<uint32_t>(texhint_it->second[i]), static_cast<uint32_t>(now[i]));
                LOG_WARN("[TEXHINT] STALE #{}: fragment program {} is being drawn with texture formats "
                         "different from the ones its cached shader was generated for:{}",
                    texhint_stale_count, fmt::ptr(gxm_fragment_shader), diff);
                if (texhint_stale_count == 500)
                    LOG_WARN("[TEXHINT] (further STALE lines suppressed)");
            }
            texhint_it->second = now;
        }
    }

    const bool compile_pipeline_async = !already_in_cache && use_async_compilation && !must_not_skip_draw(record, consider_for_async);

    if (compile_pipeline_async && probe_before_defer && support_cache_only_creation) {
        // the driver can already hold this pipeline, for example the same shaders with other render state, and then
        // hands it over without compiling
        const vk::Pipeline probed = compile_pipeline(type, render_pass, *vertex_program_binding, *fragment_program_binding, record, static_cast<bool>(record.color_surface.data), context.shader_hints, has_casts, mem, true);
        if (probed) {
            next_pipeline_cache_save = now_s() + pipeline_cache_save_delay;
            state.shaders_count_compiled++;
            it->second = probed;
            return probed;
        }
    }

    if (compile_pipeline_async) {
        // create the pipeline compile request
        CompileRequest *request = new CompileRequest;
        *request = {
            .pipeline = &it->second,
            .type = type,
            .render_pass = render_pass,
            .vertex_program_binding = vertex_program_binding,
            .fragment_program_binding = fragment_program_binding,
            .has_color_surface_data = static_cast<bool>(record.color_surface.data),
            .hints = context.shader_hints,
            .has_casts = has_casts
        };
        memcpy(request->record_data, &record, record_pipeline_len);
        it->second = pipeline_compiling;

        pipeline_compile_queue.enqueue(pipeline_compile_queue_token, request);

        return nullptr;
    } else {
        // can't wait, compile it right now
        vk::Pipeline result = compile_pipeline(type, render_pass, *vertex_program_binding, *fragment_program_binding, record, static_cast<bool>(record.color_surface.data), context.shader_hints, has_casts, mem);

        const auto time_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        next_pipeline_cache_save = time_s + pipeline_cache_save_delay;

        if (!already_in_cache)
            state.shaders_count_compiled++;

        it->second = result ? result : pipeline_failed;

        return result;
    }
}

vk::ShaderModule PipelineCache::precompile_shader(const Sha256Hash &hash, bool search_first) try {
    if (search_first) {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        auto it = shaders.find(hash);
        if (it != shaders.end())
            return it->second;
    }

    if (!fs::exists(state.shaders_path) || fs::is_empty(state.shaders_path))
        return nullptr;

    Sha256Hash shader_hash;
    memcpy(shader_hash.data(), hash.data(), sizeof(Sha256Hash));
    const std::string shader_file_name = fmt::format("vk{}-{}.spv", shader::CURRENT_VERSION, hex_string(shader_hash));
    const std::vector<uint32_t> source = renderer::pre_load_shader_spirv(state.shaders_path / shader_file_name);

    if (source.empty())
        return nullptr;

    vk::ShaderModuleCreateInfo shader_info{
        .codeSize = sizeof(uint32_t) * source.size(),
        .pCode = source.data()
    };

    vk::ShaderModule shader = state.device.createShaderModule(shader_info);
    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        shaders[hash] = shader;
    }

    return shader;
} catch (const std::exception &error) {
    LOG_ERROR("Could not load cached shader {}: {}", hex_string(hash), error.what());
    return nullptr;
}

bool PipelineCache::must_not_skip_draw(const GxmRecordState &record, bool consider_for_async) const {
    return !consider_for_async || (sync_new_surface_draws && !can_use_deferred_compilation) || (sync_depth_stencil_writers && writes_depth_or_stencil(record));
}

void PipelineCache::warm_program_pair(const std::shared_ptr<ProgramBinding> &vertex, const std::shared_ptr<ProgramBinding> &fragment) {
    if (!warm_enabled || !vertex || !fragment || !vertex->vertex_program || !fragment->fragment_program || fragment->is_maskupdate)
        return;
    const uint64_t pair = warm_pair_key(vertex->vertex_program->hash, fragment->fragment_program->hash, fragment->fragment_program->output_register_format);
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        if (!warm_pairs_requested.insert(pair).second)
            return;
    }
    warm_queue.enqueue(new WarmRequest{ vertex, fragment, pair });
}

void PipelineCache::warm_thread(MemState &mem) {
#if defined(__linux__)
    // below the guest and render threads: this work only pays off later
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
    while (true) {
        WarmRequest *request = nullptr;
        warm_queue.wait_dequeue(request);
        if (!request)
            break;
        if (!warm_stopping)
            warm_pair(*request, mem);
        delete request;
    }
}

void PipelineCache::stop_warm_threads() {
    warm_enabled = false;
    warm_stopping = true;
    for (size_t i = 0; i < warm_threads.size(); i++)
        warm_queue.enqueue(nullptr);
    for (auto &thread : warm_threads) {
        if (thread.joinable())
            thread.join();
    }
    warm_threads.clear();
    WarmRequest *request;
    while (warm_queue.try_dequeue(request))
        delete request;
    warm_stopping = false;
}

bool PipelineCache::codegen_ignores_draw_hints(const SceGxmProgram &program, bool is_vertex, const shader::Hints &hints) {
    struct Variant {
        SceGxmTextureFormat texture;
        SceGxmColorFormat color;
        bool no_attributes;
    };
    static constexpr Variant variants[] = {
        { SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, false },
        { SCE_GXM_TEXTURE_FORMAT_F32_R, SCE_GXM_COLOR_FORMAT_F16F16F16F16_ABGR, true },
        { SCE_GXM_TEXTURE_FORMAT_F16F16_GR, SCE_GXM_COLOR_FORMAT_U5U6U5_BGR, true },
        { SCE_GXM_TEXTURE_FORMAT_S16S16S16S16_ABGR, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, false },
    };
    static const std::vector<SceGxmVertexAttribute> no_attributes;
    const bool vary_color = !is_vertex && !program.is_frag_color_used();
    shader::usse::SpirvCode reference;
    for (const Variant &variant : variants) {
        shader::Hints variant_hints = hints;
        std::fill_n(is_vertex ? variant_hints.vertex_textures : variant_hints.fragment_textures, SCE_GXM_MAX_TEXTURE_UNITS, variant.texture);
        if (vary_color)
            variant_hints.color_format = variant.color;
        if (is_vertex && variant.no_attributes)
            variant_hints.attributes = &no_attributes;
        const shader::GeneratedShader generated = shader::convert_gxp(program, "warm", state.features, shader::Target::SpirVVulkan, variant_hints, false, false);
        if (generated.spirv.empty())
            return false;
        if (reference.empty())
            reference = generated.spirv;
        else if (generated.spirv != reference)
            return false;
    }
    return true;
}

bool PipelineCache::shader_available(const SceGxmProgram &program, const Sha256Hash &base_hash, bool is_vertex, const shader::Hints &hints) {
    std::string version_suffix;
    const Sha256Hash hash = shader_cache_key(&program, base_hash, is_vertex, hints, state.features, version_suffix);
    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        const auto found = shaders.find(hash);
        if (found != shaders.end() && found->second != nullptr)
            return true;
    }
    return fs::exists(state.shaders_path / fmt::format("vk{}{}-{}.spv", shader::CURRENT_VERSION, version_suffix, hex_string(base_hash)));
}

void PipelineCache::warm_pair(const WarmRequest &request, MemState &mem) try {
    const ProgramBinding &vertex_binding = *request.vertex;
    const ProgramBinding &fragment_binding = *request.fragment;
    const SceGxmProgram &vertex_gxp = *vertex_binding.program();
    const SceGxmProgram &fragment_gxp = *fragment_binding.program();

    std::vector<WarmState> states;
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        // a draw already created a pipeline for it, so the driver cache holds that draw's state
        if (pairs_drawn.contains(request.pair_key))
            return;
        states = warm_states;
    }
    if (states.empty()) {
        // nothing drawn yet: an RGBA8 surface without casts
        states.push_back(WarmState{
            .color_format = static_cast<uint32_t>(SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR),
            .render_pass = warm_render_pass,
            .interlock_render_pass = warm_interlock_render_pass,
        });
    }

    shader::Hints hints{
        .attributes = &vertex_binding.attributes,
        .color_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
    };
    std::fill_n(hints.vertex_textures, SCE_GXM_MAX_TEXTURE_UNITS, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    std::fill_n(hints.fragment_textures, SCE_GXM_MAX_TEXTURE_UNITS, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    hints.output_register_format = fragment_binding.fragment_program->output_register_format;

    // a stage with no cached module is only generated here when the draw would generate the same module
    if (!shader_available(vertex_gxp, vertex_binding.vertex_program->hash, true, hints) && !codegen_ignores_draw_hints(vertex_gxp, true, hints))
        return;

    const bool interlock = state.features.support_shader_interlock && fragment_gxp.is_frag_color_used();
    for (const WarmState &warm_state : states) {
        hints.color_format = static_cast<SceGxmColorFormat>(warm_state.color_format);
        if (!shader_available(fragment_gxp, fragment_binding.fragment_program->hash, false, hints) && !codegen_ignores_draw_hints(fragment_gxp, false, hints))
            continue;
        const vk::RenderPass render_pass = interlock ? warm_state.interlock_render_pass : warm_state.render_pass;
        if (!render_pass)
            continue;
        GxmRecordState record{};
        record.color_base_format = gxm::get_base_format(hints.color_format);
        const bool has_color_surface_data = warm_state.raw || record.color_base_format != SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16;

        vk::Pipeline pipeline;
        if (support_cache_only_creation)
            pipeline = compile_pipeline(SCE_GXM_PRIMITIVE_TRIANGLES, render_pass, vertex_binding, fragment_binding, record, has_color_surface_data, hints, warm_state.casts, mem, true);
        if (!pipeline)
            pipeline = compile_pipeline(SCE_GXM_PRIMITIVE_TRIANGLES, render_pass, vertex_binding, fragment_binding, record, has_color_surface_data, hints, warm_state.casts, mem);
        if (!pipeline)
            continue;
        // only the driver cache entry is wanted: the draw creates its own pipeline from it
        state.device.destroyPipeline(pipeline);
    }
} catch (const std::exception &error) {
    LOG_WARN("[PIPEWARM] background compile failed: {}", error.what());
}

void PipelineCache::note_draw_state(const WarmState &draw_state) {
    auto &entry = draw_states[draw_state.key()];
    if (entry.second == 0)
        entry.first = draw_state;
    entry.second++;
    draw_states_total++;

    // the most common state
    std::vector<std::pair<uint32_t, const WarmState *>> ranked;
    for (const auto &[state_key, value] : draw_states)
        ranked.push_back({ value.second, &value.first });
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<WarmState> top;
    for (size_t i = 0; i < ranked.size() && top.size() < 2; i++) {
        if (i > 0 && ranked[i].first * 4 < draw_states_total)
            break;
        top.push_back(*ranked[i].second);
    }
    std::lock_guard<std::mutex> guard(warm_mutex);
    if (top.size() != warm_states.size() || !std::equal(top.begin(), top.end(), warm_states.begin(), [](const WarmState &a, const WarmState &b) { return a.key() == b.key(); }))
        warm_states = top;
}
} // namespace renderer::vulkan
