"""Parse ST-LSM ablation result files and create summary TSV"""
import os, re, glob

result_dir = r"D:\Project\Result\comparison"
files = sorted(glob.glob(os.path.join(result_dir, "stlsm_tdwx_r*.txt")))

summary = []
for fpath in files:
    fname = os.path.basename(fpath)
    # Parse: stlsm_tdwx_r1_ud_strat_narrow_02.txt
    parts = fname.replace(".txt", "").split("_")
    repeat = parts[2].replace("r", "")
    window = "_".join(parts[3:])  # ud_strat_narrow_02
    
    with open(fpath, encoding="utf-16") as f:
        content = f.read()
    
    # Extract metrics
    wall = 0; keys = 0; files_skipped = 0; files_considered = 0
    block_reads = 0; idx_examined = 0; idx_skipped = 0; key_examined = 0
    
    m = re.search(r"wall_us_per_iter_avg=([\d.]+)", content)
    if m: wall = float(m.group(1))
    
    m = re.search(r"keys\(last_iter\)=(\d+)", content)
    if m: keys = int(m.group(1))
    
    m = re.search(r"prune_file_skipped=(\d+)", content)
    if m: files_skipped = int(m.group(1))
    
    m = re.search(r"prune_file_diag considered=(\d+)", content)
    if m: files_considered = int(m.group(1))
    
    m = re.search(r"block_read_count=(\d+)", content)
    if m: block_reads = int(m.group(1))
    
    m = re.search(r"index_examined=(\d+)", content)
    if m: idx_examined = int(m.group(1))
    
    m = re.search(r"index_skipped_st_disjoint=(\d+)", content)
    if m: idx_skipped = int(m.group(1))
    
    m = re.search(r"keys_examined=(\d+)", content)
    if m: key_examined = int(m.group(1))
    
    summary.append({
        "repeat": repeat, "window": window,
        "wall_us": wall, "keys": keys,
        "files_skipped": files_skipped, "files_considered": files_considered,
        "block_reads": block_reads, "idx_examined": idx_examined,
        "idx_skipped": idx_skipped, "key_examined": key_examined
    })

# Group by window, compute stats across repeats
from collections import defaultdict
by_window = defaultdict(list)
for s in summary:
    by_window[s["window"]].append(s)

print("window\tkeys\twall_us_avg\tblock_reads\tidx_examined\tidx_skipped\tfiles_skip\tkey_examined")
for window in sorted(by_window.keys()):
    rows = by_window[window]
    n = len(rows)
    avg_wall = sum(r["wall_us"] for r in rows) / n
    avg_keys = sum(r["keys"] for r in rows) / n
    avg_br = sum(r["block_reads"] for r in rows) / n
    avg_ie = sum(r["idx_examined"] for r in rows) / n
    avg_is = sum(r["idx_skipped"] for r in rows) / n
    avg_fs = sum(r["files_skipped"] for r in rows) / n
    avg_ke = sum(r["key_examined"] for r in rows) / n
    print(f"{window}\t{avg_keys:.0f}\t{avg_wall:.0f}\t{avg_br:.0f}\t{avg_ie:.0f}\t{avg_is:.0f}\t{avg_fs:.0f}\t{avg_ke:.0f}")

# Overall summary
all_wall = [s["wall_us"] for s in summary]
all_block = [s["block_reads"] for s in summary]
all_idx = [s["idx_examined"] for s in summary]
print(f"\nOVERALL\tavg_wall={sum(all_wall)/len(all_wall):.0f}us\tavg_block={sum(all_block)/len(all_block):.0f}\tavg_idx={sum(all_idx)/len(all_idx):.0f}")
