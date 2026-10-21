#pragma once

// Shared by the inline (master) and segmented writers: concrete field shapes,
// chunk sizes, staging of samples into chunk buffers and HDF5 direct chunk
// writes.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "h5.hpp"
#include "raybotic/scan/schema.hpp"
#include "raybotic/scan/writer.hpp"

namespace raybotic::scan::detail {

inline constexpr const char* time_field = "time";

struct FieldLayout {
    std::string name;
    DType dtype{DType::Float64};
    Shape sample_shape;
    std::size_t sample_bytes{0};
    std::uint64_t chunk_samples{1};
    bool optional{false};
    std::string units;
    std::string description;

    [[nodiscard]] std::size_t chunk_bytes() const {
        return static_cast<std::size_t>(chunk_samples) * sample_bytes;
    }
};

/// Fields of a series with concrete shapes; fields[0] is the time axis.
struct SeriesLayout {
    SeriesSpec spec;
    std::vector<FieldLayout> fields;

    /// Resolves named dimensions from `dims`, then from integer attributes and
    /// lengths of array attributes of the node. Chunks hold
    /// max(1, chunk_bytes / sample size) samples, at most max_chunk_samples.
    /// `types` picks another allowed dtype per field.
    static SeriesLayout make(const SeriesSpec& spec, const Dims& dims,
                             const Attributes& node, std::size_t chunk_bytes,
                             std::uint64_t max_chunk_samples,
                             const FieldTypes& types = {});

    /// Index into fields; npos if unknown.
    [[nodiscard]] std::size_t index(std::string_view name) const;
    [[nodiscard]] Shape sample_shape(std::string_view name) const;
};

/// A chunk buffer; uninitialised memory, the unused tail is zeroed on encode.
using ChunkBuffer = std::unique_ptr<std::byte[]>;

/// Converts one sample into the chunk buffers at slot `slot` of each field's
/// current chunk (slot = sample index within the chunk, per field). Allocates
/// missing buffers. Throws for unknown, missing or wrongly sized fields; the
/// buffers may then hold a partial sample, which the next call overwrites.
void stage_sample(const SeriesLayout& layout, std::vector<ChunkBuffer>& buffers,
                  std::span<const std::uint64_t> slots, std::int64_t time_ns,
                  std::span<const FieldValue> fields);

/// Zeroes the bytes after `valid` samples, so partial chunks compress well and
/// carry no stale memory.
void zero_tail(const FieldLayout& field, std::byte* buffer, std::uint64_t valid);

/// Chunked dataset [0.., sample shape] with unlimited first dimension.
h5::Id create_field_dataset(hid_t loc, const FieldLayout& field,
                            const CompressionOptions& compression);

/// Writes an encoded chunk and grows the dataset to at least `extent` samples.
void write_chunk(hid_t dataset, const FieldLayout& field, std::uint64_t chunk_index,
                 std::uint64_t extent, const std::vector<std::byte>& encoded);

}  // namespace raybotic::scan::detail
