# Changelog

All notable changes to rbscan are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the library uses
[Semantic Versioning](https://semver.org/).

The library version (below) and the on-disk **format version** (`format_version`
in every scan, see [docs/format.md](docs/format.md)) are independent: a
library release can read every format version with the same major number
(`1.x`). Schemas carry their own version (`raybotic.xray_detector/1`).

## [Unreleased]

## [0.2.1] - 2026-11-02

First release published on PyPI and GitHub. Writes and reads format version 1.0.

### Fixed

- The Linux release build no longer stops without a message on a fresh CI
  workspace (the reason 0.2.0 was never published).

### Added

- Scan directories with an HDF5 master file (`scan.rbscan.h5`) and segmented
  sensor streams (`seg_NNNNNN.h5`), linked into the master as virtual datasets.
- Inline and segmented series, multi-process recording, live reading via SWMR,
  appending to stored inline series and `refresh()` while reading.
- Compression: Bitshuffle+LZ4 (default, HDF5 filter 32008, readable with
  `hdf5plugin`), shuffle+deflate, deflate, none; parallel encoding and decoding
  outside the global HDF5 lock.
- Built-in schemas for X-ray source and detector, cameras, depth cameras,
  profile scanners, force/torque sensors, robot arms, transforms, CT
  acquisitions, volumes, annotations, reconstruction settings, trajectory plans
  and EZRT imports; custom schemas at runtime or via `RBSCAN_SCHEMA_PATH`.
- Tools in C++, Python and the `rbscan` CLI: `info`, `validate`,
  `resolve-geometry`, `rebuild`, `schemas`.
- `rebuild` recovers a scan whose recording process crashed: it clears the
  HDF5 status flags the crashed writer left in the master file and relinks
  every segment on disk.
- C++ SDK (Windows, Linux) with CMake package config.
- On PyPI (`pip install rbscan`): wheels for CPython 3.11–3.14 on Windows x64
  and Linux x86_64 (`manylinux_2_28`), plus the source distribution.
- `Array::values<T>()` returns a `std::span` on a named array and an owning
  `std::vector<T>` on a temporary (`reader.read(...).values<T>()`), so the
  result never dangles.

## [0.2.0] - 2026-11-02

Tagged but not published: its Linux release build failed. Everything is in
0.2.1.
