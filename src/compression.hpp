#pragma once

#include <cstddef>
#include <vector>

#include "h5.hpp"
#include "raybotic/scan/writer.hpp"

namespace raybotic::scan::detail {

/// Encodes one chunk exactly like HDF5's filter pipeline would (shuffle, then
/// deflate as a zlib stream), so it can go to H5Dwrite_chunk with filter mask 0.
/// Thread-safe; runs without the HDF5 lock.
std::vector<std::byte> encode_chunk(const std::byte* data, std::size_t size,
                                    std::size_t element_size,
                                    const CompressionOptions& options);

/// Adds the matching filters to a dataset creation property list.
void set_filters(hid_t dcpl, const CompressionOptions& options);

}  // namespace raybotic::scan::detail
