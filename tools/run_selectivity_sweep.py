#!/usr/bin/env python3
"""Selectivity sweep: fixed configs × varying query selectivity (0%–100%)."""

from __future__ import annotations
import csv, json, os, re, statistics, subprocess, sys, time
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
BENCH = PROJECT / "rocks-demo" / "build" / "st_meta_read_bench.exe"
DB = PROJECT / "data" / "runs" / "tdwx_stlsm"
WINDOWS_CSV = PROJECT / "data" / "td_wx" / "windows_selectivity.csv"
OUT_DIR = PROJECT / "Result" / "compare" / "selectivity"

ROUNDS = 5

def load_windows():
    with open(WINDOWS_CSV, encoding="utf-8") as f:
        return [{
            "label": r["Label"],
            "t_min": int(r["TMin"]), "t_max": int(r["TMax"]),
            "x_min": float(r["XMin"]), "x_max": float(r["XMax"]),
            "y_min": float(r["YMin"]), "y_max": float(r["YMax"]),
        } for r in csv.DictReader(f)]

def run_one(bench: Path, db: Path, w: dict, extra_flags: list[str], env: dict = None):
    cmd = [str(bench), "--db", str(db)]
    if "--full-scan-mode" not in extra_flags:
        cmd.append("--no-full-scan")
    cmd.extend(extra_flags)
    for k in ["t_min","t_max","x_min","x_max","y_min","y_max"]:
        cmd += [f"--prune-{k.replace('_','-')}", str(w[k])]
    proc_env = os.environ.copy()
    if env:
        proc_env.update(env)
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", env=proc_env)
    raw = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"bench exit {proc.returncode}\n{raw[-2000:]}")

    m = {}
    # finalSize — try full_scan keys=N, then prune-scan keys_in_window, then keys(last_iter)
    kv = re.search(r"full_scan mode=all_cf keys=(\d+)", raw)
    if not kv:
        kv = re.search(r"keys_in_window\(last_iter\)=(\d+)", raw)
    if not kv:
        kv = re.search(r"keys\(last_iter\)=(\d+)", raw)
    m["finalSize"] = int(kv.group(1)) if kv else 0
    # time — try full_scan wall_us first, then prune wall_us_total
    wm = re.search(r"full_scan.*wall_us=([\d.eE+-]+)", raw)
    if not wm:
        wm = re.search(r"wall_us_total=([\d.eE+-]+)", raw)
    m["time_ms"] = float(wm.group(1))/1000.0 if wm else 0
    # data_blocks_decompressed
    db_re = re.search(r"data_blocks_decompressed=(\d+)", raw)
    m["data_decomp"] = int(db_re.group(1)) if db_re else 0
    # index_examined
    ie = re.search(r"index_examined=(\d+)", raw)
    m["index_examined"] = int(ie.group(1)) if ie else 0
    # file_skipped
    fs = re.search(r"prune_file_skipped=(\d+)", raw)
    m["file_skipped"] = int(fs.group(1)) if fs else 0
    # selectivity % = finalSize / 176686 * 100
    m["selectivity_pct"] = m["finalSize"] / 1766.86
    return m, raw

def trimmed_mean(vals):
    if len(vals) < 3:
        return statistics.mean(vals)
    s = sorted(vals)
    return statistics.mean(s[1:-1])

BASE_FLAGS = ["--event-time-bucket-width-sec", "86400"]
VM_FLAGS = ["--virtual-merge", "--virtual-merge-auto",
            "--vm-time-span-sec-threshold", "21600",
            "--time-bucket-count", "32", "--rtree-leaf-size", "8"]

CONFIGS = [
    ("FullScan",   ["--full-scan-mode", "all_cf", "--no-prune-scan"] + BASE_FLAGS),
    ("OnlyGlobal", ["--prune-mode", "manifest"] + VM_FLAGS + BASE_FLAGS),
    ("OnlyLocal",  ["--prune-mode", "sst"] + BASE_FLAGS),
    ("Both",       ["--prune-mode", "sst_manifest"] + VM_FLAGS + BASE_FLAGS),
]

def run_config(name, flags, windows):
    print(f"\n{'='*60}")
    print(f"  {name}  flags={flags}")
    print(f"{'='*60}")
    results = {w["label"]: [] for w in windows}
    cold = {w["label"]: None for w in windows}
    for rnd in range(1, ROUNDS + 1):
        print(f"  R{rnd}: ", end="", flush=True)
        for w in windows:
            m, _ = run_one(BENCH, DB, w, flags)
            results[w["label"]].append(m)
            if rnd == 1:
                cold[w["label"]] = m
            print(f"{w['label']}={m['finalSize']}({m['selectivity_pct']:.1f}%) ", end="")
        print()
    return results, cold

def main():
    windows = load_windows()
    labels = [w["label"] for w in windows]
    print(f"{len(windows)} selectivity windows, {ROUNDS} rounds each\n")

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    all_data = {}
    for cfg_name, flags in CONFIGS:
        results, cold = run_config(cfg_name, flags, windows)
        all_data[cfg_name] = (results, cold)

    # ── Print table ──
    print(f"\n{'='*110}")
    print("  SELECTIVITY SWEEP SUMMARY (WARM=trimmed R2-R5  |  COLD=Round 1)")
    print(f"{'='*110}")

    metrics = [("data_decomp", "DataDecomp"), ("index_examined", "IdxExam"), ("time_ms", "Time_ms")]

    for met_key, met_name in metrics:
        print(f"\n--- {met_name} ---")
        header = f"{'Config':<16s}"
        for lab in labels:
            header += f" {lab:>12s}"
        header += f" {'WARM(trim)':>12s} {'COLD(R1)':>12s}"
        print(header)
        print("-" * (16 + 14*len(labels) + 24))

        for cfg_name, _ in CONFIGS:
            results, cold = all_data[cfg_name]
            row = f"{cfg_name:<16s}"
            all_vals = []
            all_cold = []
            for w in windows:
                lab = w["label"]
                vals = [r[met_key] for r in results[lab]]
                w_val = trimmed_mean(vals)
                c_val = cold[lab][met_key] if cold[lab] else 0
                all_vals.append(w_val)
                all_cold.append(c_val)
                if met_key == "time_ms":
                    row += f" {w_val:>11.1f}ms"
                else:
                    row += f" {w_val:>12.0f}"
            warm_all = trimmed_mean(all_vals)
            cold_all = statistics.mean(all_cold) if all_cold else 0
            if met_key == "time_ms":
                row += f" {warm_all:>11.1f}ms {cold_all:>11.1f}ms"
            else:
                row += f" {warm_all:>12.0f} {cold_all:>12.0f}"
            print(row)

    # ── Save JSON ──
    out_path = OUT_DIR / f"selectivity_{time.strftime('%Y%m%d_%H%M%S')}.json"
    # Build a serializable summary
    json_out = {}
    for cfg_name, _ in CONFIGS:
        results, cold = all_data[cfg_name]
        cfg_summary = {"windows": {}, "overall_warm": {}, "overall_cold": {}}
        all_warm_decomp, all_warm_time, all_cold_decomp, all_cold_time = [], [], [], []
        for w in windows:
            lab = w["label"]
            vals = results[lab]
            c = cold[lab]
            cfg_summary["windows"][lab] = {
                "selectivity_pct": trimmed_mean([v["selectivity_pct"] for v in vals]),
                "data_decomp_warm": trimmed_mean([v["data_decomp"] for v in vals]),
                "data_decomp_cold": c["data_decomp"] if c else 0,
                "time_ms_warm": trimmed_mean([v["time_ms"] for v in vals]),
                "time_ms_cold": c["time_ms"] if c else 0,
                "index_examined_avg": statistics.mean([v["index_examined"] for v in vals]),
            }
            all_warm_decomp.extend([v["data_decomp"] for v in vals])
            all_warm_time.extend([v["time_ms"] for v in vals])
            if c:
                all_cold_decomp.append(c["data_decomp"])
                all_cold_time.append(c["time_ms"])
        cfg_summary["overall_warm"] = {
            "data_decomp": trimmed_mean(all_warm_decomp),
            "time_ms": trimmed_mean(all_warm_time),
        }
        if all_cold_decomp:
            cfg_summary["overall_cold"] = {
                "data_decomp": statistics.mean(all_cold_decomp),
                "time_ms": statistics.mean(all_cold_time),
            }
        json_out[cfg_name] = cfg_summary
    out_path.write_text(json.dumps(json_out, indent=2, default=str) + "\n", encoding="utf-8")
    print(f"\nSaved: {out_path}")

if __name__ == "__main__":
    raise SystemExit(main())
