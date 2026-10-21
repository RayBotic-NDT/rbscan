#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace raybotic::scan::detail {

/// A second, read-only handle on an HDF5 file for positional reads of raw
/// chunk bytes outside the HDF5 lock. Reads from several threads at once are
/// fine.
class RawFile {
public:
    /// nullptr if the file cannot be opened.
    static std::shared_ptr<RawFile> open(const std::filesystem::path& path);
    ~RawFile();
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;

    /// Reads exactly `size` bytes at `offset`; false on a short read or error.
    bool read(std::uint64_t offset, std::byte* out, std::size_t size) const;

private:
    RawFile() = default;
#ifdef _WIN32
    void* handle_{nullptr};
#else
    int fd_{-1};
#endif
};

}  // namespace raybotic::scan::detail
