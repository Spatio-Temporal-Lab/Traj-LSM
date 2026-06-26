#!/usr/bin/env python3
"""10-round dense20: ablation (manifest/sst/sst_manifest) + full ST-Prune (sst_manifest).

Does not modify the DB (read-only bench). Uses warmup + run_01..run_10.

Usage:
  python tools/run_tdrive_dense20_10round_bench.py
  python tools/run_tdrive_dense20_10round_bench.py --skip-ablation
  python tools/run_tdrive_dense20_10round_bench.py --skip-st-prune
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
ABLATION_RUNNER = PROJECT / "tools" / "run_tdrive_dense20_ablation.py"
ST_RUNNER = PROJECT / "tools" / "run_tdrive_st_query_bench.py"
DEFAULT_DB = PROJECT / "data" / "runs" / "TDrive"
DEFAULT_QUERY = PROJECT / "data" / "processed" / "tman_tdrive" / "tdrive_query_st"
DEFAULT_BENCH = PROJECT / "rocks-demo" / "build" / "st_meta_read_bench.exe"
ABLATION_ROOT = PROJECT / "Result" / "TSV" / "tdrive_dense20_ablation_10round"
ST_ROOT = PROJECT / "Result" / "TSV" / "tdrive_dense20_st_prune_10round"
SUITES = "uniform_dense20,skewed_dense20"
MODES = "manifest,sst,sst_manifest"


def run_cmd(cmd: list[str], label: str) -> int:
    print(f"\n>>> {label}")
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, cwd=str(PROJECT))
    print(f"    exit={proc.returncode} wall={time.perf_counter() - t0:.1f}s")
    return proc.returncode


def run_ablation_round(out_dir: Path, modes: str, db: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    return run_cmd(
        [
            sys.executable,
            str(ABLATION_RUNNER),
            "--db",
            str(db),
            "--query-root",
            str(DEFAULT_QUERY),
            "--bench",
            str(DEFAULT_BENCH),
            "--out-dir",
            str(out_dir),
            "--suites",
            SUITES,
            "--modes",
            modes,
        ],
        f"ablation -> {out_dir}",
    )


def run_st_round(out_dir: Path, db: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    return run_cmd(
        [
            sys.executable,
            str(ST_RUNNER),
            "--db",
            str(db),
            "--query-root",
            str(DEFAULT_QUERY),
            "--bench",
            str(DEFAULT_BENCH),
            "--out-dir",
            str(out_dir),
            "--suites",
            SUITES,
            "--prune-mode",
            "sst_manifest",
        ],
        f"ST-Prune sst_manifest -> {out_dir}",
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--sleep-s", type=float, default=2.0)
    ap.add_argument("--skip-ablation", action="store_true")
    ap.add_argument("--skip-st-prune", action="store_true")
    ap.add_argument("--db", type=Path, default=DEFAULT_DB)
    ap.add_argument("--ablation-root", type=Path, default=ABLATION_ROOT)
    ap.add_argument("--st-root", type=Path, default=ST_ROOT)
    args = ap.parse_args()

    if not DEFAULT_BENCH.is_file():
        print(f"ERROR: missing {DEFAULT_BENCH}")
        return 1
    if not args.db.is_dir():
        print(f"ERROR: missing DB {args.db}")
        return 1

    meta = {
        "rounds": args.rounds,
        "db": str(args.db),
        "ablation": [],
        "st_prune": [],
    }

    if not args.skip_ablation:
        args.ablation_root.mkdir(parents=True, exist_ok=True)
        rc = run_ablation_round(args.ablation_root / "warmup", MODES, args.db)
        if rc != 0:
            return rc
        for r in range(1, args.rounds + 1):
            rc = run_ablation_round(args.ablation_root / f"run_{r:02d}", MODES, args.db)
            meta["ablation"].append({"round": r, "exit": rc})
            if rc != 0:
                break
            if r < args.rounds and args.sleep_s > 0:
                time.sleep(args.sleep_s)

    if not args.skip_st_prune:
        args.st_root.mkdir(parents=True, exist_ok=True)
        rc = run_st_round(args.st_root / "warmup", args.db)
        if rc != 0:
            return rc
        for r in range(1, args.rounds + 1):
            rc = run_st_round(args.st_root / f"run_{r:02d}", args.db)
            meta["st_prune"].append({"round": r, "exit": rc})
            if rc != 0:
                break
            if r < args.rounds and args.sleep_s > 0:
                time.sleep(args.sleep_s)

    meta_path = args.ablation_root / "bench_10round_meta.json"
    meta_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(f"\nWrote {meta_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
