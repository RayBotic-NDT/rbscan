"""Stores a simulation scene in a scan and reads it back, meshes bit for bit.

Each object brings its mesh in the file units of its source, its placement,
material and registration points; the scene adds the tube and detector
settings. Without STL files it builds a cube and a tetrahedron. With
--export, the meshes read back are written as binary STL files again.

    python simulation_scene.py D:/scans/scene [part.stl ...] [--export out_dir]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

import rbscan
from rbscan import schema_api as sc

STL_TRIANGLE = np.dtype([("normal", "<f4", 3), ("corners", "<f4", (3, 3)), ("attr", "<u2")])


def load_stl(path: Path) -> tuple[np.ndarray, np.ndarray]:
    """A binary STL as vertices [3m, 3] float32 and triangles [m, 3]; vertices are
    not merged, so they stay exactly as the file has them."""
    data = path.read_bytes()
    count = int(np.frombuffer(data, "<u4", 1, 80)[0])
    tri = np.frombuffer(data, STL_TRIANGLE, count, 84)
    vertices = tri["corners"].reshape(-1, 3).astype(np.float32)
    return vertices, np.arange(3 * count, dtype=np.uint32).reshape(-1, 3)


def save_stl(path: Path, vertices: np.ndarray, triangles: np.ndarray) -> None:
    tri = np.zeros(len(triangles), STL_TRIANGLE)
    tri["corners"] = vertices[triangles]
    a, b, c = (tri["corners"][:, k] for k in range(3))
    n = np.cross(b - a, c - a)
    tri["normal"] = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-30)
    path.write_bytes(b"rbscan example".ljust(80, b"\0")
                     + np.uint32(len(tri)).tobytes() + tri.tobytes())


def builtin_objects() -> list[rbscan.SceneObject]:
    corners = np.array([[i & 1, (i >> 1) & 1, (i >> 2) & 1] for i in range(8)], np.float32)
    cube = rbscan.SceneObject(
        name="housing", source="built-in cube", material="Al", vertices=20 * corners,
        triangles=np.array([[0, 2, 1], [1, 2, 3], [4, 5, 6], [5, 7, 6], [0, 1, 4], [1, 5, 4],
                            [2, 6, 3], [3, 6, 7], [0, 4, 2], [2, 4, 6], [1, 3, 5], [3, 7, 5]],
                           np.uint32))
    insert = rbscan.SceneObject(
        name="insert", source="built-in tetrahedron", material="C2H4", is_compound=True,
        density=0.94,  # g/cm3
        vertices=8 * np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], np.float32),
        triangles=np.array([[0, 2, 1], [0, 1, 3], [0, 3, 2], [1, 2, 3]], np.uint32))
    return [cube, insert]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("path")
    parser.add_argument("stl", nargs="*", type=Path)
    parser.add_argument("--export", type=Path, help="write the meshes read back as STL")
    args = parser.parse_args()

    objects = [rbscan.SceneObject(name=f.stem, source=str(f), material="Al",
                                  vertices=v, triangles=t)
               for f in args.stl for v, t in [load_stl(f)]] or builtin_objects()
    for i, o in enumerate(objects):
        o.unit = 0  # mesh in mm
        o.rotation = (0.0, 0.0, 15.0 * i)  # deg, about the centre
        o.position = (0.0, 0.0, 30.0 * i)  # mm, bounding-box centre in the world
    objects[0].registration.append(rbscan.RegistrationPoint(
        marker="M1", marker_id=1, placed=True,
        mesh_point=tuple(objects[0].vertices[0].tolist()), section_axis=-1))

    # The application's own document is the source of truth; the setting
    # attributes repeat it for tools that do not read the document.
    settings = dict(voltage_kv=160.0, current_ua=100.0, focal_spot_um=20.0,
                    sensor_width=1024, sensor_height=1024, pixel_pitch_u_um=139.0,
                    pixel_pitch_v_um=139.0, scintillator="CsI", scintillator_um=600.0)
    attrs = sc.SimulationScene(application="rbscan example", format="rbscan-example-scene/1",
                               document=json.dumps({"settings": settings}), **settings)
    with rbscan.ScanWriter.create(args.path, description="simulation scene",
                                  overwrite=True) as scan:
        rbscan.write_simulation_scene(scan, "scene", attrs, objects)

    back_attrs, back = rbscan.read_simulation_scene(
        rbscan.ScanReader(args.path)["simulations/scene"])
    print(f"scene from {back_attrs.application}: {back_attrs.voltage_kv:.0f} kV, "
          f"{back_attrs.current_ua:.0f} uA, detector {back_attrs.sensor_width} x "
          f"{back_attrs.sensor_height}")
    for before, o in zip(objects, back):
        same = (o.vertices.tobytes() == np.asarray(before.vertices, np.float32).tobytes()
                and np.array_equal(o.triangles, before.triangles))
        print(f"  {o.name:<10} {o.material:<5} {len(o.vertices):6} vertices "
              f"{len(o.triangles):6} triangles at z = {o.position[2]:4.0f} mm, "
              f"{len(o.registration)} registration points, "
              f"mesh {'bit-identical' if same else 'DIFFERS'}")
        if args.export:
            args.export.mkdir(parents=True, exist_ok=True)
            save_stl(args.export / f"{o.name}.stl", o.vertices, o.triangles)
        if not same:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
