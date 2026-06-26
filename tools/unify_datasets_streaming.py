#!/usr/bin/env python3
"""
Streaming merge of Wuxi + CD(PKDD) + TDrive + Geolife into one unified dataset.

Unlike unify_datasets.py (which loads everything into RAM), this version
streams line-by-line to avoid OOM with ~8.7 GB of point data.

Output: segments_meta.csv + segments_points.csv + envelope.json
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
from pathlib import Path
from datetime import datetime, timezone

# ── CD base epoch ──────────────────────────────────────────────────────────
CD_BASE_UNIX = int(datetime(2014, 8, 1, tzinfo=timezone.utc).timestamp())

# ── config ─────────────────────────────────────────────────────────────────
PROJECT = Path(__file__).resolve().parents[1]
SRC = {
    "wuxi":   PROJECT / "data" / "processed" / "wuxi",
    "cd":     PROJECT / "data" / "processed" / "cd",
    "tdrive": PROJECT / "data" / "processed" / "tdrive",
    "geolife": PROJECT / "data" / "processed" / "unified",  # pre-extracted
}
OUT_DIR = PROJECT / "data" / "processed" / "unified"

META_FIELDNAMES = ["segment_id", "dataset", "point_count",
                   "t_start_unix", "t_end_unix",
                   "lon_min", "lon_max", "lat_min", "lat_max", "source_ref"]

POINTS_FIELDNAMES = ["segment_id", "dataset", "point_index", "unix_time_s",
                     "lon", "lat", "alt_m", "source_ref"]


def stream_merge(datasets: list[str], out_meta: Path, out_points: Path):
    """
    Stream-merge the named datasets into out_meta + out_points.
    Applies CD time offset for the 'cd' dataset.
    Returns (total_segments, total_points, t_min, t_max, xmin, ymin, xmax, ymax).
    """
    out_meta.parent.mkdir(parents=True, exist_ok=True)

    total_segs = 0
    total_pts = 0
    t_min = float("inf")
    t_max = float("-inf")
    xmin, ymin = float("inf"), float("inf")
    xmax, ymax = float("-inf"), float("-inf")

    with out_meta.open("w", newline="", encoding="utf-8") as f_meta, \
         out_points.open("w", newline="", encoding="utf-8") as f_pts:

        w_meta = csv.DictWriter(f_meta, fieldnames=META_FIELDNAMES,
                                extrasaction="ignore")
        w_pts = csv.DictWriter(f_pts, fieldnames=POINTS_FIELDNAMES,
                               extrasaction="ignore")
        w_meta.writeheader()
        w_pts.writeheader()

        for ds in datasets:
            print(f"Processing {ds} ...", flush=True)
            src_dir = SRC[ds]
            meta_path = src_dir / "segments_meta.csv"
            points_path = src_dir / "segments_points.csv"

            if not meta_path.is_file() or not points_path.is_file():
                print(f"  SKIP: missing {meta_path} or {points_path}", flush=True)
                continue

            ds_segs = 0
            ds_pts = 0

            # Stream meta rows
            with meta_path.open("r", encoding="utf-8") as fm:
                reader = csv.DictReader(fm)
                for row in reader:
                    row["dataset"] = ds
                    if ds == "cd":
                        row["t_start_unix"] = str(int(row["t_start_unix"]) + CD_BASE_UNIX)
                        row["t_end_unix"]   = str(int(row["t_end_unix"])   + CD_BASE_UNIX)

                    t_s = int(row["t_start_unix"])
                    t_e = int(row["t_end_unix"])
                    lo_min = float(row["lon_min"])
                    lo_max = float(row["lon_max"])
                    la_min = float(row["lat_min"])
                    la_max = float(row["lat_max"])

                    if t_s < t_min: t_min = t_s
                    if t_e > t_max: t_max = t_e
                    if lo_min < xmin: xmin = lo_min
                    if lo_max > xmax: xmax = lo_max
                    if la_min < ymin: ymin = la_min
                    if la_max > ymax: ymax = la_max

                    w_meta.writerow(row)
                    ds_segs += 1

            # Stream points rows
            with points_path.open("r", encoding="utf-8") as fp:
                reader = csv.DictReader(fp)
                for row in reader:
                    row["dataset"] = ds
                    if ds == "cd":
                        row["unix_time_s"] = str(int(row["unix_time_s"]) + CD_BASE_UNIX)
                    w_pts.writerow(row)
                    ds_pts += 1

            print(f"  {ds}: {ds_segs:,} segments, {ds_pts:,} points", flush=True)
            total_segs += ds_segs
            total_pts += ds_pts

    return total_segs, total_pts, int(t_min), int(t_max), xmin, ymin, xmax, ymax


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--datasets", nargs="+",
                    default=["wuxi", "cd", "tdrive", "geolife"],
                    help="Which datasets to merge (default: all four)")
    ap.add_argument("--out-meta", type=Path,
                    default=OUT_DIR / "segments_meta.csv")
    ap.add_argument("--out-points", type=Path,
                    default=OUT_DIR / "segments_points.csv")
    ap.add_argument("--out-envelope", type=Path,
                    default=OUT_DIR / "envelope.json")
    args = ap.parse_args()

    # ── Phase: merge ──
    total_segs, total_pts, t_min, t_max, xmin, ymin, xmax, ymax = \
        stream_merge(args.datasets, args.out_meta, args.out_points)

    # ── Envelope ──
    t_span = t_max - t_min
    n_bins = int(math.ceil(t_span / 86400.0)) + 1

    envelope = {
        "dataset": "unified",
        "datasets_included": args.datasets,
        "segment_count": total_segs,
        "point_count": total_pts,
        "t_min_unix": t_min,
        "t_max_unix": t_max,
        "span_days": round(t_span / 86400.0, 1),
        "bucket_count": n_bins,
        "spatial_boundary": {
            "xmin": xmin, "ymin": ymin,
            "xmax": xmax, "ymax": ymax,
        },
        "tman_table_config_hint": {
            "xmin": xmin, "ymin": ymin,
            "xmax": xmax, "ymax": ymax,
            "timeBin": 86400.0,
            "timeBinNums": n_bins,
        },
    }
    args.out_envelope.write_text(json.dumps(envelope, indent=2) + "\n",
                                 encoding="utf-8")

    # ── Summary ──
    print(f"\n{'='*50}")
    print(f"Unified dataset: {total_segs:,} segments, {total_pts:,} points")
    print(f"Time range: {t_min} → {t_max}  ({t_span/86400:.1f} days, {n_bins} buckets)")
    print(f"Spatial:  lon [{xmin:.4f}, {xmax:.4f}]  lat [{ymin:.4f}, {ymax:.4f}]")
    print(f"Output:")
    print(f"  {args.out_meta}")
    print(f"  {args.out_points}")
    print(f"  {args.out_envelope}")


if __name__ == "__main__":
    main()
