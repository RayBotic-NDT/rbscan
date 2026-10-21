#include "raybotic/scan/reader.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <future>
#include <thread>

#include "compression.hpp"
#include "internal.hpp"
#include "thread_pool.hpp"
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

// --- Reading chunks past HDF5's filter pipeline --------------------------------
//
// HDF5 decompresses inside H5Dread, i.e. under the library-wide lock, so
// threads reading images queue up behind each other. For datasets with one
// sample per chunk and filters rbscan understands (shuffle, deflate) the
// reader instead fetches the stored chunk under the lock (H5Dread_chunk, a
// plain file read) and decodes it outside: in the calling thread for single
// samples, in a thread pool for ranges.

/// Whether and how a dataset's chunks can be decoded by rbscan itself.
struct DirectChunks {
    bool usable{false};
    /// Pipeline positions of the filters (bit i of the chunk's filter mask
    /// says filter i was skipped); -1 when absent.
    int shuffle{-1};
    int deflate{-1};
};

DirectChunks inspect_chunks(hid_t dataset, const FieldInfo& field) {
    DirectChunks dc;
    if constexpr (std::endian::native != std::endian::little) return dc;
    h5::Lock lock(h5::mutex());
    h5::Id dcpl(H5Dget_create_plist(dataset));
    if (!dcpl || H5Pget_layout(dcpl.get()) != H5D_CHUNKED) {
        H5Eclear2(H5E_DEFAULT);
        return dc;
    }
    // One whole sample per chunk.
    const int rank = static_cast<int>(field.sample_shape.size()) + 1;
    std::vector<hsize_t> chunk(static_cast<std::size_t>(rank));
    if (H5Pget_chunk(dcpl.get(), rank, chunk.data()) != rank || chunk[0] != 1) return dc;
    for (std::size_t i = 0; i < field.sample_shape.size(); ++i) {
        if (chunk[i + 1] != field.sample_shape[i]) return dc;
    }
    // Stored exactly as the memory type: little-endian, same width.
    h5::Id type(H5Dget_type(dataset));
    const std::size_t size = H5Tget_size(type.get());
    if (size != dtype_size(field.dtype)) return dc;
    if (size > 1 && H5Tget_order(type.get()) != H5T_ORDER_LE) return dc;

    const int filters = H5Pget_nfilters(dcpl.get());
    for (int i = 0; i < filters; ++i) {
        unsigned flags = 0;
        std::size_t n_values = 0;
        unsigned config = 0;
        const H5Z_filter_t id = H5Pget_filter2(dcpl.get(), static_cast<unsigned>(i), &flags,
                                               &n_values, nullptr, 0, nullptr, &config);
        if (id == H5Z_FILTER_SHUFFLE) {
            dc.shuffle = i;
        } else if (id == H5Z_FILTER_DEFLATE) {
            dc.deflate = i;
        } else {
            H5Eclear2(H5E_DEFAULT);
            return dc;  // fletcher32, szip, plugins: leave them to HDF5
        }
    }
    // Written shuffle first, then deflate; undone in reverse.
    if (dc.shuffle >= 0 && dc.deflate >= 0 && dc.shuffle > dc.deflate) return dc;
    dc.usable = true;
    return dc;
}

struct RawChunk {
    std::vector<std::byte> data;
    std::uint32_t mask{0};
};

/// The stored bytes of sample `index`'s chunk. False when the chunk is not
/// allocated (then HDF5 has to supply the fill value).
bool fetch_chunk(hid_t dataset, const FieldInfo& field, std::uint64_t index, RawChunk& raw) {
    h5::Lock lock(h5::mutex());
    std::vector<hsize_t> offset(field.sample_shape.size() + 1, 0);
    offset[0] = index;
    hsize_t size = 0;
    if (H5Dget_chunk_storage_size(dataset, offset.data(), &size) < 0 || size == 0) {
        H5Eclear2(H5E_DEFAULT);
        return false;
    }
    raw.data.resize(static_cast<std::size_t>(size));
    h5::check(H5Dread_chunk(dataset, H5P_DEFAULT, offset.data(), &raw.mask, raw.data.data()),
              "H5Dread_chunk " + field.name);
    return true;
}

void decode(const DirectChunks& dc, const FieldInfo& field, const RawChunk& raw,
            std::byte* out) {
    const auto applied = [&](int position) {
        return position >= 0 && (raw.mask & (1u << static_cast<unsigned>(position))) == 0;
    };
    detail::decode_chunk(raw.data.data(), raw.data.size(), out, field.sample_bytes(),
                         dtype_size(field.dtype), applied(dc.deflate), applied(dc.shuffle));
}

detail::ThreadPool& decode_pool() {
    static detail::ThreadPool pool(std::max(1u, std::thread::hardware_concurrency() / 2));
    return pool;
}

/// Rows [start, start + count) of one dataset, decoded by rbscan where it can.
void read_samples(hid_t dataset, const DirectChunks& dc, const FieldInfo& field,
                  std::uint64_t start, std::uint64_t count, std::byte* out);

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

void read_samples(hid_t dataset, const DirectChunks& dc, const FieldInfo& field,
                  std::uint64_t start, std::uint64_t count, std::byte* out) {
    if (!dc.usable) {
        read_rows(dataset, field, start, count, out);
        return;
    }
    const std::size_t bytes = field.sample_bytes();
    if (count == 1) {
        RawChunk raw;
        if (fetch_chunk(dataset, field, start, raw)) {
            decode(dc, field, raw, out);
        } else {
            read_rows(dataset, field, start, 1, out);
        }
        return;
    }
    // Fetch one chunk after the other (file reads under the lock), decode them
    // in parallel as they arrive.
    std::vector<std::future<void>> jobs;
    std::exception_ptr failed;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::byte* target = out + i * bytes;
        auto raw = std::make_shared<RawChunk>();
        try {
            if (!fetch_chunk(dataset, field, start + i, *raw)) {
                read_rows(dataset, field, start + i, 1, target);
                continue;
            }
        } catch (...) {
            failed = std::current_exception();
            break;
        }
        jobs.push_back(decode_pool().submit(
            [&dc, &field, raw, target] { decode(dc, field, *raw, target); }));
    }
    for (auto& j : jobs) {
        try {
            j.get();
        } catch (...) {
            if (!failed) failed = std::current_exception();
        }
    }
    if (failed) std::rethrow_exception(failed);
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
    std::vector<DirectChunks> direct;

    std::uint64_t size() const override {
        h5::Lock lock(h5::mutex());
        std::uint64_t n = std::numeric_limits<std::uint64_t>::max();
        for (const auto& d : datasets) n = std::min(n, h5::extent(d.get())[0]);
        return datasets.empty() ? 0 : n;
    }

    void read(std::size_t field, std::uint64_t start, std::uint64_t count,
              std::byte* out) const override {
        read_samples(datasets[field].get(), direct[field], fields[field], start, count, out);
    }
};

/// A segmented series, read from its segment files. Files are opened SWMR so
/// a recording in progress can be followed with refresh().
struct SegmentSeries final : SeriesReader::Impl {
    struct Segment {
        fs::path path;
        h5::Id file;
        std::vector<h5::Id> datasets;
        std::vector<DirectChunks> direct;
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
                    s.direct.push_back(inspect_chunks(s.datasets.back().get(), f));
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
            read_samples(s.datasets[field].get(), s.direct[field], fields[field], local, n, out);
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
        s->direct.push_back(inspect_chunks(s->datasets.back().get(), s->fields.back()));
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
