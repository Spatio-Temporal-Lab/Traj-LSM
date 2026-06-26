import matplotlib.pyplot as plt
import numpy as np

labels = ["All-Leveled", "All-Tiered(R4)", "Hybrid-L1(R4)",
          "Hybrid-L1+L2(R4)", "STFP-R4", "STFP-R6", "STFP-Rinf"]

ratios = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

mixed_read = {
    "All-Leveled":       [0, 509.7, 523.1, 527.6, 530.2, 523.3, 529.5, 529.6, 528.9, 532.9, 536.4],
    "All-Tiered(R4)":    [0, 637.4, 642.0, 643.6, 638.4, 648.4, 648.2, 644.3, 643.1, 642.9, 647.2],
    "Hybrid-L1(R4)":     [0, 627.0, 615.5, 580.2, 627.3, 597.7, 619.9, 619.9, 577.8, 584.8, 569.6],
    "Hybrid-L1+L2(R4)":  [0, 603.6, 574.4, 594.8, 574.7, 591.7, 601.1, 571.7, 576.8, 581.3, 577.3],
    "STFP-R4":           [0, 600.5, 569.8, 619.4, 577.9, 587.0, 604.7, 576.6, 590.5, 586.7, 591.6],
    "STFP-R6":           [0, 611.7, 579.8, 579.6, 583.9, 571.4, 579.4, 575.8, 577.4, 567.5, 579.8],
    "STFP-Rinf":         [0, 572.2, 562.8, 599.6, 584.3, 579.4, 620.0, 577.9, 582.8, 590.2, 611.5],
}

mixed_write = {
    "All-Leveled":       [138.7, 137.3, 85.3, 80.0, 66.5, 58.9, 45.0, 38.1, 39.2, 30.6, 0],
    "All-Tiered(R4)":    [158.0, 134.7, 91.0, 77.9, 65.3, 57.0, 45.8, 39.4, 34.9, 32.0, 0],
    "Hybrid-L1(R4)":     [136.0, 124.9, 89.7, 70.7, 63.9, 49.0, 44.3, 40.7, 30.5, 23.8, 0],
    "Hybrid-L1+L2(R4)":  [184.4, 129.7, 88.0, 76.7, 57.1, 47.1, 41.5, 33.4, 32.4, 27.7, 0],
    "STFP-R4":           [156.1, 112.0, 82.3, 65.3, 43.1, 47.0, 46.9, 35.2, 32.4, 27.2, 0],
    "STFP-R6":           [259.7, 125.0, 90.0, 75.2, 54.6, 42.5, 43.8, 36.9, 26.8, 26.3, 0],
    "STFP-Rinf":         [142.3, 115.5, 79.9, 66.0, 57.5, 48.5, 41.1, 34.7, 30.6, 33.6, 0],
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

# Read
for name in labels:
    ax1.plot(ratios[1:], mixed_read[name][1:], color=colors[name], ls=lss[name],
             marker=mks[name], markersize=8, linewidth=2.5, label=name)
ax1.set_xlabel('Read Ratio (%)', fontsize=13)
ax1.set_ylabel('Read Throughput (MB/s)', fontsize=13)
ax1.set_title('Read Throughput vs Read Ratio', fontsize=14, fontweight='bold')
ax1.grid(True, alpha=0.3)

# Write
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
plt.savefig('D:/Project/rocks-demo/stfp_v5_mixed.png', dpi=150, bbox_inches='tight', facecolor='white')
plt.close()
print("Saved: D:/Project/rocks-demo/stfp_v5_mixed.png")
