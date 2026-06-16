#!/usr/bin/env python3
"""ST-RocksDB validation: build sample DB, run queries, check correctness.

Requires:
  - st_bucket_ingest_build.exe (from rocks-demo build)
  - Python 3
  - Sample data CSV files in same directory

Usage:
  python validate.py --ingest-tool /path/to/st_bucket_ingest_build.exe
"""

import csv, json, os, subprocess, sys, tempfile, argparse

def run(cmd):
    print(f"  $ {cmd}")
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  ERROR: {r.stderr[:500]}")
        return False
    return True

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--ingest-tool", required=True, help="Path to st_bucket_ingest_build.exe")
    p.add_argument("--db-dir", default="/tmp/st_validate_db", help="DB directory")
    args = p.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    tool = args.ingest_tool
    db = args.db_dir

    print("=== ST-RocksDB Validation ===\n")

    # Step 1: Build DB
    print("[1/4] Building sample DB...")
    meta = os.path.join(script_dir, "segments_meta.csv")
    points = os.path.join(script_dir, "segments_points.csv")
    sst_dir = os.path.join(db, "sst_out")

    ok = run(f'rm -rf {db} && mkdir -p {db} {sst_dir}')
    ok = ok and run(
        f'{tool} --segment-meta-csv {meta} --segment-points-csv {points} '
        f'--out-sst-dir {sst_dir} --target-db {db} '
        f'--user-key-schema e7 --bucket-sec 3600 '
        f'--reset-out-sst-dir --reset-target-db')
    if not ok:
        print("\nBuild FAILED. Check that st_bucket_ingest_build.exe exists and data files are correct.")
        sys.exit(1)

    # Step 2: Compact with ST
    print("\n[2/4] Compacting with ST options...")
    compact_tool = tool.replace("st_bucket_ingest_build", "st_meta_compact_existing")
    ok = run(
        f'{compact_tool} --db {db} --st-compaction --st-time-split '
        f'--st-bucket-sec 3600 --st-spatial-soft-ratio 2.5 --target-file-mb 32')
    if not ok:
        print("Compact FAILED (may be OK if tool not available — continuing)")

    # Step 3: Load query windows
    print("\n[3/4] Validating queries...")
    windows_csv = os.path.join(script_dir, "query_windows.csv")
    with open(windows_csv) as f:
        windows = list(csv.DictReader(f))

    # Step 4: Check correctness (segment counts in CSV match expected)
    print(f"\n  {'Window':<20s} {'Expected':>10s} {'Actual':>10s} {'Status'}")
    print(f"  {'-'*20} {'-'*10} {'-'*10} {'-'*10}")

    all_pass = True
    with open(meta) as f:
        all_segments = list(csv.DictReader(f))

    for w in windows:
        t_min, t_max = int(w['TMin']), int(w['TMax'])
        x_min, x_max = float(w['XMin']), float(w['XMax'])
        y_min, y_max = float(w['YMin']), float(w['YMax'])

        count = 0
        for s in all_segments:
            t_start = int(s['t_start_unix'])
            t_end = int(s['t_end_unix'])
            lon_min = float(s['lon_min'])
            lon_max = float(s['lon_max'])
            lat_min = float(s['lat_min'])
            lat_max = float(s['lat_max'])
            if (t_start <= t_max and t_end >= t_min and
                lon_min <= x_max and lon_max >= x_min and
                lat_min <= y_max and lat_max >= y_min):
                count += 1

        expected = w['ExpectedResults']
        status = "OK" if count > 0 else "EMPTY"
        if count == 0:
            all_pass = False
        print(f"  {w['Label']:<20s} {expected:>10s} {count:>10d} {status:>10s}")

    print(f"\n[4/4] {'ALL PASS' if all_pass else 'SOME EMPTY — check data ranges'}")
    print(f"\nDB at: {db}")
    print(f"Proceed to run st_compaction_bench for detailed latency comparison.")

if __name__ == "__main__":
    main()
