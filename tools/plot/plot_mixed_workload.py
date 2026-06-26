#!/usr/bin/env python3
"""Plot final ST 3-way with proper compaction."""

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

CSV = """write_pct,db,write_ops_per_sec,read_ops_per_sec
0,Leveled,0.0,12.4
0,ST,0.0,26.7
0,Tiered,0.0,19.4
10,Leveled,16380.0,12.6
10,ST,25873.2,26.5
10,Tiered,22883.3,19.0
20,Leveled,21586.6,12.6
20,ST,28228.7,21.5
20,Tiered,26195.2,19.6
30,Leveled,19059.7,12.7
30,ST,34443.2,24.8
30,Tiered,26338.9,19.7
40,Leveled,25284.5,12.5
40,ST,43033.9,23.7
40,Tiered,36463.1,19.2
50,Leveled,27762.4,12.7
50,ST,40404.0,24.4
50,Tiered,38595.1,20.2
60,Leveled,36407.8,12.9
60,ST,55918.0,23.1
60,Tiered,51480.1,20.2
70,Leveled,41928.7,12.1
70,ST,48409.4,17.9
70,Tiered,54453.5,19.4
80,Leveled,47253.4,13.1
80,ST,73563.2,21.2
80,Tiered,78508.3,21.3
90,Leveled,87633.9,13.3
90,ST,69364.2,21.8
90,Tiered,88408.6,21.1
100,Leveled,217864.9,0.0
100,ST,252525.3,0.0
100,Tiered,261096.6,0.0"""

COLORS = {"Hybrid": "#2196F3", "Leveled": "#FF9800", "Tiered": "#4CAF50"}
MARKERS = {"Hybrid": "o", "Leveled": "s", "Tiered": "^"}
STYLES = {"Hybrid": "-", "Leveled": "--", "Tiered": "-."}
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
    fig.suptitle("Mixed Workload — ST 3-Way (Proper Compaction)\n"
                 "Hybrid(24SST) vs Leveled(3SST) vs Tiered(24SST) | median of 2 runs",
                 fontsize=13, fontweight="bold")

    for label in labels:
        ax_w.plot(write_pcts, [data[label][p][0] for p in write_pcts],
                  color=COLORS[label], marker=MARKERS[label],
                  linestyle=STYLES[label], linewidth=2, markersize=8, label=label)
    ax_w.set_xlabel("Write %", fontsize=12)
    ax_w.set_ylabel("Write Throughput (ops/s)", fontsize=12)
    ax_w.set_title("Write Throughput", fontsize=13)
    ax_w.legend(fontsize=11, loc="upper left")
    ax_w.grid(True, alpha=0.3); ax_w.set_xticks(write_pcts)
    ax_w.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, _: f"{y/1000:.0f}K"))

    for label in labels:
        ax_r.plot(read_pcts, [data[label][p][1] for p in read_pcts],
                  color=COLORS[label], marker=MARKERS[label],
                  linestyle=STYLES[label], linewidth=2, markersize=8, label=label)
    ax_r.set_xlabel("Write %", fontsize=12)
    ax_r.set_ylabel("Read Throughput (ops/s)", fontsize=12)
    ax_r.set_title("Read Throughput", fontsize=13)
    ax_r.legend(fontsize=11, loc="upper right")
    ax_r.grid(True, alpha=0.3); ax_r.set_xticks(read_pcts)

    fig.text(0.5, -0.01,
             "Hybrid & Tiered: 24 SSTs (time-bucket split) | Leveled: 3 SSTs (max_bytes_for_level_base failed for Leveled style)",
             ha="center", fontsize=9, color="gray", style="italic")

    plt.tight_layout()
    plt.savefig(out, dpi=150, bbox_inches="tight", facecolor="white")
    print(f"Saved: {out}")


if __name__ == "__main__":
    plot(parse(CSV))
