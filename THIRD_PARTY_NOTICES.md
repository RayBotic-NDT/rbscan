# Third-party notices

rbscan itself is MIT licensed (see [LICENSE](LICENSE)). Its binaries contain or
ship the following third-party software. The full license texts are in
[LICENSES/](LICENSES/) and are installed with the C++ SDK
(`share/rbscan/licenses/`) and the Python wheel (`*.dist-info/licenses/`).

| Component | Version | License | Where it ends up | License text |
| :--- | :--- | :--- | :--- | :--- |
| [HDF5](https://github.com/HDFGroup/hdf5) | 1.14.6 | BSD-style (HDF5 license) | `rbscan_hdf5.dll` / `librbscan_hdf5.so` in SDK and wheel | [HDF5.txt](LICENSES/HDF5.txt), [HDF5-LBNL.txt](LICENSES/HDF5-LBNL.txt) |
| [zlib](https://zlib.net) | 1.3.1 | zlib | statically linked into the HDF5 library | [zlib.txt](LICENSES/zlib.txt) |
| [libdeflate](https://github.com/ebiggers/libdeflate) | 1.24 | MIT | statically linked into `rbscan` | [libdeflate.txt](LICENSES/libdeflate.txt) |
| [LZ4](https://github.com/lz4/lz4) | 1.10.0 | BSD-2-Clause (`lib/`) | statically linked into `rbscan` | [LZ4.txt](LICENSES/LZ4.txt) |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | compiled into `rbscan` (header-only) | [nlohmann_json.txt](LICENSES/nlohmann_json.txt) |
| [pybind11](https://github.com/pybind/pybind11) | ≥ 3.0 | BSD-3-Clause | compiled into the Python module `_rbscan` (header-only) | [pybind11.txt](LICENSES/pybind11.txt) |

Build- and test-only dependencies that are not shipped: GoogleTest
(BSD-3-Clause), scikit-build-core (Apache-2.0), numpy (BSD-3-Clause, a runtime
dependency of the Python package installed separately by pip).

The Bitshuffle+LZ4 codec in `src/compression.cpp` is an independent
implementation of the chunk format of
[bitshuffle](https://github.com/kiyo-masui/bitshuffle) (HDF5 filter 32008), so
files stay readable with `hdf5plugin`; no bitshuffle source code is included.
