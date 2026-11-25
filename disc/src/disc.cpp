#include "qilin/disc/disc.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "file.hpp"

namespace qilin::disc {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kWiiMagic = 0x5D1C9EA3u;  // at 0x18 of a Wii disc's header
constexpr uint32_t kCisoHeaderSize = 0x8000, kCisoMapSize = kCisoHeaderSize - 8;
constexpr uint32_t kAbsent = 0xFFFFFFFFu;
constexpr uint32_t kApploaderHeaderSize = 0x20, kDolHeaderSize = 0x100;
// The console has 24 MB: nothing it loads whole can be larger.
constexpr uint32_t kMemorySize = 24u << 20;
constexpr size_t kMaxDepth = 64;      // directories within directories
constexpr size_t kOpenFiles = 48;     // a folder's files kept open at once

void zero(std::span<uint8_t> out) { std::ranges::fill(out, uint8_t{0}); }

// ---------------------------------------------------------------- sources

// Where the disc's bytes come from. Bounds are Disc::read's business.
struct Source {
    virtual ~Source() = default;
    virtual Result<void> read(uint64_t offset, std::span<uint8_t> out) const = 0;
    virtual uint64_t extent() const = 0;  // the end of the last byte it holds
};

// .iso/.gcm: the disc's bytes from 0, possibly cut short after the last file.
struct PlainSource final : Source {
    explicit PlainSource(File f) : file(std::move(f)) {}
    File file;

    Result<void> read(uint64_t offset, std::span<uint8_t> out) const override {
        if (offset < file.size()) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(out.size(), file.size() - offset));
            if (auto r = file.read_at(offset, out.first(n)); !r) return r;
            out = out.subspan(n);
        }
        zero(out);
        return {};
    }
    uint64_t extent() const override { return file.size(); }
};

// .ciso: the disc cut into blocks, the present ones stored in order after the header.
struct CisoSource final : Source {
    explicit CisoSource(File f) : file(std::move(f)) {}
    File file;
    uint32_t block_size = 0;
    std::vector<uint32_t> stored;  // by block: its place among the present ones, or kAbsent

    Result<void> read(uint64_t offset, std::span<uint8_t> out) const override {
        while (!out.empty()) {
            const uint64_t block = offset / block_size, within = offset % block_size;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(out.size(), block_size - within));
            if (block < stored.size() && stored[block] != kAbsent) {
                const uint64_t at = kCisoHeaderSize + uint64_t{stored[block]} * block_size + within;
                if (auto r = file.read_at(at, out.first(n)); !r) return r;
            } else {
                zero(out.first(n));
            }
            offset += n;
            out = out.subspan(n);
        }
        return {};
    }
    uint64_t extent() const override { return uint64_t{stored.size()} * block_size; }
};

// An extracted folder: each disc offset mapped to the file that covers it.
struct FolderSource final : Source {
    struct Segment {
        uint64_t offset = 0, size = 0;
        const std::vector<uint8_t>* memory = nullptr;  // the header and tables, already loaded
        fs::path file;                                 // or a file, opened when first read
        std::string what;                              // for messages: "files/Course/Luigi.arc"
    };
    std::vector<Segment> segments;  // sorted by offset, none overlapping, none empty

    // Open files, the least recently read closed first once there are
    // kOpenFiles (a disc can hold more files than a process may have open).
    // A read in progress keeps its file through the shared_ptr.
    struct Slot {
        std::shared_ptr<const File> file;
        uint64_t used = 0;
    };
    mutable std::mutex mutex;
    mutable std::vector<Slot> slots;  // by segment
    mutable std::vector<size_t> open;
    mutable uint64_t tick = 0;

    Result<std::shared_ptr<const File>> acquire(size_t index) const {
        const std::lock_guard lock(mutex);
        Slot& slot = slots[index];
        if (!slot.file) {
            auto file = File::open(segments[index].file);
            if (!file) return fail(std::move(file.error()));
            if (file->size() != segments[index].size)
                return fail(show(segments[index].file) + ": is " + std::to_string(file->size()) + " bytes, not the " +
                            std::to_string(segments[index].size) + " the file system table gives it");
            if (open.size() >= kOpenFiles) {
                const auto oldest = std::ranges::min_element(open, {}, [&](size_t i) { return slots[i].used; });
                slots[*oldest].file.reset();
                open.erase(oldest);
            }
            slot.file = std::make_shared<const File>(std::move(*file));
            open.push_back(index);
        }
        slot.used = ++tick;
        return slot.file;
    }

    Result<void> read(uint64_t offset, std::span<uint8_t> out) const override {
        zero(out);
        const uint64_t end = offset + out.size();
        auto segment = std::ranges::upper_bound(segments, offset, {}, [](const Segment& s) { return s.offset + s.size; });
        for (; segment != segments.end() && segment->offset < end; ++segment) {
            const uint64_t from = std::max(offset, segment->offset), to = std::min(end, segment->offset + segment->size);
            const std::span<uint8_t> part = out.subspan(static_cast<size_t>(from - offset), static_cast<size_t>(to - from));
            const uint64_t within = from - segment->offset;
            if (segment->memory) {
                std::memcpy(part.data(), segment->memory->data() + within, part.size());
            } else {
                auto file = acquire(static_cast<size_t>(segment - segments.begin()));
                if (!file) return fail(std::move(file.error()));
                if (auto r = (*file)->read_at(within, part); !r) return r;
            }
        }
        return {};
    }
    uint64_t extent() const override { return segments.empty() ? 0 : segments.back().offset + segments.back().size; }
};

// ---------------------------------------------------------------- the header's parts

Result<Header> parse_header(std::span<const uint8_t> boot) {
    const uint8_t* b = boot.data();
    if (std::memcmp(b + 0x200, "NKIT", 4) == 0)
        return fail("an NKit image: NKit removes and rearranges the disc's contents, so it is not the disc "
                    "as the game reads it; convert it back to a full ISO (or use the original dump)");
    if (be32(b + 0x18) == kWiiMagic)
        return fail("a Wii disc (the magic " + hex(kWiiMagic) + " at 0x18): only GameCube discs are read here");
    Header h;
    h.magic = be32(b + 0x1C);
    if (h.magic != kMagic)
        return fail("not a GameCube disc: the magic at 0x1C is " + hex(h.magic) + ", not " + hex(kMagic));
    h.game_id.assign(reinterpret_cast<const char*>(b), 6);
    h.maker = h.game_id.substr(4);
    h.disc_number = b[6];
    h.revision = b[7];
    h.audio_streaming = b[8] != 0;
    h.stream_buffer_size = b[9];
    const char* title = reinterpret_cast<const char*>(b + 0x20);
    h.title.assign(title, std::find(title, title + 0x3E0, '\0'));
    h.dol_offset = be32(b + 0x420);
    h.fst_offset = be32(b + 0x424);
    h.fst_size = be32(b + 0x428);
    h.fst_max_size = be32(b + 0x42C);
    return h;
}

// apploader.img: a 0x20-byte header (the date, the entry point, the sizes of
// the code and of the trailer), then the code and the trailer.
Result<uint32_t> apploader_size(std::span<const uint8_t> header) {
    const uint64_t size = uint64_t{kApploaderHeaderSize} + be32(header.data() + 0x14) + be32(header.data() + 0x18);
    if (size > kMemorySize)
        return fail("the apploader's header gives it " + std::to_string(size) + " bytes: more than the console's memory");
    return static_cast<uint32_t>(size);
}

// A DOL's length isn't stored: it ends where its last section does.
Result<uint32_t> dol_size(std::span<const uint8_t> header) {
    uint64_t size = 0;
    for (unsigned slot = 0; slot < 18; ++slot) {
        const uint32_t length = be32(header.data() + 0x90 + 4 * slot);
        if (length) size = std::max(size, uint64_t{be32(header.data() + 4 * slot)} + length);
    }
    if (size < kDolHeaderSize) return fail("the DOL has no sections beyond its header");
    if (size > uint64_t{kMemorySize} + kDolHeaderSize)
        return fail("the DOL's sections end at " + hex(size) + ": more than the console's memory");
    return static_cast<uint32_t>(size);
}

std::string describe(const Entry& file) {
    return "file " + file.path + " (at " + hex(file.offset) + ", " + std::to_string(file.size) + " bytes)";
}

}  // namespace

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool host_safe_name(std::string_view text) {
    if (text.empty() || text == "." || text == ".." || text.back() == ' ' || text.back() == '.') return false;
    return std::ranges::none_of(text, [](char c) {
        return c < 0x20 || c > 0x7E || std::string_view("/\\:*?\"<>|").contains(c);
    });
}

std::string_view name(Format format) {
    switch (format) {
    case Format::iso: return "iso";
    case Format::ciso: return "ciso";
    case Format::folder: return "folder";
    }
    return "?";
}

// ---------------------------------------------------------------- the FST

Result<std::vector<Entry>> parse_fst(std::span<const uint8_t> fst) {
    if (fst.size() < kFstEntrySize)
        return fail("the file system table is " + std::to_string(fst.size()) + " bytes: shorter than its root entry");
    if (fst[0] != 1) return fail("the file system table's root entry is not a directory");
    const uint32_t count = be32(fst.data() + 8);
    if (count == 0 || uint64_t{count} * kFstEntrySize > fst.size())
        return fail("the file system table's root counts " + std::to_string(count) + " entries: its " +
                    std::to_string(fst.size()) + " bytes cannot hold them");
    const std::span<const uint8_t> names = fst.subspan(size_t{count} * kFstEntrySize);

    std::vector<Entry> entries(count);
    entries[0].directory = true;
    entries[0].end = count;
    std::vector<uint32_t> within{0};  // the directories entry i lies in, outermost first
    for (uint32_t i = 1; i < count; ++i) {
        // A directory's end is within its parent's, and the root's is the count: never empties.
        while (entries[within.back()].end == i) within.pop_back();
        const uint8_t* raw = fst.data() + size_t{i} * kFstEntrySize;
        const std::string which = "file system table entry " + std::to_string(i);
        if (raw[0] > 1) return fail(which + " is of type " + std::to_string(raw[0]) + ": neither file nor directory");
        const uint32_t name_at = be32(raw) & 0xFFFFFF;
        if (name_at >= names.size())
            return fail(which + ": its name is at " + hex(name_at) + ", outside the " + std::to_string(names.size()) +
                        " bytes of names");
        const char* text = reinterpret_cast<const char*>(names.data()) + name_at;
        const void* terminator = std::memchr(text, 0, names.size() - name_at);
        if (!terminator) return fail(which + ": its name is not terminated within the table");
        Entry& e = entries[i];
        e.name.assign(text, static_cast<const char*>(terminator));
        if (e.name.empty()) return fail(which + " has an empty name");
        e.parent = within.back();
        const Entry& parent = entries[e.parent];
        e.path = parent.path.empty() ? e.name : parent.path + "/" + e.name;
        e.directory = raw[0] == 1;
        if (!e.directory) {
            e.offset = be32(raw + 4);
            e.size = be32(raw + 8);
            continue;
        }
        const uint32_t named_parent = be32(raw + 4);
        e.end = be32(raw + 8);
        if (named_parent != e.parent)
            return fail(which + " (directory " + e.path + ") names entry " + std::to_string(named_parent) +
                        " as its parent, but lies in entry " + std::to_string(e.parent));
        if (e.end <= i || e.end > parent.end)
            return fail(which + " (directory " + e.path + ") ends at entry " + std::to_string(e.end) +
                        ": it must end after itself and by entry " + std::to_string(parent.end) + ", where its parent does");
        if (within.size() >= kMaxDepth)
            return fail(which + " (directory " + e.path + ") is more than " + std::to_string(kMaxDepth) + " directories deep");
        within.push_back(i);
    }
    return entries;
}

// ---------------------------------------------------------------- Disc

struct Disc::Impl {
    fs::path path;
    Format format = Format::iso;
    Header header;
    uint64_t size = 0;
    std::vector<uint8_t> boot, bi2, apploader, fst;
    Range dol;
    std::vector<Entry> entries;
    uint32_t file_count = 0;
    uint64_t file_bytes = 0;
    std::unordered_map<std::string, uint32_t> by_path;  // lower case
    std::unique_ptr<Source> source;

    // After the FST's bytes and the source are in place.
    Result<void> index() {
        auto parsed = parse_fst(fst);
        if (!parsed) return fail(std::move(parsed.error()));
        entries = std::move(*parsed);
        size = std::max({kDiscSize, source->extent(), dol.end(), uint64_t{header.fst_offset} + fst.size(),
                         uint64_t{kApploaderOffset} + apploader.size()});
        for (uint32_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            by_path.try_emplace(lower(e.path), i);  // of two alike, the first: as a search from the top finds it
            if (e.directory) continue;
            ++file_count;
            file_bytes += e.size;
            size = std::max(size, uint64_t{e.offset} + e.size);
        }
        return {};
    }
};

struct Opener {
    static Result<Disc> image(const fs::path& path);
    static Result<Disc> folder(const fs::path& path);
};

Result<Disc> Opener::image(const fs::path& path) {
    const auto bad = [&](const std::string& message) { return fail(show(path) + ": " + message); };
    auto opened = File::open(path);
    if (!opened) return fail(std::move(opened.error()));
    const uint64_t length = opened->size();
    uint8_t magic[4] = {};
    if (length >= 4)
        if (auto r = opened->read_at(0, magic); !r) return fail(std::move(r.error()));
    const auto is = [&](const char* text) { return std::memcmp(magic, text, 4) == 0; };

    if (is("\x01\xC0\x0B\xB1"))
        return bad("a GCZ image (compressed with zlib), which is not read here: convert it to ISO or CISO with Dolphin");
    if (is("RVZ\x01") || is("WIA\x01"))
        return bad("an RVZ/WIA image (compressed), which is not read here: convert it to ISO or CISO with Dolphin");
    if (is("WBFS")) return bad("a WBFS image, which holds Wii discs: only GameCube discs are read here");

    auto impl = std::make_unique<Disc::Impl>();
    impl->path = path;
    uint64_t limit = length;  // where the image's contents must end
    if (is("CISO")) {
        if (length < kCisoHeaderSize)
            return bad("a CISO image of " + std::to_string(length) + " bytes: its block map is cut short (the header is " +
                       std::to_string(kCisoHeaderSize) + ")");
        std::vector<uint8_t> header(kCisoHeaderSize);
        if (auto r = opened->read_at(0, header); !r) return fail(std::move(r.error()));
        auto source = std::make_unique<CisoSource>(std::move(*opened));
        // The one little-endian field in all of this.
        source->block_size = uint32_t{header[4]} | uint32_t{header[5]} << 8 | uint32_t{header[6]} << 16 | uint32_t{header[7]} << 24;
        if (source->block_size == 0 || source->block_size > (1u << 30))
            return bad("a CISO image with a block size of " + std::to_string(source->block_size) + " bytes");
        uint32_t present = 0;
        for (uint32_t block = 0; block < kCisoMapSize; ++block) {
            const uint8_t flag = header[8 + block];
            if (flag > 1)
                return bad("a CISO image whose block map holds " + std::to_string(flag) + " for block " +
                           std::to_string(block) + ": only 0 (absent) and 1 (present) mean anything");
            if (!flag) continue;
            source->stored.resize(block + 1, kAbsent);
            source->stored[block] = present++;
        }
        const uint64_t needed = kCisoHeaderSize + uint64_t{present} * source->block_size;
        if (length < needed)
            return bad("a CISO image cut short: its map lists " + std::to_string(present) + " blocks of " +
                       std::to_string(source->block_size) + " bytes (" + std::to_string(needed) +
                       " bytes with the header), the file has " + std::to_string(length));
        limit = uint64_t{kCisoMapSize} * source->block_size;
        impl->format = Format::ciso;
        impl->source = std::move(source);
    } else {
        impl->format = Format::iso;
        impl->source = std::make_unique<PlainSource>(std::move(*opened));
    }
    const Source& source = *impl->source;
    const auto outside = [&](const std::string& what, uint64_t offset, uint64_t size) {
        return bad(what + " (at " + hex(offset) + ", " + std::to_string(size) + " bytes) lies outside the image, which ends at " +
                   hex(limit) + (impl->format == Format::iso ? ": is it cut short?" : ""));
    };

    if (limit < kApploaderOffset + kApploaderHeaderSize)
        return bad(std::to_string(length) + " bytes: too short for a disc's header (not a disc image?)");
    impl->boot.resize(kBootSize);
    if (auto r = source.read(0, impl->boot); !r) return fail(std::move(r.error()));
    auto header = parse_header(impl->boot);
    if (!header) return bad(header.error());
    impl->header = std::move(*header);
    const Header& h = impl->header;

    impl->bi2.resize(kBi2Size);
    if (auto r = source.read(kBi2Offset, impl->bi2); !r) return fail(std::move(r.error()));

    impl->apploader.resize(kApploaderHeaderSize);
    if (auto r = source.read(kApploaderOffset, impl->apploader); !r) return fail(std::move(r.error()));
    const auto apploader = apploader_size(impl->apploader);
    if (!apploader) return bad(apploader.error());
    if (kApploaderOffset + uint64_t{*apploader} > limit) return outside("the apploader", kApploaderOffset, *apploader);
    impl->apploader.resize(*apploader);
    if (auto r = source.read(kApploaderOffset, impl->apploader); !r) return fail(std::move(r.error()));
    impl->header.apploader_date.assign(impl->apploader.begin(), std::find(impl->apploader.begin(), impl->apploader.begin() + 0x10, uint8_t{0}));

    if (uint64_t{h.dol_offset} + kDolHeaderSize > limit) return outside("the DOL's header", h.dol_offset, kDolHeaderSize);
    uint8_t dol_header[kDolHeaderSize];
    if (auto r = source.read(h.dol_offset, dol_header); !r) return fail(std::move(r.error()));
    const auto dol = dol_size(dol_header);
    if (!dol) return bad(dol.error());
    impl->dol = {h.dol_offset, *dol};
    if (impl->dol.end() > limit) return outside("the DOL", h.dol_offset, *dol);

    if (uint64_t{h.fst_offset} + h.fst_size > limit) return outside("the file system table", h.fst_offset, h.fst_size);
    if (h.fst_size > kMemorySize)
        return bad("the file system table is " + std::to_string(h.fst_size) + " bytes: more than the console's memory");
    impl->fst.resize(h.fst_size);
    if (auto r = source.read(h.fst_offset, impl->fst); !r) return fail(std::move(r.error()));
    if (auto r = impl->index(); !r) return bad(r.error());
    for (const Entry& e : impl->entries)
        if (!e.directory && uint64_t{e.offset} + e.size > limit) return outside("file " + e.path, e.offset, e.size);
    return Disc(std::move(impl));
}

Result<Disc> Opener::folder(const fs::path& path) {
    const auto bad = [&](const std::string& message) { return fail(show(path) + ": " + message); };
    std::error_code ec;
    if (!fs::is_directory(path / "sys", ec))
        return bad("not an extracted disc: it has no sys folder (expected sys/boot.bin, bi2.bin, apploader.img, "
                   "main.dol, fst.bin and files/)");
    // A whole small file; `size` if it must be exactly that long.
    const auto load = [&](const char* leaf, uint64_t size) -> Result<std::vector<uint8_t>> {
        const fs::path file = path / "sys" / leaf;
        auto opened = File::open(file);
        if (!opened) return fail(std::move(opened.error()));
        if (size ? opened->size() != size : opened->size() > kMemorySize)
            return fail(show(file) + ": is " + std::to_string(opened->size()) + " bytes, not " +
                        (size ? std::to_string(size) : "at most " + std::to_string(kMemorySize)));
        std::vector<uint8_t> bytes(static_cast<size_t>(opened->size()));
        if (auto r = opened->read_at(0, bytes); !r) return fail(std::move(r.error()));
        return bytes;
    };

    auto impl = std::make_unique<Disc::Impl>();
    impl->path = path;
    impl->format = Format::folder;
    auto boot = load("boot.bin", kBootSize);
    if (!boot) return fail(std::move(boot.error()));
    impl->boot = std::move(*boot);
    auto header = parse_header(impl->boot);
    if (!header) return fail(show(path / "sys" / "boot.bin") + ": " + header.error());
    impl->header = std::move(*header);
    const Header& h = impl->header;

    auto bi2 = load("bi2.bin", kBi2Size);
    if (!bi2) return fail(std::move(bi2.error()));
    impl->bi2 = std::move(*bi2);

    auto apploader = load("apploader.img", 0);
    if (!apploader) return fail(std::move(apploader.error()));
    impl->apploader = std::move(*apploader);
    const std::string apploader_name = show(path / "sys" / "apploader.img");
    if (impl->apploader.size() < kApploaderHeaderSize)
        return fail(apploader_name + ": is " + std::to_string(impl->apploader.size()) + " bytes: shorter than its header");
    const auto apploader_bytes = apploader_size(std::span(impl->apploader).first(kApploaderHeaderSize));
    if (!apploader_bytes) return fail(apploader_name + ": " + apploader_bytes.error());
    if (*apploader_bytes != impl->apploader.size())
        return fail(apploader_name + ": is " + std::to_string(impl->apploader.size()) + " bytes, not the " +
                    std::to_string(*apploader_bytes) + " its header gives it");
    impl->header.apploader_date.assign(impl->apploader.begin(), std::find(impl->apploader.begin(), impl->apploader.begin() + 0x10, uint8_t{0}));

    auto fst = load("fst.bin", 0);
    if (!fst) return fail(std::move(fst.error()));
    impl->fst = std::move(*fst);
    const std::string fst_name = show(path / "sys" / "fst.bin");
    if (impl->fst.size() != h.fst_size)
        return fail(fst_name + ": is " + std::to_string(impl->fst.size()) + " bytes, not the " + std::to_string(h.fst_size) +
                    " boot.bin gives the file system table");

    const fs::path dol_file = path / "sys" / "main.dol";
    {
        auto dol = File::open(dol_file);
        if (!dol) return fail(std::move(dol.error()));
        if (dol->size() < kDolHeaderSize)
            return fail(show(dol_file) + ": is " + std::to_string(dol->size()) + " bytes: shorter than a DOL's header");
        uint8_t dol_header[kDolHeaderSize];
        if (auto r = dol->read_at(0, dol_header); !r) return fail(std::move(r.error()));
        const auto size = dol_size(dol_header);
        if (!size) return fail(show(dol_file) + ": " + size.error());
        if (*size != dol->size())
            return fail(show(dol_file) + ": is " + std::to_string(dol->size()) + " bytes, but its sections end at " +
                        std::to_string(*size));
        impl->dol = {h.dol_offset, *size};
    }

    auto source = std::make_unique<FolderSource>();
    using Segment = FolderSource::Segment;
    source->segments.push_back(Segment{0, kBootSize, &impl->boot, {}, "sys/boot.bin"});
    source->segments.push_back(Segment{kBi2Offset, kBi2Size, &impl->bi2, {}, "sys/bi2.bin"});
    source->segments.push_back(Segment{kApploaderOffset, impl->apploader.size(), &impl->apploader, {}, "sys/apploader.img"});
    source->segments.push_back(Segment{h.dol_offset, impl->dol.size, nullptr, dol_file, "sys/main.dol"});
    source->segments.push_back(Segment{h.fst_offset, impl->fst.size(), &impl->fst, {}, "sys/fst.bin"});

    auto parsed = parse_fst(impl->fst);
    if (!parsed) return fail(fst_name + ": " + parsed.error());
    std::vector<fs::path> folders{path / "files"};  // by depth: the directory being walked
    std::vector<uint32_t> ends{static_cast<uint32_t>(parsed->size())};
    for (uint32_t i = 1; i < parsed->size(); ++i) {
        const Entry& e = (*parsed)[i];
        while (ends.back() == i) ends.pop_back(), folders.pop_back();
        if (!host_safe_name(e.name))
            return fail(fst_name + ": entry " + std::to_string(i) + " is named \"" + e.name +
                        "\", which cannot be a file's name on every system: such a disc is read from its image only");
        fs::path file = folders.back() / e.name;
        if (e.directory) {
            folders.push_back(std::move(file));
            ends.push_back(e.end);
            continue;
        }
        const uint64_t size = fs::file_size(file, ec);
        if (ec) return fail(show(file) + ": missing or unreadable (" + ec.message() + "), though the file system table lists it");
        if (size != e.size)
            return fail(show(file) + ": is " + std::to_string(size) + " bytes, not the " + std::to_string(e.size) +
                        " the file system table gives it");
        if (e.size) source->segments.push_back(Segment{e.offset, e.size, nullptr, std::move(file), "files/" + e.path});
    }
    std::erase_if(source->segments, [](const Segment& s) { return s.size == 0; });
    std::ranges::stable_sort(source->segments, {}, &Segment::offset);
    for (size_t i = 1; i < source->segments.size(); ++i) {
        const Segment& a = source->segments[i - 1];
        const Segment& b = source->segments[i];
        if (b.offset < a.offset + a.size)
            return bad(a.what + " (at " + hex(a.offset) + ", " + std::to_string(a.size) + " bytes) and " + b.what + " (at " +
                       hex(b.offset) + ") overlap on the disc: a folder cannot say which one a read there means");
    }
    source->slots.resize(source->segments.size());
    impl->source = std::move(source);
    if (auto r = impl->index(); !r) return fail(fst_name + ": " + r.error());
    return Disc(std::move(impl));
}

Result<Disc> Disc::open(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (!fs::exists(status)) return fail(show(path) + ": no such file or folder");
    return fs::is_directory(status) ? Opener::folder(path) : Opener::image(path);
}

Disc::Disc(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Disc::Disc(Disc&&) noexcept = default;
Disc& Disc::operator=(Disc&&) noexcept = default;
Disc::~Disc() = default;

Format Disc::format() const { return impl_->format; }
const fs::path& Disc::path() const { return impl_->path; }
const Header& Disc::header() const { return impl_->header; }
uint64_t Disc::size() const { return impl_->size; }
std::span<const uint8_t> Disc::boot_bin() const { return impl_->boot; }
std::span<const uint8_t> Disc::bi2_bin() const { return impl_->bi2; }
std::span<const uint8_t> Disc::apploader() const { return impl_->apploader; }
std::span<const uint8_t> Disc::fst() const { return impl_->fst; }
Range Disc::apploader_range() const { return {kApploaderOffset, impl_->apploader.size()}; }
Range Disc::dol_range() const { return impl_->dol; }
Range Disc::fst_range() const { return {impl_->header.fst_offset, impl_->fst.size()}; }
const std::vector<Entry>& Disc::entries() const { return impl_->entries; }
uint32_t Disc::file_count() const { return impl_->file_count; }
uint64_t Disc::file_bytes() const { return impl_->file_bytes; }

Result<std::vector<uint8_t>> Disc::dol() const {
    std::vector<uint8_t> bytes(static_cast<size_t>(impl_->dol.size));
    if (auto r = read(impl_->dol.offset, bytes); !r) return fail(std::move(r.error()));
    return bytes;
}

const Entry* Disc::find(std::string_view path) const {
    while (path.starts_with('/')) path.remove_prefix(1);
    const auto found = impl_->by_path.find(lower(path));
    return found == impl_->by_path.end() ? nullptr : &impl_->entries[found->second];
}

Result<void> Disc::read(uint64_t offset, std::span<uint8_t> out) const {
    if (offset > impl_->size || out.size() > impl_->size - offset)
        return fail(show(impl_->path) + ": a read of " + std::to_string(out.size()) + " bytes at " + hex(offset) +
                    " runs past the end of the disc (" + hex(impl_->size) + ")");
    return impl_->source->read(offset, out);
}

Result<void> Disc::read_file(const Entry& file, uint64_t offset, std::span<uint8_t> out) const {
    if (file.directory) return fail(show(impl_->path) + ": " + file.path + " is a directory, not a file");
    if (offset > file.size || out.size() > file.size - offset)
        return fail(show(impl_->path) + ": a read of " + std::to_string(out.size()) + " bytes at " + hex(offset) +
                    " runs past the end of " + describe(file));
    return read(file.offset + offset, out);
}

}  // namespace qilin::disc
