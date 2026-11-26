// Extraction in Dolphin's layout, and the structure checks.
#include <algorithm>
#include <fstream>
#include <system_error>
#include <unordered_set>

#include "file.hpp"
#include "qilin/disc/disc.hpp"

namespace qilin::disc {

namespace fs = std::filesystem;

namespace {

// `size` bytes of the disc from `offset`, into a new file.
Result<void> copy_out(const Disc& disc, uint64_t offset, uint64_t size, const fs::path& file) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return fail(show(file) + ": cannot create");
    std::vector<uint8_t> buffer(static_cast<size_t>(std::min<uint64_t>(size, 1u << 20)));
    while (size) {
        const std::span<uint8_t> part = std::span(buffer).first(static_cast<size_t>(std::min<uint64_t>(size, buffer.size())));
        if (auto r = disc.read(offset, part); !r) return r;
        out.write(reinterpret_cast<const char*>(part.data()), static_cast<std::streamsize>(part.size()));
        if (!out) break;
        offset += part.size();
        size -= part.size();
    }
    out.close();
    if (!out) return fail(show(file) + ": cannot write (is the drive full?)");
    return {};
}

struct Area {
    uint64_t offset, size;
    std::string what;
};

std::string describe(const Area& a) {
    return a.what + " (" + hex(a.offset) + " to " + hex(a.offset + a.size) + ")";
}

}  // namespace

Result<void> write_dol(const Disc& disc, const fs::path& file) {
    return copy_out(disc, disc.dol_range().offset, disc.dol_range().size, file);
}

Result<void> extract(const Disc& disc, const fs::path& folder) {
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        if (!fs::is_directory(folder, ec)) return fail(show(folder) + ": exists and is not a folder");
        if (!fs::is_empty(folder, ec) || ec) return fail(show(folder) + ": is not empty: extract into a new folder");
    }
    // Names first: nothing is written for a disc that cannot be written whole.
    for (const Entry& e : std::span(disc.entries()).subspan(1))
        if (!host_safe_name(e.name))
            return fail(show(disc.path()) + ": \"" + e.path + "\" cannot be a file's name on every system: not extracted");

    const fs::path sys = folder / "sys";
    fs::create_directories(sys, ec);
    if (ec) return fail(show(sys) + ": cannot create: " + ec.message());
    const Header& h = disc.header();
    if (auto r = copy_out(disc, 0, kBootSize, sys / "boot.bin"); !r) return r;
    if (auto r = copy_out(disc, kBi2Offset, kBi2Size, sys / "bi2.bin"); !r) return r;
    if (auto r = copy_out(disc, kApploaderOffset, disc.apploader_range().size, sys / "apploader.img"); !r) return r;
    if (auto r = write_dol(disc, sys / "main.dol"); !r) return r;
    if (auto r = copy_out(disc, h.fst_offset, h.fst_size, sys / "fst.bin"); !r) return r;

    std::vector<fs::path> folders{folder / "files"};  // by depth: the directory being walked
    std::vector<uint32_t> ends{static_cast<uint32_t>(disc.entries().size())};
    fs::create_directory(folders.back(), ec);
    if (ec) return fail(show(folders.back()) + ": cannot create: " + ec.message());
    for (uint32_t i = 1; i < disc.entries().size(); ++i) {
        const Entry& e = disc.entries()[i];
        while (ends.back() == i) ends.pop_back(), folders.pop_back();
        fs::path target = folders.back() / e.name;
        if (!e.directory) {
            if (auto r = copy_out(disc, e.offset, e.size, target); !r) return r;
            continue;
        }
        fs::create_directory(target, ec);
        if (ec) return fail(show(target) + ": cannot create: " + ec.message());
        folders.push_back(std::move(target));
        ends.push_back(e.end);
    }
    return {};
}

std::vector<std::string> verify(const Disc& disc) {
    std::vector<std::string> problems;
    const Header& h = disc.header();

    std::vector<Area> areas{{0, kBootSize, "boot.bin"},
                            {kBi2Offset, kBi2Size, "bi2.bin"},
                            {kApploaderOffset, disc.apploader_range().size, "the apploader"},
                            {disc.dol_range().offset, disc.dol_range().size, "main.dol"},
                            {h.fst_offset, h.fst_size, "the file system table"}};
    std::unordered_set<std::string> paths;
    for (const Entry& e : std::span(disc.entries()).subspan(1)) {
        if (!paths.insert(lower(e.path)).second)
            problems.push_back("two entries are named " + e.path + " (the console compares without regard to case)");
        if (!e.directory && e.size) areas.push_back({e.offset, e.size, "file " + e.path});
    }
    std::ranges::stable_sort(areas, {}, &Area::offset);
    const Area* last = nullptr;  // the one reaching furthest so far
    for (const Area& a : areas) {
        if (a.offset + a.size > kDiscSize)
            problems.push_back(describe(a) + " runs past the end of a GameCube disc (" + hex(kDiscSize) + ")");
        if (last && a.offset < last->offset + last->size) problems.push_back(describe(*last) + " and " + describe(a) + " overlap");
        if (!last || a.offset + a.size > last->offset + last->size) last = &a;
    }
    if (h.fst_size > h.fst_max_size)
        problems.push_back("the file system table is " + std::to_string(h.fst_size) + " bytes, more than the maximum the header gives (" +
                           std::to_string(h.fst_max_size) + ")");

    // The DOL: 7 text and 11 data slots (file offset, address, size), then BSS and the entry point.
    uint8_t dol[0x100];
    if (auto r = disc.read(disc.dol_range().offset, dol); !r) {
        problems.push_back(r.error());
        return problems;
    }
    constexpr uint64_t kMemoryBase = 0x80000000u, kMemoryEnd = kMemoryBase + (24u << 20);
    std::vector<Area> sections;
    const uint32_t entry = be32(dol + 0xE0);
    bool entry_in_text = false;
    for (unsigned slot = 0; slot < 18; ++slot) {
        const uint32_t offset = be32(dol + 4 * slot), address = be32(dol + 0x48 + 4 * slot), size = be32(dol + 0x90 + 4 * slot);
        if (!size) continue;
        const std::string what = "main.dol's " + std::string(slot < 7 ? "text" : "data") + std::to_string(slot < 7 ? slot : slot - 7);
        if (offset < sizeof dol) problems.push_back(what + " starts at " + hex(offset) + " in the file: inside the header");
        if (address < kMemoryBase || uint64_t{address} + size > kMemoryEnd)
            problems.push_back(what + " loads at " + hex(address) + " (" + std::to_string(size) + " bytes): outside the console's memory");
        if (slot < 7 && entry >= address && entry - address < size) entry_in_text = true;
        sections.push_back({offset, size, what});
    }
    if (!entry_in_text) problems.push_back("main.dol's entry point " + hex(entry) + " is in none of its text sections");
    std::ranges::stable_sort(sections, {}, &Area::offset);
    for (size_t i = 1; i < sections.size(); ++i)
        if (sections[i].offset < sections[i - 1].offset + sections[i - 1].size)
            problems.push_back(describe(sections[i - 1]) + " and " + describe(sections[i]) + " overlap in the file");
    return problems;
}

}  // namespace qilin::disc
