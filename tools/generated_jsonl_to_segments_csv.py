#!/usr/bin/env python3
"""
Convert generated segment JSONL to the CSV format expected by rocks-demo tools.

Input JSONL line format (one segment per line) matches:
  tools/rebuild_generated_trajs_jsonl.py outputs:
    {"segment_id": "...", "points": [["2020-07-18 09:07:17",119.6,31.2], ...], ...}

Outputs:
  - segments_points.csv: long format (one row per point)
  - segments_meta.csv:   one row per segment (optional but useful for segment-mode ingest)

This is intentionally dependency-free (stdlib only).
"""

from __future__ import annotations

import argparse
import csv
import json
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Iterable, Optional


CN_TZ = timezone(timedelta(hours=8))


def parse_time_cn_to_unix(ts: str) -> int:
    dt = datetime.strptime(ts, "%Y-%m-%d %H:%M:%S").replace(tzinfo=CN_TZ)
    return int(dt.timestamp())


@dataclass
class MetaAcc:
    point_count: int = 0
    t_min: Optional[int] = None
    t_max: Optional[int] = None
    lon_min: float = float("inf")
    lon_max: float = float("-inf")
    lat_min: float = float("inf")
    lat_max: float = float("-inf")

    def merge_point(self, t: int, lon: float, lat: float) -> None:
        self.point_count += 1
        self.t_min = t if self.t_min is None else min(self.t_min, t)
        self.t_max = t if self.t_max is None else max(self.t_max, t)
        self.lon_min = min(self.lon_min, lon)
        self.lon_max = max(self.lon_max, lon)
        self.lat_min = min(self.lat_min, lat)
        self.lat_max = max(self.lat_max, lat)


def iter_jsonl(path: Path) -> Iterable[dict]:
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            yield json.loads(s)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--in-jsonl",
        default="data/generated_trajs_segments_p80_120.jsonl",
        help="Input JSONL with per-segment points.",
    )
    ap.add_argument(
        "--out-dir",
        default="data/processed/generated",
        help="Output directory for segments_points.csv and segments_meta.csv.",
    )
    ap.add_argument(
        "--max-segments",
        type=int,
        default=2000,
        help="For smoke: convert at most N segments (-1 means all).",
    )
    ap.add_argument(
        "--max-points-per-segment",
        type=int,
        default=-1,
        help="For smoke: cap points per segment (-1 means all).",
    )
    args = ap.parse_args()

    in_jsonl = Path(args.in_jsonl)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    points_path = out_dir / "segments_points.csv"
    meta_path = out_dir / "segments_meta.csv"

    n_seg = 0
    n_pt = 0
    n_bad = 0

    with points_path.open("w", encoding="utf-8", newline="") as fp, meta_path.open(
        "w", encoding="utf-8", newline=""
    ) as fm:
        wp = csv.writer(fp)
        wm = csv.writer(fm)

        wp.writerow(
            [
                "segment_id",
                "dataset",
                "point_index",
                "unix_time_s",
                "lon",
                "lat",
                "alt_m",
                "source_ref",
            ]
        )
        wm.writerow(
            [
                "segment_id",
                "dataset",
                "point_count",
                "t_start_unix",
                "t_end_unix",
                "lon_min",
                "lon_max",
                "lat_min",
                "lat_max",
                "source_ref",
            ]
        )

        for obj in iter_jsonl(in_jsonl):
            if args.max_segments >= 0 and n_seg >= args.max_segments:
                break
            seg_id = str(obj.get("segment_id", "")).strip()
            pts = obj.get("points")
            if not seg_id or not isinstance(pts, list) or not pts:
                n_bad += 1
                continue

            acc = MetaAcc()
            max_pps = args.max_points_per_segment
            for i, p in enumerate(pts):
                if max_pps >= 0 and i >= max_pps:
                    break
                if not isinstance(p, list) or len(p) < 3:
                    n_bad += 1
                    continue
                try:
                    t = parse_time_cn_to_unix(str(p[0]))
                    lon = float(p[1])
                    lat = float(p[2])
                except Exception:
                    n_bad += 1
                    continue
                acc.merge_point(t, lon, lat)
                wp.writerow(
                    [
                        seg_id,
                        "generated",
                        i,
                        t,
                        f"{lon:.7f}",
                        f"{lat:.7f}",
                        "0.000",
                        "generated_jsonl",
                    ]
                )
                n_pt += 1

            if acc.point_count <= 0 or acc.t_min is None or acc.t_max is None:
                n_bad += 1
                continue

            wm.writerow(
                [
                    seg_id,
                    "generated",
                    acc.point_count,
                    acc.t_min,
                    acc.t_max,
                    f"{acc.lon_min:.7f}",
                    f"{acc.lon_max:.7f}",
                    f"{acc.lat_min:.7f}",
                    f"{acc.lat_max:.7f}",
                    "generated_jsonl",
                ]
            )
            n_seg += 1

    print(f"in_jsonl={in_jsonl}")
    print(f"out_dir={out_dir}")
    print(f"segments={n_seg} points={n_pt} bad_rows={n_bad}")
    print(f"segments_points_csv={points_path}")
    print(f"segments_meta_csv={meta_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

