"""The generated typed helpers (rbscan.schema_api): every schema's attributes and
series columns written and read back unchanged."""

import dataclasses

import numpy as np
import pytest

import rbscan
from rbscan import schema_api as sc

N = 3  # samples per series; named dimensions are 2


def attribute_value(name: str, hint: str, i: int):
    hint = hint.replace(" | None", "")
    if hint == "bool":
        return i % 2 == 0
    if hint == "int":
        return 2  # named dimensions resolve from integer attributes
    if hint == "float":
        return i + 0.25
    if hint == "str":
        return f"v{i}"
    if hint.startswith("list["):
        return [attribute_value(name, hint[5:-1], i + k) for k in range(2)]
    raise AssertionError(f"{name}: {hint}")


def column(spec: dict, i: int) -> np.ndarray:
    dtype = spec["dtype"][0] if isinstance(spec["dtype"], list) else spec["dtype"]
    shape = [2 if isinstance(d, str) else d for d in spec.get("shape", [])]
    values = (np.arange(N * int(np.prod(shape, dtype=int))) + i) % 7
    return values.reshape([N, *shape]).astype(dtype)


@pytest.mark.parametrize("schema_id", sorted(sc.BY_SCHEMA))
def test_round_trip(tmp_path, schema_id):
    cls = sc.BY_SCHEMA[schema_id]
    spec = rbscan.schema(schema_id).to_dict()
    # The generated module uses postponed annotations: f.type is "int | None", ...
    values = {f.name: attribute_value(f.name, f.type, i)
              for i, f in enumerate(dataclasses.fields(cls)) if f.name != "extra"}
    attrs = cls(**values, extra={"custom_note": "kept"})

    written = {}
    with rbscan.ScanWriter.create(tmp_path) as scan:
        node = attrs.add(scan, "n0")
        for name, series_cls in cls.SERIES.items():
            fields = spec["series"][name]["fields"]
            cols = series_cls(time=np.arange(N, dtype=np.int64) + 10,
                              **{f: column(fs, i) for i, (f, fs) in enumerate(fields.items())})
            dims = {d: 2 for fs in fields.values() for d in fs.get("shape", [])
                    if isinstance(d, str)}
            if series_cls.STORAGE == "segmented":
                with node.segmented_series(name, dims=dims) as w:
                    cols.append_to(w)
            else:
                cols.append_to(node.series(name, dims=dims))
            written[name] = cols

    node = rbscan.ScanReader(tmp_path)[f"{cls.CATEGORY}/n0"]
    back = cls.read(node)
    assert back == attrs
    for name, series_cls in cls.SERIES.items():
        got = series_cls.read(node)
        assert len(got) == N
        for f in dataclasses.fields(series_cls):
            a, b = getattr(written[name], f.name), getattr(got, f.name)
            assert b.dtype == np.asarray(a).dtype, f.name
            assert b.tobytes() == np.asarray(a).tobytes(), f.name
        part = series_cls.read(node, 1, 1)
        assert part.time.tolist() == [11]


def test_missing_required_attribute_is_an_error():
    with pytest.raises(ValueError, match="raybotic.xray_source/1"):
        sc.XraySource.from_attributes({"frame_id": "x"})


def test_series_without_rows_reads_empty(tmp_path):
    with rbscan.ScanWriter.create(tmp_path) as scan:
        sc.XraySource(frame_id="s", simulated=True).add(scan, "source_0")
    node = rbscan.ScanReader(tmp_path)["sensors/source_0"]
    assert len(sc.XraySourceState.read(node)) == 0
