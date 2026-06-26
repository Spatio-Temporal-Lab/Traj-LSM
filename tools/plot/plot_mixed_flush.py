import matplotlib.pyplot as plt
import numpy as np

labels = ["All-Leveled", "All-Tiered(Real)", "Hybrid-L1", "Hybrid-L1+L2",
          "STFP-R4", "STFP-R6", "STFP-Rinf"]

ratios = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

mixed_read = {
    "All-Leveled":      [0, 484.7, 446.8, 483.7, 459.8, 445.3, 486.2, 430.1, 365.1, 486.4, 506.3],
    "All-Tiered(Real)": [0, 316.2, 292.9, 327.9, 345.9, 396.7, 451.1, 459.8, 458.4, 472.2, 652.6],
    "Hybrid-L1":        [0, 641.8, 642.3, 642.7, 648.3, 611.0, 637.2, 637.6, 621.2, 637.2, 645.0],
    "Hybrid-L1+L2":     [0, 628.5, 632.0, 629.4, 597.7, 594.0, 577.2, 561.5, 578.0, 559.5, 542.6],
    "STFP-R4":          [0, 636.2, 587.7, 640.4, 650.2, 619.4, 634.7, 625.1, 625.7, 589.6, 603.4],
    "STFP-R6":          [0, 603.7, 519.3, 593.7, 550.7, 595.2, 571.1, 485.5, 527.4, 604.5, 592.5],
    "STFP-Rinf":        [0, 580.6, 549.8, 567.6, 601.9, 591.4, 566.2, 592.4, 560.8, 571.5, 580.5],
}

mixed_write = {
    "All-Leveled":      [179.3, 119.2, 74.2, 63.3, 65.3, 44.2, 48.4, 0.2, 0.2, 43.1, 0],
    "All-Tiered(Real)": [107.6, 72.0, 52.9, 46.6, 49.4, 47.9, 40.1, 36.8, 30.0, 25.5, 0],
    "Hybrid-L1":        [148.1, 136.0, 106.5, 74.7, 66.4, 41.1, 44.7, 36.7, 32.1, 27.3, 0],
    "Hybrid-L1+L2":     [84.5, 132.8, 94.7, 75.2, 51.6, 52.2, 41.9, 31.8, 32.9, 22.0, 0],
    "STFP-R4":          [160.7, 136.7, 95.3, 85.4, 61.2, 55.6, 48.3, 42.8, 35.5, 24.8, 0],
    "STFP-R6":          [183.6, 112.2, 53.5, 56.9, 35.8, 51.0, 32.8, 22.6, 29.7, 32.4, 0],
    "STFP-Rinf":        [108.3, 116.1, 64.6, 60.3, 55.8, 41.0, 38.4, 35.3, 25.9, 21.4, 0],
}

colors = {
    "All-Leveled":      '#b0bec5',
    "All-Tiered(Real)": '#546e7a',
    "Hybrid-L1":        '#1b5e20',
    "Hybrid-L1+L2":     '#4caf50',
    "STFP-R4":          '#ffab91',
    "STFP-R6":          '#ff7043',
    "STFP-Rinf":        '#d84315',
}
linestyles = {
    "All-Leveled":      '--',
    "All-Tiered(Real)": ':',
    "Hybrid-L1":        '-',
    "Hybrid-L1+L2":     '-.',
    "STFP-R4":          '-',
    "STFP-R6":          '-.',
    "STFP-Rinf":        (0, (3, 1, 1, 1)),
}
markers = {
    "All-Leveled":      's',   "All-Tiered(Real)": 'D',
    "Hybrid-L1":        'o',   "Hybrid-L1+L2":     'v',
    "STFP-R4":          '^',   "STFP-R6":          's',   "STFP-Rinf": 'D',
}

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(18, 7))
fig.suptitle('ST-Compaction Mixed Workload (with Flush) — Read/Write vs Read Ratio\n'
             '(200K E7 keys, ΔS=0.01, STFP triggers isolated, pruned query + point truth)',
             fontsize=13, fontweight='bold')

# ── Read ──
for name in labels:
    ax1.plot(ratios[1:], mixed_read[name][1:], color=colors[name],
             linestyle=linestyles[name], marker=markers[name],
             markersize=8, linewidth=2.5, label=name)
ax1.set_xlabel('Read Ratio (%)', fontsize=13)
ax1.set_ylabel('Read Throughput (MB/s)', fontsize=13)
ax1.set_title('Read Throughput', fontsize=14, fontweight='bold')
ax1.grid(True, alpha=0.3)

# ── Write ──
for name in labels:
    ax2.plot(ratios[:-1], mixed_write[name][:-1], color=colors[name],
             linestyle=linestyles[name], marker=markers[name],
             markersize=8, linewidth=2.5, label=name)
ax2.set_xlabel('Read Ratio (%)', fontsize=13)
ax2.set_ylabel('Write Throughput (MB/s)', fontsize=13)
ax2.set_title('Write Throughput', fontsize=14, fontweight='bold')
ax2.grid(True, alpha=0.3)

handles, labs = ax1.get_legend_handles_labels()
fig.legend(handles, labs, loc='lower center', ncol=7, fontsize=9,
           bbox_to_anchor=(0.5, -0.06))
plt.tight_layout(rect=[0, 0.07, 1, 0.94])
plt.savefig('D:/Project/rocks-demo/stfp_mixed_flush.png', dpi=150,
            bbox_inches='tight', facecolor='white')
plt.close()
print("Saved: D:/Project/rocks-demo/stfp_mixed_flush.png")

# Summary
print("\n=== Read Stability (10%R to 90%R delta) ===")
for name in labels:
    r10 = mixed_read[name][1]
    r90 = mixed_read[name][9]
    d = r90 - r10
    avg = np.mean(mixed_read[name][1:])
    print(f"  {name:<20} {r10:.0f} -> {r90:.0f}  Δ={d:+.0f}  avg={avg:.0f}")
