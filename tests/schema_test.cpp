#include <gtest/gtest.h>

#include <raybotic/scan/rbscan.hpp>

namespace rs = raybotic::scan;

TEST(Schema, BuiltinsAreRegistered) {
    const auto ids = rs::SchemaRegistry::global().ids();
    for (const char* id :
         {"raybotic.sensor/1", "raybotic.xray_source/1", "raybotic.xray_detector/1",
          "raybotic.camera/1", "raybotic.depth_camera/1", "raybotic.profile_scanner/1",
          "raybotic.force_torque/1", "raybotic.robot_arm/1", "raybotic.transform/1",
          "raybotic.ct_acquisition/1", "raybotic.annotations/1", "raybotic.ezrt_import/1",
          "raybotic.volume/1"}) {
        EXPECT_NE(std::find(ids.begin(), ids.end(), id), ids.end()) << id;
    }
}

TEST(Schema, ExtendsMergesBaseAttributes) {
    const auto& det = rs::SchemaRegistry::global().get("raybotic.xray_detector");
    EXPECT_EQ(det.version, 1u);
    EXPECT_EQ(det.category, "sensors");
    EXPECT_FALSE(det.abstract);
    ASSERT_NE(det.attribute("frame_id"), nullptr);  // from raybotic.sensor
    EXPECT_TRUE(det.attribute("frame_id")->required);
    ASSERT_NE(det.attribute("sensor_width"), nullptr);
    EXPECT_EQ(det.attribute("sensor_width")->dtype, rs::DType::UInt32);

    const auto& image = det.get_series("image");
    EXPECT_EQ(image.storage, rs::Storage::Segmented);
    ASSERT_EQ(image.fields.size(), 1u);
    EXPECT_EQ(image.fields[0].dtype, rs::DType::UInt16);
    ASSERT_EQ(image.fields[0].shape.size(), 2u);
    EXPECT_EQ(image.fields[0].shape[0].name, "height");

    EXPECT_TRUE(rs::SchemaRegistry::global().get("raybotic.sensor").abstract);
}

TEST(Schema, FlatJsonRoundTrips) {
    const auto& src = rs::SchemaRegistry::global().get("raybotic.ct_acquisition/1");
    const rs::Schema copy = rs::parse_schema(src.to_json());
    EXPECT_EQ(copy.id(), src.id());
    EXPECT_EQ(copy.to_json(), src.to_json());
    const auto* pm = copy.get_series("projections").field("projection_matrix");
    ASSERT_NE(pm, nullptr);
    EXPECT_TRUE(pm->optional);
    ASSERT_EQ(pm->shape.size(), 2u);
    EXPECT_EQ(pm->shape[0].size, 3u);
    EXPECT_EQ(pm->shape[1].size, 4u);
}

TEST(Schema, CustomSensorExtendsBase) {
    auto& registry = rs::SchemaRegistry::global();
    const auto& s = registry.add_json(R"({
        "schema": "test.dosimeter", "version": 1, "extends": "raybotic.sensor/1",
        "attributes": {"range_usv_h": {"dtype": "float64", "required": true}},
        "series": {"dose": {"fields": {"rate": {"dtype": "float32", "units": "uSv/h"}}}}
    })");
    EXPECT_EQ(s.category, "sensors");
    EXPECT_NE(s.attribute("frame_id"), nullptr);
    EXPECT_EQ(s.get_series("dose").storage, rs::Storage::Inline);
    // Same schema again is fine, a different one under the same id is not.
    EXPECT_NO_THROW(registry.add_json(s.to_json()));
    EXPECT_THROW(registry.add_json(R"({"schema": "test.dosimeter", "version": 1,
        "category": "sensors", "series": {}})"),
                 rs::Error);
}

TEST(Schema, RejectsInvalid) {
    auto& r = rs::SchemaRegistry::global();
    EXPECT_THROW(r.add_json("{"), rs::Error);
    EXPECT_THROW(r.add_json(R"({"schema": "x.a", "version": 1})"),
                 rs::Error);  // no category
    EXPECT_THROW(r.add_json(R"({"schema": "x.b", "version": 1, "category": "sensors",
        "series": {"s": {"fields": {"time": {"dtype": "float64"}}}}})"),
                 rs::Error);  // reserved
    EXPECT_THROW(r.add_json(R"({"schema": "x.c", "version": 1, "category": "sensors",
        "series": {"s": {"fields": {"name": {"dtype": "string"}}}}})"),
                 rs::Error);  // strings only as attributes
    EXPECT_THROW(r.add_json(R"({"schema": "x.d", "version": 1, "extends": "nope/1"})"),
                 rs::Error);
    EXPECT_THROW((void)r.get("raybotic.unknown"), rs::Error);
}
