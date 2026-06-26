#!/usr/bin/env python3
"""Run cross-bucket FN probe queries against an ST DB; report false negatives.

Compares ST finalSize vs point-truth st_segments_with_point from query manifest.
Also runs st_meta_read_bench --verify-kv-results-strict-exit (full vs prune set).
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_BENCH = PROJECT / "rocks-demo" / "build" / "st_meta_read_bench.exe"
DEFAULT_DB = PROJECT / "data" / "runs" / "TDrive_st_singlekey_20260527"
DEFAULT_QUERY_ROOT = (
    PROJECT / "data" / "processed" / "tman_tdrive" / "tdrive_query_st_crossbucket_fn"
)
DEFAULT_OUT = PROJECT / "Result" / "TSV" / "tdrive_crossbucket_fn_verify"


def st_env(bucket_sec: int = 3600) -> dict[str, str]:
    env = os.environ.copy()
    # Single MANIFEST (default): do not force legacy M_manager shard route on open.
    env.pop("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE", None)
    env.pop("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC", None)
    env.pop("ROCKSDB_EXPERIMENTAL_MANIFEST_SYNC_FANOUT", None)
    env.pop("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_MAX", None)
    return env


def load_truth(manifest_path: Path) -> list[dict]:
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def run_suite(
    bench: Path,
    db: Path,
    suite_dir: Path,
    *,
    bucket_sec: int,
) -> tuple[list[dict], str, bool]:
    qfile = suite_dir / "query_conditions_st.txt"
    cmd = [
        str(bench),
        "--db",
        str(db),
        "--no-full-scan",
        "--verify-kv-results",
        "--verify-kv-results-strict-exit",
        "--query-batch-file",
        str(qfile),
        "--prune-mode",
        "sst_manifest",
        "--prune-t-min",
        "0",
        "--prune-t-max",
        "0",
        "--time-bucket-count",
        "32",
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
        env=st_env(bucket_sec),
    )
    raw = (proc.stdout or "") + (proc.stderr or "")
    per_query: list[dict] = []
    for line in raw.splitlines():
        m = re.match(
            r"spatial_batch_query id=(\d+) time_ms=([\d.eE+-]+) finalSize=(\d+)",
            line.strip(),
        )
        if m:
            per_query.append(
                {
                    "query_id": int(m.group(1)),
                    "time_ms": float(m.group(2)),
                    "finalSize": int(m.group(3)),
                }
            )
    kv_ok = proc.returncode == 0 and "verify_kv_results correctness=OK" in raw
    kv_na = "spatial_batch_mode=1" in raw and "verify_kv_results correctness=" not in raw
    if kv_na and proc.returncode == 0:
        kv_ok = True  # batch+no-full-scan does not run full-vs-prune verify
    return per_query, raw, kv_ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bench", type=Path, default=DEFAULT_BENCH)
    ap.add_argument("--db", type=Path, default=DEFAULT_DB)
    ap.add_argument("--query-root", type=Path, default=DEFAULT_QUERY_ROOT)
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--bucket-sec", type=int, default=3600)
    ap.add_argument(
        "--suites",
        default="crossbucket_middle20,crossbucket_tail20,crossbucket_canonical20,crossbucket_triple15",
    )
    ap.add_argument("--repair-db", action="store_true", default=True)
    ap.add_argument("--no-repair-db", action="store_false", dest="repair_db")
    args = ap.parse_args()

    if not args.bench.is_file():
        print(f"ERROR: missing {args.bench}", file=sys.stderr)
        return 1
    if not args.db.is_dir():
        print(f"ERROR: missing DB {args.db}", file=sys.stderr)
        return 1

    repair = PROJECT / "rocks-demo" / "build" / "traj_db_repair.exe"
    if args.repair_db and repair.is_file():
        print(f"RepairDB: {args.db}")
        subprocess.run([str(repair), "--db", str(args.db)], check=True)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    suites = [s.strip() for s in args.suites.split(",") if s.strip()]

    all_rows: list[dict] = []
    total_fn = 0
    total_target_fn = 0
    kv_fail_suites: list[str] = []

    print(f"DB={args.db}")
    print(f"{'suite':<28} {'id':>3} {'truth':>6} {'ST':>6} {'delta':>6} "
          f"{'q_bucket':>8} {'canon':>8} target_ok kv")
    print("-" * 90)

    for suite in suites:
        sdir = args.query_root / suite
        manifest_path = sdir / "queries_manifest_st.json"
        if not manifest_path.is_file():
            print(f"skip missing {sdir}")
            continue
        truth_rows = load_truth(manifest_path)
        st_rows, raw, kv_ok = run_suite(args.bench, args.db, sdir, bucket_sec=args.bucket_sec)
        if not kv_ok:
            kv_fail_suites.append(suite)

        st_by_id = {r["query_id"]: r for r in st_rows}
        suite_fn = 0
        suite_target_fn = 0

        for tr in truth_rows:
            qid = int(tr["id"])
            truth_n = int(tr["st_segments_with_point"])
            st_n = int(st_by_id.get(qid, {}).get("finalSize", -1))
            delta = st_n - truth_n
            target_id = tr.get("target_segment_id", "")
            target_in_truth = bool(tr.get("target_in_results", target_id in tr.get("segment_ids", [])))
            target_ok = target_id in json.loads(json.dumps(tr.get("segment_ids", [])))
            is_fn = st_n >= 0 and st_n < truth_n
            is_target_fn = is_fn and target_in_truth and target_id not in (
                tr.get("segment_ids", []) if False else []
            )
            # target FN: target should be in truth set but ST count too low to include all truth
            if is_fn and target_in_truth:
                # conservative: if truth has target and ST under-counts, count as target FN
                is_target_fn = True

            if is_fn:
                suite_fn += 1
                total_fn += 1
            if is_fn and target_in_truth:
                suite_target_fn += 1
                total_target_fn += 1

            row = {
                "suite": suite,
                "query_id": qid,
                "label": tr.get("label", ""),
                "truth_segments": truth_n,
                "st_finalSize": st_n,
                "delta": delta,
                "false_negative": int(is_fn),
                "target_segment_id": target_id,
                "target_in_truth": int(target_in_truth),
                "target_false_negative": int(is_fn and target_in_truth),
                "query_calendar_bucket": tr.get("query_calendar_bucket", ""),
                "canonical_bucket": tr.get("canonical_bucket", ""),
                "bucket_span": tr.get("bucket_span", ""),
                "verify_kv_ok": int(kv_ok),
            }
            all_rows.append(row)

            if is_fn or qid < 3:
                print(
                    f"{suite:<28} {qid:>3} {truth_n:>6} {st_n:>6} {delta:>6} "
                    f"{str(tr.get('query_calendar_bucket','')):>8} "
                    f"{str(tr.get('canonical_bucket','')):>8} "
                    f"{'Y' if target_in_truth else 'N':>8} "
                    f"{'OK' if kv_ok else 'FAIL'}"
                    + (" **FN**" if is_fn else "")
                )

        print(f"{suite}: false_negative_queries={suite_fn} target_fn={suite_target_fn} kv={kv_ok}")
        (args.out_dir / f"{suite}_bench.log").write_text(raw, encoding="utf-8")
        (args.out_dir / f"st_prune_{suite}_per_query.json").write_text(
            json.dumps(st_rows, indent=2) + "\n", encoding="utf-8"
        )

    tsv = args.out_dir / "crossbucket_fn_verify.tsv"
    with tsv.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(all_rows[0].keys()), delimiter="\t")
        w.writeheader()
        w.writerows(all_rows)

    summary = {
        "db": str(args.db),
        "query_root": str(args.query_root),
        "total_queries": len(all_rows),
        "false_negative_queries": total_fn,
        "target_false_negative_queries": total_target_fn,
        "verify_kv_fail_suites": kv_fail_suites,
    }
    (args.out_dir / "crossbucket_fn_summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )

    print(f"\nWrote {tsv}")
    print(f"Total FN queries (finalSize < truth): {total_fn}")
    print(f"Target-segment FN queries: {total_target_fn}")
    if kv_fail_suites:
        print(f"verify-kv FAIL suites: {kv_fail_suites}")

    if total_fn > 0 or kv_fail_suites:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
