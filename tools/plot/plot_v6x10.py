import matplotlib.pyplot as plt
import numpy as np

labels = ["All-Leveled", "All-Tiered(R4)", "Hybrid-L1(R4)",
          "Hybrid-L1+L2(R4)", "STFP-R4", "STFP-R6", "STFP-Rinf"]

ratios = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

mixed_read = {
    "All-Leveled":       [0, 523.8, 538.7, 549.6, 540.9, 531.5, 543.8, 539.2, 538.4, 542.1, 537.7],
    "All-Tiered(R4)":    [0, 488.6, 525.8, 521.7, 534.7, 500.7, 527.9, 522.4, 540.6, 533.0, 535.1],
    "Hybrid-L1(R4)":     [0, 529.9, 538.9, 544.1, 527.7, 546.2, 547.5, 565.0, 564.9, 568.3, 565.6],
    "Hybrid-L1+L2(R4)":  [0, 498.1, 519.7, 500.9, 544.7, 533.1, 541.5, 541.8, 545.6, 542.4, 534.1],
    "STFP-R4":           [0, 556.0, 577.4, 576.1, 559.5, 566.7, 592.5, 582.6, 591.4, 588.2, 582.6],
    "STFP-R6":           [0, 492.3, 513.3, 507.7, 503.5, 509.3, 522.0, 516.1, 514.6, 511.8, 506.6],
    "STFP-Rinf":         [0, 502.2, 515.5, 543.5, 546.1, 551.8, 566.1, 559.3, 563.2, 559.3, 553.0],
}

mixed_write = {
    "All-Leveled":       [171.8, 128.1, 100.7, 84.6, 68.0, 61.3, 46.9, 41.5, 38.1, 31.2, 0],
    "All-Tiered(R4)":    [141.1, 119.3, 85.8, 66.4, 59.6, 48.5, 45.5, 37.2, 34.3, 26.5, 0],
    "Hybrid-L1(R4)":     [177.8, 124.8, 97.8, 78.5, 58.0, 52.6, 46.4, 41.3, 34.5, 31.9, 0],
    "Hybrid-L1+L2(R4)":  [176.1, 117.7, 89.3, 70.9, 67.2, 54.7, 49.3, 38.4, 36.3, 33.4, 0],
    "STFP-R4":           [194.2, 108.7, 114.6, 83.8, 69.2, 58.5, 56.2, 47.1, 41.1, 33.3, 0],
    "STFP-R6":           [159.3, 125.6, 100.5, 86.7, 74.3, 62.4, 57.2, 48.9, 43.6, 36.0, 0],
    "STFP-Rinf":         [168.6, 113.8, 79.8, 80.0, 62.1, 54.8, 48.2, 43.4, 36.1, 30.2, 0],
}

colors = {
    "All-Leveled":       '#b0bec5',
    "All-Tiered(R4)":    '#1565c0',
    "Hybrid-L1(R4)":     '#1b5e20',
    "Hybrid-L1+L2(R4)":  '#4caf50',
    "STFP-R4":           '#ff7043',
    "STFP-R6":           '#ffab91',
    "STFP-Rinf":         '#d84315',
}
lss = {"All-Leveled": '--', "All-Tiered(R4)": '-', "Hybrid-L1(R4)": '-.',
       "Hybrid-L1+L2(R4)": ':', "STFP-R4": '-', "STFP-R6": '-.', "STFP-Rinf": (0,(3,1,1,1))}
mks = {"All-Leveled": 's', "All-Tiered(R4)": 'D', "Hybrid-L1(R4)": 'o',
       "Hybrid-L1+L2(R4)": 'v', "STFP-R4": '^', "STFP-R6": 's', "STFP-Rinf": 'D'}

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(18, 7))
fig.suptitle('ST-Compaction v6×10 — event_time_split, key=100K, 10 repeats trimmed mean\n'
             '(200K E7 keys, shuffled + Flush, pruned query + point truth)',
             fontsize=13, fontweight='bold')

for name in labels:
    ax1.plot(ratios[1:], mixed_read[name][1:], color=colors[name], ls=lss[name],
             marker=mks[name], markersize=8, linewidth=2.5, label=name)
ax1.set_xlabel('Read Ratio (%)', fontsize=13)
ax1.set_ylabel('Read Throughput (MB/s)', fontsize=13)
ax1.set_title('Read Throughput', fontsize=14, fontweight='bold')
ax1.grid(True, alpha=0.3)

for name in labels:
    ax2.plot(ratios[:-1], mixed_write[name][:-1], color=colors[name], ls=lss[name],
             marker=mks[name], markersize=8, linewidth=2.5, label=name)
ax2.set_xlabel('Read Ratio (%)', fontsize=13)
ax2.set_ylabel('Write Throughput (MB/s)', fontsize=13)
ax2.set_title('Write Throughput', fontsize=14, fontweight='bold')
ax2.grid(True, alpha=0.3)

handles, labs = ax1.get_legend_handles_labels()
fig.legend(handles, labs, loc='lower center', ncol=7, fontsize=9, bbox_to_anchor=(0.5, -0.06))
plt.tight_layout(rect=[0, 0.07, 1, 1])
plt.savefig('D:/Project/rocks-demo/stfp_v6x10.png', dpi=150, bbox_inches='tight', facecolor='white')
plt.close()

# Summary
print("Saved: D:/Project/rocks-demo/stfp_v6x10.png")
print("\n=== Avg Read (10-100%R) ===")
for name in labels:
    avg = np.mean(mixed_read[name][1:])
    print(f"  {name:<22} {avg:.0f}")
print("\n=== Avg Write (0-90%R) ===")
for name in labels:
    avg = np.mean(mixed_write[name][:-1])
    print(f"  {name:<22} {avg:.0f}")
