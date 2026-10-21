#pragma once

// RayBotic scan data format: one HDF5 master file per scan plus segment files
// per sensor stream. See docs/format.md.

#include "raybotic/scan/reader.hpp"
#include "raybotic/scan/schema.hpp"
#include "raybotic/scan/tools.hpp"
#include "raybotic/scan/types.hpp"
#include "raybotic/scan/writer.hpp"

namespace raybotic::scan {

/// Library version, e.g. "0.1.0".
[[nodiscard]] RBSCAN_API const char* version();
/// Version of the HDF5 library rbscan is linked against.
[[nodiscard]] RBSCAN_API std::string hdf5_version();

}  // namespace raybotic::scan
