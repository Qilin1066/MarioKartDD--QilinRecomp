// The DVD library's replacements (dolphin/dvd): the disc's file system and its
// data from the disc the port was started with (an image or an extracted
// folder, qilin/disc: every read is by disc offset), and the drive's
// one-at-a-time command queue with a drive's timing.
//
// Guest structures, from this SDK's code (DVDReadAbsAsyncPrio, 800AD9FC;
// DVDFastOpen, 800AB860; DVDReadPrio, 800ABCD4): a DVDCommandBlock is 0x30
// bytes, with the command at +0x08, the state at +0x0C (0 done, 1 busy,
// 2 waiting, 10 cancelled, -1 fatal error), the disc offset at +0x10, the
// length at +0x14, the buffer at +0x18, the bytes transferred at +0x20 and the
// callback at +0x28; a DVDFileInfo is a command block, then the file's start
// on the disc in bytes at +0x30, its length at +0x34 and the callback for
// asynchronous reads at +0x38.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "boot.hpp"
#include "qilin/disc/disc.hpp"
#include "os.hpp"
#include "threads.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// The file table, as the disc library decodes it: an entry's index is the
// SDK's entry number. No call in this game changes the current directory, so
// paths start at the root.
const std::vector<qilin::disc::Entry>& fst() { return game_disc().entries(); }
constexpr uint32_t kCurrentDir = 0;

bool same_name(const std::string& a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// The SDK's lookup: from the current directory, or the root after a leading
// '/'; "." and ".." step in place and up; names compare without case. (Long
// names are allowed: OSInit sets the flag for it, 800E5C8C.)
int32_t path_to_entry(std::string_view path) {
    const auto& entries = fst();
    uint32_t dir = kCurrentDir;
    if (!path.empty() && path[0] == '/') {
        dir = 0;
        path.remove_prefix(1);
    }
    while (!path.empty()) {
        const size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            dir = entries[dir].parent;
            continue;
        }
        bool found = false;
        for (uint32_t i = dir + 1; i < entries[dir].end;) {
            if (same_name(entries[i].name, part)) {
                if (!entries[i].directory && !path.empty()) return -1;  // a file used as a directory
                dir = i;
                found = true;
                break;
            }
            i = entries[i].directory ? entries[i].end : i + 1;
        }
        if (!found) return -1;
    }
    return static_cast<int32_t>(dir);
}

// ---- the drive: one command at a time, the rest waiting by priority, each
// finishing as an interrupt after the time a drive takes. The game's own
// bookkeeping counts on reads taking time (a completion callback that runs
// before the call that started the read has returned finds it half done).
constexpr uint32_t kCommand = 0x08, kState = 0x0C, kOffset = 0x10, kLength = 0x14, kAddr = 0x18, kTransferred = 0x20,
                   kCallback = 0x28;
constexpr uint32_t kFileStart = 0x30, kFileLength = 0x34, kFileCallback = 0x38;
enum : uint32_t { kEnd = 0, kBusy = 1, kWaiting = 2, kCancelled = 10, kFatal = 0xFFFFFFFFu };
constexpr uint32_t kSyncQueue = 0x00000200u;  // threads in DVDReadPrio (a key of our own)

struct Command {
    uint32_t block;
    uint32_t callback;  // guest (result, block), or 0
    int32_t priority;
};
std::deque<Command> g_waiting;
Command g_executing{};
bool g_busy = false, g_completion_posted = false;
uint64_t g_serial = 0;   // of the command running: a completion posted for one cancelled since finds another
uint64_t g_done_at = 0;  // time base
uint64_t g_head = 0;     // where the last read ended on the disc

// How long a read takes. The drive turns at a constant rate, so data passes
// faster towards the outer edge: 2 MiB/s at the start of the disc to 3.125
// MiB/s at the end of a full one (Dolphin's figures for the GameCube's drive),
// taken here as linear in the offset. Moving the head costs a seek: 20 ms and
// up to 100 ms more with the distance. MKDD_DVD_SPEED=<n> divides both (for
// tests; 0: no time at all).
double drive_speed() {
    static const double speed = [] {
        const char* e = std::getenv("MKDD_DVD_SPEED");
        return e ? std::max(0.0, std::atof(e)) : 1.0;
    }();
    return speed;
}
uint64_t read_ticks(uint64_t offset, uint32_t length) {
    if (drive_speed() == 0.0) return 0;
    constexpr double kFullDisc = double(qilin::disc::kDiscSize), kMiB = 1048576.0;
    const double rate = (2.0 + 1.125 * std::min(1.0, double(offset) / kFullDisc)) * kMiB;
    const double distance = offset > g_head ? double(offset - g_head) : double(g_head - offset);
    const double seek = distance < 32768.0 ? 0.0 : 0.020 + 0.100 * std::min(1.0, distance / kFullDisc);
    return static_cast<uint64_t>((seek + double(length) / rate) / drive_speed() * double(kTimerHz));
}

void start_next() {
    if (g_busy || g_waiting.empty()) return;
    g_executing = g_waiting.front();
    g_waiting.pop_front();
    g_busy = true;
    g_completion_posted = false;
    ++g_serial;
    const uint32_t b = g_executing.block;
    rt::poke32(b + kState, kBusy);
    g_done_at = time_base() + read_ticks(rt::peek32(b + kOffset), rt::peek32(b + kLength));
    wake_idle();
}

void finish(Cpu& c, const Command& cmd, uint32_t state, int32_t result) {
    rt::poke32(cmd.block + kState, state);
    if (cmd.callback) {
        // The SDK calls it from the drive's interrupt, interrupts off. A cancel
        // comes here from the caller's own thread: off for the callback there
        // too, so nothing is delivered (and no thread switched to) inside it.
        const bool enabled = interrupts_enabled();
        set_interrupts_enabled(false);
        guest_callback(c, cmd.callback, static_cast<uint32_t>(result), cmd.block);
        set_interrupts_enabled(enabled);
    }
    wake_all(c, kSyncQueue);
}

void complete(Cpu& c, uint64_t serial) {  // as an interrupt
    if (!g_busy || serial != g_serial) return;  // cancelled since
    g_completion_posted = false;
    const Command cmd = g_executing;
    const uint32_t b = cmd.block, offset = rt::peek32(b + kOffset), length = rt::peek32(b + kLength), addr = rt::peek32(b + kAddr);
    if (addr < 0x80000000u || uint64_t{addr} + length > 0x81800000u) rt::fatal(&c, "DVD read of %u bytes to %08X, outside memory", length, addr);
    if (const auto read = game_disc().read(offset, std::span(rt::guest_ptr(addr), length)); !read)
        rt::fatal(&c, "DVD read of %u bytes at %08X: %s", length, offset, read.error().c_str());
    g_head = uint64_t{offset} + length;
    rt::poke32(b + kTransferred, length);
    g_busy = false;
    start_next();  // before the callback, which may queue more behind it
    finish(c, cmd, kEnd, static_cast<int32_t>(length));
}

void poll_drive() {
    if (g_busy && !g_completion_posted && time_base() >= g_done_at) {
        g_completion_posted = true;
        post_interrupt([serial = g_serial](Cpu& c) { complete(c, serial); });
    }
}

// DVDReadAbsAsyncPrio: queue a read by disc offset.
void queue_read(uint32_t block, uint32_t addr, uint32_t length, uint32_t offset, uint32_t callback, int32_t priority) {
    rt::poke32(block + kCommand, 1);
    rt::poke32(block + kAddr, addr);
    rt::poke32(block + kLength, length);
    rt::poke32(block + kOffset, offset);
    rt::poke32(block + kTransferred, 0);
    rt::poke32(block + kCallback, callback);
    rt::poke32(block + kState, kWaiting);
    auto at = g_waiting.begin();
    while (at != g_waiting.end() && at->priority <= priority) ++at;  // first come first served within a priority
    g_waiting.insert(at, {block, callback, priority});
    start_next();
}

// The bounds DVDReadPrio and DVDReadAsyncPrio check (they panic outside them):
// the offset inside the file, the end within the file rounded up to the
// drive's 32 bytes.
void check_bounds(Cpu& c, const char* who, uint32_t fi, uint32_t length, uint32_t offset) {
    const uint32_t size = rt::peek32(fi + kFileLength);
    if (offset >= size) rt::fatal(&c, "%s: offset %u is outside the file (%u bytes)", who, offset, size);
    if (uint64_t{offset} + length >= uint64_t{size} + 32) rt::fatal(&c, "%s: %u bytes at %u run past the file's end (%u bytes)", who, length, offset, size);
}

// ---- the API
void DVDInit(Cpu&) {  // OSInit calls it, and the game again
    static bool done = false;
    if (done) return;
    done = true;
    add_timer_source(poll_drive, [] { return g_busy && !g_completion_posted ? g_done_at : UINT64_MAX; });
}

void DVDConvertPathToEntrynum(Cpu& c) {
    c.r[3] = static_cast<uint32_t>(path_to_entry(reinterpret_cast<const char*>(rt::guest_ptr(c.r[3]))));
}

bool open_entry(uint32_t n, uint32_t fi) {
    const auto& entries = fst();
    if (n >= entries.size() || entries[n].directory) return false;
    static const bool say = std::getenv("MKDD_DVD_LOG") != nullptr;  // development: which files the game opens, and when
    static const uint64_t first = time_base();
    if (say) rt::log("DVD: %.2f s: opens %s (%u bytes)", double(time_base() - first) / double(kTimerHz), entries[n].path.c_str(), entries[n].size);
    rt::poke32(fi + kFileStart, entries[n].offset);
    rt::poke32(fi + kFileLength, entries[n].size);
    rt::poke32(fi + kFileCallback, 0);
    rt::poke32(fi + kState, kEnd);
    return true;
}
void DVDFastOpen(Cpu& c) {  // (entrynum, fileInfo) -> TRUE, or FALSE for a directory or a bad number
    c.r[3] = static_cast<int32_t>(c.r[3]) >= 0 && open_entry(c.r[3], c.r[4]);
}
void DVDOpen(Cpu& c) {  // (path, fileInfo) -> TRUE, or FALSE if there's no such file
    const char* path = reinterpret_cast<const char*>(rt::guest_ptr(c.r[3]));
    const int32_t n = path_to_entry(path);
    if (n < 0) rt::log("DVDOpen: there's no %s on the disc", path);
    c.r[3] = n >= 0 && open_entry(static_cast<uint32_t>(n), c.r[4]);
}

// DVDCancel: take a command out of the queue, or stop the one running; its
// callback hears -3. Done at once, so there's nothing to wait for.
void cancel(Cpu& c, uint32_t block) {
    const uint32_t state = rt::peek32(block + kState);
    if (state == kWaiting) {
        for (auto it = g_waiting.begin(); it != g_waiting.end(); ++it)
            if (it->block == block) {
                const Command cmd = *it;
                g_waiting.erase(it);
                finish(c, cmd, kCancelled, -3);
                return;
            }
    } else if (state == kBusy && g_busy && g_executing.block == block) {
        const Command cmd = g_executing;
        g_busy = false;
        start_next();
        finish(c, cmd, kCancelled, -3);
    }
}
void DVDCancel(Cpu& c) {  // (block) -> 0
    cancel(c, c.r[3]);
    c.r[3] = 0;
}
void DVDClose(Cpu& c) {  // (fileInfo) -> TRUE: cancels what the file still has going
    cancel(c, c.r[3]);
    c.r[3] = 1;
}

void DVDReadAsyncPrio(Cpu& c) {  // (fileInfo, addr, length, offset, callback, prio) -> TRUE
    const uint32_t fi = c.r[3];
    check_bounds(c, "DVDReadAsync", fi, c.r[5], c.r[6]);
    rt::poke32(fi + kFileCallback, c.r[7]);  // the SDK's own callback passes the result on to it: here it's called directly
    queue_read(fi, c.r[4], c.r[5], rt::peek32(fi + kFileStart) + c.r[6], c.r[7], static_cast<int32_t>(c.r[8]));
    c.r[3] = 1;
}

void DVDReadPrio(Cpu& c) {  // (fileInfo, addr, length, offset, prio) -> bytes read, -1 on a fatal error, -3 if cancelled
    const uint32_t fi = c.r[3];
    check_bounds(c, "DVDRead", fi, c.r[5], c.r[6]);
    queue_read(fi, c.r[4], c.r[5], rt::peek32(fi + kFileStart) + c.r[6], 0, static_cast<int32_t>(c.r[7]));
    for (;;) {
        const uint32_t state = rt::peek32(fi + kState);
        if (state == kEnd) {
            c.r[3] = rt::peek32(fi + kTransferred);
            return;
        }
        if (state == kFatal || state == kCancelled) {
            c.r[3] = state == kFatal ? 0xFFFFFFFFu : 0xFFFFFFFDu;
            return;
        }
        wait_on(c, kSyncQueue);
    }
}

void DVDGetCommandBlockStatus(Cpu& c) {
    deliver_interrupts(c);  // a polling loop: let a pending completion arrive
    const uint32_t s = rt::peek32(c.r[3] + kState);
    c.r[3] = s == 3 ? 1 : s;  // DVD_STATE_COVER_CLOSED reads as busy
}

void DVDGetDriveStatus(Cpu& c) {
    deliver_interrupts(c);
    c.r[3] = g_busy ? kBusy : kEnd;
}
void DVDCheckDisk(Cpu& c) { c.r[3] = 1; }  // the game's disc is in the drive
void DVDGetCurrentDiskID(Cpu& c) { c.r[3] = 0x80000000u; }  // where the boot ROM read it to

const rt::Provided kReplacements[] = {
    // dvdfs.c
    {0x800AB56Cu, "DVDConvertPathToEntrynum", DVDConvertPathToEntrynum},
    {0x800AB860u, "DVDFastOpen", DVDFastOpen},
    {0x800AB8D4u, "DVDOpen", DVDOpen},
    {0x800AB99Cu, "DVDClose", DVDClose},
    {0x800ABBE4u, "DVDReadAsyncPrio", DVDReadAsyncPrio},
    {0x800ABCD4u, "DVDReadPrio", DVDReadPrio},
    // dvd.c
    {0x800ABE14u, "DVDInit", DVDInit},
    {0x800ADE4Cu, "DVDGetCommandBlockStatus", DVDGetCommandBlockStatus},
    {0x800ADE98u, "DVDGetDriveStatus", DVDGetDriveStatus},
    {0x800AE220u, "DVDCancel", DVDCancel},
    {0x800AE2F0u, "DVDGetCurrentDiskID", DVDGetCurrentDiskID},
    {0x800AE2F8u, "DVDCheckDisk", DVDCheckDisk},
};

}  // namespace

std::span<const rt::Provided> dvd_replacements() { return kReplacements; }

}  // namespace mkdd
