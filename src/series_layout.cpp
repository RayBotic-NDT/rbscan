#include "series_layout.hpp"

#include <algorithm>
#include <cstring>

#include "compression.hpp"
#include "convert.hpp"

namespace raybotic::scan::detail {

namespace {

std::uint64_t resolve_dim(const DimSpec& dim, const Dims& dims, const Attributes& node,
                          const std::string& series, const std::string& field) {
    if (dim.fixed()) return dim.size;
    if (auto it = dims.find(dim.name); it != dims.end()) {
        if (it->second == 0) throw Error("dimension '" + dim.name + "' must not be 0");
        return it->second;
    }
    if (auto it = node.find(dim.name); it != node.end()) {
        const Value& v = it->second;
        if (const auto* i = std::get_if<std::int64_t>(&v); i != nullptr && *i > 0) {
            return static_cast<std::uint64_t>(*i);
        }
        if (const auto* a = std::get_if<std::vector<std::string>>(&v); a && !a->empty()) {
            return a->size();
        }
        if (const auto* a = std::get_if<std::vector<std::int64_t>>(&v); a && !a->empty()) {
            return a->size();
        }
        if (const auto* a = std::get_if<std::vector<double>>(&v); a && !a->empty()) {
            return a->size();
        }
    }
    throw Error("series '" + series + "', field '" + field + "': dimension '" + dim.name +
                "' is not given (pass it in dims or as a node attribute)");
}

}  // namespace

SeriesLayout SeriesLayout::make(const SeriesSpec& spec, const Dims& dims, const Attributes& node,
                                std::size_t chunk_bytes, std::uint64_t max_chunk_samples,
                                const FieldTypes& types) {
    for (const auto& [name, type] : types) {
        const FieldSpec* f = spec.field(name);
        if (f == nullptr) {
            throw Error("series '" + spec.name + "' has no field '" + name + "'");
        }
        if (!f->allows(type)) {
            throw Error("series '" + spec.name + "', field '" + name + "' cannot be " +
                        std::string(to_string(type)));
        }
    }
    SeriesLayout layout;
    layout.spec = spec;
    auto add = [&](FieldLayout f) {
        f.sample_bytes = static_cast<std::size_t>(element_count(f.sample_shape)) *
                         dtype_size(f.dtype);
        const std::uint64_t per_chunk =
            std::max<std::uint64_t>(1, chunk_bytes / std::max<std::size_t>(1, f.sample_bytes));
        f.chunk_samples = std::max<std::uint64_t>(1, std::min(per_chunk, max_chunk_samples));
        layout.fields.push_back(std::move(f));
    };
    FieldLayout time;
    time.name = time_field;
    time.dtype = DType::Int64;
    time.units = "ns";
    time.description = "Time stamp, ns since the Unix epoch (ROS time).";
    add(std::move(time));
    for (const auto& fs : spec.fields) {
        FieldLayout f;
        f.name = fs.name;
        f.dtype = fs.dtype;
        if (auto it = types.find(fs.name); it != types.end()) f.dtype = it->second;
        for (const auto& d : fs.shape) {
            f.sample_shape.push_back(resolve_dim(d, dims, node, spec.name, fs.name));
        }
        f.optional = fs.optional;
        f.units = fs.units;
        f.description = fs.description;
        add(std::move(f));
    }
    return layout;
}

std::size_t SeriesLayout::index(std::string_view name) const {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name) return i;
    }
    return std::string::npos;
}

Shape SeriesLayout::sample_shape(std::string_view name) const {
    const auto i = index(name);
    if (i == std::string::npos) {
        throw Error("series '" + spec.name + "' has no field '" + std::string(name) + "'");
    }
    return fields[i].sample_shape;
}

void stage_sample(const SeriesLayout& layout, std::vector<ChunkBuffer>& buffers,
                  std::span<const std::uint64_t> slots, std::int64_t time_ns,
                  std::span<const FieldValue> fields) {
    const auto n = layout.fields.size();
    if (buffers.size() != n) buffers.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!buffers[i]) buffers[i].reset(new std::byte[layout.fields[i].chunk_bytes()]);
    }
    auto slot_ptr = [&](std::size_t i) {
        return buffers[i].get() + slots[i] * layout.fields[i].sample_bytes;
    };

    std::memcpy(slot_ptr(0), &time_ns, sizeof(time_ns));
    std::vector<bool> given(n, false);
    given[0] = true;
    for (const auto& fv : fields) {
        const auto i = layout.index(fv.name);
        if (i == std::string::npos || i == 0) {
            throw Error("series '" + layout.spec.name + "' has no field '" +
                        std::string(fv.name) + "'");
        }
        const auto& f = layout.fields[i];
        if (given[i]) throw Error("field '" + f.name + "' given twice");
        if (fv.value.data == nullptr) throw Error("field '" + f.name + "' has no data");
        if (fv.value.size() != element_count(f.sample_shape)) {
            throw Error("field '" + f.name + "' needs " +
                        std::to_string(element_count(f.sample_shape)) + " values, got " +
                        std::to_string(fv.value.size()));
        }
        if (fv.value.dtype == DType::String) {
            throw Error("field '" + f.name + "': strings cannot be stored in a series");
        }
        convert(fv.value.dtype, fv.value.data, f.dtype, slot_ptr(i),
                static_cast<std::size_t>(fv.value.size()));
        given[i] = true;
    }
    for (std::size_t i = 1; i < n; ++i) {
        if (given[i]) continue;
        const auto& f = layout.fields[i];
        if (!f.optional) {
            throw Error("series '" + layout.spec.name + "': field '" + f.name + "' is missing");
        }
        fill_missing(f.dtype, slot_ptr(i), static_cast<std::size_t>(element_count(f.sample_shape)));
    }
}

void zero_tail(const FieldLayout& field, std::byte* buffer, std::uint64_t valid) {
    const std::size_t used = static_cast<std::size_t>(valid) * field.sample_bytes;
    if (used < field.chunk_bytes()) std::memset(buffer + used, 0, field.chunk_bytes() - used);
}

h5::Id create_field_dataset(hid_t loc, const FieldLayout& field,
                            const CompressionOptions& compression) {
    h5::Lock lock(h5::mutex());
    const int rank = static_cast<int>(field.sample_shape.size()) + 1;
    std::vector<hsize_t> dims(static_cast<std::size_t>(rank), 0);
    std::vector<hsize_t> max_dims(static_cast<std::size_t>(rank));
    std::vector<hsize_t> chunk(static_cast<std::size_t>(rank));
    max_dims[0] = H5S_UNLIMITED;
    chunk[0] = field.chunk_samples;
    for (std::size_t i = 0; i < field.sample_shape.size(); ++i) {
        dims[i + 1] = max_dims[i + 1] = chunk[i + 1] = field.sample_shape[i];
    }
    h5::Id space(h5::check_id(H5Screate_simple(rank, dims.data(), max_dims.data()),
                              "H5Screate_simple"));
    h5::Id dcpl(h5::check_id(H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate"));
    h5::check(H5Pset_chunk(dcpl.get(), rank, chunk.data()), "H5Pset_chunk");
    set_filters(dcpl.get(), compression);
    // Chunks are only ever written whole; never allocate or fill them up front.
    h5::check(H5Pset_fill_time(dcpl.get(), H5D_FILL_TIME_NEVER), "H5Pset_fill_time");
    h5::Id type = h5::file_type(field.dtype);
    h5::Id dset(h5::check_id(H5Dcreate2(loc, field.name.c_str(), type.get(), space.get(),
                                        H5P_DEFAULT, dcpl.get(), H5P_DEFAULT),
                             "creating dataset " + field.name));
    if (!field.units.empty()) h5::write_string_attribute(dset.get(), "units", field.units);
    if (!field.description.empty()) {
        h5::write_string_attribute(dset.get(), "description", field.description);
    }
    return dset;
}

void write_chunk(hid_t dataset, const FieldLayout& field, std::uint64_t chunk_index,
                 std::uint64_t extent, const std::vector<std::byte>& encoded) {
    h5::Lock lock(h5::mutex());
    const auto rank = field.sample_shape.size() + 1;
    const Shape current = h5::extent(dataset);
    if (extent > current[0]) {
        std::vector<hsize_t> dims(rank);
        dims[0] = extent;
        for (std::size_t i = 0; i < field.sample_shape.size(); ++i) {
            dims[i + 1] = field.sample_shape[i];
        }
        h5::check(H5Dset_extent(dataset, dims.data()), "H5Dset_extent");
    }
    std::vector<hsize_t> offset(rank, 0);
    offset[0] = chunk_index * field.chunk_samples;
    h5::check(H5Dwrite_chunk(dataset, H5P_DEFAULT, 0, offset.data(), encoded.size(),
                             encoded.data()),
              "H5Dwrite_chunk " + field.name);
}

}  // namespace raybotic::scan::detail
