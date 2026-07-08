#!/usr/bin/env python3
"""
ST-LSM Ablation Study: Global Index / Local Index / ST-Compaction
Dataset: TDrive + Wuxi (6 query windows, 5 rounds each)
"""

from __future__ import annotations
import csv, json, os, re, statistics, subprocess, sys, time
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
BENCH = PROJECT / "rocks-demo" / "build" / "st_meta_read_bench.exe"
DB_LEVELED = PROJECT / "data" / "runs" / "tdwx_stlsm"        # ST compaction (leveled)
DB_TIERED = PROJECT / "data" / "runs" / "tdwx_stlsm_tiered"   # tiered (will be ingested)
WINDOWS_CSV = PROJECT / "data" / "td_wx" / "windows6.csv"
OUT_DIR = PROJECT / "Result" / "compare" / "ablation"

ROUNDS = 5  # ablation uses 5 rounds (fewer than final comparison)

def load_windows():
    with open(WINDOWS_CSV, encoding="utf-8") as f:
        return [{
            "label": r["Label"],
            "t_min": int(r["TMin"]), "t_max": int(r["TMax"]),
            "x_min": float(r["XMin"]), "x_max": float(r["XMax"]),
            "y_min": float(r["YMin"]), "y_max": float(r["YMax"]),
        } for r in csv.DictReader(f)]

def run_one(bench: Path, db: Path, w: dict, extra_flags: list[str], env: dict = None) -> tuple[dict, str]:
    cmd = [str(bench), "--db", str(db)]
    # Only add --no-full-scan if not doing a full scan
    if "--full-scan-mode" not in extra_flags:
        cmd.append("--no-full-scan")
    cmd.extend(extra_flags)
    for k in ["t_min","t_max","x_min","x_max","y_min","y_max"]:
        cmd += [f"--prune-{k.replace('_','-')}", str(w[k])]
    proc_env = os.environ.copy()
    if env:
        proc_env.update(env)
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=proc_env)
    raw = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"bench exit {proc.returncode}\n{raw[-2000:]}")

    m = {}
    # finalSize - try full-scan then prune-scan patterns
    kv = re.search(r"full_scan mode=\w+ keys=(\d+)", raw)
    if not kv:
        kv = re.search(r"prune_inwindow_kv=(\d+)", raw)
    if not kv:
        kv = re.search(r"keys_in_window\(last_iter\)=(\d+)", raw)
    if not kv:
        kv = re.search(r"keys\(last_iter\)=(\d+)", raw)
    m["finalSize"] = int(kv.group(1)) if kv else 0
    # time - try full-scan wall_us then prune wall_us_total
    wm = re.search(r"full_scan.*wall_us=(\d+)", raw)
    m["time_ms"] = float(wm.group(1))/1000.0 if wm else 0
    if m["time_ms"] == 0:
        wm = re.search(r"wall_us_total=([\d.eE+-]+)", raw)
        m["time_ms"] = float(wm.group(1))/1000.0 if wm else 0
    # data_blocks_decompressed — actual decompressed data blocks (not prefetch artifacts)
    db = re.search(r"data_blocks_decompressed=(\d+)", raw)
    m["data_decomp"] = int(db.group(1)) if db else 0
    # block_read_count (PerfContext, kept for reference)
    br = re.search(r"block_read_count=(\d+)", raw)
    m["block_read"] = int(br.group(1)) if br else 0
    # total keys scanned
    ks = re.search(r"keys_scanned_total=(\d+)", raw)
    m["keys_scanned"] = int(ks.group(1)) if ks else 0
    # file-level metrics
    fs = re.search(r"prune_file_skipped=(\d+)", raw)
    m["file_skipped"] = int(fs.group(1)) if fs else 0
    fc = re.search(r"prune_file_diag considered=(\d+)", raw)
    m["file_considered"] = int(fc.group(1)) if fc else 0
    ie = re.search(r"index_examined=(\d+)", raw)
    m["index_examined"] = int(ie.group(1)) if ie else 0
    ids = re.search(r"index_skipped_st_disjoint=(\d+)", raw)
    m["index_skipped"] = int(ids.group(1)) if ids else 0
    # full scan baseline for comparison
    keys_scanned = re.search(r"keys_scanned_total=(\d+)", raw)
    m["keys_scanned"] = int(keys_scanned.group(1)) if keys_scanned else 0
    return m, raw

def trimmed_mean(vals):
    if len(vals) < 3:
        return statistics.mean(vals)
    s = sorted(vals)
    return statistics.mean(s[1:-1])

def run_experiment(name: str, db: Path, windows: list[dict], flags: list[str],
                   rounds: int = ROUNDS, env: dict = None) -> dict:
    # ST-LSM DB uses 86400s event-time bucket width; ensure backward truncation
    # matches the key encoding bucket_sec.
    if db == DB_LEVELED:
        flags = flags + ["--event-time-bucket-width-sec", "86400"]
    print(f"\n{'='*70}")
    print(f"Ablation: {name}")
    print(f"DB: {db}  Flags: {flags}  Rounds: {rounds}")
    print(f"{'='*70}")

    all_results = {w["label"]: [] for w in windows}
    cold_results = {w["label"]: None for w in windows}   # Round 1 (cold cache)
    for rnd in range(1, rounds + 1):
        cold_run = (rnd == 1)
        print(f"Round {rnd}/{rounds}{' [COLD]' if cold_run else ''}: ", end="", flush=True)
        for w in windows:
            m, raw = run_one(BENCH, db, w, flags, env)
            all_results[w["label"]].append(m)
            if cold_run:
                cold_results[w["label"]] = m
            print(f"{w['label']}={m['finalSize']} decomp={m['data_decomp']} ", end="", flush=True)
        print()

    # Summary
    summary = {"name": name, "db": str(db), "flags": flags, "rounds": rounds, "windows": {}}
    all_data_decomp, all_data_decomp_cold = [], []
    all_times, all_times_cold = [], []
    all_idx_exam = []
    for w in windows:
        label = w["label"]
        rs = all_results[label]
        sizes = set(r["finalSize"] for r in rs)
        decomp = [r["data_decomp"] for r in rs]
        times = [r["time_ms"] for r in rs]
        idx = [r["index_examined"] for r in rs]
        idx_skip = [r["index_skipped"] for r in rs]

        all_data_decomp.extend(decomp)
        all_times.extend(times)
        all_idx_exam.extend(idx)
        if cold_results[label]:
            all_data_decomp_cold.append(cold_results[label]["data_decomp"])
            all_times_cold.append(cold_results[label]["time_ms"])

        summary["windows"][label] = {
            "finalSize": list(sizes)[0] if len(sizes) == 1 else list(sizes),
            "correct": len(sizes) == 1,
            "data_decomp_avg": statistics.mean(decomp),
            "data_decomp_cold": cold_results[label]["data_decomp"] if cold_results[label] else 0,
            "data_decomp_trim": trimmed_mean(decomp),
            "time_ms_trim": trimmed_mean(times),
            "time_ms_cold": cold_results[label]["time_ms"] if cold_results[label] else 0,
            "index_examined_avg": statistics.mean(idx),
            "index_skipped_avg": statistics.mean(idx_skip),
        }

    summary["overall"] = {
        "data_decomp_trim": trimmed_mean(all_data_decomp),
        "data_decomp_cold": statistics.mean(all_data_decomp_cold) if all_data_decomp_cold else 0,
        "time_ms_trim": trimmed_mean(all_times),
        "time_ms_cold": statistics.mean(all_times_cold) if all_times_cold else 0,
        "index_examined_avg": statistics.mean(all_idx_exam),
    }
    return summary


# =============================================================================
# Ablation 1: Global Index
# =============================================================================
def ablation_global(windows):
    """Compare different manifest-level filtering strategies."""
    experiments = [
        ("G1_NoGlobal",  ["--prune-mode", "sst"]),
        ("G2_LinearManifest", ["--prune-mode", "sst_manifest", "--manifest-linear-file-skip"]),
        ("G3_TimeBucketRTree", ["--prune-mode", "sst_manifest",
                                "--virtual-merge", "--virtual-merge-auto",
                                "--vm-time-span-sec-threshold", "21600",
                                "--time-bucket-count", "32", "--rtree-leaf-size", "8"]),
    ]
    results = {}
    for name, flags in experiments:
        results[name] = run_experiment(f"Global/{name}", DB_LEVELED, windows, flags)
    return results


# =============================================================================
# Ablation 2: Local Index (block-level truncation)
# =============================================================================
def ablation_local(windows):
    """
    Compare block read count with vs without index block ST truncation.
    """
    experiments = [
        # L1: No truncation — full-scan reads every data block
        ("L1_NoTruncation_FullScan", ["--full-scan-mode", "all_cf", "--no-prune-scan"], {}),
        # L2: Both forward + backward (default)
        ("L2_Both", ["--prune-mode", "sst"], {}),
        # L3: Only forward (no backward bucket_id stop)
        ("L3_ForwardOnly", ["--prune-mode", "sst"],
         {"ST_DISABLE_BACKWARD_TRUNCATION": "1"}),
        # L4: Only backward (no forward block-level skip)
        ("L4_BackwardOnly", ["--prune-mode", "sst"],
         {"ST_DISABLE_FORWARD_TRUNCATION": "1"}),
    ]
    results = {}
    for name, flags, env in experiments:
        results[name] = run_experiment(f"Local/{name}", DB_LEVELED, windows, flags, env=env)
    return results


# =============================================================================
# Ablation 3: ST-Compaction (leveled vs tiered)
# =============================================================================
def ablation_compaction(windows):
    """
    Compare all-leveled (ST compaction) vs all-tiered compaction.
    Uses the same query config (default sst_manifest) on both DBs.
    """
    query_flags = ["--prune-mode", "sst_manifest",
                   "--virtual-merge", "--virtual-merge-auto",
                   "--vm-time-span-sec-threshold", "21600",
                   "--time-bucket-count", "32", "--rtree-leaf-size", "8"]

    experiments = [
        ("C1_AllLeveled_ST", DB_LEVELED),
        ("C2_Native_Leveled", PROJECT / "data" / "runs" / "tdwx_vanilla"),
    ]
    results = {}
    for name, db in experiments:
        if name == "C2_AllTiered" and not db.is_dir():
            print(f"\nSKIP {name}: DB not found at {db} (ingest tiered DB first)")
            continue
        results[name] = run_experiment(f"Compaction/{name}", db, windows, query_flags)
    return results


# =============================================================================
# Ablation 4: Global vs Local vs Both (the key decomposition)
# =============================================================================
def ablation_global_vs_local(windows):
    """
    Compare pure configurations to isolate the contribution of each pruning tier:
      - Only Global: file-level filtering only, no block/key pruning
      - Only Local:  block-level pruning only, no file-level filtering
      - Global+Local: both tiers active (production config)
    """
    vm_flags = ["--virtual-merge", "--virtual-merge-auto",
                "--vm-time-span-sec-threshold", "21600",
                "--time-bucket-count", "32", "--rtree-leaf-size", "8"]

    experiments = [
        ("GL_OnlyGlobal", ["--prune-mode", "manifest"] + vm_flags),
        ("GL_OnlyLocal",  ["--prune-mode", "sst"]),
        ("GL_Both",       ["--prune-mode", "sst_manifest"] + vm_flags),
    ]
    results = {}
    for name, flags in experiments:
        results[name] = run_experiment(f"GlobalVsLocal/{name}", DB_LEVELED, windows, flags)
    return results

# =============================================================================
# Print summary tables
# =============================================================================
def print_table(title: str, experiments: dict, metrics: list[str], window_labels: list[str]):
    print(f"\n{'='*90}")
    print(f"  {title}")
    print(f"{'='*90}")

    header = f"{'Experiment':<28s}"
    for label in window_labels:
        header += f" {label:>12s}"
    header += f" {'WARM(trim)':>12s} {'COLD(R1)':>12s}"
    print(header)
    print("-" * 90)

    for exp_name, summary in experiments.items():
        for metric in metrics:
            row = f"{exp_name}/{metric:<15s}"
            vals = []
            for wl in window_labels:
                v = summary["windows"].get(wl, {}).get(metric, 0)
                vals.append(v)
                row += f" {v:>12.1f}"
            warm = summary["overall"].get(metric, 0)
            cold = summary["overall"].get(metric.replace("_trim", "_cold").replace("_avg", "_cold"), 0)
            row += f" {warm:>12.1f} {cold:>12.1f}"
            print(row)
        print()  # blank line between experiments


def main():
    windows = load_windows()
    window_labels = [w["label"] for w in windows]
    print(f"6 windows loaded, {ROUNDS} rounds per experiment")

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    all_summaries = {}

    # ── Global Index ──
    print("\n" + "=" * 70)
    print("ABLATION 1: Global Index (manifest-level file filtering)")
    print("=" * 70)
    results = ablation_global(windows)
    all_summaries["global"] = results
    print_table("Global Index: Data Decomp & Time", results,
                ["data_decomp_avg", "time_ms_trim"], window_labels)

    # ── Local Index ──
    print("\n" + "=" * 70)
    print("ABLATION 2: Local Index (block-level ST truncation)")
    print("=" * 70)
    results = ablation_local(windows)
    all_summaries["local"] = results
    print_table("Local Index: Data Decomp & Index Examined", results,
                ["data_decomp_avg", "index_examined_avg", "time_ms_trim"], window_labels)

    # ── ST-Compaction ──
    print("\n" + "=" * 70)
    print("ABLATION 3: ST-Compaction (leveled vs tiered)")
    print("=" * 70)
    results = ablation_compaction(windows)
    all_summaries["compaction"] = results
    print_table("ST-Compaction: Data Decomp & Time", results,
                ["data_decomp_avg", "time_ms_trim"], window_labels)

    # ── Global vs Local vs Both ──
    print("\n" + "=" * 70)
    print("ABLATION 4: Global Only vs Local Only vs Global+Local")
    print("=" * 70)
    results = ablation_global_vs_local(windows)
    all_summaries["global_vs_local"] = results
    print_table("Global vs Local Decomposition", results,
                ["data_decomp_avg", "index_examined_avg", "time_ms_trim"], window_labels)

    # Save aggregated results
    out_path = OUT_DIR / f"ablation_{time.strftime('%Y%m%d_%H%M%S')}.json"
    out_path.write_text(json.dumps(all_summaries, indent=2, default=str) + "\n", encoding="utf-8")
    print(f"\nResults saved: {out_path}")

    # Summary table
    print(f"\n{'='*120}")
    print("  ABLATION SUMMARY  (WARM=trimmed mean of all rounds  |  COLD=Round 1 only)")
    print(f"{'='*120}")
    print(f"{'Category':<20s} {'Experiment':<30s} {'DecompW':>10s} {'DecompC':>10s} {'TimeW':>10s} {'TimeC':>10s} {'IdxExam':>10s}")
    print("-" * 100)
    for cat, exps in all_summaries.items():
        for exp_name, s in exps.items():
            o = s["overall"]
            dd = o.get("data_decomp_trim", 0)
            dc = o.get("data_decomp_cold", 0)
            tw = o.get("time_ms_trim", 0)
            tc = o.get("time_ms_cold", 0)
            ie = o.get("index_examined_avg", 0)
            print(f"{cat:<20s} {exp_name:<30s} {dd:>10.0f} {dc:>10.0f} {tw:>9.1f}ms {tc:>9.1f}ms {ie:>10.0f}")


if __name__ == "__main__":
    raise SystemExit(main())
