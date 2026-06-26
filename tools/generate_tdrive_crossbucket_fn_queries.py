#!/usr/bin/env python3
"""Generate ST query suites to probe false negatives on cross-calendar-bucket segments.

Single-key ingest + S(F) Manifest fan-out must recall segments when the query
calendar bucket is NOT the canonical bucket (floor(t_start/W)).

Suites (under --out-root):
  crossbucket_middle20   — query hour = middle calendar bucket (non-canonical)
  crossbucket_tail20     — query hour = tail calendar bucket
  crossbucket_canonical20 — control: query hour = canonical bucket
  crossbucket_triple15   — 15 segments × (canonical/middle/tail) = 45 queries

Each query embeds point-truth ground truth (segment_ids + counts) at generation.
"""

from __future__ import annotations

import argparse
import csv
import json
import random
from dataclasses import dataclass, field
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_META = PROJECT / "data" / "processed" / "tdrive" / "segments_meta.csv"
DEFAULT_POINTS = PROJECT / "data" / "processed" / "tdrive" / "segments_points.csv"
DEFAULT_OUT = PROJECT / "data" / "processed" / "tman_tdrive" / "tdrive_query_st_crossbucket_fn"


@dataclass
class CrossSeg:
    segment_id: str
    t_start: int
    t_end: int
    b0: int
    b1: int
    lx: float
    hx: float
    ly: float
    hy: float

    @property
    def span(self) -> int:
        return self.b1 - self.b0


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
    target_segment_id: str = ""
    query_calendar_bucket: int = -1
    canonical_bucket: int = -1
    bucket_span: int = 0


@dataclass
class WindowTruth:
    spec: WindowSpec
    segment_ids: set[str] = field(default_factory=set)
    point_count: int = 0


def load_cross_segments(meta_csv: Path, bucket_sec: int) -> list[CrossSeg]:
    out: list[CrossSeg] = []
    with meta_csv.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            ts = int(row["t_start_unix"])
            te = int(row["t_end_unix"])
            b0 = ts // bucket_sec
            b1 = te // bucket_sec
            if b1 <= b0:
                continue
            out.append(
                CrossSeg(
                    segment_id=row["segment_id"],
                    t_start=ts,
                    t_end=te,
                    b0=b0,
                    b1=b1,
                    lx=float(row["lon_min"]),
                    hx=float(row["lon_max"]),
                    ly=float(row["lat_min"]),
                    hy=float(row["lat_max"]),
                )
            )
    return out


def pick_diverse(segs: list[CrossSeg], n: int, *, min_span: int, rng: random.Random) -> list[CrossSeg]:
    pool = [s for s in segs if s.span >= min_span]
    if len(pool) < n:
        raise SystemExit(f"need {n} segments with span>={min_span}, have {len(pool)}")
    by_span: dict[int, list[CrossSeg]] = {}
    for s in pool:
        by_span.setdefault(s.span, []).append(s)
    spans = sorted(by_span.keys())
    picked: list[CrossSeg] = []
    used: set[str] = set()
    si = 0
    while len(picked) < n:
        span = spans[si % len(spans)]
        cand = [s for s in by_span[span] if s.segment_id not in used]
        if not cand:
            si += 1
            if si > len(spans) * 4:
                break
            continue
        s = rng.choice(cand)
        picked.append(s)
        used.add(s.segment_id)
        si += 1
    if len(picked) < n:
        raise SystemExit(f"could only pick {len(picked)}/{n} diverse cross-bucket segments")
    return picked


def query_calendar_bucket(seg: CrossSeg, kind: str) -> int:
    if kind == "canonical":
        return seg.b0
    if kind == "tail":
        return seg.b1
    # middle (non-canonical when span >= 1)
    if seg.span == 1:
        return seg.b1
    mid = seg.b0 + seg.span // 2
    return max(seg.b0 + 1, min(mid, seg.b1))


def bucket_time_slice(seg: CrossSeg, bucket: int, bucket_sec: int) -> tuple[int, int] | None:
    t0 = max(seg.t_start, bucket * bucket_sec)
    t1 = min(seg.t_end, (bucket + 1) * bucket_sec - 1)
    if t0 > t1:
        return None
    return t0, t1


def mbr_window(seg: CrossSeg, t0: int, t1: int, *, pad_ratio: float = 0.02) -> WindowSpec | None:
    pad_x = max(0.0005, (seg.hx - seg.lx) * pad_ratio)
    pad_y = max(0.0005, (seg.hy - seg.ly) * pad_ratio)
    return WindowSpec(
        label="",
        suite="",
        t0=t0,
        t1=t1,
        x0=seg.lx - pad_x,
        x1=seg.hx + pad_x,
        y0=seg.ly - pad_y,
        y1=seg.hy + pad_y,
        target_segment_id=seg.segment_id,
        canonical_bucket=seg.b0,
        bucket_span=seg.span,
    )


def make_queries(
    segs: list[CrossSeg],
    *,
    suite: str,
    kind: str,
    bucket_sec: int,
    label_prefix: str,
) -> list[WindowSpec]:
    out: list[WindowSpec] = []
    for i, seg in enumerate(segs):
        qb = query_calendar_bucket(seg, kind)
        ts = bucket_time_slice(seg, qb, bucket_sec)
        if ts is None:
            continue
        w = mbr_window(seg, ts[0], ts[1])
        if w is None:
            continue
        w.suite = suite
        w.label = f"{label_prefix}{i:02d}_{seg.segment_id.split(':')[-1]}"
        w.query_calendar_bucket = qb
        out.append(w)
    return out


def compute_truth_for_windows(points_csv: Path, specs: list[WindowSpec]) -> list[WindowTruth]:
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


def trajectory_id(segment_id: str) -> str:
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
                "st_segments_with_point": len(seg_ids),
                "st_points_in_window": wt.point_count,
                "target_segment_id": w.target_segment_id,
                "query_calendar_bucket": w.query_calendar_bucket,
                "canonical_bucket": w.canonical_bucket,
                "bucket_span": w.bucket_span,
                "target_in_results": w.target_segment_id in wt.segment_ids,
                "segment_ids": seg_ids,
                "trajectory_ids": traj_ids,
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
                "query_calendar_bucket": w.query_calendar_bucket,
                "canonical_bucket": w.canonical_bucket,
                "target_segment_id": w.target_segment_id,
                "st_segments_with_point": len(seg_ids),
            }
        )

    (out_dir / "query_conditions_st.txt").write_text(";".join(qparts) + "\n", encoding="utf-8")
    (out_dir / "queries_manifest_st.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    (out_dir / "query_count.txt").write_text(f"{len(truths)}\n", encoding="utf-8")
    with (out_dir / "st_queries.csv").open("w", newline="", encoding="utf-8") as f:
        wcsv = csv.DictWriter(f, fieldnames=list(st_rows[0].keys()))
        wcsv.writeheader()
        wcsv.writerows(st_rows)

    missing_target = sum(1 for r in manifest if not r["target_in_results"])
    counts = [r["st_segments_with_point"] for r in manifest]
    print(
        f"  {suite}: queries={len(truths)} truth_segments=[{min(counts)},{max(counts)}] "
        f"target_missing={missing_target}"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--meta-csv", type=Path, default=DEFAULT_META)
    ap.add_argument("--points-csv", type=Path, default=DEFAULT_POINTS)
    ap.add_argument("--out-root", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--bucket-sec", type=int, default=3600)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--per-suite", type=int, default=20)
    ap.add_argument("--triple-segments", type=int, default=15)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    cross = load_cross_segments(args.meta_csv, args.bucket_sec)
    print(f"cross-bucket segments: {len(cross)} (bucket_sec={args.bucket_sec})")

    picked20 = pick_diverse(cross, args.per_suite, min_span=1, rng=rng)
    picked_triple = pick_diverse(cross, args.triple_segments, min_span=2, rng=rng)

    suite_plans: list[tuple[str, list[WindowSpec]]] = [
        (
            "crossbucket_middle20",
            make_queries(
                picked20,
                suite="crossbucket_middle20",
                kind="middle",
                bucket_sec=args.bucket_sec,
                label_prefix="mid_",
            ),
        ),
        (
            "crossbucket_tail20",
            make_queries(
                picked20,
                suite="crossbucket_tail20",
                kind="tail",
                bucket_sec=args.bucket_sec,
                label_prefix="tail_",
            ),
        ),
        (
            "crossbucket_canonical20",
            make_queries(
                picked20,
                suite="crossbucket_canonical20",
                kind="canonical",
                bucket_sec=args.bucket_sec,
                label_prefix="canon_",
            ),
        ),
    ]

    triple_specs: list[WindowSpec] = []
    for i, seg in enumerate(picked_triple):
        for kind, tag in (("canonical", "c"), ("middle", "m"), ("tail", "t")):
            qb = query_calendar_bucket(seg, kind)
            ts = bucket_time_slice(seg, qb, args.bucket_sec)
            if ts is None:
                continue
            w = mbr_window(seg, ts[0], ts[1])
            if w is None:
                continue
            w.suite = "crossbucket_triple15"
            w.label = f"tri{i:02d}_{tag}_{seg.segment_id.split(':')[-1]}"
            w.query_calendar_bucket = qb
            triple_specs.append(w)
    suite_plans.append(("crossbucket_triple15", triple_specs))

    args.out_root.mkdir(parents=True, exist_ok=True)
    for suite_name, specs in suite_plans:
        if not specs:
            raise SystemExit(f"empty suite {suite_name}")
        print(f"truth scan: {suite_name} ({len(specs)} windows)...", flush=True)
        truths = compute_truth_for_windows(args.points_csv, specs)
        write_suite(args.out_root / suite_name, truths)

    summary = {
        "bucket_sec": args.bucket_sec,
        "cross_bucket_segments": len(cross),
        "suites": [name for name, _ in suite_plans],
        "out_root": str(args.out_root),
    }
    (args.out_root / "generation_summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Wrote suites under {args.out_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
