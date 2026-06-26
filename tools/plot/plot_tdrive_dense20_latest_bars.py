#!/usr/bin/env python3
"""Plot dense20 bars from latest rerun summaries (TDrive + native_compact + optional TMan)."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

PROJECT = Path(__file__).resolve().parents[1]
SUITES = ("uniform_dense20", "skewed_dense20")
SUITE_TITLES = {
    "uniform_dense20": "Uniform dense20",
    "skewed_dense20": "Skewed dense20",
}

# Chart series order
NATIVE_SERIES = [
    ("Nat·G", "manifest", "#B3CDE3"),
    ("Nat·L", "sst", "#8C96C6"),
    ("Nat·L+G", "sst_manifest", "#88419D"),
]
ST_SERIES = [
    ("Global", "manifest", "#8DA0CB"),
    ("Local", "sst", "#FC8D62"),
    ("ST-Prune", "sst_manifest", "#1B9E77"),
    ("TMan", "tman", "#4C72B0"),
]


def load_summary(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def suite_metrics(report: dict, suite: str, mode: str) -> dict:
    s = report["suites"][suite][mode]
    return {
        "avg_ms": float(s["avg_ms_mean"]),
        "p90_ms": float(s["p90_ms_mean"]),
        "rounds": int(s["n_rounds"]),
        "mismatch": report.get("finalSize_mismatches", 0),
    }


def collect_tman_from_root(tman_root: Path, variant: str = "z_order") -> dict:
    import csv

    out: dict[str, dict[str, dict]] = {s: {} for s in SUITES}
    for suite in SUITES:
        avgs, p90s = [], []
        for run_dir in sorted(tman_root.glob("run_*")):
            p = run_dir / f"{variant}_{suite}.csv"
            if not p.is_file():
                continue
            with p.open(encoding="utf-8", newline="") as f:
                for row in csv.DictReader(f):
                    if row.get("type") == "time":
                        avgs.append(float(row["avg"]))
                        p90s.append(float(row.get("per90") or row.get("max") or 0))
        out[suite] = {
            "avg_ms": statistics.mean(avgs) if avgs else 0,
            "p90_ms": statistics.mean(p90s) if p90s else 0,
            "rounds": len(avgs),
        }
    return out


def build_plot_data(
    native_summary: Path,
    tdrive_summary: Path,
    tman_root: Path | None,
    tman_json: Path | None,
) -> tuple[dict, dict]:
    nat_rep = load_summary(native_summary)
    td_rep = load_summary(tdrive_summary)
    baseline: dict[str, dict[str, dict]] = {s: {} for s in SUITES}
    native: dict[str, dict[str, dict]] = {s: {} for s in SUITES}

    for suite in SUITES:
        for lab, mode, _ in NATIVE_SERIES:
            native[suite][lab] = suite_metrics(nat_rep, suite, mode)
        for lab, mode, _ in ST_SERIES:
            if lab == "TMan":
                if tman_root and tman_root.is_dir():
                    baseline[suite][lab] = collect_tman_from_root(tman_root)[suite]
                elif tman_json and tman_json.is_file():
                    raw = json.loads(tman_json.read_text(encoding="utf-8"))
                    baseline[suite][lab] = {
                        "avg_ms": raw[suite]["TMan"]["avg_ms"],
                        "p90_ms": raw[suite]["TMan"]["p90_ms"],
                        "rounds": raw[suite]["TMan"].get("rounds", 10),
                    }
            else:
                baseline[suite][lab] = suite_metrics(td_rep, suite, mode)
    return baseline, native


def plot_systems_bars(
    baseline: dict,
    native: dict,
    out_base: Path,
    title_note: str,
) -> None:
    labels = [s[0] for s in NATIVE_SERIES] + [s[0] for s in ST_SERIES]
    colors = [s[2] for s in NATIVE_SERIES] + [s[2] for s in ST_SERIES]
    plot_data: dict[str, list[tuple[float, float]]] = {}
    for suite in SUITES:
        plot_data[suite] = []
        for lab, _, _ in NATIVE_SERIES:
            d = native[suite][lab]
            plot_data[suite].append((d["avg_ms"], d["p90_ms"]))
        for lab, _, _ in ST_SERIES:
            d = baseline[suite][lab]
            plot_data[suite].append((d["avg_ms"], d["p90_ms"]))

    out_base.parent.mkdir(parents=True, exist_ok=True)
    sidecar = Path(str(out_base) + ".json")
    sidecar.write_text(
        json.dumps({"baseline": baseline, "native": native, "note": title_note}, indent=2)
        + "\n",
        encoding="utf-8",
    )

    fig, axes = plt.subplots(1, 2, figsize=(14, 5.5), sharey=True)
    fig.subplots_adjust(top=0.76, bottom=0.14, wspace=0.10)
    fig.suptitle(
        "TDrive dense20 · 10 rounds (latest reruns, verify-kv-results)\n"
        f"{title_note}",
        fontsize=9.5,
        y=0.99,
    )
    x_metrics = np.arange(2)
    n = len(labels)
    w = 0.085
    offsets = np.linspace(-(n - 1) / 2, (n - 1) / 2, n) * w
    ymax = max(v for suite in SUITES for v, _ in plot_data[suite])

    for ax, suite in zip(axes, SUITES):
        for i, (lab, col) in enumerate(zip(labels, colors)):
            av, p9 = plot_data[suite][i]
            vals = [av, p9]
            bars = ax.bar(
                x_metrics + offsets[i],
                vals,
                width=w,
                label=lab,
                color=col,
                edgecolor="white",
                linewidth=0.6,
            )
            for bar, v in zip(bars, vals):
                if v > 0:
                    ax.text(
                        bar.get_x() + bar.get_width() / 2,
                        v + ymax * 0.02,
                        f"{v:.0f}",
                        ha="center",
                        va="bottom",
                        fontsize=7 if v < 30 else 8,
                    )
        ax.set_xticks(x_metrics)
        ax.set_xticklabels(["Avg", "P90"], fontsize=11)
        ax.set_title(SUITE_TITLES[suite], fontsize=11)
        ax.set_ylabel("Time (ms)")
        ax.set_ylim(0, ymax * 1.15)
        ax.grid(axis="y", linestyle="--", alpha=0.35)
        ax.set_axisbelow(True)

    fig.legend(
        *axes[0].get_legend_handles_labels(),
        loc="upper center",
        ncol=4,
        frameon=False,
        fontsize=9,
        bbox_to_anchor=(0.5, 0.90),
    )
    for ext in ("png", "svg"):
        fig.savefig(f"{out_base}.{ext}", dpi=150, bbox_inches="tight", pad_inches=0.15)
        print(f"Wrote {out_base}.{ext}")
    plt.close(fig)


def plot_ablation_only(tdrive_summary: Path, out_base: Path) -> None:
    """ST fork only: Global / Local / ST-Prune / TMan."""
    rep = load_summary(tdrive_summary)
    systems = [
        ("Global-only", "manifest", "#8DA0CB"),
        ("Local-only", "sst", "#FC8D62"),
        ("ST-Prune", "sst_manifest", "#1B9E77"),
    ]
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.5))
    x = np.arange(2)
    w = 0.25
    for ax, suite in zip(axes, SUITES):
        for i, (lab, mode, col) in enumerate(systems):
            m = suite_metrics(rep, suite, mode)
            vals = [m["avg_ms"], m["p90_ms"]]
            ax.bar(x + (i - 1) * w, vals, w, label=lab, color=col, alpha=0.9)
        ax.set_xticks(x)
        ax.set_xticklabels(["Avg", "P90"])
        ax.set_title(SUITE_TITLES[suite])
        ax.set_ylabel("ms")
        ax.legend(fontsize=8)
        ax.grid(axis="y", alpha=0.3)
    fig.suptitle("TDrive ST-compaction · latest 10-round rerun", fontsize=10)
    fig.tight_layout()
    for ext in ("png", "svg"):
        fig.savefig(f"{out_base}.{ext}", dpi=150)
    plt.close(fig)
    print(f"Wrote {out_base}.png")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--tdrive-summary",
        type=Path,
        default=PROJECT / "Result/TSV/tdrive_dense20_TDrive_rerun_20260603/summary.json",
    )
    ap.add_argument(
        "--native-summary",
        type=Path,
        default=PROJECT / "Result/TSV/tdrive_dense20_native_compact_rerun_20260603/summary.json",
    )
    ap.add_argument(
        "--tman-root",
        type=Path,
        default=PROJECT / "Result/tman_tdrive_dense20_10round",
    )
    ap.add_argument(
        "--tman-json",
        type=Path,
        default=PROJECT / "Result/picture/tdrive_dense20_10round_ablation_tman.json",
    )
    ap.add_argument(
        "--out-systems",
        type=Path,
        default=PROJECT / "Result/picture/tdrive_dense20_latest_systems_bars",
    )
    ap.add_argument(
        "--out-ablation",
        type=Path,
        default=PROJECT / "Result/picture/tdrive_dense20_latest_ablation_bars",
    )
    args = ap.parse_args()

    if not args.tdrive_summary.is_file():
        print(f"ERROR: missing {args.tdrive_summary}")
        return 1
    if not args.native_summary.is_file():
        print(f"ERROR: missing {args.native_summary} (run native rerun first)")
        return 1

    tman_root = args.tman_root if args.tman_root.is_dir() else None
    baseline, native = build_plot_data(
        args.native_summary,
        args.tdrive_summary,
        tman_root,
        args.tman_json,
    )
    note = (
        "Nat = TDrive_native_compact · ST = TDrive (ST-compaction) · "
        "TMan = z_order 10r · mismatches=0 on ST/Nat reruns"
    )
    plot_systems_bars(baseline, native, args.out_systems, note)
    plot_ablation_only(args.tdrive_summary, args.out_ablation)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
