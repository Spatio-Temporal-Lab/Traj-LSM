import matplotlib.pyplot as plt
import numpy as np

labels = ["All-Leveled", "All-Tiered(R4)", "Hybrid-L1(R4)",
          "Hybrid-L1+L2(R4)", "STFP-R4", "STFP-R6"]
ratios = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

mixed_read = {
    "All-Leveled":       [0, 562.4, 557.4, 564.3, 562.9, 566.9, 569.5, 561.5, 569.3, 565.4, 567.7],
    "All-Tiered(R4)":    [0, 582.4, 566.6, 570.2, 573.7, 574.8, 573.0, 571.6, 573.8, 573.7, 574.3],
    "Hybrid-L1(R4)":     [0, 579.3, 577.1, 578.6, 575.5, 579.9, 581.1, 580.1, 578.7, 581.6, 578.0],
    "Hybrid-L1+L2(R4)":  [0, 581.2, 574.1, 574.4, 575.7, 573.9, 577.8, 577.4, 575.5, 577.0, 577.3],
    "STFP-R4":           [0, 569.4, 589.2, 569.6, 586.6, 590.1, 576.3, 565.2, 578.5, 584.3, 567.6],
    "STFP-R6":           [0, 548.5, 556.9, 548.8, 546.6, 553.0, 556.3, 555.8, 559.3, 552.7, 559.5],
}
mixed_write = {
    "All-Leveled":       [207.2, 141.0, 116.0, 92.7, 79.0, 64.3, 56.5, 50.2, 42.2, 37.2, 0],
    "All-Tiered(R4)":    [202.9, 154.0, 117.4, 92.7, 78.8, 65.2, 55.8, 49.4, 43.3, 36.8, 0],
    "Hybrid-L1(R4)":     [218.5, 154.6, 116.0, 94.8, 78.9, 67.5, 56.3, 49.6, 43.4, 35.6, 0],
    "Hybrid-L1+L2(R4)":  [186.5, 155.2, 112.2, 91.1, 75.2, 65.7, 57.2, 49.2, 44.6, 36.3, 0],
    "STFP-R4":           [202.3, 147.3, 120.1, 96.3, 83.8, 71.2, 57.8, 51.1, 45.4, 39.4, 0],
    "STFP-R6":           [213.8, 152.5, 122.4, 97.5, 79.8, 71.1, 62.4, 54.1, 48.6, 41.2, 0],
}

colors = {"All-Leveled":'#b0bec5',"All-Tiered(R4)":'#1565c0',"Hybrid-L1(R4)":'#1b5e20',
          "Hybrid-L1+L2(R4)":'#4caf50',"STFP-R4":'#ff7043',"STFP-R6":'#ffab91'}
lss = {"All-Leveled":'--',"All-Tiered(R4)":'-',"Hybrid-L1(R4)":'-.',
       "Hybrid-L1+L2(R4)":':',"STFP-R4":'-',"STFP-R6":'-.'}
mks = {"All-Leveled":'s',"All-Tiered(R4)":'D',"Hybrid-L1(R4)":'o',
       "Hybrid-L1+L2(R4)":'v',"STFP-R4":'^',"STFP-R6":'s'}

fig,(ax1,ax2)=plt.subplots(1,2,figsize=(18,7))
fig.suptitle('v8 — 8MB memtable, 16MB SST, key=100K, 10 repeats trimmed mean\n(200K E7 keys, shuffled+Flush, pruned query+point truth)',fontsize=13,fontweight='bold')
for name in labels:
    ax1.plot(ratios[1:],mixed_read[name][1:],color=colors[name],ls=lss[name],marker=mks[name],markersize=8,linewidth=2.5,label=name)
ax1.set_xlabel('Read Ratio (%)',fontsize=13); ax1.set_ylabel('Read Throughput (MB/s)',fontsize=13)
ax1.set_title('Read',fontsize=14,fontweight='bold'); ax1.grid(True,alpha=0.3)
for name in labels:
    ax2.plot(ratios[:-1],mixed_write[name][:-1],color=colors[name],ls=lss[name],marker=mks[name],markersize=8,linewidth=2.5,label=name)
ax2.set_xlabel('Read Ratio (%)',fontsize=13); ax2.set_ylabel('Write Throughput (MB/s)',fontsize=13)
ax2.set_title('Write',fontsize=14,fontweight='bold'); ax2.grid(True,alpha=0.3)
h,l=ax1.get_legend_handles_labels()
fig.legend(h,l,loc='lower center',ncol=6,fontsize=9,bbox_to_anchor=(0.5,-0.06))
plt.tight_layout(rect=[0,0.07,1,1])
plt.savefig('D:/Project/rocks-demo/stfp_v8.png',dpi=150,bbox_inches='tight',facecolor='white')
plt.close()
print("Saved: D:/Project/rocks-demo/stfp_v8.png")
for name in labels:
    print(f"  {name:<22} read_avg={np.mean(mixed_read[name][1:]):.0f}  write_avg={np.mean(mixed_write[name][:-1]):.0f}")
