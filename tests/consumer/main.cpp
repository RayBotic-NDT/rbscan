// Smoke test for the installed SDK: write a small segmented scan, read it back.
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <raybotic/scan/rbscan.hpp>
#include <vector>

namespace rs = raybotic::scan;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: rbscan_consumer <scratch dir>\n";
        return 2;
    }
    const std::filesystem::path dir = std::filesystem::path(argv[1]) / "consumer_scan";
    std::filesystem::remove_all(dir);
    constexpr std::uint64_t h = 8, w = 12, n = 5;
    try {
        {
            auto scan = rs::ScanWriter::create(dir, {.description = "sdk consumer"});
            auto det = scan.add_node("detector_0", "raybotic.xray_detector",
                                     {{"frame_id", std::string("scan_detector")},
                                      {"simulated", true},
                                      {"sensor_width", std::int64_t{w}},
                                      {"sensor_height", std::int64_t{h}},
                                      {"pixel_pitch_u_um", 139.0},
                                      {"pixel_pitch_v_um", 139.0}});
            rs::SegmentOptions opts;
            opts.segment_size = 2;
            auto images =
                det.segmented_series("image", {{"height", h}, {"width", w}}, opts);
            for (std::uint64_t i = 0; i < n; ++i) {
                const std::vector<std::uint16_t> px(h * w, static_cast<std::uint16_t>(i));
                images.append(static_cast<std::int64_t>(i),
                              {{"image", rs::view(px.data(), {h, w})}});
            }
            scan.finalize();
        }
        const auto reader = rs::ScanReader::open(dir);
        const auto images = reader.node("sensors/detector_0").series("image");
        const auto last = images.read_sample("image", n - 1);
        if (images.size() != n || last.values<std::uint16_t>()[0] != n - 1) {
            std::cerr << "read back wrong data\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    std::cout << "rbscan " << rs::version() << " SDK OK\n";
    return 0;
}
