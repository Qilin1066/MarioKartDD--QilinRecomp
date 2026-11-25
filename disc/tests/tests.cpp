// The disc library against discs made here: one synthetic disc written as a
// plain image, a CISO with absent blocks and an extracted folder must read the
// same in every way; malformed ones must be refused with a message that says
// what is wrong. No game files (runs in CI).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "qilin/disc/disc.hpp"

using namespace qilin::disc;
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
uint32_t get32(const Bytes& d, size_t at) {
    return uint32_t{d[at]} << 24 | uint32_t{d[at + 1]} << 16 | uint32_t{d[at + 2]} << 8 | uint32_t{d[at + 3]};
}
void put(Bytes& d, size_t at, std::string_view text) { std::memcpy(d.data() + at, text.data(), text.size()); }

// Never zero, so a present byte is told from a gap.
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

// ---------------------------------------------------------------- the synthetic disc

struct Node {
    std::string name;
    bool directory = false;
    uint32_t offset = 0;
    Bytes data;
    std::vector<Node> children;
};

Node make_file(std::string name, uint32_t offset, Bytes data) { return {std::move(name), false, offset, std::move(data), {}}; }
Node make_folder(std::string name, std::vector<Node> children) { return {std::move(name), true, 0, {}, std::move(children)}; }

constexpr uint32_t kDolAt = 0x2600, kFstAt = 0x3000, kManyAt = 0x10000, kMany = 80, kManySize = 33;

Node tree() {
    std::vector<Node> many;
    for (unsigned i = 0; i < kMany; ++i)  // more than the folder reader keeps open; each touching the next
        many.push_back(make_file("f" + std::to_string(i) + ".bin", kManyAt + i * kManySize, pattern(kManySize, 100 + i)));
    return make_folder("", {
        make_file("a.bin", 0x4000, pattern(100, 1)),
        make_file("empty.bin", 0x4064, {}),              // zero bytes, at the offset of the next
        make_file("b.bin", 0x4064, pattern(5000, 2)),    // touches a.bin
        make_folder("Course", {
            make_file("Luigi.arc", 0x6000, pattern(0x3000, 3)),  // after a gap
            make_folder("Deep", {make_file("zero.dat", 0xA000, Bytes(0x5000))}),  // all zero: whole CISO blocks absent
            make_folder("Empty", {}),
        }),
        make_folder("many", std::move(many)),
        make_file("z_last.txt", 0x11000, pattern(7, 4)),
    });
}

void flatten(const Node& node, uint32_t parent, Bytes& entries, Bytes& names) {
    const size_t at = entries.size();
    const uint32_t index = static_cast<uint32_t>(at / 12);
    entries.resize(at + 12);
    put32(entries, at, static_cast<uint32_t>(names.size()) | (node.directory ? 1u << 24 : 0));
    names.insert(names.end(), node.name.begin(), node.name.end());
    names.push_back(0);
    if (!node.directory) {
        put32(entries, at + 4, node.offset);
        put32(entries, at + 8, static_cast<uint32_t>(node.data.size()));
        return;
    }
    put32(entries, at + 4, parent);
    for (const Node& child : node.children) flatten(child, index, entries, names);
    put32(entries, at + 8, static_cast<uint32_t>(entries.size() / 12));
}

void place(const Node& node, Bytes& image) {
    if (node.directory) {
        for (const Node& child : node.children) place(child, image);
    } else if (!node.data.empty()) {
        if (image.size() < node.offset + node.data.size()) image.resize(node.offset + node.data.size());
        std::ranges::copy(node.data, image.begin() + node.offset);
    }
}

// The whole disc as a plain image, cut after its last file.
Bytes build(const Node& root) {
    Bytes image(0x4000);
    put(image, 0, "GTSE01");
    image[6] = 1;  // disc number
    image[7] = 2;  // revision
    put32(image, 0x1C, kMagic);
    put(image, 0x20, "Synthetic \"Disc\"");
    std::ranges::copy(pattern(kBi2Size, 9), image.begin() + kBi2Offset);
    // Apploader: 0x20 of header, 0x40 of code, 0x20 of trailer.
    put(image, kApploaderOffset, "2026/11/25");
    put32(image, kApploaderOffset + 0x14, 0x40);
    put32(image, kApploaderOffset + 0x18, 0x20);
    std::ranges::copy(pattern(0x60, 7), image.begin() + kApploaderOffset + 0x20);
    // DOL: text0 and data0, the entry point at the start of the text.
    put32(image, kDolAt + 0x00, 0x100), put32(image, kDolAt + 0x48, 0x80003100), put32(image, kDolAt + 0x90, 0x20);
    put32(image, kDolAt + 0x1C, 0x120), put32(image, kDolAt + 0x64, 0x80004000), put32(image, kDolAt + 0xAC, 0x10);
    put32(image, kDolAt + 0xE0, 0x80003100);
    std::ranges::copy(pattern(0x30, 5), image.begin() + kDolAt + 0x100);

    Bytes fst, names;
    flatten(root, 0, fst, names);
    fst.insert(fst.end(), names.begin(), names.end());
    if (fst.size() > 0x1000) std::println(stderr, "the test's FST outgrew its place"), std::exit(1);
    std::ranges::copy(fst, image.begin() + kFstAt);
    put32(image, 0x420, kDolAt);
    put32(image, 0x424, kFstAt);
    put32(image, 0x428, static_cast<uint32_t>(fst.size()));
    put32(image, 0x42C, static_cast<uint32_t>(fst.size()));
    place(root, image);
    return image;
}

constexpr uint32_t kBlock = 0x1000;  // far smaller than any real CISO's, so a small disc has holes

// A CISO of the image: blocks that are all zero are left out. Returns how many were.
unsigned write_ciso(const fs::path& path, const Bytes& image, uint32_t block_size = kBlock) {
    Bytes out(0x8000);
    put(out, 0, "CISO");
    out[4] = static_cast<uint8_t>(block_size), out[5] = static_cast<uint8_t>(block_size >> 8);
    out[6] = static_cast<uint8_t>(block_size >> 16), out[7] = static_cast<uint8_t>(block_size >> 24);
    unsigned absent = 0;
    for (size_t block = 0; block * block_size < image.size(); ++block) {
        Bytes bytes(block_size);
        const size_t from = block * block_size, n = std::min<size_t>(block_size, image.size() - from);
        std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(from), n, bytes.begin());
        if (block && std::ranges::all_of(bytes, [](uint8_t b) { return b == 0; })) {
            ++absent;
            continue;
        }
        out[8 + block] = 1;
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    write(path, out);
    return absent;
}

void write_files(const fs::path& dir, const Node& node) {
    for (const Node& child : node.children) {
        if (child.directory) {
            fs::create_directory(dir / child.name);
            write_files(dir / child.name, child);
        } else {
            write(dir / child.name, child.data);
        }
    }
}

void write_folder(const fs::path& dir, const Bytes& image, const Node& root) {
    fs::remove_all(dir);
    fs::create_directories(dir / "sys");
    fs::create_directories(dir / "files");
    const std::span<const uint8_t> all(image);
    write(dir / "sys" / "boot.bin", all.subspan(0, kBootSize));
    write(dir / "sys" / "bi2.bin", all.subspan(kBi2Offset, kBi2Size));
    write(dir / "sys" / "apploader.img", all.subspan(kApploaderOffset, 0x80));
    write(dir / "sys" / "main.dol", all.subspan(kDolAt, 0x130));
    write(dir / "sys" / "fst.bin", all.subspan(kFstAt, get32(image, 0x428)));
    write_files(dir / "files", root);
}

// Every file and folder under dir, by its path from there.
std::map<std::string, Bytes> contents(const fs::path& dir) {
    std::map<std::string, Bytes> found;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir)) {
        const std::string key = entry.path().lexically_relative(dir).generic_string();
        if (entry.is_directory()) {
            found[key + "/"];
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        found[key] = Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    return found;
}

std::string text(const fs::path& path) {
    const std::u8string u = path.u8string();
    return {u.begin(), u.end()};
}

// ---------------------------------------------------------------- the three forms agree

Bytes expected(const Bytes& image, uint64_t offset, size_t n) {
    Bytes out(n);
    for (size_t i = 0; i < n; ++i)
        if (offset + i < image.size()) out[i] = image[static_cast<size_t>(offset + i)];
    return out;
}

bool reads(const Disc& disc, const Bytes& image, uint64_t offset, size_t n) {
    Bytes got(n, 0xEE);
    const auto r = disc.read(offset, got);
    if (!r) {
        std::println(stderr, "read of {} at {:#x}: {}", n, offset, r.error());
        return false;
    }
    return got == expected(image, offset, n);
}

void check_same(const Disc& disc, const Disc& reference, const Bytes& image) {
    const Header& h = disc.header();
    CHECK(h.game_id == "GTSE01" && h.maker == "01" && h.disc_number == 1 && h.revision == 2);
    CHECK(h.magic == kMagic && h.title == "Synthetic \"Disc\"" && h.apploader_date == "2026/11/25");
    CHECK(h.dol_offset == kDolAt && h.fst_offset == kFstAt && h.fst_size == get32(image, 0x428));
    CHECK(disc.size() == kDiscSize);
    CHECK(std::ranges::equal(disc.boot_bin(), std::span(image).first(kBootSize)));
    CHECK(std::ranges::equal(disc.bi2_bin(), std::span(image).subspan(kBi2Offset, kBi2Size)));
    CHECK(std::ranges::equal(disc.apploader(), std::span(image).subspan(kApploaderOffset, 0x80)));
    CHECK(std::ranges::equal(disc.fst(), std::span(image).subspan(kFstAt, h.fst_size)));
    CHECK(disc.dol_range().offset == kDolAt && disc.dol_range().size == 0x130);
    CHECK(disc.apploader_range().offset == kApploaderOffset && disc.fst_range().size == h.fst_size);
    const auto dol = disc.dol();
    CHECK(dol && std::ranges::equal(*dol, std::span(image).subspan(kDolAt, 0x130)));

    CHECK(disc.file_count() == 6 + kMany && disc.file_bytes() == 100 + 5000 + 0x3000 + 0x5000 + kMany * kManySize + 7);
    CHECK(disc.entries().size() == reference.entries().size());
    for (size_t i = 0; i < disc.entries().size() && i < reference.entries().size(); ++i) {
        const Entry& a = disc.entries()[i];
        const Entry& b = reference.entries()[i];
        CHECK(a.directory == b.directory && a.name == b.name && a.path == b.path && a.parent == b.parent &&
              a.offset == b.offset && a.size == b.size && a.end == b.end);
    }

    // Paths: as written, in the other case, with a slash in front; directories; absent ones.
    const Entry* luigi = disc.find("Course/Luigi.arc");
    CHECK(luigi && !luigi->directory && luigi->offset == 0x6000 && luigi->size == 0x3000 && luigi->name == "Luigi.arc");
    CHECK(disc.find("/course/LUIGI.ARC") == luigi);
    const Entry* course = disc.find("Course");
    CHECK(course && course->directory && luigi && &disc.entries()[luigi->parent] == course);
    CHECK(disc.find("") == &disc.entries()[0] && disc.find("/") == &disc.entries()[0]);
    const Entry* empty_dir = disc.find("Course/Empty");
    CHECK(empty_dir && empty_dir->directory && empty_dir->end == static_cast<uint32_t>(empty_dir - disc.entries().data()) + 1);
    const Entry* zero = disc.find("Course/Deep/zero.dat");
    CHECK(zero && zero->path == "Course/Deep/zero.dat" && disc.entries()[zero->parent].path == "Course/Deep");
    const Entry* empty = disc.find("empty.bin");
    CHECK(empty && empty->size == 0 && empty->offset == 0x4064);
    CHECK(!disc.find("Course/Luigi") && !disc.find("Luigi.arc") && !disc.find("Course/") && !disc.find("nothing"));

    // By offset: within a file, across two that touch, into and out of gaps, absent blocks, the system areas.
    CHECK(reads(disc, image, 0, 0x4000));
    CHECK(reads(disc, image, 0x4000, 100));
    CHECK(reads(disc, image, 0x4060, 8));      // a.bin into b.bin
    CHECK(reads(disc, image, 0x3FF0, 0x2100)); // the FST's tail, a gap, a.bin, b.bin, a gap, Luigi.arc
    CHECK(reads(disc, image, 0x53E0, 0x40));   // b.bin's end into the gap
    CHECK(reads(disc, image, 0x5800, 0x100));  // only gap
    CHECK(reads(disc, image, 0x8FF0, 0x6100)); // Luigi.arc, zero.dat (absent in the CISO), the gap after
    CHECK(reads(disc, image, kManyAt - 5, kMany * kManySize + 10));  // 80 files in one read
    CHECK(reads(disc, image, 0x10FFE, 0x20));  // the last file and past the image's end
    CHECK(reads(disc, image, 0x11007, 0x1000));
    CHECK(reads(disc, image, 0, 0x12000));
    CHECK(reads(disc, image, 0x4000, 0));
    // The end of the disc: up to it reads (as zeros), across it does not.
    CHECK(reads(disc, image, kDiscSize - 16, 16));
    CHECK(reads(disc, image, kDiscSize, 0));
    uint8_t few[17];
    const auto across = disc.read(kDiscSize - 16, few);
    CHECK(!across && across.error().contains("past the end of the disc") && across.error().contains(text(disc.path())));
    CHECK(!disc.read(kDiscSize + 1, std::span<uint8_t>(few, size_t{0})) && !disc.read(~uint64_t{0}, few));

    // Within a file.
    Bytes part(0x100);
    CHECK(luigi && disc.read_file(*luigi, 0x2F00, part) && part == expected(image, 0x6000 + 0x2F00, 0x100));
    const auto beyond = luigi ? disc.read_file(*luigi, 0x2F01, part) : Result<void>{};
    CHECK(!beyond && beyond.error().contains("Course/Luigi.arc"));
    CHECK(course && !disc.read_file(*course, 0, std::span(part).first(0)));
    CHECK(empty && disc.read_file(*empty, 0, std::span(part).first(0)));

    // From several threads at once.
    std::vector<std::thread> threads;
    std::vector<int> wrong(4);
    for (unsigned t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            std::minstd_rand random(t + 1);
            for (int i = 0; i < 400; ++i) {
                const uint64_t offset = random() % 0x12000;
                const size_t n = random() % 0x900;
                wrong[t] += !reads(disc, image, offset, n);
            }
        });
    for (std::thread& thread : threads) thread.join();
    CHECK(wrong == std::vector<int>(4));

    CHECK(verify(disc).empty());
}

// ---------------------------------------------------------------- malformed discs

void refused(const fs::path& path, std::string_view why, int line) {
    const auto disc = Disc::open(path);
    if (disc) {
        std::println(stderr, "{}:{}: opened, though it should be refused ({})", __FILE__, line, why);
        ++failures;
    } else if (!disc.error().contains(why) || !disc.error().contains(text(path.filename()))) {
        std::println(stderr, "{}:{}: refused with \"{}\": expected the file's name and \"{}\"", __FILE__, line, disc.error(), why);
        ++failures;
    }
}
#define REFUSED(path, why) refused(path, why, __LINE__)

uint32_t index_of(const Disc& disc, std::string_view path) {
    return static_cast<uint32_t>(disc.find(path) - disc.entries().data());
}

void test_malformed(const fs::path& dir, const Bytes& good, const Node& root, const Disc& reference) {
    const fs::path iso = dir / "bad disc.iso";
    const auto with = [&](auto&& change) {
        Bytes image = good;
        change(image);
        write(iso, image);
    };
    const uint32_t count = static_cast<uint32_t>(reference.entries().size()), fst_size = get32(good, 0x428);
    const auto entry = [&](std::string_view path) { return kFstAt + 12 * index_of(reference, path); };

    // Not a disc, or not one read here.
    REFUSED(dir / "no such.iso", "no such file");
    with([](Bytes& d) { d[0x1C] ^= 0xFF; });
    REFUSED(iso, "magic at 0x1C");
    with([](Bytes& d) { put32(d, 0x18, 0x5D1C9EA3), put32(d, 0x1C, 0); });
    REFUSED(iso, "Wii disc");
    with([](Bytes& d) { put(d, 0x200, "NKIT v01"); });
    REFUSED(iso, "NKit");
    with([](Bytes& d) { d.resize(100); });
    REFUSED(iso, "too short");
    with([](Bytes& d) { d.clear(); });
    REFUSED(iso, "too short");
    with([](Bytes& d) { put(d, 0, "\x01\xC0\x0B\xB1"); });
    REFUSED(iso, "GCZ");
    with([](Bytes& d) { put(d, 0, "RVZ\x01"); });
    REFUSED(iso, "RVZ");
    with([](Bytes& d) { put(d, 0, "WIA\x01"); });
    REFUSED(iso, "WIA");
    with([](Bytes& d) { put(d, 0, "WBFS"); });
    REFUSED(iso, "WBFS");

    // The system areas.
    with([](Bytes& d) { d.resize(0x8000); });
    REFUSED(iso, "cut short");
    with([](Bytes& d) { put32(d, kApploaderOffset + 0x14, 0xFFFFFFFF); });
    REFUSED(iso, "apploader");
    with([](Bytes& d) { put32(d, kApploaderOffset + 0x14, 0x100000); });
    REFUSED(iso, "the apploader (at 0x2440");
    with([](Bytes& d) { put32(d, 0x420, 0x7FFFFFF0); });
    REFUSED(iso, "the DOL's header");
    with([](Bytes& d) { put32(d, kDolAt + 0x90, 0), put32(d, kDolAt + 0xAC, 0); });
    REFUSED(iso, "no sections");
    with([](Bytes& d) { put32(d, kDolAt + 0x90, 0x100000); });
    REFUSED(iso, "the DOL (at 0x2600");

    // The file system table.
    with([](Bytes& d) { put32(d, 0x424, 0x7FFFFFF0); });
    REFUSED(iso, "the file system table (at 0x7FFFFFF0");
    with([](Bytes& d) { put32(d, 0x428, 0xF0000000); });
    REFUSED(iso, "lies outside the image");
    with([](Bytes& d) { put32(d, 0x428, 4); });
    REFUSED(iso, "shorter than its root entry");
    with([](Bytes& d) { d[kFstAt] = 0; });
    REFUSED(iso, "root entry is not a directory");
    with([](Bytes& d) { put32(d, kFstAt + 8, 0x10000); });
    REFUSED(iso, "cannot hold them");
    with([](Bytes& d) { put32(d, kFstAt + 8, 0); });
    REFUSED(iso, "cannot hold them");
    with([](Bytes& d) { d[kFstAt + 12] = 2; });
    REFUSED(iso, "neither file nor directory");
    with([](Bytes& d) { put32(d, kFstAt + 12, 0x00FFFFFF); });
    REFUSED(iso, "its name is at 0xFFFFFF");
    with([&](Bytes& d) { put32(d, 0x428, fst_size - 1); });  // the last name loses its terminator
    REFUSED(iso, "not terminated");
    with([&](Bytes& d) { put32(d, kFstAt + 12, fst_size - 12 * count - 1); });  // the last name's terminator
    REFUSED(iso, "empty name");
    // Directories: an end at itself (a walk would never leave), before itself, past its parent's, past the table.
    const uint32_t course = index_of(reference, "Course"), deep = index_of(reference, "Course/Deep");
    with([&](Bytes& d) { put32(d, entry("Course") + 8, course); });
    REFUSED(iso, "must end after itself");
    with([&](Bytes& d) { put32(d, entry("Course") + 8, 1); });
    REFUSED(iso, "must end after itself");
    with([&](Bytes& d) { put32(d, entry("Course/Deep") + 8, reference.entries()[course].end + 1); });
    REFUSED(iso, "where its parent does");
    with([&](Bytes& d) { put32(d, entry("many") + 8, count + 5); });
    REFUSED(iso, "where its parent does");
    with([&](Bytes& d) { put32(d, entry("Course/Deep") + 4, deep); });  // its own parent
    REFUSED(iso, "as its parent");
    with([&](Bytes& d) { put32(d, entry("Course") + 4, 3); });
    REFUSED(iso, "as its parent");
    // Files outside the image.
    with([&](Bytes& d) { put32(d, entry("z_last.txt") + 8, 0x100000); });
    REFUSED(iso, "file z_last.txt");
    with([&](Bytes& d) { put32(d, entry("a.bin") + 4, 0xFFFFFFF0); });
    REFUSED(iso, "file a.bin");

    // Open, but verify() objects: files sharing bytes, the DOL's entry point and addresses, a name twice.
    const auto problems = [&](auto&& change) {
        with(change);
        const auto disc = Disc::open(iso);
        CHECK(disc);
        std::string all;
        if (disc)
            for (const std::string& problem : verify(*disc)) all += problem + "\n";
        return all;
    };
    CHECK(problems([&](Bytes& d) { put32(d, entry("b.bin") + 4, 0x4000); }).contains("file a.bin (0x4000 to 0x4064) and file b.bin"));
    CHECK(problems([&](Bytes& d) { put32(d, entry("a.bin") + 4, 0x3000); }).contains("the file system table (0x3000"));
    CHECK(problems([&](Bytes& d) { put32(d, entry("a.bin") + 4, 0x2610); }).contains("main.dol (0x2600 to 0x2730) and file a.bin"));
    CHECK(problems([](Bytes& d) { put32(d, kDolAt + 0xE0, 0x80004000); }).contains("entry point 0x80004000"));
    CHECK(problems([](Bytes& d) { put32(d, kDolAt + 0x48, 0x81800000); }).contains("text0 loads at 0x81800000"));
    CHECK(problems([](Bytes& d) { put32(d, kDolAt + 0x1C, 0x110); }).contains("overlap in the file"));
    CHECK(problems([](Bytes& d) { put32(d, kDolAt + 0x00, 0x80); }).contains("inside the header"));
    CHECK(problems([](Bytes& d) { put32(d, 0x42C, 16); }).contains("more than the maximum"));
    CHECK(problems([&](Bytes& d) { d[kFstAt + 12 * count + (get32(d, entry("b.bin")) & 0xFFFFFF)] = 'A'; }).contains("two entries are named"));
    // A file past a disc's end can only be told in a CISO (a plain image would have to be that long),
    // and one with blocks large enough for its map to reach there.
    {
        Bytes image = good;
        put32(image, entry("z_last.txt") + 4, 0x60000000);
        write_ciso(dir / "far.ciso", image, 0x10000);
        const auto disc = Disc::open(dir / "far.ciso");
        CHECK(disc && disc->size() == 0x60000000u + 7);
        const auto found = disc ? verify(*disc) : std::vector<std::string>{};
        CHECK(found.size() == 1 && found[0].contains("runs past the end of a GameCube disc"));
        uint8_t byte = 1;
        CHECK(disc && disc->read(0x60000000u + 6, std::span<uint8_t>(&byte, 1)) && byte == 0);
        CHECK(disc && reads(*disc, good, 0x3FF0, 0x2100));
    }

    // CISO.
    const fs::path ciso = dir / "bad disc.ciso";
    const auto ciso_with = [&](auto&& change) {
        write_ciso(ciso, good);
        Bytes file;
        {
            std::ifstream in(ciso, std::ios::binary);
            file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        change(file);
        write(ciso, file);
    };
    ciso_with([](Bytes& d) { d.resize(0x100); });
    REFUSED(ciso, "block map is cut short");
    ciso_with([](Bytes& d) { d[4] = d[5] = 0; });
    REFUSED(ciso, "block size of 0");
    ciso_with([](Bytes& d) { d[8 + 3] = 2; });
    REFUSED(ciso, "block map holds 2 for block 3");
    ciso_with([](Bytes& d) { d.resize(d.size() - 0x800); });
    REFUSED(ciso, "cut short");
    ciso_with([](Bytes& d) { d[8] = 0; });  // the header's block absent: reads as zeros
    REFUSED(ciso, "magic at 0x1C");
    ciso_with([](Bytes& d) { d[0x8000 + 0x1C] ^= 1; });
    REFUSED(ciso, "magic at 0x1C");

    // Folders.
    const fs::path bad = dir / "bad folder";
    const auto folder_with = [&](auto&& change) {
        write_folder(bad, good, root);
        change();
    };
    const auto fst_with = [&](auto&& change) {
        Bytes fst(good.begin() + kFstAt, good.begin() + kFstAt + fst_size);
        change(fst);
        write(bad / "sys" / "fst.bin", fst);
    };
    const auto at = [&](std::string_view path) { return 12 * index_of(reference, path); };
    fs::create_directories(dir / "no disc here");
    REFUSED(dir / "no disc here", "no sys folder");
    folder_with([&] { fs::remove(bad / "sys" / "boot.bin"); });
    REFUSED(bad, "boot.bin");
    folder_with([&] { write(bad / "sys" / "boot.bin", Bytes(100)); });
    REFUSED(bad, "is 100 bytes, not 1088");
    folder_with([&] { write(bad / "sys" / "bi2.bin", Bytes(100)); });
    REFUSED(bad, "bi2.bin");
    folder_with([&] { write(bad / "sys" / "apploader.img", Bytes(0x10)); });
    REFUSED(bad, "shorter than its header");
    folder_with([&] { write(bad / "sys" / "apploader.img", Bytes(0x30)); });
    REFUSED(bad, "is 48 bytes, not the 32 its header gives it");
    folder_with([&] { write(bad / "sys" / "main.dol", std::span(good).subspan(kDolAt, 0x120)); });
    REFUSED(bad, "its sections end at 304");
    folder_with([&] { write(bad / "sys" / "fst.bin", Bytes(40)); });
    REFUSED(bad, "boot.bin gives the file system table");
    folder_with([&] { fst_with([](Bytes& fst) { fst[0] = 0; }); });
    REFUSED(bad, "root entry is not a directory");
    folder_with([&] { fs::remove(bad / "files" / "Course" / "Luigi.arc"); });
    REFUSED(bad, "Luigi.arc");
    folder_with([&] { write(bad / "files" / "many" / "f7.bin", Bytes(5)); });
    REFUSED(bad, "is 5 bytes, not the 33");
    folder_with([&] { fs::remove_all(bad / "files" / "Course" / "Deep"); });
    REFUSED(bad, "zero.dat");
    folder_with([&] { fst_with([&](Bytes& fst) { put32(fst, at("b.bin") + 4, 0x4000); }); });
    REFUSED(bad, "overlap on the disc");
    folder_with([&] { fst_with([&](Bytes& fst) { put32(fst, at("a.bin") + 4, 0x100); }); });  // inside boot.bin
    REFUSED(bad, "sys/boot.bin");
    folder_with([&] { fst_with([&](Bytes& fst) { fst[12 * count + (get32(fst, at("a.bin")) & 0xFFFFFF)] = ':'; }); });
    REFUSED(bad, "cannot be a file's name");
    // The same name in an image: read, but not extracted.
    with([&](Bytes& d) { d[kFstAt + 12 * count + (get32(d, entry("a.bin")) & 0xFFFFFF)] = ':'; });
    {
        const auto disc = Disc::open(iso);
        CHECK(disc && disc->find(":.bin"));
        const auto done = disc ? extract(*disc, dir / "never") : Result<void>{};
        CHECK(!done && done.error().contains("cannot be a file's name") && !fs::exists(dir / "never"));
    }
    // A file that changes under an open folder is an error when read, not zeros.
    folder_with([] {});
    {
        const auto disc = Disc::open(bad);
        CHECK(disc);
        write(bad / "files" / "a.bin", Bytes(10));
        uint8_t bytes[100];
        const auto r = disc ? disc->read(0x4000, bytes) : Result<void>{};
        CHECK(!r && r.error().contains("a.bin"));
    }
}

}  // namespace

int main() {
    // A name with a space and letters outside ASCII: paths must survive both.
    const fs::path dir = fs::temp_directory_path() /
                         fs::path(u8"qilin disc tést 測 " + std::u8string(1, static_cast<char8_t>('a' + std::random_device{}() % 26)) +
                                  std::u8string(1, static_cast<char8_t>('a' + std::random_device{}() % 26)));
    fs::remove_all(dir);
    fs::create_directories(dir);

    const Node root = tree();
    const Bytes image = build(root);
    CHECK(image.size() == 0x11007);
    write(dir / "disc.iso", image);
    CHECK(write_ciso(dir / "disc.ciso", image) >= 5);  // zero.dat's blocks and the gaps
    CHECK(fs::file_size(dir / "disc.ciso") <= 0x8000 + (18 - 5) * kBlock);
    write_folder(dir / "folder", image, root);

    {
        auto iso = Disc::open(dir / "disc.iso");
        auto ciso = Disc::open(dir / "disc.ciso");
        auto folder_disc = Disc::open(dir / "folder");
        if (!iso || !ciso || !folder_disc) {
            for (const auto* disc : {&iso, &ciso, &folder_disc})
                if (!*disc) std::println(stderr, "cannot open: {}", disc->error());
            return 1;
        }
        CHECK(iso->format() == Format::iso && ciso->format() == Format::ciso && folder_disc->format() == Format::folder);
        CHECK(name(iso->format()) == "iso" && name(ciso->format()) == "ciso" && name(folder_disc->format()) == "folder");
        CHECK(iso->path() == dir / "disc.iso");
        check_same(*iso, *iso, image);
        check_same(*ciso, *iso, image);
        check_same(*folder_disc, *iso, image);

        // Extracted from each, the folder is the one written here; and a disc moved is still a disc.
        const auto original = contents(dir / "folder");
        CHECK(original.size() == 5 + 6 + kMany + 2 + 4 && original.contains("files/Course/Empty/"));
        int n = 0;
        for (auto* disc : {&iso, &ciso, &folder_disc}) {
            const fs::path out = dir / ("extracted " + std::to_string(n++));
            const Disc moved = std::move(**disc);
            const auto done = extract(moved, out);
            if (!done) std::println(stderr, "extract: {}", done.error());
            CHECK(done && contents(out) == original);
            const auto again = extract(moved, out);
            CHECK(!again && again.error().contains("is not empty"));
            CHECK(write_dol(moved, dir / "main.dol") && contents(dir).at("main.dol") == original.at("sys/main.dol"));
        }

        const auto reference = Disc::open(dir / "disc.iso");
        if (!reference) return 1;
        test_malformed(dir, image, root, *reference);
    }

    // parse_fst on its own.
    CHECK(!parse_fst({}));
    const Bytes fst(image.begin() + kFstAt, image.begin() + kFstAt + get32(image, 0x428));
    const auto entries = parse_fst(fst);
    CHECK(entries && entries->size() == 6 + kMany + 4 + 1 && (*entries)[0].directory && (*entries)[0].end == entries->size());

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("disc tests: all passed");
    return 0;
}
