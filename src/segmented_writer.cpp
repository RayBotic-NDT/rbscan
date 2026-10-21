#include <algorithm>
#include <cstring>

#include "compression.hpp"
#include "internal.hpp"
#include "writer_impl.hpp"

namespace raybotic::scan {

namespace fs = std::filesystem;
using detail::ChunkBuffer;

namespace {

void validate_node_path(std::string_view path) {
    const auto slash = path.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= path.size() ||
        path.find('/', slash + 1) != std::string_view::npos || path.find("..") != path.npos) {
        throw Error("node path must be <category>/<name>, got '" + std::string(path) + "'");
    }
}

}  // namespace

SegmentedSeriesWriter::Impl::Impl(fs::path scan_dir_, std::string node_path_,
                                  const Schema& schema_, const std::string& series,
                                  const Dims& dims, const SegmentOptions& options_,
                                  Attributes node_attributes_)
    : scan_dir(std::move(scan_dir_)),
      node_path(std::move(node_path_)),
      schema(schema_),
      options(options_),
      node_attributes(std::move(node_attributes_)) {
    validate_node_path(node_path);
    const SeriesSpec& spec = schema.get_series(series);
    if (spec.storage != Storage::Segmented) {
        throw Error("series '" + series + "' of " + schema.id() +
                    " is stored inline; use NodeWriter::series()");
    }
    if (options.segment_size == 0) throw Error("segment_size must be > 0");
    if (options.max_pending == 0) options.max_pending = 1;
    layout = detail::SeriesLayout::make(spec, dims, node_attributes, options.chunk_bytes,
                                        options.segment_size, options.dtypes);
    series_dir = scan_dir / fs::path(node_path) / series;
    if (!detail::list_segments(series_dir).empty()) {
        throw Error(detail::utf8(series_dir) + " already holds segments of another recording");
    }
    buffers.resize(layout.fields.size());

    unsigned threads = options.threads;
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    h5::init();
    pool = std::make_unique<detail::ThreadPool>(threads);
    writer = std::thread([this] { run_writer(); });
}

SegmentedSeriesWriter::Impl::~Impl() {
    try {
        close();
    } catch (...) {
        // Destructors must not throw; call close() to see errors.
    }
}

void SegmentedSeriesWriter::Impl::append(std::int64_t time_ns, std::span<const FieldValue> fields) {
    std::scoped_lock lock(append_mutex);
    if (closed) throw Error("segmented series '" + layout.spec.name + "' is closed");
    rethrow_error();
    if (time_ns < last_time) {
        throw Error("series '" + layout.spec.name + "': time must not decrease");
    }

    const auto n = layout.fields.size();
    std::vector<std::uint64_t> slots(n);
    for (std::size_t i = 0; i < n; ++i) slots[i] = in_segment % layout.fields[i].chunk_samples;
    detail::stage_sample(layout, buffers, slots, time_ns, fields);
    last_time = time_ns;
    ++in_segment;
    ++total;

    for (std::size_t i = 0; i < n; ++i) {
        const auto cs = layout.fields[i].chunk_samples;
        if (in_segment % cs == 0) submit_chunk(i, (in_segment - 1) / cs, cs, std::move(buffers[i]));
    }
    if (in_segment == options.segment_size) {
        submit_partials(false);
        Job close_job;
        close_job.kind = Job::Kind::CloseSegment;
        close_job.segment = segment;
        push(std::move(close_job));
        ++segment;
        in_segment = 0;
    } else if (options.flush_every > 0 && in_segment % options.flush_every == 0) {
        submit_partials(true);
        Job flush_job;
        flush_job.kind = Job::Kind::Flush;
        push(std::move(flush_job));
    }
}

void SegmentedSeriesWriter::Impl::flush() {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    {
        std::scoped_lock lock(append_mutex);
        if (closed) return;
        rethrow_error();
        submit_partials(true);
        Job job;
        job.kind = Job::Kind::Flush;
        job.done = done;
        push(std::move(job));
    }
    future.get();
}

void SegmentedSeriesWriter::Impl::close() {
    std::scoped_lock lock(append_mutex);
    if (closed) return;
    closed = true;
    try {
        if (in_segment > 0) {
            submit_partials(false);
            Job job;
            job.kind = Job::Kind::CloseSegment;
            job.segment = segment;
            push(std::move(job));
        }
    } catch (...) {
        // The writer thread already holds the error; rethrown below.
    }
    Job stop;
    stop.kind = Job::Kind::Stop;
    push(std::move(stop));
    if (writer.joinable()) writer.join();
    pool.reset();
    rethrow_error();
}

void SegmentedSeriesWriter::Impl::submit_chunk(std::size_t field, std::uint64_t chunk_index,
                                               std::uint64_t valid, ChunkBuffer buffer) {
    {
        std::unique_lock lock(queue_mutex);
        space_cv.wait(lock, [&] { return pending_chunks < options.max_pending || error; });
        if (error) std::rethrow_exception(error);
        ++pending_chunks;
    }
    const auto& f = layout.fields[field];
    detail::zero_tail(f, buffer.get(), valid);
    // MSVC's packaged_task needs a copyable callable, hence shared_ptr.
    std::shared_ptr<std::byte[]> data(std::move(buffer));
    const std::size_t bytes = f.chunk_bytes();
    const std::size_t element = dtype_size(f.dtype);
    const CompressionOptions compression = options.compression;

    Job job;
    job.kind = Job::Kind::Chunk;
    job.segment = segment;
    job.field = field;
    job.chunk_index = chunk_index;
    job.valid = valid;
    job.encoded = pool->submit([data, bytes, element, compression] {
        return detail::encode_chunk(data.get(), bytes, element, compression);
    });
    push(std::move(job));
}

void SegmentedSeriesWriter::Impl::submit_partials(bool copy) {
    for (std::size_t i = 0; i < layout.fields.size(); ++i) {
        const auto& f = layout.fields[i];
        const auto fill = in_segment % f.chunk_samples;
        if (fill == 0 || !buffers[i]) continue;
        ChunkBuffer buffer;
        if (copy) {
            buffer.reset(new std::byte[f.chunk_bytes()]);
            std::memcpy(buffer.get(), buffers[i].get(), fill * f.sample_bytes);
        } else {
            buffer = std::move(buffers[i]);
        }
        submit_chunk(i, in_segment / f.chunk_samples, fill, std::move(buffer));
    }
}

void SegmentedSeriesWriter::Impl::push(Job job) {
    {
        std::scoped_lock lock(queue_mutex);
        queue.push_back(std::move(job));
    }
    queue_cv.notify_one();
}

void SegmentedSeriesWriter::Impl::rethrow_error() {
    std::scoped_lock lock(queue_mutex);
    if (error) std::rethrow_exception(error);
}

void SegmentedSeriesWriter::Impl::run_writer() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(queue_mutex);
            queue_cv.wait(lock, [&] { return !queue.empty(); });
            job = std::move(queue.front());
            queue.pop_front();
        }
        std::exception_ptr failed;
        {
            std::scoped_lock lock(queue_mutex);
            failed = error;
        }
        try {
            switch (job.kind) {
                case Job::Kind::Chunk: {
                    struct Release {
                        Impl* self;
                        ~Release() {
                            {
                                std::scoped_lock lock(self->queue_mutex);
                                --self->pending_chunks;
                            }
                            self->space_cv.notify_all();
                        }
                    } release{this};
                    std::vector<std::byte> encoded = job.encoded.get();
                    if (failed) break;
                    if (open_segment != job.segment) {
                        close_segment_file();
                        open_segment_file(job.segment);
                    }
                    const auto& f = layout.fields[job.field];
                    detail::write_chunk(datasets[job.field].get(), f, job.chunk_index,
                                        job.chunk_index * f.chunk_samples + job.valid, encoded);
                    break;
                }
                case Job::Kind::Flush:
                    if (!failed && file) {
                        h5::Lock lock(h5::mutex());
                        h5::check(H5Fflush(file.get(), H5F_SCOPE_LOCAL), "H5Fflush");
                    }
                    if (job.done) {
                        if (failed) {
                            job.done->set_exception(failed);
                        } else {
                            job.done->set_value();
                        }
                    }
                    break;
                case Job::Kind::CloseSegment:
                    if (!failed && open_segment == job.segment) close_segment_file();
                    break;
                case Job::Kind::Stop:
                    close_segment_file();
                    return;
            }
        } catch (...) {
            {
                std::scoped_lock lock(queue_mutex);
                if (!error) error = std::current_exception();
            }
            space_cv.notify_all();
            if (job.done) job.done->set_exception(std::current_exception());
            if (job.kind == Job::Kind::Stop) return;
        }
    }
}

void SegmentedSeriesWriter::Impl::open_segment_file(std::uint64_t index) {
    fs::create_directories(series_dir);
    h5::Lock lock(h5::mutex());
    file = h5::create_file(detail::utf8(detail::segment_path(series_dir, index)), false, true);
    const hid_t f = file.get();
    h5::write_string_attribute(f, "format", detail::segment_format_name);
    h5::write_string_attribute(f, "format_version", format_version);
    h5::write_string_attribute(f, "node_path", node_path);
    h5::write_string_attribute(f, "schema", schema.id());
    h5::write_string_attribute(f, "schema_json", schema.to_json());
    h5::write_string_attribute(f, "series", layout.spec.name);
    h5::write_attribute(f, "segment_index", Value(static_cast<std::int64_t>(index)),
                        DType::UInt64);
    h5::write_attribute(f, "first_sample",
                        Value(static_cast<std::int64_t>(index * options.segment_size)),
                        DType::UInt64);
    h5::write_attribute(f, "segment_size", Value(static_cast<std::int64_t>(options.segment_size)),
                        DType::UInt64);
    {
        h5::Id g = h5::ensure_group(f, detail::segment_node_group);
        for (const auto& [name, value] : node_attributes) {
            detail::write_node_attribute(g.get(), schema, name, value);
        }
    }
    datasets.clear();
    for (const auto& field : layout.fields) {
        datasets.push_back(detail::create_field_dataset(f, field, options.compression));
    }
    if (options.swmr) h5::check(H5Fstart_swmr_write(f), "H5Fstart_swmr_write");
    open_segment = index;
}

void SegmentedSeriesWriter::Impl::close_segment_file() {
    h5::Lock lock(h5::mutex());
    datasets.clear();
    file.reset();
    open_segment = std::numeric_limits<std::uint64_t>::max();
}

SegmentedSeriesWriter::SegmentedSeriesWriter(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SegmentedSeriesWriter SegmentedSeriesWriter::open(const fs::path& scan_dir,
                                                  std::string_view node_path,
                                                  std::string_view schema_id,
                                                  std::string_view series, const Dims& dims,
                                                  const SegmentOptions& options,
                                                  const Attributes& attributes) {
    const Schema& schema = SchemaRegistry::global().get(schema_id);
    return SegmentedSeriesWriter(std::make_shared<Impl>(scan_dir, std::string(node_path), schema,
                                                        std::string(series), dims, options,
                                                        attributes));
}

void SegmentedSeriesWriter::append(std::int64_t time_ns, std::span<const FieldValue> fields) {
    impl_->append(time_ns, fields);
}

void SegmentedSeriesWriter::flush() { impl_->flush(); }
void SegmentedSeriesWriter::close() { impl_->close(); }

std::uint64_t SegmentedSeriesWriter::size() const {
    std::scoped_lock lock(impl_->append_mutex);
    return impl_->total;
}

std::uint64_t SegmentedSeriesWriter::segment_count() const {
    std::scoped_lock lock(impl_->append_mutex);
    return impl_->segment + (impl_->in_segment > 0 ? 1 : 0);
}

const SeriesSpec& SegmentedSeriesWriter::spec() const { return impl_->layout.spec; }

Shape SegmentedSeriesWriter::sample_shape(std::string_view field) const {
    return impl_->layout.sample_shape(field);
}

fs::path SegmentedSeriesWriter::directory() const { return impl_->series_dir; }

}  // namespace raybotic::scan
