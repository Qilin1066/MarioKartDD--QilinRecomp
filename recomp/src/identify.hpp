// `qilin identify <disc image> --json [--build <title>]`: what the player's
// disc image is, for the launcher (tools/player-kit/launcher), as the
// contract every project's recompiler keeps (ADOPTING.md there). One JSON
// object on standard output:
//
//   {"ok": true|false, "title": "...", "id": "...", "version": "...", "message": "..."}
//
// and the exit code: 0 when it is the supported build (`--build`'s, or any
// this port knows without it), 3 when it can be read but isn't (another
// game, region or revision, or a changed main.dol: the message names what it
// is and what's needed), 2 when it can't be read at all (missing, a folder,
// damaged, or a format the disc reader doesn't take, such as RVZ). The
// message is a plain sentence for the player, with no paths in it.
//
// Here the input is a GameCube disc image as the disc reader takes it
// (disc/: a plain .iso or .gcm, or a .ciso): its header names the disc (ID,
// revision, title) and main.dol's SHA-256 names the build (dol.cpp's known
// builds). Only the header, the file system table and main.dol are read;
// nothing is written.
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "dol.hpp"

namespace qilin {

struct Identity {
    int code = 2;  // 0, 3 or 2, as above
    bool ok = false;
    std::string title, id, version, message;
};

// `want` is a known build's title ("Mario Kart: Double Dash!! (USA)"), or
// empty for any; `builds` the builds this port knows (the tests give their own).
Identity identify(const std::filesystem::path& input, std::string_view want, std::span<const KnownBuild> builds);
inline Identity identify(const std::filesystem::path& input, std::string_view want) { return identify(input, want, known_builds()); }

std::string to_json(const Identity& identity);

}  // namespace qilin
