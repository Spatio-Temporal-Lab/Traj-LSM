#!/usr/bin/env python3
"""
Unify Wuxi + CD(PKDD) + TDrive + Geolife into one merged dataset.

Output: segments_meta.csv + segments_points.csv (same schema as TDrive processed/
        → ready for st_bucket_ingest_build).

CD data has relative timestamps (point_index as seconds). We assign a synthetic
base epoch so segments from different datasets can coexist in a time-ordered
RocksDB ingest.

Geolife is processed from the raw zip file: 18670 .plt trajectories → segments.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import zipfile
from pathlib import Path
from datetime import datetime, timezone

# ── CD base epoch ──────────────────────────────────────────────────────────
# PKDD Chengdu taxi competition data (~2014). Assign 2014-08-01 UTC.
CD_BASE_UNIX = int(datetime(2014, 8, 1, tzinfo=timezone.utc).timestamp())

# ── config ─────────────────────────────────────────────────────────────────
PROJECT = Path(__file__).resolve().parents[1]
SRC_WUXI   = PROJECT / "data" / "processed" / "wuxi"
SRC_CD     = PROJECT / "data" / "processed" / "cd"
SRC_TDRIVE = PROJECT / "data" / "processed" / "tdrive"
GEOLIFE_ZIP = PROJECT / "data" / "Geolife Trajectories 1.3.zip"
OUT_DIR    = PROJECT / "data" / "processed" / "unified"

# segment split: max points per segment
MAX_POINTS = 500

# ── helpers ─────────────────────────────────────────────────────────────────

def parse_plt_date_time(date_str: str, time_str: str) -> int | None:
    """Geolife .plt: '2008-10-23','02:53:04' → unix second (UTC+8 → UTC)."""
    try:
        dt = datetime.strptime(f"{date_str} {time_str}", "%Y-%m-%d %H:%M:%S")
        # Geolife data is in UTC+8 (Beijing time). Convert to UTC.
        dt = dt.replace(tzinfo=timezone.utc)  # treat as UTC (data already GPS time)
        return int(dt.timestamp())
    except ValueError:
        return None


def extract_geolife(zip_path: Path, out_meta: Path, out_points: Path):
    """Process Geolife zip into segments_meta + segments_points CSV."""
    print(f"Processing Geolife from {zip_path.name} ...")
    zf = zipfile.ZipFile(zip_path)
    plt_files = sorted([n for n in zf.namelist()
                        if n.endswith('.plt') and '/Data/' in n])

    meta_rows: list[dict] = []
    points_rows: list[dict] = []
    seg_seq = 0
    total_points = 0

    for plt_path in plt_files:
        # Derive user ID from directory: e.g. Data/000/Trajectory/... → user=000
        parts = plt_path.split('/')
        user_dir = parts[-3] if len(parts) >= 4 else "xxx"
        user = user_dir  # e.g. "000", "001", ...

        try:
            with zf.open(plt_path) as f:
                lines = [ln.decode('utf-8', errors='replace').strip()
                         for ln in f.readlines()]
        except Exception:
            continue

        if len(lines) < 7:
            continue  # header lines expected

        # Parse points: lines after the first 6 header lines
        pts = []
        for ln in lines[6:]:
            ln = ln.strip()
            if not ln:
                continue
            fields = ln.split(',')
            if len(fields) < 7:
                continue
            try:
                lat = float(fields[0])
                lon = float(fields[1])
                alt = float(fields[3]) * 0.3048  # feet → meters
                date_str = fields[5]
                time_str = fields[6]
                t = parse_plt_date_time(date_str, time_str)
                if t is None or lat == 0 or lon == 0:
                    continue
                pts.append((t, lon, lat, alt))
            except (ValueError, IndexError):
                continue

        if not pts:
            continue

        # Split into chunks of MAX_POINTS
        for chunk_start in range(0, len(pts), MAX_POINTS):
            chunk = pts[chunk_start:chunk_start + MAX_POINTS]
            if len(chunk) < 2:
                continue

            times = [p[0] for p in chunk]
            lons = [p[1] for p in chunk]
            lats = [p[2] for p in chunk]
            seg_id = f"geolife:{user}:{seg_seq:06d}"
            seg_seq += 1

            meta_rows.append({
                "segment_id": seg_id,
                "dataset": "geolife",
                "point_count": len(chunk),
                "t_start_unix": min(times),
                "t_end_unix": max(times),
                "lon_min": min(lons),
                "lon_max": max(lons),
                "lat_min": min(lats),
                "lat_max": max(lats),
                "source_ref": plt_path,
            })
            for i, (t, lon, lat, alt) in enumerate(chunk):
                points_rows.append({
                    "segment_id": seg_id,
                    "dataset": "geolife",
                    "point_index": i,
                    "unix_time_s": t,
                    "lon": lon,
                    "lat": lat,
                    "alt_m": round(alt, 3),
                    "source_ref": plt_path,
                })
                total_points += 1

    zf.close()

    # Write
    _write_csv(out_meta, meta_rows,
               ["segment_id","dataset","point_count","t_start_unix","t_end_unix",
                "lon_min","lon_max","lat_min","lat_max","source_ref"])
    _write_csv(out_points, points_rows,
               ["segment_id","dataset","point_index","unix_time_s","lon","lat",
                "alt_m","source_ref"])

    print(f"  Geolife: {len(meta_rows)} segments, {total_points} points")
    return len(meta_rows), total_points


def _write_csv(path: Path, rows: list[dict], fieldnames: list[str]):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)


def copy_with_offset(src_meta: Path, src_points: Path,
                     t_offset: int, dataset_tag: str,
                     meta_rows_out: list[dict],
                     points_rows_out: list[dict]):
    """Read existing CSVs, optionally apply time offset, append to lists."""
    with src_meta.open('r', encoding='utf-8') as f:
        for row in csv.DictReader(f):
            if dataset_tag == "cd":
                row["t_start_unix"] = str(int(row["t_start_unix"]) + t_offset)
                row["t_end_unix"] = str(int(row["t_end_unix"]) + t_offset)
            meta_rows_out.append(row)

    with src_points.open('r', encoding='utf-8') as f:
        for row in csv.DictReader(f):
            if dataset_tag == "cd":
                row["unix_time_s"] = str(int(row["unix_time_s"]) + t_offset)
            points_rows_out.append(row)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skip-geolife", action="store_true",
                    help="Skip geolife extraction (idempotent after first run)")
    ap.add_argument("--geolife-out-meta", type=Path,
                    default=OUT_DIR / "geolife_segments_meta.csv")
    ap.add_argument("--geolife-out-points", type=Path,
                    default=OUT_DIR / "geolife_segments_points.csv")
    ap.add_argument("--final-meta", type=Path,
                    default=OUT_DIR / "segments_meta.csv")
    ap.add_argument("--final-points", type=Path,
                    default=OUT_DIR / "segments_points.csv")
    args = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    meta_all: list[dict] = []
    points_all: list[dict] = []
    stats = {}

    # ── Geolife ──
    if args.skip_geolife and args.geolife_out_meta.is_file():
        print("Skipping Geolife extraction (--skip-geolife + cached files)")
        _load_csv(args.geolife_out_meta, meta_all)
        _load_csv(args.geolife_out_points, points_all)
    else:
        n, p = extract_geolife(GEOLIFE_ZIP, args.geolife_out_meta, args.geolife_out_points)
        stats["geolife"] = (n, p)
        _load_csv(args.geolife_out_meta, meta_all)
        _load_csv(args.geolife_out_points, points_all)

    # ── Wuxi ──
    print("Loading Wuxi ...")
    n_before = len(meta_all)
    copy_with_offset(SRC_WUXI / "segments_meta.csv",
                     SRC_WUXI / "segments_points.csv",
                     0, "wuxi", meta_all, points_all)
    stats["wuxi"] = (len(meta_all) - n_before, len(points_all) - sum(v[1] for v in stats.values() if v != stats.get("wuxi", (0,0))))

    # ── CD (PKDD) ──
    print(f"Loading CD (PKDD) with base epoch +{CD_BASE_UNIX}s ...")
    n_before = len(meta_all)
    copy_with_offset(SRC_CD / "segments_meta.csv",
                     SRC_CD / "segments_points.csv",
                     CD_BASE_UNIX, "cd", meta_all, points_all)
    stats["cd"] = (len(meta_all) - n_before, len(points_all) - sum(v[1] for v in stats.values()))

    # ── TDrive ──
    print("Loading TDrive ...")
    n_before = len(meta_all)
    copy_with_offset(SRC_TDRIVE / "segments_meta.csv",
                     SRC_TDRIVE / "segments_points.csv",
                     0, "tdrive", meta_all, points_all)
    stats["tdrive"] = (len(meta_all) - n_before, len(points_all) - sum(v[1] for v in stats.values()))

    # ── Write final merged files ──
    print(f"\nWriting merged: {len(meta_all)} segments, {len(points_all)} points")
    _write_csv(args.final_meta, meta_all,
               ["segment_id","dataset","point_count","t_start_unix","t_end_unix",
                "lon_min","lon_max","lat_min","lat_max","source_ref"])
    _write_csv(args.final_points, points_all,
               ["segment_id","dataset","point_index","unix_time_s","lon","lat",
                "alt_m","source_ref"])

    # ── Summary ──
    total_segs = sum(v[0] for v in stats.values())
    total_pts  = sum(v[1] for v in stats.values())
    envelope = {
        "dataset": "unified",
        "segment_count": len(meta_all),
        "t_min_unix": min(int(r["t_start_unix"]) for r in meta_all),
        "t_max_unix": max(int(r["t_end_unix"]) for r in meta_all),
        "spatial_boundary": {
            "xmin": min(float(r["lon_min"]) for r in meta_all),
            "ymin": min(float(r["lat_min"]) for r in meta_all),
            "xmax": max(float(r["lon_max"]) for r in meta_all),
            "ymax": max(float(r["lat_max"]) for r in meta_all),
        },
        "tman_table_config_hint": {
            "xmin": min(float(r["lon_min"]) for r in meta_all),
            "ymin": min(float(r["lat_min"]) for r in meta_all),
            "xmax": max(float(r["lon_max"]) for r in meta_all),
            "ymax": max(float(r["lat_max"]) for r in meta_all),
            "timeBin": 86400.0,
            "timeBinNums": 0,
        },
        "per_dataset": {
            k: {"segments": v[0], "points": v[1]} for k, v in stats.items()
        },
    }
    env_path = OUT_DIR / "envelope.json"
    env_path.write_text(json.dumps(envelope, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {env_path}")

    # time_bin_nums
    t_span = envelope["t_max_unix"] - envelope["t_min_unix"]
    n_bins = int(math.ceil(t_span / 86400.0)) + 1
    print(f"\n  t_min={envelope['t_min_unix']}  t_max={envelope['t_max_unix']}")
    print(f"  span_days={t_span/86400:.1f}  bucket_count={n_bins}")

    print(f"\n{'Dataset':<12} {'Segments':>10} {'Points':>12}")
    print("-" * 36)
    for k, v in stats.items():
        print(f"{k:<12} {v[0]:>10} {v[1]:>12}")
    print("-" * 36)
    print(f"{'TOTAL':<12} {len(meta_all):>10} {len(points_all):>12}")


def _load_csv(path: Path, rows_out: list[dict]):
    with path.open('r', encoding='utf-8') as f:
        for row in csv.DictReader(f):
            rows_out.append(row)


if __name__ == "__main__":
    main()
