// A capture's files (capture.hpp): the PNG's structure and checksums read
// back, and the tar's headers, by an independent reading of each format.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "qilin/host/capture.hpp"

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
// CRC-32 bit by bit (the writer uses a table).
uint32_t crc_slow(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
}  // namespace

int main() {
    using namespace qilin::host;
    // 300x250: more than one stored block (225,250 bytes of lines).
    const uint32_t w = 300, h = 250;
    std::vector<uint8_t> rgba(size_t{w} * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &rgba[(size_t{y} * w + x) * 4];
            p[0] = uint8_t(x), p[1] = uint8_t(y), p[2] = uint8_t(x ^ y), p[3] = 7;
        }
    const std::vector<uint8_t> png = png_rgb(rgba.data(), w, h);
    const uint8_t signature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    check(png.size() > 8 && std::memcmp(png.data(), signature, 8) == 0, "the PNG signature");
    // Walk the chunks: lengths, CRCs, and the image data inflated by hand (stored blocks).
    std::vector<uint8_t> z;
    bool chunks_ok = true, saw_end = false;
    uint32_t pw = 0, ph = 0;
    for (size_t at = 8; at + 12 <= png.size();) {
        const uint32_t len = be32(&png[at]);
        if (at + 12 + len > png.size()) {
            chunks_ok = false;
            break;
        }
        chunks_ok = chunks_ok && crc_slow(&png[at + 4], len + 4) == be32(&png[at + 8 + len]);
        const std::string type(reinterpret_cast<const char*>(&png[at + 4]), 4);
        if (type == "IHDR") {
            pw = be32(&png[at + 8]), ph = be32(&png[at + 12]);
            chunks_ok = chunks_ok && len == 13 && png[at + 16] == 8 && png[at + 17] == 2;
        }
        if (type == "IDAT") z.insert(z.end(), png.begin() + std::ptrdiff_t(at + 8), png.begin() + std::ptrdiff_t(at + 8 + len));
        if (type == "IEND") saw_end = at + 12 == png.size();
        at += 12 + len;
    }
    check(chunks_ok && saw_end && pw == w && ph == h, "chunks: IHDR, IDAT, IEND, each CRC right");
    std::vector<uint8_t> raw;
    bool z_ok = z.size() > 6 && z[0] == 0x78 && (z[0] * 256 + z[1]) % 31 == 0;
    size_t at = 2;
    for (bool last = false; z_ok && !last;) {
        if (at + 5 > z.size()) {
            z_ok = false;
            break;
        }
        last = z[at] == 1;
        const uint32_t n = z[at + 1] | uint32_t(z[at + 2]) << 8, inv = z[at + 3] | uint32_t(z[at + 4]) << 8;
        z_ok = (z[at] == 0 || z[at] == 1) && (n ^ inv) == 0xFFFF && at + 5 + n <= z.size();
        if (z_ok) raw.insert(raw.end(), z.begin() + std::ptrdiff_t(at + 5), z.begin() + std::ptrdiff_t(at + 5 + n));
        at += 5 + n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
    check(z_ok && at + 4 == z.size() && be32(&z[at]) == (b << 16 | a), "the zlib stream: stored blocks and Adler-32");
    bool pixels = raw.size() == (size_t{w} * 3 + 1) * h;
    for (uint32_t y = 0; pixels && y < h; y += 37)
        for (uint32_t x = 0; pixels && x < w; x += 41) {
            const uint8_t* p = &raw[size_t{y} * (w * 3 + 1)];
            pixels = p[0] == 0 && p[1 + x * 3] == uint8_t(x) && p[2 + x * 3] == uint8_t(y) && p[3 + x * 3] == uint8_t(x ^ y);
        }
    check(pixels, "the lines: unfiltered RGB, as given");

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "qilin_capture_test.tar";
    {
        TarWriter tar(file);
        tar.add("capture/info.txt", "hello\n", 6);
        tar.add("capture/empty", "", 0);
        const std::vector<uint8_t> block(512, 0x55);
        tar.add("capture/block.bin", block.data(), block.size());
        check(tar.close(), "a tar is written");
    }
    std::string t;
    {
        std::ifstream in(file, std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        t = buf.str();
    }
    // 512 + 512, 512, 512 + 512, and the two empty blocks at the end.
    check(t.size() == 512 * 7, "headers, contents padded to 512, two empty blocks at the end");
    const auto header_ok = [&](size_t from, const char* name, unsigned long size) {
        if (from + 512 > t.size()) return false;
        const char* hd = t.data() + from;
        unsigned sum = 0;
        for (int i = 0; i < 512; ++i) sum += i >= 148 && i < 156 ? 0x20u : static_cast<unsigned char>(hd[i]);
        return std::strcmp(hd, name) == 0 && std::strtoul(hd + 124, nullptr, 8) == size && std::strtoul(hd + 148, nullptr, 8) == sum &&
               hd[156] == 0x30 && std::memcmp(hd + 257, "ustar", 6) == 0 && hd[263] == 0x30 && hd[264] == 0x30;
    };
    check(header_ok(0, "capture/info.txt", 6) && t.compare(512, 6, "hello\n") == 0, "the first file's header, checksum and contents");
    check(header_ok(1024, "capture/empty", 0) && header_ok(1536, "capture/block.bin", 512), "an empty file and one of exactly a block");
    {
        TarWriter tar(file);
        tar.add(std::string(100, 'n'), "x", 1);
        check(!tar.ok() && !tar.close(), "a name too long for the header is refused, not cut short");
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
