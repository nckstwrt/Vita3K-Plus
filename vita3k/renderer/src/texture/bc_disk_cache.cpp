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

#include <renderer/texture/bc_disk_cache.h>

#include <util/log.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstring>

namespace renderer::texture {

namespace {

// pack: this header, then the data of each entry; index: this header, then one IndexRecord per entry
struct FileHeader {
    uint32_t magic = 0x31434342; // "BCC1"
    uint32_t version = 1;
    uint32_t record_size = 24;
    uint32_t reserved = 0;
};

struct IndexRecord {
    uint64_t key;
    uint64_t offset;
    uint32_t size;
    uint32_t check; // XXH32 of the data
};
static_assert(sizeof(IndexRecord) == 24);

// on a 2.5 GB handheld a game's whole converted texture set is well under this
constexpr uint64_t max_pack_bytes = 512ull << 20;
// stores are dropped while this much is waiting to be written
constexpr size_t max_queued_bytes = 32u << 20;

bool header_ok(std::FILE *f) {
    FileHeader h;
    const FileHeader expected;
    return std::fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(&h, &expected, sizeof(h)) == 0;
}

} // namespace

BcDiskCache::~BcDiskCache() {
    close();
}

void BcDiskCache::close() {
    {
        std::lock_guard<std::mutex> guard(queue_mutex);
        stopping = true;
    }
    queue_cv.notify_all();
    if (writer.joinable())
        writer.join();
    if (pack_out)
        std::fclose(pack_out);
    if (index_out)
        std::fclose(index_out);
    if (reader)
        std::fclose(reader);
    pack_out = index_out = reader = nullptr;
    std::lock_guard<std::mutex> guard(index_mutex);
    index.clear();
}

bool BcDiskCache::open(const fs::path &dir) {
    close();
    stopping = false;
    try {
        fs::create_directories(dir);
    } catch (const std::exception &) {
        return false;
    }
    const std::string pack_path = (dir / "bc-cache.pack").string();
    const std::string index_path = (dir / "bc-cache.idx").string();

    // what the files hold, when both have the current header: entries past the end of the pack are a write cut short
    bool valid = false;
    uint64_t existing_pack = 0;
    std::vector<IndexRecord> records;
    if (std::FILE *p = std::fopen(pack_path.c_str(), "rb")) {
        if (header_ok(p) && std::fseek(p, 0, SEEK_END) == 0)
            existing_pack = static_cast<uint64_t>(std::ftell(p));
        std::fclose(p);
        if (std::FILE *i = std::fopen(index_path.c_str(), "rb")) {
            if (existing_pack >= sizeof(FileHeader) && header_ok(i)) {
                valid = true;
                IndexRecord r;
                while (std::fread(&r, sizeof(r), 1, i) == 1)
                    if (r.offset >= sizeof(FileHeader) && r.offset + r.size <= existing_pack)
                        records.push_back(r);
            }
            std::fclose(i);
        }
    }
    if (!valid) {
        const FileHeader header;
        std::FILE *p = std::fopen(pack_path.c_str(), "wb");
        std::FILE *i = std::fopen(index_path.c_str(), "wb");
        if (!p || !i) {
            if (p)
                std::fclose(p);
            if (i)
                std::fclose(i);
            return false;
        }
        std::fwrite(&header, sizeof(header), 1, p);
        std::fwrite(&header, sizeof(header), 1, i);
        std::fclose(p);
        std::fclose(i);
        existing_pack = sizeof(FileHeader);
    }

    reader = std::fopen(pack_path.c_str(), "rb");
    pack_out = std::fopen(pack_path.c_str(), "ab");
    index_out = std::fopen(index_path.c_str(), "ab");
    if (!reader || !pack_out || !index_out) {
        close();
        return false;
    }
    pack_size = existing_pack;
    {
        std::lock_guard<std::mutex> guard(index_mutex);
        for (const IndexRecord &r : records)
            index[r.key] = Entry{ r.offset, r.size, r.check };
    }
    writer = std::thread(&BcDiskCache::writer_loop, this);
    LOG_INFO("[BCCACHE] {} converted texture mips on disk ({} MiB) in {}", records.size(), pack_size >> 20, dir.string());
    return true;
}

size_t BcDiskCache::entries() const {
    std::lock_guard<std::mutex> guard(index_mutex);
    return index.size();
}

bool BcDiskCache::lookup(const uint64_t key, const uint32_t size, std::vector<uint8_t> &out) {
    Entry entry;
    {
        std::lock_guard<std::mutex> guard(index_mutex);
        const auto found = index.find(key);
        if (found == index.end() || found->second.size != size)
            return false;
        entry = found->second;
    }
    out.resize(size);
    std::lock_guard<std::mutex> guard(read_mutex);
    if (!reader || std::fseek(reader, static_cast<long>(entry.offset), SEEK_SET) != 0 || std::fread(out.data(), 1, size, reader) != size)
        return false;
    // a damaged pack is a miss, never garbage in a texture
    return XXH32(out.data(), size, 0) == entry.check;
}

void BcDiskCache::store(const uint64_t key, const uint8_t *data, const uint32_t size) {
    {
        std::lock_guard<std::mutex> guard(index_mutex);
        if (index.contains(key))
            return;
    }
    std::lock_guard<std::mutex> guard(queue_mutex);
    if (stopping || !pack_out || queued_bytes + size > max_queued_bytes)
        return;
    queue.push_back(Job{ key, std::vector<uint8_t>(data, data + size) });
    queued_bytes += size;
    queue_cv.notify_one();
}

void BcDiskCache::writer_loop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this] { return stopping || !queue.empty(); });
            if (queue.empty())
                return;
            job = std::move(queue.front());
            queue.pop_front();
            queued_bytes -= job.data.size();
        }
        if (pack_size + job.data.size() > max_pack_bytes)
            continue;
        {
            std::lock_guard<std::mutex> guard(index_mutex);
            if (index.contains(job.key))
                continue;
        }
        const IndexRecord record{
            .key = job.key,
            .offset = pack_size,
            .size = static_cast<uint32_t>(job.data.size()),
            .check = XXH32(job.data.data(), job.data.size(), 0),
        };
        if (std::fwrite(job.data.data(), 1, job.data.size(), pack_out) != job.data.size() || std::fflush(pack_out) != 0)
            return;
        // the index only names data already in the pack
        if (std::fwrite(&record, sizeof(record), 1, index_out) != 1 || std::fflush(index_out) != 0)
            return;
        pack_size += job.data.size();
        std::lock_guard<std::mutex> guard(index_mutex);
        index[record.key] = Entry{ record.offset, record.size, record.check };
    }
}

} // namespace renderer::texture
