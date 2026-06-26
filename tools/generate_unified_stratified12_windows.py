#!/usr/bin/env python3
"""
Generate stratified-12 ST query windows for the unified (4-dataset merged) dataset.

Tiers:
  - narrow:  1 hour time window, small spatial (~0.5° lon/lat)
  - medium: ~3.7 hour time window, medium spatial (~1.2° lon/lat)
  - wide:   ~12 hour time window, spatial = full bbox of intersecting in time slice

Strategy:
  1. Sort segments by temporal midpoint, split into 4 quartiles
  2. For each quartile, find densest time slice → grid-search spatial cell
  3. Expand spatial cell to reach min_segments (default 50)
  4. Point-truth count via streaming pass over the 8.7GB points CSV

Output:
  - ST windows CSV (time + MBR + STSegments point-truth count)
  - Spatial-only CSV (same MBR)
  - JSON reference counts for query validation
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_META   = PROJECT / "data" / "processed" / "unified" / "segments_meta.csv"
DEFAULT_POINTS = PROJECT / "data" / "processed" / "unified" / "segments_points.csv"
DEFAULT_OUTDIR = PROJECT / "data" / "processed" / "unified"

# ── Segment meta (lightweight) ─────────────────────────────────────────────

class Seg:
    __slots__ = ("seg_id", "dataset", "t0", "t1", "lx", "hx", "ly", "hy", "npts")
    def __init__(self, row: dict[str, str]):
        self.seg_id  = row["segment_id"]
        self.dataset = row.get("dataset", "?")
        self.t0 = int(row["t_start_unix"])
        self.t1 = int(row["t_end_unix"])
        self.lx = float(row["lon_min"])
        self.hx = float(row["lon_max"])
        self.ly = float(row["lat_min"])
        self.hy = float(row["lat_max"])
        self.npts = int(row["point_count"])

    @property
    def t_mid(self) -> float:
        return (self.t0 + self.t1) * 0.5


def load_segments(meta_path: Path) -> list[Seg]:
    segs = []
    with meta_path.open("r", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            segs.append(Seg(row))
    print(f"Loaded {len(segs):,} segments from {meta_path}")
    return segs


# ── spatial grid helpers ───────────────────────────────────────────────────

def time_filter(segs: list[Seg], t0: int, t1: int) -> list[Seg]:
    """Segments whose [t_start, t_end] overlaps [t0, t1]."""
    return [s for s in segs if s.t0 <= t1 and s.t1 >= t0]


def count_in_bbox(segs: list[Seg], x0: float, x1: float, y0: float, y1: float) -> int:
    """Count segments whose MBR intersects the query bbox."""
    return sum(1 for s in segs if s.lx <= x1 and s.hx >= x0 and s.ly <= y1 and s.hy >= y0)


def count_in_st(segs: list[Seg], t0: int, t1: int,
                x0: float, x1: float, y0: float, y1: float) -> int:
    """Count segments that overlap in both time and space."""
    return sum(1 for s in segs
               if s.t0 <= t1 and s.t1 >= t0
               and s.lx <= x1 and s.hx >= x0
               and s.ly <= y1 and s.hy >= y0)


def grid_spatial_dense_cell(segs_pool: list[Seg],
                            cell_deg: float,
                            gx0: float, gx1: float,
                            gy0: float, gy1: float) -> tuple[float, float, float, float, int]:
    """Find the densest spatial cell of size cell_deg x cell_deg in the pool.
    Only grids cells that intersect at least one segment's MBR (sparse global grid)."""
    best_x0, best_x1 = gx0, min(gx1, gx0 + cell_deg)
    best_y0, best_y1 = gy0, min(gy1, gy0 + cell_deg)
    best_cnt = -1

    # Approach: for each segment in pool, snap its center to the grid and score that cell
    # This is much cheaper than exhaustively gridding the globe
    scored: dict[tuple[int, int], int] = {}
    for s in segs_pool:
        cx = (s.lx + s.hx) * 0.5
        cy = (s.ly + s.hy) * 0.5
        gx = int((cx - gx0) / cell_deg)
        gy = int((cy - gy0) / cell_deg)
        key = (gx, gy)
        if key not in scored:
            scored[key] = 0
        scored[key] += 1

    if not scored:
        return best_x0, best_x1, best_y0, best_y1, 0

    best_key = max(scored, key=scored.__getitem__)
    gx, gy = best_key
    best_x0 = gx0 + gx * cell_deg
    best_x1 = best_x0 + cell_deg
    best_y0 = gy0 + gy * cell_deg
    best_y1 = best_y0 + cell_deg
    best_cnt = scored[best_key]

    # Actual count via exact bbox intersection
    best_cnt = count_in_bbox(segs_pool, best_x0, best_x1, best_y0, best_y1)
    return best_x0, best_x1, best_y0, best_y1, best_cnt


def expand_spatial(segs: list[Seg], t0: int, t1: int,
                   x0: float, x1: float, y0: float, y1: float,
                   min_segments: int,
                   gx0: float, gx1: float, gy0: float, gy1: float,
                   step_frac: float = 0.05) -> tuple[float, float, float, float, int]:
    """Expand bbox until it covers ≥min_segments in the time window."""
    cnt = count_in_st(segs, t0, t1, x0, x1, y0, y1)
    if cnt >= min_segments:
        return x0, x1, y0, y1, cnt

    dx = (x1 - x0) * step_frac + 1e-6
    dy = (y1 - y0) * step_frac + 1e-6
    max_iter = 200
    for _ in range(max_iter):
        x0 = max(gx0, x0 - dx)
        x1 = min(gx1, x1 + dx)
        y0 = max(gy0, y0 - dy)
        y1 = min(gy1, y1 + dy)
        cnt = count_in_st(segs, t0, t1, x0, x1, y0, y1)
        if cnt >= min_segments:
            return x0, x1, y0, y1, cnt
        if (x0 <= gx0 and x1 >= gx1 and y0 <= gy0 and y1 >= gy1):
            break
    return x0, x1, y0, y1, cnt


def bbox_of_intersecting(segs: list[Seg], t0: int, t1: int,
                         gx0: float, gx1: float, gy0: float, gy1: float):
    """MBR of all segments whose time interval overlaps [t0, t1]."""
    pool = time_filter(segs, t0, t1)
    if not pool:
        return None
    x0 = min(s.lx for s in pool)
    x1 = max(s.hx for s in pool)
    y0 = min(s.ly for s in pool)
    y1 = max(s.hy for s in pool)
    return (max(gx0, x0), min(gx1, x1), max(gy0, y0), min(gy1, y1))


# ── main generator ─────────────────────────────────────────────────────────

def _time_partitions(segs: list[Seg], n: int = 4) -> list[tuple[int, int]]:
    """Split total time span into n equal partitions, each with its own data range."""
    t_min = min(s.t0 for s in segs)
    t_max = max(s.t1 for s in segs)
    total_span = t_max - t_min
    part_span = total_span // n
    parts = []
    for i in range(n):
        p_lo = t_min + i * part_span
        p_hi = t_min + (i + 1) * part_span if i < n - 1 else t_max
        parts.append((p_lo, p_hi))
    return parts


def _find_best_time_in_range(segs_by_mid: list[Seg], t_search_lo: int, t_search_hi: int,
                             window_s: int, t_min: int, t_max: int) -> int:
    """Find start time in range with most segments, using two-pointer over sorted starts."""
    t_search_lo = max(t_min, t_search_lo)
    t_search_hi = min(t_max, t_search_hi)
    if t_search_hi - t_search_lo < window_s:
        return t_search_lo

    # Collect segment start times within search range (expanded by window)
    starts = sorted(s.t0 for s in segs_by_mid
                    if s.t0 >= t_search_lo - window_s and s.t0 <= t_search_hi)

    if not starts:
        # Fallback: pick segment midpoints in range
        mids = sorted(s.t_mid for s in segs_by_mid
                      if s.t_mid >= t_search_lo and s.t_mid <= t_search_hi)
        if mids:
            return max(t_search_lo, min(int(mids[len(mids)//2]) - window_s // 2, t_search_hi - window_s))
        return t_search_lo

    best_t = t_search_lo
    best_cnt = 0
    j = 0
    n = len(starts)
    for i in range(n):
        t_start = starts[i]
        t_end = t_start + window_s
        while j < n and starts[j] < t_end:
            j += 1
        cnt = j - i
        if cnt > best_cnt:
            best_cnt = cnt
            best_t = t_start
    return max(t_search_lo, min(best_t, t_search_hi - window_s))


def generate_windows(
    segs: list[Seg],
    min_segments: int = 50,
    narrow_seconds: int = 3600,
    medium_seconds: int = 13320,
    wide_seconds: int = 43200,
    narrow_cell_deg: float = 0.5,
    narrow_max_span: float = 2.0,
    medium_cell_deg: float = 1.2,
    medium_max_span: float = 4.0,
) -> list[dict]:
    gx0 = min(s.lx for s in segs)
    gx1 = max(s.hx for s in segs)
    gy0 = min(s.ly for s in segs)
    gy1 = max(s.hy for s in segs)
    t_min = min(s.t0 for s in segs)
    t_max = max(s.t1 for s in segs)

    print(f"Global bounds: lon[{gx0:.4f}, {gx1:.4f}] lat[{gy0:.4f}, {gy1:.4f}] "
          f"t[{t_min}, {t_max}]")

    segs_by_mid = sorted(segs, key=lambda s: s.t_mid)
    partitions = _time_partitions(segs, 4)

    print("Time partitions (by equal span):")
    for i, (lo, hi) in enumerate(partitions):
        cnt = sum(1 for s in segs if s.t_mid >= lo and s.t_mid < hi)
        print(f"  p{i+1}: [{lo}, {hi}]  ({hi-lo:.0f}s = {(hi-lo)/86400:.1f}d)  ~{cnt:,} segs")

    rows_out: list[dict] = []

    # ── Narrow (1h) ────────────────────────────────────────────────────
    dn = narrow_seconds
    print(f"\n--- Narrow tier ({dn}s = {dn/60:.0f}min, cell={narrow_cell_deg}°) ---")
    for qi, (p_lo, p_hi) in enumerate(partitions):
        t0 = _find_best_time_in_range(segs_by_mid, p_lo, p_hi, dn, t_min, t_max)
        t0 = max(t_min, min(t0, t_max - dn))
        t1 = t0 + dn

        pool = time_filter(segs, t0, t1)
        print(f"  narrow q{qi+1}: time [{t0},{t1}] pool={len(pool)} segs")

        x0, x1, y0, y1, st_c = grid_spatial_dense_cell(
            pool, narrow_cell_deg, gx0, gx1, gy0, gy1)
        x0, x1, y0, y1, st_c = expand_spatial(
            segs, t0, t1, x0, x1, y0, y1, min_segments, gx0, gx1, gy0, gy1)

        if x1 - x0 > narrow_max_span:
            cx = (x0 + x1) * 0.5
            x0, x1 = cx - narrow_max_span / 2, cx + narrow_max_span / 2
        if y1 - y0 > narrow_max_span:
            cy = (y0 + y1) * 0.5
            y0, y1 = cy - narrow_max_span / 2, cy + narrow_max_span / 2

        sp_c = count_in_bbox(segs, x0, x1, y0, y1)
        rows_out.append({
            "Label": f"ud_strat_narrow_{qi+1:02d}",
            "TMin": str(t0), "TMax": str(t1),
            "XMin": f"{x0:.6f}", "XMax": f"{x1:.6f}",
            "YMin": f"{y0:.6f}", "YMax": f"{y1:.6f}",
            "STSegmentsMBR": str(st_c), "STSegments": "0",
            "SpatialSegments": str(sp_c),
            "Note": f"tier=narrow {dn}s cell={narrow_cell_deg}° max_span={narrow_max_span}° q{qi+1}",
        })

    # ── Medium (~3.7h) ─────────────────────────────────────────────────
    dm = medium_seconds
    print(f"\n--- Medium tier ({dm}s = {dm/3600:.2f}h, cell={medium_cell_deg}°) ---")
    for qi, (p_lo, p_hi) in enumerate(partitions):
        t0 = _find_best_time_in_range(segs_by_mid, p_lo, p_hi, dm, t_min, t_max)
        t0 = max(t_min, min(t0, t_max - dm))
        t1 = t0 + dm

        pool = time_filter(segs, t0, t1)
        print(f"  medium q{qi+1}: time [{t0},{t1}] pool={len(pool)} segs")

        x0, x1, y0, y1, st_c = grid_spatial_dense_cell(
            pool, medium_cell_deg, gx0, gx1, gy0, gy1)
        x0, x1, y0, y1, st_c = expand_spatial(
            segs, t0, t1, x0, x1, y0, y1, min_segments, gx0, gx1, gy0, gy1)

        if x1 - x0 > medium_max_span:
            cx = (x0 + x1) * 0.5
            x0, x1 = cx - medium_max_span / 2, cx + medium_max_span / 2
        if y1 - y0 > medium_max_span:
            cy = (y0 + y1) * 0.5
            y0, y1 = cy - medium_max_span / 2, cy + medium_max_span / 2

        sp_c = count_in_bbox(segs, x0, x1, y0, y1)
        rows_out.append({
            "Label": f"ud_strat_medium_{qi+1:02d}",
            "TMin": str(t0), "TMax": str(t1),
            "XMin": f"{x0:.6f}", "XMax": f"{x1:.6f}",
            "YMin": f"{y0:.6f}", "YMax": f"{y1:.6f}",
            "STSegmentsMBR": str(st_c), "STSegments": "0",
            "SpatialSegments": str(sp_c),
            "Note": f"tier=medium {dm}s cell={medium_cell_deg}° max_span={medium_max_span}° q{qi+1}",
        })

    # ── Wide (~12h) ────────────────────────────────────────────────────
    dw = wide_seconds
    print(f"\n--- Wide tier ({dw}s = {dw/3600:.1f}h) ---")
    for wi, (p_lo, p_hi) in enumerate(partitions):
        t0 = _find_best_time_in_range(segs_by_mid, p_lo, p_hi, dw, t_min, t_max)
        t0 = max(t_min, min(t0, t_max - dw))
        t1 = t0 + dw

        bb = bbox_of_intersecting(segs, t0, t1, gx0, gx1, gy0, gy1)
        if bb is None:
            # Fallback: widen search to find ANY data in this time partition
            print(f"  wide q{wi+1}: empty bbox at [{t0},{t1}], searching for data...")
            # Try the partition center
            t_center = (p_lo + p_hi) // 2
            t0 = max(t_min, t_center - dw // 2)
            t1 = min(t_max, t0 + dw)
            bb = bbox_of_intersecting(segs, t0, t1, gx0, gx1, gy0, gy1)
            if bb is None:
                # Last resort: use global spatial bbox
                print(f"  wide q{wi+1}: still empty, using global bbox")
                x0, x1, y0, y1 = gx0, gx1, gy0, gy1
            else:
                x0, x1, y0, y1 = bb
        else:
            x0, x1, y0, y1 = bb

        print(f"  wide q{wi+1}: time [{t0},{t1}] pool={len(time_filter(segs, t0, t1))} segs")

        pad = max((x1 - x0), (y1 - y0)) * 0.02 + 1e-4
        x0, x1 = max(gx0, x0 - pad), min(gx1, x1 + pad)
        y0, y1 = max(gy0, y0 - pad), min(gy1, y1 + pad)
        x0, x1, y0, y1, st_c = expand_spatial(
            segs, t0, t1, x0, x1, y0, y1, min_segments, gx0, gx1, gy0, gy1)
        sp_c = count_in_bbox(segs, x0, x1, y0, y1)

        rows_out.append({
            "Label": f"ud_strat_wide_{wi+1:02d}",
            "TMin": str(t0), "TMax": str(t1),
            "XMin": f"{x0:.6f}", "XMax": f"{x1:.6f}",
            "YMin": f"{y0:.6f}", "YMax": f"{y1:.6f}",
            "STSegmentsMBR": str(st_c), "STSegments": "0",
            "SpatialSegments": str(sp_c),
            "Note": f"tier=wide {dw}s (~{dw/3600:.1f}h) slice {wi+1}/4",
        })

    return rows_out


# ── point-truth scan ─────────────────────────────────────────────────────

def apply_point_truth(points_path: Path, rows: list[dict]):
    """Stream points CSV, count distinct segment_ids per window (point-level truth)."""
    specs = [(int(r["TMin"]), int(r["TMax"]),
              float(r["XMin"]), float(r["XMax"]),
              float(r["YMin"]), float(r["YMax"]))
             for r in rows]
    seen: list[set[str]] = [set() for _ in rows]
    counts = [0] * len(rows)
    n = 0
    print(f"\nPoint-truth scan over {points_path} ...")
    with points_path.open("r", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            n += 1
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
            if n % 10_000_000 == 0:
                print(f"  ... scanned {n:,} point rows, window caps: "
                      f"{[c for c in counts]}", flush=True)
    for r, c in zip(rows, counts):
        r["STSegments"] = str(c)
    print(f"  Done. {n:,} points scanned.")


# ── output ────────────────────────────────────────────────────────────────

def write_csv(path: Path, rows: list[dict], fieldnames: list[str]):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--meta", type=Path, default=DEFAULT_META)
    ap.add_argument("--points", type=Path, default=DEFAULT_POINTS)
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUTDIR)
    ap.add_argument("--min-segments", type=int, default=50)
    ap.add_argument("--narrow-seconds", type=int, default=3600)
    ap.add_argument("--medium-seconds", type=int, default=13320)
    ap.add_argument("--wide-seconds", type=int, default=43200)
    ap.add_argument("--narrow-cell-deg", type=float, default=0.5)
    ap.add_argument("--narrow-max-span", type=float, default=2.0)
    ap.add_argument("--medium-cell-deg", type=float, default=1.2)
    ap.add_argument("--medium-max-span", type=float, default=4.0)
    args = ap.parse_args()

    # ── Load meta ──
    segs = load_segments(args.meta)
    if not segs:
        raise SystemExit("No segments loaded")

    # ── Generate windows ──
    rows = generate_windows(
        segs,
        min_segments=args.min_segments,
        narrow_seconds=args.narrow_seconds,
        medium_seconds=args.medium_seconds,
        wide_seconds=args.wide_seconds,
        narrow_cell_deg=args.narrow_cell_deg,
        narrow_max_span=args.narrow_max_span,
        medium_cell_deg=args.medium_cell_deg,
        medium_max_span=args.medium_max_span,
    )

    # ── Point-truth ──
    apply_point_truth(args.points, rows)

    # ── Validate ──
    low = [r for r in rows if int(r["STSegments"]) < args.min_segments]
    if low:
        print(f"\nWARNING: point-truth count below min_segments ({args.min_segments}):")
        for r in low:
            print(f"  {r['Label']}: STSegments={r['STSegments']} (MBR={r['STSegmentsMBR']})")

    # ── Write ST windows CSV ──
    st_fields = ["Label", "TMin", "TMax", "XMin", "XMax", "YMin", "YMax",
                 "STSegments", "STSegmentsMBR", "SpatialSegments", "Note"]
    st_path = args.out_dir / "unified_stratified12_st_windows.csv"
    write_csv(st_path, rows, st_fields)
    print(f"\nWrote ST windows → {st_path}")

    # ── Write spatial-only CSV ──
    spatial_rows = []
    for r in rows:
        tier = r["Label"].split("_")[2]  # narrow/medium/wide
        spatial_rows.append({
            "Label": r["Label"],
            "XMin": r["XMin"], "XMax": r["XMax"],
            "YMin": r["YMin"], "YMax": r["YMax"],
            "TMin": r["TMin"], "TMax": r["TMax"],
            "SpatialSegments": r["SpatialSegments"],
            "Note": f"tier={tier}; spatial only; st_ref STSegments={r['STSegments']}",
        })
    sp_path = args.out_dir / "unified_stratified12_spatial_windows.csv"
    sp_fields = ["Label", "XMin", "XMax", "YMin", "YMax", "TMin", "TMax",
                 "SpatialSegments", "Note"]
    write_csv(sp_path, spatial_rows, sp_fields)
    print(f"Wrote spatial windows → {sp_path}")

    # ── Write JSON reference ──
    ref = {
        "meta_csv": str(args.meta.resolve()),
        "points_csv": str(args.points.resolve()),
        "st_count_method": "point_truth",
        "windows": {
            r["Label"]: {
                "ref_st_segments": int(r["STSegments"]),
                "ref_st_segments_mbr": int(r["STSegmentsMBR"]),
                "ref_spatial_segments": int(r["SpatialSegments"]),
                "t_min": int(r["TMin"]), "t_max": int(r["TMax"]),
                "x_min": float(r["XMin"]), "x_max": float(r["XMax"]),
                "y_min": float(r["YMin"]), "y_max": float(r["YMax"]),
            }
            for r in rows
        },
    }
    ref_path = args.out_dir / "unified_stratified12_ref_counts.json"
    ref_path.write_text(json.dumps(ref, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Wrote ref counts → {ref_path}")

    # ── Summary ──
    print(f"\n{'='*75}")
    print(f"{'Label':<24} {'ST':>6} {'MBR':>6} {'Spat':>6}  {'Span':>10}  T=[{rows[0]['TMin']}..]")
    print("-" * 75)
    for r in rows:
        lon_s = float(r["XMax"]) - float(r["XMin"])
        lat_s = float(r["YMax"]) - float(r["YMin"])
        print(f"{r['Label']:<24} {r['STSegments']:>6} {r['STSegmentsMBR']:>6} "
              f"{r['SpatialSegments']:>6}  {lon_s:4.2f}x{lat_s:4.2f}°  "
              f"T=[{r['TMin']},{r['TMax']}]")


if __name__ == "__main__":
    main()
