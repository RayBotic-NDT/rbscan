#pragma once

// Helpers for schemas whose tables belong together, on top of the generated
// schemas.hpp: a volume with its matrix, a simulation scene object by object
// (mesh, placement, registration points), a trajectory plan trajectory by
// trajectory, annotations marker by marker and a CT projection with its image.
// They fill in what links the tables (row ranges, ids, name lists) and write
// every table in one append_many call. Header-only.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include "raybotic/scan/schemas.hpp"

namespace raybotic::scan::schemas {

namespace detail {

inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Rows [first, first + count) of a column vector.
template <class T>
std::vector<T> slice(const std::vector<T>& v, std::uint64_t first, std::uint64_t count) {
    if (first + count > v.size()) throw Error("row range outside its table");
    return {v.begin() + static_cast<std::ptrdiff_t>(first),
            v.begin() + static_cast<std::ptrdiff_t>(first + count)};
}

}  // namespace detail

// ---------------------------------------------------------------------------
namespace volume {

/// A volume [depth, height, width] (k, j, i) and the 4x4 affine (row-major)
/// from voxel index (i, j, k, 1) to world (m), voxel centres at integers.
struct Volume {
    Array voxels;
    std::array<double, 16> voxel_to_world{};
    Attributes attributes;
};

/// Stores `voxels` (float32, uint16 or uint8) as volumes/<name>. width,
/// height, depth, voxel_size and voxel_to_world are set from the data; the
/// other attributes are taken from `a` (world_frame defaults to "world").
inline NodeWriter write(ScanWriter& scan, std::string_view name, const Array& voxels,
                        const std::array<double, 16>& voxel_to_world, Attributes a = {},
                        SegmentOptions options = {}) {
    if (voxels.shape().size() != 3) throw Error("a volume is [depth, height, width]");
    a.depth = static_cast<std::uint32_t>(voxels.shape()[0]);
    a.height = static_cast<std::uint32_t>(voxels.shape()[1]);
    a.width = static_cast<std::uint32_t>(voxels.shape()[2]);
    a.voxel_to_world.assign(voxel_to_world.begin(), voxel_to_world.end());
    a.voxel_size.clear();
    for (int c = 0; c < 3; ++c) {
        const double x = voxel_to_world[c], y = voxel_to_world[4 + c],
                     z = voxel_to_world[8 + c];
        a.voxel_size.push_back(std::sqrt(x * x + y * y + z * z));
    }
    if (!a.world_frame) a.world_frame = "world";
    NodeWriter node = add(scan, name, a);
    options.dtypes["slice"] = voxels.dtype();
    auto slices = node.segmented_series(slices_series, {}, options);
    slices.append_many(detail::now_ns(), voxels.shape()[0],
                       {{"slice", detail::view_of(voxels)}});
    slices.close();
    return node;
}

/// A volume node: its voxels [depth, height, width] and matrix.
[[nodiscard]] inline Volume read_volume(const NodeReader& node) {
    Volume v;
    v.attributes = read(node);
    if (v.attributes.voxel_to_world.size() != 16) {
        throw Error(node.path() + ": voxel_to_world is not a 4x4 matrix");
    }
    std::copy(v.attributes.voxel_to_world.begin(), v.attributes.voxel_to_world.end(),
              v.voxel_to_world.begin());
    v.voxels = read_slices(node).slice;
    return v;
}

}  // namespace volume

// ---------------------------------------------------------------------------
namespace simulation_scene {

/// A registration point of an object with the label of its marker.
struct RegistrationPoint {
    RegistrationRow row;  ///< time_ns is set on write
    std::string marker;   ///< label of the marker (registration_markers)
};

/// One object: its mesh in file units and everything the tables hold for it.
struct Object {
    std::string name;
    std::string source;
    std::string material;
    /// Placement, material flags and colour; the row ranges and time_ns are
    /// set on write and filled on read.
    ObjectsRow placement;
    std::vector<std::array<float, 3>> vertices;
    /// Corners counted from this object's first vertex.
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<RegistrationPoint> registration;
};

/// Stores the scene as simulations/<name>. object_names, object_sources,
/// object_materials and registration_markers are set from `objects`; the
/// other attributes (document, settings, ...) are taken from `a`.
inline NodeWriter write(ScanWriter& scan, std::string_view name, Attributes a,
                        const std::vector<Object>& objects,
                        std::int64_t time_ns = detail::now_ns()) {
    a.object_names.emplace();
    a.object_sources.emplace();
    a.object_materials.emplace();
    a.registration_markers.emplace();
    ObjectsColumns oc;
    VerticesColumns vc;
    TrianglesColumns tc;
    RegistrationColumns rc;
    for (const Object& o : objects) {
        a.object_names->push_back(o.name);
        a.object_sources->push_back(o.source);
        a.object_materials->push_back(o.material);
        for (const auto& t : o.triangles) {
            for (const auto corner : t) {
                if (corner >= o.vertices.size()) {
                    throw Error(o.name + ": a triangle names a vertex the object lacks");
                }
            }
        }
        ObjectsRow row = o.placement;
        row.first_vertex = vc.position.size();
        row.vertex_count = o.vertices.size();
        row.first_triangle = tc.vertices.size();
        row.triangle_count = o.triangles.size();
        row.first_registration = rc.marker_id.size();
        row.registration_count = o.registration.size();
        oc.time_ns.push_back(time_ns);
        oc.from_volume.push_back(row.from_volume ? 1 : 0);
        oc.unit.push_back(row.unit);
        oc.scale.push_back(row.scale);
        oc.rotation.push_back(row.rotation);
        oc.position.push_back(row.position);
        oc.is_compound.push_back(row.is_compound ? 1 : 0);
        oc.density.push_back(row.density);
        oc.enabled.push_back(row.enabled ? 1 : 0);
        oc.colour.push_back(row.colour);
        oc.first_vertex.push_back(row.first_vertex);
        oc.vertex_count.push_back(row.vertex_count);
        oc.first_triangle.push_back(row.first_triangle);
        oc.triangle_count.push_back(row.triangle_count);
        oc.first_registration.push_back(row.first_registration);
        oc.registration_count.push_back(row.registration_count);
        vc.position.insert(vc.position.end(), o.vertices.begin(), o.vertices.end());
        tc.vertices.insert(tc.vertices.end(), o.triangles.begin(), o.triangles.end());
        for (const RegistrationPoint& p : o.registration) {
            a.registration_markers->push_back(p.marker);
            rc.marker_id.push_back(p.row.marker_id);
            rc.placed.push_back(p.row.placed ? 1 : 0);
            rc.mesh_point.push_back(p.row.mesh_point);
            rc.section_axis.push_back(p.row.section_axis);
            rc.section_offset.push_back(p.row.section_offset);
        }
    }
    vc.time_ns.assign(vc.position.size(), time_ns);
    tc.time_ns.assign(tc.vertices.size(), time_ns);
    rc.time_ns.assign(rc.marker_id.size(), time_ns);
    if (objects.empty()) {
        a.object_names.reset();
        a.object_sources.reset();
        a.object_materials.reset();
    }
    if (a.registration_markers->empty()) a.registration_markers.reset();

    NodeWriter node = add(scan, name, a);
    // A table without rows is not created.
    if (oc.size() > 0) {
        auto w = node.series(objects_series);
        append(w, oc);
    }
    if (vc.size() > 0) {
        auto w = node.series(vertices_series);
        append(w, vc);
    }
    if (tc.size() > 0) {
        auto w = node.series(triangles_series);
        append(w, tc);
    }
    if (rc.size() > 0) {
        auto w = node.series(registration_series);
        append(w, rc);
    }
    return node;
}

/// The scene's attributes and its objects, each with its own slice of the
/// mesh and registration tables.
[[nodiscard]] inline std::pair<Attributes, std::vector<Object>> read_scene(
    const NodeReader& node) {
    std::pair<Attributes, std::vector<Object>> out{read(node), {}};
    const Attributes& a = out.first;
    const ObjectsColumns oc = read_objects(node);
    const VerticesColumns vc = read_vertices(node);
    const TrianglesColumns tc = read_triangles(node);
    const RegistrationColumns rc = read_registration(node);
    const auto label = [](const std::optional<std::vector<std::string>>& names,
                          std::size_t i) {
        return names && i < names->size() ? (*names)[i] : std::string();
    };
    for (std::size_t i = 0; i < oc.size(); ++i) {
        Object o;
        o.placement = oc.row(i);
        o.name = label(a.object_names, i);
        o.source = label(a.object_sources, i);
        o.material = label(a.object_materials, i);
        const ObjectsRow& r = o.placement;
        o.vertices = detail::slice(vc.position, r.first_vertex, r.vertex_count);
        o.triangles = detail::slice(tc.vertices, r.first_triangle, r.triangle_count);
        if (r.first_registration + r.registration_count > rc.size()) {
            throw Error(node.path() + ": registration rows outside the table");
        }
        for (std::uint64_t k = 0; k < r.registration_count; ++k) {
            const auto row = static_cast<std::size_t>(r.first_registration + k);
            o.registration.push_back({rc.row(row), label(a.registration_markers, row)});
        }
        out.second.push_back(std::move(o));
    }
    return out;
}

}  // namespace simulation_scene

// ---------------------------------------------------------------------------
namespace trajectory_plan {

/// One trajectory with its poses, in drive order.
struct Trajectory {
    /// The orbit; first_pose, pose_count and time_ns are set on write.
    TrajectoriesRow orbit;
    /// Its poses; trajectory and time_ns are set on write.
    std::vector<PosesRow> poses;
};

/// Stores the plan as trajectories/<name>, trajectories in drive order.
inline NodeWriter write(ScanWriter& scan, std::string_view name, const Attributes& a,
                        const std::vector<Trajectory>& trajectories,
                        std::int64_t time_ns = detail::now_ns()) {
    NodeWriter node = add(scan, name, a);
    if (trajectories.empty()) return node;
    auto tw = node.series(trajectories_series);
    auto pw = node.series(poses_series);
    std::uint64_t first = 0;
    for (std::size_t t = 0; t < trajectories.size(); ++t) {
        TrajectoriesRow orbit = trajectories[t].orbit;
        orbit.time_ns = time_ns;
        orbit.first_pose = first;
        orbit.pose_count = trajectories[t].poses.size();
        append(tw, orbit);
        for (PosesRow pose : trajectories[t].poses) {
            pose.time_ns = time_ns;
            pose.trajectory = static_cast<std::uint32_t>(t);
            append(pw, pose);
        }
        first += orbit.pose_count;
    }
    return node;
}

/// The plan's trajectories, each with its poses.
[[nodiscard]] inline std::vector<Trajectory> read_plan(const NodeReader& node) {
    const auto orbits = read_trajectories_rows(node);
    const auto poses = read_poses_rows(node);
    std::vector<Trajectory> out;
    for (const TrajectoriesRow& orbit : orbits) {
        Trajectory t{orbit, detail::slice(poses, orbit.first_pose, orbit.pose_count)};
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace trajectory_plan

// ---------------------------------------------------------------------------
namespace annotations {

/// A marker with the projections it was set in.
struct Marker {
    MarkersRow row;  ///< time_ns is set on write; observations is the count
    std::vector<ObservationsRow> observations;  ///< marker_id is set on write
};

/// Stores the annotations as annotations/<name>. Observations go in time
/// order of their projections, as series need it; each marker's
/// `observations` count is set from its list.
inline NodeWriter write(ScanWriter& scan, std::string_view name, const Attributes& a,
                        const std::vector<Marker>& markers,
                        std::int64_t time_ns = detail::now_ns()) {
    NodeWriter node = add(scan, name, a);
    std::vector<ObservationsRow> observations;
    if (!markers.empty()) {
        auto mw = node.series(markers_series);
        for (const Marker& m : markers) {
            MarkersRow row = m.row;
            row.time_ns = time_ns;
            row.observations = static_cast<std::uint32_t>(m.observations.size());
            append(mw, row);
            for (ObservationsRow o : m.observations) {
                o.marker_id = m.row.marker_id;
                observations.push_back(o);
            }
        }
    }
    std::stable_sort(observations.begin(), observations.end(),
                     [](const auto& x, const auto& y) { return x.time_ns < y.time_ns; });
    if (!observations.empty()) {
        auto ow = node.series(observations_series);
        for (const auto& o : observations) append(ow, o);
    }
    return node;
}

/// The markers, each with its observations (in the stored, time order).
[[nodiscard]] inline std::vector<Marker> read_annotations(const NodeReader& node) {
    std::vector<Marker> out;
    std::map<std::uint32_t, std::size_t> index;
    for (const MarkersRow& row : read_markers_rows(node)) {
        index[row.marker_id] = out.size();
        out.push_back({row, {}});
    }
    for (const ObservationsRow& o : read_observations_rows(node)) {
        const auto it = index.find(o.marker_id);
        if (it != index.end()) out[it->second].observations.push_back(o);
    }
    return out;
}

}  // namespace annotations

// ---------------------------------------------------------------------------
namespace ct_acquisition {

/// One projection: its geometry row and the detector image it refers to.
struct Projection {
    ProjectionsRow geometry;
    Array image;
};

/// Row `index` of the acquisition's projections with its image, taken from
/// the detector node the acquisition names (image_index).
[[nodiscard]] inline Projection read_projection(const ScanReader& scan,
                                                const NodeReader& acquisition,
                                                std::uint64_t index) {
    const auto rows = read_projections_rows(acquisition, index, 1);
    if (rows.empty()) {
        throw Error(acquisition.path() + ": no projection " + std::to_string(index));
    }
    Projection p{rows.front(), {}};
    const Attributes a = read(acquisition);
    p.image = scan.node(a.detector)
                  .series("image")
                  .read_sample("image", p.geometry.image_index);
    return p;
}

}  // namespace ct_acquisition

}  // namespace raybotic::scan::schemas
