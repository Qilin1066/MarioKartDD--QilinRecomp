// The memory card slots (card.cpp), for what else asks about them.
#pragma once

#include <cstdint>
#include <filesystem>

namespace mkdd {

// A card's size: the 251-block card (16 megabits in 8 KiB sectors, five of
// them the card's own).
inline constexpr uint32_t kCardMegabits = 16, kCardSectorSize = 8192, kCardBlocks = 251;

// Whether slot 0 (A) or 1 (B) holds a card.
bool card_present(uint32_t slot);
// The folder that is the card in a slot: <user folder>/card-a, card-b.
std::filesystem::path card_folder(uint32_t slot);

}  // namespace mkdd
