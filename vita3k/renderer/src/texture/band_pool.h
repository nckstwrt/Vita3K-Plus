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

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace renderer::texture {

// Splits a texture conversion (rows of PVRTC words, rows of BC blocks) into bands that run at the same
// time: the calling thread takes one band, BAND_WORKERS persistent threads the others, and run() returns
// once all are done. A game that streams compressed textures (WipEout 2048) has the renderer thread
// decode dozens of them a second; on its own it spends most of its time there.
class BandPool {
public:
    static constexpr int BAND_WORKERS = 3;

    static BandPool &get() {
        static BandPool pool;
        return pool;
    }

    // fn(lo, hi) handles [lo, hi); bands are disjoint and cover [begin, end)
    void run(const int begin, const int end, const std::function<void(int, int)> &fn) {
        const int count = end - begin;
        const int parts = std::min(count, static_cast<int>(workers.size()) + 1);
        if (parts <= 1) {
            if (count > 0)
                fn(begin, end);
            return;
        }

        const std::lock_guard<std::mutex> one_caller(run_mutex);
        {
            const std::lock_guard<std::mutex> lock(mutex);
            job = &fn;
            job_begin = begin;
            job_count = count;
            job_parts = parts;
            next_part = 1;
            pending = parts - 1;
            generation++;
        }
        cond.notify_all();

        run_part(0);
        std::unique_lock<std::mutex> lock(mutex);
        while (next_part < job_parts) {
            const int part = next_part++;
            lock.unlock();
            run_part(part);
            lock.lock();
            pending--;
        }
        done_cond.wait(lock, [&] { return pending == 0; });
        job = nullptr;
    }

private:
    BandPool() {
        for (int i = 0; i < BAND_WORKERS; i++)
            workers.emplace_back([this] { worker(); });
    }

    ~BandPool() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        cond.notify_all();
        for (std::thread &t : workers)
            t.join();
    }

    void run_part(const int part) const {
        const int lo = job_begin + static_cast<int>(static_cast<int64_t>(job_count) * part / job_parts);
        const int hi = job_begin + static_cast<int>(static_cast<int64_t>(job_count) * (part + 1) / job_parts);
        (*job)(lo, hi);
    }

    void worker() {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            cond.wait(lock, [&] { return stop || (generation != seen && next_part < job_parts); });
            if (stop)
                return;
            seen = generation;
            while (next_part < job_parts) {
                const int part = next_part++;
                lock.unlock();
                run_part(part);
                lock.lock();
                if (--pending == 0)
                    done_cond.notify_one();
            }
        }
    }

    std::vector<std::thread> workers;
    std::mutex run_mutex;
    std::mutex mutex;
    std::condition_variable cond, done_cond;
    const std::function<void(int, int)> *job = nullptr;
    int job_begin = 0, job_count = 0, job_parts = 0, next_part = 0, pending = 0;
    uint64_t generation = 0;
    bool stop = false;
};

} // namespace renderer::texture
