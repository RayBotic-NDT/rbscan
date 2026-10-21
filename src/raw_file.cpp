#include "raw_file.hpp"

#include <algorithm>
#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace raybotic::scan::detail {

#ifdef _WIN32

std::shared_ptr<RawFile> RawFile::open(const std::filesystem::path& path) {
    // Share everything: HDF5 (and a SWMR writer) keep their own handles open.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    std::shared_ptr<RawFile> f(new RawFile());
    f->handle_ = h;
    return f;
}

RawFile::~RawFile() {
    if (handle_ != nullptr) CloseHandle(handle_);
}

bool RawFile::read(std::uint64_t offset, std::byte* out, std::size_t size) const {
    while (size > 0) {
        // An explicit offset per call, so concurrent reads do not interfere.
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        const auto want = static_cast<DWORD>(
            std::min<std::size_t>(size, std::numeric_limits<DWORD>::max() / 2));
        DWORD got = 0;
        if (!ReadFile(handle_, out, want, &got, &ov) || got == 0) return false;
        out += got;
        offset += got;
        size -= got;
    }
    return true;
}

#else

std::shared_ptr<RawFile> RawFile::open(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    std::shared_ptr<RawFile> f(new RawFile());
    f->fd_ = fd;
    return f;
}

RawFile::~RawFile() {
    if (fd_ >= 0) ::close(fd_);
}

bool RawFile::read(std::uint64_t offset, std::byte* out, std::size_t size) const {
    while (size > 0) {
        const ssize_t got = ::pread(fd_, out, size, static_cast<off_t>(offset));
        if (got <= 0) return false;
        out += got;
        offset += static_cast<std::uint64_t>(got);
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

#endif

}  // namespace raybotic::scan::detail
