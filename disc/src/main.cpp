// qilin-disc: a GameCube disc image (.iso, .gcm, .ciso) or extracted folder, inspected and unpacked.
//
//   qilin-disc info    <image|folder> [--json]    id, title, revision, the DOL's size and SHA-256, the file counts
//   qilin-disc list    <image|folder>             every entry: offset, size, path (directories end in '/')
//   qilin-disc extract <image|folder> <folder>    Dolphin's layout: sys/ and files/
//   qilin-disc dol     <image|folder> <file>      just main.dol
//   qilin-disc verify  <image|folder>             structure checks
//
// Exit status: 0, 1 on an error or a failed check (the message on stderr), 2 for a wrong command line.
#include <cstdio>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

#include "dol.hpp"
#include "file.hpp"
#include "qilin/disc/disc.hpp"
#include "sha256.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#endif

using namespace qilin;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::print(stderr, "usage:\n"
                       "  qilin-disc info    <image|folder> [--json]\n"
                       "  qilin-disc list    <image|folder>\n"
                       "  qilin-disc extract <image|folder> <new folder>\n"
                       "  qilin-disc dol     <image|folder> <main.dol to write>\n"
                       "  qilin-disc verify  <image|folder>\n"
                       "An image is a GameCube .iso, .gcm or .ciso; a folder is one extracted in Dolphin's layout.\n");
    return 2;
}

int error(const std::string& message) {
    std::println(stderr, "qilin-disc: {}", message);
    return 1;
}

// A JSON string; the disc's text is bytes, so anything outside ASCII is shown as '?'.
std::string json_text(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (c < 0x20 || c > 0x7E) out += '?';
        else out += c;
    }
    return out + '"';
}

std::string plain(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c < 0x20 || c > 0x7E) c = '?';
    return out;
}

int cmd_info(const disc::Disc& d, bool json) {
    const auto dol = d.dol();
    if (!dol) return error(dol.error());
    const std::string sha = sha256_hex(*dol);
    const disc::Header& h = d.header();
    const size_t entries = d.entries().size(), directories = entries - 1 - d.file_count();
    if (json) {
        std::println("{{");
        std::println("  \"format\": {},", json_text(disc::name(d.format())));
        std::println("  \"id\": {},", json_text(h.game_id));
        std::println("  \"maker\": {},", json_text(h.maker));
        std::println("  \"disc\": {},", h.disc_number);
        std::println("  \"revision\": {},", h.revision);
        std::println("  \"title\": {},", json_text(h.title));
        std::println("  \"apploader_date\": {},", json_text(h.apploader_date));
        std::println("  \"apploader_size\": {},", d.apploader_range().size);
        std::println("  \"dol_offset\": {},", d.dol_range().offset);
        std::println("  \"dol_size\": {},", d.dol_range().size);
        std::println("  \"dol_sha256\": {},", json_text(sha));
        std::println("  \"fst_offset\": {},", h.fst_offset);
        std::println("  \"fst_size\": {},", h.fst_size);
        std::println("  \"fst_entries\": {},", entries);
        std::println("  \"files\": {},", d.file_count());
        std::println("  \"directories\": {},", directories);
        std::println("  \"file_bytes\": {}", d.file_bytes());
        std::println("}}");
        return 0;
    }
    std::println("format      {}", disc::name(d.format()));
    std::println("id          {} (maker {}, disc {}, revision {})", plain(h.game_id), plain(h.maker), h.disc_number, h.revision);
    std::println("title       {}", plain(h.title));
    std::println("apploader   {}, {} bytes", plain(h.apploader_date), d.apploader_range().size);
    std::println("main.dol    {} bytes at {}", d.dol_range().size, disc::hex(d.dol_range().offset));
    std::println("  SHA-256   {}", sha);
    std::println("FST         {} bytes at {}: {} entries ({} files, {} directories)", h.fst_size, disc::hex(h.fst_offset), entries,
                 d.file_count(), directories);
    std::println("file bytes  {} ({:.1f} MB)", d.file_bytes(), static_cast<double>(d.file_bytes()) / 1e6);
    return 0;
}

int cmd_list(const disc::Disc& d) {
    for (const disc::Entry& e : std::span(d.entries()).subspan(1)) {
        if (e.directory) std::println("{:>10} {:>10} {}/", "", "", plain(e.path));
        else std::println("{:>10} {:>10} {}", disc::hex(e.offset), e.size, plain(e.path));
    }
    return 0;
}

int cmd_verify(const disc::Disc& d) {
    std::vector<std::string> problems = disc::verify(d);
    // And the DOL as the recompiler will load it.
    if (const auto dol = d.dol(); !dol) problems.push_back(dol.error());
    else if (const auto parsed = Dol::parse(*dol); !parsed) problems.push_back("main.dol: " + parsed.error());
    for (const std::string& problem : problems) std::println(stderr, "qilin-disc: {}: {}", disc::show(d.path()), problem);
    if (!problems.empty()) return 1;
    std::println("{}: {} files in {} entries, the DOL and the system areas: all in bounds, none overlapping", plain(d.header().game_id),
                 d.file_count(), d.entries().size());
    return 0;
}

int run(const std::vector<fs::path>& args) {
    if (args.empty()) return usage();
    const std::string command = disc::show(args[0]);
    std::vector<fs::path> paths;
    bool json = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (disc::show(args[i]) == "--json") json = true;
        else paths.push_back(args[i]);
    }
    const bool two = command == "extract" || command == "dol";
    const bool one = command == "info" || command == "list" || command == "verify";
    if ((!one && !two) || paths.size() != (two ? 2u : 1u) || (json && command != "info")) return usage();

    const auto d = disc::Disc::open(paths[0]);
    if (!d) return error(d.error());
    if (command == "info") return cmd_info(*d, json);
    if (command == "list") return cmd_list(*d);
    if (command == "verify") return cmd_verify(*d);
    const auto done = command == "dol" ? disc::write_dol(*d, paths[1]) : disc::extract(*d, paths[1]);
    if (!done) return error(done.error());
    if (command == "dol") std::println("{}: {} bytes", disc::show(paths[1]), d->dol_range().size);
    else std::println("{}: sys/ and files/ ({} files, {} bytes)", disc::show(paths[1]), d->file_count(), d->file_bytes());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> args;
#if defined(_WIN32)
    // The wide command line: argv has gone through the ANSI code page, which loses characters.
    (void)argc, (void)argv;
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) args.emplace_back(wide[i]);
        LocalFree(wide);
    }
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return run(args);
}
