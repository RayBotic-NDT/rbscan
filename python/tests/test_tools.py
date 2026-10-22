import json
import math

import numpy as np
import pytest

import rbscan

FOD, FDD = 0.4, 1.0
H, W = 24, 32


def frame_rotation(theta: float) -> list[float]:
    c, s = math.cos(theta / 2), math.sin(theta / 2)
    return [0.5 * (c - s), 0.5 * (c + s), 0.5 * (c + s), 0.5 * (c - s)]


def write_scan(path, n=8):
    """Circular scan with frames at every image time; no projection table."""
    with rbscan.ScanWriter.create(path) as scan:
        scan.add_node("source_0", "raybotic.xray_source", frame_id="scan_source", simulated=True,
                      voltage_min_kv=20.0, voltage_max_kv=225.0, current_max_ua=1000.0,
                      power_max_w=320.0)
        det = scan.add_node("detector_0", "raybotic.xray_detector", frame_id="scan_detector",
                            simulated=True, sensor_width=W, sensor_height=H,
                            pixel_pitch_u_um=150.0, pixel_pitch_v_um=150.0)
        frames = {c: scan.add_node(c, "raybotic.transform", parent="world", child=c,
                                   static=False).series("pose")
                  for c in ("scan_source", "scan_detector")}
        images = det.segmented_series("image", dims={"height": H, "width": W}, segment_size=3)
        for i in range(n):
            a = 2 * math.pi * i / n
            t = 1000 * i
            frames["scan_source"].append(t, translation=[FOD * math.cos(a), FOD * math.sin(a), 0],
                                         rotation=frame_rotation(a + math.pi))
            frames["scan_detector"].append(
                t, translation=[-(FDD - FOD) * math.cos(a), -(FDD - FOD) * math.sin(a), 0],
                rotation=frame_rotation(a))
            images.append(t, image=np.full((H, W), i, np.uint16))


def test_info(tmp_path):
    write_scan(tmp_path)
    info = rbscan.info(tmp_path)
    assert info["complete"]
    det = next(n for n in info["nodes"] if n["path"] == "sensors/detector_0")
    image = det["series"][0]
    assert image["samples"] == 8 and image["segments"] == 3
    assert image["rate_hz"] == pytest.approx(1e6)
    assert image["fields"][0]["shape"] == [H, W]
    assert "sensors/detector_0" in rbscan.info_text(tmp_path)


def test_resolve_geometry_and_validate(tmp_path):
    write_scan(tmp_path)
    r = rbscan.resolve_geometry(tmp_path)
    assert r == {"acquisition": "acquisitions/scan_0", "projections": 8, "interpolated": 0,
                 "extrapolated": 0, "robot_unreachable": 0, "emission_unstable": 0}

    p = rbscan.ScanReader(tmp_path)["acquisitions/scan_0"]["projections"]
    a = 2 * math.pi * np.arange(8) / 8
    np.testing.assert_allclose(p.read("source_position")[:, 0], FOD * np.cos(a), atol=1e-12)
    np.testing.assert_allclose(p.read("detector_u")[:, :2],
                               np.stack([-np.sin(a), np.cos(a)], 1), atol=1e-12)
    np.testing.assert_allclose(p.read("detector_v"), [[0, 0, -1]] * 8, atol=1e-12)
    np.testing.assert_allclose(p.read("pixel_size"), 150e-6)
    # The rotation axis projects onto the central column.
    P = p.read("projection_matrix")
    for k in range(8):
        x = P[k] @ np.array([0.0, 0.0, 0.01, 1.0])
        assert x[0] / x[2] == pytest.approx((W - 1) / 2)
        assert x[1] / x[2] < (H - 1) / 2  # above the centre: smaller row index
    del p

    report = rbscan.validate(tmp_path)
    assert report["ok"], report
    assert report["errors"] == 0

    with pytest.raises(rbscan.Error, match="exists already"):
        rbscan.resolve_geometry(tmp_path)
    assert rbscan.resolve_geometry(tmp_path, overwrite=True)["projections"] == 8


def test_frame_graph(tmp_path):
    write_scan(tmp_path)
    g = rbscan.FrameGraph(tmp_path)
    assert g.frames == ["scan_detector", "scan_source", "world"]
    t, q, extrapolated = g.lookup("scan_detector", "scan_source", 2000)
    np.testing.assert_allclose(t, [0, 0, FDD], atol=1e-12)  # source on the detector's z axis
    assert not extrapolated
    assert g.lookup("world", "scan_source", 99999)[2]


def test_projection_matrix_centre():
    P = rbscan.projection_matrix([0.5, 0, 0], [-0.5, 0, 0], [0, 1, 0], [0, 0, -1],
                                 1e-3, 1e-3, 101, 51)
    x = P @ [0, 0, 0, 1]
    assert x[0] / x[2] == pytest.approx(50) and x[1] / x[2] == pytest.approx(25)


class CliResult:
    def __init__(self, returncode, stdout):
        self.returncode = returncode
        self.stdout = stdout


def run_cli(*args):
    """Runs the CLI in this process, so it uses the same rbscan build as the tests."""
    from contextlib import redirect_stdout
    from io import StringIO

    from rbscan.__main__ import main

    out = StringIO()
    with redirect_stdout(out):
        code = main([str(a) for a in args])
    return CliResult(code, out.getvalue())


def test_cli(tmp_path):
    write_scan(tmp_path)
    out = run_cli("info", tmp_path)
    assert out.returncode == 0 and "raybotic.xray_detector/1" in out.stdout
    out = run_cli("resolve-geometry", tmp_path)
    assert out.returncode == 0 and "8 projections" in out.stdout
    out = run_cli("validate", tmp_path, "--json")
    assert out.returncode == 0 and json.loads(out.stdout)["ok"]

    (tmp_path / "sensors/detector_0/image/seg_000001.h5").unlink()
    out = run_cli("validate", tmp_path)
    assert out.returncode == 1 and "out of sequence" in out.stdout
    assert run_cli("schemas").stdout.count("raybotic.") == 14
