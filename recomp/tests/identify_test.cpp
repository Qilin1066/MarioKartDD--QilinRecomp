// `qilin identify` (recomp/src/identify.hpp), the downloads' launcher's
// contract, against disc images made here: the supported build as a plain
// image and as a CISO (exit 0), another revision, region, game and a changed
// main.dol, a Wii disc and the wrong known build (exit 3), and what can't be
// read: RVZ, WIA and GCZ images, a folder, a missing file, a two-byte file
// with a Unicode name, a cut-short image and an unknown --build (exit 2).
// Every message is a sentence without the file's path. No game files (runs in CI).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "identify.hpp"
#include "qilin/disc/disc.hpp"
#include "sha256.hpp"

using namespace qilin;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

using Bytes = std::vector<uint8_t>;

void put32(Bytes& d, size_t at, uint32_t v) {
    d[at] = static_cast<uint8_t>(v >> 24);
    d[at + 1] = static_cast<uint8_t>(v >> 16);
    d[at + 2] = static_cast<uint8_t>(v >> 8);
    d[at + 3] = static_cast<uint8_t>(v);
}
void put(Bytes& d, size_t at, std::string_view text) { std::memcpy(d.data() + at, text.data(), text.size()); }

Bytes pattern(size_t n, unsigned seed) {
    Bytes v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 31 + seed * 17) % 255 + 1);
    return v;
}

void write(const fs::path& file, std::span<const uint8_t> bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::println(stderr, "cannot write a test file");
        std::exit(1);
    }
}

constexpr uint32_t kDolAt = 0x2600, kDolSize = 0x130, kFstAt = 0x3000;

// A small GameCube disc: its header, bi2, an apploader, a DOL (text0 and
// data0; `seed` makes its code, so its SHA-256) and a file system table with
// the root alone.
Bytes disc_image(std::string_view id, unsigned revision, unsigned seed, uint32_t magic_at_18 = 0) {
    Bytes image(0x4000);
    put(image, 0, id);
    image[7] = static_cast<uint8_t>(revision);
    if (magic_at_18) put32(image, 0x18, magic_at_18);
    else put32(image, 0x1C, disc::kMagic);
    put(image, 0x20, "Synthetic Kart");
    std::ranges::copy(pattern(disc::kBi2Size, 9), image.begin() + disc::kBi2Offset);
    put(image, disc::kApploaderOffset, "2026/11/28");
    put32(image, disc::kApploaderOffset + 0x14, 0x40);
    put32(image, disc::kApploaderOffset + 0x18, 0x20);
    std::ranges::copy(pattern(0x60, 7), image.begin() + disc::kApploaderOffset + 0x20);
    put32(image, kDolAt + 0x00, 0x100), put32(image, kDolAt + 0x48, 0x80003100), put32(image, kDolAt + 0x90, 0x20);
    put32(image, kDolAt + 0x1C, 0x120), put32(image, kDolAt + 0x64, 0x80004000), put32(image, kDolAt + 0xAC, 0x10);
    put32(image, kDolAt + 0xE0, 0x80003100);
    std::ranges::copy(pattern(0x30, seed), image.begin() + kDolAt + 0x100);
    // The root directory, one entry, and its empty name.
    put32(image, kFstAt, 1u << 24);
    put32(image, kFstAt + 4, 0);
    put32(image, kFstAt + 8, 1);
    put32(image, 0x420, kDolAt);
    put32(image, 0x424, kFstAt);
    put32(image, 0x428, disc::kFstEntrySize + 1);
    put32(image, 0x42C, disc::kFstEntrySize + 1);
    return image;
}

std::string dol_sha(const Bytes& image) { return sha256_hex(std::span(image).subspan(kDolAt, kDolSize)); }

// The image as a CISO: 0x1000-byte blocks, those all zero left out.
Bytes ciso(const Bytes& image) {
    constexpr uint32_t block = 0x1000;
    Bytes out(0x8000);
    put(out, 0, "CISO");
    out[5] = block >> 8;
    for (size_t b = 0; b * block < image.size(); ++b) {
        Bytes bytes(block);
        const size_t from = b * block, n = std::min<size_t>(block, image.size() - from);
        std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(from), n, bytes.begin());
        if (b && std::ranges::all_of(bytes, [](uint8_t x) { return x == 0; })) continue;
        out[8 + b] = 1;
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// What every answer must be: the exit code agreeing with ok, a sentence
// ending with a full stop, no path in it, and JSON that names all five keys.
void check_shape(const Identity& i, const fs::path& scratch) {
    CHECK(i.ok == (i.code == 0));
    CHECK(i.code == 0 || i.code == 2 || i.code == 3);
    CHECK(!i.message.empty() && i.message.back() == '.');
    const std::u8string where = scratch.u8string();
    CHECK(!has(i.message, std::string(where.begin(), where.end())));
    const std::string json = to_json(i);
    CHECK(json.starts_with(i.ok ? "{\"ok\": true, " : "{\"ok\": false, ") && json.ends_with("}\n"));
    for (const char* key : {"\"title\": ", "\"id\": ", "\"version\": ", "\"message\": "}) CHECK(has(json, key));
}

}  // namespace

int main() {
    const fs::path scratch = fs::temp_directory_path() / ("qilin-identify-test-" + std::to_string(std::rand() % 100000));
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);

    const Bytes right = disc_image("GM4E01", 0, 1);
    const Bytes rev1 = disc_image("GM4E01", 1, 2);
    const Bytes europe = disc_image("GM4P01", 0, 3);
    const Bytes other = disc_image("GALE01", 0, 4);
    const Bytes changed = disc_image("GM4E01", 0, 5);
    const Bytes wii = disc_image("RMCE01", 0, 6, 0x5D1C9EA3u);
    const std::string right_sha = dol_sha(right), rev1_sha = dol_sha(rev1);
    // The builds this test's recompiler knows: the right one, and revision 1 for the wrong-build case.
    const KnownBuild builds[] = {
        {right_sha, "GM4E01", 0, "Mario Kart: Double Dash!! (USA)"},
        {rev1_sha, "GM4E01", 1, "Mario Kart: Double Dash!! (USA, Rev 1)"},
    };
    const std::string_view want = "Mario Kart: Double Dash!! (USA)";
    auto at = [&](const char* name, const Bytes& bytes) {
        const fs::path p = scratch / name;
        write(p, bytes);
        return p;
    };
    auto run = [&](const fs::path& p, std::string_view w = "Mario Kart: Double Dash!! (USA)") {
        const Identity i = identify(p, w, builds);
        check_shape(i, scratch);
        return i;
    };

    // The supported build: as a plain image (any name), and as a CISO.
    for (const fs::path& p : {at("right.iso", right), at("right.gcm", right), at("right.ciso", ciso(right))}) {
        const Identity i = run(p);
        CHECK(i.code == 0 && i.ok);
        CHECK(i.id == "GM4E01" && i.version == "0" && i.title == want);
        CHECK(i.message == "This is Mario Kart: Double Dash!! (USA), disc GM4E01 revision 0: the right disc.");
    }
    // Without --build, any known build is right.
    CHECK(identify(scratch / "right.iso", "", builds).code == 0);
    // A known build, but not the one this download wants.
    {
        const Identity i = run(at("rev1.iso", rev1));
        CHECK(i.code == 3 && i.id == "GM4E01" && i.version == "1");
        CHECK(has(i.message, "revision 1") && has(i.message, "This download is for Mario Kart: Double Dash!! (USA), disc GM4E01 revision 0."));
    }
    // Another revision this port doesn't know.
    {
        Bytes rev2 = disc_image("GM4E01", 2, 7);
        const Identity i = run(at("rev2.iso", rev2));
        CHECK(i.code == 3 && i.version == "2");
        CHECK(i.message == "This is Mario Kart: Double Dash!! (USA), disc GM4E01 revision 2. This download is for revision 0 of that disc.");
    }
    // Another region, another game, a changed main.dol, a Wii disc.
    {
        const Identity i = run(at("europe.iso", europe));
        CHECK(i.code == 3 && i.id == "GM4P01" && has(i.message, "Mario Kart: Double Dash!! for Europe and Australia (GM4P01)."));
    }
    {
        const Identity i = run(at("other.iso", other));
        CHECK(i.code == 3 && i.id == "GALE01" && i.title == "Synthetic Kart" && has(i.message, "another game"));
    }
    {
        const Identity i = run(at("changed.iso", changed));
        CHECK(i.code == 3 && has(i.message, "isn't that disc's") && has(i.message, dol_sha(changed)));
    }
    {
        const Identity i = run(at("wii.iso", wii));
        CHECK(i.code == 3 && i.id == "RMCE01" && has(i.message, "a Wii disc"));
    }
    {
        Bytes wbfs(0x1000);
        put(wbfs, 0, "WBFS");
        CHECK(run(at("game.wbfs", wbfs)).code == 3);
    }

    // What can't be read: compressed images, with the way to convert them.
    for (const char* magic : {"RVZ\x01", "WIA\x01"}) {
        Bytes image = right;
        put(image, 0, std::string_view(magic, 4));
        const Identity i = run(at("compressed.rvz", image));
        CHECK(i.code == 2 && has(i.message, "RVZ or WIA") && has(i.message, "Convert it in Dolphin"));
    }
    {
        Bytes image = right;
        put(image, 0, std::string_view("\x01\xC0\x0B\xB1", 4));
        const Identity i = run(at("compressed.gcz", image));
        CHECK(i.code == 2 && has(i.message, "GCZ") && has(i.message, "Convert it in Dolphin"));
    }
    {
        Bytes image = right;
        put(image, 0x200, "NKIT");
        CHECK(run(at("game.nkit.iso", image)).code == 2);
    }
    // A folder (an extracted disc's, or any), a missing file, a file that
    // isn't a disc (two bytes, with a Unicode name), an image cut short.
    {
        fs::create_directories(scratch / "extracted" / "sys");
        write(scratch / "extracted" / "sys" / "main.dol", std::span(right).subspan(kDolAt, kDolSize));
        const Identity i = run(scratch / "extracted");
        CHECK(i.code == 2 && has(i.message, "extracted into a folder"));
        fs::create_directories(scratch / "empty");
        CHECK(run(scratch / "empty").code == 2);
    }
    {
        const Identity i = run(scratch / "missing.iso");
        CHECK(i.code == 2 && has(i.message, "missing.iso"));
    }
    {
        const fs::path unicode = scratch / fs::path(u8"ディスク été.iso");
        write(unicode, Bytes{0x09, 0x00});
        const Identity i = run(unicode);
        CHECK(i.code == 2 && i.message == "This isn't a GameCube disc image. Choose your disc's .iso, .gcm or .ciso file.");
    }
    {
        const Bytes cut(right.begin(), right.begin() + 0x2610);  // the header and part of the DOL's header
        const Identity i = run(at("cut.iso", cut));
        CHECK(i.code == 2 && has(i.message, "can't be read"));
    }
    // An unknown --build: a broken download, said plainly.
    CHECK(run(scratch / "right.iso", "Mario Kart: Double Dash!! (Mars)").code == 2);

    // JSON: quotes, backslashes and control characters escaped; UTF-8 kept.
    {
        Identity i;
        i.code = 3;
        i.title = "a \"b\" \\ c\n\t";
        i.message = "\xC3\xA9t\xC3\xA9.";
        CHECK(to_json(i) == "{\"ok\": false, \"title\": \"a \\\"b\\\" \\\\ c\\n\\t\", \"id\": \"\", \"version\": \"\", \"message\": \"\xC3\xA9t\xC3\xA9.\"}\n");
    }

    fs::remove_all(scratch, ec);
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("identify: all checks passed");
    return 0;
}
