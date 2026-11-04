"""Prints every node of a scan through its typed dataclasses (rbscan.schema_api).

For each node, the schema's attribute class (found by schema id in BY_SCHEMA)
reads the attributes, and each series class reads the first rows. Nodes of a
schema without a typed class (a user schema) are listed with their raw
attributes.

    python dump_typed.py D:/scans/part_42 [--rows 3]
"""

from __future__ import annotations

import argparse
import dataclasses

import numpy as np

import rbscan
from rbscan import schema_api as sc


def short(value) -> str:
    if isinstance(value, np.ndarray):
        if value.size <= 6:
            return np.array2string(value, precision=4, separator=", ").replace("\n", "")
        return f"<{value.dtype} {list(value.shape)}>"
    text = repr(value)
    return text if len(text) <= 70 else text[:67] + "..."


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("path")
    parser.add_argument("--rows", type=int, default=3, help="rows to show per series")
    args = parser.parse_args()

    scan = rbscan.ScanReader(args.path)
    for path in scan.node_paths():
        node = scan[path]
        cls = sc.BY_SCHEMA.get(node.schema.id)
        if cls is None:
            print(f"\n{path}  ({node.schema.id}, no typed class)")
            for key, value in node.attributes.items():
                print(f"  {key} = {short(value)}")
            continue
        attrs = cls.read(node)
        print(f"\n{path}  {cls.__name__}")
        for f in dataclasses.fields(attrs):
            value = getattr(attrs, f.name)
            if value is not None and value != {}:
                print(f"  {f.name} = {short(value)}")
        for name, series_cls in cls.SERIES.items():
            if name not in node.series_names:
                continue
            total = len(node[name])
            rows = series_cls.read(node, 0, min(args.rows, total))
            print(f"  [{name}] {series_cls.__name__}, {total} rows")
            for f in dataclasses.fields(rows):
                column = getattr(rows, f.name)
                if column is not None:
                    print(f"    {f.name:<22} {short(column)}")


if __name__ == "__main__":
    main()
