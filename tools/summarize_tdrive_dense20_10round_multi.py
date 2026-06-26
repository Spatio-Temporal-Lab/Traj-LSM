#!/usr/bin/env python3
"""Summarize 10-round dense20 ablation (+ optional ST/TMan) with finalSize verify."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
MANIFEST_ROOT = PROJECT / "data" / "processed" / "tman_tdrive" / "tdrive_query_st"
SUITES = ("uniform_dense20", "skewed_dense20")
MODES = ("manifest", "sst", "sst_manifest")
MODE_LABELS = {
    "manifest": "Global-only",
    "sst": "Local-only",
    "sst_manifest": "Local+Global",
}


def manifest_truth(suite: str) -> dict[int, int]:
    rows = json.loads(
        (MANIFEST_ROOT / suite / "queries_manifest_st.json").read_text(encoding="utf-8")
    )
    return {int(r["id"]): int(r["st_segments_with_point"]) for r in rows}


def load_time_csv(path: Path) -> dict[str, float]:
    with path.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if row.get("type") == "time":
                return {k: float(row[k]) for k in row if k != "type"}
    return {}


def verify_round(run_dir: Path, suite: str) -> int:
    truth = manifest_truth(suite)
    bad = 0
    for mode in MODES:
        pq = run_dir / mode / f"st_prune_{suite}_per_query.json"
        if not pq.is_file():
            continue
        got = {
            int(r["query_id"]): int(r["finalSize"])
            for r in json.loads(pq.read_text(encoding="utf-8"))
        }
        for qid, t in truth.items():
            if got.get(qid) != t:
                bad += 1
    return bad


def collect_ablation(root: Path) -> dict:
    out: dict = {}
    for suite in SUITES:
        out[suite] = {}
        for mode in MODES:
            avgs, p90s = [], []
            for rd in sorted(root.glob("run_*")):
                csv_p = rd / mode / f"st_prune_{suite}.csv"
                if not csv_p.is_file():
                    continue
                t = load_time_csv(csv_p)
                if t:
                    avgs.append(t.get("avg", 0))
                    p90s.append(t.get("per90", t.get("max", 0)))
            out[suite][mode] = {
                "n_rounds": len(avgs),
                "avg_ms_mean": statistics.mean(avgs) if avgs else 0,
                "avg_ms_median": statistics.median(avgs) if avgs else 0,
                "p90_ms_mean": statistics.mean(p90s) if p90s else 0,
                "p90_ms_median": statistics.median(p90s) if p90s else 0,
                "per_round_avg": avgs,
                "per_round_p90": p90s,
            }
    return out


def collect_st_or_tman(root: Path, pattern: str) -> dict:
    """pattern: st_prune_{suite}.csv or z_order_{suite}.csv"""
    out: dict = {}
    for suite in SUITES:
        avgs, p90s = [], []
        for rd in sorted(root.glob("run_*")):
            if pattern.startswith("st_prune"):
                csv_p = rd / f"st_prune_{suite}.csv"
            else:
                csv_p = rd / f"z_order_{suite}.csv"
            if not csv_p.is_file():
                continue
            t = load_time_csv(csv_p)
            if t:
                avgs.append(t.get("avg", 0))
                p90s.append(t.get("per90", t.get("max", 0)))
        out[suite] = {
            "n_rounds": len(avgs),
            "avg_ms_mean": statistics.mean(avgs) if avgs else 0,
            "avg_ms_median": statistics.median(avgs) if avgs else 0,
            "p90_ms_mean": statistics.mean(p90s) if p90s else 0,
            "p90_ms_median": statistics.median(p90s) if p90s else 0,
        }
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True, help="row label in compare table")
    ap.add_argument("--ablation-root", type=Path, required=True)
    ap.add_argument("--out-json", type=Path, required=True)
    ap.add_argument("--verify-only", action="store_true")
    args = ap.parse_args()

    total_bad = 0
    for rd in sorted(args.ablation_root.glob("run_*")):
        for suite in SUITES:
            total_bad += verify_round(rd, suite)

    stats = collect_ablation(args.ablation_root)
    report = {
        "label": args.label,
        "ablation_root": str(args.ablation_root),
        "finalSize_mismatches": total_bad,
        "suites": stats,
    }
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(f"=== {args.label} ({args.ablation_root.name}) ===")
    print(f"finalSize mismatches (all modes/rounds): {total_bad}")
    for suite in SUITES:
        print(f"\n-- {suite} --")
        for mode in MODES:
            s = stats[suite][mode]
            print(
                f"  {MODE_LABELS[mode]:14} "
                f"avg mean={s['avg_ms_mean']:.1f} median={s['avg_ms_median']:.1f} | "
                f"p90 mean={s['p90_ms_mean']:.1f} median={s['p90_ms_median']:.1f} "
                f"(n={s['n_rounds']})"
            )
    print(f"\nWrote {args.out_json}")
    return 1 if total_bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
