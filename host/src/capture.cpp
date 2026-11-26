// PNG and tar, the little of each a capture needs (capture.hpp).
#include "qilin/host/capture.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace qilin::host {

namespace {
uint32_t crc32(const uint8_t* p, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}
void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    put32(png, uint32_t(data.size()));
    const size_t from = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put32(png, crc32(png.data() + from, png.size() - from));
}
}  // namespace

std::vector<uint8_t> png_rgb(const uint8_t* rgba, uint32_t width, uint32_t height) {
    std::vector<uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};  // "\x89PNG", CR LF, ^Z, LF
    std::vector<uint8_t> header;
    put32(header, width), put32(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8 bits a channel, RGB, deflate, the one filter set, not interlaced
    chunk(png, "IHDR", header);
    // The lines, each after a filter byte (0: none).
    std::vector<uint8_t> raw;
    raw.reserve((size_t{width} * 3 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* line = rgba + size_t{y} * width * 4;
        for (uint32_t x = 0; x < width; ++x) raw.insert(raw.end(), line + size_t{x} * 4, line + size_t{x} * 4 + 3);
    }
    // A zlib stream of stored blocks (up to 65535 bytes each), then Adler-32 of the data.
    std::vector<uint8_t> z = {0x78, 0x01};
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    size_t at = 0;
    do {
        const size_t n = std::min<size_t>(raw.size() - at, 65535);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
        at += n;
    } while (at < raw.size());
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size();) {  // (5552 bytes at most before the sums could overflow)
        const size_t end = std::min(raw.size(), i + 5552);
        for (; i < end; ++i) a += raw[i], b += a;
        a %= 65521, b %= 65521;
    }
    put32(z, b << 16 | a);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

TarWriter::TarWriter(const std::filesystem::path& file) {
#if defined(_WIN32)
    out_ = _wfopen(file.c_str(), L"wb");
#else
    out_ = std::fopen(file.c_str(), "wb");
#endif
    ok_ = out_ != nullptr;
}

TarWriter::~TarWriter() {
    if (out_) std::fclose(out_);
}

void TarWriter::add(const std::string& name, const void* data, size_t size) {
    if (!ok_) return;
    if (name.empty() || name.size() > 99) {
        ok_ = false;
        return;
    }
    char h[512] = {};
    std::memcpy(h, name.data(), name.size());
    std::snprintf(h + 100, 8, "%07o", 0644u);
    std::snprintf(h + 108, 8, "%07o", 0u);
    std::snprintf(h + 116, 8, "%07o", 0u);
    std::snprintf(h + 124, 12, "%011llo", static_cast<unsigned long long>(size));
    std::snprintf(h + 136, 12, "%011llo", static_cast<unsigned long long>(std::time(nullptr)));
    std::memset(h + 148, 0x20, 8);  // the checksum's own field counts as spaces
    h[156] = 0x30;                  // "0": a file
    std::memcpy(h + 257, "ustar", 6);
    h[263] = h[264] = 0x30;
    unsigned sum = 0;
    for (const char c : h) sum += static_cast<unsigned char>(c);
    std::snprintf(h + 148, 7, "%06o", sum);
    h[155] = 0x20;
    static const char zeros[512] = {};
    const size_t pad = (512 - size % 512) % 512;
    ok_ = std::fwrite(h, 1, 512, out_) == 512 && (size == 0 || std::fwrite(data, 1, size, out_) == size) &&
          (pad == 0 || std::fwrite(zeros, 1, pad, out_) == pad);
}

bool TarWriter::add_file(const std::string& name, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    add(name, text.data(), text.size());
    return ok_;
}

bool TarWriter::close() {
    if (!out_) return false;
    static const char zeros[1024] = {};
    ok_ = ok_ && std::fwrite(zeros, 1, sizeof zeros, out_) == sizeof zeros;
    ok_ = std::fclose(out_) == 0 && ok_;
    out_ = nullptr;
    return ok_;
}

}  // namespace qilin::host
