#!/usr/bin/env python3
"""10-round repeat: run each of 9 windows as a single query, 10 rounds."""

from __future__ import annotations
import argparse, json, math, re, statistics, subprocess, sys, time
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
BENCH_EXE = PROJECT / "rocks-demo" / "build" / "st_meta_read_bench.exe"
DB = PROJECT / "data" / "runs" / "UnifiedST"
REF_COUNTS = PROJECT / "data" / "processed" / "unified" / "unified_stratified12_ref_counts.json"
OUT_DIR = PROJECT / "Result" / "TSV" / "unified_st_query_repeat"


def load_windows() -> list[tuple[str, dict]]:
    ref = json.loads(REF_COUNTS.read_text(encoding="utf-8"))
    return [(label, w) for label, w in ref["windows"].items()]


def run_one_query(bench: Path, db: Path, label: str, w: dict) -> tuple[dict, str]:
    cmd = [
        str(bench), "--db", str(db),
        "--no-full-scan", "--verify-kv-results",
        "--prune-t-min", str(w["t_min"]),
        "--prune-t-max", str(w["t_max"]),
        "--prune-x-min", str(w["x_min"]),
        "--prune-x-max", str(w["x_max"]),
        "--prune-y-min", str(w["y_min"]),
        "--prune-y-max", str(w["y_max"]),
        "--time-bucket-count", "32",
        "--rtree-leaf-size", "8",
        "--virtual-merge", "--virtual-merge-auto",
        "--vm-time-span-sec-threshold", "21600",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    raw = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"bench exit {proc.returncode}\n{raw[-2000:]}")

    m = {}
    # prune_inwindow_kv from verify output
    kv_match = re.search(r"prune_inwindow_kv=(\d+)", raw)
    m["finalSize"] = int(kv_match.group(1)) if kv_match else 0
    # prune_inwindow_segment_kv
    seg_match = re.search(r"prune_inwindow_segment_kv=(\d+)", raw)
    m["segment_kv"] = int(seg_match.group(1)) if seg_match else 0

    # wall_us
    wall_match = re.search(r"wall_us_total=([\d.eE+-]+)", raw)
    m["time_ms"] = float(wall_match.group(1)) / 1000.0 if wall_match else 0

    # block_read_count
    br_match = re.search(r"prune_scan.*block_read_count=(\d+)", raw)
    m["block_read_count"] = int(br_match.group(1)) if br_match else 0

    # BENCH_CONTRIB line
    bc_match = re.search(r"BENCH_CONTRIB\s+(.*)", raw)
    bench = {}
    if bc_match:
        for f in bc_match.group(1).split():
            if "=" in f:
                k, v = f.split("=", 1)
                try:
                    bench[k] = int(v) if v.replace("-","").isdigit() else float(v)
                except ValueError:
                    bench[k] = v
    m["_bench"] = bench

    # IO bytes_read
    io_match = re.search(r"prune_scan.*bytes_read=(\d+)", raw)
    m["bytes_read"] = int(io_match.group(1)) if io_match else 0

    return m, raw


def pctile(vals: list[float], p: float) -> float:
    if not vals: return 0.0
    xs = sorted(vals)
    k = (len(xs) - 1) * (p / 100.0)
    lo, hi = int(k), int(math.ceil(k))
    return xs[lo] if lo == hi else xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", type=Path, default=BENCH_EXE)
    ap.add_argument("--db", type=Path, default=DB)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--out-dir", type=Path, default=OUT_DIR)
    args = ap.parse_args()

    if not args.db.is_dir() or not (args.db / "CURRENT").is_file():
        print(f"ERROR: DB not found at {args.db}"); return 1
    if not args.bench.is_file():
        print(f"ERROR: bench missing at {args.bench}"); return 1

    windows = load_windows()
    print(f"{len(windows)} windows loaded:")
    for label, w in windows:
        print(f"  {label}: ref_st={w['ref_st_segments']}")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    round_dir = args.out_dir / time.strftime("%Y%m%d_%H%M%S")
    round_dir.mkdir(parents=True, exist_ok=True)

    # ── 10 rounds, 9 windows each ──
    all_results: dict[str, list[dict]] = {label: [] for label, _ in windows}

    for rnd in range(1, args.rounds + 1):
        print(f"\n{'='*60}")
        print(f"Round {rnd}/{args.rounds}")
        print(f"{'='*60}")
        for label, w in windows:
            t0 = time.perf_counter()
            try:
                m, raw = run_one_query(args.bench, args.db, label, w)
            except RuntimeError as e:
                print(f"  {label}: FAILED {e}")
                return 1
            dt = time.perf_counter() - t0
            ok = "OK" if m["finalSize"] == w["ref_st_segments"] else f"MISMATCH(ref={w['ref_st_segments']})"
            print(f"  {label:24s} finalSize={m['finalSize']:>6} {ok:>20s}  "
                  f"time={m['time_ms']:.1f}ms  blocks={m['block_read_count']}  "
                  f"wall={dt:.1f}s")
            all_results[label].append(m)
            (round_dir / f"r{rnd:02d}_{label}.log").write_text(raw, encoding="utf-8")

    # ── Summary ──
    summary = {"rounds": args.rounds, "windows": {}, "overall": {}}
    all_final_ok = True
    all_times, all_blocks = [], []

    print(f"\n{'='*70}")
    print(f"SUMMARY — {len(windows)} windows × {args.rounds} rounds")
    print(f"{'='*70}")
    print(f"{'Window':<24s} {'Correct':>8s} {'Time avg':>10s} {'Time p90':>10s}  {'Blocks avg':>10s}  Ref")
    print("-" * 80)

    for label, w in windows:
        rs = all_results[label]
        sizes = set(r["finalSize"] for r in rs)
        correct = len(sizes) == 1 and list(sizes)[0] == w["ref_st_segments"]
        if not correct:
            all_final_ok = False

        times = [r["time_ms"] for r in rs]
        blocks = [r["block_read_count"] for r in rs]
        all_times.extend(times)
        all_blocks.extend(blocks)

        t_avg = statistics.mean(times)
        t_p90 = pctile(times, 90)
        b_avg = statistics.mean(blocks)

        summary["windows"][label] = {
            "ref_st_segments": w["ref_st_segments"],
            "correct": correct,
            "finalSize_values": list(sizes),
            "time_ms_avg": t_avg, "time_ms_p90": t_p90,
            "time_ms_min": min(times), "time_ms_max": max(times),
            "block_read_avg": b_avg, "block_read_min": min(blocks), "block_read_max": max(blocks),
        }
        print(f"{label:<24s} {str(correct):>8s} {t_avg:>9.1f}ms {t_p90:>9.1f}ms  {b_avg:>10.0f}  {w['ref_st_segments']}")

    summary["overall"] = {
        "total_queries": len(windows) * args.rounds,
        "all_correct": all_final_ok,
        "time_ms_avg": statistics.mean(all_times),
        "time_ms_p90": pctile(all_times, 90),
        "block_read_avg": statistics.mean(all_blocks),
    }

    print("-" * 80)
    print(f"{'OVERALL':<24s} {str(all_final_ok):>8s} "
          f"{summary['overall']['time_ms_avg']:>9.1f}ms "
          f"{summary['overall']['time_ms_p90']:>9.1f}ms  "
          f"{summary['overall']['block_read_avg']:>10.0f}")
    print(f"\nTotal queries: {summary['overall']['total_queries']}")

    sp = round_dir / "summary.json"
    sp.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"\nFull results: {sp}")
    return 0 if all_final_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
