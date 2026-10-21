#include <gtest/gtest.h>

#include <raybotic/scan/rbscan.hpp>
#include <thread>

#include "test_util.hpp"

namespace rs = raybotic::scan;
using rbscan_test::TempDir;

namespace {

constexpr std::uint64_t H = 48;
constexpr std::uint64_t W = 64;

rs::Attributes detector_attributes() {
    return {{"frame_id", std::string("scan_detector")},
            {"simulated", true},
            {"sensor_width", std::int64_t{W}},
            {"sensor_height", std::int64_t{H}},
            {"pixel_pitch_u_um", 139.0},
            {"pixel_pitch_v_um", 139.0}};
}

/// Pixel value that encodes sample and position, to catch any mix-up.
std::uint16_t pixel(std::uint64_t i, std::uint64_t y, std::uint64_t x) {
    return static_cast<std::uint16_t>((i * 7 + y * 3 + x) & 0xFFFF);
}

std::vector<std::uint16_t> image(std::uint64_t i) {
    std::vector<std::uint16_t> img(H * W);
    for (std::uint64_t y = 0; y < H; ++y) {
        for (std::uint64_t x = 0; x < W; ++x) img[y * W + x] = pixel(i, y, x);
    }
    return img;
}

void check_images(const rs::SeriesReader& s, std::uint64_t n) {
    ASSERT_EQ(s.size(), n);
    const auto all = s.read("image");
    ASSERT_EQ(all.shape(), (rs::Shape{n, H, W}));
    const auto px = all.values<std::uint16_t>();
    for (std::uint64_t i = 0; i < n; ++i) {
        ASSERT_EQ(px[i * H * W + 5 * W + 9], pixel(i, 5, 9)) << "sample " << i;
        ASSERT_EQ(px[i * H * W + (H - 1) * W + (W - 1)], pixel(i, H - 1, W - 1)) << i;
    }
    const auto t = s.time();
    for (std::uint64_t i = 0; i < n; ++i) ASSERT_EQ(t[i], static_cast<std::int64_t>(1000 * i));
}

}  // namespace

class SegmentedCompression : public ::testing::TestWithParam<rs::Compression> {};

TEST_P(SegmentedCompression, SplitsIntoSegmentsAndReadsBack) {
    TempDir dir;
    constexpr std::uint64_t n = 23;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto det = scan.add_node("detector_0", "raybotic.xray_detector", detector_attributes());
        rs::SegmentOptions opt;
        opt.segment_size = 5;
        opt.threads = 4;
        opt.max_pending = 3;
        opt.flush_every = 2;
        opt.compression.method = GetParam();
        auto images = det.segmented_series("image", {{"height", H}, {"width", W}}, opt);
        for (std::uint64_t i = 0; i < n; ++i) {
            const auto img = image(i);
            images.append(static_cast<std::int64_t>(1000 * i),
                          {{"image", rs::view(img.data(), {H, W})}});
        }
        EXPECT_EQ(images.size(), n);
        EXPECT_EQ(images.segment_count(), 5u);
        scan.finalize();
    }
    const auto seg_dir = dir.path() / "sensors" / "detector_0" / "image";
    EXPECT_TRUE(std::filesystem::exists(seg_dir / "seg_000004.h5"));
    EXPECT_FALSE(std::filesystem::exists(seg_dir / "seg_000005.h5"));

    auto scan = rs::ScanReader::open(dir.path() / rs::master_file_name);
    auto node = scan.node("sensors/detector_0");
    EXPECT_EQ(node.series_names(), (std::vector<std::string>{"image"}));
    auto images = node.series("image");
    EXPECT_EQ(images.storage(), rs::Storage::Segmented);
    EXPECT_EQ(images.dtype("image"), rs::DType::UInt16);
    check_images(images, n);
    // A range across a segment border.
    const auto part = images.read("image", 3, 4);
    EXPECT_EQ(part.values<std::uint16_t>()[3 * H * W], pixel(6, 0, 0));
}

INSTANTIATE_TEST_SUITE_P(All, SegmentedCompression,
                         ::testing::Values(rs::Compression::None, rs::Compression::Deflate,
                                           rs::Compression::ShuffleDeflate,
                                           rs::Compression::BitshuffleLZ4));

TEST(Segmented, CompressionShrinksSmoothImages) {
    TempDir dir;
    std::uint64_t raw = 0;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto det = scan.add_node("detector_0", "raybotic.xray_detector", detector_attributes());
        auto images = det.segmented_series("image", {{"height", H}, {"width", W}});
        for (std::uint64_t i = 0; i < 10; ++i) {
            const auto img = image(i);
            images.append(static_cast<std::int64_t>(i), {{"image", rs::view(img.data(), {H, W})}});
            raw += H * W * 2;
        }
    }
    const auto file = dir.path() / "sensors/detector_0/image/seg_000000.h5";
    EXPECT_LT(std::filesystem::file_size(file), raw / 2);
}

TEST(Segmented, LiveReadWhileRecording) {
    TempDir dir;
    auto scan = rs::ScanWriter::create(dir.path());
    auto det = scan.add_node("detector_0", "raybotic.xray_detector", detector_attributes());
    rs::SegmentOptions opt;
    opt.segment_size = 4;
    opt.flush_every = 0;
    auto images = det.segmented_series("image", {{"height", H}, {"width", W}}, opt);
    for (std::uint64_t i = 0; i < 6; ++i) {
        const auto img = image(i);
        images.append(static_cast<std::int64_t>(1000 * i), {{"image", rs::view(img.data(), {H, W})}});
    }
    images.flush();

    // The master is still open for writing, so read the segments directly;
    // the second segment is open in SWMR mode.
    auto live = rs::SeriesReader::open_segments(dir.path(), "sensors/detector_0", "image");
    check_images(live, 6);
    for (std::uint64_t i = 6; i < 9; ++i) {
        const auto img = image(i);
        images.append(static_cast<std::int64_t>(1000 * i), {{"image", rs::view(img.data(), {H, W})}});
    }
    images.flush();
    live.refresh();
    check_images(live, 9);
    scan.finalize();
    check_images(rs::ScanReader::open(dir.path()).node("sensors/detector_0").series("image"), 9);
}

TEST(Segmented, OtherProcessWriterIsLinkedOnFinalize) {
    TempDir dir;
    auto scan = rs::ScanWriter::create(dir.path());
    // The "camera process" only knows the scan directory, the node path and
    // the schema; the master never declared the node.
    const rs::Attributes cam_attrs = {{"frame_id", std::string("cell_cam")},
                                      {"simulated", false},
                                      {"width", std::int64_t{8}},
                                      {"height", std::int64_t{6}},
                                      {"channels", std::int64_t{3}},
                                      {"encoding", std::string("rgb8")}};
    std::thread camera([&] {
        auto cam = rs::SegmentedSeriesWriter::open(dir.path(), "sensors/cell_cam",
                                                   "raybotic.camera", "image", {}, {}, cam_attrs);
        EXPECT_EQ(cam.sample_shape("image"), (rs::Shape{6, 8, 3}));
        std::vector<std::uint8_t> rgb(6 * 8 * 3);
        for (int i = 0; i < 12; ++i) {
            std::fill(rgb.begin(), rgb.end(), static_cast<std::uint8_t>(i));
            cam.append(i, {{"image", rs::view(rgb.data(), {6, 8, 3})}});
        }
        cam.close();
    });
    camera.join();
    scan.finalize();

    auto node = rs::ScanReader::open(dir.path()).node("sensors/cell_cam");
    EXPECT_EQ(node.schema().id(), "raybotic.camera/1");
    EXPECT_EQ(std::get<std::string>(*node.attribute("encoding")), "rgb8");
    auto s = node.series("image");
    ASSERT_EQ(s.size(), 12u);
    EXPECT_EQ(s.read_sample("image", 11).values<std::uint8_t>()[17], 11);
}

TEST(Segmented, RebuildLinksLateSegments) {
    TempDir dir;
    rs::ScanWriter::create(dir.path()).finalize();
    {
        auto w = rs::SegmentedSeriesWriter::open(dir.path(), "sensors/detector_0",
                                                 "raybotic.xray_detector", "image",
                                                 {{"height", H}, {"width", W}}, {},
                                                 detector_attributes());
        const auto img = image(0);
        w.append(0, {{"image", rs::view(img.data(), {H, W})}});
    }  // closed by the destructor
    EXPECT_FALSE(rs::ScanReader::open(dir.path()).has_node("sensors/detector_0"));
    rs::ScanWriter::rebuild(dir.path());
    auto reader = rs::ScanReader::open(dir.path());
    EXPECT_TRUE(reader.complete());
    check_images(reader.node("sensors/detector_0").series("image"), 1);
}

TEST(Segmented, RefusesMixingRecordingsAndMissingDims) {
    TempDir dir;
    auto scan = rs::ScanWriter::create(dir.path());
    auto det = scan.add_node("detector_0", "raybotic.xray_detector", detector_attributes());
    EXPECT_THROW((void)det.segmented_series("image"), rs::Error);  // height/width unknown
    EXPECT_THROW((void)det.series("image"), rs::Error);            // segmented, not inline
    {
        auto images = det.segmented_series("image", {{"height", H}, {"width", W}});
        const auto img = image(0);
        images.append(0, {{"image", rs::view(img.data(), {H, W})}});
        images.close();
        EXPECT_THROW(images.append(1, {{"image", rs::view(img.data(), {H, W})}}), rs::Error);
    }
    EXPECT_THROW((void)rs::SegmentedSeriesWriter::open(dir.path(), "sensors/detector_0",
                                                 "raybotic.xray_detector", "image",
                                                 {{"height", H}, {"width", W}}),
                 rs::Error);  // segments of another recording exist
}

TEST(Segmented, FloatProjectionsWhenTheSchemaAllowsThem) {
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto det = scan.add_node("detector_0", "raybotic.xray_detector", detector_attributes());
        rs::SegmentOptions opt;
        opt.dtypes = {{"image", rs::DType::Int32}};
        EXPECT_THROW((void)det.segmented_series("image", {{"height", H}, {"width", W}}, opt),
                     rs::Error);  // not allowed for detector images
        opt.dtypes = {{"image", rs::DType::Float32}};
        auto images = det.segmented_series("image", {{"height", H}, {"width", W}}, opt);
        std::vector<float> img(H * W, 0.25f);
        img[7] = -1.5f;  // corrected data can be negative
        images.append(0, {{"image", rs::view(img.data(), {H, W})}});
    }
    const auto scan = rs::ScanReader::open(dir.path());
    const auto images = scan.node("sensors/detector_0").series("image");
    EXPECT_EQ(images.dtype("image"), rs::DType::Float32);
    EXPECT_FLOAT_EQ(images.read_sample("image", 0).values<float>()[7], -1.5f);
    EXPECT_TRUE(rs::validate(dir.path()).ok()) << rs::to_text(rs::validate(dir.path()));
}

TEST(Segmented, BitshuffleLz4OddSizesAndAllReadPaths) {
    // 33 x 29 = 957 elements: one full block of float32 (2048) does not fit,
    // so there is a short block of 952 elements (row = 119, odd) plus 5 raw
    // leftover elements.
    constexpr std::uint64_t h = 33;
    constexpr std::uint64_t w = 29;
    constexpr std::uint64_t n = 7;
    const auto value = [](std::uint64_t i, std::uint64_t k) {
        return static_cast<float>(i) * 1000.0f + static_cast<float>(k) * 0.37f - 50.0f;
    };
    TempDir dir;
    {
        auto scan = rs::ScanWriter::create(dir.path());
        auto attrs = detector_attributes();
        attrs["sensor_width"] = std::int64_t{w};
        attrs["sensor_height"] = std::int64_t{h};
        auto det = scan.add_node("detector_0", "raybotic.xray_detector", attrs);
        rs::SegmentOptions opt;
        opt.segment_size = 3;
        opt.dtypes = {{"image", rs::DType::Float32}};
        opt.compression.method = rs::Compression::BitshuffleLZ4;
        auto images = det.segmented_series("image", {{"height", h}, {"width", w}}, opt);
        std::vector<float> img(h * w);
        for (std::uint64_t i = 0; i < n; ++i) {
            for (std::uint64_t k = 0; k < h * w; ++k) img[k] = value(i, k);
            images.append(static_cast<std::int64_t>(i), {{"image", rs::view(img.data(), {h, w})}});
        }
    }
    const auto check = [&](const rs::SeriesReader& s, const char* how) {
        ASSERT_EQ(s.size(), n) << how;
        const auto all = s.read("image").values<float>();
        for (std::uint64_t i = 0; i < n; ++i) {
            for (std::uint64_t k = 0; k < h * w; ++k) {
                ASSERT_EQ(all[i * h * w + k], value(i, k)) << how << " sample " << i << " " << k;
            }
        }
        const auto one = s.read_sample("image", 4).values<float>();
        ASSERT_EQ(one[h * w - 1], value(4, h * w - 1)) << how;
    };
    // Segment files directly (rbscan's own decoder) ...
    check(rs::SeriesReader::open_segments(dir.path(), "sensors/detector_0", "image"), "segments");
    // ... and the virtual dataset in the master file (HDF5 + registered filter).
    const auto scan = rs::ScanReader::open(dir.path());
    check(scan.node("sensors/detector_0").series("image"), "master");
    EXPECT_TRUE(rs::validate(dir.path()).ok()) << rs::to_text(rs::validate(dir.path()));
}
