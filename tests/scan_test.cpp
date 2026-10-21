#include <gtest/gtest.h>

#include <cmath>
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

TEST(Scan, WritesAndReadsInlineSeries) {
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path(), {.description = "unit test",
                                                        .operator_name = "ci",
                                                        .sample_id = "part_42",
                                                        .extra = {{"site", std::string("lab")}}});
        auto robot = scan.add_node("source_robot", "raybotic.robot_arm", robot_attributes());
        EXPECT_EQ(robot.path(), "robots/source_robot");

        // Shape [joint_names] is resolved from the attribute's length; small
        // chunks so the test crosses chunk borders.
        auto joints = robot.series("joint_states", {}, {.chunk_bytes = 48 * 4});
        EXPECT_EQ(joints.sample_shape("position"), (rs::Shape{6}));
        for (int i = 0; i < 25; ++i) {
            std::vector<double> q(6, 0.1 * i);
            std::vector<float> qd(6, 1.0f);  // converted to float64
            joints.append(1000 + i, {{"position", rs::view(q)}, {"velocity", rs::view(qd)}});
        }
        EXPECT_EQ(joints.size(), 25u);

        auto tf = scan.add_node("scan_source", "raybotic.transform",
                                {{"parent", std::string("world")},
                                 {"child", std::string("scan_source")},
                                 {"static", false}});
        auto pose = tf.series("pose");
        const double t[3] = {0.1, 0.2, 0.3};
        const double r[4] = {0, 0, 0, 1};
        pose.append(5, {{"translation", rs::view(t, {3})}, {"rotation", rs::view(r, {4})}});
        scan.finalize();
    }

    auto scan = rs::ScanReader::open(dir.path());
    EXPECT_TRUE(scan.complete());
    EXPECT_EQ(scan.format_version(), "1.0");
    const auto attrs = scan.attributes();
    EXPECT_EQ(std::get<std::string>(attrs.at("sample_id")), "part_42");
    EXPECT_EQ(std::get<std::string>(attrs.at("site")), "lab");
    EXPECT_EQ(std::get<std::string>(attrs.at("uuid")).size(), 36u);

    EXPECT_EQ(scan.node_paths(), (std::vector<std::string>{"robots/source_robot",
                                                           "frames/scan_source"}));
    auto robot = scan.node("robots/source_robot");
    EXPECT_EQ(robot.schema().id(), "raybotic.robot_arm/1");
    EXPECT_EQ(std::get<std::vector<std::string>>(*robot.attribute("joint_names")).size(), 6u);
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
        auto src = scan.add_node("source_0", "raybotic.xray_source",
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
    EXPECT_EQ(std::get<std::vector<double>>(*node.attribute("focal_spots_um")).size(), 3u);
    EXPECT_EQ(std::get<std::string>(*node.attribute("filter_material")), "Cu");
    EXPECT_EQ(std::get<std::string>(*node.attribute("x_custom")), "allowed");
    EXPECT_FALSE(node.attributes().contains("schema_json"));
}
