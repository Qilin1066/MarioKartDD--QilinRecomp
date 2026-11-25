// A file opened once and read at any offset from any thread (pread, or
// ReadFile with the offset in an OVERLAPPED): no seek state to share.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "qilin/disc/disc.hpp"

namespace qilin::disc {

std::string show(const std::filesystem::path& path);  // UTF-8, for messages
std::string hex(uint64_t value);                      // "0x1F700"
std::unexpected<std::string> fail(std::string message);
// Whether an FST name can be a file's name on every host: printable ASCII,
// none of / \ : * ? " < > |, not "." or "..", no space or full stop last.
bool host_safe_name(std::string_view name);
std::string lower(std::string_view text);  // ASCII letters only, as the console compares paths

inline uint32_t be32(const uint8_t* p) {
    return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | uint32_t{p[3]};
}

class File {
public:
    static Result<File> open(const std::filesystem::path& path);
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    ~File();

    uint64_t size() const { return size_; }  // when it was opened
    // All of `out`, or an error (a read past the end of the file is one).
    Result<void> read_at(uint64_t offset, std::span<uint8_t> out) const;

private:
    File() = default;
    void close();
    intptr_t handle_ = -1;  // a HANDLE or a file descriptor
    uint64_t size_ = 0;
    std::string name_;
};

}  // namespace qilin::disc
