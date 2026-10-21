// Read throughput of a segmented series, the way a viewer's frame cache loads
// it: T threads, each reading single samples. Also one bulk read of the whole
// series.
//
//   rbscan_read_benchmark <scan_dir> [node_path=sensors/detector_0] [series=image]
//                         [field=image] [threads=1,2,4,8,16]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <raybotic/scan/rbscan.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace rs = raybotic::scan;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scan_dir> [node] [series] [field] [threads,...]\n",
                     argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const std::string node = argc > 2 ? argv[2] : "sensors/detector_0";
    const std::string series = argc > 3 ? argv[3] : "image";
    const std::string field = argc > 4 ? argv[4] : "image";
    std::vector<unsigned> thread_counts{1, 2, 4, 8, 16};
    if (argc > 5) {
        thread_counts.clear();
        std::stringstream in(argv[5]);
        std::string item;
        while (std::getline(in, item, ',')) thread_counts.push_back(std::stoul(item));
    }

    try {
        const auto reader = rs::SeriesReader::open_segments(dir, node, series);
        const std::uint64_t n = reader.size();
        const auto shape = reader.sample_shape(field);
        const double sample_mb =
            static_cast<double>(rs::element_count(shape) * rs::dtype_size(reader.dtype(field))) /
            1e6;
        std::printf("%llu samples of %.1f MB (%s)\n", static_cast<unsigned long long>(n),
                    sample_mb, std::string(rs::to_string(reader.dtype(field))).c_str());

        for (unsigned threads : thread_counts) {
            std::atomic<std::uint64_t> next{0};
            const auto start = std::chrono::steady_clock::now();
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t) {
                pool.emplace_back([&] {
                    for (std::uint64_t i = next++; i < n; i = next++) {
                        const auto a = reader.read_sample(field, i);
                        if (a.byte_size() == 0) std::abort();
                    }
                });
            }
            for (auto& t : pool) t.join();
            const double s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("  read_sample, %2u threads: %6.0f MB/s  %6.1f samples/s\n", threads,
                        sample_mb * static_cast<double>(n) / s, static_cast<double>(n) / s);
        }

        const auto start = std::chrono::steady_clock::now();
        const auto all = reader.read(field);
        const double s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("  read (all at once):      %6.0f MB/s\n",
                    static_cast<double>(all.byte_size()) / 1e6 / s);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
