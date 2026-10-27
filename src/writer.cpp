#include "raybotic/scan/writer.hpp"

#include "compression.hpp"
#include "internal.hpp"
#include "raybotic/scan/rbscan.hpp"
#include "writer_impl.hpp"

namespace raybotic::scan {

namespace fs = std::filesystem;

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
    if (impl_->stored_schema) {
        throw Error("node '" + impl_->path +
                    "' is from an earlier session: only its attributes can be changed");
    }
    if (auto it = impl_->inline_series.find(name); it != impl_->inline_series.end()) {
        return SeriesWriter(it->second);
    }
    const SeriesSpec& spec = impl_->schema->get_series(name);
    if (spec.storage != Storage::Inline) {
        throw Error("series '" + spec.name + "' of " + impl_->schema->id() +
                    " is segmented; use segmented_series()");
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
    h5::Id file = h5::open_file(detail::utf8(master), H5F_ACC_RDWR);
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
