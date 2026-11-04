"""Helpers over related tables (rbscan.helpers)."""

import numpy as np
import pytest

import rbscan
from rbscan import schema_api as sc


def test_simulation_scene_object_by_object(tmp_path):
    part = rbscan.SceneObject(
        name="Düse", source="düse.stl", material="Ti", scale=1.5, position=(1.0, 2.0, 40.0),
        vertices=np.array([[-0.0, 1e-40, 0.1], [1, 0, 0], [0, 1, 0]], np.float32),
        triangles=np.array([[0, 1, 2]], np.uint32),
        registration=[rbscan.RegistrationPoint(marker="M7", marker_id=7, placed=True,
                                               mesh_point=(1 / 3, 0.0, 0.0))])
    holder = rbscan.SceneObject(
        name="Halter", material="C2H4", is_compound=True,
        vertices=np.array([[0, 0, 0], [2, 0, 0], [0, 2, 0], [0, 0, 2]], np.float32),
        triangles=np.array([[0, 2, 1], [0, 1, 3]], np.uint32))
    attrs = sc.SimulationScene(application="pytest", format="raybotic-simulation-scene/1",
                               document="{}", voltage_kv=160.0)
    with rbscan.ScanWriter.create(tmp_path) as scan:
        rbscan.write_simulation_scene(scan, "scene", attrs, [part, holder])
        bad = rbscan.SceneObject(name="bad", vertices=holder.vertices,
                                 triangles=np.array([[0, 1, 9]], np.uint32))
        with pytest.raises(ValueError):
            rbscan.write_simulation_scene(scan, "bad", attrs, [bad])

    a, objects = rbscan.read_simulation_scene(rbscan.ScanReader(tmp_path)["simulations/scene"])
    assert a.voltage_kv == 160.0 and a.object_names == ["Düse", "Halter"]
    assert [o.name for o in objects] == ["Düse", "Halter"]
    assert objects[0].vertices.tobytes() == part.vertices.tobytes()
    assert objects[1].triangles.tolist() == holder.triangles.tolist()  # own vertex numbers
    assert objects[0].position == (1.0, 2.0, 40.0) and objects[0].scale == 1.5
    assert objects[1].is_compound is True and objects[1].registration == []
    assert objects[0].registration == part.registration
    assert rbscan.validate(tmp_path)["ok"]


def test_trajectory_plan_trajectory_by_trajectory(tmp_path):
    def orbit(kind, tilt):
        return dict(kind=kind, centre=[0, 0, 0.1], axis=[0, 0, 1], start_direction=[1, 0, 0],
                    tilt=tilt, fod=0.3, fdd=0.6, projections=4, start_angle=0.0,
                    range=360.0, counter_clockwise=True)

    def poses(n):
        q = np.tile([0, 0, 0, 1.0], (n, 1))
        p = np.zeros((n, 3))
        return dict(angle=np.arange(n) * 90.0, skip=np.arange(n) == 1,
                    manual=np.zeros(n, bool), roi_position=p, roi_rotation=q,
                    source_position=p, source_rotation=q, detector_position=p,
                    detector_rotation=q)

    plan = [rbscan.Trajectory(orbit(0, 0.0), poses(3)), rbscan.Trajectory(orbit(1, 30.0), poses(2))]
    with rbscan.ScanWriter.create(tmp_path) as scan:
        rbscan.write_trajectory_plan(scan, "plan", sc.TrajectoryPlan(application="pytest"), plan)
    back = rbscan.read_trajectory_plan(rbscan.ScanReader(tmp_path)["trajectories/plan"])
    assert len(back) == 2
    assert back[1].orbit["tilt"] == 30.0 and back[1].orbit["kind"] == 1
    assert back[0].poses["angle"].tolist() == [0.0, 90.0, 180.0]
    assert back[1].poses["skip"].tolist() == [False, True]


def test_annotations_marker_by_marker(tmp_path):
    def row(marker_id):
        return dict(region_id=1, marker_id=marker_id, position=[0.1, 0.2, marker_id],
                    position_source=1, quality=3)

    def obs(times, projections):
        n = len(times)
        return dict(time=np.array(times), projection=np.array(projections, np.uint64),
                    image_index=np.array(projections, np.uint64), pixel=np.zeros((n, 2)))

    markers = [rbscan.Marker(row(4), obs([30, 20], [3, 2])), rbscan.Marker(row(9), obs([10], [1]))]
    attrs = sc.Annotations(application="pytest", format="test/1", document="{}")
    with rbscan.ScanWriter.create(tmp_path) as scan:
        rbscan.write_annotations(scan, "viewer", attrs, markers)
    node = rbscan.ScanReader(tmp_path)["annotations/viewer"]
    assert node["observations"].time().tolist() == [10, 20, 30]  # time order
    back = rbscan.read_annotations(node)
    assert [m.row["marker_id"] for m in back] == [4, 9]
    assert back[0].observations["projection"].tolist() == [2, 3]
    assert back[1].observations["time"].tolist() == [10]
    assert sc.AnnotationsMarkers.read(node).observations.tolist() == [2, 1]


def test_projection_with_its_image(tmp_path):
    with rbscan.ScanWriter.create(tmp_path) as scan:
        det = sc.XrayDetector(frame_id="d", simulated=True, sensor_width=4, sensor_height=3,
                              pixel_pitch_u_um=100.0, pixel_pitch_v_um=100.0).add(scan, "detector_0")
        images = np.arange(24, dtype=np.uint16).reshape(2, 3, 4)
        with det.segmented_series("image", dims={"height": 3, "width": 4}) as w:
            sc.XrayDetectorImage(time=np.array([0, 1]), image=images).append_to(w)
        acq = sc.CtAcquisition(type="ct_scan", source="sensors/source_0",
                               detector="sensors/detector_0", world_frame="world").add(scan, "ct_0")
        z = np.zeros((1, 3))
        sc.CtAcquisitionProjections(
            time=np.array([1]), image_index=np.array([1], np.uint64), source_position=z,
            detector_position=z, detector_u=z, detector_v=z, pixel_size=np.zeros((1, 2)),
            voltage_kv=np.array([120.0]), current_ua=np.array([50.0]),
            flags=np.array([0], np.uint32)).append_to(acq.series("projections"))
    p = rbscan.read_projection(rbscan.ScanReader(tmp_path), "acquisitions/ct_0", 0)
    assert p.geometry["voltage_kv"] == 120.0
    assert p.image.tolist() == images[1].tolist()
    with pytest.raises(IndexError):
        rbscan.read_projection(rbscan.ScanReader(tmp_path), "acquisitions/ct_0", 1)
