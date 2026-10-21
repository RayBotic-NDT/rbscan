// Records a synthetic CT scan the way the cell would: source and detector
// state, two robots, the scan frames, the projections (segmented, compressed
// in parallel) and the resolved geometry per projection. Prints the write rate.
//
//   rbscan_record_ct_scan <scan_dir> [projections=360] [size=1024] [threads=0]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <raybotic/scan/rbscan.hpp>
#include <string>
#include <vector>

namespace rs = raybotic::scan;

namespace {

std::vector<std::uint16_t> projection(std::uint64_t size, double angle) {
    // A blurred disc moving with the angle, plus a little texture: compresses
    // roughly like a real flat-panel image.
    std::vector<std::uint16_t> img(size * size);
    const double cx = 0.5 * size + 0.2 * size * std::cos(angle);
    const double r2 = 0.09 * size * size;
    for (std::uint64_t y = 0; y < size; ++y) {
        for (std::uint64_t x = 0; x < size; ++x) {
            const double dx = x - cx;
            const double dy = y - 0.5 * size;
            const double absorb = std::exp(-(dx * dx + dy * dy) / r2);
            img[y * size + x] =
                static_cast<std::uint16_t>(52000.0 * (1.0 - 0.7 * absorb) + ((x * 31 + y * 17) & 63));
        }
    }
    return img;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scan_dir> [projections] [size] [threads]\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const std::uint64_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 360;
    const std::uint64_t size = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1024;
    const unsigned threads = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 0;

    try {
        auto scan = rs::ScanWriter::create(dir, {.description = "synthetic CT scan",
                                                 .operator_name = "rbscan example",
                                                 .sample_id = "phantom_01"},
                                           true);

        auto source = scan.add_node("source_0", "raybotic.xray_source",
                                    {{"frame_id", std::string("scan_source")},
                                     {"simulated", true},
                                     {"voltage_min_kv", 20.0},
                                     {"voltage_max_kv", 225.0},
                                     {"current_max_ua", 1000.0},
                                     {"power_max_w", 320.0},
                                     {"focal_spots_um", std::vector<double>{5, 20, 80}},
                                     {"filter_material", std::string("Cu")}});
        auto detector = scan.add_node("detector_0", "raybotic.xray_detector",
                                      {{"frame_id", std::string("scan_detector")},
                                       {"simulated", true},
                                       {"sensor_width", static_cast<std::int64_t>(size)},
                                       {"sensor_height", static_cast<std::int64_t>(size)},
                                       {"pixel_pitch_u_um", 139.0},
                                       {"pixel_pitch_v_um", 139.0}});
        const std::vector<std::string> joints = {"J1", "J2", "J3", "J4", "J5", "J6"};
        auto robot_attrs = [&](const char* base, const char* tcp, const char* target) {
            return rs::Attributes{{"joint_names", joints},
                                  {"base_frame", std::string(base)},
                                  {"tcp_frame", std::string(tcp)},
                                  {"target_frame", std::string(target)},
                                  {"simulated", true},
                                  {"model", std::string("FANUC CRX-30iA")}};
        };
        auto source_robot = scan.add_node(
            "source_robot", "raybotic.robot_arm",
            robot_attrs("source_robot_base", "source_robot_tcp", "scan_source"));
        auto detector_robot = scan.add_node(
            "detector_robot", "raybotic.robot_arm",
            robot_attrs("detector_robot_base", "detector_robot_tcp", "scan_detector"));
        auto frame = [&](const char* child) {
            return scan.add_node(child, "raybotic.transform",
                                 {{"parent", std::string("world")},
                                  {"child", std::string(child)},
                                  {"static", false}});
        };
        auto source_tf = frame("scan_source").series("pose");
        auto detector_tf = frame("scan_detector").series("pose");
        auto acq = scan.add_node("scan_0", "raybotic.ct_acquisition",
                                 {{"type", std::string("ct_scan")},
                                  {"source", source.path()},
                                  {"detector", detector.path()},
                                  {"world_frame", std::string("world")},
                                  {"trajectory", std::string("circular, 360 deg")}});

        rs::SegmentOptions seg;
        seg.threads = threads;
        auto images = detector.segmented_series("image", {{"height", size}, {"width", size}}, seg);
        auto det_state = detector.series("state");
        auto src_state = source.series("state");
        auto source_joints = source_robot.series("joint_states");
        auto detector_joints = detector_robot.series("joint_states");
        auto projections = acq.series("projections");

        const std::int64_t t0 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        const double integration_ms = 100.0;
        {
            const std::uint8_t binning = 1, emission = 2;
            const std::uint32_t w = static_cast<std::uint32_t>(size), averages = 1;
            const double gain = 1.0, kv = 120.0, ua = 50.0, spot = 5.0, power = 6.0, filter = 0.5;
            const bool permitted = true;
            det_state.append(t0, {{"binning", rs::scalar(binning)},
                                  {"image_width", rs::scalar(w)},
                                  {"image_height", rs::scalar(w)},
                                  {"integration_time_ms", rs::scalar(integration_ms)},
                                  {"averages", rs::scalar(averages)},
                                  {"gain", rs::scalar(gain)}});
            src_state.append(t0, {{"emission_state", rs::scalar(emission)},
                                  {"emission_permitted", rs::scalar(permitted)},
                                  {"voltage_setpoint_kv", rs::scalar(kv)},
                                  {"current_setpoint_ua", rs::scalar(ua)},
                                  {"focal_spot_um", rs::scalar(spot)},
                                  {"voltage_kv", rs::scalar(kv)},
                                  {"current_ua", rs::scalar(ua)},
                                  {"power_w", rs::scalar(power)},
                                  {"filter_thickness_mm", rs::scalar(filter)}});
        }

        // A handful of synthetic images, reused, so the timing measures writing.
        std::vector<std::vector<std::uint16_t>> phantom;
        for (int k = 0; k < 16; ++k) phantom.push_back(projection(size, k * std::numbers::pi / 8));

        const double fod = 0.4, fdd = 1.0, pixel = 139e-6;
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < n; ++i) {
            const double a = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
            const std::int64_t t = t0 + static_cast<std::int64_t>(i) * 200'000'000;
            const double c = std::cos(a), s = std::sin(a);
            const double src[3] = {fod * c, fod * s, 0.0};
            const double det[3] = {-(fdd - fod) * c, -(fdd - fod) * s, 0.0};
            const double u[3] = {-s, c, 0.0};
            const double v[3] = {0.0, 0.0, -1.0};
            const double px[2] = {pixel, pixel};
            const double q_src[4] = {0, 0, std::sin(a / 2), std::cos(a / 2)};

            // Robots and frames at a higher rate than the projections.
            for (int k = 0; k < 4; ++k) {
                const std::int64_t tk = t + k * 50'000'000;
                std::vector<double> q(6);
                for (std::size_t j = 0; j < 6; ++j) q[j] = 0.3 * std::sin(a + 0.1 * j + 0.01 * k);
                source_joints.append(tk, {{"position", rs::view(q)}});
                detector_joints.append(tk, {{"position", rs::view(q)}});
                source_tf.append(tk, {{"translation", rs::view(src, {3})},
                                      {"rotation", rs::view(q_src, {4})}});
                detector_tf.append(tk, {{"translation", rs::view(det, {3})},
                                        {"rotation", rs::view(q_src, {4})}});
            }

            const auto& img = phantom[i % phantom.size()];
            images.append(t, {{"image", rs::view(img.data(), {size, size})}});

            const std::uint64_t index = i;
            const std::uint32_t flags = 0;
            const double kv = 120.0, ua = 50.0;
            projections.append(t, {{"image_index", rs::scalar(index)},
                                   {"source_position", rs::view(src, {3})},
                                   {"detector_position", rs::view(det, {3})},
                                   {"detector_u", rs::view(u, {3})},
                                   {"detector_v", rs::view(v, {3})},
                                   {"pixel_size", rs::view(px, {2})},
                                   {"voltage_kv", rs::scalar(kv)},
                                   {"current_ua", rs::scalar(ua)},
                                   {"integration_time_ms", rs::scalar(integration_ms)},
                                   {"flags", rs::scalar(flags)}});
        }
        scan.finalize();
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double mb = static_cast<double>(n * size * size * 2) / 1e6;
        std::printf("%llu projections %llux%llu: %.0f MB raw in %.2f s = %.0f MB/s\n",
                    static_cast<unsigned long long>(n), static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(size), mb, seconds, mb / seconds);
        std::printf("written to %s\n", dir.c_str());
    } catch (const rs::Error& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
