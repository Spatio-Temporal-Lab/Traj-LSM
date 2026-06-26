import matplotlib.pyplot as plt
import numpy as np

# Short labels
labels = ["All-Leveled", "All-Tiered(Real)", "Hybrid-L1", "Hybrid-L1+L2",
          "STFP-R4", "STFP-R6", "STFP-Rinf"]
xlabels = ["All-\nLeveled", "All-Tiered\n(Real)", "Hybrid\nL1",
           "Hybrid\nL1+L2", "STFP\nR4", "STFP\nR6", "STFP\nRinf"]

write_mbps = [22.7, 24.2, 21.7, 21.1, 21.0, 21.3, 22.4]
read_avg   = [536,  666,  662,  667,  596,  622,  622]

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
    "All-Leveled":      's',
    "All-Tiered(Real)": 'D',
    "Hybrid-L1":        '^',
    "Hybrid-L1+L2":     'v',
    "STFP-R4":          'o',
    "STFP-R6":          's',
    "STFP-Rinf":        'D',
}

x = np.arange(len(labels))
width = 0.28

fig, axes = plt.subplots(2, 2, figsize=(16, 11))
fig.suptitle('ST-Compaction Final Comparison (ΔS=0.01, STFP triggers isolated)\n'
             '200K E7 keys, shuffled writes, pruned query + point truth',
             fontsize=13, fontweight='bold')

# ── (a) Write Throughput ──
ax = axes[0][0]
bar_colors = [colors[n] for n in labels]
bars = ax.bar(x, write_mbps, width, color=bar_colors, edgecolor='white', linewidth=0.8)
ax.set_ylabel('Write Throughput (MB/s)', fontsize=11)
ax.set_title('Pure Write Throughput', fontsize=12, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(xlabels, fontsize=9)
for bar, v in zip(bars, write_mbps):
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.3,
            f'{v:.1f}', ha='center', fontsize=9, fontweight='bold')
best_w = np.argmax(write_mbps)
bars[best_w].set_edgecolor('#1b5e20')
bars[best_w].set_linewidth(2.5)

# ── (b) Read Throughput vs Ratio ──
ax = axes[0][1]
for name in labels:
    ax.plot(ratios, mixed_read[name], color=colors[name], linestyle=linestyles[name],
            marker=markers[name], markersize=6, linewidth=2, label=name)
ax.set_xlabel('Read Ratio (%)', fontsize=11)
ax.set_ylabel('Read Throughput (MB/s)', fontsize=11)
ax.set_title('Read Throughput vs Read Ratio', fontsize=12, fontweight='bold')
ax.grid(True, alpha=0.3)
ax.legend(fontsize=7, ncol=2)

# ── (c) Write Throughput vs Ratio ──
ax = axes[1][0]
for name in labels:
    ax.plot(ratios, mixed_write[name], color=colors[name], linestyle=linestyles[name],
            marker=markers[name], markersize=6, linewidth=2, label=name)
ax.set_xlabel('Read Ratio (%)', fontsize=11)
ax.set_ylabel('Write Throughput (MB/s)', fontsize=11)
ax.set_title('Write Throughput vs Read Ratio', fontsize=12, fontweight='bold')
ax.grid(True, alpha=0.3)

# ── (d) Write vs Read scatter ──
ax = axes[1][1]
for i, name in enumerate(labels):
    avg_r = np.mean(mixed_read[name][1:])
    ax.scatter(write_mbps[i], avg_r, c=colors[name], s=180,
               edgecolors='white', linewidth=1, label=name, zorder=5)
    ax.annotate(name, (write_mbps[i], avg_r),
                textcoords="offset points", xytext=(8, 3), fontsize=7)
ax.set_xlabel('Write Throughput (MB/s)', fontsize=11)
ax.set_ylabel('Read Throughput (MB/s, avg 10-100%R)', fontsize=11)
ax.set_title('Write vs Read Trade-off', fontsize=12, fontweight='bold')
ax.grid(True, alpha=0.3)

plt.tight_layout(rect=[0, 0.02, 1, 0.94])
plt.savefig('D:/Project/rocks-demo/stfp_final_comparison.png', dpi=150,
            bbox_inches='tight', facecolor='white')
plt.close()
print("Saved: D:/Project/rocks-demo/stfp_final_comparison.png")

# ── Summary ──
print("\n=== Pure Read/Write Summary ===")
print(f"{'Strategy':<20} {'Write':>8} {'ReadAvg':>10} {'R/W Ratio':>10}")
print("-" * 50)
for i, name in enumerate(labels):
    print(f"{name:<20} {write_mbps[i]:>7.1f}  {read_avg[i]:>9.0f}  {read_avg[i]/write_mbps[i]:>9.1f}")

# STFP degradation
print("\n=== STFP Read Degradation (10%R -> 90%R) ===")
for name in ["All-Tiered(Real)", "Hybrid-L1", "STFP-R4", "STFP-R6", "STFP-Rinf"]:
    r10 = mixed_read[name][1]
    r90 = mixed_read[name][9]
    print(f"  {name:<20}: {r10:.0f} -> {r90:.0f}  Δ={r10-r90:+.0f} MB/s ({(r10-r90)/r10*100:+.0f}%)")
