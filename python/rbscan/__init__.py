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
    # rbscan.dll and hdf5.dll sit next to the extension module.
    _os.add_dll_directory(_os.path.dirname(_os.path.abspath(__file__)))

from ._rbscan import (  # noqa: E402
    FORMAT_VERSION,
    MASTER_FILE_NAME,
    Error,
    NodeReader,
    NodeWriter,
    ScanReader,
    ScanWriter,
    Schema,
    SegmentedSeriesWriter,
    SeriesReader,
    SeriesWriter,
    hdf5_version,
    register_schema,
    register_schema_directory,
    schema,
    schemas,
    version,
)

__version__ = version()

__all__ = [
    "FORMAT_VERSION",
    "MASTER_FILE_NAME",
    "Error",
    "NodeReader",
    "NodeWriter",
    "ScanReader",
    "ScanWriter",
    "Schema",
    "SegmentedSeriesWriter",
    "SeriesReader",
    "SeriesWriter",
    "hdf5_version",
    "now_ns",
    "register_schema",
    "register_schema_directory",
    "schema",
    "schemas",
    "version",
]


def now_ns() -> int:
    """Current wall-clock time in ns since the Unix epoch (same clock as ROS time)."""
    return _time.time_ns()


def _schema_to_dict(self: Schema) -> dict:
    """The flat schema as a dict (attributes, series, fields)."""
    return _json.loads(self.json)


Schema.to_dict = _schema_to_dict  # type: ignore[attr-defined]
