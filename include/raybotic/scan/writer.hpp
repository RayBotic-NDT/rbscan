#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "raybotic/scan/export.hpp"
#include "raybotic/scan/schema.hpp"
#include "raybotic/scan/types.hpp"

namespace raybotic::scan {

RBSCAN_SUPPRESS_DLL_WARNINGS_BEGIN

/// Name of the master file inside a scan directory.
inline constexpr std::string_view master_file_name = "scan.rbscan.h5";
inline constexpr std::string_view format_name = "raybotic.scan";
inline constexpr std::string_view format_version = "1.0";

enum class Compression {
    None,
    /// zlib stream (HDF5 deflate filter).
    Deflate,
    /// Byte shuffle + deflate (HDF5 shuffle + deflate filters). Best ratio for
    /// 16-bit images, and every HDF5 tool reads it without a plugin.
    ShuffleDeflate,
    /// Bitshuffle + LZ4 (HDF5 filter 32008, as used by Dectris and others):
    /// decodes several times faster than deflate at a somewhat larger size.
    /// rbscan reads and writes it natively; h5py needs `import hdf5plugin`,
    /// other HDF5 tools the bitshuffle filter plugin. `level` is ignored. The
    /// default.
    BitshuffleLZ4,
};

struct CompressionOptions {
    Compression method{Compression::BitshuffleLZ4};
    /// Deflate: 1 (fast) .. 9 (small). Level 1 is usually the right call for
    /// projections.
    int level{1};
};

/// Options for a series stored in the master file.
struct InlineOptions {
    CompressionOptions compression{};
    /// Data types for fields that allow several (default: the schema's first).
    FieldTypes dtypes{};
    /// Target chunk size; samples per chunk = max(1, chunk_bytes / sample size).
    std::size_t chunk_bytes{256 * 1024};
};

/// Options for a series stored in segment files.
struct SegmentOptions {
    /// Samples per segment file. Projections: ~500 keeps files at a few GB.
    std::uint64_t segment_size{500};
    /// Data types for fields that allow several (default: the schema's first),
    /// e.g. {"image", DType::Float32} for corrected or simulated projections.
    FieldTypes dtypes{};
    CompressionOptions compression{};
    /// Compression threads of this writer; 0 = half the hardware threads.
    unsigned threads{0};
    /// Chunks waiting for compression or for the disk before append() blocks.
    std::size_t max_pending{64};
    /// Target chunk size; large samples (images) get one chunk each.
    std::size_t chunk_bytes{1024 * 1024};
    /// Segment files in SWMR mode, so other processes can read them live.
    bool swmr{true};
    /// Make the newest samples visible to SWMR readers every n samples
    /// (0 = only when a segment is closed or flush() is called).
    std::uint64_t flush_every{16};
};

struct ScanInfo {
    std::string description{};
    std::string operator_name{};
    std::string sample_id{};
    /// Anything else, stored as root attributes.
    Attributes extra{};
};

/// Writes one series into the master file. Appends are buffered per chunk and
/// written when a chunk is full, on flush() and on finalize.
///
/// Handles are cheap to copy and share one writer. One writer must be used by
/// one thread at a time; different writers may run on different threads.
class RBSCAN_API SeriesWriter {
  public:
    /// Every non-optional field must be given; values are converted to the
    /// field's dtype and must have the field's element count.
    void append(std::int64_t time_ns, std::span<const FieldValue> fields);
    void append(std::int64_t time_ns, std::initializer_list<FieldValue> fields) {
        append(time_ns, std::span<const FieldValue>(fields.begin(), fields.size()));
    }
    void flush();
    [[nodiscard]] std::uint64_t size() const;
    [[nodiscard]] const SeriesSpec& spec() const;
    /// Concrete sample shape of a field.
    [[nodiscard]] Shape sample_shape(std::string_view field) const;

    struct Impl;
    explicit SeriesWriter(std::shared_ptr<Impl> impl);

  private:
    std::shared_ptr<Impl> impl_;
};

/// Writes one series into segment files <scan>/<node path>/<series>/seg_NNNNNN.h5.
///
/// append() copies the sample into the current chunk and returns; full chunks
/// are compressed by a thread pool and written in order by a writer thread
/// (HDF5 direct chunk write). It blocks only when max_pending chunks are queued.
/// Errors of the background threads are rethrown by the next call.
///
/// Independent of the master file: a separate process can record a sensor with
/// open() while another one writes the master. ScanWriter::finalize() (or
/// rebuild()) links all segments it finds into the master.
class RBSCAN_API SegmentedSeriesWriter {
  public:
    /// Writer for a node created by a ScanWriter in another process (or not at
    /// all yet). schema_id must be known to SchemaRegistry::global(); dims not
    /// given are resolved from `attributes`.
    [[nodiscard]] static SegmentedSeriesWriter open(
        const std::filesystem::path& scan_dir, std::string_view node_path,
        std::string_view schema_id, std::string_view series, const Dims& dims = {},
        const SegmentOptions& options = {}, const Attributes& attributes = {});

    void append(std::int64_t time_ns, std::span<const FieldValue> fields);
    void append(std::int64_t time_ns, std::initializer_list<FieldValue> fields) {
        append(time_ns, std::span<const FieldValue>(fields.begin(), fields.size()));
    }
    /// Waits until everything appended so far is on disk and visible to readers.
    void flush();
    /// Flushes, closes the current segment and stops the threads. Idempotent.
    void close();
    [[nodiscard]] std::uint64_t size() const;
    [[nodiscard]] std::uint64_t segment_count() const;
    [[nodiscard]] const SeriesSpec& spec() const;
    [[nodiscard]] Shape sample_shape(std::string_view field) const;
    [[nodiscard]] std::filesystem::path directory() const;

    struct Impl;
    explicit SegmentedSeriesWriter(std::shared_ptr<Impl> impl);

  private:
    std::shared_ptr<Impl> impl_;
};

/// A node in the master file: sensors/<name>, robots/<name>, ...
class RBSCAN_API NodeWriter {
  public:
    [[nodiscard]] std::string path() const;
    [[nodiscard]] const Schema& schema() const;

    /// Sets or replaces one attribute (checked against the schema if it is
    /// declared there; undeclared attributes are allowed).
    void set_attribute(std::string_view name, const Value& value);

    /// The writer of an inline series; the same writer on every call.
    [[nodiscard]] SeriesWriter series(std::string_view name, const Dims& dims = {},
                                      const InlineOptions& options = {});
    /// The writer of a segmented series; the same writer on every call. It is
    /// closed by ScanWriter::finalize().
    [[nodiscard]] SegmentedSeriesWriter segmented_series(
        std::string_view name, const Dims& dims = {}, const SegmentOptions& options = {});

    struct Impl;
    explicit NodeWriter(std::shared_ptr<Impl> impl);

  private:
    std::shared_ptr<Impl> impl_;
};

/// Creates a scan directory with its master file and writes it.
///
///   auto scan = ScanWriter::create("D:/scans/part_42");
///   auto det = scan.add_node("detector_0", "raybotic.xray_detector", {...});
///   auto images = det.segmented_series("image", {{"height", h}, {"width", w}});
///   images.append(t, {{"image", view(pixels, {h, w})}});
///   scan.finalize();
class RBSCAN_API ScanWriter {
  public:
    /// Fails if the directory already holds a master file, unless overwrite.
    [[nodiscard]] static ScanWriter create(const std::filesystem::path& scan_dir,
                                           const ScanInfo& info = {},
                                           bool overwrite = false);

    /// Opens an existing scan to add or replace nodes (annotations, results,
    /// a resolved geometry, ...). finalize() relinks segments and keeps the
    /// scan's `complete` flag as it was. Fails while another writer or reader
    /// has the master file open.
    [[nodiscard]] static ScanWriter open(const std::filesystem::path& scan_dir);

    /// Adds a node under the schema's category. Required attributes must be
    /// given. Abstract schemas cannot be instantiated.
    NodeWriter add_node(std::string_view name, std::string_view schema_id,
                        const Attributes& attributes = {});
    /// A node by path ("sensors/detector_0"): one added by this writer, or one
    /// of an earlier session, whose attributes can then be changed and whose
    /// stored inline series can be appended to (series()); it takes no new
    /// series.
    [[nodiscard]] NodeWriter node(std::string_view path) const;
    /// Whether the master holds a node at `path` (from this or an earlier session).
    [[nodiscard]] bool has_node(std::string_view path) const;
    /// Removes a node from the master. Refused for nodes with segment files on
    /// disk (finalize would link them in again) and for nodes added by this
    /// writer.
    void remove_node(std::string_view path);

    /// Writes buffered inline samples and flushes the master file.
    void flush();
    /// Closes all series writers of this scan, links every segment found on
    /// disk into the master (virtual datasets), marks the scan complete and
    /// closes the file. Idempotent; called by the destructor (errors ignored).
    void finalize();
    [[nodiscard]] std::filesystem::path directory() const;

    /// Relinks the segments of an existing scan, e.g. after a crash or after
    /// other processes finished writing. Marks it complete.
    static void rebuild(const std::filesystem::path& scan_dir);

    ~ScanWriter();
    ScanWriter(ScanWriter&&) noexcept;
    ScanWriter& operator=(ScanWriter&&) noexcept;
    ScanWriter(const ScanWriter&) = delete;
    ScanWriter& operator=(const ScanWriter&) = delete;

    struct Impl;

  private:
    explicit ScanWriter(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

RBSCAN_SUPPRESS_DLL_WARNINGS_END

}  // namespace raybotic::scan
