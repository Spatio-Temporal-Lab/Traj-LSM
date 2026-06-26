#!/usr/bin/env python3
"""Convert st_meta_read_bench sweep TSV (12 windows) to TMan spatial_stats.csv format."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

# Same as plot_tman_vs_st_spatial_stats_compare.py
METRICS = [
    "time",
    "indexRanges",
    "quadCodeRanges",
    "qOrderRanges",
    "rowKeyRanges",
    "candidates",
    "finalSize",
    "vc",
    "redisAccessCount",
]


def fnum(s: str) -> float | None:
    s = (s or "").strip()
    if not s:
        return None
    try:
        x = float(s)
        return None if math.isnan(x) else x
    except ValueError:
        return None


def percentile(vals: list[float], p: float) -> float:
    if not vals:
        return 0.0
    xs = sorted(vals)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * (p / 100.0)
    lo = int(math.floor(k))
    hi = int(math.ceil(k))
    if lo == hi:
        return xs[lo]
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def summarize(vals: list[float]) -> dict[str, float]:
    if not vals:
        return {k: 0.0 for k in ("min", "max", "avg", "mid", "per70", "per80", "per90")}
    return {
        "min": min(vals),
        "max": max(vals),
        "avg": sum(vals) / len(vals),
        "mid": percentile(vals, 50),
        "per70": percentile(vals, 70),
        "per80": percentile(vals, 80),
        "per90": percentile(vals, 90),
    }


def st_row_to_metrics(row: dict[str, str]) -> dict[str, float]:
    pw = fnum(row.get("prune_wall_us", "")) or 0.0
    final_sz = fnum(row.get("kv_prune_inwindow_kv", ""))
    if final_sz is None:
        final_sz = fnum(row.get("prune_keys_in_window", ""))
    if final_sz is None:
        final_sz = fnum(row.get("prune_keys", "")) or 0.0
    return {
        "time": pw / 1000.0,
        "indexRanges": fnum(row.get("prune_file_considered", "")) or 0.0,
        "quadCodeRanges": fnum(row.get("prune_block_index_examined", "")) or 0.0,
        "qOrderRanges": 0.0,
        "rowKeyRanges": fnum(row.get("full_keys_scanned_total", "")) or 0.0,
        "candidates": fnum(row.get("full_keys", "")) or 0.0,
        "finalSize": final_sz,
        "vc": fnum(row.get("prune_key_examined", "")) or 0.0,
        "redisAccessCount": fnum(row.get("prune_block_read", "")) or 0.0,
    }


def load_st_per_window(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open(encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if (row.get("error") or "").strip():
                continue
            m = st_row_to_metrics(row)
            m["label"] = (row.get("label") or "").strip()
            rows.append(m)
    return rows


def write_spatial_stats(stats: dict[str, dict[str, float]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["type", "min", "max", "avg", "mid", "per70", "per80", "per90"])
        for m in METRICS:
            s = stats[m]
            w.writerow(
                [
                    m,
                    int(s["min"]) if m == "time" and s["min"] == int(s["min"]) else round(s["min"], 4),
                    int(s["max"]) if m == "time" and s["max"] == int(s["max"]) else round(s["max"], 4),
                    round(s["avg"], 4),
                    round(s["mid"], 4),
                    round(s["per70"], 4),
                    round(s["per80"], 4),
                    round(s["per90"], 4),
                ]
            )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--st-bench", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--source", default="st")
    ap.add_argument("--json", type=Path, default=None, help="optional per-window JSON sidecar")
    args = ap.parse_args()

    per_win = load_st_per_window(args.st_bench)
    series: dict[str, list[float]] = {m: [] for m in METRICS}
    for r in per_win:
        for m in METRICS:
            series[m].append(float(r[m]))
    stats = {m: summarize(series[m]) for m in METRICS}
    write_spatial_stats(stats, args.out)
    print(f"Wrote {args.out} ({len(per_win)} windows)")
    if args.json:
        payload = {
            "source": args.source,
            "bench_tsv": str(args.st_bench),
            "window_count": len(per_win),
            "stats": stats,
            "per_window": per_win,
        }
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"Wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
