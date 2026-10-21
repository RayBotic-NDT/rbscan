"""Benchmark and comparison between rbscan and standard h5py.

Demonstrates:
  1. Multi-threaded batch loading & decompression (bulk read vs h5py)
  2. Multi-threaded concurrent loading with ThreadPoolExecutor (viewer cache simulation)
  3. Bit-exact data verification

Usage:
    python python/examples/compare_readers.py [scan_dir] [--count 60] [--size 1024]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import math
import shutil
import tempfile
import time
from pathlib import Path

import h5py
import numpy as np

import rbscan

DETECTOR = "sensors/detector_0"


def generate_synthetic_scan(scan_dir: Path, n: int, size: int) -> None:
    """Creates a scan with realistic compressed phantom images."""
    print(f"Generating synthetic scan ({n} frames, {size}x{size}, compressed with shuffle+deflate)...")
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)

    with rbscan.ScanWriter.create(scan_dir, description="benchmark scan", overwrite=True) as scan:
        det = scan.add_node(
            "detector_0", "raybotic.xray_detector",
            frame_id="scan_detector", simulated=True,
            sensor_width=size, sensor_height=size,
            pixel_pitch_u_um=139.0, pixel_pitch_v_um=139.0
        )
        images = det.segmented_series("image", dims={"height": size, "width": size})

        for i in range(n):
            angle = 2.0 * math.pi * i / n
            cx = 0.5 * size + 0.2 * size * math.cos(angle)
            absorb = np.exp(-((x - cx) ** 2 + (y - 0.5 * size) ** 2) / (0.09 * size * size))
            texture = (x.astype(np.int32) * 31 + y.astype(np.int32) * 17) & 63
            frame = (52000.0 * (1.0 - 0.7 * absorb) + texture).astype(np.uint16)
            images.append(i * 100_000_000, image=frame)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scan_dir", nargs="?", type=Path, default=None,
                        help="Existing scan directory. If omitted, a temporary scan is created.")
    parser.add_argument("--count", type=int, default=60, help="Number of projections to read.")
    parser.add_argument("--size", type=int, default=1024, help="Frame size if generating temporary scan.")
    args = parser.parse_args()

    temp_dir = None
    if args.scan_dir is None:
        temp_dir = tempfile.mkdtemp(prefix="rbscan_bench_")
        scan_dir = Path(temp_dir)
        generate_synthetic_scan(scan_dir, args.count, args.size)
    else:
        scan_dir = args.scan_dir

    try:
        master_path = scan_dir / rbscan.MASTER_FILE_NAME
        if not master_path.exists():
            raise FileNotFoundError(f"Master file not found at {master_path}")

        print(f"\nTarget scan: {scan_dir}")

        # -------------------------------------------------------------
        # Part 1: Bulk reading comparison
        # -------------------------------------------------------------
        print("\n" + "=" * 65)
        print("PART 1: Bulk Loading & Decompression (Batch of frames)")
        print("=" * 65)

        # 1a. h5py
        with h5py.File(master_path, "r") as f:
            ds = f[f"{DETECTOR}/image/image"]
            total_samples = ds.shape[0]
            count = min(args.count, total_samples)
            shape = (count, ds.shape[1], ds.shape[2])
            raw_bytes = count * ds.shape[1] * ds.shape[2] * ds.dtype.itemsize
            raw_mb = raw_bytes / (1024 * 1024)

            t0 = time.perf_counter()
            data_h5py = ds[0:count]
            h5py_sec = time.perf_counter() - t0

        h5py_mb_s = raw_mb / h5py_sec
        print(f"h5py   (single-threaded HDF5): {h5py_mb_s:7.1f} MB/s ({h5py_sec*1000:6.1f} ms)")

        # 1b. rbscan
        reader = rbscan.ScanReader(scan_dir)
        series = reader[DETECTOR]["image"]

        t0 = time.perf_counter()
        data_rbscan = series.read("image", 0, count)
        rbscan_sec = time.perf_counter() - t0

        rbscan_mb_s = raw_mb / rbscan_sec
        bulk_speedup = h5py_sec / rbscan_sec if rbscan_sec > 0 else float("inf")
        print(f"rbscan (multi-threaded pool):  {rbscan_mb_s:7.1f} MB/s ({rbscan_sec*1000:6.1f} ms) -> {bulk_speedup:.2f}x faster")

        assert np.array_equal(data_h5py, data_rbscan), "Mismatch between h5py and rbscan output!"
        print("Data verification: MATCH (bit-exact output)")

        # -------------------------------------------------------------
        # Part 2: Concurrent multi-threaded reading (e.g. Viewer cache)
        # -------------------------------------------------------------
        print("\n" + "=" * 65)
        print("PART 2: Concurrent Reads across Python Worker Threads")
        print("=" * 65)
        print("Simulating concurrent frame requests across worker threads (e.g., UI cache):")

        thread_configs = [1, 2, 4, 8]

        print("\n[h5py - Serialized by HDF5 library lock]")
        with h5py.File(master_path, "r") as f:
            ds = f[f"{DETECTOR}/image/image"]
            for workers in thread_configs:
                t0 = time.perf_counter()
                with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
                    list(ex.map(lambda i: ds[i], range(count)))
                t = time.perf_counter() - t0
                print(f"  {workers:2d} threads: {raw_mb/t:7.1f} MB/s ({t*1000:6.1f} ms)")

        print("\n[rbscan - Parallel reads via RawFile outside HDF5 lock]")
        for workers in thread_configs:
            t0 = time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
                list(ex.map(lambda i: series.read_sample("image", i), range(count)))
            t = time.perf_counter() - t0
            speedup_vs_1 = (raw_mb / t) / (raw_mb / (t0 if False else 1.0))
            print(f"  {workers:2d} threads: {raw_mb/t:7.1f} MB/s ({t*1000:6.1f} ms)")

        print("\n" + "=" * 65)
        print("SUMMARY:")
        print("  - h5py does not scale with threads because HDF5 serializes under a global lock.")
        print("  - rbscan releases the GIL and uses RawFile handles to decompress concurrently.")
        print("=" * 65)

    finally:
        if temp_dir is not None:
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
