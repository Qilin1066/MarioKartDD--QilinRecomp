// Booting the game (boot.cpp).
#pragma once

#include <filesystem>
#include <span>

#include "qilin/rt/system.hpp"

namespace qilin::disc {
class Disc;  // qilin/disc/disc.hpp
}

namespace mkdd {

// Open the game's disc (an image, .iso, .gcm or .ciso, or an extracted folder
// with sys/ and files/), put its main.dol (checked against the supported
// build) into MEM1, and the boot information in low memory.
void load_game(const std::filesystem::path& disc);
// The disc load_game opened: everything the drive reads comes from it (dvd.cpp).
const qilin::disc::Disc& game_disc();

// __start, natively: registers, data, DBInit, OSInit, constructors, main, exit.
[[noreturn]] void start_game();

// Each library's replacements (os.cpp, ...).
std::span<const qilin::rt::Provided> os_replacements();
std::span<const qilin::rt::Provided> dvd_replacements();
std::span<const qilin::rt::Provided> vi_replacements();
// The console settings the game reads from SRAM (os.cpp), from the settings
// file; before the game starts. `language` is SRAM's: 0 English, 1 German,
// 2 French, 3 Spanish, 4 Italian, 5 Dutch.
void set_console(unsigned language, bool stereo, bool progressive);
std::span<const qilin::rt::Provided> pad_replacements();
std::span<const qilin::rt::Provided> card_replacements();
std::span<const qilin::rt::Provided> exi_replacements();
std::span<const qilin::rt::Provided> ar_replacements();

}  // namespace mkdd
