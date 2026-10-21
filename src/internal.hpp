#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "h5.hpp"
#include "raybotic/scan/schema.hpp"
#include "raybotic/scan/types.hpp"

namespace raybotic::scan::detail {

inline constexpr const char* segment_format_name = "raybotic.scan.segment";
/// Group in a segment file whose attributes are the node's attributes, so a
/// node recorded by another process can be recreated in the master.
inline constexpr const char* segment_node_group = "node_attributes";

/// UTF-8 path for the HDF5 C API.
std::string utf8(const std::filesystem::path& path);

std::filesystem::path segment_path(const std::filesystem::path& series_dir, std::uint64_t index);
/// seg_*.h5 files of a series directory, in index order.
std::vector<std::filesystem::path> list_segments(const std::filesystem::path& series_dir);

/// Checks node and attribute names, required attributes and abstractness.
void validate_node(std::string_view name, const Schema& schema, const Attributes& attributes);
/// Writes one attribute, typed by the schema where it declares it.
void write_node_attribute(hid_t obj, const Schema& schema, const std::string& name,
                          const Value& value);

/// Creates sensors/<name> etc. with schema id, schema JSON and attributes.
h5::Id create_node_group(hid_t master, const std::string& path, const Schema& schema,
                         const Attributes& attributes);

/// Finds every segmented series on disk and links it into the master as
/// virtual datasets (replacing older links). Nodes that only exist on disk
/// (recorded by another process) are created from their segment metadata.
void link_all_segments(hid_t master, const std::filesystem::path& scan_dir);

/// ISO 8601 UTC, e.g. 2026-10-21T14:03:07.123Z.
std::string now_iso8601();
std::string make_uuid();

}  // namespace raybotic::scan::detail
