"""Records a small CT scan with the typed API (rbscan.schema_api), then reads it back.

Dataclasses instead of keyword attributes and field names: a typo or a missing
required field fails when the object is built, not when the file is read. Each
table goes in with one call (append_many under the hood).

    python typed_ct_scan.py D:/scans/typed [--projections 90] [--size 256]
"""

from __future__ import annotations

import argparse
import math

import numpy as np

import rbscan
from rbscan import schema_api as sc

FOD, FDD, PIXEL = 0.4, 1.0, 139e-6  # m
PERIOD_NS = 200_000_000  # one projection every 200 ms


def disc_images(n: int, size: int) -> np.ndarray:
    """[n, size, size] uint16: a disc that moves with the angle."""
    y, x = np.mgrid[0:size, 0:size]
    angles = 2 * np.pi * np.arange(n) / n
    cx = 0.5 * size * (1 + 0.4 * np.cos(angles))[:, None, None]
    inside = (x - cx) ** 2 + (y - 0.5 * size) ** 2 < (0.15 * size) ** 2
    return np.where(inside, 20000, 52000).astype(np.uint16)


def record(path: str, n: int, size: int) -> None:
    t0 = rbscan.now_ns()
    t = t0 + np.arange(n, dtype=np.int64) * PERIOD_NS
    with rbscan.ScanWriter.create(path, description="typed CT scan", overwrite=True) as scan:
        source = sc.XraySource(frame_id="scan_source", simulated=True, voltage_max_kv=225.0,
                               focal_spots_um=[5.0, 20.0, 80.0]).add(scan, "source_0")
        detector = sc.XrayDetector(frame_id="scan_detector", simulated=True,
                                   sensor_width=size, sensor_height=size,
                                   pixel_pitch_u_um=PIXEL * 1e6,
                                   pixel_pitch_v_um=PIXEL * 1e6).add(scan, "detector_0")
        acq = sc.CtAcquisition(type="ct_scan", source=source.path, detector=detector.path,
                               world_frame="world",
                               trajectory="circle, 360 deg").add(scan, "ct_0")

        # Settings: one row each (columns of length 1). Optional columns left
        # None are not written.
        one = np.array([t0])
        sc.XraySourceState(time=one, emission_state=np.array([2]),
                           emission_permitted=np.array([True]),
                           voltage_setpoint_kv=np.array([120.0]),
                           current_setpoint_ua=np.array([50.0]),
                           voltage_kv=np.array([120.0]), current_ua=np.array([50.0]),
                           power_w=np.array([6.0])).append_to(source.series("state"))
        sc.XrayDetectorState(time=one, binning=np.array([1]), image_width=np.array([size]),
                             image_height=np.array([size]),
                             integration_time_ms=np.array([100.0]), averages=np.array([1]),
                             gain=np.array([1.0])).append_to(detector.series("state"))

        # All images and all geometry rows in one call each.
        with detector.segmented_series("image", dims={"height": size, "width": size}) as w:
            sc.XrayDetectorImage(time=t, image=disc_images(n, size)).append_to(w)
        a = 2 * np.pi * np.arange(n) / n
        c, s, z = np.cos(a), np.sin(a), np.zeros(n)
        sc.CtAcquisitionProjections(
            time=t, image_index=np.arange(n, dtype=np.uint64),
            source_position=np.stack([FOD * c, FOD * s, z], 1),
            detector_position=np.stack([-(FDD - FOD) * c, -(FDD - FOD) * s, z], 1),
            detector_u=np.stack([-s, c, z], 1),
            detector_v=np.tile([0.0, 0.0, -1.0], (n, 1)),
            pixel_size=np.full((n, 2), PIXEL),
            voltage_kv=np.full(n, 120.0), current_ua=np.full(n, 50.0),
            flags=np.zeros(n, np.uint32)).append_to(acq.series("projections"))


def show(path: str, n: int) -> None:
    scan = rbscan.ScanReader(path)
    acq = sc.CtAcquisition.read(scan["acquisitions/ct_0"])
    geometry = sc.CtAcquisitionProjections.read(scan["acquisitions/ct_0"])
    tube = sc.XraySourceState.read(scan[acq.source])
    print(f"acquisitions/ct_0: {acq.type} from {acq.source} to {acq.detector}, "
          f"{len(geometry)} projections")
    print(f"tube: {tube.voltage_kv[0]:.0f} kV, {tube.current_ua[0]:.0f} uA")
    # Source path as a whole column: one numpy array [n, 3].
    radius = np.linalg.norm(geometry.source_position, axis=1)
    print(f"source radius {radius.min():.3f} .. {radius.max():.3f} m")
    p = rbscan.read_projection(scan, "acquisitions/ct_0", n // 4)
    x, y, zz = p.geometry["source_position"]
    print(f"projection {n // 4}: source at ({x:.3f}, {y:.3f}, {zz:.3f}) m, "
          f"image {p.image.shape[1]} x {p.image.shape[0]}, "
          f"{int((p.image < 30000).sum())} dark pixels")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("path")
    parser.add_argument("--projections", type=int, default=90)
    parser.add_argument("--size", type=int, default=256)
    args = parser.parse_args()
    record(args.path, args.projections, args.size)
    show(args.path, args.projections)


if __name__ == "__main__":
    main()
