#pragma once

#include <cstddef>

#include "raybotic/scan/types.hpp"

namespace raybotic::scan::detail {

/// Copies n numbers from src to dst, converting between dtypes like a C cast.
void convert(DType src_type, const void* src, DType dst_type, void* dst, std::size_t n);

/// Value of an optional field that was left out: NaN for floats, else 0/false.
void fill_missing(DType type, void* dst, std::size_t n);

}  // namespace raybotic::scan::detail
