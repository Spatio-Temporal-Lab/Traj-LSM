#!/usr/bin/env python3
"""Summarize 10-round ST vs TMan bench (time + finalSize).

Reads:
  Result/TSV/tdrive_st_query_10round/run_XX/st_prune_{suite}.csv
  Result/tman_tdrive_st_10round/run_XX/z_order_{suite}.csv  (or tman_tdrive_st with run_*)

Usage:
  python tools/summarize_tdrive_st_10round_bench.py
  python tools/summarize_tdrive_st_10round_bench.py --st-root ... --tman-root ...
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_ST = PROJECT / "Result" / "TSV" / "tdrive_st_query_10round"
DEFAULT_TMAN = PROJECT / "Result" / "tman_tdrive_st_10round"
DEFAULT_OUT = PROJECT / "Result" / "TSV" / "tdrive_st_10round_summary.tsv"


def load_metric(csv_path: Path, metric: str) -> float | None:
    with csv_path.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if (row.get("type") or "").strip() == metric:
                return float(row.get("avg") or 0)
    return None


def per_query_time_stats(json_path: Path) -> dict[str, float] | None:
    if not json_path.is_file():
        return None
    times = [float(r["time"]) for r in json.loads(json_path.read_text(encoding="utf-8"))]
    if not times:
        return None
    times.sort()

    def pct(p: float) -> float:
        k = (len(times) - 1) * (p / 100.0)
        lo, hi = int(math.floor(k)), int(math.ceil(k))
        if lo == hi:
            return times[lo]
        return times[lo] + (times[hi] - times[lo]) * (k - lo)

    return {
        "avg": statistics.mean(times),
        "mid": pct(50),
        "p90": pct(90),
        "sum": sum(times),
    }


def collect_rounds(root: Path, system: str, variant: str, suite: str) -> list[dict]:
    rows: list[dict] = []
    if not root.is_dir():
        return rows
    for run_dir in sorted(root.glob("run_*")):
        if system == "st":
            csv_p = run_dir / f"st_prune_{suite}.csv"
            pq = run_dir / f"st_prune_{suite}_per_query.json"
        else:
            csv_p = run_dir / f"{variant}_{suite}.csv"
            pq = None
        if not csv_p.is_file():
            continue
        t_avg = load_metric(csv_p, "time")
        fs_avg = load_metric(csv_p, "finalSize")
        pq_stats = per_query_time_stats(pq) if pq else None
        rows.append(
            {
                "round": run_dir.name,
                "time_csv_avg": t_avg,
                "time_pq_avg": pq_stats["avg"] if pq_stats else None,
                "time_pq_sum": pq_stats["sum"] if pq_stats else None,
                "finalSize_avg": fs_avg,
            }
        )
    return rows


def agg(vals: list[float]) -> dict[str, float]:
    if not vals:
        return {}
    vals = sorted(vals)
    return {
        "n": len(vals),
        "mean": statistics.mean(vals),
        "std": statistics.pstdev(vals) if len(vals) > 1 else 0.0,
        "median": statistics.median(vals),
        "min": vals[0],
        "max": vals[-1],
    }


def fmt_agg(a: dict[str, float]) -> str:
    if not a:
        return "n/a"
    return f"{a['median']:.1f} (mean {a['mean']:.1f}±{a['std']:.1f}, n={int(a['n'])})"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--st-root", type=Path, default=DEFAULT_ST)
    ap.add_argument("--tman-root", type=Path, default=DEFAULT_TMAN)
    ap.add_argument("--variant", default="z_order")
    ap.add_argument("--suites", default="uniform_test,skewed_test")
    ap.add_argument("--out-tsv", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    suites = [s.strip() for s in args.suites.split(",") if s.strip()]
    report: dict = {"variant": args.variant, "suites": {}}
    tsv_rows: list[dict] = []

    print(f"{'suite':<14} {'system':<6} {'rounds':>6} {'time_median':>22} {'finalSize_median':>18}")
    print("-" * 78)

    for suite in suites:
        report["suites"][suite] = {}
        for system, root in (("ST", args.st_root), ("TMan", args.tman_root)):
            rounds = collect_rounds(root, system.lower(), args.variant, suite)
            t_vals = [r["time_csv_avg"] for r in rounds if r["time_csv_avg"] is not None]
            fs_vals = [r["finalSize_avg"] for r in rounds if r["finalSize_avg"] is not None]
            t_agg = agg([float(x) for x in t_vals])
            fs_agg = agg([float(x) for x in fs_vals])
            report["suites"][suite][system] = {
                "rounds": len(rounds),
                "time_avg_per_round": t_agg,
                "finalSize_avg_per_round": fs_agg,
                "raw": rounds,
            }
            print(
                f"{suite:<14} {system:<6} {len(rounds):>6} "
                f"{fmt_agg(t_agg):>22} {fmt_agg(fs_agg):>18}"
            )
            tsv_rows.append(
                {
                    "suite": suite,
                    "system": system,
                    "rounds": len(rounds),
                    "time_median": t_agg.get("median", ""),
                    "time_mean": t_agg.get("mean", ""),
                    "time_std": t_agg.get("std", ""),
                    "finalSize_median": fs_agg.get("median", ""),
                }
            )

        st_t = report["suites"][suite]["ST"]["time_avg_per_round"].get("median")
        tm_t = report["suites"][suite]["TMan"]["time_avg_per_round"].get("median")
        if st_t and tm_t and st_t > 0:
            ratio = tm_t / st_t
            faster = "ST" if st_t < tm_t else "TMan" if tm_t < st_t else "tie"
            if abs(ratio - 1.0) < 0.05:
                faster = "tie (~5%)"
            report["suites"][suite]["rank_median_time"] = {
                "faster": faster,
                "tman_over_st": ratio,
            }
            print(f"  -> median time rank: {faster} (TMan/ST={ratio:.2f}x)")

    args.out_tsv.parent.mkdir(parents=True, exist_ok=True)
    with args.out_tsv.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(tsv_rows[0].keys()), delimiter="\t")
        w.writeheader()
        w.writerows(tsv_rows)

    json_out = args.out_tsv.with_suffix(".json")
    json_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"\nWrote {args.out_tsv}")
    print(f"Wrote {json_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
