import json
import math
import multiprocessing as mp
from pathlib import Path

import numpy as np
import pytest

import rbscan

H, W = 32, 40

DETECTOR = dict(
    frame_id="scan_detector",
    simulated=True,
    sensor_width=W,
    sensor_height=H,
    pixel_pitch_u_um=139.0,
    pixel_pitch_v_um=139.0,
)


def frame(i: int) -> np.ndarray:
    y, x = np.mgrid[0:H, 0:W]
    return ((i * 7 + y * 3 + x) % 65536).astype(np.uint16)


def write_ct_scan(path: Path, n: int = 13, compression: str = "bitshuffle_lz4") -> None:
    with rbscan.ScanWriter.create(path, description="pytest", operator="ci", run=7) as scan:
        scan.add_node(
            "source_0", "raybotic.xray_source",
            frame_id="scan_source", simulated=True, voltage_min_kv=20, voltage_max_kv=225.0,
            current_max_ua=1000.0, power_max_w=320.0, focal_spots_um=[5.0, 20.0],
        ).series("state").append(
            0, emission_state=2, emission_permitted=True, voltage_setpoint_kv=120,
            current_setpoint_ua=50, focal_spot_um=5, voltage_kv=120, current_ua=50, power_w=6,
        )
        det = scan.add_node("detector_0", "raybotic.xray_detector", DETECTOR)
        images = det.segmented_series("image", dims={"height": H, "width": W}, segment_size=4,
                                      threads=3, compression=compression)
        det.series("calibration", dims={"height": H, "width": W}).append(
            0, dark=np.zeros((H, W), np.float32), flat=np.ones((H, W)))
        acq = scan.add_node("scan_0", "raybotic.ct_acquisition", type="ct_scan",
                            source="sensors/source_0", detector="sensors/detector_0",
                            world_frame="world")
        proj = acq.series("projections")
        for i in range(n):
            t = 1_000_000 * i
            images.append(t, image=frame(i))
            a = 2 * math.pi * i / n
            proj.append(
                t, image_index=i, source_position=[0.5 * math.cos(a), 0.5 * math.sin(a), 0],
                detector_position=[-0.5 * math.cos(a), -0.5 * math.sin(a), 0],
                detector_u=[-math.sin(a), math.cos(a), 0], detector_v=[0, 0, -1],
                pixel_size=[278e-6, 278e-6], voltage_kv=120, current_ua=50,
                integration_time_ms=100, flags=0,
            )


def test_schemas():
    ids = rbscan.schemas()
    assert "raybotic.xray_detector/1" in ids
    det = rbscan.schema("raybotic.xray_detector")
    assert det.category == "sensors"
    assert det.series_names == ["image", "state", "calibration"]
    d = det.to_dict()
    assert d["series"]["image"]["fields"]["image"]["shape"] == ["height", "width"]
    assert "frame_id" in d["attributes"]  # inherited from raybotic.sensor


def test_roundtrip(tmp_path):
    write_ct_scan(tmp_path)
    scan = rbscan.ScanReader(tmp_path)
    assert scan.complete
    assert scan.attributes["run"] == 7
    assert scan.node_paths("sensors") == ["sensors/source_0", "sensors/detector_0"]
    assert "acquisitions/scan_0" in scan

    det = scan["sensors/detector_0"]
    assert det.attributes["sensor_width"] == W
    assert det.attributes["simulated"] is True
    images = det["image"]
    assert images.storage == "segmented"
    assert len(images) == 13
    stack = images.read("image")
    assert stack.dtype == np.uint16 and stack.shape == (13, H, W)
    for i in range(13):
        np.testing.assert_array_equal(stack[i], frame(i))
    np.testing.assert_array_equal(images.read("image", 3, 2)[1], frame(4))
    assert images[-1]["time"] == 12_000_000
    assert images.index_at(4_500_000) == 4

    cal = det["calibration"]
    assert cal.read("flat").dtype == np.float32
    assert not cal.read_sample("bad_pixels", 0).any()  # optional uint8, filled with 0

    proj = scan["acquisitions/scan_0"]["projections"]
    assert proj.fields[0] == "image_index"
    assert proj.read("detector_u").shape == (13, 3)
    assert np.isnan(proj.read("projection_matrix")).all()  # optional, left out
    assert proj.units("source_position") == "m"

    src = scan["sensors/source_0"]["state"]
    assert bool(src[0]["emission_permitted"]) is True


def _h5py_for(compression: str):
    """h5py, plus the reference bitshuffle filter for rbscan's default codec."""
    h5py = pytest.importorskip("h5py")
    if compression == "bitshuffle_lz4":
        pytest.importorskip("hdf5plugin")  # registers filter 32008 with h5py
    return h5py


# shuffle_deflate: any HDF5 reads it. bitshuffle_lz4 (the default): checks that
# rbscan's chunks decode with the reference bitshuffle implementation.
H5PY_CODECS = pytest.mark.parametrize("compression", ["shuffle_deflate", "bitshuffle_lz4"])


@H5PY_CODECS
def test_h5py_sees_one_virtual_dataset(tmp_path, compression):
    h5py = _h5py_for(compression)
    write_ct_scan(tmp_path, compression=compression)
    # rbscan's own HDF5 is rbscan_hdf5.dll, so h5py can be loaded alongside.
    with h5py.File(tmp_path / rbscan.MASTER_FILE_NAME, "r") as f:
        assert f.attrs["format"] == "raybotic.scan"
        node = f["sensors/detector_0"]
        assert node.attrs["schema"] == "raybotic.xray_detector/1"
        assert json.loads(node.attrs["schema_json"])["schema"] == "raybotic.xray_detector"
        ds = node["image/image"]
        assert ds.is_virtual
        assert ds.shape == (13, H, W)
        np.testing.assert_array_equal(ds[9], frame(9))
        assert ds.attrs["units"] == "counts"
        assert node["image"].attrs["segment_count"] == 4
        assert f["acquisitions/scan_0/projections/flags"].shape == (13,)


@H5PY_CODECS
def test_scan_can_be_moved(tmp_path, compression):
    h5py = _h5py_for(compression)
    write_ct_scan(tmp_path / "a", compression=compression)
    (tmp_path / "a").rename(tmp_path / "b")
    np.testing.assert_array_equal(
        rbscan.ScanReader(tmp_path / "b")["sensors/detector_0"]["image"].read_sample("image", 5),
        frame(5))
    with h5py.File(tmp_path / "b" / rbscan.MASTER_FILE_NAME, "r") as f:
        np.testing.assert_array_equal(f["sensors/detector_0/image/image"][5], frame(5))


def test_errors(tmp_path):
    with rbscan.ScanWriter.create(tmp_path) as scan:
        with pytest.raises(rbscan.Error, match="required attribute"):
            scan.add_node("detector_0", "raybotic.xray_detector", frame_id="x")
        det = scan.add_node("detector_0", "raybotic.xray_detector", DETECTOR)
        images = det.segmented_series("image", dims={"height": H, "width": W})
        with pytest.raises(rbscan.Error, match="needs"):
            images.append(0, image=np.zeros((H, W + 1), np.uint16))
        with pytest.raises(rbscan.Error, match="no field"):
            images.append(0, image=frame(0), bogus=1)
    with pytest.raises(rbscan.Error):
        rbscan.ScanWriter.create(tmp_path)


def _record_camera(scan_dir: str, n: int) -> None:
    import rbscan as rb

    cam = rb.SegmentedSeriesWriter.open(
        scan_dir, "sensors/cell_cam", "raybotic.camera", "image", segment_size=5,
        attributes=dict(frame_id="cell_cam", simulated=False, width=8, height=6, channels=3,
                        encoding="rgb8"))
    for i in range(n):
        cam.append(i, image=np.full((6, 8, 3), i, np.uint8))
    cam.close()


def test_writer_in_other_process(tmp_path):
    scan = rbscan.ScanWriter.create(tmp_path)
    ctx = mp.get_context("spawn")
    p = ctx.Process(target=_record_camera, args=(str(tmp_path), 12))
    p.start()
    p.join(60)
    assert p.exitcode == 0
    scan.finalize()
    cam = rbscan.ScanReader(tmp_path)["sensors/cell_cam"]
    assert cam.attributes["encoding"] == "rgb8"
    imgs = cam["image"].read("image")
    assert imgs.shape == (12, 6, 8, 3)
    assert imgs[11].min() == 11


def test_live_reading(tmp_path):
    scan = rbscan.ScanWriter.create(tmp_path)
    det = scan.add_node("detector_0", "raybotic.xray_detector", DETECTOR)
    images = det.segmented_series("image", dims={"height": H, "width": W}, segment_size=100,
                                  flush_every=0)
    for i in range(3):
        images.append(i, image=frame(i))
    images.flush()
    live = rbscan.SeriesReader.open_segments(tmp_path, "sensors/detector_0", "image")
    assert len(live) == 3
    for i in range(3, 7):
        images.append(i, image=frame(i))
    images.flush()
    live.refresh()
    assert len(live) == 7
    np.testing.assert_array_equal(live.read_sample("image", 6), frame(6))
    scan.finalize()


def test_custom_schema(tmp_path):
    rbscan.register_schema(json.dumps({
        "schema": "pytest.thermo_camera", "version": 1, "extends": "raybotic.sensor/1",
        "attributes": {"width": {"dtype": "uint32", "required": True},
                       "height": {"dtype": "uint32", "required": True}},
        "series": {"thermal": {"storage": "segmented", "fields": {
            "temperature": {"dtype": "float32", "shape": ["height", "width"], "units": "K"}}}},
    }))
    with rbscan.ScanWriter.create(tmp_path) as scan:
        cam = scan.add_node("thermo", "pytest.thermo_camera", frame_id="t", simulated=True,
                            width=4, height=3)
        s = cam.segmented_series("thermal")  # dims from attributes
        s.append(0, temperature=np.full((3, 4), 293.15))
    s = rbscan.ScanReader(tmp_path)["sensors/thermo"]["thermal"]
    assert s.units("temperature") == "K"
    assert s.read_sample("temperature", 0)[2, 3] == pytest.approx(293.15)


def test_float_projections_and_volume(tmp_path):
    with rbscan.ScanWriter.create(tmp_path) as scan:
        det = scan.add_node("detector_0", "raybotic.xray_detector", DETECTOR)
        images = det.segmented_series("image", dims={"height": H, "width": W},
                                      dtypes={"image": np.float32})
        images.append(0, image=np.linspace(-1, 1, H * W, dtype=np.float32).reshape(H, W))
        vol = np.random.default_rng(1).random((5, 6, 7), dtype=np.float32)
        a = np.diag([1e-4, 2e-4, 3e-4, 1.0])
        a[:3, 3] = [0.01, -0.02, 0.03]
        rbscan.write_volume(scan, "reco", vol, a, quantity="attenuation", value_units="1/m",
                            segment_size=2)
        with pytest.raises(rbscan.Error, match="cannot be int32"):
            det.series("calibration", dims={"height": H, "width": W},
                       dtypes={"dark": "int32"})
    reader = rbscan.ScanReader(tmp_path)
    img = reader["sensors/detector_0"]["image"].read_sample("image", 0)
    assert img.dtype == np.float32 and img.min() == -1.0
    back, affine = rbscan.read_volume(reader, "volumes/reco")
    np.testing.assert_array_equal(back, vol)
    np.testing.assert_allclose(affine, a)
    node = reader["volumes/reco"]
    assert node.attributes["voxel_size"] == pytest.approx([1e-4, 2e-4, 3e-4])
    assert node["slices"].storage == "segmented"
    assert rbscan.validate(tmp_path)["ok"]

    del node, reader  # the master must be closed before it is written again

    # Volumes added to an existing scan later.
    scan = rbscan.ScanWriter.open(tmp_path)
    rbscan.write_volume(scan, "labels", (vol > 0.5).astype(np.uint8), a)
    scan.finalize()
    labels, _ = rbscan.read_volume(tmp_path, "volumes/labels")
    assert labels.dtype == np.uint8 and labels.sum() == (vol > 0.5).sum()


@pytest.mark.parametrize("compression", ["none", "deflate", "shuffle_deflate", "bitshuffle_lz4"])
def test_compression_codecs(tmp_path, compression):
    with rbscan.ScanWriter.create(tmp_path) as scan:
        det = scan.add_node("detector_0", "raybotic.xray_detector", DETECTOR)
        images = det.segmented_series("image", dims={"height": H, "width": W},
                                      segment_size=3, compression=compression)
        cal = det.series("calibration", dims={"height": H, "width": W}, compression=compression)
        cal.append(0, dark=np.full((H, W), 1.5, np.float32), flat=np.ones((H, W), np.float32))
        for i in range(7):
            images.append(i, image=frame(i))
    reader = rbscan.ScanReader(tmp_path)["sensors/detector_0"]
    np.testing.assert_array_equal(reader["image"].read_sample("image", 5), frame(5))
    np.testing.assert_array_equal(reader["image"].read("image", 2, 3)[2], frame(4))
    assert reader["calibration"].read_sample("dark", 0)[0, 0] == pytest.approx(1.5)
    assert rbscan.validate(tmp_path)["ok"]



def test_simulation_scene_keeps_the_mesh_bit_for_bit(tmp_path):
    # -0, a denormal and values a text format or a unit conversion would change.
    vertices = np.array([[-0.0, 1e-40, 0.1], [3.4028235e38, 0.0, -2.5],
                         [0.0, 7.0, 1.0 / 3.0], [1.0, 1.0, 1.0]], np.float32)
    triangles = np.array([[0, 1, 2], [0, 3, 1], [1, 3, 2], [2, 3, 0]], np.uint32)
    document = json.dumps({"format": "raybotic-simulation-scene", "version": 1})
    with rbscan.ScanWriter.create(tmp_path) as scan:
        scene = scan.add_node(
            "scene", "raybotic.simulation_scene", application="pytest",
            format="raybotic-simulation-scene/1", document=document,
            object_names=["Düse"], object_sources=["düse.stl"], object_materials=["Ti"],
            voltage_kv=160.0, sensor_width=1024,
        )
        scene.series("objects").append(
            1, from_volume=False, unit=0, scale=1.0, rotation=[0, 90, 0],
            position=[0, 0, 40], is_compound=False, density=4.5, enabled=True,
            colour=[0.94, 0.78, 0.35], first_vertex=0, vertex_count=4, first_triangle=0,
            triangle_count=4, first_registration=0, registration_count=0,
        )
        v = scene.series("vertices")
        for p in vertices:
            v.append(1, position=p)
        t = scene.series("triangles")
        for c in triangles:
            t.append(1, vertices=c)

    node = rbscan.ScanReader(tmp_path)["simulations/scene"]
    assert node.attributes["object_names"] == ["Düse"]
    assert json.loads(node.attributes["document"])["version"] == 1
    assert "registration" not in node.series_names  # no rows written
    stored = node["vertices"].read("position")
    assert stored.dtype == np.float32
    assert stored.tobytes() == vertices.tobytes()
    assert np.signbit(stored[0, 0])
    assert node["triangles"].read("vertices").tobytes() == triangles.tobytes()
    assert node["objects"].units("position") == "mm"


def test_append_many(tmp_path):
    vertices = np.arange(30, dtype=np.float32).reshape(10, 3)
    with rbscan.ScanWriter.create(tmp_path) as scan:
        scene = scan.add_node("scene", "raybotic.simulation_scene", application="pytest",
                              format="x/1", document="{}")
        v = scene.series("vertices")
        v.append_many(5, position=vertices[:6])
        v.append_many(np.array([6, 7, 8, 9]), position=vertices[6:])
        with pytest.raises(rbscan.Error):
            v.append_many([1, 2], position=vertices[:3])  # 3 samples, 2 times
    s = rbscan.ScanReader(tmp_path)["simulations/scene"]["vertices"]
    assert s.read("position").tobytes() == vertices.tobytes()
    assert s.time().tolist() == [5] * 6 + [6, 7, 8, 9]
