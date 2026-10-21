"""rbscan command line tool (same commands as the C++ rbscan.exe).

    python -m rbscan info <scan> [--json] [--attributes]
    python -m rbscan validate <scan> [--json] [--no-values] [--time-tolerance S]
    python -m rbscan resolve-geometry <scan> [--acquisition NAME] [--overwrite] ...
    python -m rbscan rebuild <scan>
    python -m rbscan schemas [ID]
"""

from __future__ import annotations

import argparse
import json
import sys

import rbscan


def _parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="rbscan", description="RayBotic scan data tool")
    sub = p.add_subparsers(dest="command", required=True)

    info = sub.add_parser("info", help="overview: nodes, series, samples, rates, sizes")
    info.add_argument("scan")
    info.add_argument("--json", action="store_true")
    info.add_argument("--attributes", action="store_true", help="also list node attributes")

    val = sub.add_parser("validate", help="check against schemas and links (exit 1 on errors)")
    val.add_argument("scan")
    val.add_argument("--json", action="store_true")
    val.add_argument("--no-values", action="store_true", help="skip checks that read values")
    val.add_argument("--time-tolerance", type=float, default=1e-3, metavar="S")

    geo = sub.add_parser("resolve-geometry",
                         help="write acquisitions/<name>/projections from the recorded frames")
    geo.add_argument("scan")
    geo.add_argument("--acquisition", default="scan_0")
    geo.add_argument("--source", default="", help="source node path (default: the only one)")
    geo.add_argument("--detector", default="", help="detector node path (default: the only one)")
    geo.add_argument("--world", default="world")
    geo.add_argument("--source-frame", default="")
    geo.add_argument("--detector-frame", default="")
    geo.add_argument("--no-matrix", action="store_true")
    geo.add_argument("--overwrite", action="store_true")

    reb = sub.add_parser("rebuild", help="relink segments and mark the scan complete")
    reb.add_argument("scan")

    sch = sub.add_parser("schemas", help="list schemas, or print one as JSON")
    sch.add_argument("id", nargs="?")
    return p


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "info":
            if args.json:
                print(json.dumps(rbscan.info(args.scan), indent=2))
            else:
                print(rbscan.info_text(args.scan, args.attributes), end="")
            return 0
        if args.command == "validate":
            if args.json:
                report = rbscan.validate(args.scan, not args.no_values, args.time_tolerance)
                print(json.dumps(report, indent=2))
                return 0 if report["ok"] else 1
            text, ok = rbscan.validate_text(args.scan, not args.no_values, args.time_tolerance)
            print(text, end="")
            return 0 if ok else 1
        if args.command == "resolve-geometry":
            r = rbscan.resolve_geometry(
                args.scan, acquisition=args.acquisition, source=args.source,
                detector=args.detector, world_frame=args.world, source_frame=args.source_frame,
                detector_frame=args.detector_frame, projection_matrix=not args.no_matrix,
                overwrite=args.overwrite)
            print(f"{r['acquisition']}/projections: {r['projections']} projections, "
                  f"{r['interpolated']} interpolated, {r['extrapolated']} invalid (outside the "
                  f"recorded frames), {r['robot_unreachable']} robot unreachable, "
                  f"{r['emission_unstable']} emission unstable")
            return 0
        if args.command == "rebuild":
            rbscan.ScanWriter.rebuild(args.scan)
            print(f"relinked {args.scan}")
            return 0
        if args.command == "schemas":
            if args.id:
                print(rbscan.schema(args.id).json)
            else:
                for sid in rbscan.schemas():
                    s = rbscan.schema(sid)
                    print(f"{sid:28} {s.category:13} {'(abstract)' if s.abstract else ''}")
            return 0
    except rbscan.Error as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
