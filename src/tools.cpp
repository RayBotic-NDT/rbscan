// summarize (info) and validate.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

#include "convert.hpp"
#include "internal.hpp"
#include "raybotic/scan/rbscan.hpp"
#include "raybotic/scan/tools.hpp"
#include "series_layout.hpp"

namespace raybotic::scan {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

namespace {

fs::path master_of(const fs::path& scan) {
    return fs::is_directory(scan) ? scan / master_file_name : scan;
}

std::uint64_t size_on_disk(const fs::path& p) {
    std::error_code ec;
    const auto s = fs::file_size(p, ec);
    return ec ? 0 : s;
}

std::string bytes_text(std::uint64_t b) {
    char buf[32];
    const double v = static_cast<double>(b);
    if (v >= 1e9) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", v / 1e9);
    } else if (v >= 1e6) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", v / 1e6);
    } else if (v >= 1e3) {
        std::snprintf(buf, sizeof(buf), "%.1f kB", v / 1e3);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
    }
    return buf;
}

std::string shape_text(const Shape& s) {
    if (s.empty()) return {};
    std::string out = "[";
    for (std::size_t i = 0; i < s.size(); ++i) {
        out += (i ? ", " : "") + std::to_string(s[i]);
    }
    return out + "]";
}

Json to_json_value(const Value& v) {
    return std::visit([](const auto& x) { return Json(x); }, v);
}

/// Sum of the stored sizes of the datasets in a master group.
std::uint64_t stored_size(hid_t master, const std::string& group_path) {
    h5::Lock lock(h5::mutex());
    if (!h5::exists(master, group_path)) return 0;
    h5::Id group = h5::open_group(master, group_path);
    std::uint64_t total = 0;
    for (const auto& name : h5::children(group.get())) {
        if (h5::is_group(group.get(), name)) continue;
        h5::Id d = h5::open_dataset(group.get(), name);
        total += H5Dget_storage_size(d.get());
    }
    return total;
}

}  // namespace

// ---------------------------------------------------------------------------
// summarize

ScanSummary summarize(const fs::path& scan_path) {
    const ScanReader scan = ScanReader::open(scan_path);
    h5::Id master = h5::open_file(detail::utf8(master_of(scan_path)), H5F_ACC_RDONLY);

    ScanSummary out;
    out.directory = scan.directory();
    out.format_version = scan.format_version();
    out.complete = scan.complete();
    out.attributes = scan.attributes();
    out.stored_bytes = size_on_disk(master_of(scan_path));

    for (const auto& path : scan.node_paths()) {
        const NodeReader node = scan.node(path);
        NodeSummary ns;
        ns.path = path;
        ns.schema_id = node.schema().id();
        ns.attributes = node.attributes();
        for (const auto& name : node.series_names()) {
            const SeriesReader s = node.series(name);
            SeriesSummary ss;
            ss.name = name;
            ss.storage = s.storage();
            ss.samples = s.size();
            if (ss.samples > 0) {
                ss.first_time = s.time(0, 1).front();
                ss.last_time = s.time(ss.samples - 1, 1).front();
                if (ss.samples > 1 && ss.last_time > ss.first_time) {
                    ss.rate_hz = static_cast<double>(ss.samples - 1) /
                                 (static_cast<double>(ss.last_time - ss.first_time) * 1e-9);
                }
            }
            std::uint64_t sample_bytes = sizeof(std::int64_t);
            for (const auto& f : s.fields()) {
                FieldSummary fs_;
                fs_.name = f;
                fs_.dtype = s.dtype(f);
                fs_.sample_shape = s.sample_shape(f);
                fs_.units = s.units(f);
                sample_bytes += element_count(fs_.sample_shape) * dtype_size(fs_.dtype);
                ss.fields.push_back(std::move(fs_));
            }
            ss.raw_bytes = sample_bytes * ss.samples;
            if (ss.storage == Storage::Segmented) {
                const auto segs = detail::list_segments(scan.directory() / fs::path(path) / name);
                ss.segments = segs.size();
                for (const auto& f : segs) ss.stored_bytes += size_on_disk(f);
                out.stored_bytes += ss.stored_bytes;
            } else {
                ss.stored_bytes = stored_size(master.get(), path + "/" + name);
            }
            out.raw_bytes += ss.raw_bytes;
            ns.series.push_back(std::move(ss));
        }
        out.nodes.push_back(std::move(ns));
    }
    return out;
}

std::string to_text(const ScanSummary& s, bool attributes) {
    std::ostringstream out;
    out << detail::utf8(s.directory) << "\n";
    out << "  format " << s.format_version << ", " << (s.complete ? "complete" : "NOT complete");
    for (const char* key : {"uuid", "created"}) {
        if (auto it = s.attributes.find(key); it != s.attributes.end()) {
            out << ", " << key << " " << std::get<std::string>(it->second);
        }
    }
    out << "\n";
    for (const char* key : {"description", "operator", "sample_id"}) {
        if (auto it = s.attributes.find(key); it != s.attributes.end()) {
            out << "  " << key << ": " << to_string(it->second) << "\n";
        }
    }
    out << "  " << bytes_text(s.raw_bytes) << " raw, " << bytes_text(s.stored_bytes)
        << " on disk";
    if (s.raw_bytes > 0) {
        char ratio[32];
        std::snprintf(ratio, sizeof(ratio), " (%.0f %%)",
                      100.0 * static_cast<double>(s.stored_bytes) /
                          static_cast<double>(s.raw_bytes));
        out << ratio;
    }
    out << "\n";

    for (const auto& n : s.nodes) {
        out << "\n" << n.path << "  (" << n.schema_id << ")\n";
        if (attributes) {
            for (const auto& [key, value] : n.attributes) {
                std::string text = to_string(value);
                if (text.size() > 100) text = text.substr(0, 97) + "...";
                out << "    @" << key << " = " << text << "\n";
            }
        }
        for (const auto& ss : n.series) {
            char line[256];
            const double seconds = static_cast<double>(ss.last_time - ss.first_time) * 1e-9;
            std::snprintf(line, sizeof(line), "  %-16s %-9s %8llu samples %9.1f s %9.2f Hz",
                          ss.name.c_str(), ss.storage == Storage::Segmented ? "segmented" : "inline",
                          static_cast<unsigned long long>(ss.samples), seconds, ss.rate_hz);
            out << line;
            if (ss.segments > 0) out << "  " << ss.segments << " segments";
            out << "  " << bytes_text(ss.raw_bytes) << " -> " << bytes_text(ss.stored_bytes) << "\n";
            for (const auto& f : ss.fields) {
                out << "      " << f.name << " " << to_string(f.dtype) << shape_text(f.sample_shape);
                if (!f.units.empty()) out << " " << f.units;
                out << "\n";
            }
        }
    }
    return out.str();
}

std::string to_json(const ScanSummary& s) {
    Json j;
    j["directory"] = detail::utf8(s.directory);
    j["format_version"] = s.format_version;
    j["complete"] = s.complete;
    j["raw_bytes"] = s.raw_bytes;
    j["stored_bytes"] = s.stored_bytes;
    Json attrs = Json::object();
    for (const auto& [k, v] : s.attributes) attrs[k] = to_json_value(v);
    j["attributes"] = attrs;
    Json nodes = Json::array();
    for (const auto& n : s.nodes) {
        Json jn;
        jn["path"] = n.path;
        jn["schema"] = n.schema_id;
        Json na = Json::object();
        for (const auto& [k, v] : n.attributes) na[k] = to_json_value(v);
        jn["attributes"] = na;
        Json series = Json::array();
        for (const auto& ss : n.series) {
            Json js;
            js["name"] = ss.name;
            js["storage"] = ss.storage == Storage::Segmented ? "segmented" : "inline";
            js["samples"] = ss.samples;
            js["first_time"] = ss.first_time;
            js["last_time"] = ss.last_time;
            js["rate_hz"] = ss.rate_hz;
            js["segments"] = ss.segments;
            js["raw_bytes"] = ss.raw_bytes;
            js["stored_bytes"] = ss.stored_bytes;
            Json fields = Json::array();
            for (const auto& f : ss.fields) {
                fields.push_back({{"name", f.name},
                                  {"dtype", std::string(to_string(f.dtype))},
                                  {"shape", f.sample_shape},
                                  {"units", f.units}});
            }
            js["fields"] = fields;
            series.push_back(js);
        }
        jn["series"] = series;
        nodes.push_back(jn);
    }
    j["nodes"] = nodes;
    return j.dump(2);
}

// ---------------------------------------------------------------------------
// validate

namespace {

class Validator {
public:
    Validator(const fs::path& scan_path, const ValidateOptions& options)
        : path_(scan_path), options_(options) {}

    ValidationReport run() {
        try {
            check();
        } catch (const Error& e) {
            error("", e.what());
        }
        return std::move(report_);
    }

private:
    void add(Severity s, std::string path, std::string message) {
        report_.issues.push_back({s, std::move(path), std::move(message)});
    }
    void error(std::string path, std::string message) {
        add(Severity::Error, std::move(path), std::move(message));
    }
    void warning(std::string path, std::string message) {
        add(Severity::Warning, std::move(path), std::move(message));
    }
    void info(std::string path, std::string message) {
        add(Severity::Info, std::move(path), std::move(message));
    }

    void check() {
        const fs::path master_path = master_of(path_);
        if (!fs::exists(master_path)) {
            error("", "no master file " + detail::utf8(master_path));
            return;
        }
        const ScanReader scan = ScanReader::open(path_);
        dir_ = scan.directory();
        master_ = h5::open_file(detail::utf8(master_path), H5F_ACC_RDONLY);

        const auto attrs = scan.attributes();
        const auto version = scan.format_version();
        if (version.rfind("1.", 0) != 0) error("", "unsupported format_version '" + version + "'");
        if (!attrs.contains("uuid")) warning("", "no uuid");
        if (!scan.complete()) {
            warning("", "scan is not complete (still recording, or crashed: run rebuild)");
        }

        try {
            frames_ = std::make_unique<FrameGraph>(FrameGraph::load(path_));
        } catch (const Error& e) {
            error("frames", e.what());
        }

        std::set<std::string> nodes;
        for (const auto& p : scan.node_paths()) {
            nodes.insert(p);
            check_node(scan, scan.node(p));
        }
        // Segments on disk that the master does not know.
        std::error_code ec;
        for (const auto& cat : fs::directory_iterator(dir_, ec)) {
            if (!cat.is_directory()) continue;
            for (const auto& node : fs::directory_iterator(cat.path(), ec)) {
                if (!node.is_directory()) continue;
                const std::string p =
                    cat.path().filename().string() + "/" + node.path().filename().string();
                if (nodes.contains(p)) continue;
                for (const auto& series : fs::directory_iterator(node.path(), ec)) {
                    if (series.is_directory() && !detail::list_segments(series.path()).empty()) {
                        warning(p, "has segments on disk but is not in the master (run rebuild)");
                        break;
                    }
                }
            }
        }
    }

    void check_node(const ScanReader& scan, const NodeReader& node) {
        const std::string& path = node.path();
        const Schema& schema = node.schema();
        if (const Schema* reg = SchemaRegistry::global().find(schema.id())) {
            if (reg->to_json() != schema.to_json()) {
                warning(path, "stored schema " + schema.id() + " differs from the registered one");
            }
        } else {
            info(path, "schema " + schema.id() + " is not registered here (stored copy used)");
        }
        if (schema.category != node.category()) {
            error(path, "schema category '" + schema.category + "' but stored under '" +
                            node.category() + "'");
        }
        check_attributes(node);

        // Series in the file or on disk that the schema does not declare.
        h5::Id group = h5::open_group(master_.get(), path);
        for (const auto& child : h5::children(group.get())) {
            if (h5::is_group(group.get(), child) && !schema.find_series(child)) {
                warning(path + "/" + child, "series is not declared by " + schema.id());
            }
        }
        for (const auto& s : schema.series) {
            if (node.has_series(s.name)) check_series(node, s);
        }

        if (frames_) {
            const auto frame = node.attribute("frame_id");
            if (frame && std::holds_alternative<std::string>(*frame) &&
                !frames_->has_frame(std::get<std::string>(*frame))) {
                info(path, "frame '" + std::get<std::string>(*frame) + "' is not under frames/");
            }
        }
        if (schema.name == "raybotic.transform") check_transform(node);
        if (schema.name == "raybotic.ct_acquisition") check_acquisition(scan, node);
    }

    void check_attributes(const NodeReader& node) {
        const auto attrs = node.attributes();
        for (const auto& spec : node.schema().attributes) {
            auto it = attrs.find(spec.name);
            if (it == attrs.end()) {
                if (spec.required) error(node.path(), "required attribute '" + spec.name + "' is missing");
                continue;
            }
            const Value& v = it->second;
            const bool is_array = std::holds_alternative<std::vector<std::int64_t>>(v) ||
                                  std::holds_alternative<std::vector<double>>(v) ||
                                  std::holds_alternative<std::vector<std::string>>(v);
            bool ok = true;
            switch (spec.dtype) {
                case DType::String:
                    ok = std::holds_alternative<std::string>(v) ||
                         std::holds_alternative<std::vector<std::string>>(v);
                    break;
                case DType::Bool:
                    ok = std::holds_alternative<bool>(v) ||
                         std::holds_alternative<std::vector<std::int64_t>>(v);
                    break;
                case DType::Float32:
                case DType::Float64:
                    ok = std::holds_alternative<double>(v) ||
                         std::holds_alternative<std::vector<double>>(v);
                    break;
                default:
                    ok = std::holds_alternative<std::int64_t>(v) ||
                         std::holds_alternative<std::vector<std::int64_t>>(v);
                    break;
            }
            if (!ok || is_array != spec.array) {
                error(node.path(), "attribute '" + spec.name + "' should be " +
                                       std::string(to_string(spec.dtype)) +
                                       (spec.array ? "[]" : "") + ", is " + to_string(v));
            }
        }
    }

    void check_series(const NodeReader& node, const SeriesSpec& spec) {
        const std::string where = node.path() + "/" + spec.name;
        SeriesReader s = node.series(spec.name);
        const auto fields = s.fields();
        const auto attrs = node.attributes();
        std::map<std::string, std::uint64_t> named;
        for (const auto& f : spec.fields) {
            if (std::find(fields.begin(), fields.end(), f.name) == fields.end()) {
                if (!f.optional) error(where, "field '" + f.name + "' is missing");
                continue;
            }
            if (!f.allows(s.dtype(f.name))) {
                error(where + "/" + f.name, "dtype " + std::string(to_string(s.dtype(f.name))) +
                                                " is not allowed by the schema");
            }
            const Shape shape = s.sample_shape(f.name);
            if (shape.size() != f.shape.size()) {
                error(where + "/" + f.name, "sample shape " + shape_text(shape) + " has rank " +
                                                std::to_string(shape.size()) + ", schema rank " +
                                                std::to_string(f.shape.size()));
                continue;
            }
            for (std::size_t i = 0; i < shape.size(); ++i) {
                const DimSpec& d = f.shape[i];
                if (d.fixed()) {
                    if (shape[i] != d.size) {
                        error(where + "/" + f.name, "dimension " + std::to_string(i) + " is " +
                                                        std::to_string(shape[i]) + ", schema says " +
                                                        std::to_string(d.size));
                    }
                    continue;
                }
                if (auto [it, fresh] = named.emplace(d.name, shape[i]); !fresh && it->second != shape[i]) {
                    error(where, "dimension '" + d.name + "' is " + std::to_string(it->second) +
                                     " in one field and " + std::to_string(shape[i]) + " in '" +
                                     f.name + "'");
                }
                if (auto a = attrs.find(d.name); a != attrs.end()) {
                    std::optional<std::uint64_t> expected;
                    if (const auto* iv = std::get_if<std::int64_t>(&a->second)) {
                        expected = static_cast<std::uint64_t>(*iv);
                    } else if (const auto* sv = std::get_if<std::vector<std::string>>(&a->second)) {
                        expected = sv->size();
                    } else if (const auto* dv = std::get_if<std::vector<double>>(&a->second)) {
                        expected = dv->size();
                    }
                    if (expected && *expected != shape[i]) {
                        warning(where + "/" + f.name,
                                "dimension '" + d.name + "' is " + std::to_string(shape[i]) +
                                    " but the node attribute says " + std::to_string(*expected));
                    }
                }
            }
        }

        if (spec.storage == Storage::Inline) {
            check_lengths_inline(where);
        } else {
            check_segments(node, spec, where);
        }

        const auto n = s.size();
        if (n == 0) {
            info(where, "is empty");
            return;
        }
        const auto time = s.time();
        for (std::size_t i = 1; i < time.size(); ++i) {
            if (time[i] < time[i - 1]) {
                error(where, "time decreases at sample " + std::to_string(i));
                break;
            }
        }
        if (!options_.check_values) return;
        for (const auto& f : spec.fields) {
            if (f.optional || (f.dtype != DType::Float32 && f.dtype != DType::Float64)) continue;
            if (std::find(fields.begin(), fields.end(), f.name) == fields.end()) continue;
            const auto per_sample = element_count(s.sample_shape(f.name));
            if (per_sample * n * 8 > (64u << 20) || per_sample > 4096) continue;  // never images
            const auto values = doubles(s, f.name);
            const auto nans = std::count_if(values.begin(), values.end(),
                                            [](double x) { return std::isnan(x); });
            if (nans > 0) {
                warning(where + "/" + f.name, std::to_string(nans) + " NaN values in a required field");
            }
        }
    }

    void check_lengths_inline(const std::string& where) {
        h5::Lock lock(h5::mutex());
        h5::Id group = h5::open_group(master_.get(), where);
        std::set<std::uint64_t> lengths;
        for (const auto& name : h5::children(group.get())) {
            if (h5::is_group(group.get(), name)) continue;
            h5::Id d = h5::open_dataset(group.get(), name);
            lengths.insert(h5::extent(d.get()).at(0));
        }
        if (lengths.size() > 1) {
            error(where, "fields have different lengths (" + std::to_string(*lengths.begin()) +
                             " .. " + std::to_string(*lengths.rbegin()) + ")");
        }
    }

    void check_segments(const NodeReader& node, const SeriesSpec& spec, const std::string& where) {
        const auto files = detail::list_segments(dir_ / fs::path(node.path()) / spec.name);
        std::uint64_t total = 0;
        std::uint64_t segment_size = 0;
        for (std::size_t k = 0; k < files.size(); ++k) {
            const std::string name = files[k].filename().string();
            const auto index = std::stoull(name.substr(4, 6));
            if (index != k) {
                error(where, "segment " + name + " out of sequence (expected index " +
                                 std::to_string(k) + ")");
                return;
            }
            h5::Lock lock(h5::mutex());
            h5::Id f;
            try {
                f = h5::open_file(detail::utf8(files[k]), H5F_ACC_RDONLY, true);
            } catch (const Error&) {
                warning(where, name + " cannot be opened (still being written?)");
                return;
            }
            const auto schema = h5::read_string_attribute(f.get(), "schema").value_or("");
            if (schema != node.schema().id()) {
                error(where, name + " has schema '" + schema + "', node has " + node.schema().id());
            }
            auto uint_attr = [&](const char* key) -> std::uint64_t {
                const auto v = h5::read_attribute(f.get(), key);
                return v && std::holds_alternative<std::int64_t>(*v)
                           ? static_cast<std::uint64_t>(std::get<std::int64_t>(*v))
                           : 0;
            };
            if (uint_attr("first_sample") != total) {
                error(where, name + ": first_sample " + std::to_string(uint_attr("first_sample")) +
                                 ", expected " + std::to_string(total));
            }
            segment_size = uint_attr("segment_size");
            std::set<std::uint64_t> lengths;
            for (const auto& child : h5::children(f.get())) {
                if (h5::is_group(f.get(), child)) continue;
                h5::Id d = h5::open_dataset(f.get(), child);
                lengths.insert(h5::extent(d.get()).at(0));
            }
            const std::uint64_t n = lengths.empty() ? 0 : *lengths.begin();
            if (lengths.size() > 1) {
                warning(where, name + ": fields have different lengths (incomplete flush?)");
            }
            if (k + 1 < files.size() && segment_size > 0 && n != segment_size) {
                warning(where, name + " holds " + std::to_string(n) + " samples, segment_size is " +
                                   std::to_string(segment_size));
            }
            total += n;
        }

        h5::Lock lock(h5::mutex());
        if (!h5::exists(master_.get(), where + "/time")) {
            warning(where, std::to_string(files.size()) +
                               " segments are not linked into the master (run rebuild)");
            return;
        }
        h5::Id t = h5::open_dataset(master_.get(), where + "/time");
        const auto linked = h5::extent(t.get()).at(0);
        if (linked != total) {
            error(where, "virtual datasets cover " + std::to_string(linked) + " of " +
                             std::to_string(total) + " samples (run rebuild)");
        }
    }

    void check_transform(const NodeReader& node) {
        const auto parent = node.attribute("parent");
        const auto child = node.attribute("child");
        if (parent && child && *parent == *child) error(node.path(), "parent equals child");
        if (!options_.check_values || !node.has_series("pose")) return;
        const auto r = doubles(node.series("pose"), "rotation");
        std::size_t bad = 0;
        for (std::size_t i = 0; i + 3 < r.size(); i += 4) {
            const double nrm = std::sqrt(r[i] * r[i] + r[i + 1] * r[i + 1] + r[i + 2] * r[i + 2] +
                                         r[i + 3] * r[i + 3]);
            bad += std::abs(nrm - 1.0) > 1e-3 ? 1 : 0;
        }
        if (bad > 0) {
            warning(node.path() + "/pose/rotation", std::to_string(bad) + " quaternions are not unit length");
        }
    }

    void check_acquisition(const ScanReader& scan, const NodeReader& node) {
        std::optional<SeriesReader> images;
        for (const char* key : {"source", "detector"}) {
            const auto v = node.attribute(key);
            if (!v || !std::holds_alternative<std::string>(*v)) continue;
            const auto& target = std::get<std::string>(*v);
            if (!scan.has_node(target)) {
                error(node.path(), std::string(key) + " '" + target + "' does not exist");
                continue;
            }
            const auto expected = std::string("raybotic.xray_") + key;
            const NodeReader ref = scan.node(target);
            if (ref.schema().name != expected) {
                warning(node.path(), std::string(key) + " '" + target + "' is a " +
                                         ref.schema().id() + ", not " + expected);
            }
            if (std::string(key) == "detector" && ref.has_series("image")) images = ref.series("image");
        }
        if (!options_.check_values || !node.has_series("projections")) return;
        const SeriesReader p = node.series("projections");
        if (p.size() == 0) return;
        const std::string where = node.path() + "/projections";

        const auto index = doubles(p, "image_index");
        const auto ptime = p.time();
        if (images) {
            const auto itime = images->time();
            std::size_t out_of_range = 0;
            std::size_t off_time = 0;
            const auto tol = static_cast<std::int64_t>(options_.time_tolerance_s * 1e9);
            for (std::size_t i = 0; i < index.size(); ++i) {
                const auto k = static_cast<std::uint64_t>(index[i]);
                if (k >= itime.size()) {
                    ++out_of_range;
                } else if (std::llabs(itime[k] - ptime[i]) > tol) {
                    ++off_time;
                }
            }
            if (out_of_range > 0) {
                error(where, std::to_string(out_of_range) + " image_index values beyond the " +
                                 std::to_string(itime.size()) + " detector images");
            }
            if (off_time > 0) {
                warning(where, std::to_string(off_time) +
                                   " projections differ in time from their image by more than " +
                                   std::to_string(options_.time_tolerance_s) + " s");
            }
        }
        const auto u = doubles(p, "detector_u");
        const auto v = doubles(p, "detector_v");
        std::size_t bad = 0;
        for (std::size_t i = 0; i + 2 < u.size() && i + 2 < v.size(); i += 3) {
            const double nu = std::sqrt(u[i] * u[i] + u[i + 1] * u[i + 1] + u[i + 2] * u[i + 2]);
            const double nv = std::sqrt(v[i] * v[i] + v[i + 1] * v[i + 1] + v[i + 2] * v[i + 2]);
            const double d = u[i] * v[i] + u[i + 1] * v[i + 1] + u[i + 2] * v[i + 2];
            bad += (std::abs(nu - 1) > 1e-3 || std::abs(nv - 1) > 1e-3 || std::abs(d) > 1e-3) ? 1 : 0;
        }
        if (bad > 0) {
            warning(where, std::to_string(bad) + " rows with detector_u/v not unit or not orthogonal");
        }
        const auto flags = doubles(p, "flags");
        const auto invalid = std::count_if(flags.begin(), flags.end(), [](double f) {
            return (static_cast<std::uint32_t>(f) & 1u) != 0;
        });
        if (invalid > 0) warning(where, std::to_string(invalid) + " projections flagged invalid");
    }

    static std::vector<double> doubles(const SeriesReader& s, std::string_view field) {
        const Array a = s.read(field);
        std::vector<double> out(static_cast<std::size_t>(a.size()));
        detail::convert(a.dtype(), a.data(), DType::Float64, out.data(), out.size());
        return out;
    }

    fs::path path_;
    ValidateOptions options_;
    ValidationReport report_;
    fs::path dir_;
    h5::Id master_;
    std::unique_ptr<FrameGraph> frames_;
};

const char* severity_name(Severity s) {
    switch (s) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Info: return "info";
    }
    return "?";
}

}  // namespace

ValidationReport validate(const fs::path& scan, const ValidateOptions& options) {
    h5::init();
    return Validator(scan, options).run();
}

std::string to_text(const ValidationReport& r) {
    std::ostringstream out;
    for (const auto& i : r.issues) {
        out << severity_name(i.severity) << ": " << (i.path.empty() ? "<scan>" : i.path) << ": "
            << i.message << "\n";
    }
    out << r.count(Severity::Error) << " errors, " << r.count(Severity::Warning) << " warnings, "
        << r.count(Severity::Info) << " notes -> " << (r.ok() ? "OK" : "FAILED") << "\n";
    return out.str();
}

std::string to_json(const ValidationReport& r) {
    Json j;
    j["ok"] = r.ok();
    j["errors"] = r.count(Severity::Error);
    j["warnings"] = r.count(Severity::Warning);
    Json issues = Json::array();
    for (const auto& i : r.issues) {
        issues.push_back(
            {{"severity", severity_name(i.severity)}, {"path", i.path}, {"message", i.message}});
    }
    j["issues"] = issues;
    return j.dump(2);
}

}  // namespace raybotic::scan
