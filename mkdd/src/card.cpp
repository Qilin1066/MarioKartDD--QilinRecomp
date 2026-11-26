// The memory card library's replacements (dolphin/card): the cards in slots A
// and B, each a folder under this port's user folder (host/paths.hpp):
// <user folder>/card-a and card-b. Slot A always holds a card (its folder is
// made at the first start); slot B holds one while a card-b folder exists.
//
// A card is the 251-block one: 16 megabits in 8 KiB sectors, five of them the
// card's own, 127 files at most. The game takes any card with 8 KiB sectors;
// its own file is 3 blocks and each ghost 5, so this is the size with room
// for both, and the commonest card of its day.
//
// Each file on the card is one host file in the standard .gci form, so it can
// be given to an emulator or a card manager as it is, and one from there can
// be dropped into the folder: the file's 64-byte directory entry as the card
// stores it (below), then its blocks. The host file's name is the game code,
// the maker code and the card file's name (bytes a host name can't hold as
// %XX), but only the entry inside says what the file is.
//
// Every change is written whole to a copy beside the file, flushed, and
// swapped in with one rename, so a crash or power cut leaves the save as it
// was. The first change after a file is opened, and a delete or format, first
// keep the old file in the save history (save_history.hpp). Copies left by a
// stop mid-write are deleted when the card is next looked at.
//
// The API is the SDK's, results and all. A call that would use the card
// takes the time a card takes (rough figures, below): the synchronous forms,
// which are what the game uses from its own card thread, sleep the calling
// thread; the asynchronous ones return and call back as an interrupt. The
// host file is read or written when the call is made.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "boot.hpp"
#include "card.hpp"
#include "later.hpp"
#include "os.hpp"
#include "save_history.hpp"
#include "threads.hpp"
#include "qilin/host/paths.hpp"
#include "qilin/rt/file.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;
namespace fs = std::filesystem;

namespace {

// CARD_RESULT_* (card.h).
constexpr int32_t kReady = 0, kBusy = -1, kNoCard = -3, kNoFile = -4, kIoError = -5, kExist = -7, kNoEnt = -8,
                  kInsSpace = -9, kNoPerm = -10, kLimit = -11, kNameTooLong = -12, kFatal = -128;
constexpr uint32_t kSlots = 2, kMaxFiles = 127, kNameLength = 32, kSegment = 512;
constexpr uint32_t kOSGetTime = 0x800EC33Cu;
constexpr uint32_t kDiscId = 0x80000000u;  // the disc's game code (4 bytes) and maker code (2)
constexpr uint32_t kWaitKey = 0x00CA4D00u;  // + slot: where a synchronous call's thread sleeps (threads.hpp)

// A file's directory entry, as the card and a .gci file hold it (big-endian).
constexpr size_t kEntrySize = 64;
constexpr size_t kGame = 0x00, kBannerFormat = 0x07, kName = 0x08, kTime = 0x28, kIconAddr = 0x2C, kIconFormat = 0x30,
                 kIconSpeed = 0x32, kPermission = 0x34, kFirstBlock = 0x36, kBlocks = 0x38,  // (0x35: how often it was copied)
                 kCommentAddr = 0x3C;
constexpr uint8_t kPublic = 0x04;  // CARD_ATTR_PUBLIC
constexpr const char* kSuffix = ".gci";
constexpr const char* kTempSuffix = ".tmp";

// CARDStat (0x6C bytes): what CARDGetStatus fills and CARDSetStatus takes.
constexpr uint32_t kStatLength = 0x20, kStatTime = 0x24, kStatGame = 0x28, kStatBannerFormat = 0x2E, kStatIconAddr = 0x30,
                   kStatIconFormat = 0x34, kStatIconSpeed = 0x36, kStatCommentAddr = 0x38, kStatOffsetBanner = 0x3C,
                   kStatOffsetBannerTlut = 0x40, kStatOffsetIcon = 0x44, kStatOffsetIconTlut = 0x64, kStatOffsetData = 0x68;
// CARDFileInfo: chan, fileNo, offset, length, iBlock.
constexpr uint32_t kInfoChan = 0, kInfoFile = 4, kInfoOffset = 8, kInfoLength = 12, kInfoBlock = 16;

// How long the card takes. Rough: a sector is read in about 5 ms; writing one
// (an erase, then 64 pages) takes about 150 ms, and so does each of the
// card's own sectors a change rewrites (the directory, the block map);
// mounting unlocks the card and reads its five system sectors. Not measured.
uint64_t ms(uint64_t n) { return ticks_from_us(n * 1000); }
uint64_t read_ticks(uint32_t bytes) { return ms(1) + ticks_from_us(uint64_t{bytes} * 5000 / kCardSectorSize); }
uint64_t write_ticks(uint32_t sectors) { return ms(150) * sectors; }
uint64_t mount_ticks() { return ms(300); }

struct Entry {
    fs::path host;
    std::array<uint8_t, kEntrySize> e{};
    bool kept = false;  // its old contents are in the save history since it was last opened
};
struct Card {
    bool mounted = false, busy = false;
    uint64_t mount_ends = 0;  // while it mounts, a probe says BUSY; busy with anything else, it answers
    uint32_t detach = 0;
    std::array<std::optional<Entry>, kMaxFiles> dir;
};
std::array<Card, kSlots> g_cards;
fs::path g_user;

uint16_t get16(const Entry& f, size_t at) { return static_cast<uint16_t>(f.e[at] << 8 | f.e[at + 1]); }
uint32_t get32(const Entry& f, size_t at) { return uint32_t{get16(f, at)} << 16 | get16(f, at + 2); }
void put16(Entry& f, size_t at, uint16_t v) { f.e[at] = static_cast<uint8_t>(v >> 8), f.e[at + 1] = static_cast<uint8_t>(v); }
void put32(Entry& f, size_t at, uint32_t v) { put16(f, at, static_cast<uint16_t>(v >> 16)), put16(f, at + 2, static_cast<uint16_t>(v)); }
uint32_t blocks_of(const Entry& f) { return get16(f, kBlocks); }

const fs::path& user() {
    if (g_user.empty()) g_user = qilin::host::user_folder();
    return g_user;
}

FILE* open_file(const fs::path& p, const char* mode) {
#if defined(_WIN32)
    const std::wstring wide(mode, mode + std::strlen(mode));
    return _wfopen(p.c_str(), wide.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}
bool read_whole(const fs::path& p, std::vector<uint8_t>& out) {
    FILE* f = open_file(p, "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
    const bool ok = !std::ferror(f);
    return std::fclose(f) == 0 && ok;
}
// `image` becomes the file at `target`: complete, or not at all.
bool replace_file(const fs::path& target, const std::vector<uint8_t>& image) {
    fs::path temporary = target;
    temporary += kTempSuffix;
    std::error_code ec;
    FILE* f = open_file(temporary, "wb");
    if (!f) return false;
    bool ok = std::fwrite(image.data(), 1, image.size(), f) == image.size();
    ok = rt::sync_file(f) && ok;
    ok = std::fclose(f) == 0 && ok;
    if (ok) fs::rename(temporary, target, ec);
    if (!ok || ec) {
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

// The disc's game and maker codes: the files that are this game's.
bool own(const Entry& f) { return std::memcmp(f.e.data(), rt::guest_ptr(kDiscId), 6) == 0; }
bool name_is(const Entry& f, const std::string& name) {
    char padded[kNameLength] = {};
    std::memcpy(padded, name.data(), std::min<size_t>(name.size(), kNameLength));
    return std::memcmp(f.e.data() + kName, padded, kNameLength) == 0;
}
std::string host_name(const Entry& f) {
    std::string raw(reinterpret_cast<const char*>(f.e.data()), 6);
    raw += '-';
    for (size_t i = 0; i < kNameLength && f.e[kName + i]; ++i) raw += static_cast<char>(f.e[kName + i]);
    std::string out;
    for (const char ch : raw) {
        const auto u = static_cast<unsigned char>(ch);
        const bool plain = (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                           std::strchr(" !#$&'()+,-.;=@[]^_`{}~", ch) != nullptr;
        if (plain && ch != 0) {
            out += ch;
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", u);
            out += hex;
        }
    }
    return out;
}

// What is in the folder now, as the card's directory. A file known from
// before keeps its number; a new one takes the lowest free.
void scan(uint32_t slot) {
    Card& k = g_cards[slot];
    const fs::path folder = card_folder(slot);
    std::error_code ec;
    std::vector<fs::path> found;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() == kTempSuffix) {
            rt::log("saves: a write to %s was cut short last time: the file before it is kept",
                    it->path().stem().string().c_str());
            std::error_code gone;
            fs::remove(it->path(), gone);
        } else if (it->path().extension() == kSuffix) {
            found.push_back(it->path());
        }
    }
    std::sort(found.begin(), found.end());
    for (auto& f : k.dir)
        if (f && std::find(found.begin(), found.end(), f->host) == found.end()) f.reset();
    uint32_t used = 0;
    for (const auto& f : k.dir)
        if (f) used += blocks_of(*f);
    for (const fs::path& p : found) {
        if (std::any_of(k.dir.begin(), k.dir.end(), [&](const auto& f) { return f && f->host == p; })) continue;
        Entry f;
        f.host = p;
        FILE* in = open_file(p, "rb");
        const bool read = in && std::fread(f.e.data(), 1, kEntrySize, in) == kEntrySize;
        if (in) std::fclose(in);
        const uintmax_t size = fs::file_size(p, ec);
        const uint32_t blocks = blocks_of(f);
        if (!read || ec || f.e[kGame] == 0xFF || f.e[kGame] == 0 || blocks == 0 ||
            size != kEntrySize + uintmax_t{blocks} * kCardSectorSize) {
            rt::log("saves: %s isn't a card file (.gci: a 64-byte entry and its 8 KiB blocks): left alone", p.string().c_str());
            continue;
        }
        const auto free = std::find(k.dir.begin(), k.dir.end(), std::nullopt);
        if (free == k.dir.end() || used + blocks > kCardBlocks) {
            rt::log("saves: %s doesn't fit on the card in slot %c (251 blocks, 127 files): left out", p.string().c_str(), 'A' + slot);
            continue;
        }
        used += blocks;
        *free = std::move(f);
    }
}

uint32_t free_blocks(const Card& k) {
    uint32_t used = 0;
    for (const auto& f : k.dir)
        if (f) used += blocks_of(*f);
    return kCardBlocks - used;
}

// The OS's clock in seconds, as the card stamps its files (from 2000).
uint32_t now_seconds(Cpu& c) {
    rt::call(c, kOSGetTime);
    return static_cast<uint32_t>((uint64_t{c.r[3]} << 32 | c.r[4]) / kTimerHz);
}

std::string guest_string(uint32_t a) { return reinterpret_cast<const char*>(rt::guest_ptr(a)); }

// The old file into the save history, once per opening.
bool keep(Entry& f) {
    if (f.kept) return true;
    std::string error;
    if (!backup_save(user(), f.host, error)) {
        rt::log("saves: %s", error.c_str());
        return false;
    }
    return f.kept = true;
}
// The file with a new entry and, from `data`, new contents at `offset`.
bool rewrite(Entry& f, const std::array<uint8_t, kEntrySize>& entry, const uint8_t* data = nullptr, uint32_t offset = 0,
             uint32_t length = 0) {
    std::vector<uint8_t> image;
    if (!read_whole(f.host, image) || image.size() != kEntrySize + size_t{blocks_of(f)} * kCardSectorSize) {
        rt::log("saves: %s can't be read back", f.host.string().c_str());
        return false;
    }
    std::memcpy(image.data(), entry.data(), kEntrySize);
    if (length) std::memcpy(image.data() + kEntrySize + offset, data, length);
    if (!replace_file(f.host, image)) {
        rt::log("saves: %s couldn't be written: the one before it is kept", f.host.string().c_str());
        return false;
    }
    f.e = entry;
    return true;
}

// ---- how a call ends: at once with `result` (a refusal, or something that
// needs no transfer), or after the card has worked for `ticks`.
struct Outcome {
    int32_t result;
    uint64_t ticks = 0;
    bool transfer = false;
};
Outcome refuse(int32_t result) { return {result}; }
Outcome after(uint64_t ticks, int32_t result = kReady) { return {result, ticks, true}; }

void detach(uint32_t slot) {
    Card& k = g_cards[slot];
    k.mounted = false;
    for (auto& f : k.dir) f.reset();
    if (const uint32_t cb = std::exchange(k.detach, 0))
        post_interrupt([cb, slot](Cpu& c) { guest_callback(c, cb, slot, static_cast<uint32_t>(kNoCard)); });
}
// The mounted card in a slot for a call, or why not.
int32_t card_for(uint32_t slot, Card*& out) {
    if (slot >= kSlots) return kFatal;
    Card& k = g_cards[slot];
    if (k.mounted && !card_present(slot)) detach(slot);  // pulled out: its folder is gone
    if (!k.mounted) return kNoCard;
    if (k.busy) return kBusy;
    out = &k;
    return kReady;
}
// The file a CARDFileInfo names.
int32_t file_for(uint32_t info, Card*& k, Entry*& f) {
    const uint32_t slot = rt::peek32(info + kInfoChan), no = rt::peek32(info + kInfoFile);
    if (slot >= kSlots || no >= kMaxFiles) return kFatal;
    if (const int32_t r = card_for(slot, k); r < 0) return r;
    if (!k->dir[no]) return kNoFile;
    f = &*k->dir[no];
    return kReady;
}
int32_t readable(const Entry& f) { return own(f) || (f.e[kPermission] & kPublic) ? kReady : kNoPerm; }
int32_t writable(const Entry& f) { return own(f) ? kReady : kNoPerm; }
int32_t find(Card& k, const std::string& name) {
    for (uint32_t i = 0; i < kMaxFiles; ++i)
        if (k.dir[i] && own(*k.dir[i]) && name_is(*k.dir[i], name)) return static_cast<int32_t>(i);
    return -1;
}
void fill_info(uint32_t info, uint32_t slot, uint32_t no, const Entry& f) {
    rt::poke32(info + kInfoChan, slot);
    rt::poke32(info + kInfoFile, no);
    rt::poke32(info + kInfoOffset, 0);
    rt::poke32(info + kInfoLength, blocks_of(f) * kCardSectorSize);
    rt::poke16(info + kInfoBlock, get16(f, kFirstBlock));
}

// ---- the operations
Outcome do_probe(uint32_t slot, uint32_t mem_size, uint32_t sector_size) {
    if (slot >= kSlots) return refuse(kFatal);
    if (!card_present(slot)) {
        if (g_cards[slot].mounted) detach(slot);
        return refuse(kNoCard);
    }
    if (g_cards[slot].busy && time_base() < g_cards[slot].mount_ends) return refuse(kBusy);
    if (mem_size) rt::poke32(mem_size, kCardMegabits);
    if (sector_size) rt::poke32(sector_size, kCardSectorSize);
    return refuse(kReady);
}
Outcome do_mount(uint32_t slot, uint32_t detach_cb) {
    if (slot >= kSlots) return refuse(kFatal);
    Card& k = g_cards[slot];
    if (k.busy) return refuse(kBusy);
    if (!card_present(slot)) {
        if (k.mounted) detach(slot);
        return refuse(kNoCard);
    }
    scan(slot);
    k.mounted = true;
    k.detach = detach_cb;
    k.mount_ends = time_base() + mount_ticks();
    return after(mount_ticks());
}
Outcome do_unmount(uint32_t slot) {
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    k->mounted = false;
    k->detach = 0;
    return refuse(kReady);
}
Outcome do_check(uint32_t slot, uint32_t transferred) {
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    if (transferred) rt::poke32(transferred, 0);
    return after(0);  // the directory and block map are sound: nothing to repair, nothing transferred
}
Outcome do_free_blocks(uint32_t slot, uint32_t bytes, uint32_t files) {
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    if (bytes) rt::poke32(bytes, free_blocks(*k) * kCardSectorSize);
    if (files) rt::poke32(files, static_cast<uint32_t>(std::count(k->dir.begin(), k->dir.end(), std::nullopt)));
    return refuse(kReady);
}
Outcome do_format(uint32_t slot) {
    if (slot >= kSlots) return refuse(kFatal);
    Card& k = g_cards[slot];
    if (k.mounted && !card_present(slot)) detach(slot);
    if (!k.mounted) return refuse(kNoCard);
    if (k.busy) return refuse(kBusy);
    int32_t result = kReady;
    for (auto& f : k.dir) {
        if (!f) continue;
        std::error_code ec;
        f->kept = false;  // what it holds now is what is lost
        if (!keep(*f) || !fs::remove(f->host, ec) || ec) {
            rt::log("saves: %s couldn't be removed to format the card: it stays", f->host.string().c_str());
            result = kIoError;
            continue;
        }
        f.reset();
    }
    return after(write_ticks(5), result);
}
Outcome do_open(uint32_t slot, const std::string& name, uint32_t info) {
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    const int32_t no = find(*k, name);
    if (no < 0) return refuse(kNoFile);
    Entry& f = *k->dir[static_cast<uint32_t>(no)];
    f.kept = false;
    fill_info(info, slot, static_cast<uint32_t>(no), f);
    return refuse(kReady);
}
Outcome do_fast_open(uint32_t slot, uint32_t no, uint32_t info) {
    if (no >= kMaxFiles) return refuse(kFatal);
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    if (!k->dir[no]) return refuse(kNoFile);
    Entry& f = *k->dir[no];
    if (const int32_t r = readable(f); r < 0) return refuse(r);
    f.kept = false;
    fill_info(info, slot, no, f);
    return refuse(kReady);
}
Outcome do_close(uint32_t info) {
    Card* k = nullptr;
    if (const int32_t r = card_for(rt::peek32(info + kInfoChan), k); r < 0) return refuse(r);
    rt::poke32(info + kInfoChan, 0xFFFFFFFFu);
    return refuse(kReady);
}
Outcome do_create(Cpu& c, uint32_t slot, const std::string& name, uint32_t size, uint32_t info) {
    if (name.size() > kNameLength) return refuse(kNameTooLong);
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    if (size == 0 || size % kCardSectorSize != 0) return refuse(kFatal);
    if (find(*k, name) >= 0) return refuse(kExist);
    const auto free = std::find(k->dir.begin(), k->dir.end(), std::nullopt);
    if (free == k->dir.end()) return refuse(kNoEnt);
    if (uint64_t{free_blocks(*k)} * kCardSectorSize < size) return refuse(kInsSpace);
    // As the SDK makes a new file: public, no banner or icon yet, no comment.
    Entry f;
    std::memcpy(f.e.data(), rt::guest_ptr(kDiscId), 6);
    f.e[0x06] = 0xFF;
    std::memcpy(f.e.data() + kName, name.data(), name.size());
    put32(f, kTime, now_seconds(c));
    put32(f, kIconAddr, 0xFFFFFFFFu);
    put16(f, kIconSpeed, 1);
    f.e[kPermission] = kPublic;
    put16(f, kFirstBlock, 5);  // the first block past the card's own; a .gci's reader places the file itself
    put16(f, kBlocks, static_cast<uint16_t>(size / kCardSectorSize));
    put16(f, 0x3A, 0xFFFF);
    put32(f, kCommentAddr, 0xFFFFFFFFu);
    const fs::path folder = card_folder(slot);
    const std::string base = host_name(f);
    std::error_code ec;
    f.host = folder / (base + kSuffix);
    for (unsigned n = 2; fs::exists(f.host, ec); ++n) f.host = folder / (base + "~" + std::to_string(n) + kSuffix);
    f.kept = true;  // nothing before it to keep
    std::vector<uint8_t> image(kEntrySize + size, 0xFF);  // its blocks as a formatted card's: erased
    std::memcpy(image.data(), f.e.data(), kEntrySize);
    if (!replace_file(f.host, image)) {
        rt::log("saves: %s couldn't be created", f.host.string().c_str());
        return after(write_ticks(1), kIoError);
    }
    const auto no = static_cast<uint32_t>(free - k->dir.begin());
    fill_info(info, slot, no, f);
    *free = std::move(f);
    return after(write_ticks(2));  // the block map, then the directory
}
// CARDRead works in 512-byte segments, CARDWrite in whole sectors.
Outcome do_read(uint32_t info, uint32_t buffer, uint32_t length, uint32_t offset) {
    if (offset % kSegment || length % kSegment) return refuse(kFatal);
    Card* k = nullptr;
    Entry* f = nullptr;
    if (const int32_t r = file_for(info, k, f); r < 0) return refuse(r);
    const uint32_t size = blocks_of(*f) * kCardSectorSize;
    if (size <= offset || size < uint64_t{offset} + length) return refuse(kLimit);
    if (const int32_t r = readable(*f); r < 0) return refuse(r);
    FILE* in = open_file(f->host, "rb");
    const bool ok = in && rt::seek_file(in, static_cast<int64_t>(kEntrySize + offset), SEEK_SET) &&
                    std::fread(rt::guest_ptr(buffer), 1, length, in) == length;
    if (in) std::fclose(in);
    if (!ok) {
        rt::log("saves: %s can't be read", f->host.string().c_str());
        return after(read_ticks(length), kIoError);
    }
    rt::poke32(info + kInfoOffset, offset + length);
    return after(read_ticks(length));
}
Outcome do_write(Cpu& c, uint32_t info, uint32_t buffer, uint32_t length, uint32_t offset) {
    if (offset % kCardSectorSize || length % kCardSectorSize) return refuse(kFatal);
    Card* k = nullptr;
    Entry* f = nullptr;
    if (const int32_t r = file_for(info, k, f); r < 0) return refuse(r);
    const uint32_t size = blocks_of(*f) * kCardSectorSize;
    if (size <= offset || size < uint64_t{offset} + length) return refuse(kLimit);
    if (const int32_t r = writable(*f); r < 0) return refuse(r);
    const uint64_t ticks = write_ticks(length / kCardSectorSize + 1);  // its sectors, then the directory (the time)
    Entry stamped;
    stamped.e = f->e;
    put32(stamped, kTime, now_seconds(c));
    if (!keep(*f) || !rewrite(*f, stamped.e, rt::guest_ptr(buffer), offset, length)) return after(ticks, kIoError);
    rt::poke32(info + kInfoOffset, offset + length);
    return after(ticks);
}
Outcome do_delete(Card& k, uint32_t no) {
    Entry& f = *k.dir[no];
    if (const int32_t r = writable(f); r < 0) return refuse(r);
    f.kept = false;
    std::error_code ec;
    if (!keep(f) || !fs::remove(f.host, ec) || ec) {
        rt::log("saves: %s couldn't be deleted", f.host.string().c_str());
        return after(write_ticks(2), kIoError);
    }
    k.dir[no].reset();
    return after(write_ticks(2));  // the directory, then the block map
}
Outcome do_delete_name(uint32_t slot, const std::string& name) {
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    const int32_t no = find(*k, name);
    return no < 0 ? refuse(kNoFile) : do_delete(*k, static_cast<uint32_t>(no));
}
Outcome do_delete_number(uint32_t slot, uint32_t no) {
    if (no >= kMaxFiles) return refuse(kFatal);
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    return !k->dir[no] ? refuse(kNoFile) : do_delete(*k, no);
}

// Where a file's banner, icons and their palettes lie in its data, from the
// formats: an 8-bit banner is 96x32 bytes and a 256-colour palette, a 16-bit
// one 96x32 words; an icon 32x32 bytes or words; the 8-bit icons share one
// palette after the last icon.
void write_offsets(uint32_t stat) {
    uint32_t at = rt::peek32(stat + kStatIconAddr);
    if (at == 0xFFFFFFFFu) {
        rt::poke8(stat + kStatBannerFormat, 0);
        rt::poke16(stat + kStatIconFormat, 0);
        rt::poke16(stat + kStatIconSpeed, 0);
        at = 0;
    }
    const uint32_t none = 0xFFFFFFFFu;
    switch (*rt::guest_ptr(stat + kStatBannerFormat) & 3) {
    case 1:
        rt::poke32(stat + kStatOffsetBanner, at);
        rt::poke32(stat + kStatOffsetBannerTlut, at + 3072);
        at += 3072 + 512;
        break;
    case 2:
        rt::poke32(stat + kStatOffsetBanner, at);
        rt::poke32(stat + kStatOffsetBannerTlut, none);
        at += 6144;
        break;
    default:
        rt::poke32(stat + kStatOffsetBanner, none);
        rt::poke32(stat + kStatOffsetBannerTlut, none);
        break;
    }
    bool palette = false;
    const uint32_t formats = rt::peek16(stat + kStatIconFormat);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t format = formats >> (2 * i) & 3;
        rt::poke32(stat + kStatOffsetIcon + 4 * i, format == 1 || format == 2 ? at : none);
        if (format == 1) at += 1024, palette = true;
        if (format == 2) at += 2048;
    }
    rt::poke32(stat + kStatOffsetIconTlut, palette ? at : none);
    if (palette) at += 512;
    rt::poke32(stat + kStatOffsetData, at);
}
Outcome do_get_status(uint32_t slot, uint32_t no, uint32_t stat) {
    if (no >= kMaxFiles) return refuse(kFatal);
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    if (!k->dir[no]) return refuse(kNoFile);
    const Entry& f = *k->dir[no];
    if (const int32_t r = readable(f); r < 0) return refuse(r);
    std::memcpy(rt::guest_ptr(stat), f.e.data() + kName, kNameLength);
    rt::poke32(stat + kStatLength, blocks_of(f) * kCardSectorSize);
    rt::poke32(stat + kStatTime, get32(f, kTime));
    std::memcpy(rt::guest_ptr(stat + kStatGame), f.e.data(), 6);
    rt::poke8(stat + kStatBannerFormat, f.e[kBannerFormat]);
    rt::poke32(stat + kStatIconAddr, get32(f, kIconAddr));
    rt::poke16(stat + kStatIconFormat, get16(f, kIconFormat));
    rt::poke16(stat + kStatIconSpeed, get16(f, kIconSpeed));
    rt::poke32(stat + kStatCommentAddr, get32(f, kCommentAddr));
    write_offsets(stat);
    return refuse(kReady);
}
Outcome do_set_status(Cpu& c, uint32_t slot, uint32_t no, uint32_t stat) {
    const uint32_t icon = rt::peek32(stat + kStatIconAddr), comment = rt::peek32(stat + kStatCommentAddr);
    if (no >= kMaxFiles || (icon != 0xFFFFFFFFu && icon >= 512) ||
        (comment != 0xFFFFFFFFu && comment % kCardSectorSize > kCardSectorSize - 64))
        return refuse(kFatal);
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    if (!k->dir[no]) return refuse(kNoFile);
    Entry& f = *k->dir[no];
    if (const int32_t r = writable(f); r < 0) return refuse(r);
    Entry next;
    next.e = f.e;
    next.e[kBannerFormat] = *rt::guest_ptr(stat + kStatBannerFormat);
    put32(next, kIconAddr, icon);
    put16(next, kIconFormat, rt::peek16(stat + kStatIconFormat));
    put16(next, kIconSpeed, rt::peek16(stat + kStatIconSpeed));
    put32(next, kCommentAddr, comment);
    write_offsets(stat);
    if (icon == 0xFFFFFFFFu) put16(next, kIconSpeed, static_cast<uint16_t>((get16(next, kIconSpeed) & ~3u) | 1));
    put32(next, kTime, now_seconds(c));
    if (!keep(f) || !rewrite(f, next.e)) return after(write_ticks(1), kIoError);
    return after(write_ticks(1));
}
Outcome do_rename(Cpu& c, uint32_t slot, uint32_t old_at, uint32_t new_at) {
    const uint8_t o = *rt::guest_ptr(old_at), n = *rt::guest_ptr(new_at);
    if (o == 0xFF || n == 0xFF || o == 0 || n == 0) return refuse(kFatal);
    const std::string old_name = guest_string(old_at), new_name = guest_string(new_at);
    if (old_name.size() > kNameLength || new_name.size() > kNameLength) return refuse(kNameTooLong);
    Card* k = nullptr;
    if (const int32_t r = card_for(slot, k); r < 0) return refuse(r);
    const int32_t no = find(*k, old_name);
    if (no < 0) return refuse(kNoFile);
    if (find(*k, new_name) >= 0) return refuse(kExist);
    Entry& f = *k->dir[static_cast<uint32_t>(no)];
    Entry next;
    next.e = f.e;
    std::memset(next.e.data() + kName, 0, kNameLength);
    std::memcpy(next.e.data() + kName, new_name.data(), new_name.size());
    put32(next, kTime, now_seconds(c));
    if (!rewrite(f, next.e)) return after(write_ticks(1), kIoError);
    // The host file's name follows; it is only a label, so failing to change it loses nothing.
    const fs::path to = card_folder(slot) / (host_name(f) + kSuffix);
    std::error_code ec;
    if (!fs::exists(to, ec)) {
        fs::rename(f.host, to, ec);
        if (!ec) f.host = to;
    }
    return after(write_ticks(1));
}

// ---- ending a call
void finish(Cpu& c, uint32_t slot, const Outcome& o, const char* who) {
    if (devices_log()) rt::log("card: %s (slot %u) = %d%s", who, slot, o.result, o.transfer && o.ticks ? ", after the card's time" : "");
    if (!o.transfer || o.ticks == 0) {
        c.r[3] = static_cast<uint32_t>(o.result);
        return;
    }
    if (in_interrupt()) rt::fatal(&c, "%s from an interrupt: it would wait for the card there for ever", who);
    Card& k = g_cards[slot];
    k.busy = true;
    run_after(o.ticks, [slot](Cpu& cc) {
        g_cards[slot].busy = false;
        wake_all(cc, kWaitKey + slot);
    });
    while (k.busy) wait_on(c, kWaitKey + slot);
    c.r[3] = static_cast<uint32_t>(o.result);
}
// The asynchronous forms: a refusal is returned; otherwise READY, and the
// result goes to callback(slot, result) when the card is done.
void finish_async(Cpu& c, uint32_t slot, const Outcome& o, uint32_t callback) {
    if (devices_log()) rt::log("card: an asynchronous call (slot %u) = %d%s", slot, o.result, o.transfer ? ", by callback" : "");
    if (!o.transfer) {
        c.r[3] = static_cast<uint32_t>(o.result);
        return;
    }
    g_cards[slot].busy = true;
    const int32_t result = o.result;
    run_after(o.ticks, [slot, result, callback](Cpu& cc) {
        g_cards[slot].busy = false;
        wake_all(cc, kWaitKey + slot);
        if (callback) guest_callback(cc, callback, slot, static_cast<uint32_t>(result));
    });
    c.r[3] = static_cast<uint32_t>(kReady);
}
uint32_t info_slot(uint32_t info) { return std::min(rt::peek32(info + kInfoChan), kSlots - 1); }  // for an outcome that waits: a valid one

// ---- the API
void CARDInit(Cpu&) {
    std::error_code ec;
    fs::create_directories(card_folder(0), ec);
    if (ec) rt::fatal(nullptr, "can't create the memory card's folder %s: %s", card_folder(0).string().c_str(), ec.message().c_str());
    rt::log("saves: memory card A is %s%s", card_folder(0).string().c_str(),
            card_present(1) ? ", and there is a card B beside it" : "");
}
void CARDProbeEx(Cpu& c) { finish(c, c.r[3], do_probe(c.r[3], c.r[4], c.r[5]), "CARDProbeEx"); }  // (slot, memSize*, sectorSize*)
void CARDMount(Cpu& c) { finish(c, c.r[3], do_mount(c.r[3], c.r[5]), "CARDMount"); }             // (slot, workArea, detach)
void CARDMountAsync(Cpu& c) { finish_async(c, c.r[3], do_mount(c.r[3], c.r[5]), c.r[6]); }       // (..., attach)
void CARDUnmount(Cpu& c) { finish(c, c.r[3], do_unmount(c.r[3]), "CARDUnmount"); }
void CARDCheck(Cpu& c) { finish(c, c.r[3], do_check(c.r[3], 0), "CARDCheck"); }
void CARDCheckExAsync(Cpu& c) { finish_async(c, c.r[3], do_check(c.r[3], c.r[4]), c.r[5]); }     // (slot, transferred*, cb)
void CARDFreeBlocks(Cpu& c) { finish(c, c.r[3], do_free_blocks(c.r[3], c.r[4], c.r[5]), "CARDFreeBlocks"); }
void CARDFormat(Cpu& c) { finish(c, c.r[3], do_format(c.r[3]), "CARDFormat"); }
void CARDFormatRegionAsync(Cpu& c) { finish_async(c, c.r[3], do_format(c.r[3]), c.r[5]); }       // (slot, encoding, cb)
void CARDOpen(Cpu& c) { finish(c, c.r[3], do_open(c.r[3], guest_string(c.r[4]), c.r[5]), "CARDOpen"); }
void CARDFastOpen(Cpu& c) { finish(c, c.r[3], do_fast_open(c.r[3], c.r[4], c.r[5]), "CARDFastOpen"); }
void CARDClose(Cpu& c) { finish(c, 0, do_close(c.r[3]), "CARDClose"); }
// (Those that stamp the file's time call the OS's clock, which leaves its answer in r3: the slot is read first.)
void CARDCreate(Cpu& c) {  // (slot, name, size, info)
    const uint32_t slot = c.r[3];
    finish(c, slot, do_create(c, slot, guest_string(c.r[4]), c.r[5], c.r[6]), "CARDCreate");
}
void CARDCreateAsync(Cpu& c) {  // (slot, name, size, info, cb)
    const uint32_t slot = c.r[3], cb = c.r[7];
    finish_async(c, slot, do_create(c, slot, guest_string(c.r[4]), c.r[5], c.r[6]), cb);
}
void CARDRead(Cpu& c) {  // (info, buffer, length, offset)
    const uint32_t slot = info_slot(c.r[3]);
    finish(c, slot, do_read(c.r[3], c.r[4], c.r[5], c.r[6]), "CARDRead");
}
void CARDReadAsync(Cpu& c) {
    const uint32_t slot = info_slot(c.r[3]), cb = c.r[7];
    finish_async(c, slot, do_read(c.r[3], c.r[4], c.r[5], c.r[6]), cb);
}
void CARDWrite(Cpu& c) {
    const uint32_t slot = info_slot(c.r[3]);
    finish(c, slot, do_write(c, c.r[3], c.r[4], c.r[5], c.r[6]), "CARDWrite");
}
void CARDWriteAsync(Cpu& c) {
    const uint32_t slot = info_slot(c.r[3]), cb = c.r[7];
    finish_async(c, slot, do_write(c, c.r[3], c.r[4], c.r[5], c.r[6]), cb);
}
void CARDDelete(Cpu& c) { finish(c, c.r[3], do_delete_name(c.r[3], guest_string(c.r[4])), "CARDDelete"); }
void CARDDeleteAsync(Cpu& c) { finish_async(c, c.r[3], do_delete_name(c.r[3], guest_string(c.r[4])), c.r[5]); }
void CARDFastDelete(Cpu& c) { finish(c, c.r[3], do_delete_number(c.r[3], c.r[4]), "CARDFastDelete"); }
void CARDFastDeleteAsync(Cpu& c) { finish_async(c, c.r[3], do_delete_number(c.r[3], c.r[4]), c.r[5]); }
void CARDGetStatus(Cpu& c) { finish(c, c.r[3], do_get_status(c.r[3], c.r[4], c.r[5]), "CARDGetStatus"); }
void CARDSetStatus(Cpu& c) {
    const uint32_t slot = c.r[3];
    finish(c, slot, do_set_status(c, slot, c.r[4], c.r[5]), "CARDSetStatus");
}
void CARDSetStatusAsync(Cpu& c) {  // (slot, fileNo, stat, cb)
    const uint32_t slot = c.r[3], cb = c.r[6];
    finish_async(c, slot, do_set_status(c, slot, c.r[4], c.r[5]), cb);
}
void CARDRename(Cpu& c) {
    const uint32_t slot = c.r[3];
    finish(c, slot, do_rename(c, slot, c.r[4], c.r[5]), "CARDRename");
}
void CARDRenameAsync(Cpu& c) {  // (slot, old, new, cb)
    const uint32_t slot = c.r[3], cb = c.r[6];
    finish_async(c, slot, do_rename(c, slot, c.r[4], c.r[5]), cb);
}

const rt::Provided kReplacements[] = {
    {0x800B4668u, "CARDInit", CARDInit},
    {0x800B4870u, "CARDFreeBlocks", CARDFreeBlocks},
    {0x800B6F74u, "CARDCheckExAsync", CARDCheckExAsync},
    {0x800B7504u, "CARDCheck", CARDCheck},
    {0x800B7624u, "CARDProbeEx", CARDProbeEx},
    {0x800B7CE8u, "CARDMountAsync", CARDMountAsync},
    {0x800B7E88u, "CARDMount", CARDMount},
    {0x800B7F6Cu, "CARDUnmount", CARDUnmount},
    {0x800B815Cu, "__CARDFormatRegionAsync", CARDFormatRegionAsync},
    {0x800B87B4u, "CARDFormat", CARDFormat},
    {0x800B8C7Cu, "CARDFastOpen", CARDFastOpen},
    {0x800B8D80u, "CARDOpen", CARDOpen},
    {0x800B8E9Cu, "CARDClose", CARDClose},
    {0x800B9028u, "CARDCreateAsync", CARDCreateAsync},
    {0x800B9248u, "CARDCreate", CARDCreate},
    {0x800B9578u, "CARDReadAsync", CARDReadAsync},
    {0x800B96BCu, "CARDRead", CARDRead},
    {0x800B9924u, "CARDWriteAsync", CARDWriteAsync},
    {0x800B9A38u, "CARDWrite", CARDWrite},
    {0x800B9B24u, "CARDFastDeleteAsync", CARDFastDeleteAsync},
    {0x800B9C50u, "CARDFastDelete", CARDFastDelete},
    {0x800B9C98u, "CARDDeleteAsync", CARDDeleteAsync},
    {0x800B9DA8u, "CARDDelete", CARDDelete},
    {0x800B9FE8u, "CARDGetStatus", CARDGetStatus},
    {0x800BA0FCu, "CARDSetStatusAsync", CARDSetStatusAsync},
    {0x800BA270u, "CARDSetStatus", CARDSetStatus},
    {0x800BA2B8u, "CARDRenameAsync", CARDRenameAsync},
    {0x800BA4B4u, "CARDRename", CARDRename},
};

}  // namespace

fs::path card_folder(uint32_t slot) { return user() / (slot == 0 ? "card-a" : "card-b"); }
bool card_present(uint32_t slot) {
    std::error_code ec;
    return slot < kSlots && fs::is_directory(card_folder(slot), ec);
}

std::span<const rt::Provided> card_replacements() { return kReplacements; }

}  // namespace mkdd
