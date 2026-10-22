// Python bindings: numpy in, numpy out. The heavy lifting (conversion,
// compression, I/O) runs with the GIL released.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <raybotic/scan/rbscan.hpp>

namespace py = pybind11;
namespace rs = raybotic::scan;

namespace {

// ---------------------------------------------------------------------------
// Values and arrays

rs::Value to_value(const py::handle& obj) {
    if (py::isinstance<py::bool_>(obj) ||
        py::isinstance(obj, py::module_::import("numpy").attr("bool_"))) {
        return obj.cast<bool>();
    }
    if (py::isinstance<py::str>(obj)) return obj.cast<std::string>();
    if (py::isinstance<py::int_>(obj) ||
        py::isinstance(obj, py::module_::import("numpy").attr("integer"))) {
        return obj.cast<std::int64_t>();
    }
    if (py::isinstance<py::float_>(obj) ||
        py::isinstance(obj, py::module_::import("numpy").attr("floating"))) {
        return obj.cast<double>();
    }
    if (py::isinstance<py::array>(obj) || py::isinstance<py::sequence>(obj)) {
        py::list items;
        if (py::isinstance<py::array>(obj)) {
            items = obj.attr("ravel")().attr("tolist")();
        } else {
            for (auto item : obj) items.append(item);
        }
        bool all_str = true;
        bool all_int = true;
        for (auto item : items) {
            all_str = all_str && py::isinstance<py::str>(item);
            all_int = all_int &&
                      (py::isinstance<py::int_>(item) || py::isinstance<py::bool_>(item));
        }
        if (items.empty()) return std::vector<double>{};
        if (all_str) return items.cast<std::vector<std::string>>();
        if (all_int) return items.cast<std::vector<std::int64_t>>();
        return items.cast<std::vector<double>>();
    }
    throw rs::Error("cannot store a " +
                    std::string(py::str(py::type::handle_of(obj).attr("__name__"))) +
                    " as an attribute");
}

py::object from_value(const rs::Value& value) {
    return std::visit([](const auto& v) -> py::object { return py::cast(v); }, value);
}

rs::Attributes to_attributes(const py::handle& mapping) {
    rs::Attributes out;
    if (mapping.is_none()) return out;
    for (auto [key, value] : mapping.cast<py::dict>()) {
        out.emplace(key.cast<std::string>(), to_value(value));
    }
    return out;
}

py::dict from_attributes(const rs::Attributes& attrs) {
    py::dict out;
    for (const auto& [key, value] : attrs) out[py::str(key)] = from_value(value);
    return out;
}

rs::Dims to_dims(const py::handle& mapping) {
    rs::Dims out;
    if (mapping.is_none()) return out;
    for (auto [key, value] : mapping.cast<py::dict>()) {
        out.emplace(key.cast<std::string>(), value.cast<std::uint64_t>());
    }
    return out;
}

py::dtype numpy_dtype(rs::DType d) {
    switch (d) {
        case rs::DType::Bool:
            return py::dtype::of<bool>();
        case rs::DType::Int8:
            return py::dtype::of<std::int8_t>();
        case rs::DType::UInt8:
            return py::dtype::of<std::uint8_t>();
        case rs::DType::Int16:
            return py::dtype::of<std::int16_t>();
        case rs::DType::UInt16:
            return py::dtype::of<std::uint16_t>();
        case rs::DType::Int32:
            return py::dtype::of<std::int32_t>();
        case rs::DType::UInt32:
            return py::dtype::of<std::uint32_t>();
        case rs::DType::Int64:
            return py::dtype::of<std::int64_t>();
        case rs::DType::UInt64:
            return py::dtype::of<std::uint64_t>();
        case rs::DType::Float32:
            return py::dtype::of<float>();
        case rs::DType::Float64:
            return py::dtype::of<double>();
        case rs::DType::String:
            break;
    }
    throw rs::Error("no numpy dtype for strings");
}

rs::DType dtype_from_numpy(const py::dtype& dt) {
    const char kind = dt.kind();
    const auto size = dt.itemsize();
    if (kind == 'b') return rs::DType::Bool;
    if (kind == 'i') {
        switch (size) {
            case 1:
                return rs::DType::Int8;
            case 2:
                return rs::DType::Int16;
            case 4:
                return rs::DType::Int32;
            case 8:
                return rs::DType::Int64;
            default:
                break;
        }
    }
    if (kind == 'u') {
        switch (size) {
            case 1:
                return rs::DType::UInt8;
            case 2:
                return rs::DType::UInt16;
            case 4:
                return rs::DType::UInt32;
            case 8:
                return rs::DType::UInt64;
            default:
                break;
        }
    }
    if (kind == 'f' && size == 4) return rs::DType::Float32;
    if (kind == 'f' && size == 8) return rs::DType::Float64;
    throw rs::Error("unsupported array dtype " + std::string(py::str(dt)));
}

py::array to_numpy(rs::Array array) {
    const auto dt = numpy_dtype(array.dtype());
    std::vector<py::ssize_t> shape(array.shape().begin(), array.shape().end());
    auto* bytes = new std::vector<std::byte>(array.release());
    py::capsule owner(bytes,
                      [](void* p) { delete static_cast<std::vector<std::byte>*>(p); });
    return py::array(dt, shape, bytes->data(), owner);
}

/// Fields of one sample: a dict and/or keyword arguments, values anything
/// numpy can turn into an array. Keeps the arrays alive for the C++ call.
struct Sample {
    std::vector<py::array> arrays;
    std::vector<std::string> names;
    std::vector<rs::FieldValue> fields;

    Sample(const py::object& dict, const py::kwargs& kwargs) {
        auto add = [&](const py::handle& key, const py::handle& value) {
            py::array a = py::module_::import("numpy").attr("ascontiguousarray")(value);
            if (a.dtype().kind() == 'f' && a.dtype().itemsize() == 2) {
                a = a.attr("astype")("float32");
            }
            names.push_back(key.cast<std::string>());
            arrays.push_back(a);
        };
        if (!dict.is_none()) {
            for (auto [key, value] : dict.cast<py::dict>()) add(key, value);
        }
        for (auto [key, value] : kwargs) add(key, value);
        for (std::size_t i = 0; i < arrays.size(); ++i) {
            const auto& a = arrays[i];
            rs::ConstArrayView v;
            v.dtype = dtype_from_numpy(a.dtype());
            v.shape.assign(a.shape(), a.shape() + a.ndim());
            v.data = a.data();
            fields.push_back({names[i], v});
        }
    }
};

rs::FieldTypes to_field_types(const py::handle& mapping) {
    rs::FieldTypes out;
    if (mapping.is_none()) return out;
    for (auto [key, value] : mapping.cast<py::dict>()) {
        // A dtype name, a numpy dtype or a numpy type (np.float32).
        const std::string name =
            py::str(py::module_::import("numpy").attr("dtype")(value).attr("name"));
        out.emplace(key.cast<std::string>(), rs::dtype_from_string(name));
    }
    return out;
}

rs::CompressionOptions compression_options(const std::string& method, int level) {
    rs::CompressionOptions c;
    if (method == "none") {
        c.method = rs::Compression::None;
    } else if (method == "deflate") {
        c.method = rs::Compression::Deflate;
    } else if (method == "shuffle_deflate") {
        c.method = rs::Compression::ShuffleDeflate;
    } else if (method == "bitshuffle_lz4") {
        c.method = rs::Compression::BitshuffleLZ4;
    } else {
        throw rs::Error(
            "compression must be none, deflate, shuffle_deflate or bitshuffle_lz4");
    }
    c.level = level;
    return c;
}

rs::SegmentOptions segment_options(std::uint64_t segment_size,
                                   const std::string& compression, int level,
                                   unsigned threads, std::size_t max_pending,
                                   std::size_t chunk_bytes, bool swmr,
                                   std::uint64_t flush_every, const py::object& dtypes) {
    rs::SegmentOptions o;
    o.dtypes = to_field_types(dtypes);
    o.segment_size = segment_size;
    o.compression = compression_options(compression, level);
    o.threads = threads;
    o.max_pending = max_pending;
    o.chunk_bytes = chunk_bytes;
    o.swmr = swmr;
    o.flush_every = flush_every;
    return o;
}

const char* storage_name(rs::Storage s) {
    return s == rs::Storage::Segmented ? "segmented" : "inline";
}

std::uint64_t count_arg(const py::object& count) {
    return count.is_none() ? rs::all : count.cast<std::uint64_t>();
}

py::dict sample_dict(const rs::SeriesReader& s, std::uint64_t index) {
    py::dict out;
    out["time"] = s.time(index, 1).at(0);
    for (const auto& f : s.fields()) {
        rs::Array a = s.read_sample(f, index);
        out[py::str(f)] = to_numpy(std::move(a));
    }
    return out;
}

}  // namespace

PYBIND11_MODULE(_rbscan, m) {
    m.doc() = "RayBotic scan data format (HDF5 master file + segmented sensor streams)";

    py::register_exception<rs::Error>(m, "Error", PyExc_RuntimeError);

    m.def("version", [] { return std::string(rs::version()); });
    m.def("hdf5_version", &rs::hdf5_version);
    m.attr("MASTER_FILE_NAME") = std::string(rs::master_file_name);
    m.attr("FORMAT_VERSION") = std::string(rs::format_version);

    // --- schemas -----------------------------------------------------------
    py::class_<rs::Schema>(m, "Schema")
        .def_property_readonly("id", &rs::Schema::id)
        .def_readonly("name", &rs::Schema::name)
        .def_readonly("version", &rs::Schema::version)
        .def_readonly("category", &rs::Schema::category)
        .def_readonly("description", &rs::Schema::description)
        .def_readonly("abstract", &rs::Schema::abstract)
        .def_property_readonly("json", &rs::Schema::to_json)
        .def_property_readonly("series_names",
                               [](const rs::Schema& s) {
                                   std::vector<std::string> out;
                                   for (const auto& x : s.series) out.push_back(x.name);
                                   return out;
                               })
        .def_property_readonly("attribute_names",
                               [](const rs::Schema& s) {
                                   std::vector<std::string> out;
                                   for (const auto& x : s.attributes)
                                       out.push_back(x.name);
                                   return out;
                               })
        .def("__repr__",
             [](const rs::Schema& s) { return "<rbscan.Schema " + s.id() + ">"; });

    m.def(
        "schemas", [] { return rs::SchemaRegistry::global().ids(); },
        "Ids of all registered schemas.");
    m.def(
        "schema",
        [](const std::string& id) { return &rs::SchemaRegistry::global().get(id); },
        py::return_value_policy::reference, py::arg("id"),
        "A registered schema by 'name/version' or 'name' (newest version).");
    m.def(
        "register_schema",
        [](const std::string& json) {
            return &rs::SchemaRegistry::global().add_json(json);
        },
        py::return_value_policy::reference, py::arg("json"),
        "Adds a schema given as JSON text; returns it (flattened).");
    m.def(
        "register_schema_directory",
        [](const std::filesystem::path& dir) {
            rs::SchemaRegistry::global().add_directory(dir);
        },
        py::arg("directory"));

    // --- writers -----------------------------------------------------------
    py::class_<rs::SeriesWriter>(m, "SeriesWriter")
        .def(
            "append",
            [](rs::SeriesWriter& w, std::int64_t time_ns, const py::object& fields,
               const py::kwargs& kwargs) {
                Sample sample(fields, kwargs);
                py::gil_scoped_release release;
                w.append(time_ns, sample.fields);
            },
            py::arg("time_ns"), py::arg("fields") = py::none(),
            "Appends one sample: time in ns plus every field (dict and/or keywords).")
        .def("flush", &rs::SeriesWriter::flush, py::call_guard<py::gil_scoped_release>())
        .def("__len__", &rs::SeriesWriter::size)
        .def_property_readonly("name",
                               [](const rs::SeriesWriter& w) { return w.spec().name; })
        .def_property_readonly("fields",
                               [](const rs::SeriesWriter& w) {
                                   std::vector<std::string> out;
                                   for (const auto& f : w.spec().fields)
                                       out.push_back(f.name);
                                   return out;
                               })
        .def("sample_shape", [](const rs::SeriesWriter& w, const std::string& f) {
            const auto s = w.sample_shape(f);
            return py::tuple(py::cast(std::vector<std::uint64_t>(s.begin(), s.end())));
        });

    py::class_<rs::SegmentedSeriesWriter>(m, "SegmentedSeriesWriter")
        .def_static(
            "open",
            [](const std::filesystem::path& scan_dir, const std::string& node_path,
               const std::string& schema, const std::string& series,
               const py::object& dims, const py::object& attributes,
               std::uint64_t segment_size, const std::string& compression, int level,
               unsigned threads, std::size_t max_pending, std::size_t chunk_bytes,
               bool swmr, std::uint64_t flush_every, const py::object& dtypes) {
                return rs::SegmentedSeriesWriter::open(
                    scan_dir, node_path, schema, series, to_dims(dims),
                    segment_options(segment_size, compression, level, threads,
                                    max_pending, chunk_bytes, swmr, flush_every, dtypes),
                    to_attributes(attributes));
            },
            py::arg("scan_dir"), py::arg("node_path"), py::arg("schema"),
            py::arg("series"), py::arg("dims") = py::none(),
            py::arg("attributes") = py::none(), py::arg("segment_size") = 500,
            py::arg("compression") = "bitshuffle_lz4", py::arg("level") = 1,
            py::arg("threads") = 0, py::arg("max_pending") = 64,
            py::arg("chunk_bytes") = 1 << 20, py::arg("swmr") = true,
            py::arg("flush_every") = 16, py::arg("dtypes") = py::none(),
            "Writer for a segmented series without the master file (e.g. in another "
            "process).")
        .def(
            "append",
            [](rs::SegmentedSeriesWriter& w, std::int64_t time_ns,
               const py::object& fields, const py::kwargs& kwargs) {
                Sample sample(fields, kwargs);
                py::gil_scoped_release release;
                w.append(time_ns, sample.fields);
            },
            py::arg("time_ns"), py::arg("fields") = py::none())
        .def("flush", &rs::SegmentedSeriesWriter::flush,
             py::call_guard<py::gil_scoped_release>())
        .def("close", &rs::SegmentedSeriesWriter::close,
             py::call_guard<py::gil_scoped_release>())
        .def("__len__", &rs::SegmentedSeriesWriter::size)
        .def_property_readonly("segment_count", &rs::SegmentedSeriesWriter::segment_count)
        .def_property_readonly("directory", &rs::SegmentedSeriesWriter::directory)
        .def_property_readonly(
            "name", [](const rs::SegmentedSeriesWriter& w) { return w.spec().name; })
        .def("sample_shape",
             [](const rs::SegmentedSeriesWriter& w, const std::string& f) {
                 const auto s = w.sample_shape(f);
                 return py::tuple(
                     py::cast(std::vector<std::uint64_t>(s.begin(), s.end())));
             })
        .def("__enter__", [](rs::SegmentedSeriesWriter& w) { return w; })
        .def("__exit__", [](rs::SegmentedSeriesWriter& w, const py::object&,
                            const py::object&, const py::object&) {
            py::gil_scoped_release release;
            w.close();
        });

    py::class_<rs::NodeWriter>(m, "NodeWriter")
        .def_property_readonly("path", &rs::NodeWriter::path)
        .def_property_readonly("schema", &rs::NodeWriter::schema,
                               py::return_value_policy::reference_internal)
        .def("set_attribute",
             [](rs::NodeWriter& n, const std::string& name, const py::object& value) {
                 n.set_attribute(name, to_value(value));
             })
        .def(
            "series",
            [](rs::NodeWriter& n, const std::string& name, const py::object& dims,
               const std::string& compression, int level, std::size_t chunk_bytes,
               const py::object& dtypes) {
                rs::InlineOptions o;
                o.compression = compression_options(compression, level);
                o.chunk_bytes = chunk_bytes;
                o.dtypes = to_field_types(dtypes);
                return n.series(name, to_dims(dims), o);
            },
            py::arg("name"), py::arg("dims") = py::none(),
            py::arg("compression") = "bitshuffle_lz4", py::arg("level") = 1,
            py::arg("chunk_bytes") = 256 * 1024, py::arg("dtypes") = py::none(),
            "Writer of an inline series (master file); dtypes picks another allowed type "
            "per field.")
        .def(
            "segmented_series",
            [](rs::NodeWriter& n, const std::string& name, const py::object& dims,
               std::uint64_t segment_size, const std::string& compression, int level,
               unsigned threads, std::size_t max_pending, std::size_t chunk_bytes,
               bool swmr, std::uint64_t flush_every, const py::object& dtypes) {
                return n.segmented_series(
                    name, to_dims(dims),
                    segment_options(segment_size, compression, level, threads,
                                    max_pending, chunk_bytes, swmr, flush_every, dtypes));
            },
            py::arg("name"), py::arg("dims") = py::none(), py::arg("segment_size") = 500,
            py::arg("compression") = "bitshuffle_lz4", py::arg("level") = 1,
            py::arg("threads") = 0, py::arg("max_pending") = 64,
            py::arg("chunk_bytes") = 1 << 20, py::arg("swmr") = true,
            py::arg("flush_every") = 16, py::arg("dtypes") = py::none(),
            "Writer of a segmented series (segment files); dtypes picks another allowed "
            "type "
            "per field, e.g. {'image': 'float32'}.");

    py::class_<rs::ScanWriter>(m, "ScanWriter")
        .def_static(
            "create",
            [](const std::filesystem::path& dir, const std::string& description,
               const std::string& operator_name, const std::string& sample_id,
               bool overwrite, const py::kwargs& extra) {
                rs::ScanInfo info;
                info.description = description;
                info.operator_name = operator_name;
                info.sample_id = sample_id;
                info.extra = to_attributes(extra);
                return rs::ScanWriter::create(dir, info, overwrite);
            },
            py::arg("scan_dir"), py::arg("description") = "", py::arg("operator") = "",
            py::arg("sample_id") = "", py::arg("overwrite") = false,
            "Creates a scan directory with its master file. Extra keywords become root "
            "attributes.")
        .def(
            "add_node",
            [](rs::ScanWriter& w, const std::string& name, const std::string& schema,
               const py::object& attributes, const py::kwargs& kwargs) {
                rs::Attributes attrs = to_attributes(attributes);
                for (auto& [k, v] : to_attributes(kwargs)) attrs[k] = v;
                return w.add_node(name, schema, attrs);
            },
            py::arg("name"), py::arg("schema"), py::arg("attributes") = py::none(),
            "Adds a node (sensor, robot, frame, ...); attributes as dict and/or "
            "keywords.")
        .def_static("open", &rs::ScanWriter::open, py::arg("scan_dir"),
                    "Opens an existing scan to add or replace nodes.")
        .def("node", &rs::ScanWriter::node, py::arg("path"))
        .def("has_node", &rs::ScanWriter::has_node, py::arg("path"))
        .def("remove_node", &rs::ScanWriter::remove_node, py::arg("path"))
        .def("flush", &rs::ScanWriter::flush, py::call_guard<py::gil_scoped_release>())
        .def("finalize", &rs::ScanWriter::finalize,
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("directory", &rs::ScanWriter::directory)
        .def_static("rebuild", &rs::ScanWriter::rebuild, py::arg("scan_dir"),
                    py::call_guard<py::gil_scoped_release>(),
                    "Relinks the segments of an existing scan and marks it complete.")
        .def("__enter__", [](py::object self) { return self; })
        .def("__exit__", [](rs::ScanWriter& w, const py::object&, const py::object&,
                            const py::object&) {
            py::gil_scoped_release release;
            w.finalize();
        });

    // --- readers -----------------------------------------------------------
    py::class_<rs::SeriesReader>(m, "SeriesReader")
        .def_static(
            "open_segments", &rs::SeriesReader::open_segments, py::arg("scan_dir"),
            py::arg("node_path"), py::arg("series"),
            "Reads a segmented series from its segment files while it is recorded.")
        .def_property_readonly("name", &rs::SeriesReader::name)
        .def_property_readonly(
            "storage",
            [](const rs::SeriesReader& s) { return storage_name(s.storage()); })
        .def_property_readonly("fields", &rs::SeriesReader::fields)
        .def("__len__", &rs::SeriesReader::size)
        .def("dtype", [](const rs::SeriesReader& s,
                         const std::string& f) { return numpy_dtype(s.dtype(f)); })
        .def("sample_shape",
             [](const rs::SeriesReader& s, const std::string& f) {
                 const auto shape = s.sample_shape(f);
                 return py::tuple(
                     py::cast(std::vector<std::uint64_t>(shape.begin(), shape.end())));
             })
        .def("units", &rs::SeriesReader::units)
        .def(
            "time",
            [](const rs::SeriesReader& s, std::uint64_t start, const py::object& count) {
                std::vector<std::int64_t> t;
                const auto n = count_arg(count);
                {
                    py::gil_scoped_release release;
                    t = s.time(start, n);
                }
                py::array_t<std::int64_t> out(static_cast<py::ssize_t>(t.size()));
                std::copy(t.begin(), t.end(), out.mutable_data());
                return out;
            },
            py::arg("start") = 0, py::arg("count") = py::none(), "Time stamps in ns.")
        .def(
            "read",
            [](const rs::SeriesReader& s, const std::string& field, std::uint64_t start,
               const py::object& count) {
                const auto n = count_arg(count);
                rs::Array a;
                {
                    py::gil_scoped_release release;
                    a = s.read(field, start, n);
                }
                return to_numpy(std::move(a));
            },
            py::arg("field"), py::arg("start") = 0, py::arg("count") = py::none(),
            "Samples [start, start+count) of a field as one array [count, "
            "*sample_shape].")
        .def(
            "read_sample",
            [](const rs::SeriesReader& s, const std::string& field, std::uint64_t index) {
                rs::Array a;
                {
                    py::gil_scoped_release release;
                    a = s.read_sample(field, index);
                }
                return to_numpy(std::move(a));
            },
            py::arg("field"), py::arg("index"))
        .def("index_at", &rs::SeriesReader::index_at, py::arg("time_ns"),
             "Index of the last sample with time <= time_ns, or None.")
        .def("refresh", &rs::SeriesReader::refresh,
             py::call_guard<py::gil_scoped_release>())
        .def("__getitem__", [](const rs::SeriesReader& s, std::int64_t index) {
            const auto n = static_cast<std::int64_t>(s.size());
            if (index < 0) index += n;
            if (index < 0 || index >= n) throw py::index_error();
            return sample_dict(s, static_cast<std::uint64_t>(index));
        });

    py::class_<rs::NodeReader>(m, "NodeReader")
        .def_property_readonly("path", &rs::NodeReader::path)
        .def_property_readonly("name", &rs::NodeReader::name)
        .def_property_readonly("category", &rs::NodeReader::category)
        .def_property_readonly("schema", &rs::NodeReader::schema,
                               py::return_value_policy::reference_internal)
        .def_property_readonly(
            "attributes",
            [](const rs::NodeReader& n) { return from_attributes(n.attributes()); })
        .def("attribute",
             [](const rs::NodeReader& n, const std::string& name) -> py::object {
                 auto v = n.attribute(name);
                 return v ? from_value(*v) : py::none();
             })
        .def_property_readonly("series_names", &rs::NodeReader::series_names)
        .def("has_series", &rs::NodeReader::has_series)
        .def("series", &rs::NodeReader::series, py::arg("name"))
        .def("__getitem__", &rs::NodeReader::series)
        .def("__contains__", &rs::NodeReader::has_series)
        .def("__repr__", [](const rs::NodeReader& n) {
            return "<rbscan.NodeReader " + n.path() + " (" + n.schema().id() + ")>";
        });

    // --- tools -------------------------------------------------------------
    auto json_loads = [](const std::string& text) {
        return py::module_::import("json").attr("loads")(text);
    };
    m.def(
        "info",
        [json_loads](const std::filesystem::path& scan) {
            std::string text;
            {
                py::gil_scoped_release release;
                text = rs::to_json(rs::summarize(scan));
            }
            return json_loads(text);
        },
        py::arg("scan"),
        "Overview of a scan as a dict (nodes, series, samples, rates, sizes).");
    m.def(
        "info_text",
        [](const std::filesystem::path& scan, bool attributes) {
            return rs::to_text(rs::summarize(scan), attributes);
        },
        py::arg("scan"), py::arg("attributes") = false,
        py::call_guard<py::gil_scoped_release>(),
        "Overview of a scan as printable text.");
    m.def(
        "validate",
        [json_loads](const std::filesystem::path& scan, bool check_values,
                     double tolerance) {
            rs::ValidateOptions o;
            o.check_values = check_values;
            o.time_tolerance_s = tolerance;
            std::string text;
            {
                py::gil_scoped_release release;
                text = rs::to_json(rs::validate(scan, o));
            }
            return json_loads(text);
        },
        py::arg("scan"), py::arg("check_values") = true,
        py::arg("time_tolerance_s") = 1e-3,
        "Checks a scan; returns {'ok', 'errors', 'warnings', 'issues': [...]}.");
    m.def(
        "validate_text",
        [](const std::filesystem::path& scan, bool check_values, double tolerance) {
            rs::ValidateOptions o;
            o.check_values = check_values;
            o.time_tolerance_s = tolerance;
            const auto r = rs::validate(scan, o);
            return std::make_pair(rs::to_text(r), r.ok());
        },
        py::arg("scan"), py::arg("check_values") = true,
        py::arg("time_tolerance_s") = 1e-3, py::call_guard<py::gil_scoped_release>(),
        "(report text, ok)");
    m.def(
        "resolve_geometry",
        [](const std::filesystem::path& scan, const std::string& acquisition,
           const std::string& source, const std::string& detector,
           const std::string& world_frame, const std::string& source_frame,
           const std::string& detector_frame, bool projection_matrix, bool overwrite) {
            rs::GeometryOptions o;
            o.acquisition = acquisition;
            o.source = source;
            o.detector = detector;
            o.world_frame = world_frame;
            o.source_frame = source_frame;
            o.detector_frame = detector_frame;
            o.projection_matrix = projection_matrix;
            o.overwrite = overwrite;
            rs::GeometryResult r;
            {
                py::gil_scoped_release release;
                r = rs::resolve_geometry(scan, o);
            }
            py::dict out;
            out["acquisition"] = r.acquisition;
            out["projections"] = r.projections;
            out["interpolated"] = r.interpolated;
            out["extrapolated"] = r.extrapolated;
            out["robot_unreachable"] = r.robot_unreachable;
            out["emission_unstable"] = r.emission_unstable;
            return out;
        },
        py::arg("scan"), py::arg("acquisition") = "scan_0", py::arg("source") = "",
        py::arg("detector") = "", py::arg("world_frame") = "world",
        py::arg("source_frame") = "", py::arg("detector_frame") = "",
        py::arg("projection_matrix") = true, py::arg("overwrite") = false,
        "Writes acquisitions/<acquisition>/projections from the frames, the detector's "
        "image "
        "times and pixel pitch and the source state.");
    m.def(
        "projection_matrix",
        [](const std::array<double, 3>& s, const std::array<double, 3>& d,
           const std::array<double, 3>& u, const std::array<double, 3>& v, double pu,
           double pv, std::uint64_t width, std::uint64_t height) {
            const auto pm = rs::projection_matrix(s, d, u, v, pu, pv, width, height);
            py::array_t<double> out({3, 4});
            std::copy(pm.begin(), pm.end(), out.mutable_data());
            return out;
        },
        py::arg("source_position"), py::arg("detector_position"), py::arg("detector_u"),
        py::arg("detector_v"), py::arg("pixel_u"), py::arg("pixel_v"), py::arg("width"),
        py::arg("height"), "3x4 matrix: world point (m) -> (column, row), homogeneous.");

    py::class_<rs::FrameGraph>(m, "FrameGraph")
        .def(py::init([](const std::filesystem::path& scan) {
                 return rs::FrameGraph::load(scan);
             }),
             py::arg("scan"), "The TF tree stored under frames/.")
        .def(
            "lookup",
            [](const rs::FrameGraph& g, const std::string& parent,
               const std::string& child, std::int64_t t) {
                bool extrapolated = false;
                const rs::Pose p = g.lookup(parent, child, t, &extrapolated);
                py::array_t<double> tr(3);
                py::array_t<double> rot(4);
                std::copy(p.translation.begin(), p.translation.end(), tr.mutable_data());
                std::copy(p.rotation.begin(), p.rotation.end(), rot.mutable_data());
                return py::make_tuple(tr, rot, extrapolated);
            },
            py::arg("parent"), py::arg("child"), py::arg("time_ns"),
            "Pose of child in parent at time_ns: (translation, quaternion xyzw, "
            "extrapolated).")
        .def("has_frame", &rs::FrameGraph::has_frame)
        .def_property_readonly("frames", &rs::FrameGraph::frames);

    py::class_<rs::ScanReader>(m, "ScanReader")
        .def(py::init(
                 [](const std::filesystem::path& p) { return rs::ScanReader::open(p); }),
             py::arg("path"), "Opens a scan directory or its master file.")
        .def_property_readonly("directory", &rs::ScanReader::directory)
        .def_property_readonly("format_version", &rs::ScanReader::format_version)
        .def_property_readonly("complete", &rs::ScanReader::complete)
        .def_property_readonly(
            "attributes",
            [](const rs::ScanReader& r) { return from_attributes(r.attributes()); })
        .def_property_readonly("categories", &rs::ScanReader::categories)
        .def("node_paths", &rs::ScanReader::node_paths, py::arg("category") = "")
        .def("has_node", &rs::ScanReader::has_node)
        .def("node", &rs::ScanReader::node, py::arg("path"))
        .def("__getitem__", &rs::ScanReader::node)
        .def("__contains__", &rs::ScanReader::has_node)
        .def("__enter__", [](py::object self) { return self; })
        .def("__exit__", [](const rs::ScanReader&, const py::object&, const py::object&,
                            const py::object&) {});
}
