// The memory card replacements (card.cpp) over synthetic guest memory and a
// temporary user folder: the card's file system as the SDK presents it
// (results, free blocks, permissions), the time calls take, the order
// asynchronous calls finish in, safe writes and the save history.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "boot.hpp"
#include "card.hpp"
#include "save_history.hpp"
#include "device_harness.hpp"

namespace rt = qilin::rt;
namespace fs = std::filesystem;
using namespace harness;

namespace {
fs::path folder;
constexpr uint64_t kSeconds = 123456;
void os_get_time(rt::Cpu& c) {  // the OS's clock, where card.cpp calls it
    const uint64_t t = kSeconds * mkdd::kTimerHz + 17;
    c.r[3] = static_cast<uint32_t>(t >> 32);
    c.r[4] = static_cast<uint32_t>(t);
}
const rt::FunctionEntry kFunctions[] = {{0x800EC33Cu, os_get_time}};
}  // namespace
namespace qilin::host {
fs::path user_folder() { return folder; }
}

int main() {
    folder = fs::temp_directory_path() / ("mkdd-card-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    rt::init({kFunctions, {}});
    auto cpu = rt::new_cpu();
    std::memcpy(rt::guest_ptr(0x80000000u), "GM4E01", 6);
    constexpr uint32_t name = 0x80100000, name2 = 0x80100040, info = 0x80100100, info2 = 0x80100120, out = 0x80100200,
                       stat = 0x80100300, buffer = 0x80110000, buffer2 = 0x80120000, cb = 0x80200000, cb2 = 0x80200040,
                       detach_cb = 0x80200080;
    const auto call = [&](const char* fn, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, uint32_t e = 0) {
        cpu.r[3] = a, cpu.r[4] = b, cpu.r[5] = c, cpu.r[6] = d, cpu.r[7] = e;
        for (const auto& r : mkdd::card_replacements())
            if (std::strcmp(fn, r.name) == 0) { r.fn(cpu); return static_cast<int32_t>(cpu.r[3]); }
        std::fprintf(stderr, "unknown replacement: %s\n", fn);
        ++failures;
        return -999;
    };
    const auto set = [&](uint32_t at, const char* text) { std::strcpy(reinterpret_cast<char*>(rt::guest_ptr(at)), text); };
    const auto bytes = [&](const fs::path& p) { std::ifstream in(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); };
    const auto free_bytes = [&] { return call("CARDFreeBlocks", 0, out, out + 4) == 0 ? static_cast<int32_t>(rt::peek32(out)) : -1; };
    const auto free_files = [&] { return rt::peek32(out + 4); };
    const fs::path save = folder / "card-a" / "GM4E01-MarioKart Double Dash!!.gci";

    // ---- cards and slots
    call("CARDInit");
    check(fs::is_directory(folder / "card-a") && !fs::exists(folder / "card-b"), "slot A's card is made; slot B stays empty");
    check(call("CARDProbeEx", 0, out, out + 4) == 0 && rt::peek32(out) == 16 && rt::peek32(out + 4) == 8192,
          "slot A: a 16-megabit card with 8 KiB sectors");
    check(call("CARDProbeEx", 1, out, out + 4) == -3 && call("CARDProbeEx", 2, out, out + 4) == -128, "slot B: no card; slot 2: no such slot");
    check(call("CARDFreeBlocks", 0, out, out + 4) == -3 && call("CARDMount", 1, buffer, 0) == -3, "an unmounted card and an empty slot: no card");
    uint64_t t = now;
    check(call("CARDMount", 0, buffer, 0) == 0 && now - t == ms(300), "mounting takes the calling thread 300 ms");
    check(free_bytes() == 251 * 8192 && free_files() == 127 && call("CARDCheck", 0) == 0, "an empty card: 251 blocks, 127 files, sound");

    // ---- the game's save: 3 blocks
    set(name, "MarioKart Double Dash!!");
    check(call("CARDOpen", 0, name, info) == -4, "opening a file that isn't there");
    check(call("CARDCreate", 0, name, 100, info) == -128 && call("CARDCreate", 0, name, 252 * 8192, info) == -9,
          "a size that isn't whole sectors is refused outright; one past the card's room for lack of space");
    set(name2, "a name of thirty-three characters");
    check(call("CARDCreate", 0, name2, 8192, info) == -12, "a name over 32 characters is too long");
    t = now;
    check(call("CARDCreate", 0, name, 0x6000, info) == 0 && now - t == ms(300), "creating writes the block map and the directory");
    check(rt::peek32(info) == 0 && rt::peek32(info + 4) == 0 && rt::peek32(info + 12) == 0x6000, "the new file is open: slot 0, file 0");
    check(free_bytes() == 248 * 8192 && free_files() == 126, "3 blocks and a directory entry are taken");
    check(call("CARDCreate", 0, name, 0x6000, info2) == -7, "a second file of the same name: exists");
    std::string image = bytes(save);
    check(image.size() == 64 + 0x6000 && image.compare(0, 6, "GM4E01") == 0 && image.compare(8, 24, std::string("MarioKart Double Dash!!") + '\0') == 0 &&
          image[0x38] == 0 && image[0x39] == 3 && image[0x34] == 4 && static_cast<uint8_t>(image[64]) == 0xFF,
          "the host file is a .gci: the entry (game, name, 3 blocks, public), then erased blocks");
    for (uint32_t i = 0; i < 0x2000; ++i) rt::poke8(buffer + i, static_cast<uint8_t>(i * 3 + 1));
    check(call("CARDWrite", info, buffer, 0x2000, 0x1000) == -128 && call("CARDWrite", info, buffer, 0x2000, 0x6000) == -11 &&
          call("CARDWrite", info, buffer, 0x4000, 0x4000) == -11, "writes are whole sectors inside the file");
    t = now;
    check(call("CARDWrite", info, buffer, 0x2000, 0x2000) == 0 && now - t == ms(300), "writing a sector: the sector, then the directory");
    check(call("CARDRead", info, buffer2, 0x200, 0x2100) == -128 && call("CARDRead", info, buffer2, 0x200, 0x6000) == -11,
          "reads are whole 512-byte segments inside the file");
    check(call("CARDRead", info, buffer2, 0x400, 0x2200) == 0 && std::memcmp(rt::guest_ptr(buffer2), rt::guest_ptr(buffer + 0x200), 0x400) == 0,
          "what was written is read back");
    check(mkdd::save_backups(folder).empty(), "a new file's first contents leave nothing to keep");

    // ---- status: the banner, icons and comment
    check(call("CARDGetStatus", 0, 0, stat) == 0 && rt::peek32(stat + 0x20) == 0x6000 && rt::peek32(stat + 0x24) == kSeconds &&
          std::memcmp(rt::guest_ptr(stat + 0x28), "GM4E01", 6) == 0 && rt::peek32(stat + 0x30) == 0xFFFFFFFFu &&
          rt::peek32(stat + 0x3C) == 0xFFFFFFFFu && rt::peek32(stat + 0x68) == 0,
          "a new file's status: its size, time and game, no banner or icons");
    rt::poke8(stat + 0x2E, 1);        // an 8-bit banner
    rt::poke32(stat + 0x30, 0x40);    // pictures from 0x40
    rt::poke16(stat + 0x34, 0x0015);  // three 8-bit icons
    rt::poke16(stat + 0x36, 0x002A);
    rt::poke32(stat + 0x38, 0);       // the comment at the start
    check(call("CARDSetStatus", 0, 0, stat) == 0 && call("CARDGetStatus", 0, 0, stat) == 0, "the status is set and read back");
    check(rt::peek32(stat + 0x3C) == 0x40 && rt::peek32(stat + 0x40) == 0x40 + 3072 && rt::peek32(stat + 0x44) == 0x40 + 3584 &&
          rt::peek32(stat + 0x4C) == 0x40 + 3584 + 2048 && rt::peek32(stat + 0x50) == 0xFFFFFFFFu &&
          rt::peek32(stat + 0x64) == 0x40 + 3584 + 3072 && rt::peek32(stat + 0x68) == 0x40 + 3584 + 3072 + 512,
          "the banner, its palette, the icons and theirs follow one another");
    image = bytes(save);
    check(image[7] == 1 && image[0x31] == 0x15 && image[0x33] == 0x2A && image[0x2F] == 0x40, "the .gci's entry holds the new status");
    rt::poke32(stat + 0x30, 512);
    check(call("CARDSetStatus", 0, 0, stat) == -128, "pictures must start in the first 512 bytes");
    rt::poke32(stat + 0x30, 0x40);
    check(call("CARDClose", info) == 0 && rt::peek32(info) == 0xFFFFFFFFu && call("CARDRead", info, buffer2, 0x200, 0) == -128,
          "a closed file's info names no card");
    check(call("CARDFastOpen", 0, 0, info) == 0 && call("CARDFastOpen", 0, 5, info2) == -4 && call("CARDFastOpen", 0, 127, info2) == -128,
          "opening by number: the file, an empty entry, no such entry");

    // ---- the save history: the file as it was before the first change since it was opened
    const std::string before = bytes(save);
    std::memset(rt::guest_ptr(buffer), 0x11, 0x2000);
    check(call("CARDWrite", info, buffer, 0x2000, 0) == 0 && mkdd::save_backups(folder).size() == 1 &&
          bytes(mkdd::save_backups(folder)[0].file) == before, "the first write after opening keeps the old file");
    check(call("CARDWrite", info, buffer, 0x2000, 0x4000) == 0 && call("CARDSetStatus", 0, 0, stat) == 0 &&
          mkdd::save_backups(folder).size() == 1, "the rest of that save adds nothing to the history");
    check(mkdd::save_backups(folder)[0].label.starts_with("Card A: GM4E01-MarioKart Double Dash!! / "), "the history names the card and the file");

    // ---- safe writes
    const std::string good = bytes(save);
    { std::ofstream cut(save.string() + ".tmp", std::ios::binary); cut << "half a save"; }
    check(call("CARDUnmount", 0) == 0 && call("CARDFreeBlocks", 0, out, out + 4) == -3 && call("CARDUnmount", 0) == -3, "unmounting; then there is no card to ask");
    check(call("CARDMount", 0, buffer, 0) == 0 && !fs::exists(save.string() + ".tmp") && bytes(save) == good && free_bytes() == 248 * 8192,
          "a write cut short by a crash is discarded at the next mount: the save before it stands");
    check(call("CARDOpen", 0, name, info) == 0 && rt::peek32(info + 4) == 0, "the file keeps its number across a mount");
    fs::rename(folder / "save-backups", folder / "saved-history");
    { std::ofstream obstruction(folder / "save-backups"); obstruction << "blocked"; }
    std::memset(rt::guest_ptr(buffer), 0x22, 0x2000);
    check(call("CARDWrite", info, buffer, 0x2000, 0) == -5 && bytes(save) == good, "if the old save can't be kept, the write fails and the save stays");
    fs::remove(folder / "save-backups");
    fs::rename(folder / "saved-history", folder / "save-backups");

    // ---- other games' files
    {
        std::string other(64 + 2 * 8192, '\x33');
        std::memset(other.data(), 0, 64);
        std::memcpy(other.data(), "GZLE01", 6);
        std::memcpy(other.data() + 8, "gczelda", 7);
        other[0x34] = 4, other[0x39] = 2;
        std::ofstream(folder / "card-a" / "public.gci", std::ios::binary) << other;
        std::memcpy(other.data() + 8, "private", 7);
        other[0x34] = 0;
        std::ofstream(folder / "card-a" / "private.gci", std::ios::binary) << other;
        std::ofstream(folder / "card-a" / "broken.gci", std::ios::binary) << "not a card file";
    }
    call("CARDUnmount", 0);
    check(call("CARDMount", 0, buffer, 0) == 0 && free_bytes() == 244 * 8192 && free_files() == 124, "files dropped into the folder are on the card; a malformed one isn't");
    // (new files take the free entries in the order of their host names: private.gci entry 1, public.gci entry 2)
    set(name2, "gczelda");
    check(call("CARDOpen", 0, name2, info2) == -4, "another game's file can't be opened by name");
    check(call("CARDFastOpen", 0, 2, info2) == 0 && call("CARDRead", info2, buffer2, 0x200, 0) == 0 && *rt::guest_ptr(buffer2) == 0x33 &&
          call("CARDGetStatus", 0, 2, stat) == 0, "a public one can be read by number");
    check(call("CARDWrite", info2, buffer, 0x2000, 0) == -10 && call("CARDFastDelete", 0, 2) == -10, "but not written or deleted");
    check(call("CARDFastOpen", 0, 1, info2) == -10 && call("CARDGetStatus", 0, 1, stat) == -10, "a private one can't even be read");

    // ---- renaming and deleting
    set(name2, "Other");
    check(call("CARDRename", 0, name2, name) == -4 && call("CARDRename", 0, name, name2) == 0 && call("CARDOpen", 0, name, info) == -4 &&
          call("CARDOpen", 0, name2, info) == 0 && fs::exists(folder / "card-a" / "GM4E01-Other.gci") && !fs::exists(save),
          "a renamed file answers to its new name, and its host file follows");
    check(call("CARDCreate", 0, name, 8192, info2) == 0 && call("CARDRename", 0, name2, name) == -7, "renaming onto an existing name: exists");
    check(call("CARDDelete", 0, name) == 0 && call("CARDDelete", 0, name) == -4 && !fs::exists(save), "deleting, once");
    const std::string other_save = bytes(folder / "card-a" / "GM4E01-Other.gci");
    const size_t history = mkdd::save_backups(folder).size();
    check(call("CARDFastDelete", 0, 0) == 0 && free_bytes() == 247 * 8192 && mkdd::save_backups(folder).size() == history + 1,
          "a deleted save goes to the history first");
    std::string error;
    check(mkdd::request_save_restore(folder, mkdd::save_backups(folder)[0].file, error) && mkdd::apply_save_restore(folder, error) &&
          bytes(folder / "card-a" / "GM4E01-Other.gci") == other_save, "and can be restored from it");
    call("CARDUnmount", 0);
    check(call("CARDMount", 0, buffer, 0) == 0 && call("CARDOpen", 0, name2, info) == 0, "the restored save is on the card at the next mount");

    // ---- asynchronous calls: back at once, the result by callback when the card is done
    callbacks.clear();
    t = now;
    check(call("CARDReadAsync", info, buffer2, 0x200, 0x100, cb) == -128 && callbacks.empty(), "a refused call returns its error and never calls back");
    check(call("CARDWriteAsync", info, buffer, 0x2000, 0, cb) == 0 && now == t && callbacks.empty(), "an accepted one returns READY at once");
    check(call("CARDFreeBlocks", 0, out, out + 4) == -1 && call("CARDReadAsync", info, buffer2, 0x200, 0, cb) == -1, "the card is busy meanwhile");
    fs::create_directories(folder / "card-b");
    check(call("CARDProbeEx", 1, out, out + 4) == 0 && call("CARDMountAsync", 1, buffer, detach_cb, cb2) == 0, "a card put in slot B mounts while A writes");
    advance(cpu, ms(299));
    check(callbacks.empty(), "nothing is done before its time");
    advance(cpu, ms(2));
    check(callbacks.size() == 2 && callbacks[0] == Callback{cb, 0, 0, t + ms(300)} && callbacks[1] == Callback{cb2, 1, 0, t + ms(300)},
          "both finish at 300 ms, in the order they began");
    callbacks.clear();
    t = now;
    set(name, "ghost");
    check(call("CARDCreateAsync", 1, name, 5 * 8192, info2, cb2) == 0 && call("CARDReadAsync", info, buffer2, 0x2000, 0, cb) == 0, "a create on B, then a read on A");
    advance(cpu, ms(400));
    check(callbacks.size() == 2 && callbacks[0] == Callback{cb, 0, 0, t + ms(6)} && callbacks[1] == Callback{cb2, 1, 0, t + ms(300)},
          "the short read finishes first");
    check(*rt::guest_ptr(buffer2) == 0x22, "the read's data is there");
    callbacks.clear();
    check(call("CARDCheckExAsync", 1, out, cb) == 0 && callbacks.empty(), "a check with nothing to repair still answers by callback");
    advance(cpu, 1);
    check(callbacks.size() == 1 && callbacks[0].a0 == 1 && callbacks[0].a1 == 0 && rt::peek32(out) == 0, "at the next chance");

    // ---- a card pulled out while it writes: NOCARD, and the file as it was
    const fs::path ghost = folder / "card-b" / "GM4E01-ghost.gci", ghost_out = folder / "card-b-out" / "GM4E01-ghost.gci";
    const std::string ghost_before = bytes(ghost);
    const size_t history_before = mkdd::save_backups(folder).size();
    std::memset(rt::guest_ptr(buffer), 0x44, 0x2000);
    callbacks.clear();
    t = now;
    check(call("CARDWriteAsync", info2, buffer, 0x2000, 0, cb) == 0, "a write to card B begins");
    advance(cpu, ms(100));
    fs::rename(folder / "card-b", folder / "card-b-out");
    advance(cpu, ms(300));
    check(callbacks.size() == 2 && callbacks[0] == Callback{cb, 1, 0xFFFFFFFDu, t + ms(300)} &&
              callbacks[1] == Callback{detach_cb, 1, 0xFFFFFFFDu, t + ms(300)},
          "pulled out 100 ms into it: the write's callback says NOCARD, then the detach callback");
    check(bytes(ghost_out) == ghost_before && !fs::exists(ghost_out.string() + ".tmp") && mkdd::save_backups(folder).size() == history_before,
          "the card holds the file as it was, and the history has nothing new");
    fs::rename(folder / "card-b-out", folder / "card-b");
    check(call("CARDFreeBlocks", 1, out, out + 4) == -3 && call("CARDMount", 1, buffer, detach_cb) == 0 && call("CARDFastOpen", 1, 0, info2) == 0 &&
              call("CARDRead", info2, buffer2, 0x200, 0) == 0 && *rt::guest_ptr(buffer2) == 0xFF,
          "put back, the card must be mounted again; the file reads as before the write");
#if defined(_WIN32)
    _putenv_s("MKDD_CARD_PULL_AT", "1");
#else
    setenv("MKDD_CARD_PULL_AT", "1", 1);
#endif
    call("CARDInit");  // (the hook counts the writes from here)
    const fs::path other = folder / "card-a" / "GM4E01-Other.gci", other_out = folder / "card-a.pulled" / "GM4E01-Other.gci";
    const std::string other_before = bytes(other);
    std::memset(rt::guest_ptr(buffer), 0x55, 0x2000);
    callbacks.clear();
    t = now;
    check(call("CARDWrite", info, buffer, 0x2000, 0x2000) == -3 && now - t == ms(300),
          "MKDD_CARD_PULL_AT=1: card A pulled out halfway through a synchronous write, which says NOCARD when its time is over");
    check(bytes(other_out) == other_before && !fs::exists(other_out.string() + ".tmp") && callbacks.empty() &&
              call("CARDProbeEx", 0, out, out + 4) == -3,
          "the file is as it was; no detach callback (none was given); the slot is empty");
    fs::rename(folder / "card-a.pulled", folder / "card-a");
    check(call("CARDMount", 0, buffer, 0) == 0 && call("CARDOpen", 0, name2, info) == 0 && call("CARDRead", info, buffer2, 0x2000, 0x2000) == 0 &&
              std::memcmp(rt::guest_ptr(buffer2), other_before.data() + 64 + 0x2000, 0x2000) == 0,
          "put back and mounted: the old contents");

    // ---- a card pulled out; a card formatted
    callbacks.clear();
    fs::remove_all(folder / "card-b");
    t = now;
    check(call("CARDFreeBlocks", 1, out, out + 4) == -3, "a card pulled out is gone for the next call");
    advance(cpu, 1);
    check(callbacks.size() == 1 && callbacks[0] == Callback{detach_cb, 1, 0xFFFFFFFDu, t}, "and its detach callback says so");
    const size_t kept = mkdd::save_backups(folder).size();
    t = now;
    check(call("CARDFormat", 0) == 0 && now - t == ms(750) && free_bytes() == 251 * 8192 && free_files() == 127 &&
          !fs::exists(folder / "card-a" / "public.gci"), "formatting empties the card");
    check(mkdd::save_backups(folder).size() == kept + 3 && fs::exists(folder / "card-a" / "broken.gci"),
          "each file it held is in the history; what wasn't a card file is left alone");

    fs::remove_all(folder);
    return failures ? 1 : 0;
}
