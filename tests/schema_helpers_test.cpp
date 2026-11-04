// The hand-written helpers over related tables (schema_helpers.hpp).

#include <gtest/gtest.h>

#include <cstring>
#include <raybotic/scan/rbscan.hpp>
#include <raybotic/scan/schema_helpers.hpp>

#include "test_util.hpp"

namespace rs = raybotic::scan;
namespace sc = raybotic::scan::schemas;
using rbscan_test::TempDir;

TEST(SchemaHelpers, VolumeWithItsMatrix) {
    TempDir dir;
    rs::Array voxels(rs::DType::UInt16, {3, 4, 5});
    auto* v = static_cast<std::uint16_t*>(voxels.data());
    for (std::uint64_t i = 0; i < voxels.size(); ++i)
        v[i] = static_cast<std::uint16_t>(i);
    const std::array<double, 16> m = {2e-4, 0, 0,    -1e-3, 0, 3e-4, 0, 0,
                                      0,    0, 1e-4, 5e-3,  0, 0,    0, 1};
    {
        auto scan = rs::ScanWriter::create(dir.path());
        sc::volume::Attributes a;
        a.quantity = "attenuation";
        sc::volume::write(scan, "reco", voxels, m, a);
        scan.finalize();
    }
    const auto node = rs::ScanReader::open(dir.path()).node("volumes/reco");
    const auto back = sc::volume::read_volume(node);
    EXPECT_TRUE(rbscan_test::same(back.voxels, voxels));
    EXPECT_EQ(back.voxel_to_world, m);
    EXPECT_EQ(back.attributes.width, 5u);
    EXPECT_EQ(back.attributes.depth, 3u);
    EXPECT_DOUBLE_EQ(back.attributes.voxel_size[1], 3e-4);
    EXPECT_EQ(*back.attributes.world_frame, "world");
    EXPECT_EQ(*back.attributes.quantity, "attenuation");
}

TEST(SchemaHelpers, SimulationSceneObjectByObject) {
    TempDir dir;
    sc::simulation_scene::Object part;
    part.name = "D\xC3\xBCse";
    part.source = "duese.stl";
    part.material = "Ti";
    part.placement.scale = 1.5;
    part.placement.enabled = true;
    part.placement.position = {1.0, 2.0, 40.0};
    part.vertices = {{-0.0f, 1e-40f, 0.1f}, {1, 0, 0}, {0, 1, 0}};
    part.triangles = {{0, 1, 2}};
    sc::simulation_scene::RegistrationPoint p;
    p.marker = "M7";
    p.row.marker_id = 7;
    p.row.placed = true;
    p.row.mesh_point = {1.0 / 3.0, 0.0, 0.0};
    part.registration.push_back(p);
    sc::simulation_scene::Object holder;
    holder.name = "Halter";
    holder.material = "C2H4";
    holder.placement.is_compound = true;
    holder.vertices = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}};
    holder.triangles = {{0, 2, 1}, {0, 1, 3}};
    {
        auto scan = rs::ScanWriter::create(dir.path());
        sc::simulation_scene::Attributes a;
        a.application = "test";
        a.format = "raybotic-simulation-scene/1";
        a.document = "{}";
        a.voltage_kv = 160.0;
        sc::simulation_scene::write(scan, "scene", a, {part, holder});
        auto bad = holder;
        bad.triangles = {{0, 1, 9}};
        EXPECT_THROW(sc::simulation_scene::write(scan, "bad", a, {bad}), rs::Error);
        scan.finalize();
    }
    const auto node = rs::ScanReader::open(dir.path()).node("simulations/scene");
    const auto [a, objects] = sc::simulation_scene::read_scene(node);
    EXPECT_EQ(*a.voltage_kv, 160.0);
    ASSERT_EQ(objects.size(), 2u);
    EXPECT_EQ(objects[0].name, "D\xC3\xBCse");
    EXPECT_EQ(objects[1].material, "C2H4");
    EXPECT_TRUE(objects[1].placement.is_compound);
    EXPECT_EQ(objects[0].placement.position, part.placement.position);
    EXPECT_EQ(std::memcmp(objects[0].vertices.data(), part.vertices.data(), 36), 0);
    EXPECT_EQ(objects[1].vertices, holder.vertices);
    EXPECT_EQ(objects[1].triangles, holder.triangles);  // counted from its own vertices
    EXPECT_EQ(objects[1].placement.first_vertex, 3u);
    ASSERT_EQ(objects[0].registration.size(), 1u);
    EXPECT_EQ(objects[0].registration[0].marker, "M7");
    EXPECT_EQ(objects[0].registration[0].row.mesh_point, p.row.mesh_point);
    EXPECT_TRUE(objects[1].registration.empty());
    EXPECT_TRUE(rs::validate(dir.path()).ok());
}

TEST(SchemaHelpers, TrajectoryPlanTrajectoryByTrajectory) {
    TempDir dir;
    std::vector<sc::trajectory_plan::Trajectory> plan(2);
    plan[0].orbit.kind = 0;
    plan[0].orbit.fod = 0.3;
    plan[0].poses.resize(3);
    plan[0].poses[1].angle = 120.0;
    plan[1].orbit.kind = 1;
    plan[1].orbit.tilt = 30.0;
    plan[1].poses.resize(2);
    plan[1].poses[1].skip = true;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        sc::trajectory_plan::Attributes a;
        a.application = "test";
        sc::trajectory_plan::write(scan, "plan", a, plan);
        scan.finalize();
    }
    const auto back = sc::trajectory_plan::read_plan(
        rs::ScanReader::open(dir.path()).node("trajectories/plan"));
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[0].poses.size(), 3u);
    EXPECT_DOUBLE_EQ(back[0].poses[1].angle, 120.0);
    EXPECT_EQ(back[1].orbit.first_pose, 3u);
    EXPECT_EQ(back[1].poses[1].trajectory, 1u);
    EXPECT_TRUE(back[1].poses[1].skip);
    EXPECT_DOUBLE_EQ(back[1].orbit.tilt, 30.0);
}

TEST(SchemaHelpers, AnnotationsMarkerByMarker) {
    TempDir dir;
    std::vector<sc::annotations::Marker> markers(2);
    markers[0].row.marker_id = 4;
    markers[0].row.position = {0.1, 0.2, 0.3};
    markers[1].row.marker_id = 9;
    // Out of time order across markers: written sorted, read back per marker.
    sc::annotations::ObservationsRow o;
    o.time_ns = 30;
    o.projection = 3;
    markers[0].observations.push_back(o);
    o.time_ns = 10;
    o.projection = 1;
    markers[1].observations.push_back(o);
    o.time_ns = 20;
    o.projection = 2;
    markers[0].observations.push_back(o);
    {
        auto scan = rs::ScanWriter::create(dir.path());
        sc::annotations::Attributes a;
        a.application = "test";
        a.format = "test/1";
        a.document = "{}";
        sc::annotations::write(scan, "viewer", a, markers);
        scan.finalize();
    }
    const auto back = sc::annotations::read_annotations(
        rs::ScanReader::open(dir.path()).node("annotations/viewer"));
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[0].row.observations, 2u);
    ASSERT_EQ(back[0].observations.size(), 2u);
    EXPECT_EQ(back[0].observations[0].projection, 2u);  // time order
    EXPECT_EQ(back[0].observations[0].marker_id, 4u);
    EXPECT_EQ(back[1].observations.at(0).projection, 1u);
    EXPECT_EQ(back[0].row.position, markers[0].row.position);
}

TEST(SchemaHelpers, ProjectionWithItsImage) {
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        sc::xray_detector::Attributes det;
        det.frame_id = "scan_detector";
        det.simulated = true;
        det.sensor_width = 4;
        det.sensor_height = 3;
        det.pixel_pitch_u_um = 100.0;
        det.pixel_pitch_v_um = 100.0;
        auto images = sc::xray_detector::add(scan, "detector_0", det)
                          .segmented_series("image", {{"height", 3}, {"width", 4}});
        sc::xray_detector::ImageRow image;
        for (int i = 0; i < 2; ++i) {
            image.time_ns = i;
            image.image = rs::Array(rs::DType::UInt16, {3, 4});
            static_cast<std::uint16_t*>(image.image.data())[5] =
                static_cast<std::uint16_t>(7 + i);
            sc::xray_detector::append(images, image);
        }
        sc::ct_acquisition::Attributes acq;
        acq.type = "ct_scan";
        acq.source = "sensors/source_0";
        acq.detector = "sensors/detector_0";
        acq.world_frame = "world";
        auto projections =
            sc::ct_acquisition::add(scan, "ct_0", acq).series("projections");
        sc::ct_acquisition::ProjectionsRow row;
        row.time_ns = 1;
        row.image_index = 1;
        row.voltage_kv = 120.0;
        sc::ct_acquisition::append(projections, row);
        scan.finalize();
    }
    const auto scan = rs::ScanReader::open(dir.path());
    const auto p =
        sc::ct_acquisition::read_projection(scan, scan.node("acquisitions/ct_0"), 0);
    EXPECT_EQ(p.geometry.image_index, 1u);
    EXPECT_DOUBLE_EQ(p.geometry.voltage_kv, 120.0);
    EXPECT_EQ(p.image.values<std::uint16_t>()[5], 8u);
    EXPECT_THROW((void)sc::ct_acquisition::read_projection(
                     scan, scan.node("acquisitions/ct_0"), 1),
                 rs::Error);
}
