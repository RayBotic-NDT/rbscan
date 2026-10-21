#include "raybotic/scan/types.hpp"

#include <array>
#include <sstream>

namespace raybotic::scan {

namespace {

constexpr std::array<std::pair<DType, std::string_view>, 12> dtype_names = {{
    {DType::Bool, "bool"},
    {DType::Int8, "int8"},
    {DType::UInt8, "uint8"},
    {DType::Int16, "int16"},
    {DType::UInt16, "uint16"},
    {DType::Int32, "int32"},
    {DType::UInt32, "uint32"},
    {DType::Int64, "int64"},
    {DType::UInt64, "uint64"},
    {DType::Float32, "float32"},
    {DType::Float64, "float64"},
    {DType::String, "string"},
}};

}  // namespace

std::string_view to_string(DType dtype) {
    for (const auto& [d, name] : dtype_names) {
        if (d == dtype) return name;
    }
    return "unknown";
}

DType dtype_from_string(std::string_view name) {
    for (const auto& [d, n] : dtype_names) {
        if (n == name) return d;
    }
    throw Error("unknown dtype '" + std::string(name) + "'");
}

std::size_t dtype_size(DType dtype) {
    switch (dtype) {
        case DType::Bool:
        case DType::Int8:
        case DType::UInt8: return 1;
        case DType::Int16:
        case DType::UInt16: return 2;
        case DType::Int32:
        case DType::UInt32:
        case DType::Float32: return 4;
        case DType::Int64:
        case DType::UInt64:
        case DType::Float64: return 8;
        case DType::String: return 0;
    }
    return 0;
}

std::string to_string(const Value& value) {
    std::ostringstream out;
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                out << (v ? "true" : "false");
            } else if constexpr (std::is_same_v<T, std::string>) {
                out << '"' << v << '"';
            } else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
                out << v;
            } else {
                out << '[';
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (i > 0) out << ", ";
                    if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                        out << '"' << v[i] << '"';
                    } else {
                        out << v[i];
                    }
                }
                out << ']';
            }
        },
        value);
    return out.str();
}

}  // namespace raybotic::scan
