#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "raybotic/scan/export.hpp"
#include "raybotic/scan/types.hpp"

namespace raybotic::scan {

/// One attribute a node of this schema carries.
struct AttributeSpec {
    std::string name;
    DType dtype{DType::String};
    /// 1-D array instead of a scalar.
    bool array{false};
    bool required{false};
    std::string units;
    std::string description;
};

/// One dimension of a field's sample shape: a fixed size, or a name resolved
/// when the series is created (explicit Dims, else a node attribute of that
/// name - an integer, or the length of an array attribute).
struct DimSpec {
    std::string name;  ///< empty for a fixed size
    std::uint64_t size{0};

    [[nodiscard]] bool fixed() const { return name.empty(); }
};

/// One field of a series. Every sample of a series has every field (optional
/// fields left out are filled with NaN / 0 / false).
struct FieldSpec {
    std::string name;
    /// The default type; `dtypes` lists every allowed one (dtype first). A
    /// writer picks another allowed type per series (FieldTypes).
    DType dtype{DType::Float64};
    std::vector<DType> dtypes;
    std::vector<DimSpec> shape;  ///< empty = scalar
    bool optional{false};
    std::string units;
    std::string description;

    [[nodiscard]] bool allows(DType t) const {
        for (auto d : dtypes) {
            if (d == t) return true;
        }
        return t == dtype;
    }
};

enum class Storage {
    /// In the master file, for small data (states, poses, tables).
    Inline,
    /// In segment files next to the master, written by its own writer
    /// (thread or process) and linked into the master as virtual datasets.
    Segmented,
};

/// A time series: a shared time axis (int64 ns) plus fields.
struct SeriesSpec {
    std::string name;
    Storage storage{Storage::Inline};
    std::string description;
    std::vector<FieldSpec> fields;

    [[nodiscard]] RBSCAN_API const FieldSpec* field(std::string_view field_name) const;
};

/// What a node (sensor, robot, frame, acquisition, ...) looks like: its
/// attributes and series. Parsed from JSON (see docs/format.md); "extends" is
/// resolved at parse time, so a Schema is always flat.
struct Schema {
    std::string name;  ///< e.g. raybotic.xray_detector
    std::uint32_t version{1};
    std::string category;  ///< top-level group: sensors, robots, frames, acquisitions, ...
    std::string description;
    bool abstract{false};
    std::vector<AttributeSpec> attributes;
    std::vector<SeriesSpec> series;

    /// name/version, e.g. raybotic.xray_detector/1.
    [[nodiscard]] RBSCAN_API std::string id() const;
    [[nodiscard]] RBSCAN_API const AttributeSpec* attribute(std::string_view attr_name) const;
    [[nodiscard]] RBSCAN_API const SeriesSpec* find_series(std::string_view series_name) const;
    /// Throws Error if the series does not exist.
    [[nodiscard]] RBSCAN_API const SeriesSpec& get_series(std::string_view series_name) const;

    /// Flat JSON of this schema; stored in every node so files describe
    /// themselves without the registry.
    [[nodiscard]] RBSCAN_API std::string to_json() const;
};

RBSCAN_SUPPRESS_DLL_WARNINGS_BEGIN

/// All known schemas. The global registry starts with the schemas compiled into
/// the library (schemas/*.json) and with every *.json in the directories listed
/// in the RBSCAN_SCHEMA_PATH environment variable.
class RBSCAN_API SchemaRegistry {
public:
    static SchemaRegistry& global();

    /// Parses and adds a schema; "extends" must name a schema already known.
    /// Returns the flat schema. Throws on invalid JSON or a different schema
    /// already registered under the same id.
    const Schema& add_json(std::string_view json);
    /// Adds every *.json file of a directory (in name order, retrying so a
    /// schema may extend one defined in a later file).
    void add_directory(const std::filesystem::path& dir);

    /// "name/version" or just "name" for the newest version. Throws if unknown.
    [[nodiscard]] const Schema& get(std::string_view id) const;
    [[nodiscard]] const Schema* find(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> ids() const;

    SchemaRegistry();
    ~SchemaRegistry();
    SchemaRegistry(const SchemaRegistry&) = delete;
    SchemaRegistry& operator=(const SchemaRegistry&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

RBSCAN_SUPPRESS_DLL_WARNINGS_END

/// Parses a flat schema (no "extends"), e.g. the one stored in a file.
[[nodiscard]] RBSCAN_API Schema parse_schema(std::string_view json);

}  // namespace raybotic::scan
