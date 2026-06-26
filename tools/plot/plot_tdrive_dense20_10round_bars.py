#!/usr/bin/env python3
"""TDrive dense20 10-round: Global / Local / ST-Prune vs TMan (avg + P90 bars)."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_JSON = PROJECT / "Result" / "picture" / "tdrive_dense20_10round_ablation_tman.json"
OUT = PROJECT / "Result" / "picture" / "tdrive_dense20_10round_bars"

SUITES = ("uniform_dense20", "skewed_dense20")
LABELS = {"uniform_dense20": "Uniform dense20", "skewed_dense20": "Skewed dense20"}

# label, json key, color
SYSTEMS = [
    ("Global", "Global-only", "#8DA0CB"),
    ("Local", "Local-only", "#FC8D62"),
    ("ST-Prune", "ST-Prune", "#1B9E77"),
    ("TMan", "TMan", "#4C72B0"),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", type=Path, default=DEFAULT_JSON)
    ap.add_argument("--out", type=Path, default=OUT)
    args = ap.parse_args()

    data = json.loads(args.json.read_text(encoding="utf-8"))
    args.out.parent.mkdir(parents=True, exist_ok=True)

    fig, axes = plt.subplots(1, 2, figsize=(11, 5), sharey=True)
    fig.subplots_adjust(top=0.78, wspace=0.12)
    fig.suptitle(
        "TDrive dense20 · 10 rounds (warmup + run_01–10)\n"
        "Mean per-round avg & P90 (ms) · ST-Prune = st_prune_10round (sst_manifest)",
        fontsize=12,
        y=0.98,
    )

    x = np.arange(2)
    w = 0.18
    offsets = np.linspace(-1.5, 1.5, len(SYSTEMS)) * w

    ymax = 0.0
    for suite in SUITES:
        for _, key, _ in SYSTEMS:
            ymax = max(ymax, data[suite][key]["avg_ms"], data[suite][key]["p90_ms"])

    for ax, suite in zip(axes, SUITES):
        for i, (lab, key, col) in enumerate(SYSTEMS):
            d = data[suite][key]
            vals = [d["avg_ms"], d["p90_ms"]]
            bars = ax.bar(
                x + offsets[i],
                vals,
                width=w,
                label=lab,
                color=col,
                edgecolor="white",
                linewidth=0.8,
            )
            for bar, v in zip(bars, vals):
                ax.text(
                    bar.get_x() + bar.get_width() / 2,
                    v + ymax * 0.02,
                    f"{v:.0f}",
                    ha="center",
                    va="bottom",
                    fontsize=9,
                )
        ax.set_xticks(x)
        ax.set_xticklabels(["Avg", "P90"], fontsize=11)
        ax.set_title(LABELS[suite], fontsize=11)
        ax.set_ylabel("Time (ms)")
        ax.set_ylim(0, ymax * 1.15)
        ax.grid(axis="y", linestyle="--", alpha=0.35)
        ax.set_axisbelow(True)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=4, frameon=False, bbox_to_anchor=(0.5, 0.90))

    for ext in ("png", "svg"):
        p = f"{args.out}.{ext}"
        fig.savefig(p, dpi=150, bbox_inches="tight", pad_inches=0.15)
        print(f"Wrote {p}")
    plt.close(fig)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
