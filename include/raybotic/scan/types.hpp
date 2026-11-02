#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "raybotic/scan/export.hpp"

namespace raybotic::scan {

/// Everything rbscan throws. Header-only on purpose, so it crosses the DLL
/// boundary without an exported base class.
class Error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Element types of attributes and series fields. String is only allowed for
/// attributes; series fields are fixed-size numbers so they can be chunked,
/// compressed and concatenated across segments.
enum class DType : std::uint8_t {
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64,
    String,
};

[[nodiscard]] RBSCAN_API std::string_view to_string(DType dtype);
/// "uint16", "float64", ... Throws Error for unknown names.
[[nodiscard]] RBSCAN_API DType dtype_from_string(std::string_view name);
/// Bytes per element; 0 for String.
[[nodiscard]] RBSCAN_API std::size_t dtype_size(DType dtype);

template <class T>
constexpr DType dtype_of();
template <>
constexpr DType dtype_of<bool>() {
    return DType::Bool;
}
template <>
constexpr DType dtype_of<std::int8_t>() {
    return DType::Int8;
}
template <>
constexpr DType dtype_of<std::uint8_t>() {
    return DType::UInt8;
}
template <>
constexpr DType dtype_of<std::int16_t>() {
    return DType::Int16;
}
template <>
constexpr DType dtype_of<std::uint16_t>() {
    return DType::UInt16;
}
template <>
constexpr DType dtype_of<std::int32_t>() {
    return DType::Int32;
}
template <>
constexpr DType dtype_of<std::uint32_t>() {
    return DType::UInt32;
}
template <>
constexpr DType dtype_of<std::int64_t>() {
    return DType::Int64;
}
template <>
constexpr DType dtype_of<std::uint64_t>() {
    return DType::UInt64;
}
template <>
constexpr DType dtype_of<float>() {
    return DType::Float32;
}
template <>
constexpr DType dtype_of<double>() {
    return DType::Float64;
}

using Shape = std::vector<std::uint64_t>;

[[nodiscard]] inline std::uint64_t element_count(const Shape& shape) {
    std::uint64_t n = 1;
    for (auto d : shape) n *= d;
    return n;
}

/// Non-owning, row-major view of numbers handed to a writer. The shape is only
/// used to check the element count against the field, so a flat buffer for a
/// [H, W] field is fine.
struct ConstArrayView {
    DType dtype{DType::Float64};
    Shape shape;
    const void* data{nullptr};

    [[nodiscard]] std::uint64_t size() const { return element_count(shape); }
    [[nodiscard]] std::size_t byte_size() const {
        return static_cast<std::size_t>(size()) * dtype_size(dtype);
    }
};

template <class T>
[[nodiscard]] ConstArrayView view(const T* data, Shape shape) {
    return {dtype_of<T>(), std::move(shape), data};
}
template <class T>
[[nodiscard]] ConstArrayView view(std::span<const T> data) {
    return {dtype_of<T>(), {data.size()}, data.data()};
}
template <class T>
[[nodiscard]] ConstArrayView view(const std::vector<T>& data) {
    return {dtype_of<T>(), {data.size()}, data.data()};
}
/// A scalar. The value must outlive the append call.
template <class T>
[[nodiscard]] ConstArrayView scalar(const T& value) {
    return {dtype_of<T>(), {}, &value};
}

/// Owning row-major array returned by readers.
class Array {
  public:
    Array() = default;
    Array(DType dtype, Shape shape)
        : dtype_(dtype),
          shape_(std::move(shape)),
          bytes_(static_cast<std::size_t>(element_count(shape_)) * dtype_size(dtype)) {}

    [[nodiscard]] DType dtype() const { return dtype_; }
    [[nodiscard]] const Shape& shape() const { return shape_; }
    [[nodiscard]] std::uint64_t size() const { return element_count(shape_); }
    [[nodiscard]] std::size_t byte_size() const { return bytes_.size(); }
    [[nodiscard]] void* data() { return bytes_.data(); }
    [[nodiscard]] const void* data() const { return bytes_.data(); }

    /// Typed access; throws if T does not match the stored dtype.
    template <class T>
    [[nodiscard]] std::span<const T> values() const& {
        if (dtype_of<T>() != dtype_) {
            throw Error("Array holds " + std::string(to_string(dtype_)) + ", not " +
                        std::string(to_string(dtype_of<T>())));
        }
        return {reinterpret_cast<const T*>(bytes_.data()),
                static_cast<std::size_t>(size())};
    }
    /// On a temporary (`reader.read(...).values<T>()`) a span would dangle
    /// once the statement ends, so this returns a copy.
    template <class T>
    [[nodiscard]] std::vector<T> values() const&& {
        const auto v = static_cast<const Array&>(*this).values<T>();
        return {v.begin(), v.end()};
    }

    /// Moves the buffer out, e.g. to hand it to numpy without a copy.
    [[nodiscard]] std::vector<std::byte> release() { return std::move(bytes_); }

  private:
    DType dtype_{DType::Float64};
    Shape shape_;
    std::vector<std::byte> bytes_;
};

/// Attribute value. Integers of every width map to int64, floats to double;
/// the schema decides the type stored in the file.
using Value =
    std::variant<bool, std::int64_t, double, std::string, std::vector<std::int64_t>,
                 std::vector<double>, std::vector<std::string>>;
using Attributes = std::map<std::string, Value>;

/// Human-readable form for logs and error messages.
[[nodiscard]] RBSCAN_API std::string to_string(const Value& value);

/// Concrete sizes for the named dimensions of a series' fields, e.g.
/// {"height", 1536}, {"width", 1536}.
using Dims = std::map<std::string, std::uint64_t, std::less<>>;

/// Data type per field for fields whose schema allows several (e.g. a
/// detector image as uint16 or float32): {"image", DType::Float32}.
using FieldTypes = std::map<std::string, DType, std::less<>>;

/// One field of a sample handed to append().
struct FieldValue {
    std::string_view name;
    ConstArrayView value;
};

}  // namespace raybotic::scan
