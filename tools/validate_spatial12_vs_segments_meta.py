#!/usr/bin/env python3
"""Cross-check spatial12 bench vs segments_meta window counts.

kv_verify only proves full_scan and prune_scan agree on in-window (key,value) hashes.
It does NOT compare to segments_meta SpatialSegments, and trivially passes when both are 0.
"""
from __future__ import annotations

import argparse
import csv
from pathlib import Path


def load_segments(meta_csv: Path) -> list[tuple[int, int, float, float, float, float]]:
    segs: list[tuple[int, int, float, float, float, float]] = []
    with meta_csv.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            segs.append(
                (
                    int(row["t_start_unix"]),
                    int(row["t_end_unix"]),
                    float(row["lon_min"]),
                    float(row["lon_max"]),
                    float(row["lat_min"]),
                    float(row["lat_max"]),
                )
            )
    return segs


def spatial_hit(
    s: tuple[int, int, float, float, float, float],
    x0: float,
    x1: float,
    y0: float,
    y1: float,
) -> bool:
    _ts, _te, lx, hx, ly, hy = s
    return not (lx > x1 or hx < x0 or ly > y1 or hy < y0)


def st_hit(
    s: tuple[int, int, float, float, float, float],
    t0: int,
    t1: int,
    x0: float,
    x1: float,
    y0: float,
    y1: float,
) -> bool:
    ts, te, lx, hx, ly, hy = s
    if te < t0 or ts > t1:
        return False
    return not (lx > x1 or hx < x0 or ly > y1 or hy < y0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--windows",
        type=Path,
        default=Path("tools/st_validity_experiment_windows_wuxi_stratified12_spatial_n4m4w4.csv"),
    )
    ap.add_argument(
        "--meta",
        type=Path,
        default=Path("data/processed/wuxi/segments_meta.csv"),
    )
    ap.add_argument(
        "--bench",
        type=Path,
        default=Path("Result/TSV/st_stream_full_spatial12_bench_verify.tsv"),
    )
    args = ap.parse_args()

    segs = load_segments(args.meta)
    bench_rows: dict[str, dict[str, str]] = {}
    with args.bench.open(encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            bench_rows[row["label"]] = row

    print(
        "label\tcsv_spatial\tmeta_spatial\tmeta_st\tkv\tfull_keys\tprune_k_inw\tkv_ok\tzero_trivial"
    )
    issues = 0
    with args.windows.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            lb = row["Label"]
            x0, x1 = float(row["XMin"]), float(row["XMax"])
            y0, y1 = float(row["YMin"]), float(row["YMax"])
            t0, t1 = int(row["TMin"]), int(row["TMax"])
            csv_sp = int(row["SpatialSegments"])
            meta_sp = sum(1 for s in segs if spatial_hit(s, x0, x1, y0, y1))
            meta_st = sum(1 for s in segs if st_hit(s, t0, t1, x0, x1, y0, y1))
            b = bench_rows.get(lb, {})
            kv = int(b.get("kv_full_inwindow_kv") or 0)
            fk = int(b.get("full_keys") or 0)
            pk = int(b.get("prune_keys_in_window") or 0)
            kv_ok = (b.get("kv_correct") or "") == "OK"
            zero_trivial = kv == 0 and kv_ok
            if csv_sp != meta_sp:
                issues += 1
            print(
                f"{lb}\t{csv_sp}\t{meta_sp}\t{meta_st}\t{kv}\t{fk}\t{pk}\t"
                f"{'OK' if kv_ok else 'FAIL'}\t{'YES' if zero_trivial else 'no'}"
            )

    print(f"\nsegments_meta rows={len(segs)} bench_windows={len(bench_rows)} csv_mismatch={issues}")
    print(
        "\nNote: kv_inwindow_kv counts ST DB (key,value) with point-truth in ST time+MBR window,"
        "\n      not segments_meta segment rows. full_keys is key-envelope filter, not point-truth."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
