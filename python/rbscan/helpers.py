"""Helpers for schemas whose tables belong together, on top of rbscan.schema_api.

A simulation scene object by object (mesh, placement, registration points), a
trajectory plan trajectory by trajectory, annotations marker by marker and a CT
projection with its image. They fill in what links the tables (row ranges, ids,
name lists) and write each table in one append_many call. Volumes:
rbscan.write_volume / rbscan.read_volume.
"""

from __future__ import annotations

import dataclasses
import time as _time
from dataclasses import dataclass, field
from typing import Any

import numpy as np

from . import schema_api as sc

__all__ = [
    "Marker",
    "Projection",
    "RegistrationPoint",
    "SceneObject",
    "Trajectory",
    "read_annotations",
    "read_projection",
    "read_simulation_scene",
    "read_trajectory_plan",
    "write_annotations",
    "write_simulation_scene",
    "write_trajectory_plan",
]


def _now() -> int:
    return _time.time_ns()


def _names(obj) -> list[str]:
    return [f.name for f in dataclasses.fields(obj)]


def _times(t: int, n: int) -> np.ndarray:
    return np.full(n, t, dtype=np.int64)


# ---------------------------------------------------------------------------
# Simulation scene


@dataclass
class RegistrationPoint:
    """A spot of an object's mesh (file units) and the marker showing it."""

    marker: str = ""
    marker_id: int = -1
    placed: bool = False
    mesh_point: tuple[float, float, float] = (0.0, 0.0, 0.0)
    section_axis: int = -1
    section_offset: float = 0.0


@dataclass
class SceneObject:
    """One object of a simulation scene: its mesh in file units and placement.

    vertices: [n, 3] float32; triangles: [m, 3] uint32, counted from the
    object's own first vertex. See raybotic.simulation_scene/1 for the fields.
    """

    name: str
    vertices: np.ndarray
    triangles: np.ndarray
    source: str = ""
    material: str = "Al"
    from_volume: bool = False
    unit: int = 0
    scale: float = 1.0
    rotation: tuple[float, float, float] = (0.0, 0.0, 0.0)
    position: tuple[float, float, float] = (0.0, 0.0, 0.0)
    is_compound: bool = False
    density: float = 0.0
    enabled: bool = True
    colour: tuple[float, float, float] = (0.8, 0.8, 0.8)
    registration: list[RegistrationPoint] = field(default_factory=list)


_PLACEMENT = ("from_volume", "unit", "scale", "rotation", "position", "is_compound",
              "density", "enabled", "colour")


def write_simulation_scene(scan, name: str, attributes: sc.SimulationScene,
                           objects: list[SceneObject], time: int | None = None):
    """Stores a scene as simulations/<name>; returns its NodeWriter.

    object_names, object_sources, object_materials and registration_markers are
    set from `objects`; the other attributes (document, settings) come from
    `attributes`.
    """
    t = _now() if time is None else time
    verts = [np.ascontiguousarray(o.vertices, dtype=np.float32).reshape(-1, 3)
             for o in objects]
    tris = [np.ascontiguousarray(o.triangles, dtype=np.uint32).reshape(-1, 3)
            for o in objects]
    for o, v, f in zip(objects, verts, tris):
        if f.size and f.max() >= len(v):
            raise ValueError(f"{o.name}: a triangle names a vertex the object lacks")
    points = [p for o in objects for p in o.registration]
    a = dataclasses.replace(attributes)
    if objects:
        a.object_names = [o.name for o in objects]
        a.object_sources = [o.source for o in objects]
        a.object_materials = [o.material for o in objects]
    if points:
        a.registration_markers = [p.marker for p in points]
    node = a.add(scan, name)

    n = len(objects)
    if n:
        counts = {
            "vertex": np.array([len(v) for v in verts], np.uint64),
            "triangle": np.array([len(f) for f in tris], np.uint64),
            "registration": np.array([len(o.registration) for o in objects], np.uint64),
        }
        columns = {k: np.array([getattr(o, k) for o in objects]) for k in _PLACEMENT}
        for k, c in counts.items():
            columns[f"first_{k}"] = np.concatenate([[0], np.cumsum(c)[:-1]]).astype(np.uint64)
            columns[f"{k}_count"] = c
        sc.SimulationSceneObjects(time=_times(t, n), **columns).append_to(
            node.series("objects"))
    if verts and sum(len(v) for v in verts):
        v = np.concatenate(verts)
        sc.SimulationSceneVertices(time=_times(t, len(v)), position=v).append_to(
            node.series("vertices"))
    if tris and sum(len(f) for f in tris):
        f = np.concatenate(tris)
        sc.SimulationSceneTriangles(time=_times(t, len(f)), vertices=f).append_to(
            node.series("triangles"))
    if points:
        sc.SimulationSceneRegistration(
            time=_times(t, len(points)),
            marker_id=np.array([p.marker_id for p in points], np.int32),
            placed=np.array([p.placed for p in points], bool),
            mesh_point=np.array([p.mesh_point for p in points], np.float64),
            section_axis=np.array([p.section_axis for p in points], np.int8),
            section_offset=np.array([p.section_offset for p in points], np.float64),
        ).append_to(node.series("registration"))
    return node


def read_simulation_scene(node) -> tuple[sc.SimulationScene, list[SceneObject]]:
    """The scene's attributes and its objects with their meshes."""
    a = sc.SimulationScene.read(node)
    o = sc.SimulationSceneObjects.read(node)
    v = sc.SimulationSceneVertices.read(node).position
    f = sc.SimulationSceneTriangles.read(node).vertices
    r = sc.SimulationSceneRegistration.read(node)
    names = a.object_names or []
    sources = a.object_sources or []
    materials = a.object_materials or []
    markers = a.registration_markers or []
    objects = []
    for i in range(len(o)):
        v0, nv = int(o.first_vertex[i]), int(o.vertex_count[i])
        t0, nt = int(o.first_triangle[i]), int(o.triangle_count[i])
        r0, nr = int(o.first_registration[i]), int(o.registration_count[i])
        if v0 + nv > len(v) or t0 + nt > len(f) or r0 + nr > len(r):
            raise ValueError(f"{node.path}: object {i} has rows outside its tables")
        objects.append(SceneObject(
            name=names[i] if i < len(names) else "",
            source=sources[i] if i < len(sources) else "",
            material=materials[i] if i < len(materials) else "",
            vertices=v[v0:v0 + nv],
            triangles=f[t0:t0 + nt],
            **{k: (tuple(getattr(o, k)[i].tolist()) if getattr(o, k).ndim > 1
                   else getattr(o, k)[i].item()) for k in _PLACEMENT},
            registration=[RegistrationPoint(
                marker=markers[k] if k < len(markers) else "",
                marker_id=int(r.marker_id[k]), placed=bool(r.placed[k]),
                mesh_point=tuple(r.mesh_point[k].tolist()),
                section_axis=int(r.section_axis[k]),
                section_offset=float(r.section_offset[k])) for k in range(r0, r0 + nr)],
        ))
    return a, objects


# ---------------------------------------------------------------------------
# Trajectory plan


@dataclass
class Trajectory:
    """One planned trajectory: its orbit (a row of the trajectories table, without
    first_pose / pose_count) and its poses as columns of the poses table (without
    trajectory)."""

    orbit: dict[str, Any]
    poses: dict[str, np.ndarray]


def write_trajectory_plan(scan, name: str, attributes: sc.TrajectoryPlan,
                          trajectories: list[Trajectory], time: int | None = None):
    """Stores a plan as trajectories/<name>, trajectories in drive order."""
    t = _now() if time is None else time
    node = attributes.add(scan, name)
    if not trajectories:
        return node
    counts = [len(next(iter(tr.poses.values()))) if tr.poses else 0 for tr in trajectories]
    first = np.concatenate([[0], np.cumsum(counts)[:-1]]).astype(np.uint64)
    orbit = {k: np.array([tr.orbit[k] for tr in trajectories])
             for k in trajectories[0].orbit}
    sc.TrajectoryPlanTrajectories(time=_times(t, len(trajectories)), first_pose=first,
                                  pose_count=np.array(counts, np.uint64),
                                  **orbit).append_to(node.series("trajectories"))
    total = int(sum(counts))
    if total:
        keys = trajectories[0].poses.keys()
        poses = {k: np.concatenate([np.asarray(tr.poses[k]) for tr in trajectories
                                    if tr.poses]) for k in keys}
        poses["trajectory"] = np.repeat(np.arange(len(trajectories), dtype=np.uint32),
                                        counts)
        sc.TrajectoryPlanPoses(time=_times(t, total), **poses).append_to(
            node.series("poses"))
    return node


def read_trajectory_plan(node) -> list[Trajectory]:
    """The plan's trajectories, each with its poses."""
    tr = sc.TrajectoryPlanTrajectories.read(node)
    poses = sc.TrajectoryPlanPoses.read(node)
    pose_fields = [k for k in _names(poses) if k not in ("time", "trajectory")]
    orbit_fields = [k for k in _names(tr)
                    if k not in ("time", "first_pose", "pose_count")]
    out = []
    for i in range(len(tr)):
        p0, n = int(tr.first_pose[i]), int(tr.pose_count[i])
        out.append(Trajectory(
            orbit={k: (getattr(tr, k)[i].tolist()) for k in orbit_fields},
            poses={k: getattr(poses, k)[p0:p0 + n] for k in pose_fields}))
    return out


# ---------------------------------------------------------------------------
# Annotations


@dataclass
class Marker:
    """A marker (a row of the markers table, without observations) and where it
    was set: columns of the observations table (with "time", without
    marker_id)."""

    row: dict[str, Any]
    observations: dict[str, np.ndarray] = field(default_factory=dict)


def write_annotations(scan, name: str, attributes: sc.Annotations, markers: list[Marker],
                      time: int | None = None):
    """Stores annotations as annotations/<name>. Observations go in time order of
    their projections, as series need it."""
    t = _now() if time is None else time
    node = attributes.add(scan, name)
    if not markers:
        return node
    counts = [len(m.observations.get("time", ())) for m in markers]
    row = {k: np.array([m.row[k] for m in markers]) for k in markers[0].row}
    sc.AnnotationsMarkers(time=_times(t, len(markers)),
                          observations=np.array(counts, np.uint32), **row).append_to(
        node.series("markers"))
    if sum(counts):
        keys = [k for k in markers[0].observations if k != "time"]
        times = np.concatenate([np.asarray(m.observations["time"], np.int64)
                                for m in markers if m.observations])
        order = np.argsort(times, kind="stable")
        cols = {k: np.concatenate([np.asarray(m.observations[k]) for m in markers
                                   if m.observations])[order] for k in keys}
        cols["marker_id"] = np.repeat([m.row["marker_id"] for m in markers],
                                      counts).astype(np.uint32)[order]
        sc.AnnotationsObservations(time=times[order], **cols).append_to(
            node.series("observations"))
    return node


def read_annotations(node) -> list[Marker]:
    """The markers, each with its observations (in the stored, time order)."""
    m = sc.AnnotationsMarkers.read(node)
    o = sc.AnnotationsObservations.read(node)
    row_fields = [k for k in _names(m) if k not in ("time", "observations")]
    obs_fields = [k for k in _names(o) if k != "marker_id"]
    out = []
    for i in range(len(m)):
        sel = o.marker_id == m.marker_id[i]
        out.append(Marker(row={k: getattr(m, k)[i].tolist() for k in row_fields},
                          observations={k: getattr(o, k)[sel] for k in obs_fields}))
    return out


# ---------------------------------------------------------------------------
# CT acquisition


@dataclass
class Projection:
    """One projection: its geometry (a row of the projections table) and image."""

    geometry: dict[str, Any]
    image: np.ndarray


def read_projection(scan, acquisition: str, index: int) -> Projection:
    """Row `index` of an acquisition's projections with the detector image it
    refers to (image_index into the detector node the acquisition names)."""
    acq = scan[acquisition]
    rows = sc.CtAcquisitionProjections.read(acq, index, 1)
    if len(rows) != 1:
        raise IndexError(f"{acquisition}: no projection {index}")
    geometry = {k: getattr(rows, k)[0].tolist() for k in _names(rows)}
    detector = sc.CtAcquisition.read(acq).detector
    image = scan[detector]["image"].read_sample("image", int(geometry["image_index"]))
    return Projection(geometry=geometry, image=image)
