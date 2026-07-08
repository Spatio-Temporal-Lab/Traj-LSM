#!/usr/bin/env python3
"""Parameter sweep experiments for ST-RocksDB.

Sweep order:
  1. Pyramid key caps (env vars → rebuild rocksdb → reingest)
  2. Bucket width (--bucket-sec → reingest only)
  3. Block size (--block-size → reingest only)
"""

from __future__ import annotations
import csv, json, os, re, shutil, statistics, subprocess, sys, time
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
BENCH = PROJECT / "rocks-demo" / "build" / "st_meta_read_bench.exe"
INGEST = PROJECT / "rocks-demo" / "build" / "st_bucket_ingest_build.exe"
ROCKSDB_BUILD = PROJECT / "rocksdb" / "build"
DEMO_BUILD = PROJECT / "rocks-demo" / "build"
META_CSV = PROJECT / "data" / "td_wx" / "segments_meta.csv"
POINTS_CSV = PROJECT / "data" / "td_wx" / "segments_points.csv"
WINDOWS_CSV = PROJECT / "data" / "td_wx" / "windows_selectivity.csv"
OUT_DIR = PROJECT / "Result" / "paramsweep"
DB_DIR = PROJECT / "data" / "runs" / "tdwx_paramsweep"
SST_DIR = DB_DIR / "_sst_stage"

ROUNDS = 3
VM_FLAGS = ["--virtual-merge", "--virtual-merge-auto",
            "--vm-time-span-sec-threshold", "21600",
            "--time-bucket-count", "32", "--rtree-leaf-size", "8"]


def load_windows():
    with open(WINDOWS_CSV, encoding="utf-8") as f:
        return [{"label": r["Label"], "t_min": int(r["TMin"]), "t_max": int(r["TMax"]),
                 "x_min": float(r["XMin"]), "x_max": float(r["XMax"]),
                 "y_min": float(r["YMin"]), "y_max": float(r["YMax"])}
                for r in csv.DictReader(f)]


def run_cmd(cmd, check=True, env=None, timeout=600, show=False, tee=False):
    """Run a command. show=True: live output. tee=True: live output + capture."""
    proc_env = os.environ.copy()
    if env:
        proc_env.update(env)
    if show or tee:
        print(f"  [{cmd[0]}] {' '.join(cmd[1:6])}...", flush=True)
        if tee:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding="utf-8",
                                    errors="replace", env=proc_env)
            lines = []
            for line in proc.stdout:
                print(f"    {line.rstrip()}", flush=True)
                lines.append(line)
            proc.wait(timeout=timeout)
            out = "".join(lines)
            if check and proc.returncode != 0:
                raise RuntimeError(f"FAIL: {' '.join(cmd)}")
            return proc.returncode, out
        else:
            proc = subprocess.run(cmd, env=proc_env, timeout=timeout)
            if check and proc.returncode != 0:
                raise RuntimeError(f"FAIL: {' '.join(cmd)}")
            return proc.returncode, ""
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", env=proc_env, timeout=timeout)
    if check and proc.returncode != 0:
        raise RuntimeError(f"FAIL: {' '.join(cmd)}\n{proc.stdout[-500:]}{proc.stderr[-500:]}")
    return proc.returncode, proc.stdout + proc.stderr


def build_rocksdb(env=None):
    print("  Building rocksdb...", flush=True)
    _, out = run_cmd(["cmake", "--build", str(ROCKSDB_BUILD), "--config", "Release", "-j", "8"],
                     env=env, show=True)
    ok = "FAILED" not in out and "error:" not in out.lower()
    print("  rocksdb build OK" if ok else "  rocksdb build FAILED", flush=True)
    return ok


def build_demo():
    for exe in DEMO_BUILD.glob("*.exe"):
        exe.unlink()
    print("  Building rocks-demo...", flush=True)
    _, out = run_cmd(["cmake", "--build", str(DEMO_BUILD), "--config", "Release", "-j", "8"],
                     check=False, show=True)
    ok = "FAILED" not in out and "error:" not in out.lower()
    print("  demo build OK" if ok else "  demo build FAILED", flush=True)
    return ok


def ingest_db(bucket_sec=86400, block_size=65536, extra_env=None):
    if DB_DIR.exists():
        shutil.rmtree(DB_DIR, ignore_errors=True)
    cmd = [str(INGEST), "--segment-meta-csv", str(META_CSV),
           "--segment-points-csv", str(POINTS_CSV),
           "--out-sst-dir", str(SST_DIR), "--target-db", str(DB_DIR),
           "--bucket-sec", str(bucket_sec), "--block-size", str(block_size),
           "--user-key-schema", "e7", "--experimental-st-compaction",
           "--reset-out-sst-dir", "--reset-target-db"]
    print("  Ingesting...", flush=True)
    _, out = run_cmd(cmd, env=extra_env, timeout=600, tee=True)
    if "DONE" not in out:
        print(f"    INGEST FAILED: {out[-400:]}")
        return False
    n_entries = re.search(r"total_entries=(\d+)", out)
    n_sst = re.search(r"sst_per_level: L2=(\d+)", out)
    print(f"    Ingest: entries={n_entries.group(1) if n_entries else '?'} sst={n_sst.group(1) if n_sst else '?'}")
    return True


def run_query(w, base_flags):
    cmd = [str(BENCH), "--db", str(DB_DIR)]
    if "--full-scan-mode" not in base_flags:
        cmd.append("--no-full-scan")
    cmd.extend(base_flags)
    for k in ["t_min","t_max","x_min","x_max","y_min","y_max"]:
        cmd += [f"--prune-{k.replace('_','-')}", str(w[k])]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=120)
    except subprocess.TimeoutExpired:
        return None
    raw = proc.stdout + proc.stderr
    m = {}
    kv = re.search(r"full_scan mode=all_cf keys=(\d+)", raw)
    if not kv: kv = re.search(r"keys_in_window\(last_iter\)=(\d+)", raw)
    if not kv: kv = re.search(r"keys\(last_iter\)=(\d+)", raw)
    m["finalSize"] = int(kv.group(1)) if kv else 0
    wm = re.search(r"full_scan.*wall_us=([\d.eE+-]+)", raw)
    if not wm: wm = re.search(r"wall_us_total=([\d.eE+-]+)", raw)
    m["time_ms"] = float(wm.group(1)) / 1000.0 if wm else 0
    db_r = re.search(r"data_blocks_decompressed=(\d+)", raw)
    m["data_decomp"] = int(db_r.group(1)) if db_r else -1
    ie = re.search(r"index_examined=(\d+)", raw)
    m["index_examined"] = int(ie.group(1)) if ie else 0
    return m


def trimmed_mean(vals):
    if len(vals) < 3: return statistics.mean(vals)
    s = sorted(vals)
    return statistics.mean(s[1:-1])


def run_selectivity(bucket_width_sec, windows):
    """Run all configs × all windows × ROUNDS, return summary dict."""
    bw_flag = [f"--event-time-bucket-width-sec", str(bucket_width_sec)]
    configs = [
        ("FullScan",   ["--full-scan-mode", "all_cf", "--no-prune-scan"] + bw_flag),
        ("OnlyGlobal", ["--prune-mode", "manifest"] + VM_FLAGS + bw_flag),
        ("Both",       ["--prune-mode", "sst_manifest"] + VM_FLAGS + bw_flag),
    ]
    results = {}
    for cfg_name, flags in configs:
        print(f"    {cfg_name}...", end=" ", flush=True)
        win_results = {w["label"]: [] for w in windows}
        for rnd in range(1, ROUNDS + 1):
            for w in windows:
                m = run_query(w, flags)
                if m:
                    win_results[w["label"]].append(m)
            print(f"R{rnd} ", end="", flush=True)
        win_sum = {}
        for w in windows:
            lab = w["label"]
            rs = win_results[lab]
            if not rs: continue
            win_sum[lab] = {
                "finalSize": rs[0]["finalSize"],
                "data_decomp_warm": trimmed_mean([r["data_decomp"] for r in rs]),
                "data_decomp_cold": rs[0]["data_decomp"],
                "time_ms_warm": trimmed_mean([r["time_ms"] for r in rs]),
                "time_ms_cold": rs[0]["time_ms"],
                "idx_exam": statistics.mean([r["index_examined"] for r in rs]),
            }
        dd = [v["data_decomp_warm"] for v in win_sum.values() if v]
        tm = [v["time_ms_warm"] for v in win_sum.values() if v]
        dd_c = [v["data_decomp_cold"] for v in win_sum.values() if v]
        tm_c = [v["time_ms_cold"] for v in win_sum.values() if v]
        results[cfg_name] = {
            "windows": win_sum,
            "overall_data_decomp_warm": statistics.mean(dd) if dd else 0,
            "overall_data_decomp_cold": statistics.mean(dd_c) if dd_c else 0,
            "overall_time_ms_warm": statistics.mean(tm) if tm else 0,
            "overall_time_ms_cold": statistics.mean(tm_c) if tm_c else 0,
        }
    return results


def print_sweep_table(title, points, windows):
    labels = [w["label"] for w in windows]
    print(f"\n{'='*120}")
    print(f"  {title}")
    print(f"{'='*120}")

    for metric, mname in [("data_decomp_warm", "DataDecomp"), ("time_ms_warm", "TimeMs")]:
        print(f"\n--- {mname} (warm trimmed) ---")
        hdr = f"{'Point':<30s}"
        for lab in labels:
            hdr += f" {lab:>12s}"
        hdr += f" {'OVERALL':>12s}"
        print(hdr)
        print("-" * (30 + 14 * len(labels) + 12))
        for pt in points:
            r = pt["results"]["Both"]["windows"]
            row = f"{pt['label']:<30s}"
            all_v = []
            for lab in labels:
                v = r.get(lab, {}).get(metric, 0)
                all_v.append(v)
                row += f" {v:>12.0f}" if metric.startswith("data") else f" {v:>11.1f}ms"
            ov = pt["results"]["Both"].get(f"overall_{metric}", 0)
            row += f" {ov:>12.0f}" if metric.startswith("data") else f" {ov:>11.1f}ms"
            print(row)


# ==============================================================================
def sweep_pyramid(windows):
    points = [
        ("P_Tiny",    3000,   5000,  20000),
        ("P_Small",   6000,  10000,  40000),
        ("P_Default", 12000, 20000, 80000),
        ("P_Large",   24000, 40000, 160000),
    ]
    results = []
    for label, l1, l2, l3 in points:
        print(f"\n### Pyramid {label}: L1={l1} L2={l2} L3={l3}")
        env = {"ST_PYRAMID_L1_KEYS": str(l1), "ST_PYRAMID_L2_KEYS": str(l2),
               "ST_PYRAMID_L3_KEYS": str(l3)}
        if not build_rocksdb(env):
            print(f"  SKIP: rocksdb build failed"); continue
        if not build_demo():
            print(f"  SKIP: demo build failed"); continue
        if not ingest_db(86400, 65536, env):
            print(f"  SKIP: ingest failed"); continue
        print("  Running selectivity...")
        res = run_selectivity(86400, windows)
        results.append({"label": label, "env": env, "results": res})
        b = res["Both"]
        print(f"  => Both WARM: decomp={b['overall_data_decomp_warm']:.0f} time={b['overall_time_ms_warm']:.1f}ms"
              f"  COLD: decomp={b['overall_data_decomp_cold']:.0f} time={b['overall_time_ms_cold']:.1f}ms")
    return results


def sweep_bucket(windows):
    points = [("BW_1h", 3600), ("BW_1d", 86400)]
    results = []
    for label, bw in points:
        print(f"\n### Bucket {label}: {bw}s")
        if not ingest_db(bw, 65536):
            print(f"  SKIP"); continue
        print("  Running selectivity...")
        res = run_selectivity(bw, windows)
        results.append({"label": label, "params": {"bucket_sec": bw}, "results": res})
        b = res["Both"]
        print(f"  => Both WARM: decomp={b['overall_data_decomp_warm']:.0f} time={b['overall_time_ms_warm']:.1f}ms"
              f"  COLD: decomp={b['overall_data_decomp_cold']:.0f} time={b['overall_time_ms_cold']:.1f}ms")
    return results


def sweep_blocksize(windows):
    points = [("BS_16K", 16384), ("BS_64K", 65536), ("BS_256K", 262144)]
    results = []
    for label, bs in points:
        print(f"\n### BlockSize {label}: {bs}")
        if not ingest_db(86400, bs):
            print(f"  SKIP"); continue
        print("  Running selectivity...")
        res = run_selectivity(86400, windows)
        results.append({"label": label, "params": {"block_size": bs}, "results": res})
        b = res["Both"]
        print(f"  => Both WARM: decomp={b['overall_data_decomp_warm']:.0f} time={b['overall_time_ms_warm']:.1f}ms"
              f"  COLD: decomp={b['overall_data_decomp_cold']:.0f} time={b['overall_time_ms_cold']:.1f}ms")
    return results


def main():
    print("ST-RocksDB Parameter Sweep\n" + "=" * 60)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    windows = load_windows()
    print(f"{len(windows)} selectivity windows loaded")

    all_data = {}

    # Sweep 1: Pyramid key caps
    print("\n" + "=" * 60)
    print("SWEEP 1: Pyramid Key Caps")
    print("=" * 60)
    pts = sweep_pyramid(windows)
    all_data["pyramid"] = pts
    print_sweep_table("Pyramid Key Caps", pts, windows)

    # Sweep 2: Bucket width
    print("\n" + "=" * 60)
    print("SWEEP 2: Bucket Width")
    print("=" * 60)
    pts = sweep_bucket(windows)
    all_data["bucket"] = pts
    print_sweep_table("Bucket Width", pts, windows)

    # Sweep 3: Block size
    print("\n" + "=" * 60)
    print("SWEEP 3: Block Size")
    print("=" * 60)
    pts = sweep_blocksize(windows)
    all_data["blocksize"] = pts
    print_sweep_table("Block Size", pts, windows)

    # Save
    out_path = OUT_DIR / f"paramsweep_{time.strftime('%Y%m%d_%H%M%S')}.json"
    out_path.write_text(json.dumps(all_data, indent=2, default=str) + "\n", encoding="utf-8")
    print(f"\nSaved: {out_path}")


if __name__ == "__main__":
    raise SystemExit(main())
