# Typed API per schema

The generic API takes any schema: attributes as a map, samples as named fields.
On top of it, rbscan ships a typed API for every built-in schema, in C++ and
Python, plus helpers for schemas whose tables belong together.

| Layer | C++ | Python |
|---|---|---|
| Generic | `raybotic/scan/rbscan.hpp` | `rbscan.ScanWriter`, `rbscan.ScanReader` |
| Typed, per schema (generated) | `raybotic/scan/schemas.hpp` | `rbscan.schema_api` |
| Related tables (hand-written) | `raybotic/scan/schema_helpers.hpp` | `rbscan.write_simulation_scene`, ... |

The typed layer is **generated** from [`schemas/*.json`](../schemas) by
[`tools/gen_schema_api.py`](../tools/gen_schema_api.py), so it always matches
the schemas (field meanings and units: [schemas.md](schemas.md)). After changing
a schema, run `python tools/gen_schema_api.py`; the CI job `lint:generated`
fails while the generated files are out of date. Both layers sit on the
generic API and write the same files.

## Bulk writes

`SeriesWriter::append_many` (and `SegmentedSeriesWriter::append_many`) appends
many samples in one call: one time stamp per sample, or one for all, plus
whole columns `[count, sample shape...]`. Each sample is converted and checked
as with `append()`. The typed `Columns` writers use it; in Python it saves one
call per row:

```python
series.append_many(t, position=vertices)            # one time for all rows
series.append_many(times, position=..., angle=...)  # one time per row
```

## C++: `raybotic/scan/schemas.hpp`

Header-only; `rbscan.hpp` does not include it. One namespace per schema,
`raybotic::scan::schemas::<name>` (`xray_source`, `ct_acquisition`, ...), with

| Name | What |
|---|---|
| `schema_id`, `category` | e.g. `"raybotic.xray_source/1"`, `"sensors"` |
| `Attributes` | the node's attributes; optional ones are `std::optional`, undeclared ones go to `extra`. `to_attributes()` / `from_attributes()` convert from and to the generic map |
| `add(scan, name, attrs)` | adds the node, returns its `NodeWriter` |
| `read(node)` | the attributes of a `NodeReader`; throws for a missing required one |
| `<s>_series` | the series name, e.g. `state_series` |
| `<S>Row` | one sample: `time_ns` plus every field. Scalars as their C++ type, fixed shapes as `std::array` (a 4x4 as 16 values), named shapes and fields with several dtypes as `Array`; optional fields `std::optional` (left empty: not written) |
| `<S>Columns` | many samples: one `std::vector` per field (`bool` as 0/1 bytes, fixed shapes as `std::vector<std::array>`), an `Array [count, ...]` for array fields; `row(i)` picks one |
| `append(writer, row)` / `append(writer, columns)` | writes to a `SeriesWriter` or `SegmentedSeriesWriter`; columns in one `append_many` |
| `read_<s>(node, start, count)` | columns; empty when the node has no such series |
| `read_<s>_rows(node, start, count)` | the same as rows |

A member whose name is a C++ keyword gets a trailing underscore
(`transform::Attributes::static_`).

```cpp
#include <raybotic/scan/rbscan.hpp>
#include <raybotic/scan/schemas.hpp>

namespace rs = raybotic::scan;
namespace src = raybotic::scan::schemas::xray_source;

auto scan = rs::ScanWriter::create("D:/scans/part_42");
src::Attributes a;
a.frame_id = "scan_source";
a.simulated = true;
a.voltage_max_kv = 225.0;
auto node = src::add(scan, "source_0", a);
auto state = node.series(src::state_series);

src::StateRow s;
s.time_ns = t;
s.emission_state = 2;
s.voltage_kv = 120.0;  // ... every non-optional field
src::append(state, s);
scan.finalize();

const auto reader = rs::ScanReader::open("D:/scans/part_42");
const auto source = reader.node("sensors/source_0");
const src::Attributes back = src::read(source);
const src::StateColumns c = src::read_state(source);  // c.voltage_kv[i], ...
```

## Python: `rbscan.schema_api`

Per schema a dataclass of its attributes, per series a dataclass of numpy
columns:

| Name | What |
|---|---|
| `XraySource` (CamelCase of the schema) | required attributes first, optional ones default `None`, undeclared ones in `extra`. `SCHEMA`, `CATEGORY`, `SERIES` (series name to class) |
| `.add(scan, name)` | adds the node, returns its `NodeWriter` |
| `XraySource.read(node)` / `.from_attributes(dict)` | from a `NodeReader`; `ValueError` for a missing required attribute |
| `XraySourceState` (schema + series) | `time` plus one array per field `[count, *sample_shape]`; optional columns default `None` (not written) |
| `.append_to(writer)` | all samples in one `append_many` |
| `XraySourceState.read(node, start=0, count=None)` | columns; empty when the node has no such series |
| `BY_SCHEMA` | attribute class by schema id |

```python
import numpy as np
import rbscan
from rbscan import schema_api as sc

with rbscan.ScanWriter.create(path) as scan:
    node = sc.XraySource(frame_id="scan_source", simulated=True).add(scan, "source_0")
    n = len(t)
    sc.XraySourceState(time=t, emission_state=np.full(n, 2), emission_permitted=np.ones(n, bool),
                       voltage_setpoint_kv=kv, current_setpoint_ua=ua, voltage_kv=kv,
                       current_ua=ua, power_w=kv * ua * 1e-3).append_to(node.series("state"))

node = rbscan.ScanReader(path)["sensors/source_0"]
src = sc.XraySource.read(node)
state = sc.XraySourceState.read(node)  # state.voltage_kv: numpy array
```

## Related tables

Some schemas spread one thing over several tables linked by row ranges, ids
or name lists. The helpers write and read them as one, set the links
themselves and write each table in one go.

| Schema | C++ (`schema_helpers.hpp`, in the schema's namespace) | Python (`rbscan.`) |
|---|---|---|
| `raybotic.volume/1` | `volume::write(scan, name, voxels, voxel_to_world, attrs)`, `volume::read_volume(node)` | `write_volume`, `read_volume` |
| `raybotic.simulation_scene/1` | `simulation_scene::write(scan, name, attrs, objects)`, `read_scene(node)`; `Object`, `RegistrationPoint` | `write_simulation_scene`, `read_simulation_scene`; `SceneObject`, `RegistrationPoint` |
| `raybotic.trajectory_plan/1` | `trajectory_plan::write(scan, name, attrs, trajectories)`, `read_plan(node)`; `Trajectory` | `write_trajectory_plan`, `read_trajectory_plan`; `Trajectory` |
| `raybotic.annotations/1` | `annotations::write(scan, name, attrs, markers)`, `read_annotations(node)`; `Marker` | `write_annotations`, `read_annotations`; `Marker` |
| `raybotic.ct_acquisition/1` | `ct_acquisition::read_projection(scan, acquisition, index)` | `read_projection(scan, path, index)` |

- **Simulation scene**: each object brings its mesh in file units (float32
  vertices, triangles counted from its own first vertex), placement and
  registration points; the `first_*` / `*_count` ranges and the name lists
  (`object_names`, ..., `registration_markers`) are set on write and resolved
  on read. Meshes are stored bit for bit.
- **Trajectory plan**: trajectories in drive order, each with its poses;
  `first_pose`, `pose_count` and `poses.trajectory` are set on write.
- **Annotations**: markers with their observations; observations are written
  in time order (series need it) and grouped by marker on read; each marker's
  `observations` count is set on write.
- **CT projection**: a row of `projections` with the detector image it refers
  to (`image_index` into the detector node the acquisition names).

```cpp
#include <raybotic/scan/schema_helpers.hpp>

namespace scene = raybotic::scan::schemas::simulation_scene;

scene::Object part;
part.name = "Nozzle";
part.material = "Ti";
part.placement.scale = 1.0;
part.placement.enabled = true;
part.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
part.triangles = {{0, 1, 2}};
scene::Attributes a;
a.application = "my tool 1.0";
a.format = "raybotic-simulation-scene/1";
a.document = scene_json;
scene::write(scan, "scene", a, {part});

const auto [attrs, objects] = scene::read_scene(reader.node("simulations/scene"));
```

```python
part = rbscan.SceneObject(name="Nozzle", material="Ti", vertices=v, triangles=f)
attrs = sc.SimulationScene(application="my tool 1.0",
                           format="raybotic-simulation-scene/1", document=scene_json)
rbscan.write_simulation_scene(scan, "scene", attrs, [part])
attrs, objects = rbscan.read_simulation_scene(reader["simulations/scene"])
```

## Runnable examples

Each one writes a scan, reads it back and prints what it found.

| What | C++ ([`examples/`](../examples)) | Python ([`python/examples/`](../python/examples)) |
|---|---|---|
| CT scan: source, detector, images and geometry as columns; a projection with its image | `typed_ct_scan.cpp` | `typed_ct_scan.py` |
| Simulation scene from STL files (or built-in shapes), meshes checked bit for bit | `simulation_scene.cpp` | `simulation_scene.py` (`--export` writes the meshes back as STL) |
| Trajectory plan (circle, laminography) and markers seen in projections | `plan_and_annotate.cpp` | `plan_and_annotate.py` |
| Every node of any scan through its typed classes | | `dump_typed.py` |

```bash
build/bin/Release/rbscan_simulation_scene.exe D:/scans/scene part.stl holder.stl
python python/examples/dump_typed.py D:/scans/scene
```
