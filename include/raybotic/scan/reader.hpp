#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "raybotic/scan/export.hpp"
#include "raybotic/scan/schema.hpp"
#include "raybotic/scan/types.hpp"

namespace raybotic::scan {

RBSCAN_SUPPRESS_DLL_WARNINGS_BEGIN

inline constexpr std::uint64_t all = std::numeric_limits<std::uint64_t>::max();

/// Reads one series. Inline series come from the master file; segmented ones
/// straight from their segment files (opened SWMR, so a recording in progress
/// can be read - call refresh() to see new samples).
class RBSCAN_API SeriesReader {
  public:
    /// Reads a segmented series straight from its segment files, without the
    /// master file - e.g. to watch a recording while the master is still open
    /// for writing. Throws if no segment exists yet.
    [[nodiscard]] static SeriesReader open_segments(const std::filesystem::path& scan_dir,
                                                    std::string_view node_path,
                                                    std::string_view series);

    [[nodiscard]] std::string name() const;
    [[nodiscard]] Storage storage() const;
    [[nodiscard]] std::uint64_t size() const;
    /// Field names in schema order (without "time").
    [[nodiscard]] std::vector<std::string> fields() const;
    [[nodiscard]] DType dtype(std::string_view field) const;
    [[nodiscard]] Shape sample_shape(std::string_view field) const;
    [[nodiscard]] std::string units(std::string_view field) const;

    /// Time stamps [start, start + count), ns.
    [[nodiscard]] std::vector<std::int64_t> time(std::uint64_t start = 0,
                                                 std::uint64_t count = all) const;
    /// Samples [start, start + count) of a field: shape [count, sample shape...].
    [[nodiscard]] Array read(std::string_view field, std::uint64_t start = 0,
                             std::uint64_t count = all) const;
    /// One sample: shape = sample shape.
    [[nodiscard]] Array read_sample(std::string_view field, std::uint64_t index) const;
    /// Index of the last sample with time <= time_ns, if any.
    [[nodiscard]] std::optional<std::uint64_t> index_at(std::int64_t time_ns) const;

    /// Picks up samples and segments written since the reader was opened.
    void refresh();

    struct Impl;
    explicit SeriesReader(std::shared_ptr<Impl> impl);

  private:
    std::shared_ptr<Impl> impl_;
};

class RBSCAN_API NodeReader {
  public:
    [[nodiscard]] std::string path() const;
    [[nodiscard]] std::string name() const;
    [[nodiscard]] std::string category() const;
    /// The schema stored with the node (independent of the registry).
    [[nodiscard]] const Schema& schema() const;
    [[nodiscard]] Attributes attributes() const;
    [[nodiscard]] std::optional<Value> attribute(std::string_view name) const;
    /// Series that hold data, in schema order.
    [[nodiscard]] std::vector<std::string> series_names() const;
    [[nodiscard]] bool has_series(std::string_view name) const;
    [[nodiscard]] SeriesReader series(std::string_view name) const;

    struct Impl;
    explicit NodeReader(std::shared_ptr<Impl> impl);

  private:
    std::shared_ptr<Impl> impl_;
};

class RBSCAN_API ScanReader {
  public:
    /// The scan directory or its master file.
    [[nodiscard]] static ScanReader open(const std::filesystem::path& path);

    [[nodiscard]] std::filesystem::path directory() const;
    [[nodiscard]] std::string format_version() const;
    /// false while recording or after a crash (see ScanWriter::rebuild).
    [[nodiscard]] bool complete() const;
    /// Root attributes: uuid, created, description, operator, ...
    [[nodiscard]] Attributes attributes() const;

    [[nodiscard]] std::vector<std::string> categories() const;
    /// Node paths ("sensors/detector_0"), of one category or all.
    [[nodiscard]] std::vector<std::string> node_paths(
        std::string_view category = {}) const;
    [[nodiscard]] bool has_node(std::string_view path) const;
    [[nodiscard]] NodeReader node(std::string_view path) const;

    struct Impl;

  private:
    explicit ScanReader(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

RBSCAN_SUPPRESS_DLL_WARNINGS_END

}  // namespace raybotic::scan
