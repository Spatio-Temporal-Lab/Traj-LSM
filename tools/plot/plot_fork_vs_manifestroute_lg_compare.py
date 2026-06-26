#!/usr/bin/env python3
"""Compare Local+Global (sst_manifest) between two experiment TSVs (per-window bars)."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


def median_naive(xs: list[float]) -> float:
    if not xs:
        return float("nan")
    xs = sorted(xs)
    m = len(xs) // 2
    if len(xs) % 2:
        return xs[m]
    return 0.5 * (xs[m - 1] + xs[m])


def read_tsv(path: Path) -> list[dict]:
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def fnum(s: str) -> float | None:
    s = (s or "").strip()
    if not s:
        return None
    try:
        x = float(s)
        return x if not math.isnan(x) else None
    except ValueError:
        return None


def ratio_wall(row: dict) -> float | None:
    vw = fnum(row.get("vanilla_wall_us", ""))
    pw = fnum(row.get("prune_wall_us", ""))
    if vw is None or pw is None or pw <= 0:
        return None
    return vw / pw


def stratified_window_sort_key(label: str) -> tuple:
    parts = label.strip().replace("wx_strat_", "").split("_")
    if len(parts) != 2:
        return (9, 99)
    grp, idx = parts[0], parts[1]
    g = {"narrow": 0, "medium": 1, "wide": 2}.get(grp, 9)
    try:
        n = int(idx)
    except ValueError:
        n = 99
    return (g, n)


def lg_by_label(rows: list[dict]) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for r in rows:
        if r.get("prune_mode", "").strip() != "sst_manifest":
            continue
        lab = r.get("label", "").strip()
        if lab:
            out[lab] = r
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--fork-tsv",
        type=Path,
        required=True,
        help="ablation_global_local_long.tsv (fork / no offline compact on ablation base)",
    )
    ap.add_argument(
        "--manifest-tsv",
        type=Path,
        required=True,
        help="ablation_global_local_long.tsv (manifest-route STDB)",
    )
    ap.add_argument(
        "--out",
        type=Path,
        required=True,
        help="Output figure path (.png and .svg written with same basename)",
    )
    args = ap.parse_args()

    a = lg_by_label(read_tsv(args.fork_tsv))
    b = lg_by_label(read_tsv(args.manifest_tsv))
    labels = sorted(set(a) & set(b), key=stratified_window_sort_key)

    vals_a = [ratio_wall(a[lb]) for lb in labels]
    vals_b = [ratio_wall(b[lb]) for lb in labels]

    fork_name = args.fork_tsv.parent.name
    man_name = args.manifest_tsv.parent.name
    legend_a = f"Fork, no offline CompactRange\n({fork_name})"
    legend_b = f"manifest-route STDB\n({man_name})"

    import matplotlib.pyplot as plt

    x = range(len(labels))
    xw = 0.36
    fig, ax = plt.subplots(figsize=(12, 5.8))
    ra = [v if v is not None else 0.0 for v in vals_a]
    rb = [v if v is not None else 0.0 for v in vals_b]
    ax.bar([i - xw / 2 for i in x], ra, width=xw, label=legend_a, color="#4472c4")
    ax.bar([i + xw / 2 for i in x], rb, width=xw, label=legend_b, color="#c55a11")
    ax.set_xticks(list(x))
    ax.set_xticklabels(
        [lb.replace("wx_strat_", "") for lb in labels], rotation=38, ha="right"
    )
    ax.set_ylabel("ratio_wall = vanilla_wall_us / prune_wall_us")
    ax.set_title(
        "Local+Global (prune_mode=sst_manifest) per window: "
        "fork ingest copy vs manifest-route STDB (same WindowsCsv rows)"
    )
    ax.legend(loc="upper left", fontsize=8, framealpha=0.92)
    ax.grid(axis="y", alpha=0.3)
    ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=0.85, alpha=0.75)
    med_a = median_naive([v for v in vals_a if v is not None])
    med_b = median_naive([v for v in vals_b if v is not None])
    fig.text(
        0.5,
        0.02,
        f"median ratio_wall: fork run {med_a:.2f}; manifest-route run {med_b:.2f}",
        ha="center",
        fontsize=9,
        color="#333",
    )
    plt.tight_layout(rect=[0, 0.05, 1, 1])
    png = args.out.with_suffix(".png")
    svg = args.out.with_suffix(".svg")
    fig.savefig(png, dpi=170)
    fig.savefig(svg)
    plt.close(fig)

    meta = args.out.with_suffix(".json")
    meta.write_text(
        json.dumps(
            {
                "fork_tsv": str(args.fork_tsv),
                "manifest_tsv": str(args.manifest_tsv),
                "prune_mode": "sst_manifest",
                "metric": "ratio_wall = vanilla_wall_us / prune_wall_us",
                "windows": labels,
                "fork_ratio_wall": dict(zip(labels, vals_a, strict=True)),
                "manifest_route_ratio_wall": dict(zip(labels, vals_b, strict=True)),
                "median_fork": med_a,
                "median_manifest_route": med_b,
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    print(f"Wrote {png}\n{svg}\n{meta}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
