# rbscan — RayBotic Scan Data Format

**`rbscan`** is a high-throughput C++20 and Python library for recording, inspecting, and reading multi-sensor robotic X-ray / CT scans.

It combines the convenience of **standard HDF5** with the write/read speed and crash resilience needed during live industrial data acquisition:

- **One scan = one directory** with an HDF5 master file (`scan.rbscan.h5`) and segmented sensor streams (`seg_000000.h5`, …).
- **100 % standard HDF5**: When a scan is finalized, all segments are linked into the master file as **HDF5 Virtual Datasets (VDS)**. External tools (`h5py`, HDFView, MATLAB, ParaView) see a single contiguous `[N, H, W]` dataset without needing `rbscan`.
- **Multi-GB/s parallel I/O**: Heavy image streams are compressed on a background thread pool (`shuffle_deflate`, `bitshuffle_lz4`, or uncompressed `none`) and written via `H5Dwrite_chunk`. On read, `rbscan` bypasses the global HDF5 lock using positional OS file reads and parallel chunk decoding.
- **Multi-process & live SWMR**: Detector drivers can record segments in their own process while a viewer or reconstruction pipeline reads them live (HDF5 SWMR). If a recording crashes, `rbscan rebuild` relinks all segments on disk.
- **Self-describing schemas**: Every node (X-ray source, detector, robot arm, TF frame, CT geometry, 3D volume, annotations) is validated against a JSON schema that is embedded directly into the file.

---

## Table of Contents

1. [Directory Layout & Core Concepts](#directory-layout--core-concepts)
2. [Installation](#installation)
3. [Python Quickstart](#python-quickstart)
4. [C++ Quickstart](#c-quickstart)
5. [Compression Codecs](#compression-codecs)
6. [Command-Line Tool (`rbscan`)](#command-line-tool-rbscan)
7. [Built-in Schemas & Custom Sensors](#built-in-schemas--custom-sensors)
8. [Building from Source](#building-from-source)

---

## Directory Layout & Core Concepts

```text
part_42/
├── scan.rbscan.h5                  # Master file: metadata, frames, robot states,
│                                   # projection geometry, and virtual datasets
└── sensors/
    └── detector_0/
        └── image/
            ├── seg_000000.h5       # 500 projections per segment (configurable)
            ├── seg_000001.h5
            └── ...
```

| Concept | How it works |
| :--- | :--- |
| **Inline series** | Low-rate data (source kV/µA, robot joint states, TF poses, projection matrices) is stored directly inside `scan.rbscan.h5`. |
| **Segmented series** | High-rate streams (X-ray projections, camera images, depth maps, reconstructed volume slices) are split into segment files (`seg_000000.h5`, …) and linked into `scan.rbscan.h5` as virtual datasets on `finalize()`. |
| **Multi-process recording** | A separate process can open a `SegmentedSeriesWriter` directly in the scan directory without touching the master file; `finalize()` (or `rebuild()`) links all discovered segments into `scan.rbscan.h5`. |
| **Post-hoc editing** | `ScanWriter::open(dir)` opens an existing scan to add, replace, or remove nodes (e.g., resolved CT geometry, reconstructed 3D volumes, or viewer annotations). |

For the full on-disk specification, see [docs/format.md](docs/format.md).

---

## Installation

### Python

Install from a prebuilt wheel:

```bash
pip install rbscan-*.whl
```

> **Note on HDF5 coexistence:** The bundled HDF5 runtime is named `rbscan_hdf5.dll` (`librbscan_hdf5.so` on Linux) so it never clashes with `h5py`, PyTables, or MATLAB in the same process.

### C++ SDK (CMake)

Extract `rbscan-sdk-win64.zip` (or install from source) and point CMake's `CMAKE_PREFIX_PATH` / `rbscan_DIR` at it:

```cmake
find_package(rbscan CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE rbscan::rbscan)
```

> Because the C++ API passes standard library types (`std::string`, `std::vector`, `std::filesystem::path`) across the DLL boundary, link consumers with the same MSVC toolset and configuration (`Release`).

---

## Python Quickstart

### 1. Recording a Scan

```python
import numpy as np
import rbscan

with rbscan.ScanWriter.create("D:/scans/part_42", description="housing", sample_id="P-42") as scan:
    det = scan.add_node(
        "detector_0",
        "raybotic.xray_detector",
        frame_id="scan_detector",
        simulated=False,
        sensor_width=3072,
        sensor_height=3072,
        pixel_pitch_u_um=139.0,
        pixel_pitch_v_um=139.0,
    )

    images = det.segmented_series(
        "image",
        dims={"height": 1536, "width": 1536},
        compression="bitshuffle_lz4",  # "bitshuffle_lz4" (default), "shuffle_deflate", "deflate", "none"
    )

    frame = np.zeros((1536, 1536), dtype=np.uint16)
    images.append(rbscan.now_ns(), image=frame)
```

### 2. Reading a Scan

With `rbscan` (parallel chunk decoding outside the HDF5 lock):

```python
import rbscan

scan = rbscan.ScanReader("D:/scans/part_42")
images = scan["sensors/detector_0"]["image"]

single = images.read_sample("image", 0)    # uint16 [1536, 1536]
stack  = images.read("image", 0, 10)       # uint16 [10, 1536, 1536]
times  = images.time()                     # int64  [N] (nanoseconds since Unix epoch)
```

Or with plain `h5py` via the master file's virtual datasets:

```python
import hdf5plugin  # only needed if recorded with compression="bitshuffle_lz4"
import h5py

with h5py.File("D:/scans/part_42/scan.rbscan.h5", "r") as f:
    frame0 = f["/sensors/detector_0/image/image"][0]
```

### 3. Storing & Reading 3D Volumes

```python
import numpy as np
import rbscan

volume = np.zeros((512, 512, 512), dtype=np.float32)  # [Z, Y, X]
voxel_to_world = np.eye(4, dtype=np.float64) * 1e-4   # 0.1 mm voxels in meters
voxel_to_world[3, 3] = 1.0

rbscan.write_volume("D:/scans/part_42", "reco_0", volume, voxel_to_world, compression="bitshuffle_lz4")
vol_array, tf_matrix, attrs = rbscan.read_volume("D:/scans/part_42", "volumes/reco_0")
```

More runnable examples live in [`python/examples/`](python/examples/):
- [`record_ct_scan.py`](python/examples/record_ct_scan.py) — records a synthetic CT scan across two processes, resolves geometry, and reads it back.
- [`inspect_scan.py`](python/examples/inspect_scan.py) — prints a summary of all nodes and series in a scan.

---

## C++ Quickstart

```cpp
#include <raybotic/scan/rbscan.hpp>
namespace rs = raybotic::scan;

// --- Write ---
auto scan = rs::ScanWriter::create("D:/scans/part_42", {.description = "housing"});
auto det = scan.add_node("detector_0", "raybotic.xray_detector",
                         {{"frame_id", std::string("scan_detector")},
                          {"simulated", false},
                          {"sensor_width", std::int64_t{3072}},
                          {"sensor_height", std::int64_t{3072}},
                          {"pixel_pitch_u_um", 139.0},
                          {"pixel_pitch_v_um", 139.0}});

rs::SegmentOptions opts;
opts.compression.method = rs::Compression::BitshuffleLZ4;
auto images = det.segmented_series("image", {{"height", 1536}, {"width", 1536}}, opts);
images.append(rs::now_ns(), {{"image", rs::view(pixels.data(), {1536, 1536})}});
scan.finalize();

// --- Read ---
auto reader = rs::ScanReader::open("D:/scans/part_42");
auto series = reader.node("sensors/detector_0").series("image");
auto sample = series.read_sample("image", 0);          // single projection
auto stack  = series.read("image", 0, 10);             // 10 projections decoded in parallel
```

---

## Compression Codecs

Both `SeriesWriter` (inline) and `SegmentedSeriesWriter` (segmented) support four compression modes:

| Codec (Python / CLI) | C++ Enum | Typical Size (16-bit CT) | Read Speed (1 / 8 threads) | Best For |
| :--- | :--- | :---: | :---: | :--- |
| `"shuffle_deflate"` | `Compression::ShuffleDeflate` | ~44 % | ~0.5 / ~3.4 GB/s | Archival storage & maximum universal HDF5 compatibility (built-in filter). |
| `"bitshuffle_lz4"` *(default)* | `Compression::BitshuffleLZ4` | ~66 % | ~0.9 / ~6.0 GB/s | Fast interactive viewing & high-rate detectors (HDF5 filter `32008`, compatible with `hdf5plugin`). |
| `"deflate"` | `Compression::Deflate` | ~55 % | ~0.5 / ~3.2 GB/s | Standard zlib deflate without byte shuffle. |
| `"none"` | `Compression::None` | 100 % | ~3.1 / ~5.6 GB/s | Maximum single-thread read speed on fast local NVMe SSDs (zero-copy direct positional reads). |

---

## Command-Line Tool (`rbscan`)

The CLI is available both as a native executable (`rbscan.exe`) and via the Python package (`rbscan` or `python -m rbscan`):

```bash
rbscan info D:/scans/part_42                 # Overview of nodes, series, sample counts, rates & disk usage
rbscan validate D:/scans/part_42             # Schema, reference & integrity check (exit code 1 on error; --json for CI)
rbscan resolve-geometry D:/scans/part_42     # Compute per-projection 3x4 matrices from recorded TF frames
rbscan rebuild D:/scans/part_42              # Relink all segment files on disk after an interrupted recording
rbscan schemas [raybotic.xray_detector]      # List built-in schemas or print a schema's JSON definition
```

| Subcommand | Description |
| :--- | :--- |
| **`info`** | Prints storage mode, sample count, duration, sample rate, segment count, and raw vs. compressed size per series. Supports `--json` and `--attributes`. |
| **`validate`** | Verifies all nodes against their embedded schemas (required attributes, dtypes, shapes, named dimensions), monotonic timestamps, VDS segment links, cross-node references, unit quaternions, and orthogonal detector axes without reading image payloads. |
| **`resolve-geometry`** | Interpolates the recorded TF tree (`frames/*`, linear translation + quaternion slerp) to each projection timestamp and writes `acquisitions/<name>/projections` (source/detector positions, detector axes, $3 \times 4$ projection matrix, kV, µA, integration time, and validity flags). |
| **`rebuild`** | Scans the directory for `seg_*.h5` files, reconstructs missing node entries, and rebuilds the virtual datasets in `scan.rbscan.h5`. |

All CLI operations are also exposed in C++ (`rs::summarize`, `rs::validate`, `rs::resolve_geometry`, `rs::FrameGraph`, `rs::projection_matrix`) and Python (`rbscan.info`, `rbscan.validate`, `rbscan.resolve_geometry`, `rbscan.FrameGraph`).

---

## Built-in Schemas & Custom Sensors

| Schema ID | Category | Series & Purpose |
| :--- | :--- | :--- |
| `raybotic.sensor` *(abstract)* | `sensors` | Base schema for all sensors (`frame_id`, `simulated`, `vendor`, `model`, `serial`). |
| `raybotic.xray_source` | `sensors` | `state` — kV, µA, power, focal spot size, emission state. |
| `raybotic.xray_detector` | `sensors` | `image` (segmented; `uint16`, `float32`, or `uint8`), `state`, `calibration` (dark/flat/bad pixel map). |
| `raybotic.camera` | `sensors` | `image` (segmented RGB/grayscale camera stream). |
| `raybotic.depth_camera` | `sensors` | `depth` (segmented depth maps). |
| `raybotic.profile_scanner` | `sensors` | `profile` (segmented 2D laser line profiles). |
| `raybotic.force_torque` | `sensors` | `wrench` (6-axis force/torque measurements). |
| `raybotic.robot_arm` | `robots` | `joint_states`, `tracking` (positions, velocities, efforts). |
| `raybotic.transform` | `frames` | `pose` — dynamic or static 3D transform edge (`parent_frame` $\rightarrow$ `child_frame`). |
| `raybotic.ct_acquisition` | `acquisitions` | `scan_poses`, `projections` — resolved per-projection CT geometry. |
| `raybotic.volume` | `volumes` | `slices` (segmented 3D volume; `float32`, `uint16`, or `uint8`) + `voxel_to_world` $4 \times 4$ matrix. |
| `raybotic.annotations` | `annotations` | `markers`, `observations` + full viewer annotation document. |
| `raybotic.ezrt_import` | `imports` | `headers` — original EZRT projection headers for lossless round-trip conversion. |

### Adding a Custom Schema

A new sensor schema is a plain JSON file extending `raybotic.sensor/1` (see [`schemas/`](schemas/) for examples). You can:
1. Register it at runtime via `rs::register_schema(json)` / `rbscan.register_schema(json)`,
2. Place it in a folder listed in the `RBSCAN_SCHEMA_PATH` environment variable, or
3. Add it to `schemas/` to compile it into the library.

Because `rbscan` embeds the resolved schema JSON into every node group on disk, **readers never need the external schema file** to read or validate a scan.

---

## Building from Source

### Prerequisites

- **CMake** $\ge 3.24$
- **C++20 compiler**: Visual Studio 2022 (Windows) or GCC / Clang (Linux)
- **Python** $\ge 3.9$ (optional, when building the Python bindings or wheels)

HDF5 `1.14.6` and `zlib` are built automatically once at configure time into `.deps/` and reused across subsequent builds; `libdeflate`, `lz4`, `nlohmann_json`, `pybind11`, and `googletest` are fetched via CMake `FetchContent`.

### C++ Library, CLI & Tests

```bash
cmake --preset default
cmake --build --preset release
ctest --preset release --output-on-failure
```

To use an existing system HDF5 installation instead of building HDF5 in `.deps/`, configure with `-DRBSCAN_BUILD_HDF5=OFF -DHDF5_DIR=/path/to/hdf5/cmake`.

### Python Wheel

```bash
uv build --wheel -o dist
```

### Benchmarks

Generate a synthetic CT scan and benchmark multi-threaded read throughput:

```bash
build/bin/Release/rbscan_record_ct_scan.exe D:/scans/bench 200 1536 1536 bitshuffle_lz4
build/bin/Release/rbscan_read_benchmark.exe D:/scans/bench
```
