#include "raybotic/scan/writer.hpp"

#include "compression.hpp"
#include "internal.hpp"
#include "raybotic/scan/rbscan.hpp"
#include "writer_impl.hpp"

namespace raybotic::scan {

namespace fs = std::filesystem;

namespace {

/// Rows [start, start + count) of `dataset` into `out`, in memory layout.
void read_rows(hid_t dataset, const detail::FieldLayout& field, std::uint64_t start,
               std::uint64_t count, std::byte* out) {
    const auto rank = field.sample_shape.size() + 1;
    std::vector<hsize_t> offset(rank, 0);
    std::vector<hsize_t> size(rank);
    offset[0] = start;
    size[0] = count;
    for (std::size_t i = 0; i < field.sample_shape.size(); ++i) {
        size[i + 1] = field.sample_shape[i];
    }
    h5::Id file_space(h5::check_id(H5Dget_space(dataset), "H5Dget_space"));
    h5::check(H5Sselect_hyperslab(file_space.get(), H5S_SELECT_SET, offset.data(),
                                  nullptr, size.data(), nullptr),
              "H5Sselect_hyperslab");
    h5::Id memory_space(
        h5::check_id(H5Screate_simple(static_cast<int>(rank), size.data(), nullptr),
                     "H5Screate_simple"));
    h5::Id type = h5::memory_type(field.dtype);
    h5::check(H5Dread(dataset, type.get(), memory_space.get(), file_space.get(),
                      H5P_DEFAULT, out),
              "reading " + field.name);
}

/// The writer of an inline series stored by an earlier session: the layout
/// comes from its datasets, the last, partly filled chunk back into the
/// buffers, so appending goes on where it ended.
std::shared_ptr<SeriesWriter::Impl> resume_series(
    const std::shared_ptr<ScanWriter::Impl>& scan, hid_t node_group,
    const SeriesSpec& spec) {
    h5::Lock lock(h5::mutex());
    auto s = std::make_shared<SeriesWriter::Impl>();
    s->scan = scan;
    s->group = h5::open_group(node_group, spec.name);
    s->layout.spec = spec;

    std::vector<std::pair<std::string, bool>> names{{detail::time_field, false}};
    for (const auto& f : spec.fields) {
        if (h5::exists(s->group.get(), f.name)) names.emplace_back(f.name, f.optional);
    }
    for (const auto& [name, optional] : names) {
        h5::Id d = h5::open_dataset(s->group.get(), name);
        detail::FieldLayout f;
        f.name = name;
        f.optional = optional;
        {
            h5::Id type(h5::check_id(H5Dget_type(d.get()), "H5Dget_type"));
            f.dtype = h5::dtype_of(type.get());
        }
        const Shape extent = h5::extent(d.get());
        f.sample_shape.assign(extent.begin() + 1, extent.end());
        std::uint64_t elements = 1;
        for (auto e : f.sample_shape) elements *= e;
        f.sample_bytes = static_cast<std::size_t>(elements) * dtype_size(f.dtype);
        h5::Id dcpl(h5::check_id(H5Dget_create_plist(d.get()), "H5Dget_create_plist"));
        std::vector<hsize_t> chunk(extent.size());
        if (H5Pget_chunk(dcpl.get(), static_cast<int>(chunk.size()), chunk.data()) < 1) {
            throw Error("series '" + spec.name + "': field '" + name +
                        "' is not chunked");
        }
        f.chunk_samples = chunk[0];
        if (name == detail::time_field) {
            s->compression = detail::filters_of(dcpl.get());
            s->total = extent[0];
        } else if (extent[0] != s->total) {
            throw Error("series '" + spec.name + "': field '" + name + "' has " +
                        std::to_string(extent[0]) + " samples, time has " +
                        std::to_string(s->total));
        }
        s->layout.fields.push_back(std::move(f));
        s->datasets.push_back(std::move(d));
    }

    s->buffers.resize(s->layout.fields.size());
    for (std::size_t i = 0; i < s->layout.fields.size(); ++i) {
        const auto& f = s->layout.fields[i];
        s->buffers[i].reset(new std::byte[f.chunk_bytes()]);
        const auto fill = s->total % f.chunk_samples;
        if (fill > 0) {
            read_rows(s->datasets[i].get(), f, s->total - fill, fill,
                      s->buffers[i].get());
        }
    }
    if (s->total > 0) {
        std::int64_t last = 0;
        read_rows(s->datasets[0].get(), s->layout.fields[0], s->total - 1, 1,
                  reinterpret_cast<std::byte*>(&last));
        s->last_time = last;
    }
    s->flushed_total = s->total;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// SeriesWriter (inline, in the master file)

void SeriesWriter::Impl::append(std::int64_t time_ns,
                                std::span<const FieldValue> fields) {
    if (closed) throw Error("series '" + layout.spec.name + "': the scan is finalized");
    if (time_ns < last_time) {
        throw Error("series '" + layout.spec.name + "': time must not decrease");
    }
    const auto n = layout.fields.size();
    std::vector<std::uint64_t> slots(n);
    for (std::size_t i = 0; i < n; ++i) slots[i] = total % layout.fields[i].chunk_samples;
    detail::stage_sample(layout, buffers, slots, time_ns, fields);
    last_time = time_ns;
    ++total;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& f = layout.fields[i];
        if (total % f.chunk_samples != 0) continue;
        const auto encoded = detail::encode_chunk(buffers[i].get(), f.chunk_bytes(),
                                                  dtype_size(f.dtype), compression);
        detail::write_chunk(datasets[i].get(), f, (total - 1) / f.chunk_samples, total,
                            encoded);
    }
}

void SeriesWriter::Impl::flush() {
    if (closed || flushed_total == total) return;
    for (std::size_t i = 0; i < layout.fields.size(); ++i) {
        const auto& f = layout.fields[i];
        const auto fill = total % f.chunk_samples;
        if (fill == 0) continue;
        detail::zero_tail(f, buffers[i].get(), fill);
        const auto encoded = detail::encode_chunk(buffers[i].get(), f.chunk_bytes(),
                                                  dtype_size(f.dtype), compression);
        detail::write_chunk(datasets[i].get(), f, total / f.chunk_samples, total,
                            encoded);
    }
    flushed_total = total;
}

void SeriesWriter::Impl::close() {
    if (closed) return;
    flush();
    h5::Lock lock(h5::mutex());
    datasets.clear();
    group.reset();
    buffers.clear();
    closed = true;
}

SeriesWriter::SeriesWriter(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

void SeriesWriter::append(std::int64_t time_ns, std::span<const FieldValue> fields) {
    impl_->append(time_ns, fields);
}
void SeriesWriter::flush() { impl_->flush(); }
std::uint64_t SeriesWriter::size() const { return impl_->total; }
const SeriesSpec& SeriesWriter::spec() const { return impl_->layout.spec; }
Shape SeriesWriter::sample_shape(std::string_view field) const {
    return impl_->layout.sample_shape(field);
}

// ---------------------------------------------------------------------------
// NodeWriter

std::shared_ptr<ScanWriter::Impl> NodeWriter::Impl::lock_scan() const {
    auto s = scan.lock();
    if (!s) throw Error("node '" + path + "': its ScanWriter is gone");
    s->require_open();
    return s;
}

NodeWriter::NodeWriter(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::string NodeWriter::path() const { return impl_->path; }
const Schema& NodeWriter::schema() const { return *impl_->schema; }

void NodeWriter::set_attribute(std::string_view name, const Value& value) {
    auto scan = impl_->lock_scan();
    const std::string key(name);
    if (key == "schema" || key == "schema_json") {
        throw Error("attribute name '" + key + "' is reserved");
    }
    h5::Lock lock(h5::mutex());
    detail::write_node_attribute(impl_->group.get(), *impl_->schema, key, value);
    impl_->attributes[key] = value;
}

SeriesWriter NodeWriter::series(std::string_view name, const Dims& dims,
                                const InlineOptions& options) {
    auto scan = impl_->lock_scan();
    if (auto it = impl_->inline_series.find(name); it != impl_->inline_series.end()) {
        return SeriesWriter(it->second);
    }
    const SeriesSpec& spec = impl_->schema->get_series(name);
    if (spec.storage != Storage::Inline) {
        throw Error("series '" + spec.name + "' of " + impl_->schema->id() +
                    " is segmented; use segmented_series()");
    }
    if (impl_->stored_schema) {
        // A node of an earlier session: its stored series goes on.
        bool stored = false;
        {
            h5::Lock lock(h5::mutex());
            stored = h5::exists(impl_->group.get(), spec.name);
        }
        if (!stored) {
            throw Error("node '" + impl_->path +
                        "' is from an earlier session and has no series '" + spec.name +
                        "' to append to");
        }
        auto s = resume_series(scan, impl_->group.get(), spec);
        impl_->inline_series.emplace(spec.name, s);
        return SeriesWriter(s);
    }
    auto s = std::make_shared<SeriesWriter::Impl>();
    s->scan = scan;
    s->compression = options.compression;
    s->layout = detail::SeriesLayout::make(
        spec, dims, impl_->attributes, options.chunk_bytes,
        std::numeric_limits<std::uint64_t>::max(), options.dtypes);
    h5::Lock lock(h5::mutex());
    s->group = h5::ensure_group(impl_->group.get(), spec.name);
    h5::write_string_attribute(s->group.get(), "storage", "inline");
    for (const auto& f : s->layout.fields) {
        s->datasets.push_back(
            detail::create_field_dataset(s->group.get(), f, s->compression));
    }
    impl_->inline_series.emplace(spec.name, s);
    return SeriesWriter(s);
}

SegmentedSeriesWriter NodeWriter::segmented_series(std::string_view name,
                                                   const Dims& dims,
                                                   const SegmentOptions& options) {
    auto scan = impl_->lock_scan();
    if (impl_->stored_schema) {
        throw Error("node '" + impl_->path +
                    "' is from an earlier session: only its attributes can be changed");
    }
    if (auto it = impl_->segmented.find(name); it != impl_->segmented.end()) {
        return SegmentedSeriesWriter(it->second);
    }
    auto s = std::make_shared<SegmentedSeriesWriter::Impl>(
        scan->dir, impl_->path, *impl_->schema, std::string(name), dims, options,
        impl_->attributes);
    impl_->segmented.emplace(std::string(name), s);
    return SegmentedSeriesWriter(s);
}

// ---------------------------------------------------------------------------
// ScanWriter

void ScanWriter::Impl::require_open() const {
    if (finalized) throw Error("scan " + detail::utf8(dir) + " is finalized");
}

void ScanWriter::Impl::finalize() {
    if (finalized) return;
    std::exception_ptr first_error;
    auto keep = [&] {
        if (!first_error) first_error = std::current_exception();
    };
    for (auto& node : nodes) {
        for (auto& [name, s] : node->segmented) {
            try {
                s->close();
            } catch (...) {
                keep();
            }
        }
        for (auto& [name, s] : node->inline_series) {
            try {
                s->close();
            } catch (...) {
                keep();
            }
        }
    }
    try {
        h5::Lock lock(h5::mutex());
        detail::link_all_segments(file.get(), dir);
        h5::write_attribute(file.get(), "complete", Value(mark_complete), DType::Bool);
        h5::write_string_attribute(file.get(), "finalized", detail::now_iso8601());
    } catch (...) {
        keep();
    }
    {
        h5::Lock lock(h5::mutex());
        for (auto& node : nodes) node->group.reset();
        file.reset();
    }
    finalized = true;
    if (first_error) std::rethrow_exception(first_error);
}

ScanWriter::ScanWriter(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
ScanWriter::ScanWriter(ScanWriter&&) noexcept = default;
ScanWriter& ScanWriter::operator=(ScanWriter&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            try {
                impl_->finalize();
            } catch (...) {
            }
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

ScanWriter::~ScanWriter() {
    if (!impl_) return;
    try {
        impl_->finalize();
    } catch (...) {
        // Use finalize() to see errors.
    }
}

ScanWriter ScanWriter::create(const fs::path& scan_dir, const ScanInfo& info,
                              bool overwrite) {
    h5::init();
    fs::create_directories(scan_dir);
    const fs::path master = scan_dir / master_file_name;
    if (fs::exists(master) && !overwrite) {
        throw Error(detail::utf8(master) + " exists already");
    }
    if (overwrite) {
        // Old segments would be linked into the new scan; remove them (and only
        // them: <category>/<node>/<series>/seg_NNNNNN.h5).
        std::error_code ec;
        for (const auto& category : fs::directory_iterator(scan_dir, ec)) {
            if (!category.is_directory()) continue;
            for (const auto& node : fs::directory_iterator(category.path(), ec)) {
                if (!node.is_directory()) continue;
                for (const auto& series : fs::directory_iterator(node.path(), ec)) {
                    if (!series.is_directory()) continue;
                    for (const auto& seg : detail::list_segments(series.path()))
                        fs::remove(seg);
                    fs::remove(series.path(), ec);  // only if empty now
                }
                fs::remove(node.path(), ec);
            }
            fs::remove(category.path(), ec);
        }
    }
    auto impl = std::make_shared<Impl>();
    impl->dir = scan_dir;
    h5::Lock lock(h5::mutex());
    impl->file = h5::create_file(detail::utf8(master), overwrite, false);
    const hid_t f = impl->file.get();
    h5::write_string_attribute(f, "format", format_name);
    h5::write_string_attribute(f, "format_version", format_version);
    h5::write_string_attribute(f, "uuid", detail::make_uuid());
    h5::write_string_attribute(f, "created", detail::now_iso8601());
    h5::write_string_attribute(f, "software", std::string("rbscan ") + version());
    h5::write_attribute(f, "complete", Value(false), DType::Bool);
    if (!info.description.empty())
        h5::write_string_attribute(f, "description", info.description);
    if (!info.operator_name.empty())
        h5::write_string_attribute(f, "operator", info.operator_name);
    if (!info.sample_id.empty())
        h5::write_string_attribute(f, "sample_id", info.sample_id);
    for (const auto& [name, value] : info.extra) h5::write_attribute(f, name, value);
    return ScanWriter(std::move(impl));
}

ScanWriter ScanWriter::open(const fs::path& scan_dir) {
    h5::init();
    const fs::path master = scan_dir / master_file_name;
    if (!fs::exists(master)) throw Error("no scan at " + detail::utf8(scan_dir));
    auto impl = std::make_shared<Impl>();
    impl->dir = scan_dir;
    h5::Lock lock(h5::mutex());
    try {
        impl->file = h5::open_file(detail::utf8(master), H5F_ACC_RDWR);
    } catch (const Error& e) {
        throw Error(
            std::string("cannot open the scan for writing; close every reader or writer "
                        "of it first: ") +
            e.what());
    }
    if (h5::read_string_attribute(impl->file.get(), "format") != format_name) {
        throw Error(detail::utf8(master) + " is not a " + std::string(format_name) +
                    " file");
    }
    const auto complete = h5::read_attribute(impl->file.get(), "complete");
    impl->mark_complete =
        complete && std::holds_alternative<bool>(*complete) && std::get<bool>(*complete);
    return ScanWriter(std::move(impl));
}

bool ScanWriter::has_node(std::string_view path) const {
    impl_->require_open();
    h5::Lock lock(h5::mutex());
    return h5::exists(impl_->file.get(), std::string(path)) &&
           h5::is_group(impl_->file.get(), std::string(path));
}

void ScanWriter::remove_node(std::string_view path) {
    impl_->require_open();
    const std::string p(path);
    for (const auto& n : impl_->nodes) {
        if (n->path == p && !n->stored_schema) {
            throw Error("node '" + p + "' was added by this writer");
        }
    }
    std::erase_if(impl_->nodes, [&](const auto& n) { return n->path == p; });
    std::error_code ec;
    for (const auto& series : fs::directory_iterator(impl_->dir / fs::path(p), ec)) {
        if (series.is_directory() && !detail::list_segments(series.path()).empty()) {
            throw Error("node '" + p + "' has segment files in " +
                        detail::utf8(series.path()) + "; delete them first");
        }
    }
    h5::Lock lock(h5::mutex());
    if (!has_node(p)) throw Error("no node '" + p + "'");
    h5::check(H5Ldelete(impl_->file.get(), p.c_str(), H5P_DEFAULT), "removing " + p);
}

NodeWriter ScanWriter::add_node(std::string_view name, std::string_view schema_id,
                                const Attributes& attributes) {
    impl_->require_open();
    const Schema& schema = SchemaRegistry::global().get(schema_id);
    detail::validate_node(name, schema, attributes);
    auto node = std::make_shared<NodeWriter::Impl>();
    node->scan = impl_;
    node->path = schema.category + "/" + std::string(name);
    node->schema = &schema;
    node->attributes = attributes;
    node->group =
        detail::create_node_group(impl_->file.get(), node->path, schema, attributes);
    impl_->nodes.push_back(node);
    return NodeWriter(node);
}

NodeWriter ScanWriter::node(std::string_view path) const {
    impl_->require_open();
    for (const auto& n : impl_->nodes) {
        if (n->path == path) return NodeWriter(n);
    }
    if (!has_node(path)) throw Error("no node '" + std::string(path) + "'");
    // A node of an earlier session: attributes can be changed, series not.
    h5::Lock lock(h5::mutex());
    auto n = std::make_shared<NodeWriter::Impl>();
    n->scan = impl_;
    n->path = std::string(path);
    n->group = h5::open_group(impl_->file.get(), n->path);
    const auto json = h5::read_string_attribute(n->group.get(), "schema_json");
    if (!json) throw Error("'" + n->path + "' is not a node (no schema)");
    n->stored_schema = std::make_shared<const Schema>(parse_schema(*json));
    n->schema = n->stored_schema.get();
    n->attributes = h5::read_attributes(n->group.get());
    n->attributes.erase("schema");
    n->attributes.erase("schema_json");
    impl_->nodes.push_back(n);
    return NodeWriter(std::move(n));
}

void ScanWriter::flush() {
    impl_->require_open();
    for (auto& node : impl_->nodes) {
        for (auto& [name, s] : node->inline_series) s->flush();
    }
    h5::Lock lock(h5::mutex());
    h5::check(H5Fflush(impl_->file.get(), H5F_SCOPE_LOCAL), "H5Fflush");
}

void ScanWriter::finalize() { impl_->finalize(); }

fs::path ScanWriter::directory() const { return impl_->dir; }

void ScanWriter::rebuild(const fs::path& scan_dir) {
    h5::init();
    const fs::path master =
        fs::is_directory(scan_dir) ? scan_dir / master_file_name : scan_dir;
    h5::Lock lock(h5::mutex());
    h5::Id file = h5::open_crashed_file(detail::utf8(master));
    detail::link_all_segments(file.get(), master.parent_path());
    h5::write_attribute(file.get(), "complete", Value(true), DType::Bool);
    h5::write_string_attribute(file.get(), "finalized", detail::now_iso8601());
}

const char* version() { return RBSCAN_VERSION; }

std::string hdf5_version() {
    unsigned major = 0, minor = 0, release = 0;
    H5get_libversion(&major, &minor, &release);
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(release);
}

}  // namespace raybotic::scan
