#!/usr/bin/env python3
"""Generate TDrive dense ST query suites: 20 windows, each >=100 point-truth segments.

Each window embeds ground truth (segment_id list + counts) at generation time via a
full scan of segments_points.csv — no post-hoc validation needed for finalSize.

Outputs per suite under --out-root/<suite>/:
  query_conditions_st.txt   (TMan STQuery format, ';' separated, 6 fields)
  queries_manifest_st.json  (per-window bbox/time + segment_ids + counts)
  st_queries.csv
  query_count.txt

Suites:
  uniform_dense20  — 20 windows spread across space/time (farthest-point sampling)
  skewed_dense20   — 20 highest-density windows (greedy, low overlap)

Usage:
  python tools/generate_tdrive_dense20_st_windows.py
  python tools/generate_tdrive_dense20_st_windows.py --min-segments 120 --queries-per-suite 20
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_META = PROJECT / "data" / "processed" / "tdrive" / "segments_meta.csv"
DEFAULT_POINTS = PROJECT / "data" / "processed" / "tdrive" / "segments_points.csv"
DEFAULT_OUT = PROJECT / "data" / "processed" / "tman_tdrive" / "tdrive_query_st"


@dataclass
class Bounds:
    lon_min: float
    lon_max: float
    lat_min: float
    lat_max: float
    t_min: int
    t_max: int


@dataclass
class WindowSpec:
    label: str
    suite: str
    t0: int
    t1: int
    x0: float
    x1: float
    y0: float
    y1: float
    seed_count: int = 0


@dataclass
class WindowTruth:
    spec: WindowSpec
    segment_ids: set[str] = field(default_factory=set)
    point_count: int = 0


def load_bounds(meta_csv: Path) -> Bounds:
    lon_min = lat_min = 1e18
    lon_max = lat_max = -1e18
    t_min = 1 << 62
    t_max = 0
    with meta_csv.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            lon_min = min(lon_min, float(row["lon_min"]))
            lon_max = max(lon_max, float(row["lon_max"]))
            lat_min = min(lat_min, float(row["lat_min"]))
            lat_max = max(lat_max, float(row["lat_max"]))
            t_min = min(t_min, int(row["t_start_unix"]))
            t_max = max(t_max, int(row["t_end_unix"]))
    return Bounds(lon_min, lon_max, lat_min, lat_max, t_min, t_max)


def bucket_centers(
    tb: int, i: int, j: int, bounds: Bounds, time_bin: int, cell_deg: float
) -> tuple[float, float, float]:
    cx = bounds.lon_min + (i + 0.5) * cell_deg
    cy = bounds.lat_min + (j + 0.5) * cell_deg
    ct = (tb + 0.5) * time_bin
    return cx, cy, ct


def dist3(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    # normalize: 1 deg ~ 111km, time in hours
    dx = (a[0] - b[0]) * 111.0 * math.cos(math.radians((a[1] + b[1]) / 2))
    dy = (a[1] - b[1]) * 111.0
    dt = (a[2] - b[2]) / 3600.0
    return math.sqrt(dx * dx + dy * dy + dt * dt)


def bbox_overlap(a: WindowSpec, b: WindowSpec) -> bool:
    if a.t1 < b.t0 or b.t1 < a.t0:
        return False
    if a.x1 < b.x0 or b.x1 < a.x0:
        return False
    if a.y1 < b.y0 or b.y1 < a.y0:
        return False
    return True


def build_bucket_index(
    points_csv: Path,
    bounds: Bounds,
    *,
    time_bin: int,
    cell_deg: float,
    min_segments: int,
) -> list[tuple[tuple[int, int, int], set[str]]]:
    buckets: dict[tuple[int, int, int], set[str]] = defaultdict(set)
    n_rows = 0
    with points_csv.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            n_rows += 1
            sid = row["segment_id"]
            t = int(float(row["unix_time_s"]))
            lon = float(row["lon"])
            lat = float(row["lat"])
            tb = t // time_bin
            i = int((lon - bounds.lon_min) / cell_deg)
            j = int((lat - bounds.lat_min) / cell_deg)
            buckets[(tb, i, j)].add(sid)
            if n_rows % 1_000_000 == 0:
                print(f"  bucket scan: {n_rows} point rows...", flush=True)
    print(f"  bucket scan done: {n_rows} rows, {len(buckets)} buckets")
    cands = [(k, v) for k, v in buckets.items() if len(v) >= min_segments]
    cands.sort(key=lambda kv: -len(kv[1]))
    print(f"  candidates (>={min_segments} segs): {len(cands)}")
    return cands


def seed_to_window(
    tb: int,
    i: int,
    j: int,
    bounds: Bounds,
    *,
    time_bin: int,
    cell_deg: float,
    label: str,
    suite: str,
    seed_count: int,
) -> WindowSpec:
    t0 = tb * time_bin
    t1 = min(bounds.t_max, (tb + 1) * time_bin - 1)
    x0 = bounds.lon_min + i * cell_deg
    x1 = min(bounds.lon_max, bounds.lon_min + (i + 1) * cell_deg)
    y0 = bounds.lat_min + j * cell_deg
    y1 = min(bounds.lat_max, bounds.lat_min + (j + 1) * cell_deg)
    return WindowSpec(label, suite, t0, t1, x0, x1, y0, y1, seed_count)


def expand_until_min(
    spec: WindowSpec,
    bounds: Bounds,
    *,
    min_segments: int,
    max_time_span: int,
    max_spatial_span: float,
) -> WindowSpec:
    """Expand time then spatial until bucket-sized window likely hits min_segments."""
    cur = WindowSpec(
        spec.label,
        spec.suite,
        spec.t0,
        spec.t1,
        spec.x0,
        spec.x1,
        spec.y0,
        spec.y1,
        spec.seed_count,
    )
    for _ in range(24):
        if cur.t1 - cur.t0 + 1 >= max_time_span and max(
            cur.x1 - cur.x0, cur.y1 - cur.y0
        ) >= max_spatial_span:
            break
        # alternate time expand / spatial expand
        if cur.t1 - cur.t0 + 1 < max_time_span:
            pad = min(max(900, (cur.t1 - cur.t0 + 1) // 4), max_time_span)
            cur.t0 = max(bounds.t_min, cur.t0 - pad // 2)
            cur.t1 = min(bounds.t_max, cur.t1 + pad // 2)
        span_x = cur.x1 - cur.x0
        span_y = cur.y1 - cur.y0
        if max(span_x, span_y) < max_spatial_span:
            dx = span_x * 0.15
            dy = span_y * 0.15
            cur.x0 = max(bounds.lon_min, cur.x0 - dx)
            cur.x1 = min(bounds.lon_max, cur.x1 + dx)
            cur.y0 = max(bounds.lat_min, cur.y0 - dy)
            cur.y1 = min(bounds.lat_max, cur.y1 + dy)
    return cur


def compute_truth_for_windows(
    points_csv: Path, specs: list[WindowSpec]
) -> list[WindowTruth]:
    """One pass: point-truth segment sets for all windows."""
    truths = [WindowTruth(spec=s) for s in specs]
    seen: list[set[str]] = [set() for _ in specs]
    n_rows = 0
    with points_csv.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            n_rows += 1
            sid = row["segment_id"]
            t = int(float(row["unix_time_s"]))
            lon = float(row["lon"])
            lat = float(row["lat"])
            for i, w in enumerate(specs):
                if sid in seen[i]:
                    continue
                if t < w.t0 or t > w.t1:
                    continue
                if lon < w.x0 or lon > w.x1 or lat < w.y0 or lat > w.y1:
                    continue
                seen[i].add(sid)
                truths[i].segment_ids.add(sid)
                truths[i].point_count += 1
            if n_rows % 1_000_000 == 0:
                print(f"  truth scan: {n_rows} point rows...", flush=True)
    print(f"  truth scan done: {n_rows} rows")
    return truths


def pick_uniform(
    cands: list[tuple[tuple[int, int, int], set[str]]],
    bounds: Bounds,
    *,
    time_bin: int,
    cell_deg: float,
    n: int,
    suite: str,
) -> list[WindowSpec]:
    items = []
    for (tb, i, j), segs in cands:
        cx, cy, ct = bucket_centers(tb, i, j, bounds, time_bin, cell_deg)
        items.append(((tb, i, j), len(segs), (cx, cy, ct)))
    if len(items) < n:
        raise SystemExit(f"uniform: need {n} candidates, have {len(items)}")

    # farthest-point sampling
    items.sort(key=lambda x: -x[1])
    pool = items[: max(n * 8, 200)]
    selected: list[tuple[int, int, int]] = [pool[0][0]]
    centers = {pool[0][0]: pool[0][2]}

    while len(selected) < n:
        best_key = None
        best_score = -1.0
        for key, cnt, cen in pool:
            if key in selected:
                continue
            dmin = min(dist3(cen, centers[k]) for k in selected)
            score = dmin + 0.001 * cnt
            if score > best_score:
                best_score = score
                best_key = key
        if best_key is None:
            break
        selected.append(best_key)
        centers[best_key] = next(c for k, _, c in pool if k == best_key)

    out: list[WindowSpec] = []
    for qi, key in enumerate(selected):
        tb, i, j = key
        cnt = next(c for k, c, _ in items if k == key)
        out.append(
            seed_to_window(
                tb,
                i,
                j,
                bounds,
                time_bin=time_bin,
                cell_deg=cell_deg,
                label=f"dense20_u{qi:02d}",
                suite=suite,
                seed_count=cnt,
            )
        )
    return out


def pick_skewed(
    cands: list[tuple[tuple[int, int, int], set[str]]],
    bounds: Bounds,
    *,
    time_bin: int,
    cell_deg: float,
    n: int,
    suite: str,
) -> list[WindowSpec]:
    out: list[WindowSpec] = []
    for qi, ((tb, i, j), segs) in enumerate(cands):
        if len(out) >= n:
            break
        spec = seed_to_window(
            tb,
            i,
            j,
            bounds,
            time_bin=time_bin,
            cell_deg=cell_deg,
            label=f"dense20_s{qi:02d}",
            suite=suite,
            seed_count=len(segs),
        )
        if any(bbox_overlap(spec, s) for s in out):
            continue
        out.append(spec)
    if len(out) < n:
        raise SystemExit(f"skewed: only placed {len(out)}/{n} non-overlapping windows")
    return out


def trajectory_id(segment_id: str) -> str:
    # tdrive:3644:3644_1 -> 3644
    parts = segment_id.split(":")
    return parts[1] if len(parts) >= 2 else segment_id


def write_suite(out_dir: Path, truths: list[WindowTruth]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    suite = truths[0].spec.suite

    manifest: list[dict] = []
    st_rows: list[dict] = []
    qparts: list[str] = []

    for qid, wt in enumerate(truths):
        w = wt.spec
        seg_ids = sorted(wt.segment_ids)
        traj_ids = sorted({trajectory_id(s) for s in seg_ids})
        t_min_ms = w.t0 * 1000
        t_max_ms = w.t1 * 1000
        qparts.append(
            f"{w.x0:.6f},{w.y0:.6f},{w.x1:.6f},{w.y1:.6f},{t_min_ms},{t_max_ms}"
        )
        manifest.append(
            {
                "id": qid,
                "suite": suite,
                "label": w.label,
                "xmin": w.x0,
                "ymin": w.y0,
                "xmax": w.x1,
                "ymax": w.y1,
                "t_min_unix": w.t0,
                "t_max_unix": w.t1,
                "t_min_ms": t_min_ms,
                "t_max_ms": t_max_ms,
                "duration_s": w.t1 - w.t0 + 1,
                "st_segments_with_point": len(seg_ids),
                "st_points_in_window": wt.point_count,
                "seed_bucket_segments": w.seed_count,
                "segment_ids": seg_ids,
                "trajectory_ids": traj_ids,
                "trajectory_count": len(traj_ids),
            }
        )
        st_rows.append(
            {
                "query_id": qid,
                "label": w.label,
                "xmin": f"{w.x0:.6f}",
                "ymin": f"{w.y0:.6f}",
                "xmax": f"{w.x1:.6f}",
                "ymax": f"{w.y1:.6f}",
                "t_min_unix": w.t0,
                "t_max_unix": w.t1,
                "st_segments_with_point": len(seg_ids),
            }
        )

    (out_dir / "query_conditions_st.txt").write_text(";".join(qparts) + "\n", encoding="utf-8")
    (out_dir / "queries_manifest_st.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    (out_dir / "query_count.txt").write_text(f"{len(truths)}\n", encoding="utf-8")
    with (out_dir / "st_queries.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(st_rows[0].keys()))
        w.writeheader()
        w.writerows(st_rows)

    counts = [len(wt.segment_ids) for wt in truths]
    print(
        f"  {suite}: queries={len(truths)} "
        f"segments=[{min(counts)},{max(counts)}] avg={sum(counts)/len(counts):.1f}"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--meta-csv", type=Path, default=DEFAULT_META)
    ap.add_argument("--points-csv", type=Path, default=DEFAULT_POINTS)
    ap.add_argument("--out-root", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--queries-per-suite", type=int, default=20)
    ap.add_argument("--min-segments", type=int, default=100)
    ap.add_argument("--time-bin-sec", type=int, default=3600)
    ap.add_argument("--cell-deg", type=float, default=0.05)
    ap.add_argument("--max-time-span-sec", type=int, default=14400)
    ap.add_argument("--max-spatial-span-deg", type=float, default=0.12)
    args = ap.parse_args()

    bounds = load_bounds(args.meta_csv)
    print(f"bounds lon=[{bounds.lon_min:.4f},{bounds.lon_max:.4f}] "
          f"lat=[{bounds.lat_min:.4f},{bounds.lat_max:.4f}] "
          f"t=[{bounds.t_min},{bounds.t_max}]")

    cands = build_bucket_index(
        args.points_csv,
        bounds,
        time_bin=args.time_bin_sec,
        cell_deg=args.cell_deg,
        min_segments=args.min_segments,
    )
    if len(cands) < args.queries_per_suite:
        raise SystemExit(f"not enough dense buckets: {len(cands)} < {args.queries_per_suite}")

    suite_specs: list[tuple[str, list[WindowSpec]]] = [
        (
            "uniform_dense20",
            pick_uniform(
                cands,
                bounds,
                time_bin=args.time_bin_sec,
                cell_deg=args.cell_deg,
                n=args.queries_per_suite,
                suite="uniform_dense20",
            ),
        ),
        (
            "skewed_dense20",
            pick_skewed(
                cands,
                bounds,
                time_bin=args.time_bin_sec,
                cell_deg=args.cell_deg,
                n=args.queries_per_suite,
                suite="skewed_dense20",
            ),
        ),
    ]

    summary: dict = {"suites": {}, "min_segments": args.min_segments}
    for suite_name, specs in suite_specs:
        print(f"\n=== {suite_name} ===")
        expanded = []
        for s in specs:
            if s.seed_count >= args.min_segments:
                expanded.append(s)
            else:
                expanded.append(
                    expand_until_min(
                        s,
                        bounds,
                        min_segments=args.min_segments,
                        max_time_span=args.max_time_span_sec,
                        max_spatial_span=args.max_spatial_span_deg,
                    )
                )
        truths = compute_truth_for_windows(args.points_csv, expanded)
        bad = [t for t in truths if len(t.segment_ids) < args.min_segments]
        if bad:
            details = ", ".join(
                f"{t.spec.label}={len(t.segment_ids)}" for t in bad[:5]
            )
            raise SystemExit(
                f"{suite_name}: {len(bad)} windows below min_segments ({details})"
            )
        out_dir = args.out_root / suite_name
        write_suite(out_dir, truths)
        summary["suites"][suite_name] = {
            "queries": len(truths),
            "segment_min": min(len(t.segment_ids) for t in truths),
            "segment_max": max(len(t.segment_ids) for t in truths),
            "segment_avg": sum(len(t.segment_ids) for t in truths) / len(truths),
            "out_dir": str(out_dir),
        }

    summary_path = args.out_root / "dense20_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"\nWrote summary -> {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
