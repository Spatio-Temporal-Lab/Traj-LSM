import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np

# ── 300K × 3 repeats median results ──
strategies = [
    "All-\nLeveled",
    "All-Tiered\n(Real)",
    "Hybrid\nL1",
    "Hybrid\nL1+L2",
    "STFP\nR4",
    "STFP\nR6",
    "STFP\nRinf",
]

write_mbps = [15.4, 17.5, 18.1, 17.7, 16.7, 17.0, 17.6]
read_mbps  = [598,  673,  641,  595,  615,  656,  669]
write_amp  = [2.9,  2.7,  2.9,  2.4,  2.9,  2.7,  2.9]
file_skip  = [38.5, 68.8, 68.8, 68.8, 68.8, 68.8, 68.8]

# Colors: highlight STFP family
colors = ['#90a4ae', '#90a4ae', '#90a4ae', '#90a4ae',
          '#ff7043', '#ff5722', '#e64a19']  # orange for STFP

x = np.arange(len(strategies))
width = 0.35

fig, axes = plt.subplots(2, 2, figsize=(14, 10))
fig.suptitle('ST-Compaction Compaction Strategies Comparison\n'
             '(300K E7 keys, shuffled writes, pruned queries + point truth, 3 repeats, median)',
             fontsize=13, fontweight='bold')

# ── (a) Write Throughput ──
ax = axes[0][0]
bars = ax.bar(x, write_mbps, width, color=colors, edgecolor='white', linewidth=0.8)
ax.set_ylabel('Write Throughput (MB/s)', fontsize=11)
ax.set_title('Write Throughput', fontsize=12, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(strategies, fontsize=9)
ax.set_ylim(0, max(write_mbps) * 1.25)
for bar, v in zip(bars, write_mbps):
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.3,
            f'{v:.1f}', ha='center', fontsize=10, fontweight='bold')
# Highlight best
best_w = np.argmax(write_mbps)
bars[best_w].set_edgecolor('#1b5e20')
bars[best_w].set_linewidth(2.5)

# ── (b) Read Throughput ──
ax = axes[0][1]
bars = ax.bar(x, read_mbps, width, color=colors, edgecolor='white', linewidth=0.8)
ax.set_ylabel('Read Throughput (MB/s)', fontsize=11)
ax.set_title('Read Throughput (Pruned Query + Point Truth)', fontsize=12, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(strategies, fontsize=9)
ax.set_ylim(0, max(read_mbps) * 1.25)
for bar, v in zip(bars, read_mbps):
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 5,
            f'{v:.0f}', ha='center', fontsize=10, fontweight='bold')
best_r = np.argmax(read_mbps)
bars[best_r].set_edgecolor('#1b5e20')
bars[best_r].set_linewidth(2.5)

# ── (c) Write Amplification ──
ax = axes[1][0]
bars = ax.bar(x, write_amp, width, color=colors, edgecolor='white', linewidth=0.8)
ax.set_ylabel('Write Amplification (×)', fontsize=11)
ax.set_title('Write Amplification (Compaction Write / Raw Data)', fontsize=12, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(strategies, fontsize=9)
ax.set_ylim(0, max(write_amp) * 1.3)
for bar, v in zip(bars, write_amp):
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.03,
            f'{v:.1f}×', ha='center', fontsize=10, fontweight='bold')
best_a = np.argmin(write_amp)
bars[best_a].set_edgecolor('#1b5e20')
bars[best_a].set_linewidth(2.5)

# ── (d) File-level Pruning Effectiveness ──
ax = axes[1][1]
bars = ax.bar(x, file_skip, width, color=colors, edgecolor='white', linewidth=0.8)
ax.set_ylabel('File Skip Rate (%)', fontsize=11)
ax.set_title('File-Level Pruning: SST Files Skipped', fontsize=12, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(strategies, fontsize=9)
ax.set_ylim(0, 100)
ax.axhline(y=50, color='gray', linestyle='--', alpha=0.4, label='50%')
for bar, v in zip(bars, file_skip):
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1.5,
            f'{v:.1f}%', ha='center', fontsize=10, fontweight='bold')
ax.legend(fontsize=9)

plt.tight_layout(rect=[0, 0, 1, 0.94])
plt.savefig('D:/Project/rocks-demo/st_compaction_bench_results.png', dpi=150,
            bbox_inches='tight', facecolor='white')
plt.close()
print("Chart saved to D:/Project/rocks-demo/st_compaction_bench_results.png")

# ── Also print a combined summary table ──
print("\n=== Combined Ranking ===")
print(f"{'Strategy':<18} {'Write':>8} {'Read':>8} {'W-Amp':>8} {'FileSkip':>10}")
print("-" * 54)
for i, s in enumerate([s.replace('\n',' ') for s in strategies]):
    w_rank = sorted(range(len(write_mbps)), key=lambda j: write_mbps[j], reverse=True).index(i) + 1
    r_rank = sorted(range(len(read_mbps)),  key=lambda j: read_mbps[j],  reverse=True).index(i) + 1
    a_rank = sorted(range(len(write_amp)),  key=lambda j: write_amp[j]).index(i) + 1
    print(f"{s:<18} {write_mbps[i]:>7.1f} {read_mbps[i]:>7.0f} {write_amp[i]:>7.1f}× {file_skip[i]:>9.1f}%")

# ── STFP vs others summary ──
stfp_indices = [4, 5, 6]  # R4, R6, Rinf
other_indices = [0, 1, 2, 3]  # Leveled, Tiered, Hybrid-L1, Hybrid-L1+L2

print("\n=== STFP vs Others (Average) ===")
print(f"          Write(MB/s)  Read(MB/s)  W-Amp  FileSkip%")
stfp_w = np.mean([write_mbps[i] for i in stfp_indices])
stfp_r = np.mean([read_mbps[i] for i in stfp_indices])
stfp_a = np.mean([write_amp[i] for i in stfp_indices])
other_w = np.mean([write_mbps[i] for i in other_indices])
other_r = np.mean([read_mbps[i] for i in other_indices])
other_a = np.mean([write_amp[i] for i in other_indices])
print(f"STFP       {stfp_w:>8.1f}    {stfp_r:>7.0f}     {stfp_a:>4.1f}×   {file_skip[4]:>6.1f}%")
print(f"Others     {other_w:>8.1f}    {other_r:>7.0f}     {other_a:>4.1f}×   {file_skip[0]:>6.1f}%")
print(f"Δ          {stfp_w-other_w:>+8.1f}    {stfp_r-other_r:>+7.0f}     {stfp_a-other_a:>+4.1f}×")
