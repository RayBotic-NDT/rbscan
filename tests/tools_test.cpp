#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <raybotic/scan/rbscan.hpp>

#include "test_util.hpp"

namespace rs = raybotic::scan;
using rbscan_test::TempDir;

namespace {

using V3 = std::array<double, 3>;

constexpr double fod = 0.4;
constexpr double fdd = 1.0;
constexpr std::uint64_t W = 40;
constexpr std::uint64_t H = 30;
constexpr std::int64_t frame_period = 100;  // frames every 100 ns
constexpr std::int64_t image_period = 250;  // images every 250 ns (between frames)

/// Scan frame turned by theta about world z: y up, z along the beam axis.
std::array<double, 4> frame_rotation(double theta) {
    const double c = std::cos(theta / 2), s = std::sin(theta / 2);
    return {0.5 * (c - s), 0.5 * (c + s), 0.5 * (c + s), 0.5 * (c - s)};
}

double angle_at(std::int64_t t) { return 1e-5 * static_cast<double>(t); }

/// A circular scan: frames, source state (emission off at the start), detector
/// with binning 2, a robot that is unreachable for a while, images. The last
/// image lies after the last frame sample.
void write_scan(const std::filesystem::path& dir, int images = 12) {
    auto scan = rs::ScanWriter::create(dir);
    auto source = scan.add_node("source_0", "raybotic.xray_source",
                                {{"frame_id", std::string("scan_source")},
                                 {"simulated", true},
                                 {"voltage_min_kv", 20.0},
                                 {"voltage_max_kv", 225.0},
                                 {"current_max_ua", 1000.0},
                                 {"power_max_w", 320.0}});
    auto detector = scan.add_node("detector_0", "raybotic.xray_detector",
                                  {{"frame_id", std::string("scan_detector")},
                                   {"simulated", true},
                                   {"sensor_width", std::int64_t{2 * W}},
                                   {"sensor_height", std::int64_t{2 * H}},
                                   {"pixel_pitch_u_um", 100.0},
                                   {"pixel_pitch_v_um", 100.0}});
    auto robot = scan.add_node("detector_robot", "raybotic.robot_arm",
                               {{"joint_names", std::vector<std::string>{"j1"}},
                                {"base_frame", std::string("base")},
                                {"tcp_frame", std::string("tcp")},
                                {"target_frame", std::string("scan_detector")},
                                {"simulated", true}});

    auto add_state = [](rs::SeriesWriter& s, std::int64_t t, std::uint8_t emission) {
        const bool permitted = true;
        const double kv = 120.0, ua = 50.0, spot = 5.0, power = 6.0;
        s.append(t, {{"emission_state", rs::scalar(emission)},
                     {"emission_permitted", rs::scalar(permitted)},
                     {"voltage_setpoint_kv", rs::scalar(kv)},
                     {"current_setpoint_ua", rs::scalar(ua)},
                     {"focal_spot_um", rs::scalar(spot)},
                     {"voltage_kv", rs::scalar(kv)},
                     {"current_ua", rs::scalar(ua)},
                     {"power_w", rs::scalar(power)}});
    };
    auto src_state = source.series("state");
    add_state(src_state, 0, 1);    // starting
    add_state(src_state, 400, 2);  // on from t = 400

    {
        const std::uint8_t binning = 2;
        const std::uint32_t w = W, h = H, averages = 1;
        const double ms = 100.0, gain = 1.0;
        detector.series("state").append(0, {{"binning", rs::scalar(binning)},
                                            {"image_width", rs::scalar(w)},
                                            {"image_height", rs::scalar(h)},
                                            {"integration_time_ms", rs::scalar(ms)},
                                            {"averages", rs::scalar(averages)},
                                            {"gain", rs::scalar(gain)}});
    }
    {
        auto tracking = robot.series("tracking");
        const double e = 0.0;
        for (auto [t, status] :
             {std::pair<std::int64_t, std::uint8_t>{0, 1}, {1000, 3}, {1600, 1}}) {
            tracking.append(t, {{"status", rs::scalar(status)},
                                {"position_error", rs::scalar(e)},
                                {"orientation_error", rs::scalar(e)}});
        }
    }

    auto tf = [&](const char* child) {
        return scan
            .add_node(child, "raybotic.transform",
                      {{"parent", std::string("world")},
                       {"child", std::string(child)},
                       {"static", false}})
            .series("pose");
    };
    auto src_tf = tf("scan_source");
    auto det_tf = tf("scan_detector");
    const std::int64_t last_frame = (images - 2) * image_period;
    for (std::int64_t t = 0; t <= last_frame; t += frame_period) {
        const double a = angle_at(t);
        const V3 s{fod * std::cos(a), fod * std::sin(a), 0.0};
        const V3 d{-(fdd - fod) * std::cos(a), -(fdd - fod) * std::sin(a), 0.0};
        const auto qs = frame_rotation(a + std::numbers::pi);
        const auto qd = frame_rotation(a);
        src_tf.append(t, {{"translation", rs::view(s.data(), {3})},
                          {"rotation", rs::view(qs.data(), {4})}});
        det_tf.append(t, {{"translation", rs::view(d.data(), {3})},
                          {"rotation", rs::view(qd.data(), {4})}});
    }

    auto img = detector.segmented_series("image", {{"height", H}, {"width", W}},
                                         {.segment_size = 5});
    std::vector<std::uint16_t> px(W * H, 1000);
    for (int i = 0; i < images; ++i) {
        img.append(i * image_period, {{"image", rs::view(px.data(), {H, W})}});
    }
    scan.finalize();
}

std::vector<double> doubles(const rs::SeriesReader& s, const char* field) {
    const auto a = s.read(field);
    const auto v = a.values<double>();
    return {v.begin(), v.end()};
}

}  // namespace

TEST(Geometry, ProjectionMatrixMatchesRayIntersection) {
    const V3 s{0.3, -0.2, 0.1};
    const V3 d{-0.5, 0.4, -0.05};
    // Detector axes: some orthonormal pair, not aligned with the beam.
    const V3 u{0.6, 0.8, 0.0};
    const V3 v{0.0, 0.0, -1.0};
    const double pu = 2e-4, pv = 3e-4;
    const auto P = rs::projection_matrix(s, d, u, v, pu, pv, W, H);
    const V3 n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
               u[0] * v[1] - u[1] * v[0]};
    auto dot = [](const V3& a, const V3& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    for (const V3& x : {V3{0, 0, 0}, V3{0.05, -0.02, 0.03}, V3{-0.1, 0.1, -0.02}}) {
        const V3 dir{x[0] - s[0], x[1] - s[1], x[2] - s[2]};
        const double lambda =
            dot(V3{d[0] - s[0], d[1] - s[1], d[2] - s[2]}, n) / dot(dir, n);
        const V3 p{s[0] + lambda * dir[0] - d[0], s[1] + lambda * dir[1] - d[1],
                   s[2] + lambda * dir[2] - d[2]};
        const double col = dot(p, u) / pu + 0.5 * (W - 1);
        const double row = dot(p, v) / pv + 0.5 * (H - 1);
        const double hx = P[0] * x[0] + P[1] * x[1] + P[2] * x[2] + P[3];
        const double hy = P[4] * x[0] + P[5] * x[1] + P[6] * x[2] + P[7];
        const double hw = P[8] * x[0] + P[9] * x[1] + P[10] * x[2] + P[11];
        EXPECT_NEAR(hx / hw, col, 1e-9);
        EXPECT_NEAR(hy / hw, row, 1e-9);
    }
}

TEST(Geometry, FrameGraphInterpolatesAndChains) {
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto add = [&](const char* name, const char* parent, bool is_static) {
            return scan
                .add_node(name, "raybotic.transform",
                          {{"parent", std::string(parent)},
                           {"child", std::string(name)},
                           {"static", is_static}})
                .series("pose");
        };
        auto base = add("base", "world", true);
        auto tool = add("tool", "base", false);
        auto cam = add("cam", "world", true);
        const double t_base[3] = {1, 0, 0}, id[4] = {0, 0, 0, 1};
        const double rz90[4] = {0, 0, std::sin(std::numbers::pi / 4),
                                std::cos(std::numbers::pi / 4)};
        base.append(0, {{"translation", rs::view(t_base, {3})},
                        {"rotation", rs::view(rz90, {4})}});
        const double t0[3] = {0, 0, 0}, t1[3] = {0, 2, 0};
        tool.append(
            0, {{"translation", rs::view(t0, {3})}, {"rotation", rs::view(id, {4})}});
        tool.append(
            100, {{"translation", rs::view(t1, {3})}, {"rotation", rs::view(rz90, {4})}});
        const double t_cam[3] = {0, 0, 5};
        cam.append(
            0, {{"translation", rs::view(t_cam, {3})}, {"rotation", rs::view(id, {4})}});
    }
    const auto g = rs::FrameGraph::load(dir.path());
    EXPECT_TRUE(g.has_frame("world"));
    EXPECT_EQ(g.frames(), (std::vector<std::string>{"base", "cam", "tool", "world"}));

    bool ex = false;
    // Halfway: tool at (0, 1, 0) in base, base turned 90 deg about z at (1, 0, 0).
    const auto p = g.lookup("world", "tool", 50, &ex);
    EXPECT_FALSE(ex);
    EXPECT_NEAR(p.translation[0], 0.0, 1e-12);
    EXPECT_NEAR(p.translation[1], 0.0, 1e-12);
    // Rotation: 90 deg (base) + 45 deg (slerp halfway) = 135 deg about z.
    EXPECT_NEAR(p.rotation[2], std::sin(135.0 / 2 * std::numbers::pi / 180), 1e-9);

    // Between siblings: tool in cam.
    const auto q = g.lookup("cam", "tool", 100, &ex);
    EXPECT_NEAR(q.translation[0], 1.0 - 2.0, 1e-12);
    EXPECT_NEAR(q.translation[2], -5.0, 1e-12);

    (void)g.lookup("world", "tool", 500, &ex);
    EXPECT_TRUE(ex);
    EXPECT_THROW((void)g.lookup("world", "nowhere_else", 0), rs::Error);
}

TEST(Geometry, ResolveGeometryFromFrames) {
    TempDir dir;
    write_scan(dir.path());
    const auto r = rs::resolve_geometry(dir.path());
    EXPECT_EQ(r.acquisition, "acquisitions/scan_0");
    EXPECT_EQ(r.projections, 12u);
    EXPECT_EQ(r.extrapolated, 1u);       // last image after the last frame
    EXPECT_EQ(r.emission_unstable, 2u);  // images at t = 0, 250 before emission on
    EXPECT_EQ(r.robot_unreachable, 3u);  // t = 1000, 1250, 1500
    EXPECT_GT(r.interpolated, 0u);

    {  // the reader must be closed before the scan is written again
        const auto scan = rs::ScanReader::open(dir.path());
        const auto acq = scan.node("acquisitions/scan_0");
        EXPECT_EQ(std::get<std::string>(*acq.attribute("detector")),
                  "sensors/detector_0");
        const auto p = acq.series("projections");
        ASSERT_EQ(p.size(), 12u);
        const auto src = doubles(p, "source_position");
        const auto det = doubles(p, "detector_position");
        const auto u = doubles(p, "detector_u");
        const auto v = doubles(p, "detector_v");
        const auto px = doubles(p, "pixel_size");
        const auto pm = doubles(p, "projection_matrix");
        const auto kv = doubles(p, "voltage_kv");
        const auto flags = p.read("flags");
        for (std::size_t i = 0; i < 11; ++i) {
            // Images between two frame samples are interpolated (chord, not arc).
            const std::int64_t t = static_cast<std::int64_t>(i) * image_period;
            const double a = angle_at(t);
            const double tol = (t % frame_period == 0) ? 1e-12 : 1e-6;
            EXPECT_NEAR(src[3 * i], fod * std::cos(a), tol) << i;
            EXPECT_NEAR(det[3 * i + 1], -(fdd - fod) * std::sin(a), tol) << i;
            EXPECT_NEAR(u[3 * i], -std::sin(a), tol) << i;  // image right
            EXPECT_NEAR(u[3 * i + 1], std::cos(a), tol) << i;
            EXPECT_NEAR(v[3 * i + 2], -1.0, 1e-9) << i;  // rows run downwards
            EXPECT_DOUBLE_EQ(px[2 * i], 200e-6);         // 100 um * binning 2
            // The rotation centre projects to the image centre.
            const double hw = pm[12 * i + 11];
            EXPECT_NEAR(pm[12 * i + 3] / hw, 0.5 * (W - 1), 1e-6) << i;
            EXPECT_NEAR(pm[12 * i + 7] / hw, 0.5 * (H - 1), 1e-6) << i;
        }
        EXPECT_DOUBLE_EQ(kv[0], 120.0);
        EXPECT_EQ(flags.values<std::uint32_t>()[0] & 8u, 8u);   // emission starting
        EXPECT_EQ(flags.values<std::uint32_t>()[11] & 1u, 1u);  // extrapolated
    }
    EXPECT_THROW(rs::resolve_geometry(dir.path()), rs::Error);  // exists
    rs::GeometryOptions o;
    o.overwrite = true;
    o.projection_matrix = false;
    EXPECT_NO_THROW(rs::resolve_geometry(dir.path(), o));
    const auto pm2 = rs::ScanReader::open(dir.path())
                         .node("acquisitions/scan_0")
                         .series("projections")
                         .read_sample("projection_matrix", 0);
    EXPECT_TRUE(std::isnan(pm2.values<double>()[0]));
}

TEST(Tools, SummarizeCountsSamplesAndBytes) {
    TempDir dir;
    write_scan(dir.path());
    const auto s = rs::summarize(dir.path());
    EXPECT_TRUE(s.complete);
    EXPECT_GT(s.raw_bytes, 12u * W * H * 2);
    EXPECT_GT(s.stored_bytes, 0u);
    const auto it = std::find_if(s.nodes.begin(), s.nodes.end(), [](const auto& n) {
        return n.path == "sensors/detector_0";
    });
    ASSERT_NE(it, s.nodes.end());
    const auto& image = it->series.front();
    EXPECT_EQ(image.name, "image");
    EXPECT_EQ(image.samples, 12u);
    EXPECT_EQ(image.segments, 3u);
    EXPECT_NEAR(image.rate_hz, 1e9 / image_period, 1e-3);
    EXPECT_EQ(image.fields.front().sample_shape, (rs::Shape{H, W}));
    EXPECT_NE(rs::to_text(s).find("sensors/detector_0"), std::string::npos);
    EXPECT_NE(rs::to_json(s).find("\"segments\": 3"), std::string::npos);
}

TEST(Tools, ValidatePassesAGoodScan) {
    TempDir dir;
    write_scan(dir.path());
    rs::resolve_geometry(dir.path());
    const auto r = rs::validate(dir.path());
    EXPECT_TRUE(r.ok()) << rs::to_text(r);
    // The one image after the last frame is flagged invalid: a warning.
    EXPECT_EQ(r.count(rs::Severity::Warning), 1u) << rs::to_text(r);
}

TEST(Tools, ValidateFindsBrokenSegmentsAndReferences) {
    TempDir dir;
    write_scan(dir.path());
    rs::resolve_geometry(dir.path());

    // 1) a segment added after finalize is not linked.
    {
        auto w = rs::SegmentedSeriesWriter::open(dir.path(), "sensors/camera_0",
                                                 "raybotic.camera", "image", {}, {},
                                                 {{"frame_id", std::string("cam")},
                                                  {"simulated", true},
                                                  {"width", std::int64_t{2}},
                                                  {"height", std::int64_t{2}},
                                                  {"channels", std::int64_t{1}},
                                                  {"encoding", std::string("mono8")}});
        const std::uint8_t px[4] = {};
        w.append(0, {{"image", rs::view(px, {2, 2, 1})}});
    }
    // 2) a detector segment removed in the middle.
    std::filesystem::remove(dir.path() / "sensors/detector_0/image/seg_000001.h5");

    const auto r = rs::validate(dir.path());
    EXPECT_FALSE(r.ok());
    const auto text = rs::to_text(r);
    EXPECT_NE(text.find("out of sequence"), std::string::npos) << text;
    EXPECT_NE(
        text.find("sensors/camera_0: has segments on disk but is not in the master"),
        std::string::npos)
        << text;
    EXPECT_NE(text.find("image_index values beyond"), std::string::npos) << text;
    EXPECT_NE(rs::to_json(r).find("\"ok\": false"), std::string::npos);
}
