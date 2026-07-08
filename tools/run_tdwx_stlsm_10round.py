#!/usr/bin/env python3
"""ST-LSM 10-round repeat query for td_wx (6 windows, trim max/min, avg 8)."""

from __future__ import annotations
import csv, json, re, statistics, subprocess, sys, time
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
BENCH_EXE = PROJECT / "rocks-demo" / "build" / "st_meta_read_bench.exe"
DB = PROJECT / "data" / "runs" / "tdwx_stlsm"
WINDOWS_CSV = PROJECT / "data" / "td_wx" / "windows6.csv"
OUT_DIR = PROJECT / "Result" / "tdwx_comparison" / "stlsm"

def load_windows() -> list[dict]:
    """Load 6 query windows from windows6.csv."""
    windows = []
    with open(WINDOWS_CSV, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            windows.append({
                "label": row["Label"],
                "t_min": int(row["TMin"]), "t_max": int(row["TMax"]),
                "x_min": float(row["XMin"]), "x_max": float(row["XMax"]),
                "y_min": float(row["YMin"]), "y_max": float(row["YMax"]),
            })
    return windows

def run_one_query(bench: Path, db: Path, w: dict) -> tuple[dict, str]:
    cmd = [
        str(bench), "--db", str(db),
        "--no-full-scan", "--verify-kv-results",
        "--event-time-bucket-width-sec", "86400",
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
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    raw = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"bench exit {proc.returncode}\n{raw[-2000:]}")

    m = {}
    kv_match = re.search(r"prune_inwindow_kv=(\d+)", raw)
    m["finalSize"] = int(kv_match.group(1)) if kv_match else 0
    seg_match = re.search(r"prune_inwindow_segment_kv=(\d+)", raw)
    m["segment_kv"] = int(seg_match.group(1)) if seg_match else 0
    wall_match = re.search(r"wall_us_total=([\d.eE+-]+)", raw)
    m["time_ms"] = float(wall_match.group(1)) / 1000.0 if wall_match else 0
    br_match = re.search(r"prune_scan.*block_read_count=(\d+)", raw)
    m["block_read_count"] = int(br_match.group(1)) if br_match else 0
    fs_match = re.search(r"prune_file_skipped=(\d+)", raw)
    m["file_skipped"] = int(fs_match.group(1)) if fs_match else 0
    fc_match = re.search(r"prune_file_diag considered=(\d+)", raw)
    m["file_considered"] = int(fc_match.group(1)) if fc_match else 0
    fn_match = re.search(r"false_negative_count=(\d+)", raw)
    m["false_negative"] = int(fn_match.group(1)) if fn_match else -1
    return m, raw

def trimmed_mean(vals: list[float]) -> float:
    """Drop min and max, average the rest. Needs at least 3 values."""
    if len(vals) < 3:
        return statistics.mean(vals)
    s = sorted(vals)
    return statistics.mean(s[1:-1])

def main():
    rounds = 10
    if not DB.is_dir() or not (DB / "CURRENT").is_file():
        print(f"ERROR: DB not found at {DB}"); return 1
    if not BENCH_EXE.is_file():
        print(f"ERROR: bench missing at {BENCH_EXE}"); return 1

    windows = load_windows()
    print(f"{len(windows)} windows loaded:")
    for w in windows:
        print(f"  {w['label']}: t=[{w['t_min']},{w['t_max']}] x=[{w['x_min']},{w['x_max']}] y=[{w['y_min']},{w['y_max']}]")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    round_dir = OUT_DIR / time.strftime("%Y%m%d_%H%M%S")
    round_dir.mkdir(parents=True, exist_ok=True)

    # ── 10 rounds ──
    all_results: dict[str, list[dict]] = {w["label"]: [] for w in windows}

    for rnd in range(1, rounds + 1):
        print(f"\n{'='*60}")
        print(f"Round {rnd}/{rounds}")
        print(f"{'='*60}")
        for w in windows:
            label = w["label"]
            t0 = time.perf_counter()
            try:
                m, raw = run_one_query(BENCH_EXE, DB, w)
            except RuntimeError as e:
                print(f"  {label}: FAILED {e}"); return 1
            dt = time.perf_counter() - t0
            print(f"  {label:20s} finalSize={m['finalSize']:>6}  "
                  f"time={m['time_ms']:.1f}ms  blocks={m['block_read_count']}  "
                  f"files={m['file_skipped']}/{m['file_considered']}  "
                  f"fn={m['false_negative']}  wall={dt:.1f}s")
            all_results[label].append(m)
            (round_dir / f"r{rnd:02d}_{label}.log").write_text(raw, encoding="utf-8")

    # ── Summary ──
    print(f"\n{'='*80}")
    print(f"SUMMARY — {len(windows)} windows x {rounds} rounds")
    print(f"{'='*80}")
    print(f"{'Window':<20s} {'finalSize':>10s} {'Time avg':>10s} {'TrimAvg':>10s} {'Time min':>10s} {'Time max':>10s}  {'Blocks':>8s}")
    print("-" * 85)

    summary = {"rounds": rounds, "windows": {}}
    all_final_ok = True
    all_times, all_blocks = [], []

    for w in windows:
        label = w["label"]
        rs = all_results[label]
        sizes = set(r["finalSize"] for r in rs)
        fn_vals = set(r["false_negative"] for r in rs)
        correct = len(sizes) == 1  # all rounds same result
        fn_ok = fn_vals == {0}
        if not correct or not fn_ok:
            all_final_ok = False
            if not fn_ok:
                print(f"  WARNING: {label} false_negative={fn_vals}")

        times = [r["time_ms"] for r in rs]
        blocks = [r["block_read_count"] for r in rs]
        all_times.extend(times)
        all_blocks.extend(blocks)

        t_avg = statistics.mean(times)
        t_trim = trimmed_mean(times)
        b_avg = statistics.mean(blocks)

        summary["windows"][label] = {
            "finalSize": list(sizes)[0] if len(sizes) == 1 else list(sizes),
            "correct": correct, "fn_ok": fn_ok,
            "time_ms_avg": t_avg, "time_ms_trimmed_mean": t_trim,
            "time_ms_min": min(times), "time_ms_max": max(times),
            "block_read_avg": b_avg,
        }
        print(f"{label:<20s} {str(list(sizes)[0] if correct else list(sizes)):>10s} "
              f"{t_avg:>9.1f}ms {t_trim:>9.1f}ms {min(times):>9.1f}ms {max(times):>9.1f}ms  {b_avg:>8.0f}")

    print("-" * 85)
    print(f"{'OVERALL':<20s} {'':>10s} "
          f"{statistics.mean(all_times):>9.1f}ms {trimmed_mean(all_times):>9.1f}ms")

    sp = round_dir / "summary.json"
    sp.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"\nFull results: {sp}")
    return 0 if all_final_ok else 1

if __name__ == "__main__":
    raise SystemExit(main())
