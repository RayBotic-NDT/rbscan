// Links segment files into the master file as HDF5 virtual datasets, so tools
// that only know HDF5 (h5py, HDFView, MATLAB) see one dataset per field.

#include <map>

#include "internal.hpp"

namespace raybotic::scan::detail {

namespace {

namespace fs = std::filesystem;

struct SourceDataset {
    std::string name;
    DType dtype{DType::Float64};
    Shape sample_shape;
    std::optional<std::string> units;
    std::optional<std::string> description;
};

struct Segment {
    fs::path path;
    std::uint64_t samples{0};
};

struct SegmentSet {
    std::vector<Segment> segments;
    std::vector<SourceDataset> datasets;
    std::string schema_json;
    Attributes node_attributes;
};

std::vector<fs::path> subdirs(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory() && entry.path().filename().string().front() != '.') {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Reads what the segments hold. Stops at the first segment that cannot be
/// opened (still being created), so the linked samples stay contiguous.
SegmentSet inspect(const std::vector<fs::path>& files) {
    h5::Lock lock(h5::mutex());
    SegmentSet set;
    for (const auto& file : files) {
        h5::Id f;
        try {
            f = h5::open_file(utf8(file), H5F_ACC_RDONLY, true);
        } catch (const Error&) {
            break;
        }
        std::vector<std::string> names;
        for (const auto& child : h5::children(f.get())) {
            if (!h5::is_group(f.get(), child)) names.push_back(child);
        }
        if (set.segments.empty()) {
            set.schema_json =
                h5::read_string_attribute(f.get(), "schema_json").value_or("");
            if (h5::exists(f.get(), segment_node_group)) {
                h5::Id g = h5::open_group(f.get(), segment_node_group);
                set.node_attributes = h5::read_attributes(g.get());
            }
            for (const auto& name : names) {
                h5::Id d = h5::open_dataset(f.get(), name);
                h5::Id type(h5::check_id(H5Dget_type(d.get()), "H5Dget_type"));
                SourceDataset sd;
                sd.name = name;
                sd.dtype = h5::dtype_of(type.get());
                const Shape e = h5::extent(d.get());
                sd.sample_shape.assign(e.begin() + 1, e.end());
                sd.units = h5::read_string_attribute(d.get(), "units");
                sd.description = h5::read_string_attribute(d.get(), "description");
                set.datasets.push_back(std::move(sd));
            }
        }
        std::uint64_t n = std::numeric_limits<std::uint64_t>::max();
        for (const auto& sd : set.datasets) {
            h5::Id d = h5::open_dataset(f.get(), sd.name);
            n = std::min(n, h5::extent(d.get())[0]);
        }
        if (set.datasets.empty()) n = 0;
        set.segments.push_back({file, n});
    }
    return set;
}

void link_series(hid_t master, const fs::path& scan_dir, const std::string& group_path,
                 const SegmentSet& set) {
    h5::Lock lock(h5::mutex());
    if (h5::exists(master, group_path)) {
        h5::check(H5Ldelete(master, group_path.c_str(), H5P_DEFAULT),
                  "H5Ldelete " + group_path);
    }
    h5::Id group = h5::ensure_group(master, group_path);
    std::uint64_t total = 0;
    for (const auto& s : set.segments) total += s.samples;
    h5::write_string_attribute(group.get(), "storage", "segmented");
    h5::write_attribute(group.get(), "segment_count",
                        Value(static_cast<std::int64_t>(set.segments.size())),
                        DType::UInt64);

    for (const auto& sd : set.datasets) {
        const int rank = static_cast<int>(sd.sample_shape.size()) + 1;
        std::vector<hsize_t> dims(static_cast<std::size_t>(rank));
        dims[0] = total;
        for (std::size_t i = 0; i < sd.sample_shape.size(); ++i)
            dims[i + 1] = sd.sample_shape[i];
        h5::Id vspace(
            h5::check_id(H5Screate_simple(rank, dims.data(), nullptr), "H5Screate"));
        h5::Id dcpl(h5::check_id(H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate"));

        std::uint64_t offset = 0;
        for (const auto& seg : set.segments) {
            if (seg.samples == 0) continue;
            std::vector<hsize_t> start(static_cast<std::size_t>(rank), 0);
            std::vector<hsize_t> count(dims);
            start[0] = offset;
            count[0] = seg.samples;
            h5::check(H5Sselect_hyperslab(vspace.get(), H5S_SELECT_SET, start.data(),
                                          nullptr, count.data(), nullptr),
                      "H5Sselect_hyperslab");
            h5::Id src_space(
                h5::check_id(H5Screate_simple(rank, count.data(), nullptr), "H5Screate"));
            // Relative to the master file's directory, so the scan can move.
            const std::string src_file = utf8(fs::relative(seg.path, scan_dir));
            const std::string src_dataset = "/" + sd.name;
            h5::check(H5Pset_virtual(dcpl.get(), vspace.get(), src_file.c_str(),
                                     src_dataset.c_str(), src_space.get()),
                      "H5Pset_virtual");
            offset += seg.samples;
        }
        h5::check(H5Sselect_all(vspace.get()), "H5Sselect_all");
        h5::Id type = h5::file_type(sd.dtype);
        h5::Id dset(
            h5::check_id(H5Dcreate2(group.get(), sd.name.c_str(), type.get(),
                                    vspace.get(), H5P_DEFAULT, dcpl.get(), H5P_DEFAULT),
                         "creating virtual dataset " + sd.name));
        if (sd.units) h5::write_string_attribute(dset.get(), "units", *sd.units);
        if (sd.description)
            h5::write_string_attribute(dset.get(), "description", *sd.description);
    }
}

}  // namespace

void link_all_segments(hid_t master, const fs::path& scan_dir) {
    h5::Lock lock(h5::mutex());
    for (const auto& category : subdirs(scan_dir)) {
        for (const auto& node : subdirs(category)) {
            const std::string node_path =
                category.filename().string() + "/" + node.filename().string();
            for (const auto& series : subdirs(node)) {
                const auto files = list_segments(series);
                if (files.empty()) continue;
                const SegmentSet set = inspect(files);
                if (set.segments.empty()) continue;

                if (!h5::exists(master, node_path)) {
                    if (set.schema_json.empty()) continue;
                    const Schema schema = parse_schema(set.schema_json);
                    create_node_group(master, node_path, schema, set.node_attributes);
                }
                link_series(master, scan_dir,
                            node_path + "/" + series.filename().string(), set);
            }
        }
    }
}

}  // namespace raybotic::scan::detail
