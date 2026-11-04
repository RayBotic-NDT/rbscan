// Plans two trajectories (a circle and a laminography orbit), stores the plan,
// then stores markers that were clicked in a few projections, and reads both
// back the way a viewer would: trajectory by trajectory, marker by marker.
//
//   rbscan_plan_and_annotate <scan_dir>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <raybotic/scan/rbscan.hpp>
#include <raybotic/scan/schema_helpers.hpp>
#include <vector>

namespace rs = raybotic::scan;
namespace plan = raybotic::scan::schemas::trajectory_plan;
namespace notes = raybotic::scan::schemas::annotations;

namespace {

/// Quaternion (x, y, z, w) for a turn of `deg` about the world z axis.
std::array<double, 4> about_z(double deg) {
    const double h = 0.5 * deg * std::numbers::pi / 180.0;
    return {0.0, 0.0, std::sin(h), std::cos(h)};
}

/// An orbit about the z axis through `centre`, `count` poses over `range` deg,
/// the source tilted `tilt` deg out of the plane normal to the axis.
plan::Trajectory orbit(std::uint8_t kind, double tilt, std::uint32_t count,
                       double range) {
    plan::Trajectory t;
    plan::TrajectoriesRow& o = t.orbit;
    o.kind = kind;
    o.centre = {0.0, 0.0, 0.1};
    o.axis = {0.0, 0.0, 1.0};
    o.start_direction = {1.0, 0.0, 0.0};
    o.tilt = tilt;
    o.fod = 0.4;
    o.fdd = 1.0;
    o.projections = count;
    o.start_angle = 0.0;
    o.range = range;
    o.counter_clockwise = true;

    const double lift = std::tan(tilt * std::numbers::pi / 180.0);
    for (std::uint32_t i = 0; i < count; ++i) {
        const double deg = range * i / count;
        const double a = deg * std::numbers::pi / 180.0;
        const double c = std::cos(a), s = std::sin(a);
        plan::PosesRow p;
        p.angle = deg;
        p.skip = i % 10 == 9;  // every tenth pose switched off, e.g. unreachable
        p.roi_position = o.centre;
        p.roi_rotation = about_z(0.0);
        p.source_position = {o.fod * c, o.fod * s, o.centre[2] + o.fod * lift};
        p.source_rotation = about_z(deg + 180.0);
        const double back = o.fdd - o.fod;
        p.detector_position = {-back * c, -back * s, o.centre[2] - back * lift};
        p.detector_rotation = about_z(deg);
        t.poses.push_back(p);
    }
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scan_dir>\n", argv[0]);
        return 2;
    }
    try {
        {
            auto scan = rs::ScanWriter::create(argv[1],
                                               {.description = "plan and markers"}, true);
            plan::Attributes pa;
            pa.application = "rbscan example";
            pa.acquisition = "acquisitions/ct_0";
            plan::write(scan, "plan", pa,
                        {orbit(0, 0.0, 36, 360.0), orbit(1, 30.0, 20, 360.0)});

            // Two markers, each seen in some projections of acquisitions/ct_0.
            // Observations may come in any order; write() sorts them by time.
            const std::int64_t t0 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::system_clock::now()
                                            .time_since_epoch())
                                        .count(),
                               period = 200'000'000;
            auto marker = [&](std::uint32_t id, std::array<double, 3> at,
                              std::vector<std::uint64_t> seen_in) {
                notes::Marker m;
                m.row.region_id = 1;
                m.row.marker_id = id;
                m.row.position = at;
                m.row.position_source = 1;  // triangulated from the observations
                m.row.quality = 3;
                for (const auto projection : seen_in) {
                    notes::ObservationsRow o;
                    o.time_ns = t0 + static_cast<std::int64_t>(projection) * period;
                    o.projection = projection;
                    o.image_index = projection;
                    o.pixel = {100.0 + static_cast<double>(projection), 200.0 - 0.5 * id};
                    m.observations.push_back(o);
                }
                return m;
            };
            notes::Attributes na;
            na.application = "rbscan example";
            na.format = "rbscan-example-annotations/1";
            na.document = "{}";
            na.acquisition = "acquisitions/ct_0";
            notes::write(scan, "viewer", na,
                         {marker(1, {0.01, 0.0, 0.1}, {30, 0, 12}),
                          marker(2, {-0.01, 0.005, 0.12}, {5, 25})});
            scan.finalize();
        }

        const auto reader = rs::ScanReader::open(argv[1]);
        for (const plan::Trajectory& t :
             plan::read_plan(reader.node("trajectories/plan"))) {
            std::size_t driven = 0;
            for (const auto& p : t.poses) driven += p.skip ? 0 : 1;
            std::printf(
                "%-12s tilt %2.0f deg: %zu poses, %zu driven, first source at "
                "(%.3f, %.3f, %.3f) m\n",
                t.orbit.kind == 0 ? "circle" : "laminography", t.orbit.tilt,
                t.poses.size(), driven, t.poses[0].source_position[0],
                t.poses[0].source_position[1], t.poses[0].source_position[2]);
        }
        for (const notes::Marker& m :
             notes::read_annotations(reader.node("annotations/viewer"))) {
            std::printf("marker %u seen in %u projections:", m.row.marker_id,
                        m.row.observations);
            for (const auto& o : m.observations) {
                std::printf(" %llu", static_cast<unsigned long long>(o.projection));
            }
            std::printf("\n");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
