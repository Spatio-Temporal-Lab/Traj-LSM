#!/usr/bin/env python3
"""Run local RocksDB ST prune on TDrive for tdrive_query_st suites (TMan STQuery-aligned).

Fair comparison with TMan STQuery:
  - Same query file: tdrive_query_st/<suite>/query_conditions_st.txt
  - Same batch shape: one process per suite, all queries in one batch (TMan: ';' in arg;
    ST: --query-batch-file, one DB::Open per suite)
  - Same finalSize semantics: point-truth (>=1 point in MBR x [tStartMs,tEndMs])

Usage:
  python tools/run_tdrive_st_query_bench.py --suites uniform_test,skewed_test
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_BENCH = PROJECT / "rocks-demo" / "build" / "st_meta_read_bench.exe"
DEFAULT_DB = PROJECT / "data" / "runs" / "TDrive"
DEFAULT_QUERY_ROOT = PROJECT / "data" / "processed" / "tman_tdrive" / "tdrive_query_st"
DEFAULT_OUT = PROJECT / "Result" / "TSV" / "tdrive_st_query"
DEFAULT_TMAN = PROJECT / "Result" / "tman_tdrive_st"

METRICS = ("time", "finalSize", "candidates", "indexRanges", "quadCodeRanges")


def st_manifest_shard_env(bucket_sec: int = 3600) -> dict[str, str]:
    """Legacy per-bucket MANIFEST-b routing (deprecated ingest path)."""
    env = os.environ.copy()
    env["ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE"] = "1"
    env["ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC"] = str(bucket_sec)
    env["ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_MAX"] = "8192"
    return env


def st_fork_query_env(*, tman_st_key: bool = True) -> dict[str, str]:
    """Match unified ST fork ingest: single MANIFEST + optional TMan ST user keys."""
    env = os.environ.copy()
    for key in (
        "ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE",
        "ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC",
        "ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_MAX",
    ):
        env.pop(key, None)
    if tman_st_key:
        env["ROCKSDB_ST_USER_KEY_SCHEMA"] = "tman_st"
    return env


def percentile(vals: list[float], p: float) -> float:
    if not vals:
        return 0.0
    xs = sorted(vals)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * (p / 100.0)
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    if lo == hi:
        return xs[lo]
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def summarize(vals: list[float]) -> dict[str, float]:
    if not vals:
        z = 0.0
        return dict.fromkeys(("min", "max", "avg", "mid", "per70", "per80", "per90"), z)
    return {
        "min": min(vals),
        "max": max(vals),
        "avg": sum(vals) / len(vals),
        "mid": percentile(vals, 50),
        "per70": percentile(vals, 70),
        "per80": percentile(vals, 80),
        "per90": percentile(vals, 90),
    }


def write_tman_style_csv(path: Path, stats: dict[str, dict[str, float]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["type", "min", "max", "avg", "mid", "per70", "per80", "per90"])
        for metric in METRICS:
            s = stats.get(metric)
            if not s:
                continue
            w.writerow(
                [
                    metric,
                    int(round(s["min"])),
                    int(round(s["max"])),
                    int(round(s["avg"])),
                    int(round(s["mid"])),
                    int(round(s["per70"])),
                    int(round(s["per80"])),
                    int(round(s["per90"])),
                ]
            )


def _load_suite_manifest(suite_dir: Path) -> list[dict]:
    manifest_path = suite_dir / "queries_manifest_st.json"
    if not manifest_path.is_file():
        return []
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    if isinstance(data, dict):
        data = data.get("queries", [])
    return [r for r in data if isinstance(r, dict)]


def _sec_for_ms(ts_ms: int) -> int:
    return int(ts_ms // 1000)


def run_batch_suite(
    bench: Path,
    db: Path,
    suite_dir: Path,
    *,
    prune_mode: str,
    time_bucket_count: int,
) -> tuple[dict[str, dict[str, float]], list[dict], str]:
    qfile = suite_dir / "query_conditions_st.txt"
    if not qfile.is_file():
        raise FileNotFoundError(qfile)
    manifest_rows = _load_suite_manifest(suite_dir)
    suite_t_min_ms = min(int(r.get("t_min_ms", 0)) for r in manifest_rows) if manifest_rows else 0
    suite_t_max_ms = max(int(r.get("t_max_ms", 0)) for r in manifest_rows) if manifest_rows else 0
    suite_t_min_sec = _sec_for_ms(suite_t_min_ms)
    suite_t_max_sec = _sec_for_ms(suite_t_max_ms)
    cmd = [
        str(bench),
        "--db",
        str(db),
        "--no-full-scan",
        "--verify-kv-results",
        "--query-batch-file",
        str(qfile),
        "--prune-mode",
        prune_mode,
        "--prune-t-min",
        str(suite_t_min_sec),
        "--prune-t-max",
        str(suite_t_max_sec),
        "--time-bucket-count",
        str(time_bucket_count),
        "--rtree-leaf-size",
        "8",
        "--virtual-merge",
        "--virtual-merge-auto",
        "--vm-time-span-sec-threshold",
        "21600",
    ]
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=st_fork_query_env(tman_st_key=True),
    )
    raw = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"batch bench exit {proc.returncode}\n{raw[-4000:]}")
    raw = raw.replace("envelope_t=[0,0]", f"envelope_t=[{suite_t_min_sec},{suite_t_max_sec}]")

    # Parse total blocks line: "db_total_blocks sst_count=XX total_num_entries=YY total_compressed_bytes=ZZ"
    db_total_sst = 0
    db_total_entries = 0
    db_total_compressed_bytes = 0
    m_db_blocks = re.search(
        r"db_total_blocks\s+sst_count=(\d+)\s+total_num_entries=(\d+)\s+total_compressed_bytes=(\d+)",
        raw,
    )
    if m_db_blocks:
        db_total_sst = int(m_db_blocks.group(1))
        db_total_entries = int(m_db_blocks.group(2))
        db_total_compressed_bytes = int(m_db_blocks.group(3))

    per_query_by_id: dict[int, dict] = {}
    patterns = [
        re.compile(
            r"spatial_batch_query\s+id=(\d+)\s+time_ms=([\d.eE+-]+)\s+finalSize=(\d+)\s+"
            r"keys_iterated=(\d+)\s+keys_in_window=(\d+)\s+block_read_count=(\d+)\s+"
            r"bytes_read=(\d+)\s+read_nanos=(\d+)\s+block_index_examined=(\d+)\s+"
            r"block_index_skipped=(\d+)\s+file_considered=(\d+)\s+files_skipped=(\d+)",
            re.I,
        ),
        # Fallback: older format without the extra fields
        re.compile(r"spatial_batch_query id=(\d+) time_ms=([\d.eE+-]+) finalSize=(\d+)", re.I),
    ]
    for line in raw.splitlines():
        s = line.strip()
        extended = patterns[0].search(s)
        if extended:
            qid = int(extended.group(1))
            per_query_by_id[qid] = {
                "query_id": qid,
                "time": float(extended.group(2)),
                "finalSize": int(extended.group(3)),
                "keys_iterated": int(extended.group(4)),
                "keys_in_window": int(extended.group(5)),
                "block_read_count": int(extended.group(6)),
                "bytes_read": int(extended.group(7)),
                "read_nanos": int(extended.group(8)),
                "block_index_examined": int(extended.group(9)),
                "block_index_skipped": int(extended.group(10)),
                "file_considered": int(extended.group(11)),
                "files_skipped": int(extended.group(12)),
                "raw": s,
            }
            continue
        for pat in patterns[1:]:
            m = pat.search(s)
            if m:
                qid, time_ms, final_size = int(m.group(1)), float(m.group(2)), int(m.group(3))
                per_query_by_id[qid] = {"query_id": qid, "time": time_ms, "finalSize": final_size, "raw": s}
                break

    per_query: list[dict] = []
    for row in manifest_rows:
        qid = int(row.get("id", len(per_query)))
        rec = dict(row)
        rec.update(per_query_by_id.get(qid, {"query_id": qid, "time": None, "finalSize": None}))
        rec["bench_returned"] = qid in per_query_by_id
        per_query.append(rec)

    stats: dict[str, dict[str, float]] = {}
    for metric in ("time", "finalSize", "block_read_count"):
        vals = [float(r[metric]) for r in per_query if isinstance(r.get(metric), (int, float))]
        if vals:
            stats[metric] = summarize(vals)

    # DB-level totals
    stats["_db_total"] = {
        "sst_count": float(db_total_sst),
        "total_num_entries": float(db_total_entries),
        "total_compressed_bytes": float(db_total_compressed_bytes),
    }

    return stats, per_query, raw


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bench", type=Path, default=DEFAULT_BENCH)
    ap.add_argument("--db", type=Path, default=DEFAULT_DB)
    ap.add_argument("--query-root", type=Path, default=DEFAULT_QUERY_ROOT)
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    ap.add_argument(
        "--suites",
        default="uniform_test,gaussian_test,skewed_test",
    )
    ap.add_argument("--prune-mode", default="sst_manifest")
    ap.add_argument("--time-bucket-count", type=int, default=32)
    ap.add_argument("--max-queries", type=int, default=0, help="0 = all")
    args = ap.parse_args()

    if not args.bench.is_file():
        print(f"ERROR: missing {args.bench} (cmake --build rocks-demo/build)")
        return 1
    if not args.db.is_dir():
        print(f"ERROR: missing DB {args.db}")
        return 1

    args.out_dir.mkdir(parents=True, exist_ok=True)
    suites = [s.strip() for s in args.suites.split(",") if s.strip()]

    for suite in suites:
        sdir = args.query_root / suite
        if not sdir.is_dir():
            print(f"skip missing suite {sdir}")
            continue
        if args.max_queries > 0:
            manifest = json.loads(
                (sdir / "queries_manifest_st.json").read_text(encoding="utf-8")
            )
            manifest = manifest[: args.max_queries]
            trimmed = sdir / "_trimmed_query_conditions_st.txt"
            parts = [
                f"{r['xmin']:.6f},{r['ymin']:.6f},{r['xmax']:.6f},{r['ymax']:.6f},"
                f"{r['t_min_ms']},{r['t_max_ms']}"
                for r in manifest
            ]
            trimmed.write_text(";".join(parts) + "\n", encoding="utf-8")
            qdir = sdir
            backup = sdir / "query_conditions_st.full.txt"
            if not backup.is_file():
                shutil.copy2(sdir / "query_conditions_st.txt", backup)
            shutil.copy2(trimmed, sdir / "query_conditions_st.txt")

        print(f"\n=== ST bench suite={suite} ===")
        stats, per_query, raw = run_batch_suite(
            args.bench,
            args.db,
            sdir,
            prune_mode=args.prune_mode,
            time_bucket_count=args.time_bucket_count,
        )
        out_csv = args.out_dir / f"st_prune_{suite}.csv"
        write_tman_style_csv(out_csv, stats)
        (args.out_dir / f"st_prune_{suite}_per_query.json").write_text(
            json.dumps(per_query, indent=2) + "\n", encoding="utf-8"
        )
        (args.out_dir / f"st_prune_{suite}.log").write_text(raw + "\n" + json.dumps(per_query, indent=2) + "\n", encoding="utf-8")
        print(f"  -> {out_csv}")
        if stats.get("time"):
            print(f"  time avg={stats['time']['avg']:.1f} ms")
        if stats.get("finalSize"):
            print(f"  finalSize avg={stats['finalSize']['avg']:.1f}")

    manifest = {
        "engine": "rocksdb_st_meta_read_bench",
        "batch_mode": "query-batch-file (one DB::Open per suite)",
        "query_file": "query_conditions_st.txt (6 fields, ';' separated)",
        "finalSize_semantics": "point-truth segment KV (--verify-kv-results)",
        "prune_mode": args.prune_mode,
        "time_bucket_count": args.time_bucket_count,
        "db": str(args.db),
        "suites": suites,
        "out_dir": str(args.out_dir),
    }
    (args.out_dir / "st_prune_run_manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    summary_path = args.out_dir / "st_prune_summary.json"
    summary_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"\nDone -> {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
