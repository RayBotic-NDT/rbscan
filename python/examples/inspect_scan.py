"""Prints what a scan holds: nodes, schemas, series and their sizes.

    python inspect_scan.py D:/scans/part_42
"""

import sys

import rbscan


def main(path: str) -> None:
    scan = rbscan.ScanReader(path)
    attrs = scan.attributes
    print(f"{scan.directory}  format {scan.format_version}  complete={scan.complete}")
    for key in ("uuid", "created", "description", "operator", "sample_id"):
        if key in attrs:
            print(f"  {key}: {attrs[key]}")
    for node_path in scan.node_paths():
        node = scan[node_path]
        print(f"\n{node_path}  ({node.schema.id})")
        for name in node.series_names:
            s = node[name]
            fields = ", ".join(
                f"{f}{list(s.sample_shape(f)) or ''} {s.dtype(f)}" for f in s.fields)
            span = ""
            if len(s):
                t = s.time()
                span = f", {(t[-1] - t[0]) / 1e9:.1f} s"
            print(f"  {name} [{s.storage}]: {len(s)} samples{span}  ({fields})")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
