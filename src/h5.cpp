#include "h5.hpp"

#include "compression.hpp"

#include <cmath>
#include <limits>
#include <type_traits>

namespace raybotic::scan::h5 {

namespace {

herr_t collect_error(unsigned /*n*/, const H5E_error2_t* err, void* data) {
    auto* text = static_cast<std::string*>(data);
    *text += "\n  ";
    if (err->func_name != nullptr) {
        *text += err->func_name;
        *text += ": ";
    }
    if (err->desc != nullptr) *text += err->desc;
    return 0;
}

std::string error_stack() {
    std::string text;
    H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD, collect_error, &text);
    H5Eclear2(H5E_DEFAULT);
    return text;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : path) {
        if (c == '/') {
            if (!part.empty()) parts.push_back(std::move(part));
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) parts.push_back(std::move(part));
    return parts;
}

Id copy_type(hid_t predefined) { return Id(check_id(H5Tcopy(predefined), "H5Tcopy")); }

Id bool_type() {
    // Same layout h5py uses, so numpy reads these as bool.
    Id type(check_id(H5Tenum_create(H5T_NATIVE_INT8), "H5Tenum_create"));
    std::int8_t value = 0;
    check(H5Tenum_insert(type.get(), "FALSE", &value), "H5Tenum_insert");
    value = 1;
    check(H5Tenum_insert(type.get(), "TRUE", &value), "H5Tenum_insert");
    return type;
}

Id string_type() {
    Id type = copy_type(H5T_C_S1);
    check(H5Tset_size(type.get(), H5T_VARIABLE), "H5Tset_size");
    check(H5Tset_cset(type.get(), H5T_CSET_UTF8), "H5Tset_cset");
    return type;
}

/// Attribute payload after conversion: numbers widened, the target type decides
/// what lands in the file.
struct Payload {
    DType dtype{DType::Int64};
    bool array{false};
    std::vector<std::int64_t> ints;
    std::vector<double> floats;
    std::vector<std::string> strings;
};

bool is_integer(DType d) {
    return d != DType::Bool && d != DType::Float32 && d != DType::Float64 &&
           d != DType::String;
}

void check_integer_range(DType d, std::int64_t v, const std::string& name) {
    auto fail = [&] {
        throw Error("attribute '" + name + "': " + std::to_string(v) + " does not fit " +
                    std::string(to_string(d)));
    };
    auto in = [&](auto lo, auto hi) {
        if (v < static_cast<std::int64_t>(lo) || v > static_cast<std::int64_t>(hi)) fail();
    };
    switch (d) {
        case DType::Int8: in(INT8_MIN, INT8_MAX); break;
        case DType::UInt8: in(0, UINT8_MAX); break;
        case DType::Int16: in(INT16_MIN, INT16_MAX); break;
        case DType::UInt16: in(0, UINT16_MAX); break;
        case DType::Int32: in(INT32_MIN, INT32_MAX); break;
        case DType::UInt32: in(0, UINT32_MAX); break;
        case DType::UInt64: if (v < 0) fail(); break;
        default: break;
    }
}

Payload make_payload(const std::string& name, const Value& value, std::optional<DType> as,
                     bool as_array) {
    Payload p;
    // What the value is, independent of the target.
    std::vector<std::int64_t> ints;
    std::vector<double> floats;
    std::vector<std::string> strings;
    bool value_is_array = false;
    enum class Kind { Bool, Int, Float, String } kind = Kind::Int;
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                kind = Kind::Bool;
                ints = {v ? 1 : 0};
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                kind = Kind::Int;
                ints = {v};
            } else if constexpr (std::is_same_v<T, double>) {
                kind = Kind::Float;
                floats = {v};
            } else if constexpr (std::is_same_v<T, std::string>) {
                kind = Kind::String;
                strings = {v};
            } else if constexpr (std::is_same_v<T, std::vector<std::int64_t>>) {
                kind = Kind::Int;
                ints = v;
                value_is_array = true;
            } else if constexpr (std::is_same_v<T, std::vector<double>>) {
                kind = Kind::Float;
                floats = v;
                value_is_array = true;
            } else {
                kind = Kind::String;
                strings = v;
                value_is_array = true;
            }
        },
        value);

    if (!as) {
        p.array = value_is_array;
        switch (kind) {
            case Kind::Bool: p.dtype = DType::Bool; p.ints = ints; break;
            case Kind::Int: p.dtype = DType::Int64; p.ints = ints; break;
            case Kind::Float: p.dtype = DType::Float64; p.floats = floats; break;
            case Kind::String: p.dtype = DType::String; p.strings = strings; break;
        }
        return p;
    }

    p.dtype = *as;
    p.array = as_array;
    if (value_is_array && !as_array) {
        throw Error("attribute '" + name + "' must be a scalar " + std::string(to_string(*as)));
    }
    auto mismatch = [&] {
        return Error("attribute '" + name + "' must be " + std::string(to_string(*as)) +
                     (as_array ? "[]" : "") + ", got " + to_string(value));
    };
    if (*as == DType::String) {
        if (kind != Kind::String) throw mismatch();
        p.strings = strings;
    } else if (*as == DType::Bool) {
        if (kind == Kind::Bool || kind == Kind::Int) {
            for (auto v : ints) {
                if (v != 0 && v != 1) throw mismatch();
            }
            p.ints = ints;
        } else {
            throw mismatch();
        }
    } else if (is_integer(*as)) {
        if (kind == Kind::Int || kind == Kind::Bool) {
            p.ints = ints;
        } else if (kind == Kind::Float) {
            for (double f : floats) {
                if (f != std::floor(f) || std::abs(f) > 9.0e18) throw mismatch();
                p.ints.push_back(static_cast<std::int64_t>(f));
            }
        } else {
            throw mismatch();
        }
        for (auto v : p.ints) check_integer_range(*as, v, name);
    } else {
        if (kind == Kind::Float) {
            p.floats = floats;
        } else if (kind == Kind::Int) {
            for (auto v : ints) p.floats.push_back(static_cast<double>(v));
        } else {
            throw mismatch();
        }
    }
    return p;
}

}  // namespace

std::recursive_mutex& mutex() {
    static std::recursive_mutex m;
    return m;
}

void init() {
    static std::once_flag once;
    std::call_once(once, [] {
        Lock lock(mutex());
        H5open();
        H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
        detail::register_filters();
    });
}

void check(herr_t status, std::string_view what) {
    if (status < 0) throw Error(std::string(what) + " failed" + error_stack());
}

hid_t check_id(hid_t id, std::string_view what) {
    if (id < 0) throw Error(std::string(what) + " failed" + error_stack());
    return id;
}

void Id::reset() {
    if (id_ < 0) return;
    Lock lock(mutex());
    switch (H5Iget_type(id_)) {
        case H5I_FILE: H5Fclose(id_); break;
        case H5I_GROUP: H5Gclose(id_); break;
        case H5I_DATASET: H5Dclose(id_); break;
        case H5I_DATASPACE: H5Sclose(id_); break;
        case H5I_DATATYPE: H5Tclose(id_); break;
        case H5I_ATTR: H5Aclose(id_); break;
        case H5I_GENPROP_LST: H5Pclose(id_); break;
        default: H5Idec_ref(id_); break;
    }
    H5Eclear2(H5E_DEFAULT);
    id_ = H5I_INVALID_HID;
}

Id file_type(DType dtype) {
    switch (dtype) {
        case DType::Bool: return bool_type();
        case DType::Int8: return copy_type(H5T_STD_I8LE);
        case DType::UInt8: return copy_type(H5T_STD_U8LE);
        case DType::Int16: return copy_type(H5T_STD_I16LE);
        case DType::UInt16: return copy_type(H5T_STD_U16LE);
        case DType::Int32: return copy_type(H5T_STD_I32LE);
        case DType::UInt32: return copy_type(H5T_STD_U32LE);
        case DType::Int64: return copy_type(H5T_STD_I64LE);
        case DType::UInt64: return copy_type(H5T_STD_U64LE);
        case DType::Float32: return copy_type(H5T_IEEE_F32LE);
        case DType::Float64: return copy_type(H5T_IEEE_F64LE);
        case DType::String: return string_type();
    }
    throw Error("unknown dtype");
}

Id memory_type(DType dtype) {
    switch (dtype) {
        case DType::Bool: return bool_type();
        case DType::Int8: return copy_type(H5T_NATIVE_INT8);
        case DType::UInt8: return copy_type(H5T_NATIVE_UINT8);
        case DType::Int16: return copy_type(H5T_NATIVE_INT16);
        case DType::UInt16: return copy_type(H5T_NATIVE_UINT16);
        case DType::Int32: return copy_type(H5T_NATIVE_INT32);
        case DType::UInt32: return copy_type(H5T_NATIVE_UINT32);
        case DType::Int64: return copy_type(H5T_NATIVE_INT64);
        case DType::UInt64: return copy_type(H5T_NATIVE_UINT64);
        case DType::Float32: return copy_type(H5T_NATIVE_FLOAT);
        case DType::Float64: return copy_type(H5T_NATIVE_DOUBLE);
        case DType::String: return string_type();
    }
    throw Error("unknown dtype");
}

DType dtype_of(hid_t type) {
    const auto size = H5Tget_size(type);
    switch (H5Tget_class(type)) {
        case H5T_ENUM:
            if (size == 1) return DType::Bool;
            break;
        case H5T_INTEGER: {
            const bool is_signed = H5Tget_sign(type) == H5T_SGN_2;
            switch (size) {
                case 1: return is_signed ? DType::Int8 : DType::UInt8;
                case 2: return is_signed ? DType::Int16 : DType::UInt16;
                case 4: return is_signed ? DType::Int32 : DType::UInt32;
                case 8: return is_signed ? DType::Int64 : DType::UInt64;
                default: break;
            }
            break;
        }
        case H5T_FLOAT:
            if (size == 4) return DType::Float32;
            if (size == 8) return DType::Float64;
            break;
        case H5T_STRING: return DType::String;
        default: break;
    }
    throw Error("unsupported HDF5 datatype");
}

Id create_file(const std::string& path, bool overwrite, bool /*swmr_capable*/) {
    Lock lock(mutex());
    Id fapl(check_id(H5Pcreate(H5P_FILE_ACCESS), "H5Pcreate"));
    // 1.10 format: needed for SWMR and virtual datasets, readable by HDF5 >= 1.10.
    check(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
          "H5Pset_libver_bounds");
    Id fcpl(check_id(H5Pcreate(H5P_FILE_CREATE), "H5Pcreate"));
    check(H5Pset_link_creation_order(fcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
          "H5Pset_link_creation_order");
    const unsigned flags = overwrite ? H5F_ACC_TRUNC : H5F_ACC_EXCL;
    return Id(check_id(H5Fcreate(path.c_str(), flags, fcpl.get(), fapl.get()),
                       "creating " + path));
}

Id open_file(const std::string& path, unsigned flags, bool swmr_read) {
    Lock lock(mutex());
    Id fapl(check_id(H5Pcreate(H5P_FILE_ACCESS), "H5Pcreate"));
    if (flags & H5F_ACC_RDWR) {
        check(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
              "H5Pset_libver_bounds");
    }
    if (swmr_read) flags |= H5F_ACC_SWMR_READ;
    return Id(check_id(H5Fopen(path.c_str(), flags, fapl.get()), "opening " + path));
}

bool exists(hid_t loc, const std::string& path) {
    Lock lock(mutex());
    std::string prefix;
    for (const auto& part : split_path(path)) {
        prefix += (prefix.empty() ? "" : "/") + part;
        const htri_t e = H5Lexists(loc, prefix.c_str(), H5P_DEFAULT);
        if (e <= 0) {
            H5Eclear2(H5E_DEFAULT);
            return false;
        }
    }
    return true;
}

Id ensure_group(hid_t loc, const std::string& path) {
    Lock lock(mutex());
    if (exists(loc, path)) return open_group(loc, path);
    Id lcpl(check_id(H5Pcreate(H5P_LINK_CREATE), "H5Pcreate"));
    check(H5Pset_create_intermediate_group(lcpl.get(), 1), "H5Pset_create_intermediate_group");
    Id gcpl(check_id(H5Pcreate(H5P_GROUP_CREATE), "H5Pcreate"));
    check(H5Pset_link_creation_order(gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
          "H5Pset_link_creation_order");
    return Id(check_id(H5Gcreate2(loc, path.c_str(), lcpl.get(), gcpl.get(), H5P_DEFAULT),
                       "creating group " + path));
}

Id open_group(hid_t loc, const std::string& path) {
    Lock lock(mutex());
    return Id(check_id(H5Gopen2(loc, path.c_str(), H5P_DEFAULT), "opening group " + path));
}

Id open_dataset(hid_t loc, const std::string& path) {
    Lock lock(mutex());
    return Id(check_id(H5Dopen2(loc, path.c_str(), H5P_DEFAULT), "opening dataset " + path));
}

std::vector<std::string> children(hid_t group) {
    Lock lock(mutex());
    std::vector<std::string> names;
    auto cb = [](hid_t, const char* name, const H5L_info2_t*, void* data) -> herr_t {
        static_cast<std::vector<std::string>*>(data)->emplace_back(name);
        return 0;
    };
    // Creation order where the group tracks it, else by name.
    if (H5Literate2(group, H5_INDEX_CRT_ORDER, H5_ITER_INC, nullptr, cb, &names) < 0) {
        H5Eclear2(H5E_DEFAULT);
        names.clear();
        check(H5Literate2(group, H5_INDEX_NAME, H5_ITER_INC, nullptr, cb, &names),
              "H5Literate2");
    }
    return names;
}

bool is_group(hid_t loc, const std::string& name) {
    Lock lock(mutex());
    H5O_info2_t info;
    if (H5Oget_info_by_name3(loc, name.c_str(), &info, H5O_INFO_BASIC, H5P_DEFAULT) < 0) {
        H5Eclear2(H5E_DEFAULT);
        return false;
    }
    return info.type == H5O_TYPE_GROUP;
}

void write_attribute(hid_t obj, const std::string& name, const Value& value,
                     std::optional<DType> as, bool as_array) {
    const Payload p = make_payload(name, value, as, as_array);
    Lock lock(mutex());
    if (H5Aexists(obj, name.c_str()) > 0) check(H5Adelete(obj, name.c_str()), "H5Adelete");
    H5Eclear2(H5E_DEFAULT);

    const std::size_t n = p.dtype == DType::String ? p.strings.size()
                          : (p.dtype == DType::Float32 || p.dtype == DType::Float64)
                              ? p.floats.size()
                              : p.ints.size();
    Id space;
    if (p.array) {
        const hsize_t dims[1] = {n};
        space = Id(check_id(H5Screate_simple(1, dims, nullptr), "H5Screate_simple"));
    } else {
        space = Id(check_id(H5Screate(H5S_SCALAR), "H5Screate"));
    }
    Id ftype = file_type(p.dtype);
    Id attr(check_id(H5Acreate2(obj, name.c_str(), ftype.get(), space.get(), H5P_DEFAULT,
                                H5P_DEFAULT),
                     "creating attribute " + name));
    if (p.dtype == DType::String) {
        std::vector<const char*> ptrs;
        for (const auto& s : p.strings) ptrs.push_back(s.c_str());
        if (ptrs.empty()) return;
        Id mtype = memory_type(DType::String);
        check(H5Awrite(attr.get(), mtype.get(), ptrs.data()), "writing attribute " + name);
    } else if (p.dtype == DType::Bool) {
        std::vector<std::int8_t> bytes;
        for (auto v : p.ints) bytes.push_back(static_cast<std::int8_t>(v));
        if (bytes.empty()) return;
        Id mtype = memory_type(DType::Bool);
        check(H5Awrite(attr.get(), mtype.get(), bytes.data()), "writing attribute " + name);
    } else if (p.dtype == DType::Float32 || p.dtype == DType::Float64) {
        if (p.floats.empty()) return;
        check(H5Awrite(attr.get(), H5T_NATIVE_DOUBLE, p.floats.data()),
              "writing attribute " + name);
    } else {
        if (p.ints.empty()) return;
        check(H5Awrite(attr.get(), H5T_NATIVE_INT64, p.ints.data()), "writing attribute " + name);
    }
}

void write_string_attribute(hid_t obj, const std::string& name, std::string_view value) {
    write_attribute(obj, name, Value(std::string(value)), DType::String);
}

std::optional<Value> read_attribute(hid_t obj, const std::string& name) {
    Lock lock(mutex());
    if (H5Aexists(obj, name.c_str()) <= 0) {
        H5Eclear2(H5E_DEFAULT);
        return std::nullopt;
    }
    Id attr(check_id(H5Aopen(obj, name.c_str(), H5P_DEFAULT), "opening attribute " + name));
    Id type(check_id(H5Aget_type(attr.get()), "H5Aget_type"));
    Id space(check_id(H5Aget_space(attr.get()), "H5Aget_space"));
    const int rank = H5Sget_simple_extent_ndims(space.get());
    const auto n = static_cast<std::size_t>(H5Sget_simple_extent_npoints(space.get()));
    const bool array = rank > 0;

    DType dtype;
    try {
        dtype = dtype_of(type.get());
    } catch (const Error&) {
        return std::nullopt;  // compound, reference, ...: not ours
    }

    if (dtype == DType::String) {
        std::vector<std::string> strings;
        if (H5Tis_variable_str(type.get()) > 0) {
            std::vector<char*> ptrs(n, nullptr);
            Id mtype = memory_type(DType::String);
            if (n > 0) {
                check(H5Aread(attr.get(), mtype.get(), ptrs.data()), "reading attribute " + name);
            }
            for (char* p : ptrs) strings.emplace_back(p != nullptr ? p : "");
            if (n > 0) H5Treclaim(mtype.get(), space.get(), H5P_DEFAULT, ptrs.data());
        } else {
            const std::size_t len = H5Tget_size(type.get());
            std::vector<char> buf(len * n);
            if (n > 0) {
                check(H5Aread(attr.get(), type.get(), buf.data()), "reading attribute " + name);
            }
            for (std::size_t i = 0; i < n; ++i) {
                std::string s(buf.data() + i * len, len);
                s.resize(s.find('\0') == std::string::npos ? len : s.find('\0'));
                while (!s.empty() && s.back() == ' ') s.pop_back();
                strings.push_back(std::move(s));
            }
        }
        if (array) return Value(std::move(strings));
        return Value(strings.empty() ? std::string() : strings.front());
    }
    if (dtype == DType::Float32 || dtype == DType::Float64) {
        std::vector<double> v(n);
        if (n > 0) check(H5Aread(attr.get(), H5T_NATIVE_DOUBLE, v.data()), "reading " + name);
        if (array) return Value(std::move(v));
        return Value(v.empty() ? 0.0 : v.front());
    }
    if (dtype == DType::Bool) {
        std::vector<std::int8_t> v(n);
        Id mtype = memory_type(DType::Bool);
        if (n > 0) check(H5Aread(attr.get(), mtype.get(), v.data()), "reading " + name);
        if (array) return Value(std::vector<std::int64_t>(v.begin(), v.end()));
        return Value(!v.empty() && v.front() != 0);
    }
    std::vector<std::int64_t> v(n);
    if (n > 0) check(H5Aread(attr.get(), H5T_NATIVE_INT64, v.data()), "reading " + name);
    if (array) return Value(std::move(v));
    return Value(v.empty() ? std::int64_t{0} : v.front());
}

std::optional<std::string> read_string_attribute(hid_t obj, const std::string& name) {
    auto v = read_attribute(obj, name);
    if (!v) return std::nullopt;
    if (const auto* s = std::get_if<std::string>(&*v)) return *s;
    return std::nullopt;
}

Attributes read_attributes(hid_t obj) {
    Lock lock(mutex());
    std::vector<std::string> names;
    auto cb = [](hid_t, const char* name, const H5A_info_t*, void* data) -> herr_t {
        static_cast<std::vector<std::string>*>(data)->emplace_back(name);
        return 0;
    };
    hsize_t idx = 0;
    check(H5Aiterate2(obj, H5_INDEX_NAME, H5_ITER_INC, &idx, cb, &names), "H5Aiterate2");
    Attributes out;
    for (const auto& name : names) {
        if (auto v = read_attribute(obj, name)) out.emplace(name, std::move(*v));
    }
    return out;
}

Shape extent(hid_t dataset) {
    Lock lock(mutex());
    Id space(check_id(H5Dget_space(dataset), "H5Dget_space"));
    const int rank = H5Sget_simple_extent_ndims(space.get());
    std::vector<hsize_t> dims(static_cast<std::size_t>(rank));
    H5Sget_simple_extent_dims(space.get(), dims.data(), nullptr);
    return Shape(dims.begin(), dims.end());
}

}  // namespace raybotic::scan::h5
