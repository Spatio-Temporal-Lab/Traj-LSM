import matplotlib.pyplot as plt
import numpy as np

labels = ["All-Leveled", "All-Tiered(Real)", "Hybrid-L1", "Hybrid-L1+L2",
          "STFP-R4", "STFP-R6", "STFP-Rinf"]

ratios = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

mixed_read = {
    "All-Leveled":      [0, 535.8, 527.2, 527.5, 535.3, 523.2, 529.6, 526.9, 522.9, 578.5, 582.7],
    "All-Tiered(Real)": [0, 669.2, 667.4, 670.7, 663.9, 664.9, 663.2, 666.0, 666.2, 661.9, 659.8],
    "Hybrid-L1":        [0, 665.7, 662.8, 655.6, 661.3, 667.3, 655.8, 657.8, 667.7, 659.9, 658.9],
    "Hybrid-L1+L2":     [0, 675.0, 668.9, 666.2, 667.4, 668.1, 665.9, 671.1, 659.6, 664.8, 668.8],
    "STFP-R4":          [0, 673.0, 650.6, 652.3, 604.0, 587.7, 560.2, 587.9, 528.1, 467.2, 618.0],
    "STFP-R6":          [0, 593.9, 639.8, 634.8, 623.8, 621.9, 623.7, 626.9, 614.5, 626.5, 610.7],
    "STFP-Rinf":        [0, 623.8, 582.4, 635.0, 639.6, 612.5, 633.8, 613.2, 623.8, 619.7, 622.4],
}

mixed_write = {
    "All-Leveled":      [128.6, 144.8, 100.0, 79.2, 67.7, 48.9, 47.2, 40.3, 41.3, 34.3, 0],
    "All-Tiered(Real)": [121.1, 125.9, 90.3, 85.9, 64.2, 50.6, 50.8, 37.8, 44.7, 30.8, 0],
    "Hybrid-L1":        [143.5, 130.1, 98.1, 81.5, 65.5, 50.3, 46.7, 44.9, 37.0, 29.8, 0],
    "Hybrid-L1+L2":     [242.3, 137.4, 96.8, 62.7, 66.0, 58.8, 43.5, 45.8, 35.0, 37.5, 0],
    "STFP-R4":          [186.4, 123.5, 87.5, 82.8, 65.1, 47.2, 45.0, 38.0, 22.4, 17.7, 0],
    "STFP-R6":          [193.5, 129.6, 92.1, 73.8, 60.5, 42.1, 39.9, 42.1, 32.3, 27.9, 0],
    "STFP-Rinf":        [188.2, 104.6, 89.0, 74.4, 67.0, 50.3, 39.5, 39.5, 33.8, 29.1, 0],
}

colors = {
    "All-Leveled":      '#b0bec5',
    "All-Tiered(Real)": '#546e7a',
    "Hybrid-L1":        '#37474f',
    "Hybrid-L1+L2":     '#263238',
    "STFP-R4":          '#ffab91',
    "STFP-R6":          '#ff7043',
    "STFP-Rinf":        '#d84315',
}
linestyles = {
    "All-Leveled":      '--',
    "All-Tiered(Real)": '--',
    "Hybrid-L1":        ':',
    "Hybrid-L1+L2":     ':',
    "STFP-R4":          '-',
    "STFP-R6":          '-.',
    "STFP-Rinf":        (0, (3, 1, 1, 1)),
}
markers = {
    "All-Leveled":      's',   "All-Tiered(Real)": 'D',
    "Hybrid-L1":        '^',   "Hybrid-L1+L2":     'v',
    "STFP-R4":          'o',   "STFP-R6":          's',   "STFP-Rinf": 'D',
}

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(18, 7))
fig.suptitle('ST-Compaction Mixed Workload Throughput vs Read Ratio\n'
             '(200K E7 keys, ΔS=0.01, STFP triggers isolated, pruned query + point truth)',
             fontsize=13, fontweight='bold')

# ── Read ──
for name in labels:
    ax1.plot(ratios[1:], mixed_read[name][1:], color=colors[name],
             linestyle=linestyles[name], marker=markers[name],
             markersize=7, linewidth=2.2, label=name)
ax1.set_xlabel('Read Ratio (%)', fontsize=13)
ax1.set_ylabel('Read Throughput (MB/s)', fontsize=13)
ax1.set_title('Read Throughput', fontsize=14, fontweight='bold')
ax1.grid(True, alpha=0.3)
ax1.set_ylim(400, 720)

# ── Write ──
for name in labels:
    ax2.plot(ratios[:-1], mixed_write[name][:-1], color=colors[name],
             linestyle=linestyles[name], marker=markers[name],
             markersize=7, linewidth=2.2, label=name)
ax2.set_xlabel('Read Ratio (%)', fontsize=13)
ax2.set_ylabel('Write Throughput (MB/s)', fontsize=13)
ax2.set_title('Write Throughput', fontsize=14, fontweight='bold')
ax2.grid(True, alpha=0.3)

handles, labs = ax1.get_legend_handles_labels()
fig.legend(handles, labs, loc='lower center', ncol=7, fontsize=9,
           bbox_to_anchor=(0.5, -0.06))
plt.tight_layout(rect=[0, 0.07, 1, 0.94])
plt.savefig('D:/Project/rocks-demo/stfp_mixed_rw_throughput.png', dpi=150,
            bbox_inches='tight', facecolor='white')
plt.close()
print("Saved: D:/Project/rocks-demo/stfp_mixed_rw_throughput.png")
