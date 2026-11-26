// The DVD replacements (dvd.cpp) over a small synthetic disc image, without
// the game: path lookups as the SDK resolves them, what a file's information
// holds, and the drive: one command at a time, by priority, each finishing as
// an interrupt after a drive's time, with the data and the callback then and
// not before.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "boot.hpp"
#include "device_harness.hpp"
#include "qilin/disc/disc.hpp"

namespace rt = qilin::rt;
using namespace harness;

namespace {
std::optional<qilin::disc::Disc> disc;
}
namespace mkdd {
const qilin::disc::Disc& game_disc() { return *disc; }
void wake_idle() {}
void deliver_interrupts(rt::Cpu& c) { harness::deliver(c); }
}  // namespace mkdd

namespace {

using Bytes = std::vector<uint8_t>;
void put32(Bytes& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24), b[at + 1] = uint8_t(v >> 16), b[at + 2] = uint8_t(v >> 8), b[at + 3] = uint8_t(v);
}
void put(Bytes& b, size_t at, std::string_view s) { std::memcpy(b.data() + at, s.data(), s.size()); }

// Files on the test's disc: /a.bin, /Course/Luigi.arc, /Course/big.bin.
constexpr uint32_t kDolAt = 0x2600, kFstAt = 0x2800, kA = 0x3000, kLuigi = 0x4000, kBig = 0x8000;
constexpr uint32_t kASize = 100, kLuigiSize = 0x1234, kBigSize = 0x60000;
uint8_t byte_at(uint32_t offset) { return static_cast<uint8_t>(offset * 7 + (offset >> 8)); }

Bytes build() {
    Bytes image(kBig + kBigSize);
    put(image, 0, "GTSE01");
    put32(image, 0x1C, qilin::disc::kMagic);
    put(image, qilin::disc::kApploaderOffset, "2026/11/26");
    put32(image, qilin::disc::kApploaderOffset + 0x14, 0x40);
    put32(image, qilin::disc::kApploaderOffset + 0x18, 0x20);
    // A DOL of one text and one data section.
    put32(image, kDolAt + 0x00, 0x100), put32(image, kDolAt + 0x48, 0x80003100), put32(image, kDolAt + 0x90, 0x20);
    put32(image, kDolAt + 0x1C, 0x120), put32(image, kDolAt + 0x64, 0x80004000), put32(image, kDolAt + 0xAC, 0x10);
    put32(image, kDolAt + 0xE0, 0x80003100);
    // The file table: root, a.bin, Course (a directory of two files).
    const char names[] = "\0a.bin\0Course\0Luigi.arc\0big.bin";
    Bytes fst(5 * 12);
    put32(fst, 0, 1u << 24), put32(fst, 8, 5);
    put32(fst, 12, 1), put32(fst, 16, kA), put32(fst, 20, kASize);
    put32(fst, 24, 1u << 24 | 7), put32(fst, 28, 0), put32(fst, 32, 5);
    put32(fst, 36, 14), put32(fst, 40, kLuigi), put32(fst, 44, kLuigiSize);
    put32(fst, 48, 24), put32(fst, 52, kBig), put32(fst, 56, kBigSize);
    fst.insert(fst.end(), names, names + sizeof names);
    std::memcpy(image.data() + kFstAt, fst.data(), fst.size());
    put32(image, 0x420, kDolAt), put32(image, 0x424, kFstAt);
    put32(image, 0x428, static_cast<uint32_t>(fst.size())), put32(image, 0x42C, static_cast<uint32_t>(fst.size()));
    for (uint32_t a = kA; a < kA + kASize; ++a) image[a] = byte_at(a);
    for (uint32_t a = kLuigi; a < kLuigi + kLuigiSize; ++a) image[a] = byte_at(a);
    for (uint32_t a = kBig; a < kBig + kBigSize; ++a) image[a] = byte_at(a);
    return image;
}

bool holds(uint32_t guest, uint32_t disc_offset, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (*rt::guest_ptr(guest + i) != byte_at(disc_offset + i)) return false;
    return true;
}

}  // namespace

int main() {
#if defined(_WIN32)
    _putenv("MKDD_DVD_SPEED=1");
#else
    setenv("MKDD_DVD_SPEED", "1", 1);
#endif
    const auto path = std::filesystem::temp_directory_path() / "mkdd_dvd_test.iso";
    {
        const Bytes image = build();
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    }
    auto opened = qilin::disc::Disc::open(path);
    if (!opened) {
        std::fprintf(stderr, "FAIL the test's own disc doesn't open: %s\n", opened.error().c_str());
        return 1;
    }
    disc.emplace(std::move(*opened));

    rt::init({{}, {}});
    auto cpu = rt::new_cpu();
    const auto dvd = [&](const char* name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, uint32_t e = 0, uint32_t f = 0) {
        cpu.r[3] = a, cpu.r[4] = b, cpu.r[5] = c, cpu.r[6] = d, cpu.r[7] = e, cpu.r[8] = f;
        for (const auto& r : mkdd::dvd_replacements())
            if (std::strcmp(name, r.name) == 0) {
                r.fn(cpu);
                return cpu.r[3];
            }
        std::fprintf(stderr, "unknown replacement: %s\n", name);
        ++failures;
        return 0xDEADBEEFu;
    };
    constexpr uint32_t text = 0x80100000, file1 = 0x80100100, file2 = 0x80100200, file3 = 0x80100300, buffer = 0x80200000,
                       buffer2 = 0x80300000, cb = 0x80004000;
    const auto entry = [&](const char* p) {
        std::strcpy(reinterpret_cast<char*>(rt::guest_ptr(text)), p);
        return static_cast<int32_t>(dvd("DVDConvertPathToEntrynum", text));
    };
    const auto open = [&](const char* p, uint32_t fi) {
        std::strcpy(reinterpret_cast<char*>(rt::guest_ptr(text)), p);
        return dvd("DVDOpen", text, fi);
    };
    dvd("DVDInit");
    dvd("DVDInit");  // the game calls it again after OSInit

    // ---- lookups
    check(entry("/a.bin") == 1 && entry("a.bin") == 1 && entry("/Course") == 2, "a path resolves to its entry number, from the root");
    check(entry("/course/LUIGI.ARC") == 3 && entry("Course/./big.bin") == 4, "names compare without case; '.' stays in place");
    check(entry("/Course/../a.bin") == 1 && entry("/") == 0, "'..' steps up; '/' is the root");
    check(entry("/Course/none.bin") == -1 && entry("/a.bin/x") == -1, "a missing file, and a file used as a directory, aren't found");
    check(open("/Course/Luigi.arc", file1) == 1 && rt::peek32(file1 + 0x30) == kLuigi && rt::peek32(file1 + 0x34) == kLuigiSize &&
              rt::peek32(file1 + 0x0C) == 0,
          "DVDOpen fills the file's start (in bytes), its length and an idle state");
    check(open("/nothing", file2) == 0 && dvd("DVDFastOpen", 2, file2) == 0 && dvd("DVDFastOpen", 99, file2) == 0,
          "there's no opening a missing file, a directory or a number past the table");
    check(dvd("DVDFastOpen", 4, file2) == 1 && rt::peek32(file2 + 0x30) == kBig && dvd("DVDFastOpen", 1, file3) == 1, "DVDFastOpen by entry number");

    // ---- an asynchronous read: busy until the drive's time has passed, then the data and the callback
    std::memset(rt::guest_ptr(buffer), 0xEE, 0x2000);
    check(dvd("DVDReadAsyncPrio", file1, buffer, 0x1220, 0x20, cb, 2) == 1 && rt::peek32(file1 + 0x0C) == 1 && dvd("DVDGetDriveStatus") == 1,
          "an asynchronous read returns at once with its block busy");
    check(callbacks.empty() && *rt::guest_ptr(buffer) == 0xEE && rt::peek32(file1 + 0x20) == 0, "nothing has arrived yet: no data, no callback");
    // A second and a third queue behind it; the third has the better priority.
    dvd("DVDReadAsyncPrio", file2, buffer2, 0x40000, 0, cb + 4, 3);
    dvd("DVDReadAsyncPrio", file3, buffer2 + 0x50000, 96, 0, cb + 8, 1);
    check(rt::peek32(file2 + 0x0C) == 2 && rt::peek32(file3 + 0x0C) == 2 && dvd("DVDGetCommandBlockStatus", file2) == 2, "the next reads wait their turn");
    const uint64_t t0 = now;
    advance(cpu, ms(1));  // 0x1220 bytes take over 2 ms at the start of the disc (2 MiB a second)
    check(callbacks.empty() && rt::peek32(file1 + 0x0C) == 1, "a millisecond on, it's still reading");
    advance(cpu, ms(400));
    check(callbacks.size() >= 1 && callbacks[0].fn == cb && callbacks[0].a0 == 0x1220 && callbacks[0].a1 == file1 &&
              callbacks[0].at - t0 >= ms(2),
          "the callback comes with the length and the file, after the transfer's time");
    check(holds(buffer, kLuigi + 0x20, kLuigiSize - 0x20) && rt::peek32(file1 + 0x20) == 0x1220 && rt::peek32(file1 + 0x0C) == 0,
          "the data is there, the count is the length, the block is done");
    check(callbacks.size() == 3 && callbacks[1].fn == cb + 8 && callbacks[2].fn == cb + 4, "waiting reads run by priority, one at a time");
    check(callbacks[2].at - callbacks[1].at >= 0x40000 * mkdd::kTimerHz / (4u << 20), "a larger read takes its bytes' time (under 4 MiB a second)");
    check(holds(buffer2, kBig, 0x40000) && holds(buffer2 + 0x50000, kA, 96) && dvd("DVDGetDriveStatus") == 0, "both arrived; the drive is idle");

    // ---- a synchronous read sleeps its thread until the drive is done
    callbacks.clear();
    const uint64_t t1 = now;
    check(dvd("DVDReadPrio", file2, buffer, 0x8000, 0x10000, 2) == 0x8000 && holds(buffer, kBig + 0x10000, 0x8000) && now > t1 && callbacks.empty(),
          "a synchronous read returns the bytes read, after the drive's time, with no callback");
    check(dvd("DVDReadPrio", file3, buffer, 128, 0, 2) == 128 && holds(buffer, kA, kASize) && *rt::guest_ptr(buffer + kASize) == 0,
          "a read may run past a file's end to the drive's 32 bytes: the disc's own bytes there (none)");

    // ---- cancelling
    dvd("DVDReadAsyncPrio", file1, buffer, 0x1000, 0, cb, 2);
    dvd("DVDReadAsyncPrio", file2, buffer2, 0x1000, 0, cb + 4, 2);
    check(dvd("DVDCancel", file2) == 0 && rt::peek32(file2 + 0x0C) == 10 && callbacks.size() == 1 && callbacks[0].fn == cb + 4 &&
              callbacks[0].a0 == 0xFFFFFFFDu,
          "a waiting read is cancelled at once: state 10, its callback hears -3");
    check(dvd("DVDClose", file1) == 1 && rt::peek32(file1 + 0x0C) == 10 && dvd("DVDGetDriveStatus") == 0, "closing a file cancels its read in flight");
    advance(cpu, ms(500));
    check(callbacks.size() == 2 && callbacks[1].a0 == 0xFFFFFFFDu, "and nothing more comes of either");
    check(dvd("DVDCheckDisk") == 1 && dvd("DVDGetCurrentDiskID") == 0x80000000u, "the disc is in; its ID is where the boot ROM read it");

    disc.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
