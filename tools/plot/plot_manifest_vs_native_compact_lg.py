#!/usr/bin/env python3
"""Plot compare_manifest_vs_native_compact_long.tsv (layout_arm + benchmark columns)."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import median as median_stat


def stratified_window_sort_key(label: str) -> tuple:
    lab = label.strip().replace("wx_strat_", "")
    parts = lab.split("_")
    if len(parts) != 2:
        return (9, 99)
    grp, idx = parts[0], parts[1]
    g = {"narrow": 0, "medium": 1, "wide": 2}.get(grp, 9)
    try:
        n = int(idx)
    except ValueError:
        n = 99
    return (g, n)


def read_long(path: Path) -> list[dict]:
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def fnum(s: str) -> float | None:
    s = (s or "").strip()
    if not s or s.lower() == "infinity":
        return None
    try:
        x = float(s)
        return None if math.isnan(x) else x
    except ValueError:
        return None


def ratio_wall(row: dict) -> float | None:
    v = fnum(row.get("ratio_wall", ""))
    if v is not None and math.isfinite(v):
        return v
    vw = fnum(row.get("vanilla_wall_us", ""))
    pw = fnum(row.get("prune_wall_us", ""))
    if vw is None or pw is None or pw <= 0:
        return None
    return vw / pw


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--long-tsv", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True, help="basename for .png/.svg/.json")
    args = ap.parse_args()

    rows = read_long(args.long_tsv)
    by_arm: dict[str, dict[str, dict]] = {"manifest_route": {}, "native_compact": {}}
    for r in rows:
        arm = (r.get("layout_arm") or "").strip()
        if arm not in by_arm:
            continue
        lb = (r.get("label") or "").strip()
        if lb:
            by_arm[arm][lb] = r

    labels = sorted(
        set(by_arm["manifest_route"]) & set(by_arm["native_compact"]),
        key=stratified_window_sort_key,
    )

    vals_a = [ratio_wall(by_arm["manifest_route"][lb]) for lb in labels]
    vals_b = [ratio_wall(by_arm["native_compact"][lb]) for lb in labels]

    import matplotlib.pyplot as plt

    x = range(len(labels))
    xw = 0.36
    fig, ax = plt.subplots(figsize=(12, 5.8))
    ax.bar(
        [i - xw / 2 for i in x],
        [v if v is not None else 0.0 for v in vals_a],
        width=xw,
        label="manifest-route DB (repair_for_vanilla/openable)",
        color="#4472c4",
    )
    ax.bar(
        [i + xw / 2 for i in x],
        [v if v is not None else 0.0 for v in vals_b],
        width=xw,
        label="same DB copy, offline compact (no --st-time-split)",
        color="#c55a11",
    )
    ax.set_xticks(list(x))
    ax.set_xticklabels(
        [lb.replace("wx_strat_", "") for lb in labels], rotation=38, ha="right"
    )
    ax.set_ylabel("ratio_wall (Vanilla baseline: vanilla_wall_us / prune_wall_us)")
    ax.set_title(
        "Local+Global (prune=sst_manifest): manifest-route layout vs native offline compact rewrite"
    )
    ax.legend(loc="upper left", fontsize=8, framealpha=0.93)
    ax.grid(axis="y", alpha=0.3)
    ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=0.85, alpha=0.75)


    def med(xs: list[float | None]) -> float:
        v = [float(t) for t in xs if t is not None]
        return float("nan") if not v else float(median_stat(v))


    ma, mb = med(vals_a), med(vals_b)
    fig.text(
        0.5,
        0.015,
        f"median ratio_wall — manifest-route: {ma:.2f} · native offline compact copy: {mb:.2f}",
        ha="center",
        fontsize=9,
        color="#333",
    )
    plt.tight_layout(rect=[0, 0.06, 1, 1])
    png = args.out.with_suffix(".png")
    svg = args.out.with_suffix(".svg")
    fig.savefig(png, dpi=170)
    fig.savefig(svg)
    plt.close(fig)

    js = args.out.with_suffix(".json")
    js.write_text(
        json.dumps(
            {
                "long_tsv": str(args.long_tsv),
                "metric": "ratio_wall",
                "windows": labels,
                "manifest_route": dict(zip(labels, vals_a, strict=True)),
                "native_offline_compact": dict(zip(labels, vals_b, strict=True)),
                "median_manifest_route": ma,
                "median_native_compact": mb,
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    print(f"Wrote {png}\n{svg}\n{js}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
