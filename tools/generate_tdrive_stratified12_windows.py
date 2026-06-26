#!/usr/bin/env python3
"""Generate stratified 12 query windows for TDrive (4 narrow + 4 medium + 4 wide).

Outputs:
  1) ST windows CSV (time + MBR) with per-window segment counts
  2) Spatial-only CSV (same MBR; TMin/TMax kept as ST cross-reference only)
  3) JSON ref counts for result validation

STSegments = point-truth: distinct segment_id with >=1 point in [TMin,TMax] x MBR
(from segments_points.csv). Window placement still uses MBR density on meta.
STSegmentsMBR (optional column) = legacy segment-level MBR intersect count.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_wuxi_stratified12_aligned_windows import (  # noqa: E402
    bbox_of_intersecting,
    count_in,
    expand_spatial,
    find_densest_time_slice,
    grid_spatial_for_time,
    load_segments,
    segment_mid,
    t_max_global,
    t_min_global,
)


def spatial_count(segs, x0, x1, y0, y1) -> int:
    return sum(
        1 for s in segs if not (s.lx > x1 or s.hx < x0 or s.ly > y1 or s.hy < y0)
    )


def tier_of_label(label: str) -> str:
    if "narrow" in label:
        return "narrow"
    if "medium" in label:
        return "medium"
    if "wide" in label:
        return "wide"
    raise ValueError(label)


def max_span(x0: float, x1: float, y0: float, y1: float) -> float:
    return max(x1 - x0, y1 - y0)


def apply_point_truth_st_segments(
    points_csv: Path, rows: list[dict], *, show_progress: bool = True
) -> None:
    """Set STSegments from segments_points (>=1 point per segment in window)."""
    specs: list[tuple[int, int, float, float, float, float]] = []
    for r in rows:
        specs.append(
            (
                int(r["TMin"]),
                int(r["TMax"]),
                float(r["XMin"]),
                float(r["XMax"]),
                float(r["YMin"]),
                float(r["YMax"]),
            )
        )
    seen: list[set[str]] = [set() for _ in rows]
    counts = [0] * len(rows)
    n_rows = 0
    with points_csv.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            n_rows += 1
            sid = row["segment_id"]
            t = int(float(row["unix_time_s"]))
            lon = float(row["lon"])
            lat = float(row["lat"])
            for i, (t0, t1, x0, x1, y0, y1) in enumerate(specs):
                if sid in seen[i]:
                    continue
                if t0 <= t <= t1 and x0 <= lon <= x1 and y0 <= lat <= y1:
                    seen[i].add(sid)
                    counts[i] += 1
            if show_progress and n_rows % 500_000 == 0 and n_rows > 0:
                print(f"  point-truth scan: {n_rows} point rows...", flush=True)
    for r, c in zip(rows, counts):
        r["STSegments"] = str(c)


def validate_tier_spans(rows: list[dict]) -> None:
    by_tier: dict[str, list[float]] = {"narrow": [], "medium": [], "wide": []}
    for r in rows:
        t = tier_of_label(r["Label"])
        by_tier[t].append(
            max_span(float(r["XMin"]), float(r["XMax"]), float(r["YMin"]), float(r["YMax"]))
        )
    n_max = max(by_tier["narrow"])
    m_min, m_max = min(by_tier["medium"]), max(by_tier["medium"])
    w_min = min(by_tier["wide"])
    if not (n_max < m_min):
        raise SystemExit(f"tier span check failed: narrow max {n_max:.4f} >= medium min {m_min:.4f}")
    if not (m_max < w_min):
        raise SystemExit(f"tier span check failed: medium max {m_max:.4f} >= wide min {w_min:.4f}")


def generate_windows(
    segs,
    *,
    min_segments: int,
    narrow_seconds: int,
    medium_seconds: int,
    wide_seconds: int,
    time_search_step: int,
    narrow_cell_deg: float,
    narrow_max_lon_span: float,
    narrow_max_lat_span: float,
    medium_cell_deg: float,
    medium_max_lon_span: float,
    medium_max_lat_span: float,
) -> list[dict]:
    gx0 = min(s.lx for s in segs)
    gx1 = max(s.hx for s in segs)
    gy0 = min(s.ly for s in segs)
    gy1 = max(s.hy for s in segs)
    t_min = t_min_global(segs)
    t_max = t_max_global(segs)

    segs_by_mid = sorted(segs, key=segment_mid)
    n = len(segs_by_mid)
    qsize = max(1, n // 4)

    rows_out: list[dict] = []
    narrow_cells: set[tuple[int, int]] = set()
    medium_cells: set[tuple[int, int]] = set()

    dn = narrow_seconds
    for qi in range(4):
        lo = qi * qsize
        hi = (qi + 1) * qsize if qi < 3 else n
        q_lo = segment_mid(segs_by_mid[lo])
        q_hi = segment_mid(segs_by_mid[hi - 1])
        t_lo = max(t_min, q_lo - dn)
        t_hi = min(t_max, q_hi + dn)
        if t_hi - t_lo < dn:
            t_lo, t_hi = t_min, min(t_max, t_min + dn)
        t0 = find_densest_time_slice(
            segs, t_lo, t_hi, dn, gx0, gx1, gy0, gy1, time_search_step
        )
        t0 = max(t_min, min(t0, t_max - dn))
        t1 = t0 + dn
        q_mid_lo = segment_mid(segs_by_mid[lo])
        q_mid_hi = segment_mid(segs_by_mid[hi - 1])
        pool = [s for s in segs if q_mid_lo <= segment_mid(s) <= q_mid_hi]
        if len(pool) < max(min_segments, 100):
            pool = segs
        step = max(max((gx1 - gx0), (gy1 - gy0)) * 0.004, 0.008)
        x0, x1, y0, y1, st_c, ck = grid_spatial_for_time(
            segs,
            t0,
            t1,
            gx0,
            gx1,
            gy0,
            gy1,
            narrow_cell_deg,
            min_segments,
            narrow_max_lon_span,
            narrow_max_lat_span,
            step,
            step,
            segment_pool=pool,
            exclude_cells=narrow_cells,
        )
        if ck != (-1, -1):
            narrow_cells.add(ck)
        sp_c = spatial_count(segs, x0, x1, y0, y1)
        rows_out.append(
            {
                "Label": f"td_strat_narrow_{qi + 1:02d}",
                "TMin": str(t0),
                "TMax": str(t1),
                "XMin": f"{x0:.6f}",
                "XMax": f"{x1:.6f}",
                "YMin": f"{y0:.6f}",
                "YMax": f"{y1:.6f}",
                "STSegmentsMBR": str(st_c),
                "STSegments": "0",
                "SpatialSegments": str(sp_c),
                "Note": (
                    f"tier=narrow {dn}s cell={narrow_cell_deg}deg "
                    f"max_span~{narrow_max_lon_span}x{narrow_max_lat_span}deg q{qi + 1}; "
                    f"st_count=point_truth"
                ),
            }
        )

    dm = medium_seconds
    for qi in range(4):
        lo = qi * qsize
        hi = (qi + 1) * qsize if qi < 3 else n
        q_lo = segment_mid(segs_by_mid[lo])
        q_hi = segment_mid(segs_by_mid[hi - 1])
        t_lo = max(t_min, q_lo - dm // 2)
        t_hi = min(t_max, q_hi + dm // 2)
        if t_hi - t_lo < dm:
            t_lo, t_hi = t_min, min(t_max, t_min + dm)
        t0 = find_densest_time_slice(
            segs, t_lo, t_hi, dm, gx0, gx1, gy0, gy1, time_search_step
        )
        t0 = max(t_min, min(t0, t_max - dm))
        t1 = t0 + dm
        q_mid_lo = segment_mid(segs_by_mid[lo])
        q_mid_hi = segment_mid(segs_by_mid[hi - 1])
        pool = [s for s in segs if q_mid_lo <= segment_mid(s) <= q_mid_hi]
        if len(pool) < max(min_segments, 100):
            pool = segs
        step = max(max((gx1 - gx0), (gy1 - gy0)) * 0.005, 0.012)
        x0, x1, y0, y1, st_c, ck = grid_spatial_for_time(
            segs,
            t0,
            t1,
            gx0,
            gx1,
            gy0,
            gy1,
            medium_cell_deg,
            min_segments,
            medium_max_lon_span,
            medium_max_lat_span,
            step,
            step,
            segment_pool=pool,
            exclude_cells=medium_cells,
        )
        if ck != (-1, -1):
            medium_cells.add(ck)
        sp_c = spatial_count(segs, x0, x1, y0, y1)
        rows_out.append(
            {
                "Label": f"td_strat_medium_{qi + 1:02d}",
                "TMin": str(t0),
                "TMax": str(t1),
                "XMin": f"{x0:.6f}",
                "XMax": f"{x1:.6f}",
                "YMin": f"{y0:.6f}",
                "YMax": f"{y1:.6f}",
                "STSegmentsMBR": str(st_c),
                "STSegments": "0",
                "SpatialSegments": str(sp_c),
                "Note": (
                    f"tier=medium {dm}s (~{dm / 3600:.2f}h) cell={medium_cell_deg}deg "
                    f"max_span~{medium_max_lon_span}x{medium_max_lat_span}deg q{qi + 1}; "
                    f"st_count=point_truth"
                ),
            }
        )

    dw = wide_seconds
    span = t_max - t_min - dw
    if span < 0:
        starts = [t_min] * 4
    else:
        step = max(span // 4, 1)
        starts = [max(t_min, min(t_min + i * step, t_max - dw)) for i in range(4)]

    for wi, t0 in enumerate(starts):
        t1 = t0 + dw
        bb = bbox_of_intersecting(segs, t0, t1, gx0, gx1, gy0, gy1)
        if bb is None:
            raise SystemExit(f"wide {wi + 1}: empty bbox")
        x0, x1, y0, y1 = bb
        pad = max((x1 - x0), (y1 - y0)) * 0.02 + 1e-4
        x0, x1, y0, y1 = x0 - pad, x1 + pad, y0 - pad, y1 + pad
        x0, x1, y0, y1, st_c = expand_spatial(
            segs, t0, t1, x0, x1, y0, y1, min_segments, gx0, gx1, gy0, gy1
        )
        sp_c = spatial_count(segs, x0, x1, y0, y1)
        rows_out.append(
            {
                "Label": f"td_strat_wide_{wi + 1:02d}",
                "TMin": str(t0),
                "TMax": str(t1),
                "XMin": f"{x0:.6f}",
                "XMax": f"{x1:.6f}",
                "YMin": f"{y0:.6f}",
                "YMax": f"{y1:.6f}",
                "STSegmentsMBR": str(st_c),
                "STSegments": "0",
                "SpatialSegments": str(sp_c),
                "Note": (
                    f"tier=wide {dw}s (~{dw / 3600:.1f}h) slice {wi + 1}/4; "
                    f"expanded for coverage; st_count=point_truth"
                ),
            }
        )

    if len(rows_out) != 12:
        raise SystemExit(f"expected 12 windows, got {len(rows_out)}")
    for t, n in (
        ("narrow", sum(1 for r in rows_out if "narrow" in r["Label"])),
        ("medium", sum(1 for r in rows_out if "medium" in r["Label"])),
        ("wide", sum(1 for r in rows_out if "wide" in r["Label"])),
    ):
        if n != 4:
            raise SystemExit(f"expected 4 {t} windows, got {n}")
    validate_tier_spans(rows_out)
    low = [r for r in rows_out if int(r["STSegmentsMBR"]) < min_segments]
    if low:
        raise SystemExit(
            "could not reach min_segments (MBR placement) for: "
            + ", ".join(f"{r['Label']}={r['STSegmentsMBR']}" for r in low)
        )
    return rows_out


def write_csv(path: Path, rows: list[dict], fieldnames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--meta",
        type=Path,
        default=Path("data/processed/tdrive/segments_meta.csv"),
    )
    ap.add_argument(
        "--points",
        type=Path,
        default=Path("data/processed/tdrive/segments_points.csv"),
    )
    ap.add_argument(
        "--out-st",
        type=Path,
        default=Path("tools/st_validity_experiment_windows_tdrive_stratified12_n4m4w4.csv"),
    )
    ap.add_argument(
        "--out-spatial",
        type=Path,
        default=Path("tools/st_validity_experiment_windows_tdrive_stratified12_spatial_n4m4w4.csv"),
    )
    ap.add_argument(
        "--out-json",
        type=Path,
        default=Path("data/processed/tdrive/window_ref_counts.json"),
    )
    ap.add_argument(
        "--out-sweep-json",
        type=Path,
        default=Path("data/processed/tdrive/window_ref_counts_sweep.json"),
    )
    ap.add_argument(
        "--recount-only",
        action="store_true",
        help="Keep existing --out-st windows; only recompute STSegments from --points",
    )
    ap.add_argument("--min-segments", type=int, default=50)
    ap.add_argument("--narrow-seconds", type=int, default=3600)
    ap.add_argument("--medium-seconds", type=int, default=13320)
    ap.add_argument("--wide-seconds", type=int, default=43200)
    ap.add_argument("--time-search-step", type=int, default=600)
    ap.add_argument("--narrow-cell-deg", type=float, default=0.06)
    ap.add_argument("--narrow-max-lon-span", type=float, default=0.55)
    ap.add_argument("--narrow-max-lat-span", type=float, default=0.55)
    ap.add_argument("--medium-cell-deg", type=float, default=0.12)
    ap.add_argument("--medium-max-lon-span", type=float, default=1.2)
    ap.add_argument("--medium-max-lat-span", type=float, default=1.2)
    args = ap.parse_args()

    if args.recount_only:
        if not args.out_st.is_file():
            raise SystemExit(f"--recount-only needs existing windows: {args.out_st}")
        with args.out_st.open(newline="", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
        if len(rows) != 12:
            raise SystemExit(f"expected 12 windows in {args.out_st}, got {len(rows)}")
        segs = load_segments(args.meta)
        for r in rows:
            if "STSegmentsMBR" not in r or not str(r.get("STSegmentsMBR", "")).strip():
                r["STSegmentsMBR"] = str(
                    count_in(
                        segs,
                        int(r["TMin"]),
                        int(r["TMax"]),
                        float(r["XMin"]),
                        float(r["XMax"]),
                        float(r["YMin"]),
                        float(r["YMax"]),
                    )
                )
    else:
        segs = load_segments(args.meta)
        if not segs:
            raise SystemExit(f"no segments in {args.meta}")
        rows = generate_windows(
        segs,
        min_segments=args.min_segments,
        narrow_seconds=args.narrow_seconds,
        medium_seconds=args.medium_seconds,
        wide_seconds=args.wide_seconds,
        time_search_step=args.time_search_step,
        narrow_cell_deg=args.narrow_cell_deg,
        narrow_max_lon_span=args.narrow_max_lon_span,
        narrow_max_lat_span=args.narrow_max_lat_span,
        medium_cell_deg=args.medium_cell_deg,
        medium_max_lon_span=args.medium_max_lon_span,
        medium_max_lat_span=args.medium_max_lat_span,
        )

    if not args.points.is_file():
        raise SystemExit(f"points csv not found: {args.points}")
    print(f"Point-truth STSegments from {args.points} ...")
    apply_point_truth_st_segments(args.points, rows)
    low = [r for r in rows if int(r["STSegments"]) < args.min_segments]
    if low:
        raise SystemExit(
            "point-truth count below min_segments for: "
            + ", ".join(
                f"{r['Label']}={r['STSegments']} (MBR was {r['STSegmentsMBR']})" for r in low
            )
        )

    st_fields = [
        "Label",
        "TMin",
        "TMax",
        "XMin",
        "XMax",
        "YMin",
        "YMax",
        "STSegments",
        "STSegmentsMBR",
        "SpatialSegments",
        "Note",
    ]
    write_csv(args.out_st, rows, st_fields)

    spatial_rows: list[dict] = []
    for r in rows:
        lon_span = float(r["XMax"]) - float(r["XMin"])
        lat_span = float(r["YMax"]) - float(r["YMin"])
        tier = tier_of_label(r["Label"])
        spatial_rows.append(
            {
                "Label": r["Label"],
                "XMin": r["XMin"],
                "XMax": r["XMax"],
                "YMin": r["YMin"],
                "YMax": r["YMax"],
                "TMin": r["TMin"],
                "TMax": r["TMax"],
                "SpatialSegments": r["SpatialSegments"],
                "Note": (
                    f"tier={tier}; spatial_only; spatial_segments={r['SpatialSegments']}; "
                    f"span~{lon_span:.3f}x{lat_span:.3f}deg; "
                    f"ST_ref_T=[{r['TMin']},{r['TMax']}] (spatial query ignores time)"
                ),
            }
        )
    spatial_fields = [
        "Label",
        "XMin",
        "XMax",
        "YMin",
        "YMax",
        "TMin",
        "TMax",
        "SpatialSegments",
        "Note",
    ]
    write_csv(args.out_spatial, spatial_rows, spatial_fields)

    ref = {
        "meta_csv": str(args.meta.resolve()),
        "points_csv": str(args.points.resolve()),
        "st_count_method": "point_truth",
        "st_windows_csv": str(args.out_st.resolve()),
        "spatial_windows_csv": str(args.out_spatial.resolve()),
        "windows": {
            r["Label"]: {
                "ref_st_segments": int(r["STSegments"]),
                "ref_st_segments_mbr": int(r["STSegmentsMBR"]),
                "ref_spatial_segments": int(r["SpatialSegments"]),
                "t_min": int(r["TMin"]),
                "t_max": int(r["TMax"]),
                "x_min": float(r["XMin"]),
                "x_max": float(r["XMax"]),
                "y_min": float(r["YMin"]),
                "y_max": float(r["YMax"]),
            }
            for r in rows
        },
    }
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(ref, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    sweep = {
        r["Label"]: {
            "ref_meta_st_segments": int(r["STSegments"]),
            "ref_meta_st_segments_mbr": int(r["STSegmentsMBR"]),
            "ref_spatial_segments": int(r["SpatialSegments"]),
        }
        for r in rows
    }
    args.out_sweep_json.write_text(
        json.dumps(sweep, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    print(f"Wrote ST windows -> {args.out_st}")
    print(f"Wrote spatial windows -> {args.out_spatial}")
    print(f"Wrote ref counts -> {args.out_json}")
    print(f"Wrote sweep ref -> {args.out_sweep_json}")
    for r in rows:
        print(
            f"  {r['Label']:22s} ST={r['STSegments']:>5s} (MBR={r['STSegmentsMBR']:>5s})  "
            f"spatial={r['SpatialSegments']:>5s}  "
            f"T=[{r['TMin']},{r['TMax']}]  "
            f"span={float(r['XMax']) - float(r['XMin']):.3f}x"
            f"{float(r['YMax']) - float(r['YMin']):.3f}deg"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
