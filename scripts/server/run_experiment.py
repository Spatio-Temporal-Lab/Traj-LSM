import argparse
import csv
import datetime
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description="Fresh ST-LSM ingestion and window queries")
    parser.add_argument("--data-root", type=Path, default=ROOT.parent / "data")
    parser.add_argument("--dataset", choices=["shared", "td_wx", "processed/smoke", "processed/base", "processed/expanded"], default="processed/base")
    parser.add_argument("--bin-dir", type=Path, default=ROOT / "build-server" / "bin")
    parser.add_argument("--file-count", type=int, default=16,
                        help="Maximum SSTable files allowed in a non-L0 ST level before same-level merge; 0 disables it")
    parser.add_argument("--runs", type=int, choices=[0, 4, 8, 12, 16],
                        help="Compatibility alias for --file-count")
    parser.add_argument("--bucket-sec", type=int, default=3600)
    parser.add_argument("--block-size", type=int, default=32768)
    parser.add_argument("--block-compaction-ratio", type=float, default=0.3)
    parser.add_argument("--trigger-ratio", type=float, default=0.9)
    parser.add_argument("--level-base-mib", type=int, default=1024)
    parser.add_argument("--stfp", action="store_true",
                        help="Compatibility flag for older scripts; current ST-compaction uses file-count and capacity triggers")
    parser.add_argument("--max-rows", type=int)
    parser.add_argument("--max-windows", type=int)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.runs is not None:
        args.file_count = args.runs
    if args.bucket_sec <= 0 or args.block_size <= 0 or args.level_base_mib <= 0 or args.repeats <= 0:
        parser.error("bucket, block, capacity and repeat values must be positive")
    if args.file_count < 0:
        parser.error("file-count must be non-negative")
    if not 0 < args.trigger_ratio <= 1 or not 0 <= args.block_compaction_ratio <= 1:
        parser.error("trigger ratio must be in (0, 1], block ratio in [0, 1]")
    if args.max_windows is not None and args.max_windows <= 0:
        parser.error("max-windows must be positive")
    dataset = args.data_root.resolve() / args.dataset
    metadata = dataset / "segments_meta.csv"
    points = dataset / "segments_points.csv"
    windows = dataset / ("windows6.csv" if args.dataset == "td_wx" else "query_windows.csv")
    for required in [metadata, points]:
        if not required.is_file():
            parser.error(f"Missing input: {required}")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output = (args.output or ROOT / "results" / f"{args.dataset.replace('/', '_')}-F{args.file_count}-{stamp}").resolve()
    if output.exists():
        parser.error(f"Output already exists; choose a fresh directory: {output}")
    suffix = ".exe" if os.name == "nt" else ""
    ingest = args.bin_dir.resolve() / ("st_bucket_ingest_build" + suffix)
    query = args.bin_dir.resolve() / ("st_meta_read_bench" + suffix)
    commands = [[str(ingest), "--segment-meta-csv", str(metadata),
                 "--segment-points-csv", str(points), "--user-key-schema", "e7",
                 "--bucket-sec", str(args.bucket_sec), "--block-size", str(args.block_size),
                 "--experimental-st-compaction", "--st-l1-l2-tiered", "--st-real-tiered",
                 "--st-tiered-max-runs-per-level", str(args.file_count),
                 "--max-level-bytes-mb", str(args.level_base_mib),
                 "--st-set-opt", f"experimental_st_compaction_level_trigger_ratio={args.trigger_ratio}",
                 "--st-set-opt", f"experimental_st_block_compaction_ratio={args.block_compaction_ratio}",
                 "--st-set-opt", f"experimental_st_compaction_stfp_enable={str(args.stfp).lower()}",
                 "--out-sst-dir", str(output / "sst"), "--target-db", str(output / "db")]]
    if args.max_rows is not None:
        if args.max_rows <= 0:
            parser.error("max-rows must be positive")
        commands[0].extend(["--max-rows", str(args.max_rows)])
    if windows.is_file():
        with windows.open(encoding="utf-8-sig", newline="") as source:
            for window_index, window in enumerate(csv.DictReader(source)):
                if args.max_windows is not None and window_index >= args.max_windows:
                    break
                for repeat in range(args.repeats):
                    command = [str(query), "--db", str(output / "db"), "--no-full-scan",
                               "--prune-mode", "sst_manifest", "--event-time-bucket-width-sec", str(args.bucket_sec),
                               "--block-cache-mb", "512", "--iterator-repeat", "1"]
                    for field, option in [("TMin", "t-min"), ("TMax", "t-max"), ("XMin", "x-min"),
                                          ("XMax", "x-max"), ("YMin", "y-min"), ("YMax", "y-max")]:
                        command.extend(["--prune-" + option, window[field]])
                    commands.append(command)
    else:
        print(f"No query windows for {args.dataset}; ingestion only.")
    environment = os.environ.copy()
    environment.update(ROCKSDB_ST_USER_KEY_SCHEMA="e7", ST_DISABLE_FORWARD_TRUNCATION="0", ST_DISABLE_BACKWARD_TRUNCATION="0")
    plan = {"arguments": {name: str(value) if isinstance(value, Path) else value for name, value in vars(args).items()},
            "commands": commands, "dataset": str(dataset), "output": str(output),
            "query_windows": str(windows) if windows.is_file() else None}
    if args.dry_run:
        print(json.dumps(plan, ensure_ascii=False, indent=2))
        return
    for binary in [ingest] + ([query] if len(commands) > 1 else []):
        if not binary.is_file():
            parser.error(f"Build executable first: {binary}")
    output.mkdir(parents=True, exist_ok=False)
    (output / "commands.json").write_text(json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8")
    for index, command in enumerate(commands):
        logfile = output / f"{index:04d}-{'ingest' if index == 0 else 'query'}.log"
        print(f"[{index + 1}/{len(commands)}] {logfile}", flush=True)
        with logfile.open("w", encoding="utf-8") as log:
            subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(f"Completed: {output}")


if __name__ == "__main__":
    main()
