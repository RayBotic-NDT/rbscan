#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace raybotic::scan::detail {

std::string utf8(const std::filesystem::path& path) {
    const auto s = path.generic_u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

std::filesystem::path segment_path(const std::filesystem::path& series_dir, std::uint64_t index) {
    char name[32];
    std::snprintf(name, sizeof(name), "seg_%06llu.h5", static_cast<unsigned long long>(index));
    return series_dir / name;
}

std::vector<std::filesystem::path> list_segments(const std::filesystem::path& series_dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(series_dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(series_dir, ec)) {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file() && name.size() == 13 && name.rfind("seg_", 0) == 0 &&
            entry.path().extension() == ".h5") {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void validate_node(std::string_view name, const Schema& schema, const Attributes& attributes) {
    const bool name_ok = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.';
    });
    if (!name_ok) {
        throw Error("invalid node name '" + std::string(name) + "' (letters, digits, _ - .)");
    }
    if (schema.abstract) throw Error("schema " + schema.id() + " is abstract");
    for (const auto& a : schema.attributes) {
        if (a.required && !attributes.contains(a.name)) {
            throw Error("node '" + std::string(name) + "' (" + schema.id() +
                        "): required attribute '" + a.name + "' is missing");
        }
    }
    for (const auto& [key, value] : attributes) {
        if (key == "schema" || key == "schema_json") {
            throw Error("attribute name '" + key + "' is reserved");
        }
    }
}

void write_node_attribute(hid_t obj, const Schema& schema, const std::string& name,
                          const Value& value) {
    if (const auto* spec = schema.attribute(name)) {
        h5::write_attribute(obj, name, value, spec->dtype, spec->array);
    } else {
        h5::write_attribute(obj, name, value);
    }
}

h5::Id create_node_group(hid_t master, const std::string& path, const Schema& schema,
                         const Attributes& attributes) {
    h5::Lock lock(h5::mutex());
    if (h5::exists(master, path)) throw Error("node '" + path + "' exists already");
    h5::Id group = h5::ensure_group(master, path);
    try {
        h5::write_string_attribute(group.get(), "schema", schema.id());
        h5::write_string_attribute(group.get(), "schema_json", schema.to_json());
        for (const auto& [name, value] : attributes) {
            write_node_attribute(group.get(), schema, name, value);
        }
    } catch (...) {
        // No half-written node: a corrected add_node() must be able to retry.
        group.reset();
        H5Ldelete(master, path.c_str(), H5P_DEFAULT);
        H5Eclear2(H5E_DEFAULT);
        throw;
    }
    return group;
}

std::string now_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms));
    return buf;
}

std::string make_uuid() {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                        static_cast<std::uint64_t>(
                            std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::uint64_t hi = gen();
    std::uint64_t lo = gen();
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // RFC 4122 variant
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08llx-%04llx-%04llx-%04llx-%012llx",
                  static_cast<unsigned long long>(hi >> 32),
                  static_cast<unsigned long long>((hi >> 16) & 0xFFFF),
                  static_cast<unsigned long long>(hi & 0xFFFF),
                  static_cast<unsigned long long>(lo >> 48),
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return buf;
}

}  // namespace raybotic::scan::detail
