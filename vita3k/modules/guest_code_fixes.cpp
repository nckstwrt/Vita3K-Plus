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

#include <emuenv/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <util/log.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct GuestCodeFix {
    const char *module; // SceKernelModuleInfo::module_name
    const char *description;
    const char *pattern; // hex bytes to find; ?? matches any byte
    const char *replacement; // hex bytes written over the start of the match; ?? keeps the byte that is there
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
};
// clang-format on

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
        if (applied)
            LOG_INFO("{}: {} (guest code fix at 0x{:08X})", module.module_name, fix.description, site);
        else
            LOG_ERROR("{}: {}: writing the guest code at 0x{:08X} failed", module.module_name, fix.description, site);
    }
}
