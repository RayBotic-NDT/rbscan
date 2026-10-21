#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "h5.hpp"
#include "raybotic/scan/writer.hpp"
#include "series_layout.hpp"
#include "thread_pool.hpp"

namespace raybotic::scan {

struct ScanWriter::Impl {
    std::filesystem::path dir;
    h5::Id file;
    bool finalized{false};
    /// Value of the `complete` flag finalize() writes: true for new scans, the
    /// previous value for scans opened with ScanWriter::open().
    bool mark_complete{true};
    std::vector<std::shared_ptr<NodeWriter::Impl>> nodes;

    void require_open() const;
    void finalize();
};

struct SeriesWriter::Impl {
    std::weak_ptr<ScanWriter::Impl> scan;
    detail::SeriesLayout layout;
    CompressionOptions compression;
    h5::Id group;
    std::vector<h5::Id> datasets;
    std::vector<detail::ChunkBuffer> buffers;
    std::uint64_t total{0};
    std::uint64_t flushed_total{0};
    std::int64_t last_time{std::numeric_limits<std::int64_t>::min()};
    bool closed{false};

    void append(std::int64_t time_ns, std::span<const FieldValue> fields);
    void flush();
    void close();
};

struct SegmentedSeriesWriter::Impl {
    Impl(std::filesystem::path scan_dir, std::string node_path, const Schema& schema,
         const std::string& series, const Dims& dims, const SegmentOptions& options,
         Attributes node_attributes);
    ~Impl();

    void append(std::int64_t time_ns, std::span<const FieldValue> fields);
    void flush();
    void close();

    std::filesystem::path scan_dir;
    std::string node_path;
    Schema schema;
    detail::SeriesLayout layout;
    SegmentOptions options;
    Attributes node_attributes;
    std::filesystem::path series_dir;

    // Caller side, guarded by append_mutex.
    std::mutex append_mutex;
    std::vector<detail::ChunkBuffer> buffers;
    std::uint64_t total{0};
    std::uint64_t in_segment{0};
    std::uint64_t segment{0};
    std::uint64_t segments_started{0};
    std::int64_t last_time{std::numeric_limits<std::int64_t>::min()};
    bool closed{false};

    // Pipeline between caller, compression pool and writer thread.
    struct Job {
        enum class Kind { Chunk, Flush, CloseSegment, Stop } kind{Kind::Chunk};
        std::uint64_t segment{0};
        std::size_t field{0};
        std::uint64_t chunk_index{0};
        std::uint64_t valid{0};
        std::future<std::vector<std::byte>> encoded;
        std::shared_ptr<std::promise<void>> done;
    };
    std::unique_ptr<detail::ThreadPool> pool;
    std::thread writer;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::condition_variable space_cv;
    std::deque<Job> queue;
    std::size_t pending_chunks{0};
    std::exception_ptr error;

    // Writer thread only.
    h5::Id file;
    std::vector<h5::Id> datasets;
    std::uint64_t open_segment{std::numeric_limits<std::uint64_t>::max()};

private:
    void submit_chunk(std::size_t field, std::uint64_t chunk_index, std::uint64_t valid,
                      detail::ChunkBuffer buffer);
    void submit_partials(bool copy);
    void push(Job job);
    void rethrow_error();
    void run_writer();
    void open_segment_file(std::uint64_t index);
    void close_segment_file();
};

struct NodeWriter::Impl {
    std::weak_ptr<ScanWriter::Impl> scan;
    std::string path;
    const Schema* schema{nullptr};
    h5::Id group;
    Attributes attributes;
    std::map<std::string, std::shared_ptr<SeriesWriter::Impl>, std::less<>> inline_series;
    std::map<std::string, std::shared_ptr<SegmentedSeriesWriter::Impl>, std::less<>> segmented;

    std::shared_ptr<ScanWriter::Impl> lock_scan() const;
};

}  // namespace raybotic::scan
