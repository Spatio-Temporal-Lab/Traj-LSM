#!/usr/bin/env python3
"""Summarize st_table_props TSV: SST/block/KV distribution + optional level breakdown."""
from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_TSV = PROJECT / "Result" / "TSV" / "tdrive_sst_block_stats.tsv"
DEFAULT_OUT = PROJECT / "Result" / "TSV" / "tdrive_sst_block_stats_summary.json"


def pct(vals: list[float], p: float) -> float:
    if not vals:
        return 0.0
    xs = sorted(vals)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * (p / 100.0)
    lo, hi = int(k // 1), min(int(k // 1) + 1, len(xs) - 1)
    if lo == hi:
        return xs[lo]
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def agg(vals: list[float]) -> dict:
    if not vals:
        return {}
    return {
        "n": len(vals),
        "min": min(vals),
        "max": max(vals),
        "mean": statistics.mean(vals),
        "median": statistics.median(vals),
        "p90": pct(vals, 90),
        "p99": pct(vals, 99),
        "sum": sum(vals),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tsv", type=Path, default=DEFAULT_TSV)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--segment-rows", type=int, default=47102,
                    help="logical segment rows from ingest (for KV/segment ratio)")
    args = ap.parse_args()

    rows = []
    with args.tsv.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if row.get("file") == "TOTAL":
                continue
            rows.append(
                {
                    "file": row["file"],
                    "level": int(row["level"]),
                    "file_bytes": int(row["file_bytes"]),
                    "entries": int(row["entries"]),
                    "data_blocks": int(row["data_blocks"]),
                    "data_bytes": int(row["data_bytes"]),
                    "avg_data_block_bytes": float(row["avg_data_block_bytes"]),
                    "avg_kv_per_block": float(row["avg_kv_per_block"]),
                }
            )

    by_level: dict[int, list] = {}
    for r in rows:
        by_level.setdefault(r["level"], []).append(r)

    def collect(field: str, rs: list) -> list[float]:
        return [float(r[field]) for r in rs]

    report = {
        "n_sst": len(rows),
        "note_entries": (
            "RocksDB num_entries = KV 条数（ST 库中多为 segment 相关 key：master/ghost/points 等），"
            "avg_kv_per_block 可视为「每个 data block 内平均 KV 条数」，不是唯一轨迹 ID 数。"
        ),
        "segment_rows_ingest": args.segment_rows,
        "global": {
            "sst_file_bytes": agg(collect("file_bytes", rows)),
            "kv_entries": agg(collect("entries", rows)),
            "data_blocks": agg(collect("data_blocks", rows)),
            "avg_data_block_bytes": agg(collect("avg_data_block_bytes", rows)),
            "avg_kv_per_block": agg(collect("avg_kv_per_block", rows)),
        },
        "per_level": {},
    }

    total_kv = sum(r["entries"] for r in rows)
    if args.segment_rows > 0:
        report["kv_per_segment_row_ingest"] = total_kv / args.segment_rows

    for lv, rs in sorted(by_level.items()):
        report["per_level"][f"L{lv}"] = {
            "n_sst": len(rs),
            "sst_file_bytes": agg(collect("file_bytes", rs)),
            "kv_entries": agg(collect("entries", rs)),
            "data_blocks": agg(collect("data_blocks", rs)),
            "avg_data_block_bytes": agg(collect("avg_data_block_bytes", rs)),
            "avg_kv_per_block": agg(collect("avg_kv_per_block", rs)),
        }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    g = report["global"]
    print("=== TDrive SST / Block 统计 ===")
    print(f"SST 文件数: {report['n_sst']}")
    print(f"SST 文件大小 (B): mean={g['sst_file_bytes']['mean']:.0f} "
          f"median={g['sst_file_bytes']['median']:.0f} "
          f"min={g['sst_file_bytes']['min']:.0f} max={g['sst_file_bytes']['max']:.0f}")
    print(f"每 SST data block 数: mean={g['data_blocks']['mean']:.1f} "
          f"median={g['data_blocks']['median']:.0f} "
          f"sum={int(g['data_blocks']['sum'])}")
    print(f"平均 data block 大小 (B): mean={g['avg_data_block_bytes']['mean']:.0f} "
          f"median={g['avg_data_block_bytes']['median']:.0f} "
          f"p90={g['avg_data_block_bytes']['p90']:.0f}")
    print(f"每 data block KV 条数: mean={g['avg_kv_per_block']['mean']:.2f} "
          f"median={g['avg_kv_per_block']['median']:.2f} "
          f"p90={g['avg_kv_per_block']['p90']:.2f}")
    print(f"全库 KV 总数: {int(g['kv_entries']['sum'])}")
    if "kv_per_segment_row_ingest" in report:
        print(f"KV / 灌库 segment 行 (47102): {report['kv_per_segment_row_ingest']:.2f}")
    for lv, d in report["per_level"].items():
        print(f"\n--- {lv} (n_sst={d['n_sst']}) ---")
        print(f"  SST size mean={d['sst_file_bytes']['mean']:.0f} B, "
              f"blocks/SST mean={d['data_blocks']['mean']:.1f}, "
              f"avg block={d['avg_data_block_bytes']['mean']:.0f} B, "
              f"KV/block mean={d['avg_kv_per_block']['mean']:.2f}")
    print(f"\nWrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
