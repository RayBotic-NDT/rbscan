// Records a small CT scan with the typed API (schemas.hpp): structs instead of
// attribute maps and field names, one append call per table. Then reads one
// projection back together with the detector image it refers to.
//
//   rbscan_typed_ct_scan <scan_dir> [projections=90] [size=256]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <raybotic/scan/rbscan.hpp>
#include <raybotic/scan/schema_helpers.hpp>
#include <string>

namespace rs = raybotic::scan;
namespace src = raybotic::scan::schemas::xray_source;
namespace det = raybotic::scan::schemas::xray_detector;
namespace acq = raybotic::scan::schemas::ct_acquisition;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scan_dir> [projections] [size]\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const std::uint64_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 90;
    const std::uint64_t size = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 256;
    const double fod = 0.4, fdd = 1.0, pixel = 139e-6;  // m
    const std::int64_t period = 200'000'000;            // one projection every 200 ms

    try {
        auto scan = rs::ScanWriter::create(dir, {.description = "typed CT scan"}, true);

        // Nodes: required attributes are plain members, optional ones std::optional.
        src::Attributes sa;
        sa.frame_id = "scan_source";
        sa.simulated = true;
        sa.voltage_max_kv = 225.0;
        sa.focal_spots_um = std::vector<double>{5, 20, 80};
        auto source = src::add(scan, "source_0", sa);

        det::Attributes da;
        da.frame_id = "scan_detector";
        da.simulated = true;
        da.sensor_width = da.sensor_height = static_cast<std::uint32_t>(size);
        da.pixel_pitch_u_um = da.pixel_pitch_v_um = pixel * 1e6;
        auto detector = det::add(scan, "detector_0", da);

        acq::Attributes aa;
        aa.type = "ct_scan";
        aa.source = source.path();
        aa.detector = detector.path();
        aa.world_frame = "world";
        aa.trajectory = "circle, 360 deg";
        auto acquisition = acq::add(scan, "ct_0", aa);

        // Settings: one row each. Optional fields left empty are not written.
        const std::int64_t t0 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        src::StateRow tube;
        tube.time_ns = t0;
        tube.emission_state = 2;
        tube.emission_permitted = true;
        tube.voltage_setpoint_kv = tube.voltage_kv = 120.0;
        tube.current_setpoint_ua = tube.current_ua = 50.0;
        tube.power_w = 6.0;
        auto tube_state = source.series(src::state_series);
        src::append(tube_state, tube);

        det::StateRow panel;
        panel.time_ns = t0;
        panel.binning = 1;
        panel.image_width = panel.image_height = static_cast<std::uint32_t>(size);
        panel.integration_time_ms = 100.0;
        panel.averages = 1;
        panel.gain = 1.0;
        auto panel_state = detector.series(det::state_series);
        det::append(panel_state, panel);

        // Images and geometry as columns: all n rows in one append_many each.
        det::ImageColumns images;
        images.image = rs::Array(rs::DType::UInt16, {n, size, size});
        auto* px = static_cast<std::uint16_t*>(images.image.data());
        acq::ProjectionsColumns geometry;
        for (std::uint64_t i = 0; i < n; ++i) {
            const double a =
                2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
            const double c = std::cos(a), s = std::sin(a);
            const std::int64_t t = t0 + static_cast<std::int64_t>(i) * period;

            // A disc that moves with the angle.
            const double cx = 0.5 * static_cast<double>(size) * (1.0 + 0.4 * c);
            const double r = 0.15 * static_cast<double>(size);
            for (std::uint64_t y = 0; y < size; ++y) {
                for (std::uint64_t x = 0; x < size; ++x) {
                    const double dx = static_cast<double>(x) - cx;
                    const double dy =
                        static_cast<double>(y) - 0.5 * static_cast<double>(size);
                    px[(i * size + y) * size + x] =
                        dx * dx + dy * dy < r * r ? 20000 : 52000;
                }
            }
            images.time_ns.push_back(t);

            geometry.time_ns.push_back(t);
            geometry.image_index.push_back(i);
            geometry.source_position.push_back({fod * c, fod * s, 0.0});
            geometry.detector_position.push_back(
                {-(fdd - fod) * c, -(fdd - fod) * s, 0.0});
            geometry.detector_u.push_back({-s, c, 0.0});
            geometry.detector_v.push_back({0.0, 0.0, -1.0});
            geometry.pixel_size.push_back({pixel, pixel});
            geometry.voltage_kv.push_back(tube.voltage_kv);
            geometry.current_ua.push_back(tube.current_ua);
            geometry.flags.push_back(0);
            // Optional columns (scan_pose_index, ...) stay empty: not written.
        }
        auto image_writer = detector.segmented_series(
            det::image_series, {{"height", size}, {"width", size}});
        det::append(image_writer, images);
        image_writer.close();
        auto projections = acquisition.series(acq::projections_series);
        acq::append(projections, geometry);
        scan.finalize();

        // Read back: typed attributes, columns, and a projection with its image.
        const auto reader = rs::ScanReader::open(dir);
        const auto node = reader.node("acquisitions/ct_0");
        const acq::Attributes back = acq::read(node);
        const src::StateColumns tube_back = src::read_state(reader.node(back.source));
        const auto quarter = acq::read_projection(reader, node, n / 4);
        const auto pixels = quarter.image.values<std::uint16_t>();
        std::uint64_t dark = 0;
        for (const auto v : pixels) dark += v < 30000 ? 1 : 0;

        std::printf("%s: %s from %s to %s, %llu projections\n", node.path().c_str(),
                    back.type.c_str(), back.source.c_str(), back.detector.c_str(),
                    static_cast<unsigned long long>(acq::read_projections(node).size()));
        std::printf("tube: %.0f kV, %.0f uA\n", tube_back.voltage_kv.at(0),
                    tube_back.current_ua.at(0));
        std::printf(
            "projection %llu: source at (%.3f, %.3f, %.3f) m, image %llu x %llu, "
            "%llu dark pixels\n",
            static_cast<unsigned long long>(n / 4), quarter.geometry.source_position[0],
            quarter.geometry.source_position[1], quarter.geometry.source_position[2],
            static_cast<unsigned long long>(quarter.image.shape()[1]),
            static_cast<unsigned long long>(quarter.image.shape()[0]),
            static_cast<unsigned long long>(dark));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
