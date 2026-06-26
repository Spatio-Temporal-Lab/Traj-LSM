import matplotlib.pyplot as plt
import numpy as np

labels = ["All-Leveled", "All-Tiered\n(Real)", "Hybrid\nL1",
          "Hybrid\nL1+L2", "STFP\nR4", "STFP\nR6", "STFP\nRinf"]

write_mbps = [22.7, 24.2, 21.7, 21.1, 21.0, 21.3, 22.4]
read_avg   = [536,  666,  662,  667,  596,  622,  622]

x = np.arange(len(labels))
width = 0.32

fig, ax = plt.subplots(figsize=(14, 6))

bars_w = ax.bar(x - width/2, write_mbps, width, label='Write Throughput',
                color='#546e7a', edgecolor='white', linewidth=0.8)
bars_r = ax.bar(x + width/2, read_avg, width, label='Read Throughput (pruned query + point truth)',
                color='#ff7043', edgecolor='white', linewidth=0.8)

for bar, v in zip(bars_w, write_mbps):
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
            f'{v:.1f}', ha='center', fontsize=11, fontweight='bold', color='#546e7a')
for bar, v in zip(bars_r, read_avg):
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 6,
            f'{v:.0f}', ha='center', fontsize=11, fontweight='bold', color='#e64a19')

ax.set_ylabel('Throughput (MB/s)', fontsize=13)
ax.set_title('ST-Compaction Read/Write Throughput\n'
             '(200K E7 keys, ΔS=0.01, shuffled writes, pruned query + point truth)',
             fontsize=13, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=11)
ax.legend(fontsize=12, loc='upper right')
ax.set_ylim(0, 750)
ax.grid(axis='y', alpha=0.3)

plt.tight_layout()
plt.savefig('D:/Project/rocks-demo/stfp_rw_throughput.png', dpi=150,
            bbox_inches='tight', facecolor='white')
plt.close()
print("Saved: D:/Project/rocks-demo/stfp_rw_throughput.png")
