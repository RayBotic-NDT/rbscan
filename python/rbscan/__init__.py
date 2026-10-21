"""RayBotic scan data format.

One scan is a directory with an HDF5 master file (``scan.rbscan.h5``) and, per
segmented sensor stream, a folder of segment files that the master links as
virtual datasets::

    import rbscan

    with rbscan.ScanWriter.create("D:/scans/part_42", description="housing") as scan:
        det = scan.add_node("detector_0", "raybotic.xray_detector",
                            frame_id="scan_detector", simulated=True,
                            sensor_width=1536, sensor_height=1536,
                            pixel_pitch_u_um=139.0, pixel_pitch_v_um=139.0)
        images = det.segmented_series("image", dims={"height": 768, "width": 768})
        images.append(rbscan.now_ns(), image=frame)

    scan = rbscan.ScanReader("D:/scans/part_42")
    images = scan["sensors/detector_0"]["image"]
    stack = images.read("image", 0, 10)        # numpy [10, 768, 768] uint16

The files are plain HDF5, so h5py works as well (``h5py.File(master)``).
"""

from __future__ import annotations

import json as _json
import os as _os
import time as _time

if hasattr(_os, "add_dll_directory"):
    # rbscan.dll and rbscan_hdf5.dll sit next to the extension module.
    _os.add_dll_directory(_os.path.dirname(_os.path.abspath(__file__)))

from ._rbscan import (  # noqa: E402
    FORMAT_VERSION,
    MASTER_FILE_NAME,
    Error,
    FrameGraph,
    NodeReader,
    NodeWriter,
    ScanReader,
    ScanWriter,
    Schema,
    SegmentedSeriesWriter,
    SeriesReader,
    SeriesWriter,
    hdf5_version,
    info,
    info_text,
    projection_matrix,
    register_schema,
    register_schema_directory,
    resolve_geometry,
    schema,
    schemas,
    validate,
    validate_text,
    version,
)

__version__ = version()

__all__ = [
    "FORMAT_VERSION",
    "MASTER_FILE_NAME",
    "Error",
    "FrameGraph",
    "NodeReader",
    "NodeWriter",
    "ScanReader",
    "ScanWriter",
    "Schema",
    "SegmentedSeriesWriter",
    "SeriesReader",
    "SeriesWriter",
    "hdf5_version",
    "info",
    "info_text",
    "now_ns",
    "projection_matrix",
    "register_schema",
    "register_schema_directory",
    "resolve_geometry",
    "schema",
    "schemas",
    "validate",
    "validate_text",
    "version",
    "read_volume",
    "write_volume",
]


def now_ns() -> int:
    """Current wall-clock time in ns since the Unix epoch (same clock as ROS time)."""
    return _time.time_ns()


def _schema_to_dict(self: Schema) -> dict:
    """The flat schema as a dict (attributes, series, fields)."""
    return _json.loads(self.json)


Schema.to_dict = _schema_to_dict  # type: ignore[attr-defined]


def write_volume(scan: ScanWriter, name: str, volume, voxel_to_world, *, quantity: str = "",
                 value_units: str = "", acquisition: str = "", parameters: str = "",
                 software: str = "", world_frame: str = "world", segment_size: int = 256,
                 **options) -> NodeWriter:
    """Store a volume as volumes/<name> (schema raybotic.volume/1).

    volume: array [depth, height, width] (k, j, i) of float32, uint16 or uint8.
    voxel_to_world: 4x4 affine from voxel index (i, j, k, 1) to world (m), indices
    at voxel centres. Further keywords go to segmented_series (threads, level, ...).
    """
    import numpy as np

    v = np.ascontiguousarray(volume)
    if v.ndim != 3:
        raise ValueError("volume must be [depth, height, width]")
    a = np.asarray(voxel_to_world, dtype=float).reshape(4, 4)
    depth, height, width = v.shape
    attrs = dict(width=width, height=height, depth=depth, voxel_to_world=a.ravel().tolist(),
                 voxel_size=np.linalg.norm(a[:3, :3], axis=0).tolist(), world_frame=world_frame)
    for key, value in (("quantity", quantity), ("value_units", value_units),
                       ("acquisition", acquisition), ("parameters", parameters),
                       ("software", software)):
        if value:
            attrs[key] = value
    node = scan.add_node(name, "raybotic.volume", attrs)
    slices = node.segmented_series("slices", dtypes={"slice": v.dtype.name},
                                   segment_size=segment_size, **options)
    t = now_ns()
    for k in range(depth):
        slices.append(t, slice=v[k])
    slices.close()
    return node


def read_volume(scan, path: str):
    """A volume node as (array [depth, height, width], 4x4 voxel_to_world in m).

    scan: a ScanReader or a scan directory; path: e.g. "volumes/reco".
    """
    import numpy as np

    reader = scan if isinstance(scan, ScanReader) else ScanReader(scan)
    node = reader[path]
    a = np.asarray(node.attributes["voxel_to_world"], dtype=float).reshape(4, 4)
    return node["slices"].read("slice"), a
