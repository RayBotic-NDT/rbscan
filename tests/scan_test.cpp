#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <raybotic/scan/rbscan.hpp>

#include "test_util.hpp"

namespace rs = raybotic::scan;
using rbscan_test::TempDir;

namespace {

rs::Attributes robot_attributes() {
    return {{"joint_names", std::vector<std::string>{"j1", "j2", "j3", "j4", "j5", "j6"}},
            {"base_frame", std::string("source_robot_base")},
            {"tcp_frame", std::string("source_robot_tcp")},
            {"simulated", true},
            {"model", std::string("FANUC CRX-30iA")}};
}

}  // namespace

TEST(Scan, InlineSeriesWithBitshuffleLz4) {
    // Five samples of 6 float64 per chunk = 30 elements: a short block of 24
    // plus 6 raw leftover elements. Inline chunks hold several samples, so
    // reads go through HDF5 and the registered filter.
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto robot =
            scan.add_node("source_robot", "raybotic.robot_arm", robot_attributes());
        rs::InlineOptions opt;
        opt.chunk_bytes = 5 * 48;
        opt.compression.method = rs::Compression::BitshuffleLZ4;
        auto joints = robot.series("joint_states", {}, opt);
        for (int i = 0; i < 23; ++i) {
            std::vector<double> q(6, 0.1 * i - 1.0);
            joints.append(i, {{"position", rs::view(q)}});
        }
        scan.finalize();
    }
    auto joints = rs::ScanReader::open(dir.path())
                      .node("robots/source_robot")
                      .series("joint_states");
    ASSERT_EQ(joints.size(), 23u);
    const auto q = joints.read("position").values<double>();
    for (int i = 0; i < 23; ++i) EXPECT_DOUBLE_EQ(q[i * 6 + 5], 0.1 * i - 1.0) << i;
    EXPECT_TRUE(std::isnan(joints.read_sample("effort", 22).values<double>()[0]));
}

TEST(Scan, WritesAndReadsInlineSeries) {
    TempDir dir;
    {
        auto scan =
            rs::ScanWriter::create(dir.path(), {.description = "unit test",
                                                .operator_name = "ci",
                                                .sample_id = "part_42",
                                                .extra = {{"site", std::string("lab")}}});
        auto robot =
            scan.add_node("source_robot", "raybotic.robot_arm", robot_attributes());
        EXPECT_EQ(robot.path(), "robots/source_robot");

        // Shape [joint_names] is resolved from the attribute's length; small
        // chunks so the test crosses chunk borders.
        auto joints = robot.series("joint_states", {}, {.chunk_bytes = 48 * 4});
        EXPECT_EQ(joints.sample_shape("position"), (rs::Shape{6}));
        for (int i = 0; i < 25; ++i) {
            std::vector<double> q(6, 0.1 * i);
            std::vector<float> qd(6, 1.0f);  // converted to float64
            joints.append(1000 + i,
                          {{"position", rs::view(q)}, {"velocity", rs::view(qd)}});
        }
        EXPECT_EQ(joints.size(), 25u);

        auto tf = scan.add_node("scan_source", "raybotic.transform",
                                {{"parent", std::string("world")},
                                 {"child", std::string("scan_source")},
                                 {"static", false}});
        auto pose = tf.series("pose");
        const double t[3] = {0.1, 0.2, 0.3};
        const double r[4] = {0, 0, 0, 1};
        pose.append(5,
                    {{"translation", rs::view(t, {3})}, {"rotation", rs::view(r, {4})}});
        scan.finalize();
    }

    auto scan = rs::ScanReader::open(dir.path());
    EXPECT_TRUE(scan.complete());
    EXPECT_EQ(scan.format_version(), "1.0");
    const auto attrs = scan.attributes();
    EXPECT_EQ(std::get<std::string>(attrs.at("sample_id")), "part_42");
    EXPECT_EQ(std::get<std::string>(attrs.at("site")), "lab");
    EXPECT_EQ(std::get<std::string>(attrs.at("uuid")).size(), 36u);

    EXPECT_EQ(scan.node_paths(),
              (std::vector<std::string>{"robots/source_robot", "frames/scan_source"}));
    auto robot = scan.node("robots/source_robot");
    EXPECT_EQ(robot.schema().id(), "raybotic.robot_arm/1");
    EXPECT_EQ(std::get<std::vector<std::string>>(*robot.attribute("joint_names")).size(),
              6u);
    EXPECT_TRUE(std::get<bool>(*robot.attribute("simulated")));
    EXPECT_EQ(robot.series_names(), (std::vector<std::string>{"joint_states"}));

    auto joints = robot.series("joint_states");
    ASSERT_EQ(joints.size(), 25u);
    EXPECT_EQ(joints.units("position"), "rad");
    EXPECT_EQ(joints.time(24, 1).at(0), 1024);
    const auto q = joints.read("position", 20, 5);
    EXPECT_EQ(q.shape(), (rs::Shape{5, 6}));
    EXPECT_DOUBLE_EQ(q.values<double>()[6], 0.1 * 21);
    // effort was left out: optional, filled with NaN.
    EXPECT_TRUE(std::isnan(joints.read_sample("effort", 3).values<double>()[0]));
    EXPECT_EQ(joints.index_at(1010), 10u);
    EXPECT_EQ(joints.index_at(999), std::nullopt);
    EXPECT_EQ(joints.index_at(99999), 24u);

    auto pose = scan.node("frames/scan_source").series("pose");
    EXPECT_DOUBLE_EQ(pose.read_sample("translation", 0).values<double>()[2], 0.3);
}

TEST(Scan, ValidatesNodesAndSamples) {
    TempDir dir;
    auto scan = rs::ScanWriter::create(dir.path());
    // Missing required attribute.
    EXPECT_THROW(scan.add_node("r", "raybotic.robot_arm", {}), rs::Error);
    // Abstract base schema.
    EXPECT_THROW(scan.add_node("s", "raybotic.sensor",
                               {{"frame_id", std::string("x")}, {"simulated", true}}),
                 rs::Error);
    // Wrong attribute type.
    auto attrs = robot_attributes();
    attrs["simulated"] = std::string("yes");
    EXPECT_THROW(scan.add_node("r", "raybotic.robot_arm", attrs), rs::Error);

    auto robot = scan.add_node("r", "raybotic.robot_arm", robot_attributes());
    EXPECT_THROW(scan.add_node("r", "raybotic.robot_arm", robot_attributes()), rs::Error);
    auto tracking = robot.series("tracking");
    const std::uint8_t status = 1;
    const double err = 0.001;
    tracking.append(10, {{"status", rs::scalar(status)},
                         {"position_error", rs::scalar(err)},
                         {"orientation_error", rs::scalar(err)}});
    // Missing field, unknown field, wrong size, time going back.
    EXPECT_THROW(tracking.append(11, {{"status", rs::scalar(status)}}), rs::Error);
    EXPECT_THROW(tracking.append(11, {{"status", rs::scalar(status)},
                                      {"position_error", rs::scalar(err)},
                                      {"orientation_error", rs::scalar(err)},
                                      {"bogus", rs::scalar(err)}}),
                 rs::Error);
    const double two[2] = {1, 2};
    EXPECT_THROW(tracking.append(11, {{"status", rs::scalar(status)},
                                      {"position_error", rs::view(two, {2})},
                                      {"orientation_error", rs::scalar(err)}}),
                 rs::Error);
    EXPECT_THROW(tracking.append(9, {{"status", rs::scalar(status)},
                                     {"position_error", rs::scalar(err)},
                                     {"orientation_error", rs::scalar(err)}}),
                 rs::Error);
    // Segmented series through the inline API and vice versa.
    EXPECT_THROW((void)robot.segmented_series("tracking"), rs::Error);
    EXPECT_EQ(tracking.size(), 1u);
    scan.finalize();
    EXPECT_THROW(tracking.append(20, {{"status", rs::scalar(status)},
                                      {"position_error", rs::scalar(err)},
                                      {"orientation_error", rs::scalar(err)}}),
                 rs::Error);

    EXPECT_THROW((void)rs::ScanWriter::create(dir.path()), rs::Error);  // exists already
    EXPECT_NO_THROW((void)rs::ScanWriter::create(dir.path(), {}, true));
}

TEST(Scan, AttributesFollowTheSchemaType) {
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto src =
            scan.add_node("source_0", "raybotic.xray_source",
                          {{"frame_id", std::string("scan_source")},
                           {"simulated", true},
                           {"voltage_min_kv", std::int64_t{20}},  // int -> float64
                           {"voltage_max_kv", 225.0},
                           {"current_max_ua", 1000.0},
                           {"power_max_w", 320.0},
                           {"focal_spots_um", std::vector<std::int64_t>{5, 20, 80}},
                           {"x_custom", std::string("allowed")}});
        src.set_attribute("filter_material", std::string("Cu"));
        EXPECT_THROW(src.set_attribute("voltage_max_kv", std::string("high")), rs::Error);
    }
    auto node = rs::ScanReader::open(dir.path()).node("sensors/source_0");
    EXPECT_DOUBLE_EQ(std::get<double>(*node.attribute("voltage_min_kv")), 20.0);
    EXPECT_EQ(std::get<std::vector<double>>(*node.attribute("focal_spots_um")).size(),
              3u);
    EXPECT_EQ(std::get<std::string>(*node.attribute("filter_material")), "Cu");
    EXPECT_EQ(std::get<std::string>(*node.attribute("x_custom")), "allowed");
    EXPECT_FALSE(node.attributes().contains("schema_json"));
}

TEST(Scan, OpenExistingScanToAddAndReplaceNodes) {
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        (void)scan.add_node("source_robot", "raybotic.robot_arm", robot_attributes());
    }
    auto document = [](const char* text) {
        return rs::Attributes{{"application", std::string("test")},
                              {"format", std::string("test/1")},
                              {"document", std::string(text)}};
    };
    for (const char* text : {"first", "second"}) {
        auto scan = rs::ScanWriter::open(dir.path());
        if (scan.has_node("annotations/viewer")) scan.remove_node("annotations/viewer");
        auto node = scan.add_node("viewer", "raybotic.annotations", document(text));
        const double pos[3] = {0.01, 0.02, 0.03};
        const std::uint32_t id = 7, n = 2;
        const std::uint8_t src = 1, quality = 3;
        node.series("markers").append(1, {{"region_id", rs::scalar(id)},
                                          {"marker_id", rs::scalar(id)},
                                          {"position", rs::view(pos, {3})},
                                          {"position_source", rs::scalar(src)},
                                          {"quality", rs::scalar(quality)},
                                          {"observations", rs::scalar(n)}});
        EXPECT_THROW(scan.remove_node("annotations/viewer"),
                     rs::Error);  // added just now
        scan.finalize();
    }
    const auto reader = rs::ScanReader::open(dir.path());
    EXPECT_TRUE(reader.complete());
    EXPECT_TRUE(reader.has_node("robots/source_robot"));
    const auto node = reader.node("annotations/viewer");
    EXPECT_EQ(std::get<std::string>(*node.attribute("document")), "second");
    EXPECT_EQ(node.series("markers").size(), 1u);
}

TEST(Scan, TrajectoryPlanTables) {
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto plan = scan.add_node("plan", "raybotic.trajectory_plan",
                                  {{"application", std::string("test")}});
        EXPECT_EQ(plan.path(), "trajectories/plan");
        const double centre[3] = {0.0, 0.0, 0.1};
        const double axis[3] = {0.0, 0.0, 1.0};
        const double start[3] = {1.0, 0.0, 0.0};
        const std::uint8_t kind = 1;
        const std::uint32_t projections = 2;
        const std::uint64_t first = 0;
        const std::uint64_t count = 2;
        const bool ccw = true;
        plan.series("trajectories")
            .append(1, {{"kind", rs::scalar(kind)},
                        {"centre", rs::view(centre, {3})},
                        {"axis", rs::view(axis, {3})},
                        {"start_direction", rs::view(start, {3})},
                        {"tilt", rs::scalar(30.0)},
                        {"fod", rs::scalar(0.3)},
                        {"fdd", rs::scalar(0.6)},
                        {"projections", rs::scalar(projections)},
                        {"start_angle", rs::scalar(0.0)},
                        {"range", rs::scalar(360.0)},
                        {"counter_clockwise", rs::scalar(ccw)},
                        {"first_pose", rs::scalar(first)},
                        {"pose_count", rs::scalar(count)}});
        auto poses = plan.series("poses");
        const double position[3] = {0.0, 0.0, 0.1};
        const double rotation[4] = {0.0, 0.0, 0.0, 1.0};
        for (int i = 0; i < 2; ++i) {
            const std::uint32_t trajectory = 0;
            const bool skip = i == 1;
            const bool manual = false;
            poses.append(1, {{"trajectory", rs::scalar(trajectory)},
                             {"angle", rs::scalar(180.0 * i)},
                             {"skip", rs::scalar(skip)},
                             {"manual", rs::scalar(manual)},
                             {"roi_position", rs::view(position, {3})},
                             {"roi_rotation", rs::view(rotation, {4})},
                             {"source_position", rs::view(position, {3})},
                             {"source_rotation", rs::view(rotation, {4})},
                             {"detector_position", rs::view(position, {3})},
                             {"detector_rotation", rs::view(rotation, {4})}});
        }
        EXPECT_THROW(scan.add_node("bad", "raybotic.trajectory_plan", {}), rs::Error);
        scan.finalize();
    }
    const auto reader = rs::ScanReader::open(dir.path());
    const auto plan = reader.node("trajectories/plan");
    EXPECT_EQ(plan.series("trajectories").size(), 1u);
    EXPECT_EQ(
        plan.series("trajectories").read_sample("kind", 0).values<std::uint8_t>()[0], 1u);
    EXPECT_EQ(plan.series("poses").size(), 2u);
    EXPECT_DOUBLE_EQ(plan.series("poses").read_sample("angle", 1).values<double>()[0],
                     180.0);
    EXPECT_TRUE(rs::validate(dir.path()).ok()) << rs::to_text(rs::validate(dir.path()));
}

TEST(Scan, SimulationSceneKeepsTheMeshBitForBit) {
    TempDir dir;
    // A tetrahedron with values a text format or a unit conversion would
    // change: -0, a denormal, 0.1 and the largest float.
    const float vertices[4][3] = {{-0.0f, 1e-40f, 0.1f},
                                  {3.4028235e38f, 0.0f, -2.5f},
                                  {0.0f, 7.0f, 1.0f / 3.0f},
                                  {1.0f, 1.0f, 1.0f}};
    const std::uint32_t triangles[4][3] = {{0, 1, 2}, {0, 3, 1}, {1, 3, 2}, {2, 3, 0}};
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto scene = scan.add_node(
            "scene", "raybotic.simulation_scene",
            {{"application", std::string("test")},
             {"format", std::string("raybotic-simulation-scene/1")},
             {"document", std::string("{}")},
             {"object_names", std::vector<std::string>{"D\xC3\xBCse"}},
             {"object_sources", std::vector<std::string>{"C:/stl/duese.stl"}},
             {"object_materials", std::vector<std::string>{"Ti"}},
             {"registration_markers", std::vector<std::string>{"M1"}},
             {"voltage_kv", 160.0},
             {"binning", std::int64_t{2}}});
        EXPECT_EQ(scene.path(), "simulations/scene");
        const double rotation[3] = {0.0, 90.0, -2.5};
        const double position[3] = {1.25, -3.0, 40.0};
        const float colour[3] = {0.94f, 0.78f, 0.35f};
        const std::uint8_t unit = 2;
        const bool no = false;
        const bool yes = true;
        const std::uint64_t zero = 0;
        const std::uint64_t one = 1;
        const std::uint64_t four = 4;
        scene.series("objects").append(1, {{"from_volume", rs::scalar(no)},
                                           {"unit", rs::scalar(unit)},
                                           {"scale", rs::scalar(1.5)},
                                           {"rotation", rs::view(rotation, {3})},
                                           {"position", rs::view(position, {3})},
                                           {"is_compound", rs::scalar(no)},
                                           {"density", rs::scalar(4.5)},
                                           {"enabled", rs::scalar(yes)},
                                           {"colour", rs::view(colour, {3})},
                                           {"first_vertex", rs::scalar(zero)},
                                           {"vertex_count", rs::scalar(four)},
                                           {"first_triangle", rs::scalar(zero)},
                                           {"triangle_count", rs::scalar(four)},
                                           {"first_registration", rs::scalar(zero)},
                                           {"registration_count", rs::scalar(one)}});
        auto v = scene.series("vertices");
        for (const auto& p : vertices) v.append(1, {{"position", rs::view(p, {3})}});
        auto t = scene.series("triangles");
        for (const auto& c : triangles) t.append(1, {{"vertices", rs::view(c, {3})}});
        const double point[3] = {0.5, 0.25, 0.0};
        const std::int32_t marker = -1;
        const std::int8_t axis = 2;
        scene.series("registration")
            .append(1, {{"marker_id", rs::scalar(marker)},
                        {"placed", rs::scalar(yes)},
                        {"mesh_point", rs::view(point, {3})},
                        {"section_axis", rs::scalar(axis)},
                        {"section_offset", rs::scalar(-0.75)}});
        EXPECT_THROW(scan.add_node("bad", "raybotic.simulation_scene",
                                   {{"application", std::string("test")}}),
                     rs::Error);
        scan.finalize();
    }
    const auto reader = rs::ScanReader::open(dir.path());
    const auto scene = reader.node("simulations/scene");
    EXPECT_EQ(scene.schema().id(), "raybotic.simulation_scene/1");
    EXPECT_EQ(std::get<std::vector<std::string>>(*scene.attribute("object_names"))[0],
              "D\xC3\xBCse");
    EXPECT_EQ(std::get<std::int64_t>(*scene.attribute("binning")), 2);
    EXPECT_FALSE(scene.attribute("sensor_width").has_value());

    const auto stored = scene.series("vertices").read("position");
    const auto floats = stored.values<float>();
    ASSERT_EQ(floats.size(), 12u);
    EXPECT_EQ(std::memcmp(floats.data(), vertices, sizeof(vertices)), 0);
    EXPECT_TRUE(std::signbit(floats[0]));
    const auto corners =
        scene.series("triangles").read("vertices").values<std::uint32_t>();
    ASSERT_EQ(corners.size(), 12u);
    EXPECT_EQ(std::memcmp(corners.data(), triangles, sizeof(triangles)), 0);

    const auto objects = scene.series("objects");
    EXPECT_EQ(objects.units("position"), "mm");
    EXPECT_DOUBLE_EQ(objects.read_sample("position", 0).values<double>()[2], 40.0);
    EXPECT_EQ(objects.read_sample("triangle_count", 0).values<std::uint64_t>()[0], 4u);
    const auto registration = scene.series("registration");
    EXPECT_EQ(registration.read_sample("marker_id", 0).values<std::int32_t>()[0], -1);
    EXPECT_EQ(registration.read_sample("section_axis", 0).values<std::int8_t>()[0], 2);
    EXPECT_TRUE(rs::validate(dir.path()).ok()) << rs::to_text(rs::validate(dir.path()));
}

TEST(Scan, LaterSessionsAppendToAnInlineSeries) {
    TempDir dir;
    // Small chunks, so sessions end inside a chunk and across chunk borders.
    rs::InlineOptions small;
    small.chunk_bytes = 64;
    auto row = [](rs::SeriesWriter& s, int i) {
        const double v[3] = {1.0 * i, 2.0 * i, 3.0 * i};
        const double q[4] = {0.0, 0.0, 0.0, 1.0};
        s.append(1000 * i,
                 {{"translation", rs::view(v, {3})}, {"rotation", rs::view(q, {4})}});
    };
    int next = 0;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto tf = scan.add_node("tool", "raybotic.transform",
                                {{"parent", std::string("world")},
                                 {"child", std::string("tool")},
                                 {"static", false}});
        auto pose = tf.series("pose", {}, small);
        for (; next < 3; ++next) row(pose, next);
        scan.finalize();
    }
    for (int session = 0; session < 3; ++session) {
        auto scan = rs::ScanWriter::open(dir.path());
        auto pose = scan.node("frames/tool").series("pose");
        EXPECT_EQ(pose.size(), static_cast<std::uint64_t>(next));
        EXPECT_THROW(row(pose, 0), rs::Error);  // time must not decrease
        for (int k = 0; k < 2 + session; ++k, ++next) row(pose, next);
        scan.finalize();
    }
    const auto reader = rs::ScanReader::open(dir.path());
    const auto pose = reader.node("frames/tool").series("pose");
    ASSERT_EQ(pose.size(), static_cast<std::uint64_t>(next));
    const auto time = pose.time();
    const rs::Array t = pose.read("translation");
    const auto v = t.values<double>();
    for (int i = 0; i < next; ++i) {
        EXPECT_EQ(time[static_cast<size_t>(i)], 1000 * i);
        EXPECT_DOUBLE_EQ(v[3 * static_cast<size_t>(i) + 2], 3.0 * i) << "row " << i;
    }
    EXPECT_TRUE(rs::validate(dir.path()).ok()) << rs::to_text(rs::validate(dir.path()));
}

TEST(Scan, ReconstructionSettingsAndRunStatus) {
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto run = scan.add_node("run_1", "raybotic.reconstruction",
                                 {{"config", std::string(R"({"version": "1.0"})")},
                                  {"config_format", std::string("raybotic.reco/1.0")},
                                  {"algorithm", std::string("fbp")},
                                  {"roi_names", std::vector<std::string>{"volume"}},
                                  {"status", std::string("running")}});
        EXPECT_EQ(run.path(), "reconstructions/run_1");
        const bool enabled = true;
        const std::uint32_t dims[3] = {64, 64, 32};
        const double voxel[3] = {1e-4, 1e-4, 2e-4};
        const double mid[3] = {0.0, 0.01, -0.02};
        run.series("rois").append(1, {{"enabled", rs::scalar(enabled)},
                                      {"dims", rs::view(dims, {3})},
                                      {"voxel_size", rs::view(voxel, {3})},
                                      {"midpoint", rs::view(mid, {3})}});
        EXPECT_THROW(scan.add_node("bad", "raybotic.reconstruction", {}),
                     rs::Error);  // no config
        scan.finalize();
    }
    {
        // A later session updates the run's status; its series can only go on.
        auto scan = rs::ScanWriter::open(dir.path());
        auto run = scan.node("reconstructions/run_1");
        run.set_attribute("status", std::string("ok"));
        run.set_attribute("volume", std::string("volumes/reco"));
        EXPECT_EQ(run.series("rois").size(), 1u);
        EXPECT_THROW((void)scan.node("reconstructions/missing"), rs::Error);
        scan.finalize();
    }
    const auto reader = rs::ScanReader::open(dir.path());
    const auto run = reader.node("reconstructions/run_1");
    EXPECT_EQ(std::get<std::string>(*run.attribute("status")), "ok");
    EXPECT_EQ(std::get<std::string>(*run.attribute("volume")), "volumes/reco");
    EXPECT_EQ(std::get<std::string>(*run.attribute("algorithm")), "fbp");
    EXPECT_EQ(run.series("rois").read_sample("dims", 0).values<std::uint32_t>()[2], 32u);
    EXPECT_TRUE(rs::validate(dir.path()).ok()) << rs::to_text(rs::validate(dir.path()));
}

TEST(Scan, AppendManyWritesWholeColumns) {
    TempDir dir;
    const std::vector<float> positions = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::uint32_t> corners = {0, 1, 2};
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto scene = scan.add_node("scene", "raybotic.simulation_scene",
                                   {{"application", std::string("test")},
                                    {"format", std::string("x/1")},
                                    {"document", std::string("{}")}});
        auto vertices = scene.series("vertices");
        vertices.append_many(5, 3, {{"position", rs::view(positions)}});
        const std::vector<std::int64_t> times = {7, 8};
        // Converted per sample like append(): float64 into a float32 field.
        const std::vector<double> more = {9, 10, 11, 12, 13, 14};
        vertices.append_many(times, {{"position", rs::view(more)}});
        scene.series("triangles").append_many(5, 1, {{"vertices", rs::view(corners)}});
        EXPECT_THROW(vertices.append_many(9, 2, {{"position", rs::view(positions)}}),
                     rs::Error);  // 9 values are not 2 samples of 3
        scan.finalize();
    }
    const auto node = rs::ScanReader::open(dir.path()).node("simulations/scene");
    const auto vertices = node.series("vertices");
    ASSERT_EQ(vertices.size(), 5u);
    EXPECT_EQ(vertices.time(), (std::vector<std::int64_t>{5, 5, 5, 7, 8}));
    const auto stored = vertices.read("position").values<float>();
    EXPECT_FLOAT_EQ(stored[8], 8.0f);
    EXPECT_FLOAT_EQ(stored[14], 14.0f);
}
