#pragma once

#include <cstddef>
#include <vector>

#include "h5.hpp"
#include "raybotic/scan/writer.hpp"

namespace raybotic::scan::detail {

/// HDF5 filter id of Bitshuffle (registered with The HDF Group).
inline constexpr H5Z_filter_t bitshuffle_filter_id = 32008;
/// Its cd_values[4] for LZ4 compression after the bit shuffle.
inline constexpr unsigned bitshuffle_lz4 = 2;

/// Encodes one chunk exactly like HDF5's filter pipeline would (shuffle, then
/// deflate as a zlib stream; or bitshuffle + LZ4), so it can go to
/// H5Dwrite_chunk with filter mask 0. Thread-safe; runs without the HDF5 lock.
std::vector<std::byte> encode_chunk(const std::byte* data, std::size_t size,
                                    std::size_t element_size,
                                    const CompressionOptions& options);

/// Undoes encode_chunk: inflate (if `deflated`), then unshuffle (if
/// `shuffled`), into exactly `out_size` bytes. Thread-safe; runs without the
/// HDF5 lock. Throws Error on corrupt data or a size mismatch.
void decode_chunk(const std::byte* data, std::size_t size, std::byte* out,
                  std::size_t out_size, std::size_t element_size, bool deflated,
                  bool shuffled);

/// Undoes the Bitshuffle+LZ4 filter (header, then LZ4 blocks of bit-shuffled
/// elements) into exactly `out_size` bytes. Thread-safe; throws Error on
/// corrupt data or a size mismatch.
void decode_bitshuffle_lz4(const std::byte* data, std::size_t size, std::byte* out,
                           std::size_t out_size, std::size_t element_size);

/// Registers rbscan's Bitshuffle+LZ4 implementation as HDF5 filter 32008, so
/// datasets using it can be created and read through H5Dread (VDS, partial
/// reads). Called by h5::init() under the HDF5 lock.
void register_filters();

/// Adds the matching filters to a dataset creation property list.
void set_filters(hid_t dcpl, const CompressionOptions& options);
/// The options set_filters() was given for `dcpl`; throws for a filter
/// pipeline rbscan does not write.
[[nodiscard]] CompressionOptions filters_of(hid_t dcpl);

}  // namespace raybotic::scan::detail
