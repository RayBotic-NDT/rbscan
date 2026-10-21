"""Records a synthetic CT scan the way the cell would, then reads it back.

Writes the X-ray source and detector state, two robots with joint states, the
scan frames, the projections (segmented, compressed in parallel) and the
resolved geometry per projection. The detector images are recorded by a
separate process, like a detector node would do, and are linked into the
master when the scan is finalized.

    python record_ct_scan.py D:/scans/phantom [--projections 360] [--size 768]
"""

from __future__ import annotations

import argparse
import math
import multiprocessing as mp
import time
from pathlib import Path

import numpy as np

import rbscan

DETECTOR = "sensors/detector_0"
JOINTS = ["J1", "J2", "J3", "J4", "J5", "J6"]
FOD, FDD, PIXEL = 0.4, 1.0, 139e-6  # m
PERIOD_NS = 200_000_000  # one projection every 200 ms


def frame_rotation(theta: float) -> list[float]:
    """Quaternion (x, y, z, w) of a scan frame that looks along -x rotated by theta about z.

    Scan frames have y up and z along the beam axis (ScanPose convention): at
    theta = 0, x -> +y, y -> +z, z -> +x (a 120 deg turn about (1, 1, 1)), then
    the turn about the world z axis.
    """
    c, s = math.cos(theta / 2), math.sin(theta / 2)
    return [0.5 * (c - s), 0.5 * (c + s), 0.5 * (c + s), 0.5 * (c - s)]


def phantom(size: int, angle: float) -> np.ndarray:
    """A blurred disc that moves with the angle, plus a little texture."""
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    cx = 0.5 * size + 0.2 * size * math.cos(angle)
    absorb = np.exp(-((x - cx) ** 2 + (y - 0.5 * size) ** 2) / (0.09 * size * size))
    texture = (x.astype(np.int32) * 31 + y.astype(np.int32) * 17) & 63
    return (52000.0 * (1.0 - 0.7 * absorb) + texture).astype(np.uint16)


def record_detector(scan_dir: str, t0: int, n: int, size: int) -> None:
    """The detector process: knows only the scan directory, its node and schema."""
    images = rbscan.SegmentedSeriesWriter.open(
        scan_dir, DETECTOR, "raybotic.xray_detector", "image",
        dims={"height": size, "width": size},
        attributes=dict(frame_id="scan_detector", simulated=True, sensor_width=size,
                        sensor_height=size, pixel_pitch_u_um=PIXEL * 1e6,
                        pixel_pitch_v_um=PIXEL * 1e6),
        segment_size=100)
    frames = [phantom(size, k * math.pi / 8) for k in range(16)]
    for i in range(n):
        images.append(t0 + i * PERIOD_NS, image=frames[i % len(frames)])
    images.close()


def record(scan_dir: Path, n: int, size: int) -> None:
    t0 = rbscan.now_ns()

    # The detector runs in its own process and writes its own segment files.
    detector_proc = mp.get_context("spawn").Process(
        target=record_detector, args=(str(scan_dir), t0, n, size))

    scan = rbscan.ScanWriter.create(scan_dir, description="synthetic CT scan",
                                    operator="rbscan example", sample_id="phantom_01",
                                    overwrite=True)
    detector_proc.start()

    source = scan.add_node(
        "source_0", "raybotic.xray_source", frame_id="scan_source", simulated=True,
        voltage_min_kv=20.0, voltage_max_kv=225.0, current_max_ua=1000.0, power_max_w=320.0,
        focal_spots_um=[5.0, 20.0, 80.0], filter_material="Cu")
    source.series("state").append(
        t0, emission_state=2, emission_permitted=True, voltage_setpoint_kv=120.0,
        current_setpoint_ua=50.0, focal_spot_um=5.0, voltage_kv=120.0, current_ua=50.0,
        power_w=6.0, filter_thickness_mm=0.5)

    robots = {}
    for name, target in (("source_robot", "scan_source"), ("detector_robot", "scan_detector")):
        robots[name] = scan.add_node(
            name, "raybotic.robot_arm", joint_names=JOINTS, base_frame=f"{name}_base",
            tcp_frame=f"{name}_tcp", target_frame=target, simulated=True,
            model="FANUC CRX-30iA").series("joint_states")

    frames = {
        child: scan.add_node(child, "raybotic.transform", parent="world", child=child,
                             static=False).series("pose")
        for child in ("scan_source", "scan_detector")
    }

    acq = scan.add_node("scan_0", "raybotic.ct_acquisition", type="ct_scan",
                        source="sensors/source_0", detector=DETECTOR, world_frame="world",
                        trajectory="circular, 360 deg")
    projections = acq.series("projections")

    for i in range(n):
        a = 2 * math.pi * i / n
        t = t0 + i * PERIOD_NS
        c, s = math.cos(a), math.sin(a)
        src = [FOD * c, FOD * s, 0.0]
        det = [-(FDD - FOD) * c, -(FDD - FOD) * s, 0.0]
        # Detector: z towards the source; source: z towards the detector.
        det_rotation = frame_rotation(a)
        src_rotation = frame_rotation(a + math.pi)

        # Robots and frames at four times the projection rate.
        for k in range(4):
            tk = t + k * PERIOD_NS // 4
            q = 0.3 * np.sin(a + 0.1 * np.arange(6) + 0.01 * k)
            for joints in robots.values():
                joints.append(tk, position=q)
            frames["scan_source"].append(tk, translation=src, rotation=src_rotation)
            frames["scan_detector"].append(tk, translation=det, rotation=det_rotation)

        projections.append(
            t, image_index=i, source_position=src, detector_position=det,
            detector_u=[-s, c, 0.0], detector_v=[0.0, 0.0, -1.0], pixel_size=[PIXEL, PIXEL],
            voltage_kv=120.0, current_ua=50.0, integration_time_ms=100.0, flags=0)

    detector_proc.join()
    if detector_proc.exitcode != 0:
        raise RuntimeError("detector process failed")
    scan.finalize()  # links the detector's segments into the master


def read_back(scan_dir: Path) -> None:
    scan = rbscan.ScanReader(scan_dir)
    print(f"\n{scan.directory}: complete={scan.complete}, uuid={scan.attributes['uuid']}")
    for path in scan.node_paths():
        node = scan[path]
        series = ", ".join(f"{s}[{len(node[s])}]" for s in node.series_names)
        print(f"  {path:28} {node.schema.id:28} {series}")

    # Projection i with its geometry and the robot pose at exposure time.
    proj = scan["acquisitions/scan_0"]["projections"]
    images = scan[DETECTOR]["image"]
    joints = scan["robots/source_robot"]["joint_states"]
    i = len(proj) // 4
    row = proj[i]
    image = images.read_sample("image", int(row["image_index"]))
    k = joints.index_at(int(row["time"]))
    print(f"\nprojection {i}: image {image.shape} {image.dtype}, "
          f"mean {image.mean():.0f}, source at {np.round(row['source_position'], 3)} m, "
          f"source robot J1 = {joints.read_sample('position', k)[0]:.3f} rad")

    # Any HDF5 tool sees the segments as one dataset.
    try:
        import h5py
    except ImportError:
        return
    with h5py.File(scan_dir / rbscan.MASTER_FILE_NAME, "r") as f:
        ds = f[f"{DETECTOR}/image/image"]
        print(f"h5py: {ds.name} {ds.shape} {ds.dtype}, virtual={ds.is_virtual}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scan_dir", type=Path)
    parser.add_argument("--projections", type=int, default=360)
    parser.add_argument("--size", type=int, default=768)
    args = parser.parse_args()

    start = time.perf_counter()
    record(args.scan_dir, args.projections, args.size)
    seconds = time.perf_counter() - start
    mb = args.projections * args.size * args.size * 2 / 1e6
    print(f"recorded {args.projections} projections {args.size}x{args.size} "
          f"({mb:.0f} MB raw) in {seconds:.1f} s")
    read_back(args.scan_dir)


if __name__ == "__main__":
    main()
