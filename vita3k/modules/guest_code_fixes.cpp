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

#include <modules/guest_code_fixes.h>

#include <cpu/functions.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct GuestCodeFix {
    const char *module; // SceKernelModuleInfo::module_name
    const char *description;
    const char *pattern; // hex bytes to find; ?? matches any byte
    const char *replacement; // hex bytes written over the start of the match; ?? keeps the byte that is there
    uint32_t watch_return = 0; // where a sleep inside the patched code returns to (Thumb bit set), so long waits get reported
};

// clang-format off
constexpr GuestCodeFix fixes[] = {
    // This is for libfios2 as shipped with Resistance: Burning Skies, Sonic & All-Stars Racing Transformed, Madden NFL 13 and
    // Need for Speed Most Wanted. "ExecuteChunk" drops the scheduler lock while it dispatches a chunk. On "would block" it puts
    // the chunk back and advances the scan cursor with cursor + 1. A kick that occurs in between (e.g. a zlib job finishing
    // sets the cursor to 0 and signals a scheduler that is not waiting yet) becomes 1, so the entry at queue position 0
    // is never looked at again. i.e. A finished decompression job stays queued holding the archive buffers and every later
    // read waits behind it. Later libfios2 builds guard the increment with a kick counter. Here the cursor only advances
    // while it is still non-zero.
    {
        "SceLibFios2",
        "scheduler no longer loses a kick while re-queueing a blocked chunk",
        "41 F2 7C 00 58 F8 00 20 40 46 39 1C ?? ?? ?? ?? " // movw r0,#0x107c; ldr.w r2,[r8,r0]; mov r0,r8; adds r1,r7,#0; bl insert
        "58 F8 04 00 01 30 48 F8 04 00 02 E0 28 1C ?? ?? ?? ?? " // ldr.w r0,[r8,r4]; adds r0,#1; str.w r0,[r8,r4]; b would_block; adds r0,r5,#0; bl lock
        "1E 20 C8 F2 82 00", // would_block: movs r0,#0x1e; movt r0,#0x8082
        "08 EB 04 06 B2 68 40 46 39 1C 00 BF ?? ?? ?? ?? " // add.w r6,r8,r4; ldr r2,[r6,#8]; mov r0,r8; adds r1,r7,#0; nop; bl insert
        "30 68 10 B1 01 30 30 60 00 BF", // ldr r0,[r6]; cbz r0,b; adds r0,#1; str r0,[r6]; nop (b would_block follows)
    },
    // This is for libsceavplayer as loaded from the firmware. sceAvPlayerJumpToTime reads the player state, posts the jump
    // to the controller thread and then sleeps in 100 us steps until the state differs from what it read, for as long as
    // it takes. The controller calls the game back when a jump completes, and Killzone Mercenary holds its own lock across
    // JumpToTime while its callback takes that same lock. If the callback for one jump arrives while the next JumpToTime
    // is waiting, the controller blocks on the lock, never takes the new jump and the wait never ends. On hardware the
    // callback lands within a few ms but here the controller can run late. The wait now gives up after 1000 checks and
    // returns success, so the game releases its lock, the callback finishes and the controller takes the queued jump. The
    // state getter the loop called (r0 + 0x50) is inlined to make room for the count.
    {
        "SceAvPlayer",
        "sceAvPlayerJumpToTime no longer waits forever for the player to take the jump",
        "DB F8 B0 00 DD E9 04 67 04 F0 BD FE 90 F9 00 40 " // ldr.w r0,[fp,#0xb0]; ldrd r6,r7,[sp,#0x10]; bl state; ldrsb.w r4,[r0]
        "3B 1C DB F8 B0 00 32 1C 05 21 05 F0 10 FF " // adds r3,r7,#0; ldr.w r0,[fp,#0xb0]; adds r2,r6,#0; movs r1,#5; bl post
        "00 28 02 DA 5F F0 FF 30 0C E0 " // cmp r0,#0; bge poll; movs.w r0,#-1; b epilogue
        "DB F8 B0 00 04 F0 AB FE 90 F9 00 00 A0 42 03 D1 " // poll: ldr.w r0,[fp,#0xb0]; bl state; ldrsb.w r0,[r0]; cmp r0,r4; bne done
        "64 20 0B F0 00 E8 F3 E7 06 98", // movs r0,#100; blx sceKernelDelayThread; b poll; done: ldr r0,[sp,#0x18]
        "DB F8 B0 00 DD E9 04 67 90 F9 50 40 " // ldr.w r0,[fp,#0xb0]; ldrd r6,r7,[sp,#0x10]; ldrsb.w r4,[r0,#0x50]
        "3B 1C 32 1C 05 21 05 F0 14 FF " // adds r3,r7,#0; adds r2,r6,#0; movs r1,#5; bl post
        "00 28 0D DB 40 F2 E8 35 " // cmp r0,#0; blt fail; movw r5,#1000
        "DB F8 B0 00 90 F9 50 00 A0 42 0A D1 01 3D 08 D0 " // poll: ldr.w r0,[fp,#0xb0]; ldrsb.w r0,[r0,#0x50]; cmp r0,r4; bne done; subs r5,#1; beq done
        "64 20 0B F0 04 E8 F3 E7 " // movs r0,#100; blx sceKernelDelayThread; b poll
        "5F F0 FF 30 02 E0 00 BF 00 BF 06 98", // fail: movs.w r0,#-1; b epilogue; nop; nop; done: ldr r0,[sp,#0x18]
        0x35,
    },
};
// clang-format on

// the return address of a sleep inside a patched wait, 0 while no such fix is loaded
std::atomic<Address> g_watched_return{ 0 };

struct WatchedWait {
    uint32_t checks = 0;
    int64_t first_us = 0;
    int64_t last_us = 0;
};
std::mutex g_watch_mutex;
std::map<SceUID, WatchedWait> g_watched_waits;
uint64_t g_long_waits = 0;
uint64_t g_given_up = 0;

// -1 stands for ??
std::vector<int> parse_bytes(std::string_view text) {
    std::vector<int> bytes;
    for (size_t pos = 0; pos + 1 < text.size(); pos += 3)
        bytes.push_back(text[pos] == '?' ? -1 : std::stoi(std::string(text.substr(pos, 2)), nullptr, 16));
    return bytes;
}

} // namespace

void apply_guest_code_fixes(EmuEnvState &emuenv, const SceKernelModuleInfo &module) {
    for (const GuestCodeFix &fix : fixes) {
        if (std::strcmp(module.module_name, fix.module) != 0)
            continue;
        const std::vector<int> pattern = parse_bytes(fix.pattern);
        const std::vector<int> replacement = parse_bytes(fix.replacement);

        std::vector<Address> sites;
        for (const SceKernelSegmentInfo &segment : module.segments) {
            if ((segment.perms & 1) == 0 || segment.memsz < pattern.size())
                continue;
            std::vector<uint8_t> code(segment.memsz);
            if (!debug_safe_copy_guest(emuenv.mem, segment.vaddr.address(), code.data(), segment.memsz))
                continue;
            const auto matches = [](uint8_t byte, int expected) { return expected < 0 || byte == expected; };
            for (auto it = code.begin(); (it = std::search(it, code.end(), pattern.begin(), pattern.end(), matches)) != code.end(); ++it)
                sites.push_back(segment.vaddr.address() + static_cast<Address>(it - code.begin()));
        }
        if (sites.empty())
            continue;
        if (sites.size() > 1) {
            LOG_WARN("{}: {}: code found {} times, not applied", module.module_name, fix.description, sites.size());
            continue;
        }

        // each run of replaced bytes is written front to back and read back
        const Address site = sites.front();
        bool applied = true;
        for (size_t start = 0; start < replacement.size() && applied;) {
            if (replacement[start] < 0) {
                start++;
                continue;
            }
            size_t end = start;
            std::vector<uint8_t> run;
            while (end < replacement.size() && replacement[end] >= 0)
                run.push_back(static_cast<uint8_t>(replacement[end++]));
            std::vector<uint8_t> check(run.size());
            applied = debug_safe_write_guest(emuenv.mem, site + static_cast<Address>(start), run.data(), static_cast<uint32_t>(run.size()))
                && debug_safe_copy_guest(emuenv.mem, site + static_cast<Address>(start), check.data(), static_cast<uint32_t>(check.size()))
                && check == run;
            start = end;
        }
        emuenv.kernel.invalidate_jit_cache(site, pattern.size());
        if (applied && fix.watch_return)
            g_watched_return = site + fix.watch_return;
        if (applied)
            LOG_INFO("{}: {} (guest code fix at 0x{:08X})", module.module_name, fix.description, site);
        else
            LOG_ERROR("{}: {}: writing the guest code at 0x{:08X} failed", module.module_name, fix.description, site);
    }
}

void note_guest_delay(EmuEnvState &emuenv, const SceUID thread_id) {
    const Address watched = g_watched_return.load(std::memory_order_relaxed);
    if (!watched)
        return;
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread || thread->last_import_lr != watched)
        return;

    // a healthy wait ends once the controller picks the jump up within its 5 ms idle poll
    constexpr uint32_t LONG_WAIT = 100;
    // the patched loop sleeps at most 999 times and gives up at the check after the last one
    constexpr uint32_t LAST_SLEEP = 999;
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    const std::lock_guard<std::mutex> lock(g_watch_mutex);
    WatchedWait &wait = g_watched_waits[thread_id];
    if (now - wait.last_us > 50000) {
        if (wait.checks >= LONG_WAIT && wait.checks < LAST_SLEEP)
            LOG_INFO("[AVPLAYER] {} ({}): the player took the jump after {} checks, {} ms", thread->name, thread_id, wait.checks,
                (wait.last_us - wait.first_us) / 1000);
        wait.checks = 0;
        wait.first_us = now;
    }
    wait.last_us = now;
    wait.checks++;
    if (wait.checks != LONG_WAIT && wait.checks != LAST_SLEEP)
        return;

    // the loop keeps the state it waits to see change in r4 and reaches the player through [r11 + 0xB0]
    const uint32_t before = read_reg(*thread->cpu, 4);
    Address player = 0;
    int8_t state = -1;
    const bool read = debug_safe_copy_guest(emuenv.mem, read_reg(*thread->cpu, 11) + 0xB0, &player, sizeof(player)) && debug_safe_copy_guest(emuenv.mem, player + 0x50, &state, sizeof(state));
    const int64_t waited_ms = (now - wait.first_us) / 1000;
    if (wait.checks == LONG_WAIT) {
        g_long_waits++;
        LOG_WARN("[AVPLAYER] {} ({}) has waited {} ms in sceAvPlayerJumpToTime for the player to take the jump: state {}, still the {} it read before posting (player 0x{:08X}; long waits {}, given up {})",
            thread->name, thread_id, waited_ms, read ? state : -1, static_cast<int32_t>(before), player, g_long_waits, g_given_up);
    } else {
        g_given_up++;
        LOG_WARN("[AVPLAYER] {} ({}) stops waiting after {} ms unless the state changes by the next check: state {}, still {}; the jump stays queued and the game can release its lock (long waits {}, given up {})",
            thread->name, thread_id, waited_ms, read ? state : -1, static_cast<int32_t>(before), g_long_waits, g_given_up);
    }
}
