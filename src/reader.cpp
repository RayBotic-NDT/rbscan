#include "raybotic/scan/reader.hpp"

#include <algorithm>
#include <cstring>

#include "internal.hpp"
#include "raybotic/scan/writer.hpp"
#include "series_layout.hpp"

namespace raybotic::scan {

namespace fs = std::filesystem;

namespace {

struct FieldInfo {
    std::string name;
    DType dtype{DType::Float64};
    Shape sample_shape;
    std::string units;

    [[nodiscard]] std::size_t sample_bytes() const {
        return static_cast<std::size_t>(element_count(sample_shape)) * dtype_size(dtype);
    }
};

/// Reads rows [start, start + count) of a dataset into `out`.
void read_rows(hid_t dataset, const FieldInfo& field, std::uint64_t start, std::uint64_t count,
               void* out) {
    if (count == 0) return;
    h5::Lock lock(h5::mutex());
    const auto rank = field.sample_shape.size() + 1;
    std::vector<hsize_t> offset(rank, 0);
    std::vector<hsize_t> block(rank);
    offset[0] = start;
    block[0] = count;
    for (std::size_t i = 0; i < field.sample_shape.size(); ++i) block[i + 1] = field.sample_shape[i];
    h5::Id fspace(h5::check_id(H5Dget_space(dataset), "H5Dget_space"));
    h5::check(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, offset.data(), nullptr,
                                  block.data(), nullptr),
              "H5Sselect_hyperslab");
    h5::Id mspace(h5::check_id(H5Screate_simple(static_cast<int>(rank), block.data(), nullptr),
                               "H5Screate_simple"));
    h5::Id mtype = h5::memory_type(field.dtype);
    h5::check(H5Dread(dataset, mtype.get(), mspace.get(), fspace.get(), H5P_DEFAULT, out),
              "reading " + field.name);
}

FieldInfo describe(hid_t dataset, const std::string& name, const FieldSpec* spec) {
    h5::Lock lock(h5::mutex());
    h5::Id type(h5::check_id(H5Dget_type(dataset), "H5Dget_type"));
    FieldInfo info;
    info.name = name;
    info.dtype = h5::dtype_of(type.get());
    const Shape e = h5::extent(dataset);
    info.sample_shape.assign(e.begin() + 1, e.end());
    if (spec != nullptr) {
        info.units = spec->units;
    } else if (auto u = h5::read_string_attribute(dataset, "units")) {
        info.units = *u;
    }
    return info;
}

}  // namespace

// ---------------------------------------------------------------------------
// SeriesReader

struct SeriesReader::Impl {
    SeriesSpec spec;
    std::vector<FieldInfo> fields;  // [0] = time
    std::vector<std::int64_t> time_cache;
    bool time_cached{false};

    virtual ~Impl() = default;
    virtual std::uint64_t size() const = 0;
    virtual void read(std::size_t field, std::uint64_t start, std::uint64_t count,
                      std::byte* out) const = 0;
    virtual void refresh() {}

    std::size_t index(std::string_view name) const {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].name == name) return i;
        }
        throw Error("series '" + spec.name + "' has no field '" + std::string(name) + "'");
    }

    void clamp(std::uint64_t start, std::uint64_t& count) const {
        const auto n = size();
        if (start > n) {
            throw Error("series '" + spec.name + "': start " + std::to_string(start) +
                        " is beyond its " + std::to_string(n) + " samples");
        }
        count = std::min(count, n - start);
    }
};

namespace {

/// A series in the master file (inline, or virtual datasets).
struct MasterSeries final : SeriesReader::Impl {
    std::vector<h5::Id> datasets;

    std::uint64_t size() const override {
        h5::Lock lock(h5::mutex());
        std::uint64_t n = std::numeric_limits<std::uint64_t>::max();
        for (const auto& d : datasets) n = std::min(n, h5::extent(d.get())[0]);
        return datasets.empty() ? 0 : n;
    }

    void read(std::size_t field, std::uint64_t start, std::uint64_t count,
              std::byte* out) const override {
        read_rows(datasets[field].get(), fields[field], start, count, out);
    }
};

/// A segmented series, read from its segment files. Files are opened SWMR so
/// a recording in progress can be followed with refresh().
struct SegmentSeries final : SeriesReader::Impl {
    struct Segment {
        fs::path path;
        h5::Id file;
        std::vector<h5::Id> datasets;
        std::uint64_t first{0};
        std::uint64_t samples{0};
    };
    fs::path dir;
    std::vector<Segment> segments;

    std::uint64_t size() const override {
        return segments.empty() ? 0 : segments.back().first + segments.back().samples;
    }

    static std::uint64_t samples_of(const Segment& s) {
        std::uint64_t n = std::numeric_limits<std::uint64_t>::max();
        for (const auto& d : s.datasets) n = std::min(n, h5::extent(d.get())[0]);
        return s.datasets.empty() ? 0 : n;
    }

    void refresh() override {
        h5::Lock lock(h5::mutex());
        if (!segments.empty()) {
            auto& last = segments.back();
            for (auto& d : last.datasets) H5Drefresh(d.get());
            H5Eclear2(H5E_DEFAULT);
            last.samples = samples_of(last);
        }
        const auto files = detail::list_segments(dir);
        for (std::size_t i = segments.size(); i < files.size(); ++i) {
            Segment s;
            s.path = files[i];
            try {
                s.file = h5::open_file(detail::utf8(s.path), H5F_ACC_RDONLY, true);
                if (fields.empty()) {
                    // First segment: take the fields from the file.
                    const auto names = h5::children(s.file.get());
                    std::vector<std::string> order = {detail::time_field};
                    for (const auto& f : spec.fields) {
                        if (std::find(names.begin(), names.end(), f.name) != names.end()) {
                            order.push_back(f.name);
                        }
                    }
                    for (const auto& name : order) {
                        h5::Id d = h5::open_dataset(s.file.get(), name);
                        fields.push_back(describe(d.get(), name, spec.field(name)));
                        if (name == detail::time_field) fields.back().units = "ns";
                    }
                }
                for (const auto& f : fields) {
                    s.datasets.push_back(h5::open_dataset(s.file.get(), f.name));
                }
            } catch (const Error&) {
                break;  // being created right now; picked up by the next refresh
            }
            s.first = size();
            s.samples = samples_of(s);
            segments.push_back(std::move(s));
        }
        time_cached = false;
    }

    void read(std::size_t field, std::uint64_t start, std::uint64_t count,
              std::byte* out) const override {
        const auto bytes = fields[field].sample_bytes();
        for (const auto& s : segments) {
            if (count == 0) break;
            const auto end = s.first + s.samples;
            if (start >= end) continue;
            const auto local = start - s.first;
            const auto n = std::min(count, end - start);
            read_rows(s.datasets[field].get(), fields[field], local, n, out);
            out += n * bytes;
            start += n;
            count -= n;
        }
        if (count != 0) throw Error("series '" + spec.name + "': samples missing in segments");
    }
};

}  // namespace

SeriesReader::SeriesReader(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

SeriesReader SeriesReader::open_segments(const fs::path& scan_dir, std::string_view node_path,
                                         std::string_view series) {
    h5::init();
    const fs::path dir = scan_dir / fs::path(std::string(node_path)) / std::string(series);
    const auto files = detail::list_segments(dir);
    if (files.empty()) throw Error("no segments in " + detail::utf8(dir));
    std::string json;
    {
        h5::Lock lock(h5::mutex());
        h5::Id f = h5::open_file(detail::utf8(files.front()), H5F_ACC_RDONLY, true);
        json = h5::read_string_attribute(f.get(), "schema_json").value_or("");
    }
    if (json.empty()) throw Error(detail::utf8(files.front()) + " has no schema");
    auto s = std::make_shared<SegmentSeries>();
    s->spec = parse_schema(json).get_series(series);
    s->dir = dir;
    s->refresh();
    return SeriesReader(s);
}

std::string SeriesReader::name() const { return impl_->spec.name; }
Storage SeriesReader::storage() const { return impl_->spec.storage; }
std::uint64_t SeriesReader::size() const { return impl_->size(); }

std::vector<std::string> SeriesReader::fields() const {
    std::vector<std::string> out;
    for (std::size_t i = 1; i < impl_->fields.size(); ++i) out.push_back(impl_->fields[i].name);
    return out;
}

DType SeriesReader::dtype(std::string_view field) const {
    return impl_->fields[impl_->index(field)].dtype;
}

Shape SeriesReader::sample_shape(std::string_view field) const {
    return impl_->fields[impl_->index(field)].sample_shape;
}

std::string SeriesReader::units(std::string_view field) const {
    return impl_->fields[impl_->index(field)].units;
}

std::vector<std::int64_t> SeriesReader::time(std::uint64_t start, std::uint64_t count) const {
    impl_->clamp(start, count);
    std::vector<std::int64_t> out(static_cast<std::size_t>(count));
    impl_->read(0, start, count, reinterpret_cast<std::byte*>(out.data()));
    return out;
}

Array SeriesReader::read(std::string_view field, std::uint64_t start, std::uint64_t count) const {
    const auto i = impl_->index(field);
    impl_->clamp(start, count);
    const auto& f = impl_->fields[i];
    Shape shape{count};
    shape.insert(shape.end(), f.sample_shape.begin(), f.sample_shape.end());
    Array out(f.dtype, std::move(shape));
    impl_->read(i, start, count, static_cast<std::byte*>(out.data()));
    return out;
}

Array SeriesReader::read_sample(std::string_view field, std::uint64_t index) const {
    const auto i = impl_->index(field);
    if (index >= impl_->size()) {
        throw Error("series '" + impl_->spec.name + "': sample " + std::to_string(index) +
                    " out of range");
    }
    const auto& f = impl_->fields[i];
    Array out(f.dtype, f.sample_shape);
    impl_->read(i, index, 1, static_cast<std::byte*>(out.data()));
    return out;
}

std::optional<std::uint64_t> SeriesReader::index_at(std::int64_t time_ns) const {
    if (!impl_->time_cached || impl_->time_cache.size() != impl_->size()) {
        impl_->time_cache = time();
        impl_->time_cached = true;
    }
    const auto& t = impl_->time_cache;
    auto it = std::upper_bound(t.begin(), t.end(), time_ns);
    if (it == t.begin()) return std::nullopt;
    return static_cast<std::uint64_t>(std::distance(t.begin(), it) - 1);
}

void SeriesReader::refresh() { impl_->refresh(); }

// ---------------------------------------------------------------------------
// NodeReader / ScanReader

struct ScanReader::Impl {
    fs::path dir;
    h5::Id file;
};

struct NodeReader::Impl {
    std::shared_ptr<ScanReader::Impl> scan;
    std::string path;
    Schema schema;
    h5::Id group;

    [[nodiscard]] fs::path series_dir(const std::string& series) const {
        return scan->dir / fs::path(path) / series;
    }
    [[nodiscard]] bool in_master(const std::string& series) const {
        return h5::exists(group.get(), series) && h5::is_group(group.get(), series);
    }
};

NodeReader::NodeReader(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::string NodeReader::path() const { return impl_->path; }
std::string NodeReader::name() const { return impl_->path.substr(impl_->path.find('/') + 1); }
std::string NodeReader::category() const { return impl_->path.substr(0, impl_->path.find('/')); }
const Schema& NodeReader::schema() const { return impl_->schema; }

Attributes NodeReader::attributes() const {
    auto attrs = h5::read_attributes(impl_->group.get());
    attrs.erase("schema");
    attrs.erase("schema_json");
    return attrs;
}

std::optional<Value> NodeReader::attribute(std::string_view name) const {
    return h5::read_attribute(impl_->group.get(), std::string(name));
}

bool NodeReader::has_series(std::string_view name) const {
    const auto* spec = impl_->schema.find_series(name);
    if (spec == nullptr) return false;
    if (spec->storage == Storage::Segmented) {
        return !detail::list_segments(impl_->series_dir(spec->name)).empty();
    }
    return impl_->in_master(spec->name);
}

std::vector<std::string> NodeReader::series_names() const {
    std::vector<std::string> out;
    for (const auto& s : impl_->schema.series) {
        if (has_series(s.name)) out.push_back(s.name);
    }
    return out;
}

SeriesReader NodeReader::series(std::string_view name) const {
    const SeriesSpec& spec = impl_->schema.get_series(name);
    if (!has_series(name)) {
        throw Error("node '" + impl_->path + "' has no data for series '" + spec.name + "'");
    }
    if (spec.storage == Storage::Segmented) {
        auto s = std::make_shared<SegmentSeries>();
        s->spec = spec;
        s->dir = impl_->series_dir(spec.name);
        s->refresh();
        return SeriesReader(s);
    }
    h5::Lock lock(h5::mutex());
    auto s = std::make_shared<MasterSeries>();
    s->spec = spec;
    h5::Id group = h5::open_group(impl_->group.get(), spec.name);
    std::vector<std::string> order = {detail::time_field};
    for (const auto& f : spec.fields) {
        if (h5::exists(group.get(), f.name)) order.push_back(f.name);
    }
    for (const auto& field : order) {
        s->datasets.push_back(h5::open_dataset(group.get(), field));
        s->fields.push_back(describe(s->datasets.back().get(), field, spec.field(field)));
        if (field == detail::time_field) s->fields.back().units = "ns";
    }
    return SeriesReader(s);
}

ScanReader::ScanReader(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

ScanReader ScanReader::open(const fs::path& path) {
    h5::init();
    const fs::path master = fs::is_directory(path) ? path / master_file_name : path;
    auto impl = std::make_shared<Impl>();
    impl->dir = master.parent_path();
    impl->file = h5::open_file(detail::utf8(master), H5F_ACC_RDONLY);
    const auto format = h5::read_string_attribute(impl->file.get(), "format");
    if (format != format_name) {
        throw Error(detail::utf8(master) + " is not a " + std::string(format_name) + " file");
    }
    return ScanReader(std::move(impl));
}

fs::path ScanReader::directory() const { return impl_->dir; }

std::string ScanReader::format_version() const {
    return h5::read_string_attribute(impl_->file.get(), "format_version").value_or("");
}

bool ScanReader::complete() const {
    const auto v = h5::read_attribute(impl_->file.get(), "complete");
    return v && std::holds_alternative<bool>(*v) && std::get<bool>(*v);
}

Attributes ScanReader::attributes() const { return h5::read_attributes(impl_->file.get()); }

std::vector<std::string> ScanReader::categories() const {
    std::vector<std::string> out;
    for (const auto& name : h5::children(impl_->file.get())) {
        if (h5::is_group(impl_->file.get(), name)) out.push_back(name);
    }
    return out;
}

std::vector<std::string> ScanReader::node_paths(std::string_view category) const {
    h5::Lock lock(h5::mutex());
    std::vector<std::string> out;
    for (const auto& cat : categories()) {
        if (!category.empty() && cat != category) continue;
        h5::Id group = h5::open_group(impl_->file.get(), cat);
        for (const auto& name : h5::children(group.get())) {
            if (!h5::is_group(group.get(), name)) continue;
            h5::Id node = h5::open_group(group.get(), name);
            if (h5::read_string_attribute(node.get(), "schema")) out.push_back(cat + "/" + name);
        }
    }
    return out;
}

bool ScanReader::has_node(std::string_view path) const {
    return h5::exists(impl_->file.get(), std::string(path)) &&
           h5::is_group(impl_->file.get(), std::string(path));
}

NodeReader ScanReader::node(std::string_view path) const {
    if (!has_node(path)) throw Error("no node '" + std::string(path) + "'");
    h5::Lock lock(h5::mutex());
    auto impl = std::make_shared<NodeReader::Impl>();
    impl->scan = impl_;
    impl->path = std::string(path);
    impl->group = h5::open_group(impl_->file.get(), impl->path);
    const auto json = h5::read_string_attribute(impl->group.get(), "schema_json");
    if (!json) throw Error("'" + impl->path + "' is not a node (no schema)");
    impl->schema = parse_schema(*json);
    return NodeReader(std::move(impl));
}

}  // namespace raybotic::scan
