// Stores a simulation scene in a scan and reads it back: objects with their
// meshes (bit for bit, in the file units of their source), placement,
// material and registration points, plus the tube and detector settings.
// Without STL files it builds a cube and a tetrahedron.
//
//   rbscan_simulation_scene <scan_dir> [part.stl ...]

#include <cstdio>
#include <cstring>
#include <fstream>
#include <raybotic/scan/rbscan.hpp>
#include <raybotic/scan/schema_helpers.hpp>
#include <string>
#include <vector>

namespace rs = raybotic::scan;
namespace scene = raybotic::scan::schemas::simulation_scene;

namespace {

/// A binary STL as an indexed mesh. Vertices are not merged (three per
/// triangle), so they stay exactly as the file has them.
scene::Object load_stl(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char header[80];
    std::uint32_t count = 0;
    if (!in.read(header, sizeof header) ||
        !in.read(reinterpret_cast<char*>(&count), sizeof count)) {
        throw rs::Error(path + ": not a binary STL");
    }
    scene::Object o;
    o.source = path;
    for (std::uint32_t t = 0; t < count; ++t) {
        float f[12];  // normal, three corners
        std::uint16_t attribute = 0;
        if (!in.read(reinterpret_cast<char*>(f), sizeof f) ||
            !in.read(reinterpret_cast<char*>(&attribute), sizeof attribute)) {
            throw rs::Error(path + ": file ends early");
        }
        const auto first = static_cast<std::uint32_t>(o.vertices.size());
        for (int c = 1; c <= 3; ++c)
            o.vertices.push_back({f[3 * c], f[3 * c + 1], f[3 * c + 2]});
        o.triangles.push_back({first, first + 1, first + 2});
    }
    return o;
}

scene::Object cube(float edge) {
    scene::Object o;
    o.source = "built-in cube";
    for (int i = 0; i < 8; ++i) {
        o.vertices.push_back(
            {i & 1 ? edge : 0.0f, i & 2 ? edge : 0.0f, i & 4 ? edge : 0.0f});
    }
    o.triangles = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                   {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    return o;
}

scene::Object tetrahedron(float edge) {
    scene::Object o;
    o.source = "built-in tetrahedron";
    o.vertices = {{0, 0, 0}, {edge, 0, 0}, {0, edge, 0}, {0, 0, edge}};
    o.triangles = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scan_dir> [part.stl ...]\n", argv[0]);
        return 2;
    }
    try {
        std::vector<scene::Object> objects;
        for (int i = 2; i < argc; ++i) {
            objects.push_back(load_stl(argv[i]));
            objects.back().name = "part " + std::to_string(i - 1);
            objects.back().material = "Al";
        }
        if (objects.empty()) {
            objects.push_back(cube(20.0f));
            objects.back().name = "housing";
            objects.back().material = "Al";
            objects.push_back(tetrahedron(8.0f));
            objects.back().name = "insert";
            objects.back().material = "C2H4";  // a formula: is_compound
            objects.back().placement.is_compound = true;
            objects.back().placement.density = 0.94;  // g/cm3
        }
        for (std::size_t i = 0; i < objects.size(); ++i) {
            scene::ObjectsRow& p = objects[i].placement;
            p.unit = 0;  // the mesh is in mm
            p.scale = 1.0;
            p.rotation = {0.0, 0.0, 15.0 * static_cast<double>(i)};  // deg
            p.position = {0.0, 0.0, 30.0 * static_cast<double>(i)};  // mm
            p.enabled = true;
            p.colour = {0.8f, 0.6f - 0.3f * static_cast<float>(i % 2), 0.2f};
        }
        // A registration point on the first object: its first vertex, shown
        // by marker M1.
        scene::RegistrationPoint m1;
        m1.marker = "M1";
        m1.row.marker_id = 1;
        m1.row.placed = true;
        const auto& v0 = objects[0].vertices.at(0);
        m1.row.mesh_point = {v0[0], v0[1], v0[2]};
        m1.row.section_axis = -1;  // clicked on the surface in a 3D view
        objects[0].registration.push_back(m1);

        // The application's own scene document goes in `document`; the
        // settings attributes repeat it for other tools.
        scene::Attributes a;
        a.application = "rbscan example";
        a.format = "rbscan-example-scene/1";
        a.document = R"({"note": "the application's scene document"})";
        a.voltage_kv = 160.0;
        a.current_ua = 100.0;
        a.focal_spot_um = 20.0;
        a.sensor_width = a.sensor_height = 1024;
        a.pixel_pitch_u_um = a.pixel_pitch_v_um = 139.0;
        a.scintillator = "CsI";
        a.scintillator_um = 600.0;
        {
            auto scan = rs::ScanWriter::create(argv[1],
                                               {.description = "simulation scene"}, true);
            scene::write(scan, "scene", a, objects);
            scan.finalize();
        }

        const auto reader = rs::ScanReader::open(argv[1]);
        const auto [attrs, back] = scene::read_scene(reader.node("simulations/scene"));
        std::printf("scene from %s: %.0f kV, %.0f uA, detector %u x %u\n",
                    attrs.application.c_str(), attrs.voltage_kv.value_or(0),
                    attrs.current_ua.value_or(0), attrs.sensor_width.value_or(0),
                    attrs.sensor_height.value_or(0));
        for (std::size_t i = 0; i < back.size(); ++i) {
            const scene::Object& o = back[i];
            const bool same =
                o.vertices.size() == objects[i].vertices.size() &&
                std::memcmp(o.vertices.data(), objects[i].vertices.data(),
                            o.vertices.size() * sizeof(o.vertices[0])) == 0 &&
                o.triangles == objects[i].triangles;
            std::printf(
                "  %-8s %-5s %5zu vertices %5zu triangles at z = %4.0f mm, "
                "%zu registration points, mesh %s\n",
                o.name.c_str(), o.material.c_str(), o.vertices.size(), o.triangles.size(),
                o.placement.position[2], o.registration.size(),
                same ? "bit-identical" : "DIFFERS");
            if (!same) return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
