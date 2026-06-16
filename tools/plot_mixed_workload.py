#!/usr/bin/env python3
"""Plot all 3 ST strategies with time-bucket split (fast run: 500ops, repeat=3)."""

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

CSV = """write_pct,db,write_ops_per_sec,read_ops_per_sec
0,Leveled,0.0,25.7
0,ST,0.0,25.3
0,Tiered,0.0,19.6
10,Leveled,28328.6,25.3
10,ST,30248.0,27.1
10,Tiered,28686.2,18.6
20,Leveled,31466.3,25.6
20,ST,36390.1,27.4
20,Tiered,31605.6,19.8
30,Leveled,35385.7,25.4
30,ST,38971.2,27.3
30,Tiered,38769.7,21.3
40,Leveled,41554.1,25.5
40,ST,41186.2,25.4
40,Tiered,47025.6,22.2
50,Leveled,43538.8,25.5
50,ST,45142.7,25.4
50,Tiered,53407.4,22.4
60,Leveled,52900.7,25.3
60,ST,53399.8,25.2
60,Tiered,58777.4,21.0
70,Leveled,58168.5,22.1
70,ST,65359.5,24.9
70,Tiered,73514.0,22.3
80,Leveled,77309.6,23.3
80,ST,81950.4,25.8
80,Tiered,94228.5,22.8
90,Leveled,115236.9,24.5
90,ST,110456.6,25.7
90,Tiered,134892.1,22.4
100,Leveled,186219.7,0.0
100,ST,261233.0,0.0
100,Tiered,226551.9,0.0"""

COLORS = {"ST": "#2196F3", "Leveled": "#FF9800", "Tiered": "#4CAF50"}
MARKERS = {"ST": "o", "Leveled": "s", "Tiered": "^"}
STYLES = {"ST": "-", "Leveled": "--", "Tiered": "-."}
LABEL_MAP = {"ST": "Hybrid", "Leveled": "Leveled", "Tiered": "Tiered"}


def parse(text):
    data = {}
    for line in text.strip().split("\n")[1:]:
        p, db, w, r = line.split(",")
        data.setdefault(LABEL_MAP.get(db.strip(), db.strip()), {})[int(p)] = (float(w), float(r))
    return data


def plot(data, out="tools/mixed_workload_final.png"):
    labels = ["Hybrid", "Leveled", "Tiered"]
    pcts = sorted(data[labels[0]].keys())
    write_pcts = [p for p in pcts if 10 <= p <= 100]
    read_pcts = [p for p in pcts if p <= 90]

    fig, (ax_w, ax_r) = plt.subplots(1, 2, figsize=(14, 6))
    fig.suptitle(
        "Mixed Workload — 3 ST Strategies (all with time-bucket + MBR flush)\n"
        "Hybrid vs Leveled vs Tiered  |  95K segs, 200MB  |  median of 3 runs",
        fontsize=13, fontweight="bold")

    for label in labels:
        ax_w.plot(write_pcts, [data[label][p][0] for p in write_pcts],
                  color=COLORS.get(label, "#999"), marker=MARKERS.get(label, "x"),
                  linestyle=STYLES.get(label, "-"), linewidth=2, markersize=8,
                  label=label)
    ax_w.set_xlabel("Write %", fontsize=12)
    ax_w.set_ylabel("Write Throughput (ops/s)", fontsize=12)
    ax_w.set_title("Write Throughput", fontsize=13)
    ax_w.legend(fontsize=11, loc="upper left")
    ax_w.grid(True, alpha=0.3); ax_w.set_xticks(write_pcts)
    ax_w.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, _: f"{y/1000:.0f}K"))

    for label in labels:
        ax_r.plot(read_pcts, [data[label][p][1] for p in read_pcts],
                  color=COLORS.get(label, "#999"), marker=MARKERS.get(label, "x"),
                  linestyle=STYLES.get(label, "-"), linewidth=2, markersize=8,
                  label=label)
    ax_r.set_xlabel("Write %", fontsize=12)
    ax_r.set_ylabel("Read Throughput (ops/s)", fontsize=12)
    ax_r.set_title("Read Throughput", fontsize=13)
    ax_r.legend(fontsize=11, loc="upper right")
    ax_r.grid(True, alpha=0.3); ax_r.set_xticks(read_pcts)

    fig.text(0.5, -0.01,
             "DB layout: Hybrid=10 L6 SSTs | Leveled=10 L6 SSTs | Tiered=7 L6 SSTs (time-bucket split enabled for all)",
             ha="center", fontsize=9, color="gray", style="italic")

    plt.tight_layout()
    plt.savefig(out, dpi=150, bbox_inches="tight", facecolor="white")
    print(f"Saved: {out}")


if __name__ == "__main__":
    plot(parse(CSV))
