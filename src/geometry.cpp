// Frame graph (TF) over the stored transforms, and the projection geometry
// resolved from it.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>

#include "convert.hpp"
#include "internal.hpp"
#include "pose_math.hpp"
#include "raybotic/scan/rbscan.hpp"
#include "raybotic/scan/tools.hpp"
#include "series_layout.hpp"

namespace raybotic::scan {

namespace fs = std::filesystem;
using detail::Quat;
using detail::Vec3;

namespace {

constexpr std::uint32_t flag_invalid = 1u << 0;
constexpr std::uint32_t flag_interpolated = 1u << 1;
constexpr std::uint32_t flag_robot_unreachable = 1u << 2;
constexpr std::uint32_t flag_emission_unstable = 1u << 3;

std::vector<double> read_doubles(const SeriesReader& s, std::string_view field) {
    const Array a = s.read(field);
    std::vector<double> out(static_cast<std::size_t>(a.size()));
    detail::convert(a.dtype(), a.data(), DType::Float64, out.data(), out.size());
    return out;
}

/// Index of the last sample with time <= t.
std::optional<std::size_t> last_at(const std::vector<std::int64_t>& times,
                                   std::int64_t t) {
    auto it = std::upper_bound(times.begin(), times.end(), t);
    if (it == times.begin()) return std::nullopt;
    return static_cast<std::size_t>(std::distance(times.begin(), it) - 1);
}

std::string string_attribute(const NodeReader& node, const std::string& name) {
    const auto v = node.attribute(name);
    if (v && std::holds_alternative<std::string>(*v)) return std::get<std::string>(*v);
    return {};
}

double double_attribute(const NodeReader& node, const std::string& name) {
    const auto v = node.attribute(name);
    if (!v) throw Error(node.path() + ": attribute '" + name + "' is missing");
    if (const auto* d = std::get_if<double>(&*v)) return *d;
    if (const auto* i = std::get_if<std::int64_t>(&*v)) return static_cast<double>(*i);
    throw Error(node.path() + ": attribute '" + name + "' is not a number");
}

/// The node path of the one node with this schema, or the given one.
std::string pick_node(const ScanReader& scan, const std::string& given,
                      const char* schema) {
    if (!given.empty()) {
        if (!scan.has_node(given)) throw Error("no node '" + given + "'");
        return given;
    }
    std::vector<std::string> found;
    for (const auto& p : scan.node_paths()) {
        if (scan.node(p).schema().name == schema) found.push_back(p);
    }
    if (found.size() != 1) {
        throw Error(std::string("expected exactly one ") + schema + " node, found " +
                    std::to_string(found.size()) + "; name it in the options");
    }
    return found.front();
}

}  // namespace

// ---------------------------------------------------------------------------
// FrameGraph

struct FrameGraph::Impl {
    struct Edge {
        std::string parent;
        std::string child;
        bool is_static{false};
        std::vector<std::int64_t> time;
        std::vector<Vec3> translation;
        std::vector<Quat> rotation;

        Pose at(std::int64_t t, bool& extrapolated, bool& interpolated) const {
            Pose p;
            if (is_static || time.size() == 1) {
                p.translation = translation.front();
                p.rotation = rotation.front();
                return p;
            }
            if (t <= time.front() || t >= time.back()) {
                const std::size_t i = t <= time.front() ? 0 : time.size() - 1;
                if (t != time[i]) extrapolated = true;
                p.translation = translation[i];
                p.rotation = rotation[i];
                return p;
            }
            const auto hi = static_cast<std::size_t>(std::distance(
                time.begin(), std::upper_bound(time.begin(), time.end(), t)));
            const std::size_t lo = hi - 1;
            if (time[lo] == t) {
                p.translation = translation[lo];
                p.rotation = rotation[lo];
                return p;
            }
            interpolated = true;
            const double alpha = static_cast<double>(t - time[lo]) /
                                 static_cast<double>(time[hi] - time[lo]);
            for (int k = 0; k < 3; ++k) {
                p.translation[k] = translation[lo][k] +
                                   alpha * (translation[hi][k] - translation[lo][k]);
            }
            p.rotation = detail::slerp(rotation[lo], rotation[hi], alpha);
            return p;
        }
    };
    std::map<std::string, Edge, std::less<>> by_child;

    /// Pose of `frame` in the root of its tree.
    std::pair<std::string, Pose> to_root(std::string_view frame, std::int64_t t,
                                         bool& extrapolated, bool& interpolated) const {
        Pose pose;
        std::string current(frame);
        for (int depth = 0;; ++depth) {
            auto it = by_child.find(current);
            if (it == by_child.end()) return {current, pose};
            if (depth > 1000) throw Error("frames form a loop at '" + current + "'");
            pose = detail::compose(it->second.at(t, extrapolated, interpolated), pose);
            current = it->second.parent;
        }
    }

    Pose lookup(std::string_view parent, std::string_view child, std::int64_t t,
                bool& extrapolated, bool& interpolated) const {
        const auto [root_p, pose_p] = to_root(parent, t, extrapolated, interpolated);
        const auto [root_c, pose_c] = to_root(child, t, extrapolated, interpolated);
        if (root_p != root_c) {
            throw Error("frames '" + std::string(parent) + "' and '" +
                        std::string(child) + "' are not connected (roots '" + root_p +
                        "' and '" + root_c + "')");
        }
        return detail::compose(detail::inverse(pose_p), pose_c);
    }
};

namespace {

std::shared_ptr<FrameGraph::Impl> load_frames(const fs::path& scan_path) {
    auto impl = std::make_shared<FrameGraph::Impl>();
    const ScanReader scan = ScanReader::open(scan_path);
    for (const auto& path : scan.node_paths()) {
        const NodeReader node = scan.node(path);
        if (node.schema().name != "raybotic.transform" || !node.has_series("pose"))
            continue;
        FrameGraph::Impl::Edge e;
        e.parent = string_attribute(node, "parent");
        e.child = string_attribute(node, "child");
        if (e.parent.empty() || e.child.empty()) {
            throw Error(path + ": parent and child must be set");
        }
        const auto v = node.attribute("static");
        e.is_static = v && std::holds_alternative<bool>(*v) && std::get<bool>(*v);
        const SeriesReader pose = node.series("pose");
        e.time = pose.time();
        if (e.time.empty()) continue;
        const auto t = read_doubles(pose, "translation");
        const auto r = read_doubles(pose, "rotation");
        for (std::size_t i = 0; i < e.time.size(); ++i) {
            e.translation.push_back({t[3 * i], t[3 * i + 1], t[3 * i + 2]});
            e.rotation.push_back(detail::normalized(
                Quat{r[4 * i], r[4 * i + 1], r[4 * i + 2], r[4 * i + 3]}));
        }
        if (impl->by_child.contains(e.child)) {
            throw Error("frame '" + e.child + "' has two parents (" + path + ")");
        }
        impl->by_child.emplace(e.child, std::move(e));
    }
    return impl;
}

}  // namespace

FrameGraph::FrameGraph(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

FrameGraph FrameGraph::load(const fs::path& scan_path) {
    return FrameGraph(load_frames(scan_path));
}

Pose FrameGraph::lookup(std::string_view parent, std::string_view child,
                        std::int64_t time_ns, bool* extrapolated) const {
    bool ex = false;
    bool in = false;
    const Pose p = impl_->lookup(parent, child, time_ns, ex, in);
    if (extrapolated != nullptr) *extrapolated = ex;
    return p;
}

bool FrameGraph::has_frame(std::string_view frame) const {
    if (impl_->by_child.contains(frame)) return true;
    for (const auto& [child, e] : impl_->by_child) {
        if (e.parent == frame) return true;
    }
    return false;
}

std::vector<std::string> FrameGraph::frames() const {
    std::vector<std::string> out;
    for (const auto& [child, e] : impl_->by_child) {
        out.push_back(child);
        out.push_back(e.parent);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Projection geometry

std::array<double, 12> projection_matrix(const std::array<double, 3>& s,
                                         const std::array<double, 3>& d,
                                         const std::array<double, 3>& u,
                                         const std::array<double, 3>& v, double pixel_u,
                                         double pixel_v, std::uint64_t width,
                                         std::uint64_t height) {
    // A world point X projects along D = X - s onto the detector plane
    // (through d, normal n) at p = s + D (d - s).n / D.n. With w = D.n:
    //   ((p - d).u) w = D . [((s - d).u) n + ((d - s).n) u]
    // so column * w and row * w are linear in X, which gives the 3x4 matrix.
    const Vec3 n = detail::normalized(detail::cross(u, v));
    const Vec3 sd = detail::sub(s, d);
    const double cu = 0.5 * static_cast<double>(width - 1);
    const double cv = 0.5 * static_cast<double>(height - 1);
    auto row = [&](const Vec3& axis, double pixel, double centre) {
        const Vec3 a = detail::add(detail::scale(n, detail::dot(sd, axis)),
                                   detail::scale(axis, -detail::dot(sd, n)));
        return detail::add(detail::scale(a, 1.0 / pixel), detail::scale(n, centre));
    };
    const Vec3 r0 = row(u, pixel_u, cu);
    const Vec3 r1 = row(v, pixel_v, cv);
    const Vec3& r2 = n;
    return {r0[0], r0[1], r0[2], -detail::dot(r0, s),  //
            r1[0], r1[1], r1[2], -detail::dot(r1, s),  //
            r2[0], r2[1], r2[2], -detail::dot(r2, s)};
}

GeometryResult resolve_geometry(const fs::path& scan_path,
                                const GeometryOptions& options) {
    const Schema& acq_schema = SchemaRegistry::global().get("raybotic.ct_acquisition/1");
    const std::string acq_path = acq_schema.category + "/" + options.acquisition;

    // --- read everything needed; the reader is closed before writing -------
    std::string source_path, detector_path, source_frame, detector_frame;
    std::vector<std::int64_t> image_time;
    std::uint64_t width = 0, height = 0;
    double pitch_u = 0.0, pitch_v = 0.0;
    std::vector<std::int64_t> det_time, src_time;
    std::vector<double> binning, integration, voltage, current, emission;
    struct Robot {
        std::vector<std::int64_t> time;
        std::vector<double> status;
    };
    std::vector<Robot> robots;
    bool acquisition_exists = false;
    {
        const ScanReader scan = ScanReader::open(scan_path);
        source_path = pick_node(scan, options.source, "raybotic.xray_source");
        detector_path = pick_node(scan, options.detector, "raybotic.xray_detector");
        const NodeReader source = scan.node(source_path);
        const NodeReader detector = scan.node(detector_path);
        source_frame = options.source_frame.empty() ? string_attribute(source, "frame_id")
                                                    : options.source_frame;
        detector_frame = options.detector_frame.empty()
                             ? string_attribute(detector, "frame_id")
                             : options.detector_frame;
        if (!detector.has_series("image")) throw Error(detector_path + " has no images");
        const SeriesReader images = detector.series("image");
        image_time = images.time();
        const Shape shape = images.sample_shape("image");
        if (shape.size() < 2) throw Error(detector_path + ": image is not 2-D");
        height = shape[0];
        width = shape[1];
        pitch_u = double_attribute(detector, "pixel_pitch_u_um") * 1e-6;
        pitch_v = double_attribute(detector, "pixel_pitch_v_um") * 1e-6;
        if (detector.has_series("state")) {
            const SeriesReader st = detector.series("state");
            det_time = st.time();
            binning = read_doubles(st, "binning");
            integration = read_doubles(st, "integration_time_ms");
        }
        if (source.has_series("state")) {
            const SeriesReader st = source.series("state");
            src_time = st.time();
            voltage = read_doubles(st, "voltage_kv");
            current = read_doubles(st, "current_ua");
            emission = read_doubles(st, "emission_state");
        }
        for (const auto& p : scan.node_paths()) {
            const NodeReader node = scan.node(p);
            if (node.schema().name != "raybotic.robot_arm" ||
                !node.has_series("tracking"))
                continue;
            const std::string target = string_attribute(node, "target_frame");
            if (target != source_frame && target != detector_frame) continue;
            const SeriesReader tr = node.series("tracking");
            robots.push_back({tr.time(), read_doubles(tr, "status")});
        }
        if (scan.has_node(acq_path)) {
            const NodeReader acq = scan.node(acq_path);
            if (acq.schema().name != acq_schema.name) {
                throw Error(acq_path + " exists with schema " + acq.schema().id());
            }
            acquisition_exists = true;
            if (acq.has_series("projections") && !options.overwrite) {
                throw Error(acq_path + "/projections exists already (set overwrite)");
            }
        }
    }
    if (source_frame.empty() || detector_frame.empty()) {
        throw Error("source or detector frame unknown (frame_id missing)");
    }
    const auto graph = load_frames(scan_path);

    // --- one row per image --------------------------------------------------
    const std::size_t n = image_time.size();
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    std::map<std::string, std::vector<double>> col;
    auto column = [&](const char* name, std::size_t per_sample) -> std::vector<double>& {
        auto& c = col[name];
        c.assign(n * per_sample, nan);
        return c;
    };
    auto& c_index = column("image_index", 1);
    auto& c_src = column("source_position", 3);
    auto& c_det = column("detector_position", 3);
    auto& c_u = column("detector_u", 3);
    auto& c_v = column("detector_v", 3);
    auto& c_px = column("pixel_size", 2);
    auto& c_pm = column("projection_matrix", 12);
    auto& c_kv = column("voltage_kv", 1);
    auto& c_ua = column("current_ua", 1);
    auto& c_ms = column("integration_time_ms", 1);
    auto& c_flags = column("flags", 1);

    GeometryResult result;
    result.acquisition = acq_path;
    result.projections = n;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t t = image_time[i];
        std::uint32_t flags = 0;
        bool extrapolated = false;
        bool interpolated = false;
        const Pose src = graph->lookup(options.world_frame, source_frame, t, extrapolated,
                                       interpolated);
        const Pose det = graph->lookup(options.world_frame, detector_frame, t,
                                       extrapolated, interpolated);
        // Detector frame: x = image right (columns), rows run along -y.
        const Vec3 u = detail::rotate(det.rotation, {1.0, 0.0, 0.0});
        const Vec3 v = detail::rotate(det.rotation, {0.0, -1.0, 0.0});

        double bin = 1.0;
        if (auto k = last_at(det_time, t)) {
            if (binning[*k] >= 1.0) bin = binning[*k];
            c_ms[i] = integration[*k];
        }
        if (auto k = last_at(src_time, t)) {
            c_kv[i] = voltage[*k];
            c_ua[i] = current[*k];
            if (emission[*k] != 2.0) flags |= flag_emission_unstable;  // not EMISSION_ON
        }
        for (const auto& r : robots) {
            if (auto k = last_at(r.time, t);
                k && r.status[*k] == 3.0) {  // STATUS_UNREACHABLE
                flags |= flag_robot_unreachable;
            }
        }
        if (extrapolated) flags |= flag_invalid;
        if (interpolated) flags |= flag_interpolated;

        const double pu = pitch_u * bin;
        const double pv = pitch_v * bin;
        c_index[i] = static_cast<double>(i);
        for (std::size_t k = 0; k < 3; ++k) {
            c_src[3 * i + k] = src.translation[k];
            c_det[3 * i + k] = det.translation[k];
            c_u[3 * i + k] = u[k];
            c_v[3 * i + k] = v[k];
        }
        c_px[2 * i] = pu;
        c_px[2 * i + 1] = pv;
        if (options.projection_matrix) {
            const auto pm = projection_matrix(src.translation, det.translation, u, v, pu,
                                              pv, width, height);
            std::copy(pm.begin(), pm.end(),
                      c_pm.begin() + static_cast<std::ptrdiff_t>(12 * i));
        }
        c_flags[i] = flags;

        result.interpolated += interpolated ? 1 : 0;
        result.extrapolated += extrapolated ? 1 : 0;
        result.robot_unreachable += (flags & flag_robot_unreachable) ? 1 : 0;
        result.emission_unstable += (flags & flag_emission_unstable) ? 1 : 0;
    }

    // --- write ---------------------------------------------------------------
    h5::init();
    const fs::path master =
        fs::is_directory(scan_path) ? scan_path / master_file_name : scan_path;
    h5::Lock lock(h5::mutex());
    h5::Id file;
    try {
        file = h5::open_file(detail::utf8(master), H5F_ACC_RDWR);
    } catch (const Error& e) {
        throw Error(
            std::string("cannot open the scan for writing; close every reader or writer "
                        "of it first: ") +
            e.what());
    }
    h5::Id node;
    if (acquisition_exists) {
        node = h5::open_group(file.get(), acq_path);
    } else {
        node = detail::create_node_group(file.get(), acq_path, acq_schema,
                                         {{"type", std::string("ct_scan")},
                                          {"source", source_path},
                                          {"detector", detector_path},
                                          {"world_frame", options.world_frame}});
    }
    if (h5::exists(node.get(), "projections")) {
        h5::check(H5Ldelete(node.get(), "projections", H5P_DEFAULT),
                  "H5Ldelete projections");
    }
    h5::Id group = h5::ensure_group(node.get(), "projections");
    h5::write_string_attribute(group.get(), "storage", "inline");
    h5::write_string_attribute(group.get(), "generated_by", "rbscan resolve_geometry");
    h5::write_string_attribute(group.get(), "world_frame", options.world_frame);
    h5::write_string_attribute(group.get(), "source_frame", source_frame);
    h5::write_string_attribute(group.get(), "detector_frame", detector_frame);

    const auto layout =
        detail::SeriesLayout::make(acq_schema.get_series("projections"), {}, {},
                                   256 * 1024, std::numeric_limits<std::uint64_t>::max());
    const CompressionOptions compression{};
    for (const auto& f : layout.fields) {
        const auto per_sample = static_cast<std::size_t>(element_count(f.sample_shape));
        std::vector<std::byte> buffer(n * f.sample_bytes);
        if (f.name == detail::time_field) {
            std::memcpy(buffer.data(), image_time.data(), buffer.size());
        } else if (auto it = col.find(f.name); it != col.end()) {
            detail::convert(DType::Float64, it->second.data(), f.dtype, buffer.data(),
                            n * per_sample);
        } else {
            detail::fill_missing(f.dtype, buffer.data(), n * per_sample);
        }
        h5::Id dset = detail::create_field_dataset(group.get(), f, compression);
        if (n == 0) continue;
        std::vector<hsize_t> dims{n};
        dims.insert(dims.end(), f.sample_shape.begin(), f.sample_shape.end());
        h5::check(H5Dset_extent(dset.get(), dims.data()), "H5Dset_extent");
        h5::Id mtype = h5::memory_type(f.dtype);
        h5::check(H5Dwrite(dset.get(), mtype.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                           buffer.data()),
                  "writing " + f.name);
    }
    return result;
}

}  // namespace raybotic::scan
