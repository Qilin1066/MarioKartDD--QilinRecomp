// Booting the game (boot.cpp).
#pragma once

#include <filesystem>
#include <span>

#include "qilin/rt/system.hpp"

namespace mkdd {

// main.dol (checked against the supported build) into MEM1, and the boot
// information in low memory. `folder` holds the extracted disc: sys/, files/.
void load_game(const std::filesystem::path& folder);

// __start, natively: registers, data, DBInit, OSInit, constructors, main, exit.
[[noreturn]] void start_game();

// Each library's replacements (os.cpp, ...).
std::span<const qilin::rt::Provided> os_replacements();
std::span<const qilin::rt::Provided> dvd_replacements();
std::span<const qilin::rt::Provided> vi_replacements();
// For libraries whose replacements aren't written yet (stubs_tmp.cpp: to be deleted).
std::span<const qilin::rt::Provided> temporary_stubs();

// The console settings the game reads from SRAM (os.cpp), from the settings
// file; before the game starts. `language` is SRAM's: 0 English, 1 German,
// 2 French, 3 Spanish, 4 Italian, 5 Dutch.
void set_console(unsigned language, bool stereo, bool progressive);

}  // namespace mkdd
