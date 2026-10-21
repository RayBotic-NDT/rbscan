#pragma once

// Utilities on whole scans: summary (info), consistency checks (validate) and
// the projection geometry resolved from the recorded frames.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "raybotic/scan/export.hpp"
#include "raybotic/scan/schema.hpp"
#include "raybotic/scan/types.hpp"

namespace raybotic::scan {

RBSCAN_SUPPRESS_DLL_WARNINGS_BEGIN

// ---------------------------------------------------------------------------
// info

struct FieldSummary {
    std::string name;
    DType dtype{DType::Float64};
    Shape sample_shape;
    std::string units;
};

struct SeriesSummary {
    std::string name;
    Storage storage{Storage::Inline};
    std::uint64_t samples{0};
    std::int64_t first_time{0};
    std::int64_t last_time{0};
    /// Mean sample rate over the series; 0 for fewer than two samples.
    double rate_hz{0.0};
    std::vector<FieldSummary> fields;
    /// Uncompressed size of all fields including time.
    std::uint64_t raw_bytes{0};
    /// Size on disk (datasets in the master, or the segment files).
    std::uint64_t stored_bytes{0};
    std::uint64_t segments{0};
};

struct NodeSummary {
    std::string path;
    std::string schema_id;
    Attributes attributes;
    std::vector<SeriesSummary> series;
};

struct ScanSummary {
    std::filesystem::path directory;
    std::string format_version;
    bool complete{false};
    Attributes attributes;
    std::vector<NodeSummary> nodes;
    std::uint64_t raw_bytes{0};
    std::uint64_t stored_bytes{0};
};

/// Reads the structure and the time axes of a scan (not the data).
[[nodiscard]] RBSCAN_API ScanSummary summarize(const std::filesystem::path& scan);
/// Human-readable overview; with `attributes` also every node attribute.
[[nodiscard]] RBSCAN_API std::string to_text(const ScanSummary& summary, bool attributes = false);
[[nodiscard]] RBSCAN_API std::string to_json(const ScanSummary& summary);

// ---------------------------------------------------------------------------
// validate

enum class Severity { Error, Warning, Info };

struct Issue {
    Severity severity{Severity::Error};
    /// Where: "", a node path, "<node>/<series>" or "<node>/<series>/<field>".
    std::string path;
    std::string message;
};

struct ValidationReport {
    std::vector<Issue> issues;

    [[nodiscard]] std::size_t count(Severity severity) const {
        std::size_t n = 0;
        for (const auto& i : issues) n += i.severity == severity ? 1 : 0;
        return n;
    }
    /// No errors (warnings are allowed).
    [[nodiscard]] bool ok() const { return count(Severity::Error) == 0; }
};

struct ValidateOptions {
    /// Also check values of small fields: NaN in required float fields, unit
    /// quaternions, unit and orthogonal detector vectors, indices in range.
    /// Large fields (images) are never read.
    bool check_values{true};
    /// Allowed difference between a projection's time and its image's time.
    double time_tolerance_s{1e-3};
};

/// Checks a scan against its schemas and its own links: required attributes
/// and their types, fields and shapes, equal lengths, time order, segment
/// sequence and virtual dataset links, references between nodes, geometry.
[[nodiscard]] RBSCAN_API ValidationReport validate(const std::filesystem::path& scan,
                                                   const ValidateOptions& options = {});
[[nodiscard]] RBSCAN_API std::string to_text(const ValidationReport& report);
[[nodiscard]] RBSCAN_API std::string to_json(const ValidationReport& report);

// ---------------------------------------------------------------------------
// frames and geometry

/// Rigid transform: rotation as unit quaternion x, y, z, w; translation in m.
struct Pose {
    std::array<double, 3> translation{0.0, 0.0, 0.0};
    std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
};

/// The TF tree stored under frames/, with interpolation in time (linear for
/// translations, slerp for rotations; static frames are constant).
class RBSCAN_API FrameGraph {
public:
    /// Loads every raybotic.transform node of a scan.
    [[nodiscard]] static FrameGraph load(const std::filesystem::path& scan);

    /// Pose of `child` in `parent` at time t. `extrapolated` is set when t lies
    /// outside the recorded range of a dynamic frame on the way (the nearest
    /// sample is used then). Throws if the frames are not connected.
    [[nodiscard]] Pose lookup(std::string_view parent, std::string_view child, std::int64_t time_ns,
                              bool* extrapolated = nullptr) const;
    [[nodiscard]] bool has_frame(std::string_view frame) const;
    /// All frame names (parents and children).
    [[nodiscard]] std::vector<std::string> frames() const;

    struct Impl;
    explicit FrameGraph(std::shared_ptr<Impl> impl);

private:
    std::shared_ptr<Impl> impl_;
};

struct GeometryOptions {
    /// Name of the raybotic.ct_acquisition node to fill (created if missing).
    std::string acquisition{"scan_0"};
    /// Node paths; empty = the only raybotic.xray_source / raybotic.xray_detector.
    std::string source;
    std::string detector;
    std::string world_frame{"world"};
    /// Empty = the frame_id attribute of the source / detector node.
    std::string source_frame;
    std::string detector_frame;
    /// Also write projection_matrix (world point in m -> column, row).
    bool projection_matrix{true};
    /// Replace an existing projections series.
    bool overwrite{false};
};

struct GeometryResult {
    std::string acquisition;  ///< node path
    std::uint64_t projections{0};
    std::uint64_t interpolated{0};
    std::uint64_t extrapolated{0};      ///< flagged invalid (bit 0)
    std::uint64_t robot_unreachable{0};  ///< bit 2
    std::uint64_t emission_unstable{0};  ///< bit 3
};

/// Writes acquisitions/<name>/projections from the detector's image times, the
/// frames (TF), the detector's pixel pitch and binning and the source state:
/// one row per image with source and detector position, detector axes, pixel
/// size, projection matrix, kV, uA, integration time and flags. The scan must
/// not be open elsewhere.
RBSCAN_API GeometryResult resolve_geometry(const std::filesystem::path& scan,
                                           const GeometryOptions& options = {});

/// Projection matrix [3x4] row-major: world point (m) -> (column, row) of a
/// detector with `width` x `height` pixels whose centre is `detector_position`.
[[nodiscard]] RBSCAN_API std::array<double, 12> projection_matrix(
    const std::array<double, 3>& source_position, const std::array<double, 3>& detector_position,
    const std::array<double, 3>& detector_u, const std::array<double, 3>& detector_v,
    double pixel_u, double pixel_v, std::uint64_t width, std::uint64_t height);

RBSCAN_SUPPRESS_DLL_WARNINGS_END

}  // namespace raybotic::scan
