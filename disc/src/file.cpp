// off_t is 64 bits wherever this is built for (64-bit hosts only); the
// definition makes that so on a 32-bit libc too.
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "file.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace qilin::disc {

std::string show(const std::filesystem::path& path) {
    // u8string throws for a name that isn't valid Unicode (Windows allows them).
    try {
        const std::u8string text = path.u8string();
        return {text.begin(), text.end()};
    } catch (...) {
        return "(a path that isn't valid Unicode)";
    }
}

std::string hex(uint64_t value) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string digits;
    do {
        digits += kDigits[value & 15];
        value >>= 4;
    } while (value);
    return "0x" + std::string(digits.rbegin(), digits.rend());
}

std::unexpected<std::string> fail(std::string message) { return std::unexpected(std::move(message)); }

File::File(File&& other) noexcept
    : handle_(std::exchange(other.handle_, -1)), size_(other.size_), name_(std::move(other.name_)) {}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, -1);
        size_ = other.size_;
        name_ = std::move(other.name_);
    }
    return *this;
}

File::~File() { close(); }

#if defined(_WIN32)

namespace {
std::string last_error() { return std::error_code(static_cast<int>(GetLastError()), std::system_category()).message(); }
}  // namespace

Result<File> File::open(const std::filesystem::path& path) {
    File file;
    file.name_ = show(path);
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return fail(file.name_ + ": cannot open: " + last_error());
    file.handle_ = reinterpret_cast<intptr_t>(handle);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size)) return fail(file.name_ + ": cannot tell its size: " + last_error());
    file.size_ = static_cast<uint64_t>(size.QuadPart);
    return file;
}

void File::close() {
    if (handle_ != -1) CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = -1;
}

Result<void> File::read_at(uint64_t offset, std::span<uint8_t> out) const {
    const HANDLE handle = reinterpret_cast<HANDLE>(handle_);
    while (!out.empty()) {
        // On a synchronous handle the OVERLAPPED only carries the offset.
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size(), size_t{1} << 30));
        DWORD got = 0;
        if (!ReadFile(handle, out.data(), want, &got, &at)) {
            if (GetLastError() == ERROR_HANDLE_EOF) got = 0;
            else return fail(name_ + ": cannot read at " + hex(offset) + ": " + last_error());
        }
        if (got == 0) return fail(name_ + ": read at " + hex(offset) + " is past the end of the file");
        offset += got;
        out = out.subspan(got);
    }
    return {};
}

#else

namespace {
std::string last_error() { return std::error_code(errno, std::generic_category()).message(); }
}  // namespace

Result<File> File::open(const std::filesystem::path& path) {
    File file;
    file.name_ = show(path);
    int fd;
    do fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) return fail(file.name_ + ": cannot open: " + last_error());
    file.handle_ = fd;
    struct stat info;
    if (::fstat(fd, &info) != 0) return fail(file.name_ + ": cannot tell its size: " + last_error());
    if (!S_ISREG(info.st_mode)) return fail(file.name_ + ": not a regular file");
    file.size_ = static_cast<uint64_t>(info.st_size);
    return file;
}

void File::close() {
    if (handle_ != -1) ::close(static_cast<int>(handle_));
    handle_ = -1;
}

Result<void> File::read_at(uint64_t offset, std::span<uint8_t> out) const {
    while (!out.empty()) {
        const size_t want = std::min<size_t>(out.size(), size_t{1} << 30);
        const ssize_t got = ::pread(static_cast<int>(handle_), out.data(), want, static_cast<off_t>(offset));
        if (got < 0) {
            if (errno == EINTR) continue;
            return fail(name_ + ": cannot read at " + hex(offset) + ": " + last_error());
        }
        if (got == 0) return fail(name_ + ": read at " + hex(offset) + " is past the end of the file");
        offset += static_cast<uint64_t>(got);
        out = out.subspan(static_cast<size_t>(got));
    }
    return {};
}

#endif

}  // namespace qilin::disc
