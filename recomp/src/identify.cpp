// `qilin identify` (identify.hpp). Developer tool code: std::format is fine here.
#include "identify.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

#include "qilin/disc/disc.hpp"
#include "sha256.hpp"

namespace qilin {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kWiiMagic = 0x5D1C9EA3u;  // at 0x18 of a Wii disc's header
constexpr uint64_t kCisoHeader = 0x8000;      // a CISO's block map; the disc's first block follows

// Up to `count` bytes from `offset` of a file; fewer at its end, nothing if it can't be read.
std::optional<std::vector<uint8_t>> read_bytes(const fs::path& file, uint64_t offset, size_t count) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg(static_cast<std::streamoff>(offset));
    if (!f) return std::vector<uint8_t>{};
    std::vector<uint8_t> bytes(count);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
    if (f.bad()) return std::nullopt;
    bytes.resize(static_cast<size_t>(f.gcount()));
    return bytes;
}

uint32_t be32(const std::vector<uint8_t>& b, size_t at) {
    return b.size() < at + 4 ? 0 : uint32_t{b[at]} << 24 | uint32_t{b[at + 1]} << 16 | uint32_t{b[at + 2]} << 8 | uint32_t{b[at + 3]};
}

bool starts(const std::vector<uint8_t>& b, size_t at, std::string_view text) {
    return b.size() >= at + text.size() && std::memcmp(b.data() + at, text.data(), text.size()) == 0;
}

std::string shown(const fs::path& p) {
    const std::u8string text = p.u8string();
    return {text.begin(), text.end()};
}

// The disc's text as the player can read it: its bytes are ASCII on Western
// discs and may be Shift-JIS on Japanese ones, which isn't UTF-8.
std::string printable(std::string_view text) {
    std::string out;
    for (const char c : text) out += c >= 0x20 && c < 0x7F ? c : '?';
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// What a disc header (boot.bin's first 0x60 bytes) says: its ID and title.
struct Named {
    std::string id, title;
    unsigned revision = 0;
};
std::optional<Named> named(const std::vector<uint8_t>& header) {
    if (header.size() < 0x60) return std::nullopt;
    Named n;
    for (size_t i = 0; i < 6; ++i) {
        const char c = static_cast<char>(header[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return std::nullopt;  // not a disc ID
        n.id += c;
    }
    n.revision = header[7];
    std::string title;
    for (size_t i = 0x20; i < 0x60 && header[i]; ++i) title += static_cast<char>(header[i]);
    n.title = printable(title);
    return n;
}

// "Mario Kart: Double Dash!! (USA), disc GM4E01 revision 0"
std::string describe(const KnownBuild& b) { return std::format("{}, disc {} revision {}", b.title, b.disc_id, b.revision); }

// "Mario Kart: Double Dash!!": a build's title without its region.
std::string game_name(const KnownBuild& b) {
    const std::string_view t = b.title;
    const size_t paren = t.find(" (");
    return std::string(paren == std::string_view::npos ? t : t.substr(0, paren));
}

// The region a GameCube disc ID's fourth letter names.
std::string_view region_name(char c) {
    switch (c) {
    case 'E': return "North America";
    case 'P': return "Europe and Australia";
    case 'J': return "Japan";
    case 'K': return "Korea";
    case 'D': return "Germany";
    case 'F': return "France";
    case 'S': return "Spain";
    case 'I': return "Italy";
    case 'U': return "Australia";
    case 'X': case 'Y': case 'Z': return "Europe";
    default: return "";
    }
}

Identity answer(int code, std::string message) {
    Identity i;
    i.code = code;
    i.ok = code == 0;
    i.message = std::move(message);
    return i;
}

// The disc reader's reason, without the file's path (the player knows which
// file they chose), and ending with one full stop.
std::string reason(std::string text, const fs::path& input) {
    const std::string path = shown(input);
    if (!path.empty() && text.starts_with(path + ": ")) text.erase(0, path.size() + 2);
    for (size_t at; !path.empty() && (at = text.find(path)) != std::string::npos;) text.replace(at, path.size(), shown(input.filename()));
    while (!text.empty() && (text.back() == '.' || text.back() == ' ')) text.pop_back();
    return text + ".";
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            out += std::format("\\u{:04x}", u);
        } else {
            out += c;  // UTF-8 as it is
        }
    }
    return out + "\"";
}

}  // namespace

Identity identify(const fs::path& input, std::string_view want, std::span<const KnownBuild> builds) {
    const KnownBuild* wanted = nullptr;
    if (!want.empty()) {
        for (const KnownBuild& b : builds)
            if (b.title == want) wanted = &b;
        if (!wanted) return answer(2, std::format("This recompiler knows no build called {}.", want));
    } else if (builds.size() == 1) {
        wanted = &builds.front();
    }
    const std::string for_text = wanted ? " This download is for " + describe(*wanted) + "." : std::string();
    const std::string choose = " Choose your disc's .iso, .gcm or .ciso file.";
    const std::string convert = " Convert it in Dolphin: right-click the game, choose Convert File, and pick ISO or CISO.";

    std::error_code ec;
    if (!fs::exists(input, ec)) return answer(2, "Can't find " + shown(input.filename()) + ": it may have been moved or deleted.");
    if (fs::is_directory(input, ec)) {
        if (fs::is_regular_file(input / "sys" / "main.dol", ec) || fs::is_regular_file(input / "main.dol", ec))
            return answer(2, "This is a disc extracted into a folder. This download is built from the disc image itself." + choose);
        return answer(2, "This is a folder, not a disc image." + choose);
    }

    // The container, from its first bytes (not its name): what the disc
    // reader refuses gets a sentence of its own.
    const auto start = read_bytes(input, 0, disc::kBootSize);
    if (!start) return answer(2, "Can't read " + shown(input.filename()) + ": it may be in use, or not yours to read.");
    if (starts(*start, 0, "RVZ\x01") || starts(*start, 0, "WIA\x01"))
        return answer(2, "This is an RVZ or WIA image, which is compressed in a way this can't read." + convert);
    if (starts(*start, 0, "\x01\xC0\x0B\xB1")) return answer(2, "This is a GCZ image, which is compressed in a way this can't read." + convert);
    if (starts(*start, 0, "WBFS")) return answer(3, "This is a WBFS image, which holds a Wii game." + for_text);
    const bool ciso = starts(*start, 0, "CISO");
    const auto header = ciso ? read_bytes(input, kCisoHeader, disc::kBootSize) : start;
    if (!header) return answer(2, "Can't read " + shown(input.filename()) + ": it may be in use, or not yours to read.");
    if (starts(*header, 0x200, "NKIT"))
        return answer(2, "This is an NKit image, which leaves out and moves parts of the disc: convert it back to a full ISO with NKit, "
                         "or use your original dump.");
    const std::optional<Named> says = named(*header);
    if (says && be32(*header, 0x18) == kWiiMagic) {
        Identity i = answer(3, std::format("This is {} ({}), a Wii disc.{}", says->title.empty() ? "a game" : says->title, says->id, for_text));
        i.title = says->title;
        i.id = says->id;
        i.version = std::to_string(says->revision);
        return i;
    }
    if (!says || be32(*header, 0x1C) != disc::kMagic) return answer(2, "This isn't a GameCube disc image." + choose);

    // A GameCube disc: opened as the game will open it.
    auto disc = disc::Disc::open(input);
    if (!disc)
        return answer(2, "This disc image can't be read: " + reason(disc.error(), input) +
                             " It may be cut short or damaged: make the image from your disc again.");
    const auto dol = disc->dol();
    if (!dol)
        return answer(2, "This disc's main.dol can't be read: " + reason(dol.error(), input) +
                             " It may be cut short or damaged: make the image from your disc again.");
    const std::string sha = sha256_hex(*dol);
    const disc::Header& h = disc->header();
    const std::string id = printable(h.game_id), title = printable(h.title);

    const KnownBuild* build = nullptr;
    for (const KnownBuild& b : builds)
        if (b.sha256 == sha) build = &b;
    if (!build) {
        Identity i = answer(3, "");
        i.title = title;
        i.id = id;
        i.version = std::to_string(h.revision);
        if (wanted && id == wanted->disc_id) {
            if (h.revision != wanted->revision)
                i.message = std::format("This is {}, disc {} revision {}. This download is for revision {} of that disc.", wanted->title,
                                        id, h.revision, wanted->revision);
            else
                i.message = std::format("This is disc {} revision {}, but its main.dol isn't that disc's (SHA-256 {}): it may have been "
                                        "changed or damaged. Make the image from your disc again.",
                                        id, h.revision, sha);
        } else if (wanted && id.size() == 6 && std::string_view(id).substr(0, 3) == std::string_view(wanted->disc_id).substr(0, 3)) {
            const std::string_view region = region_name(id[3]);
            i.message = std::format("This is {} for {} ({}).{}", game_name(*wanted), region.empty() ? "another region" : region, id, for_text);
        } else {
            i.message = std::format("This is {} ({}), another game.{}", title.empty() ? "a disc" : title, id, for_text);
        }
        return i;
    }

    Identity i = answer(0, "This is " + describe(*build) + ": the right disc.");
    i.title = std::string(build->title);
    i.id = std::string(build->disc_id);
    i.version = std::to_string(build->revision);
    if (wanted && wanted != build) {
        i.code = 3;
        i.ok = false;
        i.message = "This is " + describe(*build) + "." + for_text;
    }
    return i;
}

std::string to_json(const Identity& i) {
    return std::string("{\"ok\": ") + (i.ok ? "true" : "false") + ", \"title\": " + json_string(i.title) + ", \"id\": " + json_string(i.id) +
           ", \"version\": " + json_string(i.version) + ", \"message\": " + json_string(i.message) + "}\n";
}

}  // namespace qilin
