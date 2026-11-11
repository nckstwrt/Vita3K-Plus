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

#pragma once

#include <util/fs.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace renderer::texture {

// Converted texture data on disk, per game: the BC blocks a PVRTC mip was decoded and encoded into, by a hash of
// the mip's source bytes (and what the conversion depends on). Converting a mip costs a PVRTC decode and a BC
// encode on the render thread; a race start converts hundreds of them at once, every time the game reloads its
// track textures, so a mip converted once is read back from here afterwards, in this session or a later one.
// Writes go through a background thread; the render thread only reads entries already written.
class BcDiskCache {
public:
    ~BcDiskCache();
    // the pack and its index in dir, created when missing; false when they cannot be used
    bool open(const fs::path &dir);
    // the data stored for key, if it is size bytes
    bool lookup(uint64_t key, uint32_t size, std::vector<uint8_t> &out);
    // keep data for key (copied; written by the background thread)
    void store(uint64_t key, const uint8_t *data, uint32_t size);
    size_t entries() const;

private:
    struct Entry {
        uint64_t offset;
        uint32_t size;
        uint32_t check; // XXH32 of the data, checked when it is read back
    };
    struct Job {
        uint64_t key;
        std::vector<uint8_t> data;
    };
    void writer_loop();
    void close();

    mutable std::mutex index_mutex;
    std::unordered_map<uint64_t, Entry> index;
    std::mutex read_mutex;
    std::FILE *reader = nullptr;

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Job> queue;
    size_t queued_bytes = 0;
    bool stopping = false;
    std::thread writer;
    std::FILE *pack_out = nullptr;
    std::FILE *index_out = nullptr;
    uint64_t pack_size = 0;
};

} // namespace renderer::texture
