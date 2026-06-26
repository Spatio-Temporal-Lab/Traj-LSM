#!/usr/bin/env python3
"""Plot 5-way mixed workload: Leveled vs Tiered vs Hybrid(L1) vs Hybrid(L1+L2) vs STFR"""

import sys, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from collections import defaultdict

COLORS = {
    "Leveled": "#FF9800",
    "Tiered":  "#4CAF50",
    "Hybrid1": "#2196F3",
    "Hybrid2": "#9C27B0",
    "STFR":    "#F44336",
}
MARKERS = {
    "Leveled": "s",
    "Tiered":  "^",
    "Hybrid1": "o",
    "Hybrid2": "D",
    "STFR":    "*",
}
STYLES = {
    "Leveled": "--",
    "Tiered":  "-.",
    "Hybrid1": "-",
    "Hybrid2": ":",
    "STFR":    "-",
}
LABEL_MAP = {
    "Leveled": "Leveled",
    "Tiered":  "Tiered",
    "Hybrid1": "Hybrid(L1)",
    "Hybrid2": "Hybrid(L1+L2)",
    "STFR":    "STFR",
}
ORDER = ["Leveled", "Tiered", "Hybrid1", "Hybrid2", "STFR"]


def parse_csv(path):
    """Parse CSV lines from a log file: write_pct,db,write_ops_per_sec,read_ops_per_sec"""
    data = defaultdict(lambda: defaultdict(list))
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(",")
            if len(parts) < 4:
                continue
            # Skip header and non-numeric lines
            try:
                pct = int(parts[0])
            except ValueError:
                continue
            db = parts[1].strip()
            try:
                wps = float(parts[2])
                rps = float(parts[3])
            except ValueError:
                continue
            data[db][pct].append((wps, rps))
    # Average across repeats
    avg = {}
    for db in data:
        avg[db] = {}
        for pct in data[db]:
            vals = data[db][pct]
            w_avg = sum(v[0] for v in vals) / len(vals)
            r_avg = sum(v[1] for v in vals) / len(vals)
            avg[db][pct] = (w_avg, r_avg)
    return avg


def plot(data, out="tools/mixed_workload_5way.png"):
    labels = [l for l in ORDER if l in data]
    if not labels:
        print("No data found!")
        return

    pcts = sorted(data[labels[0]].keys())
    write_pcts = [p for p in pcts if 10 <= p <= 100]
    read_pcts = [p for p in pcts if p <= 90]

    fig, (ax_w, ax_r) = plt.subplots(1, 2, figsize=(16, 6.5))
    fig.suptitle("Mixed Workload — 5-Way Compaction Strategy Comparison\n"
                 "Leveled · Tiered · Hybrid(L1) · Hybrid(L1+L2) · STFR | "
                 "L1=10MB, L2=100MB, L3=1000MB",
                 fontsize=13, fontweight="bold")

    for label in labels:
        display = LABEL_MAP.get(label, label)
        xs = write_pcts
        ys = [data[label][p][0] for p in xs]
        ax_w.plot(xs, ys, color=COLORS[label], marker=MARKERS[label],
                  linestyle=STYLES[label], linewidth=2, markersize=8,
                  label=display)
    ax_w.set_xlabel("Write %", fontsize=12)
    ax_w.set_ylabel("Write Throughput (ops/s)", fontsize=12)
    ax_w.set_title("Write Throughput", fontsize=13)
    ax_w.legend(fontsize=10, loc="upper left")
    ax_w.grid(True, alpha=0.3)
    ax_w.set_xticks(write_pcts)
    ax_w.yaxis.set_major_formatter(
        ticker.FuncFormatter(lambda y, _: f"{y/1000:.0f}K"))

    for label in labels:
        display = LABEL_MAP.get(label, label)
        xs = read_pcts
        ys = [data[label][p][1] for p in xs]
        ax_r.plot(xs, ys, color=COLORS[label], marker=MARKERS[label],
                  linestyle=STYLES[label], linewidth=2, markersize=8,
                  label=display)
    ax_r.set_xlabel("Write %", fontsize=12)
    ax_r.set_ylabel("Read Throughput (ops/s)", fontsize=12)
    ax_r.set_title("Read Throughput", fontsize=13)
    ax_r.legend(fontsize=10, loc="upper right")
    ax_r.grid(True, alpha=0.3)
    ax_r.set_xticks(read_pcts)

    plt.tight_layout()
    plt.savefig(out, dpi=150, bbox_inches="tight", facecolor="white")
    print(f"Saved: {out}")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        csv_path = sys.argv[1]
    else:
        csv_path = "D:/tmp/mixed_workload_results.csv"
    if len(sys.argv) > 2:
        out_path = sys.argv[2]
    else:
        out_path = "tools/mixed_workload_5way.png"

    if not os.path.exists(csv_path):
        print(f"CSV not found: {csv_path}")
        sys.exit(1)

    data = parse_csv(csv_path)
    print(f"Loaded: {list(data.keys())}")
    plot(data, out_path)
