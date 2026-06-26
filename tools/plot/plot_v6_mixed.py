import matplotlib.pyplot as plt
import numpy as np

labels = ["All-Leveled", "All-Tiered(R4)", "Hybrid-L1(R4)",
          "Hybrid-L1+L2(R4)", "STFP-R4", "STFP-R6", "STFP-Rinf"]

ratios = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

mixed_read = {
    "All-Leveled":       [0, 639.5, 608.2, 611.3, 599.4, 636.6, 623.8, 615.9, 638.1, 636.9, 621.4],
    "All-Tiered(R4)":    [0, 554.7, 624.3, 598.6, 604.3, 598.7, 640.2, 607.9, 641.8, 619.8, 644.8],
    "Hybrid-L1(R4)":     [0, 543.6, 590.9, 582.4, 587.1, 570.4, 633.3, 619.1, 626.1, 614.4, 624.7],
    "Hybrid-L1+L2(R4)":  [0, 610.4, 614.0, 643.0, 610.3, 600.4, 640.5, 620.1, 644.0, 638.7, 634.9],
    "STFP-R4":           [0, 546.2, 608.9, 592.7, 586.1, 594.9, 631.8, 626.5, 627.5, 623.0, 616.5],
    "STFP-R6":           [0, 533.5, 621.5, 602.0, 605.1, 597.8, 635.7, 633.8, 607.2, 624.1, 627.5],
    "STFP-Rinf":         [0, 578.5, 580.6, 588.3, 561.5, 606.8, 642.9, 606.7, 641.0, 620.5, 616.0],
}

mixed_write = {
    "All-Leveled":       [175.7, 157.4, 105.4, 80.7, 76.2, 63.9, 62.5, 49.3, 43.6, 37.6, 0],
    "All-Tiered(R4)":    [195.1, 164.9, 107.9, 92.2, 76.0, 59.1, 63.4, 51.7, 44.7, 37.3, 0],
    "Hybrid-L1(R4)":     [221.5, 153.3, 119.4, 72.1, 69.8, 65.9, 64.2, 49.1, 42.1, 33.0, 0],
    "Hybrid-L1+L2(R4)":  [228.9, 130.7, 107.0, 97.3, 80.8, 63.4, 64.5, 44.2, 43.3, 32.9, 0],
    "STFP-R4":           [104.4, 116.1, 111.6, 101.7, 82.5, 69.3, 60.3, 47.9, 41.3, 35.9, 0],
    "STFP-R6":           [170.5, 146.5, 120.3, 98.7, 94.5, 70.6, 61.5, 47.5, 39.4, 38.4, 0],
    "STFP-Rinf":         [137.3, 132.4, 110.2, 99.2, 70.9, 69.1, 61.1, 46.1, 41.3, 31.1, 0],
}

colors = {
    "All-Leveled":       '#b0bec5',
    "All-Tiered(R4)":    '#1565c0',
    "Hybrid-L1(R4)":     '#1b5e20',
    "Hybrid-L1+L2(R4)":  '#4caf50',
    "STFP-R4":           '#ffab91',
    "STFP-R6":           '#ff7043',
    "STFP-Rinf":         '#d84315',
}
lss = {"All-Leveled": '--', "All-Tiered(R4)": '-', "Hybrid-L1(R4)": '-.',
       "Hybrid-L1+L2(R4)": ':', "STFP-R4": '-', "STFP-R6": '-.', "STFP-Rinf": (0,(3,1,1,1))}
mks = {"All-Leveled": 's', "All-Tiered(R4)": 'D', "Hybrid-L1(R4)": 'o',
       "Hybrid-L1+L2(R4)": 'v', "STFP-R4": '^', "STFP-R6": 's', "STFP-Rinf": 'D'}

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(18, 7))
fig.suptitle('ST-Compaction v6 — event_time_split=true, max_point_keys=100K, repeat=3\n'
             '(200K E7 keys, shuffled + Flush, pruned query + point truth)',
             fontsize=13, fontweight='bold')

for name in labels:
    ax1.plot(ratios[1:], mixed_read[name][1:], color=colors[name], ls=lss[name],
             marker=mks[name], markersize=8, linewidth=2.5, label=name)
ax1.set_xlabel('Read Ratio (%)', fontsize=13)
ax1.set_ylabel('Read Throughput (MB/s)', fontsize=13)
ax1.set_title('Read Throughput vs Read Ratio', fontsize=14, fontweight='bold')
ax1.grid(True, alpha=0.3)

for name in labels:
    ax2.plot(ratios[:-1], mixed_write[name][:-1], color=colors[name], ls=lss[name],
             marker=mks[name], markersize=8, linewidth=2.5, label=name)
ax2.set_xlabel('Read Ratio (%)', fontsize=13)
ax2.set_ylabel('Write Throughput (MB/s)', fontsize=13)
ax2.set_title('Write Throughput vs Read Ratio', fontsize=14, fontweight='bold')
ax2.grid(True, alpha=0.3)

handles, labs = ax1.get_legend_handles_labels()
fig.legend(handles, labs, loc='lower center', ncol=7, fontsize=9, bbox_to_anchor=(0.5, -0.06))
plt.tight_layout(rect=[0, 0.07, 1, 1])
plt.savefig('D:/Project/rocks-demo/stfp_v6_mixed.png', dpi=150, bbox_inches='tight', facecolor='white')
plt.close()
print("Saved: D:/Project/rocks-demo/stfp_v6_mixed.png")
