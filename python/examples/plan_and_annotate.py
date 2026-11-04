"""Plans two trajectories and stores markers clicked in projections, then reads both back.

The plan holds a circle and a laminography orbit, trajectory by trajectory,
in drive order. The markers refer to projection rows of an acquisition; their
observations may come in any order, the helper writes them in time order and
groups them by marker again on read.

    python plan_and_annotate.py D:/scans/plan
"""

from __future__ import annotations

import sys

import numpy as np

import rbscan
from rbscan import schema_api as sc

PERIOD_NS = 200_000_000


def about_z(deg: np.ndarray) -> np.ndarray:
    """Quaternions (x, y, z, w) [n, 4] for turns about the world z axis."""
    h = np.radians(deg) / 2
    return np.stack([np.zeros_like(h), np.zeros_like(h), np.sin(h), np.cos(h)], 1)


def orbit(kind: int, tilt: float, count: int, fod: float = 0.4,
          fdd: float = 1.0) -> rbscan.Trajectory:
    centre = np.array([0.0, 0.0, 0.1])
    angle = 360.0 * np.arange(count) / count
    a = np.radians(angle)
    ring = np.stack([np.cos(a), np.sin(a), np.zeros(count)], 1)
    lift = np.tan(np.radians(tilt))
    up = np.array([0.0, 0.0, 1.0])
    return rbscan.Trajectory(
        orbit=dict(kind=kind, centre=centre, axis=up, start_direction=[1.0, 0.0, 0.0],
                   tilt=tilt, fod=fod, fdd=fdd, projections=count, start_angle=0.0,
                   range=360.0, counter_clockwise=True),
        poses=dict(angle=angle,
                   skip=np.arange(count) % 10 == 9,  # e.g. the robot cannot reach these
                   manual=np.zeros(count, bool),
                   roi_position=np.tile(centre, (count, 1)),
                   roi_rotation=about_z(np.zeros(count)),
                   source_position=centre + fod * (ring + lift * up),
                   source_rotation=about_z(angle + 180.0),
                   detector_position=centre - (fdd - fod) * (ring + lift * up),
                   detector_rotation=about_z(angle)))


def marker(marker_id: int, position, seen_in: list[int], t0: int) -> rbscan.Marker:
    p = np.array(seen_in, np.uint64)
    return rbscan.Marker(
        row=dict(region_id=1, marker_id=marker_id, position=position,
                 position_source=1, quality=3),  # triangulated, good
        observations=dict(time=t0 + p.astype(np.int64) * PERIOD_NS, projection=p,
                          image_index=p,
                          pixel=np.stack([100.0 + p, np.full(len(p), 200.0)], 1)))


def main(path: str) -> None:
    t0 = rbscan.now_ns()
    with rbscan.ScanWriter.create(path, description="plan and markers", overwrite=True) as scan:
        rbscan.write_trajectory_plan(
            scan, "plan", sc.TrajectoryPlan(application="rbscan example",
                                            acquisition="acquisitions/ct_0"),
            [orbit(0, 0.0, 36), orbit(1, 30.0, 20)])
        rbscan.write_annotations(
            scan, "viewer",
            sc.Annotations(application="rbscan example", format="rbscan-example-annotations/1",
                           document="{}", acquisition="acquisitions/ct_0"),
            [marker(1, [0.01, 0.0, 0.1], [30, 0, 12], t0),
             marker(2, [-0.01, 0.005, 0.12], [5, 25], t0)])

    scan = rbscan.ScanReader(path)
    for t in rbscan.read_trajectory_plan(scan["trajectories/plan"]):
        kind = "circle" if t.orbit["kind"] == 0 else "laminography"
        driven = int((~t.poses["skip"]).sum())
        x, y, z = t.poses["source_position"][0]
        print(f"{kind:<12} tilt {t.orbit['tilt']:2.0f} deg: {len(t.poses['angle'])} poses, "
              f"{driven} driven, first source at ({x:.3f}, {y:.3f}, {z:.3f}) m")
    for m in rbscan.read_annotations(scan["annotations/viewer"]):
        print(f"marker {m.row['marker_id']} seen in projections "
              f"{m.observations['projection'].tolist()}")
    # The tables are plain series as well: all observations, in time order.
    obs = sc.AnnotationsObservations.read(scan["annotations/viewer"])
    print("observations by time:", list(zip(obs.marker_id.tolist(), obs.projection.tolist())))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "plan_scan")
