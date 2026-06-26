#!/usr/bin/env python3
"""Plot 12-spatial query time: ST (wuxi_stream_st_full) vs TMan four variants.

Only latency (ms); no finalSize / index metrics.

Usage:
  python tools/plot_spatial12_query_time_compare.py \\
    --st-bench Result/TSV/st_stream_full_spatial12_bench.tsv \\
    --tman-dir Result/tman_spatial12 \\
    --out Result/picture/spatial12_query_time_compare
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

TMAN_VARIANTS = [
    ("z_order", "z_order", "#4C72B0"),
    ("locs_tsp", "locs_tsp", "#55A868"),
    ("xz_order", "xz_order", "#C44E52"),
    ("xz_star", "xz_star", "#8172B3"),
]


def fnum(s: str) -> float | None:
    s = (s or "").strip()
    if not s:
        return None
    try:
        x = float(s)
        return None if math.isnan(x) else x
    except ValueError:
        return None


def load_st_per_window_ms(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open(encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if (row.get("error") or "").strip():
                continue
            us = fnum(row.get("prune_wall_us", ""))
            if us is None:
                continue
            rows.append(
                {
                    "label": (row.get("label") or "").strip(),
                    "time_ms": us / 1000.0,
                    "kv_correct": (row.get("kv_correct") or "").strip(),
                    "kv_ref_ok": (row.get("kv_ref_ok") or "").strip(),
                }
            )
    return rows


def load_tman_time_stats(csv_path: Path) -> dict[str, float]:
    with csv_path.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if (row.get("type") or "").strip() == "time":
                return {
                    k: fnum(row.get(k, "")) or 0.0
                    for k in ("min", "max", "avg", "mid", "per70", "per80", "per90")
                }
    return {}


def summarize_ms(vals: list[float]) -> dict[str, float]:
    if not vals:
        return {"avg": 0.0, "mid": 0.0, "per90": 0.0, "min": 0.0, "max": 0.0}
    xs = sorted(vals)
    n = len(xs)

    def pct(p: float) -> float:
        if n == 1:
            return xs[0]
        k = (n - 1) * p / 100.0
        lo, hi = int(math.floor(k)), int(math.ceil(k))
        if lo == hi:
            return xs[lo]
        return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)

    return {
        "min": xs[0],
        "max": xs[-1],
        "avg": sum(xs) / n,
        "mid": pct(50),
        "per90": pct(90),
    }


def _bar_value_label(v: float) -> str:
    if v >= 100:
        return f"{v:.0f}"
    if v >= 10:
        return f"{v:.1f}"
    return f"{v:.2f}"


def _annotate_bars(ax, bars, values: list[float], dy_frac: float = 0.012) -> None:
    ymax = ax.get_ylim()[1]
    dy = ymax * dy_frac
    for bar, val in zip(bars, values):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + dy,
            _bar_value_label(val),
            ha="center",
            va="bottom",
            fontsize=8,
            rotation=0,
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--st-bench",
        type=Path,
        default=Path("Result/TSV/st_stream_full_spatial12_bench.tsv"),
    )
    ap.add_argument("--tman-dir", type=Path, default=Path("Result/tman_spatial12"))
    ap.add_argument("--out", type=Path, default=Path("Result/picture/spatial12_query_time_compare"))
    ap.add_argument(
        "--note",
        type=str,
        default="12-spatial; ST prune_wall_us; kv_verify v2 (full↔prune + segments_meta ref); TMan spatial_stats (ms)",
    )
    args = ap.parse_args()

    st_wins = load_st_per_window_ms(args.st_bench)
    st_stats = summarize_ms([r["time_ms"] for r in st_wins])

    tman: dict[str, dict[str, float]] = {}
    for key, label, _ in TMAN_VARIANTS:
        p = args.tman_dir / f"{key}_stratified12_spatial.csv"
        if not p.is_file():
            print(f"warn: missing {p}")
            continue
        tman[label] = load_tman_time_stats(p)

    if not tman:
        raise SystemExit(f"No TMan CSVs under {args.tman_dir}")

    import matplotlib.pyplot as plt

    series_labels = ["ST (prune)"] + [v[1] for v in TMAN_VARIANTS if v[1] in tman]
    avgs = [st_stats["avg"]] + [tman[v[1]]["avg"] for v in TMAN_VARIANTS if v[1] in tman]
    p90s = [st_stats["per90"]] + [tman[v[1]]["per90"] for v in TMAN_VARIANTS if v[1] in tman]
    colors = ["#DD8452"] + [v[2] for v in TMAN_VARIANTS if v[1] in tman]

    # --- Figure 1: avg group | p90 group (all series per metric) ---
    fig, ax = plt.subplots(figsize=(11, 5.5), constrained_layout=True)
    n_series = len(series_labels)
    group_centers = [0, 1]  # avg, p90
    group_names = ["avg", "p90"]
    w = 0.14
    inner_gap = w * 1.05
    offsets = [(i - (n_series - 1) / 2) * inner_gap for i in range(n_series)]

    all_bars = []
    for gi, group_vals in enumerate([avgs, p90s]):
        cx = group_centers[gi]
        for si, (off, val, color, name) in enumerate(
            zip(offsets, group_vals, colors, series_labels)
        ):
            bar = ax.bar(
                cx + off,
                val,
                width=w,
                color=color,
                edgecolor="black",
                linewidth=0.4,
                label=name if gi == 0 else "_nolegend_",
                alpha=0.95 if gi == 0 else 0.55,
                hatch="" if gi == 0 else "//",
            )
            all_bars.append((bar[0], val))

    y_top = max(max(avgs), max(p90s))
    ax.set_ylim(0, y_top * 1.14)
    for bar, val in all_bars:
        ymax = ax.get_ylim()[1]
        dy = ymax * 0.012
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + dy,
            _bar_value_label(val),
            ha="center",
            va="bottom",
            fontsize=7,
        )
    ax.set_xticks(group_centers)
    ax.set_xticklabels(group_names)
    ax.set_ylabel("query time (ms)")
    ax.set_title("12-spatial query latency: ST vs TMan (4 variants)")
    ax.legend(loc="upper left", fontsize=8, ncol=2)
    ax.grid(axis="y", alpha=0.3)
    ax.text(
        0.5,
        -0.22,
        args.note,
        transform=ax.transAxes,
        ha="center",
        fontsize=8,
        color="#444",
        wrap=True,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    for ext in ("png", "svg"):
        fig.savefig(f"{args.out}_avg_p90.{ext}", dpi=150, bbox_inches="tight")
    plt.close(fig)

    # --- Figure 2: per-window ST line + TMan avg reference lines ---
    fig2, ax2 = plt.subplots(figsize=(11, 5), constrained_layout=True)
    xs = list(range(len(st_wins)))
    ax2.plot(
        xs,
        [r["time_ms"] for r in st_wins],
        "o-",
        color="#DD8452",
        linewidth=2,
        markersize=6,
        label=f"ST per-window (avg={st_stats['avg']:.1f} ms)",
    )
    for key, label, color in TMAN_VARIANTS:
        if label not in tman:
            continue
        ax2.axhline(
            tman[label]["avg"],
            color=color,
            linestyle="--",
            linewidth=1.5,
            label=f"TMan {label} avg={tman[label]['avg']:.0f} ms",
        )
    ax2.set_xticks(xs)
    ax2.set_xticklabels(
        [r["label"].replace("wx_strat_", "") for r in st_wins],
        rotation=45,
        ha="right",
        fontsize=8,
    )
    ax2.set_ylabel("query time (ms)")
    ax2.set_title("Per-window ST latency vs TMan variant means (12-spatial)")
    ax2.legend(loc="upper right", fontsize=7)
    ax2.grid(axis="y", alpha=0.3)
    st_times = [r["time_ms"] for r in st_wins]
    y2_top = max(st_times) if st_times else 1.0
    ax2.set_ylim(0, y2_top * 1.18)
    for xi, t in zip(xs, st_times):
        ax2.annotate(
            _bar_value_label(t),
            (xi, t),
            textcoords="offset points",
            xytext=(0, 6),
            ha="center",
            fontsize=7,
            color="#DD8452",
        )
    for ext in ("png", "svg"):
        fig2.savefig(f"{args.out}_per_window.{ext}", dpi=150, bbox_inches="tight")
    plt.close(fig2)

    meta = {
        "st_bench": str(args.st_bench),
        "tman_dir": str(args.tman_dir),
        "st_per_window": st_wins,
        "st_summary": st_stats,
        "tman": tman,
        "note": args.note,
        "verify_on_st_bench": any(r.get("kv_correct") for r in st_wins),
        "kv_ref_ok_all": all(
            (r.get("kv_ref_ok") or r.get("kv_correct") or "") in ("OK", "")
            for r in st_wins
        ),
    }
    args.out.with_suffix(".json").write_text(
        json.dumps(meta, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"Wrote {args.out}_avg_p90.png / _per_window.png")
    print(f"Wrote {args.out.with_suffix('.json')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
