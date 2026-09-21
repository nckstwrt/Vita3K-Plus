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

#include <kernel/types.h>

struct EmuEnvState;

// Repairs known defects in Sony libraries, whether a game ships them in its own sce_module folder or they load from the
// firmware. Each fix is found by the library's exact code when the module loads (never by title)
void apply_guest_code_fixes(EmuEnvState &emuenv, const SceKernelModuleInfo &module);

// called by sceKernelDelayThread and reports a thread whose sleeps inside a patched wait runs long
void note_guest_delay(EmuEnvState &emuenv, SceUID thread_id);
