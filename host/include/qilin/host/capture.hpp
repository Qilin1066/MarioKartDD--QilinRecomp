// The files of a capture for a bug report (F9 in the game): pictures as PNG
// and everything gathered into one tar archive, with nothing but the standard
// library, so a capture can't fail for want of one.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace qilin::host {

// An RGBA8 picture as a PNG (8-bit RGB; the alpha is dropped). Stored, not
// compressed: a capture is a few frames, and the archive is what gets sent.
std::vector<uint8_t> png_rgb(const uint8_t* rgba, uint32_t width, uint32_t height);

// A tar archive (ustar), written file by file. Any failure makes ok() false
// from then on, close() included.
class TarWriter {
public:
    explicit TarWriter(const std::filesystem::path& file);
    ~TarWriter();
    TarWriter(const TarWriter&) = delete;
    TarWriter& operator=(const TarWriter&) = delete;

    // `name`: up to 99 characters, with / between folders.
    void add(const std::string& name, const void* data, size_t size);
    // A file's contents as they are now (a log still being written); false if it can't be read.
    bool add_file(const std::string& name, const std::filesystem::path& file);
    bool close();  // the end marker; true if everything was written
    bool ok() const { return ok_; }

private:
    std::FILE* out_ = nullptr;
    bool ok_ = false;
};

}  // namespace qilin::host
