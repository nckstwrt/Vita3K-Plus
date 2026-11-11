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

// Pipeline recipes: everything a pipeline is created from (the two programs, the pipeline part of the
// record, the render pass, the shader hints), saved the first time a draw creates it, in
// <cache>/pipeline-recipes/<title>/<self>/recipes-vk.dat. The pipeline cache only holds the driver's
// compiled results, which a new driver (another pipelineCacheUUID) or a translator change makes useless,
// and then every pipeline used to compile during play. At boot every recipe is created again on worker
// threads, so by the time the game draws with it the driver has it in its cache.

#include <renderer/functions.h>
#include <renderer/types.h>
#include <renderer/vulkan/pipeline_cache.h>
#include <renderer/vulkan/state.h>
#include <renderer/vulkan/types.h>

#include <util/fs.h>
#include <util/log.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace renderer::vulkan {

namespace {

constexpr uint32_t recipes_magic = 0x31525056; // "VPR1"
constexpr uint32_t recipes_version = 1;
constexpr size_t recipe_record_len = offsetof(GxmRecordState, vertex_streams);
constexpr const char *recipes_file_name = "recipes-vk.dat";

// what the file layout depends on: a file written with other sizes is dropped
struct RecipesHeader {
    uint32_t magic = recipes_magic;
    uint32_t version = recipes_version;
    uint32_t record_len = recipe_record_len;
    uint32_t attribute_size = sizeof(SceGxmVertexAttribute);
    uint32_t stream_size = sizeof(SceGxmVertexStream);
    uint32_t blend_size = sizeof(vk::PipelineColorBlendAttachmentState);
    uint32_t texture_units = SCE_GXM_MAX_TEXTURE_UNITS;
    uint32_t reserved = 0;
};

struct Writer {
    std::vector<uint8_t> data;
    void bytes(const void *p, size_t n) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        data.insert(data.end(), b, b + n);
    }
    template <typename T>
    void put(const T &v) {
        bytes(&v, sizeof(T));
    }
};

struct Reader {
    const uint8_t *p;
    const uint8_t *end;
    bool ok = true;
    bool bytes(void *out, size_t n) {
        if (!ok || static_cast<size_t>(end - p) < n)
            return ok = false;
        memcpy(out, p, n);
        p += n;
        return true;
    }
    template <typename T>
    bool get(T &v) {
        return bytes(&v, sizeof(T));
    }
};

// the fields of a fragment program that its blend info decided (renderer::create, vulkan::create)
struct FragmentFields {
    vk::PipelineColorBlendAttachmentState blending;
    uint64_t blending_hash = 0;
    uint8_t color_write_mask = 0xF;
    uint8_t depth_mirror = 0;
    uint32_t output_register_format = 0;
    uint32_t multisample_mode = 0;
};

FragmentFields fragment_fields(const ProgramBinding &binding) {
    const auto &fp = *reinterpret_cast<const VKFragmentProgram *>(binding.fragment_program.get());
    return FragmentFields{
        .blending = fp.blending,
        .blending_hash = fp.blending_hash,
        .color_write_mask = fp.color_write_mask,
        .depth_mirror = fp.depth_mirror,
        .output_register_format = static_cast<uint32_t>(fp.output_register_format),
        .multisample_mode = static_cast<uint32_t>(fp.multisample_mode),
    };
}

// a program entry: 'P', id, kind, the GXP, then what the binding adds to it
void write_program(Writer &w, uint64_t id, const ProgramBinding &binding, bool is_vertex) {
    w.put<uint8_t>('P');
    w.put(id);
    w.put<uint8_t>(is_vertex ? 'V' : 'F');
    w.put(static_cast<uint32_t>(binding.gxp.size()));
    w.bytes(binding.gxp.data(), binding.gxp.size());
    if (is_vertex) {
        w.put(static_cast<uint32_t>(binding.attributes.size()));
        w.bytes(binding.attributes.data(), binding.attributes.size() * sizeof(SceGxmVertexAttribute));
        w.put(static_cast<uint32_t>(binding.streams.size()));
        w.bytes(binding.streams.data(), binding.streams.size() * sizeof(SceGxmVertexStream));
        w.put(binding.key_hash);
    } else {
        w.put<uint8_t>(binding.is_maskupdate ? 1 : 0);
        const FragmentFields f = fragment_fields(binding);
        w.put(f);
    }
}

uint64_t program_id(const ProgramBinding &binding, bool is_vertex) {
    Writer w;
    write_program(w, 0, binding, is_vertex);
    return XXH3_64bits(w.data.data(), w.data.size());
}

struct ParsedProgram {
    bool is_vertex = false;
    std::vector<uint8_t> gxp;
    std::vector<SceGxmVertexAttribute> attributes;
    std::vector<SceGxmVertexStream> streams;
    uint64_t key_hash = 0;
    bool is_maskupdate = false;
    FragmentFields fragment;
};

struct ParsedRecipe {
    uint64_t key = 0;
    uint64_t vertex_id = 0, fragment_id = 0;
    uint32_t type = 0;
    std::array<uint8_t, recipe_record_len> record{};
    PipelineRenderPassParams render_pass;
    bool has_color_surface_data = false, has_casts = false;
    uint32_t color_format = 0;
    std::array<uint32_t, SCE_GXM_MAX_TEXTURE_UNITS> vertex_textures{}, fragment_textures{};
    uint32_t output_register_format = 0;
    bool vertex_store_pass = false, hw_depth_cull = false;
};

void put_render_pass(Writer &w, const PipelineRenderPassParams &rp) {
    w.put(static_cast<uint32_t>(rp.format));
    const uint8_t flags[6] = { rp.depth_load, rp.stencil_load, rp.force_store, rp.is_color_transient, rp.no_color, rp.has_raw };
    w.bytes(flags, sizeof(flags));
}

bool get_render_pass(Reader &r, PipelineRenderPassParams &rp) {
    uint32_t format;
    uint8_t flags[6];
    if (!r.get(format) || !r.bytes(flags, sizeof(flags)))
        return false;
    rp.format = static_cast<vk::Format>(format);
    rp.depth_load = flags[0];
    rp.stencil_load = flags[1];
    rp.force_store = flags[2];
    rp.is_color_transient = flags[3];
    rp.no_color = flags[4];
    rp.has_raw = flags[5];
    return true;
}

// The whole file: programs by id and recipes. False when it is missing, written with another layout, or
// truncated before its first entry (a truncated last entry is dropped).
bool read_recipes(const fs::path &path, std::unordered_map<uint64_t, ParsedProgram> &programs, std::vector<ParsedRecipe> &recipes) {
    fs::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    RecipesHeader header;
    const RecipesHeader expected;
    if (data.size() < sizeof(header))
        return false;
    memcpy(&header, data.data(), sizeof(header));
    if (memcmp(&header, &expected, sizeof(header)) != 0)
        return false;
    Reader r{ data.data() + sizeof(header), data.data() + data.size() };
    while (r.p < r.end) {
        uint8_t tag;
        if (!r.get(tag))
            break;
        if (tag == 'P') {
            uint64_t id;
            uint8_t kind;
            uint32_t size;
            if (!r.get(id) || !r.get(kind) || !r.get(size) || size > (64u << 20))
                break;
            ParsedProgram prog;
            prog.is_vertex = kind == 'V';
            prog.gxp.resize(size);
            if (!r.bytes(prog.gxp.data(), size))
                break;
            if (prog.is_vertex) {
                uint32_t n;
                if (!r.get(n) || n > 4096)
                    break;
                prog.attributes.resize(n);
                if (!r.bytes(prog.attributes.data(), n * sizeof(SceGxmVertexAttribute)) || !r.get(n) || n > 4096)
                    break;
                prog.streams.resize(n);
                if (!r.bytes(prog.streams.data(), n * sizeof(SceGxmVertexStream)) || !r.get(prog.key_hash))
                    break;
            } else {
                uint8_t mask;
                if (!r.get(mask) || !r.get(prog.fragment))
                    break;
                prog.is_maskupdate = mask != 0;
            }
            programs[id] = std::move(prog);
        } else if (tag == 'R') {
            ParsedRecipe rec;
            uint8_t flags[4];
            if (!r.get(rec.key) || !r.get(rec.vertex_id) || !r.get(rec.fragment_id) || !r.get(rec.type)
                || !r.bytes(rec.record.data(), rec.record.size()) || !get_render_pass(r, rec.render_pass)
                || !r.get(rec.color_format) || !r.get(rec.vertex_textures) || !r.get(rec.fragment_textures)
                || !r.get(rec.output_register_format) || !r.bytes(flags, sizeof(flags)))
                break;
            rec.has_color_surface_data = flags[0];
            rec.has_casts = flags[1];
            rec.vertex_store_pass = flags[2];
            rec.hw_depth_cull = flags[3];
            recipes.push_back(rec);
        } else {
            break;
        }
    }
    return true;
}

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

bool PipelineCache::recipes_enabled() const {
    return !state.recipes_path.empty() && state.cmd_set_index_range_malisx2 != nullptr;
}

void PipelineCache::record_recipe(uint64_t key, SceGxmPrimitiveType type, vk::RenderPass render_pass, const ProgramBinding &vertex_binding,
    const ProgramBinding &fragment_binding, const GxmRecordState &record, bool has_color_surface_data, const shader::Hints &hints, bool has_casts) {
    if (!recipes_enabled() || !vertex_binding.vertex_program || !fragment_binding.fragment_program)
        return;
    RenderPassParams rp;
    {
        std::lock_guard<std::mutex> guard(render_pass_params_mutex);
        const auto found = render_pass_params.find(static_cast<VkRenderPass>(render_pass));
        if (found == render_pass_params.end())
            return;
        rp = found->second;
    }

    std::lock_guard<std::mutex> guard(recipes_mutex);
    if (!recipes_recorded.insert(key).second)
        return;

    const fs::path path = state.recipes_path / recipes_file_name;
    Writer w;
    if (!recipes_file_checked) {
        // start_recipe_replay read the file, or there is none (or a stale one): a file without a valid header
        // is started again
        recipes_file_checked = true;
        try {
            fs::create_directories(state.recipes_path);
        } catch (const std::exception &) {
            return;
        }
        fs::ifstream in(path, std::ios::binary);
        RecipesHeader header;
        const RecipesHeader expected;
        const bool valid = in.is_open() && in.read(reinterpret_cast<char *>(&header), sizeof(header)) && memcmp(&header, &expected, sizeof(header)) == 0;
        in.close();
        if (!valid) {
            fs::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(&expected), sizeof(expected));
            recipe_programs_written.clear();
        }
    }

    const uint64_t vertex_id = program_id(vertex_binding, true);
    const uint64_t fragment_id = program_id(fragment_binding, false);
    if (recipe_programs_written.insert(vertex_id).second)
        write_program(w, vertex_id, vertex_binding, true);
    if (recipe_programs_written.insert(fragment_id).second)
        write_program(w, fragment_id, fragment_binding, false);

    w.put<uint8_t>('R');
    w.put(key);
    w.put(vertex_id);
    w.put(fragment_id);
    w.put(static_cast<uint32_t>(type));
    w.bytes(&record, recipe_record_len);
    put_render_pass(w, rp);
    w.put(static_cast<uint32_t>(hints.color_format));
    std::array<uint32_t, SCE_GXM_MAX_TEXTURE_UNITS> textures;
    for (int i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++)
        textures[i] = static_cast<uint32_t>(hints.vertex_textures[i]);
    w.put(textures);
    for (int i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++)
        textures[i] = static_cast<uint32_t>(hints.fragment_textures[i]);
    w.put(textures);
    w.put(static_cast<uint32_t>(hints.output_register_format));
    const uint8_t flags[4] = { has_color_surface_data, has_casts, hints.vertex_store_pass, hints.hw_depth_cull };
    w.bytes(flags, sizeof(flags));

    fs::ofstream out(path, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char *>(w.data.data()), static_cast<std::streamsize>(w.data.size()));
}

void PipelineCache::stop_recipe_replay() {
    replay_stopping = true;
    for (auto &thread : replay_threads) {
        if (thread.joinable())
            thread.join();
    }
    replay_threads.clear();
    replay_stopping = false;
}

void PipelineCache::start_recipe_replay() {
    stop_recipe_replay();
    {
        std::lock_guard<std::mutex> guard(recipes_mutex);
        recipes_recorded.clear();
        recipe_programs_written.clear();
        recipes_file_checked = false;
    }
    if (!recipes_enabled())
        return;

    std::unordered_map<uint64_t, ParsedProgram> programs;
    std::vector<ParsedRecipe> recipes;
    if (!read_recipes(state.recipes_path / recipes_file_name, programs, recipes))
        return;
    {
        // what the file has is not written again
        std::lock_guard<std::mutex> guard(recipes_mutex);
        recipes_file_checked = true;
        for (const auto &[id, prog] : programs)
            recipe_programs_written.insert(id);
        for (const ParsedRecipe &rec : recipes)
            recipes_recorded.insert(rec.key);
    }
    if (recipes.empty())
        return;

    // the programs, created as the game creates them (renderer::create), with what its blend info decided
    auto bindings = std::make_shared<std::unordered_map<uint64_t, std::shared_ptr<ProgramBinding>>>();
    GXPPtrMap gxp_ptr_map;
    for (auto &[id, prog] : programs) {
        auto binding = std::make_shared<ProgramBinding>();
        binding->gxp = std::move(prog.gxp);
        const SceGxmProgram &gxp = *binding->program();
        if (binding->gxp.size() < sizeof(SceGxmProgram) || gxp.size > binding->gxp.size())
            continue;
        if (prog.is_vertex) {
            std::unique_ptr<VertexProgram> vp;
            if (!renderer::create(vp, state, gxp, gxp_ptr_map, prog.attributes))
                continue;
            binding->vertex_program = std::move(vp);
            binding->attributes = std::move(prog.attributes);
            binding->streams = std::move(prog.streams);
            binding->key_hash = prog.key_hash;
        } else {
            std::unique_ptr<FragmentProgram> fp;
            if (!renderer::create(fp, state, gxp, nullptr, gxp_ptr_map, static_cast<SceGxmOutputRegisterFormat>(prog.fragment.output_register_format),
                    static_cast<SceGxmMultisampleMode>(prog.fragment.multisample_mode)))
                continue;
            auto *fp_vk = reinterpret_cast<VKFragmentProgram *>(fp.get());
            fp_vk->blending = prog.fragment.blending;
            fp_vk->blending_hash = prog.fragment.blending_hash;
            fp_vk->color_write_mask = prog.fragment.color_write_mask;
            fp_vk->depth_mirror = prog.fragment.depth_mirror;
            binding->fragment_program = std::move(fp);
            binding->is_maskupdate = prog.is_maskupdate;
        }
        (*bindings)[id] = std::move(binding);
    }

    // the render passes, on this thread: retrieve_render_pass is not for the workers
    struct Item {
        uint64_t key;
        SceGxmPrimitiveType type;
        vk::RenderPass render_pass;
        std::shared_ptr<ProgramBinding> vertex, fragment;
        GxmRecordState record;
        bool has_color_surface_data, has_casts;
        shader::Hints hints;
    };
    auto items = std::make_shared<std::vector<Item>>();
    items->reserve(recipes.size());
    for (const ParsedRecipe &rec : recipes) {
        const auto v = bindings->find(rec.vertex_id);
        const auto f = bindings->find(rec.fragment_id);
        if (v == bindings->end() || f == bindings->end() || !v->second->vertex_program || !f->second->fragment_program)
            continue;
        const RenderPassParams &rp = rec.render_pass;
        Item item{
            .key = rec.key,
            .type = static_cast<SceGxmPrimitiveType>(rec.type),
            .render_pass = retrieve_render_pass(rp.format, rp.depth_load, rp.stencil_load, rp.force_store, rp.is_color_transient, rp.no_color, rp.has_raw),
            .vertex = v->second,
            .fragment = f->second,
            .record = {},
            .has_color_surface_data = rec.has_color_surface_data,
            .has_casts = rec.has_casts,
            .hints = {},
        };
        memcpy(&item.record, rec.record.data(), recipe_record_len);
        item.hints.attributes = &item.vertex->attributes;
        item.hints.color_format = static_cast<SceGxmColorFormat>(rec.color_format);
        for (int i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
            item.hints.vertex_textures[i] = static_cast<SceGxmTextureFormat>(rec.vertex_textures[i]);
            item.hints.fragment_textures[i] = static_cast<SceGxmTextureFormat>(rec.fragment_textures[i]);
        }
        item.hints.output_register_format = static_cast<SceGxmOutputRegisterFormat>(rec.output_register_format);
        item.hints.vertex_store_pass = rec.vertex_store_pass;
        // a recipe written while the driver culled near and far fragments uses that vertex shader only while it still does
        item.hints.hw_depth_cull = rec.hw_depth_cull && state.cmd_set_depth_cull_malisx2 != nullptr;
        items->push_back(std::move(item));
        items->back().hints.attributes = &items->back().vertex->attributes;
    }
    LOG_INFO("[RECIPES] {} pipelines of earlier sessions to create again ({} programs)", items->size(), bindings->size());

    struct Progress {
        std::atomic<size_t> next{ 0 }, done{ 0 }, compiled{ 0 }, cached{ 0 }, failed{ 0 };
        uint64_t start_ms = now_ms();
    };
    auto progress = std::make_shared<Progress>();
    // all of them before the game reaches gameplay: a driver update recompiles every recipe (~25 a second a thread)
    const int thread_count = 5;
    for (int t = 0; t < thread_count; t++) {
        replay_threads.emplace_back([this, items, bindings, progress]() {
            // the game's memory is known once the renderer is fully set up
            for (int i = 0; i < 600 && !state.mem && !replay_stopping; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!state.mem)
                return;
            MemState &mem = *state.mem;
            while (!replay_stopping) {
                const size_t i = progress->next.fetch_add(1);
                if (i >= items->size())
                    break;
                const Item &item = (*items)[i];
                try {
                    vk::Pipeline pipeline;
                    if (support_cache_only_creation)
                        pipeline = compile_pipeline(item.type, item.render_pass, *item.vertex, *item.fragment, item.record, item.has_color_surface_data, item.hints, item.has_casts, mem, true);
                    if (pipeline) {
                        progress->cached++;
                    } else {
                        pipeline = compile_pipeline(item.type, item.render_pass, *item.vertex, *item.fragment, item.record, item.has_color_surface_data, item.hints, item.has_casts, mem);
                        if (pipeline)
                            progress->compiled++;
                        else
                            progress->failed++;
                    }
                    // only the driver's cache entry is wanted: the draw creates its own pipeline from it
                    if (pipeline)
                        state.device.destroyPipeline(pipeline);
                } catch (const std::exception &error) {
                    progress->failed++;
                    LOG_WARN("[RECIPES] pipeline {:016X}: {}", item.key, error.what());
                }
                const size_t done = ++progress->done;
                if (done == items->size()) {
                    LOG_INFO("[RECIPES] done: {} pipelines in {:.1f} s ({} compiled, {} already in the driver cache, {} failed)", done,
                        (now_ms() - progress->start_ms) / 1000.0, progress->compiled.load(), progress->cached.load(), progress->failed.load());
                    // keep what was compiled
                    next_pipeline_cache_save = 0;
                } else if (done % 100 == 0) {
                    LOG_INFO("[RECIPES] {}/{} after {:.1f} s", done, items->size(), (now_ms() - progress->start_ms) / 1000.0);
                }
            }
        });
    }
}

} // namespace renderer::vulkan
