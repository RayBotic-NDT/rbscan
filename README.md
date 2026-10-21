# rbscan — RayBotic scan data format

One format for everything a robot-guided X-ray scan produces: robot joint
states, frames, X-ray source and detector parameters, projections, and any
other sensor that comes along later.

- **C++ library** `rbscan.dll` (`raybotic::scan`, headers in `include/`)
- **Python package** `rbscan` (pybind11, numpy in / numpy out)
- **Schemas** per sensor / data type in `schemas/*.json`, compiled into the
  library and stored in every file

## Layout

```
part_42/
├── scan.rbscan.h5                  master: metadata, frames, robots, states,
│                                   projection table, virtual datasets
└── sensors/
    └── detector_0/
        └── image/
            ├── seg_000000.h5       500 projections each (configurable)
            ├── seg_000001.h5
            └── ...
```

- Small data (states, poses, joint states, the projection table) is stored
  **inline** in the master file.
- Large streams (projections, camera images, point clouds) are **segmented**:
  each has its own writer, which can run in its own thread or process, and its
  own segment files. Full chunks are compressed by a thread pool and written
  with HDF5 direct chunk write, so `append()` only copies the sample and returns.
- `finalize()` links all segments into the master as **virtual datasets**, so
  plain HDF5 tools (h5py, HDFView, MATLAB) see one `[N, H, W]` dataset.
- Segment files are written in **SWMR** mode, so a recording can be read while
  it is running (`SeriesReader::open_segments`).
- After a crash, or when other processes finished later,
  `ScanWriter::rebuild(dir)` relinks everything it finds on disk.
- `ScanWriter::open(dir)` adds or replaces nodes in an existing scan later
  (annotations, results); `remove_node()` drops one. The viewer stores its
  markers that way.

The full description is in [docs/format.md](docs/format.md).

## Schemas

| Schema | Category | Series |
|---|---|---|
| `raybotic.sensor` (abstract) | sensors | – (frame_id, simulated, vendor, …) |
| `raybotic.xray_source` | sensors | `state` (kV, µA, W, focal spot, emission) |
| `raybotic.xray_detector` | sensors | `image` (segmented; uint16, float32 or uint8), `state`, `calibration` (dark/flat/bad pixels) |
| `raybotic.camera` | sensors | `image` (segmented) |
| `raybotic.depth_camera` | sensors | `depth` (segmented) |
| `raybotic.profile_scanner` | sensors | `profile` (segmented) |
| `raybotic.force_torque` | sensors | `wrench` |
| `raybotic.robot_arm` | robots | `joint_states`, `tracking` |
| `raybotic.transform` | frames | `pose` (one TF edge) |
| `raybotic.ct_acquisition` | acquisitions | `scan_poses`, `projections` (resolved geometry per projection) |
| `raybotic.annotations` | annotations | `markers`, `observations` (+ the viewer's full document) |
| `raybotic.ezrt_import` | imports | `headers` (original EZRT headers, for a lossless way back) |
| `raybotic.volume` | volumes | `slices` (segmented; float32, uint16 or uint8) + `voxel_to_world` |

A field may allow several data types (`"dtype": ["uint16", "float32"]`, the
first is the default); the writer picks one per series with
`SegmentOptions::dtypes` / `dtypes={"image": "float32"}` in Python, e.g. for
corrected or simulated projections. Volumes: `rbscan.write_volume(scan, name,
array, voxel_to_world)` and `rbscan.read_volume(scan, "volumes/<name>")`.

A new sensor is a new JSON file. It extends `raybotic.sensor/1` and declares
its attributes and series. Register it at runtime (`register_schema`), or put it
in a directory listed in `RBSCAN_SCHEMA_PATH`, or add it to `schemas/` and it is
compiled in. Files carry their schemas, so readers do not need the registry.

## Tools

The same commands come as `rbscan.exe` (C++, built with the library) and as
`rbscan` / `python -m rbscan` from the Python package:

```bash
rbscan info D:/scans/part_42                 # nodes, series, samples, time span, rate, size
rbscan validate D:/scans/part_42             # exit code 1 on errors; --json for CI
rbscan resolve-geometry D:/scans/part_42     # projection table from the recorded frames
rbscan rebuild D:/scans/part_42              # relink segments after a crash
rbscan schemas [raybotic.xray_detector]
```

- **info**: overview per node and series: storage, samples, duration, rate,
  segments, raw size vs. size on disk; `--json` for scripts, `--attributes`
  for every node attribute.
- **validate**: checks the scan against its schemas (required attributes and
  their types, fields, dtypes, shapes, named dimensions), equal field lengths,
  time order, the segment sequence and the virtual dataset links, references
  between nodes (`source`/`detector`, `image_index`, time of projection vs.
  image) and values of small fields (NaN in required fields, unit quaternions,
  unit and orthogonal detector axes, projections flagged invalid). Images are
  never read.
- **resolve-geometry**: writes `acquisitions/<name>/projections` from the TF
  frames under `frames/` (interpolated to each image's time: linear for
  translations, slerp for rotations), the detector's pixel pitch and binning
  and the source state. One row per image: source and detector position,
  detector axes, pixel size, projection matrix, kV, µA, integration time and
  flags (bit 0: outside the recorded frames, bit 1: interpolated, bit 2: robot
  unreachable, bit 3: emission not on).

In code: `rs::summarize`, `rs::validate`, `rs::resolve_geometry`,
`rs::FrameGraph` (TF lookups on a scan) and `rs::projection_matrix`, plus
`rbscan.info`, `rbscan.validate`, `rbscan.resolve_geometry` and
`rbscan.FrameGraph` in Python.

## C++

```cpp
#include <raybotic/scan/rbscan.hpp>
namespace rs = raybotic::scan;

auto scan = rs::ScanWriter::create("D:/scans/part_42", {.description = "housing"});
auto det = scan.add_node("detector_0", "raybotic.xray_detector",
                         {{"frame_id", std::string("scan_detector")}, {"simulated", false},
                          {"sensor_width", 3072}, {"sensor_height", 3072},
                          {"pixel_pitch_u_um", 139.0}, {"pixel_pitch_v_um", 139.0}});
auto images = det.segmented_series("image", {{"height", 1536}, {"width", 1536}});
images.append(t_ns, {{"image", rs::view(pixels, {1536, 1536})}});
scan.finalize();

auto reader = rs::ScanReader::open("D:/scans/part_42");
auto stack = reader.node("sensors/detector_0").series("image").read("image", 0, 10);
```

In CMake, `find_package(rbscan)` + `target_link_libraries(app PRIVATE rbscan::rbscan)`.
The C++ API passes STL types across the DLL boundary, so the consumer must use
the same MSVC toolset and the same configuration (Debug/Release) as rbscan.

## Python

```python
import rbscan

with rbscan.ScanWriter.create("D:/scans/part_42", description="housing") as scan:
    det = scan.add_node("detector_0", "raybotic.xray_detector", frame_id="scan_detector",
                        simulated=False, sensor_width=3072, sensor_height=3072,
                        pixel_pitch_u_um=139.0, pixel_pitch_v_um=139.0)
    images = det.segmented_series("image", dims={"height": 1536, "width": 1536})
    images.append(rbscan.now_ns(), image=frame)          # numpy uint16 [1536, 1536]

scan = rbscan.ScanReader("D:/scans/part_42")
images = scan["sensors/detector_0"]["image"]
stack = images.read("image", 0, 10)                       # [10, 1536, 1536] uint16
```

Examples in `python/examples/`:

- `record_ct_scan.py <dir>` records a synthetic CT scan: source, robots, frames
  and the projection table in the main process, and the detector images in a
  separate process (`SegmentedSeriesWriter.open`). It then finalizes the scan
  and reads it back, with rbscan and with h5py.
- `inspect_scan.py <dir>` prints what a scan holds.

## Build

Needs CMake ≥ 3.24 and Visual Studio 2022 (or GCC/Clang on Linux). HDF5 1.14.6
and zlib are built **once** at configure time into `.deps/` (a few minutes);
libdeflate, nlohmann_json, pybind11 and googletest come in via FetchContent.

```bash
cmake --preset default
cmake --build --preset release
ctest --preset release
```

Python wheel (uses the same `.deps/`):

```bash
uv build --wheel -o dist
```

To build from an existing HDF5 install instead, set `-DRBSCAN_BUILD_HDF5=OFF`
and point `HDF5_DIR` at it.

The bundled HDF5 is named `rbscan_hdf5.dll`. Windows loads only one module per
file name into a process, so a plain `hdf5.dll` would clash with h5py's
(or MATLAB's) in the same process.

`examples/record_ct_scan.cpp` writes a synthetic 360-projection scan with
robots, frames and geometry, and prints the write rate:

```bash
build/bin/Release/rbscan_record_ct_scan.exe D:/scans/test 360 1536
```
