// SPDX-License-Identifier: GPL-3.0-or-later
// Loads the game's executable modules (NSO files from ExeFS) into guest memory,
// the same way Horizon's loader does: rtld, main, subsdk0-9, sdk, packed one
// after another from the code base.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common.h"

namespace nxrt {

struct LoadedModule {
    std::string file;  // ExeFS file name ("rtld", "main", "sdk", ...)
    u64 base = 0;
    u64 size = 0;      // page-aligned image size, including .bss
    u64 text_size = 0;
};

struct ProgramMetadata {
    u64 program_id = 0;
    u8 main_thread_priority = 44;
    u8 main_thread_core = 0;
    u32 main_thread_stack_size = 0x100000;
    bool is_64bit = true;
};

bool LoadNpdm(const std::filesystem::path& file, ProgramMetadata& out);
// Loads every module present in `exefs`; returns false on a malformed file.
bool LoadExeFs(const std::filesystem::path& exefs, u64 base, std::vector<LoadedModule>& out);

// Address of a symbol a loaded module exports (its .dynsym, found through MOD0),
// or 0. Lets the runtime find SDK entry points such as nvnBootstrapLoader in
// any game.
u64 FindExport(const LoadedModule& module, const std::string& name);

} // namespace nxrt
