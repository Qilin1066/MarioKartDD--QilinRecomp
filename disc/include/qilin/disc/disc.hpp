// A GameCube disc, read where it lies: a plain image (.iso, .gcm), a CISO
// image (.ciso) or a folder extracted in Dolphin's layout (sys/, files/).
//
// GameCube discs are not encrypted, so an image is the disc's bytes as they
// are: a 0x440-byte header (boot.bin), 0x2000 bytes of bi2.bin, the apploader
// at 0x2440, and the DOL and the file system table (FST) where the header says.
// docs/disc.md has the formats.
//
// One Disc serves any number of threads: it is not modified once opened, and
// reads are positional (no seek state). Nothing here loads an image into
// memory; only the header, bi2.bin, the apploader and the FST are kept.
//
// Runtime code: no std::format (see CMakeLists.txt). Errors come back as a
// message that names the file and what is wrong with it; nothing throws.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::disc {

template <class T>
using Result = std::expected<T, std::string>;

inline constexpr uint64_t kDiscSize = 1'459'978'240;  // a whole GameCube disc
inline constexpr uint32_t kMagic = 0xC2339F3Du;       // at 0x1C of the header
inline constexpr uint32_t kBootSize = 0x440;          // boot.bin, at 0
inline constexpr uint32_t kBi2Offset = 0x440, kBi2Size = 0x2000;
inline constexpr uint32_t kApploaderOffset = 0x2440;
inline constexpr uint32_t kFstEntrySize = 12;

struct Range {
    uint64_t offset = 0, size = 0;  // on the disc, in bytes
    uint64_t end() const { return offset + size; }
};

// boot.bin's fields. Text is the disc's bytes as they are (ASCII on every
// Western disc; Shift-JIS titles are possible on Japanese ones).
struct Header {
    std::string game_id;      // 6 characters: "GM4E01" (the last two are the maker)
    std::string maker;        // 2 characters: "01"
    uint8_t disc_number = 0;  // 0 for the first disc
    uint8_t revision = 0;
    bool audio_streaming = false;
    uint8_t stream_buffer_size = 0;
    uint32_t magic = 0;       // kMagic (checked when the disc is opened)
    std::string title;        // 0x20, up to its NUL
    uint32_t dol_offset = 0;  // 0x420
    uint32_t fst_offset = 0;  // 0x424
    uint32_t fst_size = 0;    // 0x428
    uint32_t fst_max_size = 0;       // 0x42C: the largest FST of a multi-disc game
    std::string apploader_date;      // the apploader's first bytes: "2003/04/17"
};

// One entry of the FST, decoded. entries()[0] is the root directory.
struct Entry {
    bool directory = false;
    std::string name;     // the root's is empty
    std::string path;     // from the root, '/' between names, none in front: "Course/Luigi.arc"
    uint32_t parent = 0;  // index of the directory this entry lies in (the root: 0)
    uint32_t offset = 0;  // file: where its bytes start on the disc
    uint32_t size = 0;    // file: its length
    uint32_t end = 0;     // directory: index one past its last entry (at any depth)
};

// Decodes an FST (fst.bin's bytes). Rejects what the console's own DVD code
// would walk off the end of: counts and name offsets outside the table, names
// without a terminator, directories whose end lies before themselves or
// beyond their parent's.
Result<std::vector<Entry>> parse_fst(std::span<const uint8_t> fst);

enum class Format { iso, ciso, folder };
std::string_view name(Format format);  // "iso", "ciso", "folder"

class Disc {
public:
    // A file (the format is recognised from its contents, not its name) or
    // an extracted folder (the one holding sys/ and files/).
    static Result<Disc> open(const std::filesystem::path& path);

    Disc(Disc&&) noexcept;
    Disc& operator=(Disc&&) noexcept;
    ~Disc();

    Format format() const;
    const std::filesystem::path& path() const;
    const Header& header() const;

    // What read() accepts: kDiscSize, or more if the image is longer.
    uint64_t size() const;

    // Kept in memory for the Disc's lifetime.
    std::span<const uint8_t> boot_bin() const;   // kBootSize bytes
    std::span<const uint8_t> bi2_bin() const;    // kBi2Size bytes
    std::span<const uint8_t> apploader() const;  // header, code and trailer: apploader.img
    // The FST as the game sees it: the bytes at header().fst_offset (a
    // folder's sys/fst.bin), file offsets as they are in it.
    std::span<const uint8_t> fst() const;

    Range apploader_range() const;
    Range dol_range() const;  // its size comes from the DOL's own header
    Range fst_range() const;
    Result<std::vector<uint8_t>> dol() const;  // main.dol's bytes

    const std::vector<Entry>& entries() const;
    uint32_t file_count() const;  // entries that are files
    uint64_t file_bytes() const;  // the sum of their sizes
    // By path, as the console's DVDOpen compares: ASCII letters match in
    // either case; a leading '/' is allowed. "" is the root. Null if absent.
    const Entry* find(std::string_view path) const;

    // Fills `out` with the disc's bytes from `offset`, as the drive would
    // return them. Bytes no file or system area covers (and a CISO's absent
    // blocks, a trimmed image's tail) read as zero. An error if the range
    // runs past size(), or the host's read fails. Safe from several threads.
    Result<void> read(uint64_t offset, std::span<uint8_t> out) const;
    // The same within one file: an error past the file's end.
    Result<void> read_file(const Entry& file, uint64_t offset, std::span<uint8_t> out) const;

private:
    struct Impl;
    explicit Disc(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend struct Opener;
};

// Writes the disc out in Dolphin's layout (sys/boot.bin, bi2.bin,
// apploader.img, main.dol, fst.bin; files/...), as its "Extract Entire Disc"
// does. `folder` must not exist yet, or be empty.
Result<void> extract(const Disc& disc, const std::filesystem::path& folder);
// Just main.dol.
Result<void> write_dol(const Disc& disc, const std::filesystem::path& file);

// Structure checks beyond what open() insists on: every area and file within
// a GameCube disc's size, none overlapping another, the FST no larger than
// the header's maximum, no two names alike in one directory, the DOL's
// sections inside its file, loading into the console's memory and holding
// the entry point. Returns what is wrong, one message each; empty if nothing.
std::vector<std::string> verify(const Disc& disc);

}  // namespace qilin::disc
