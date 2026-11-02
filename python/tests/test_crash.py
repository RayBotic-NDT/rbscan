"""A recording process that is killed mid-scan leaves a scan rebuild can repair."""

import os
import subprocess
import sys
import textwrap
import time

import numpy as np
import pytest

import rbscan

H, W = 16, 24

RECORDER = textwrap.dedent(
    """
    import os, sys
    package_dir = os.environ.get("RBSCAN_TEST_PACKAGE_DIR")
    if package_dir:
        sys.path.insert(0, package_dir)
    import numpy as np
    import rbscan

    scan = rbscan.ScanWriter.create(sys.argv[1], description="crash test")
    src = scan.add_node("source_0", "raybotic.xray_source", frame_id="scan_source",
                        simulated=True, voltage_min_kv=20.0, voltage_max_kv=225.0,
                        current_max_ua=1000.0, power_max_w=320.0)
    det = scan.add_node("detector_0", "raybotic.xray_detector", frame_id="scan_detector",
                        simulated=True, sensor_width={W}, sensor_height={H},
                        pixel_pitch_u_um=150.0, pixel_pitch_v_um=150.0)
    state = src.series("state")
    images = det.segmented_series("image", dims={{"height": {H}, "width": {W}}},
                                  segment_size=4, flush_every=1)
    scan.flush()
    i = 0
    while True:
        images.append(1000 * i, image=np.full(({H}, {W}), i % 65536, np.uint16))
        images.flush()
        print(i + 1, flush=True)
        i += 1
    """
).format(H=H, W=W)


def test_rebuild_after_killed_recording(tmp_path):
    scan_dir = tmp_path / "scan"
    child = subprocess.Popen([sys.executable, "-c", RECORDER, str(scan_dir)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             env=os.environ.copy())
    try:
        written = 0
        while written < 10:
            line = child.stdout.readline()
            assert line, "recorder exited early:\n" + child.stderr.read()
            written = int(line)
        # File locking keeps rebuild away from a writer that is still alive.
        with pytest.raises(rbscan.Error):
            rbscan.ScanWriter.rebuild(scan_dir)
    finally:
        child.kill()  # no finalize, no destructors: like a crash or power loss
        child.wait(timeout=30)

    # The master was not closed: readers refuse it and point at rebuild.
    with pytest.raises(rbscan.Error, match="rbscan rebuild"):
        rbscan.ScanReader(scan_dir)

    # Windows drops the file locks of a killed process a moment later.
    deadline = time.monotonic() + 10
    while True:
        try:
            rbscan.ScanReader(scan_dir)
        except rbscan.Error as e:
            if "already open for write" in str(e) or time.monotonic() > deadline:
                break
            time.sleep(0.05)

    rbscan.ScanWriter.rebuild(scan_dir)

    scan = rbscan.ScanReader(scan_dir)
    assert scan.complete
    images = scan["sensors/detector_0"]["image"]
    # Everything flushed before the kill survives; the sample being written may not.
    assert len(images) >= written
    stack = images.read("image")
    for i in range(len(images)):
        np.testing.assert_array_equal(stack[i], np.full((H, W), i, np.uint16))
    report = rbscan.validate(scan_dir)
    assert report["ok"], report["issues"]
