#include "convert.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace raybotic::scan::detail {

namespace {

template <class F>
void dispatch(DType type, F&& f) {
    switch (type) {
        case DType::Bool:
            f(bool{});
            return;
        case DType::Int8:
            f(std::int8_t{});
            return;
        case DType::UInt8:
            f(std::uint8_t{});
            return;
        case DType::Int16:
            f(std::int16_t{});
            return;
        case DType::UInt16:
            f(std::uint16_t{});
            return;
        case DType::Int32:
            f(std::int32_t{});
            return;
        case DType::UInt32:
            f(std::uint32_t{});
            return;
        case DType::Int64:
            f(std::int64_t{});
            return;
        case DType::UInt64:
            f(std::uint64_t{});
            return;
        case DType::Float32:
            f(float{});
            return;
        case DType::Float64:
            f(double{});
            return;
        case DType::String:
            break;
    }
    throw Error("strings cannot be stored in a series");
}

}  // namespace

void convert(DType src_type, const void* src, DType dst_type, void* dst, std::size_t n) {
    if (src_type == dst_type) {
        std::memcpy(dst, src, n * dtype_size(src_type));
        return;
    }
    dispatch(src_type, [&](auto s) {
        using S = decltype(s);
        dispatch(dst_type, [&](auto d) {
            using D = decltype(d);
            const S* in = static_cast<const S*>(src);
            D* out = static_cast<D*>(dst);
            if constexpr (std::is_same_v<D, bool>) {
                for (std::size_t i = 0; i < n; ++i) out[i] = in[i] != S{};
            } else {
                for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<D>(in[i]);
            }
        });
    });
}

void fill_missing(DType type, void* dst, std::size_t n) {
    dispatch(type, [&](auto t) {
        using T = decltype(t);
        T* out = static_cast<T*>(dst);
        if constexpr (std::is_floating_point_v<T>) {
            std::fill(out, out + n, std::numeric_limits<T>::quiet_NaN());
        } else {
            std::fill(out, out + n, T{});
        }
    });
}

}  // namespace raybotic::scan::detail
