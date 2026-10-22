# rbscan format 1.0

A scan is a **directory**. It holds one HDF5 master file and, for every
segmented series, a folder of segment files. All files use the HDF5 1.10 file
format (needed for SWMR and virtual datasets) and can be read with HDF5 ≥ 1.10.

## Conventions

- **Time**: every series has a dataset `time`, int64, ns since the Unix epoch
  (ROS time). It never decreases within a series. Simulations use the same clock.
- **Units**: SI, unless the field name or the schema says otherwise (`_kv`,
  `_ua`, `_mm`, `_ms`, as in the ROS messages). Every dataset carries `units`
  and `description` attributes when the schema gives them.
- **Poses**: frames as in ROS TF. A quaternion is `x, y, z, w`. A pose in one
  array is `px, py, pz, qx, qy, qz, qw`.
- **Arrays**: row-major, the first dimension is the sample index.
- **Strings**: variable-length UTF-8. Booleans use the h5py enum
  (`FALSE = 0, TRUE = 1` on int8), so numpy reads them as `bool`.

## Master file `scan.rbscan.h5`

Root attributes:

| Attribute | Type | |
|---|---|---|
| `format` | string | `raybotic.scan` |
| `format_version` | string | `1.0` |
| `uuid` | string | random UUID v4 of this scan |
| `created`, `finalized` | string | ISO 8601 UTC |
| `software` | string | `rbscan <version>` |
| `complete` | bool | true once finalized or rebuilt; false while recording or after a crash |
| `description`, `operator`, `sample_id` | string | optional |
| anything else | | `ScanInfo::extra` |

Below the root, one group per **category** and one group per **node**:

```
/sensors/<name>        raybotic.xray_source, raybotic.xray_detector, raybotic.camera, ...
/robots/<name>         raybotic.robot_arm
/frames/<child frame>  raybotic.transform
/acquisitions/<name>   raybotic.ct_acquisition
/<category>/<name>     whatever a custom schema declares
```

A node group carries:

- `schema`: the id, e.g. `raybotic.xray_detector/1`
- `schema_json`: the flat schema (with `extends` resolved), so the file
  describes itself
- its attributes, with the types the schema declares

Each series is a subgroup `<node>/<series>` with attribute `storage`
(`inline` or `segmented`) and one dataset per field plus `time`:

```
/sensors/detector_0/state/time                  int64   [N]
/sensors/detector_0/state/integration_time_ms   float64 [N]
/sensors/detector_0/image/time                  int64   [M]        (virtual)
/sensors/detector_0/image/image                 uint16  [M, H, W]  (virtual)
```

Inline datasets are chunked (about 256 KB per chunk) and compressed with
shuffle + deflate.

## Segment files

```
<scan>/<category>/<node>/<series>/seg_000000.h5
```

- Each segment holds `segment_size` samples (default 500); the last one may hold
  fewer.
- Datasets: `time` and one per field, `[n, sample shape]`, chunked
  (one image per chunk, smaller samples up to about 1 MB per chunk).
- Compression (`Compression`, `compression=` in Python):
  - `ShuffleDeflate` / `"shuffle_deflate"`: HDF5 byte shuffle +
    deflate. Built into every HDF5 installation; best compression ratio for
    16-bit images.
  - `BitshuffleLZ4` / `"bitshuffle_lz4"` (default): HDF5 filter 32008 (`bitshuffle` with
    LZ4, as used by Dectris detectors). Decodes several times faster than
    deflate at a slightly larger file size. rbscan reads and writes it natively
    (and registers filter 32008 with HDF5 for virtual datasets); external tools
    need the bitshuffle filter plugin (`import hdf5plugin` in Python before
    opening with `h5py`).
  - `Deflate` / `"deflate"`: zlib deflate without byte shuffle.
  - `None` / `"none"`: uncompressed chunks, read directly into the destination
    buffer without a copy.
- Root attributes: `format = raybotic.scan.segment`, `format_version`,
  `node_path`, `schema`, `schema_json`, `series`, `segment_index`,
  `first_sample`, `segment_size`.
- Group `node_attributes`: the node's attributes. A node recorded by another
  process can therefore be recreated in the master on finalize or rebuild.

The master's segmented series group holds one virtual dataset per field. It
maps segment *k* to rows `[first_k, first_k + n_k)`. The source file names are
relative to the master's directory, so the scan directory can be moved or
copied as a whole. The attribute `segment_count` is set as well.

### Writing

```
append() --copy--> chunk buffer --full--> thread pool (shuffle + deflate)
                                               |
                                               v (in order)
                                   writer thread: H5Dwrite_chunk, H5Dset_extent
```

- `append()` blocks only when `max_pending` chunks are waiting.
- Every `flush_every` samples (default 16), partial chunks are written and the
  file is flushed, so SWMR readers see the newest samples.
- The encoded chunks are byte-identical to what HDF5's shuffle, deflate and
  bitshuffle (32008) filters produce. On read, rbscan looks up chunk offsets
  and reads and decodes one-sample-per-chunk datasets outside the global HDF5
  lock (using its own thread pool for range reads and direct OS positional
  reads into the caller's buffer for uncompressed chunks).

### Several processes

A segmented series does not need the master file. A detector node in its own
process can call

```cpp
SegmentedSeriesWriter::open(scan_dir, "sensors/detector_0", "raybotic.xray_detector",
                            "image", dims, options, attributes);
```

The process that owns the master calls `finalize()` when everything is done.
If it finishes earlier, call `ScanWriter::rebuild(scan_dir)` afterwards. Both
scan the directory and link every segment they find.

While the master is open for writing, other processes cannot open it (HDF5
file locking). To follow a recording live, use
`SeriesReader::open_segments(scan_dir, node_path, series)` and `refresh()`.

## Projection geometry

`acquisitions/<name>/projections` holds one row per projection, in the world
frame, lengths in m. `rbscan resolve-geometry` derives it from the recorded
frames. The conventions match `raybotic_scan_pose_interfaces/ScanPose`:

- **Detector frame** (`frame_id` of the detector): its origin is the centre of
  the active area, x points to image right (increasing column), y points up,
  z is the normal towards the source. Rows therefore run along -y, so
  `detector_u = R·(1, 0, 0)` and `detector_v = R·(0, -1, 0)`.
- **Source frame** (`frame_id` of the source): its origin is the focal spot,
  z is the central beam.
- `detector_position` is the image centre, i.e. pixel coordinate
  ((W - 1) / 2, (H - 1) / 2).
- `pixel_size` = pitch × binning, where binning is the detector state in
  force at the image's time.
- `projection_matrix` maps homogeneous world points (m) to (column, row) · w.
- `flags`:

  | Bit | Meaning |
  |---|---|
  | 0 | invalid: the time lies outside the recorded frames |
  | 1 | geometry interpolated between frame samples |
  | 2 | a robot tracking the source or detector frame was unreachable |
  | 3 | emission was not on |

## Volumes

`volumes/<name>` (`raybotic.volume/1`) is a stack of slices: sample k of the
segmented series `slices` is slice k, `[height, width]` in float32, uint16 or
uint8. Attribute `voxel_to_world` (4x4, row-major, m) maps voxel index
(i, j, k, 1) = (column, row, slice, 1) to the world, with indices at voxel
centres; `voxel_size` repeats its column norms. `acquisition` and `parameters`
say what it was made from and how.

## Schemas

A schema is a JSON object:

```json
{
  "schema": "raybotic.xray_detector",
  "version": 1,
  "extends": "raybotic.sensor/1",
  "category": "sensors",
  "abstract": false,
  "description": "...",
  "attributes": {
    "sensor_width": { "dtype": "uint32", "required": true, "units": "px", "description": "..." },
    "focal_spots_um": { "dtype": "float64", "array": true }
  },
  "series": {
    "image": {
      "storage": "segmented",
      "description": "...",
      "fields": {
        "image": { "dtype": "uint16", "shape": ["height", "width"], "units": "counts" },
        "dark": { "dtype": "float32", "shape": ["height", "width"], "optional": true }
      }
    }
  }
}
```

- `dtype`: `bool`, `int8` … `uint64`, `float32`, `float64`; `string` is allowed
  for attributes only. A field may list several (`["uint16", "float32"]`); the
  first is the default and the writer picks one per series. Readers take the
  type from the data.
- `shape`: a list of fixed sizes and/or names. A name is resolved when the
  series is created: first from the explicit `dims`, then from an integer node
  attribute of that name, then from the length of an array attribute of that
  name (e.g. `["joint_names"]`).
- `storage`: `inline` (default) or `segmented`.
- `optional` fields that are left out are filled with NaN (floats) or 0 / false.
- `extends`: inherits category, attributes and series from another schema;
  entries with the same name are replaced.
- The id is `name/version`. Change the version when a field changes meaning or
  type. Adding optional fields or attributes does not need a new version.
  Readers ignore attributes they do not know, and writers may add undeclared
  attributes.

The time field is implicit; `time` is reserved as a field name.
