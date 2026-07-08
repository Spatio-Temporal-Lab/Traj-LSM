"""Parse ST-LSM and TMan results for comparison"""
import os, re, glob

# === ST-LSM Results ===
result_dir = r"D:\Project\Result\comparison"
files = sorted(glob.glob(os.path.join(result_dir, "stlsm_tdwx_r*.txt")))

stlsm_by_window = {}
for fpath in files:
    fname = os.path.basename(fpath)
    parts = fname.replace(".txt", "").split("_")
    window = "_".join(parts[3:])
    
    with open(fpath, encoding="utf-16") as f:
        content = f.read()
    
    m = re.search(r"wall_us_per_iter_avg=([\d.]+)", content)
    wall = float(m.group(1)) if m else 0
    
    m = re.search(r"keys\(last_iter\)=(\d+)", content)
    keys = int(m.group(1)) if m else 0
    
    m = re.search(r"block_read_count=(\d+)", content)
    blocks = int(m.group(1)) if m else 0
    
    m = re.search(r"index_examined=(\d+)", content)
    idx = int(m.group(1)) if m else 0
    
    m = re.search(r"index_skipped_st_disjoint=(\d+)", content)
    idx_skip = int(m.group(1)) if m else 0
    
    m = re.search(r"prune_file_skipped=(\d+)", content)
    fs = int(m.group(1)) if m else 0
    
    m = re.search(r"prune_file_diag considered=(\d+)", content)
    fc = int(m.group(1)) if m else 0
    
    if window not in stlsm_by_window:
        stlsm_by_window[window] = {"walls": [], "keys": [], "blocks": [], "idx": [], "idx_skip": [], "fs": [], "fc": []}
    stlsm_by_window[window]["walls"].append(wall)
    stlsm_by_window[window]["keys"].append(keys)
    stlsm_by_window[window]["blocks"].append(blocks)
    stlsm_by_window[window]["idx"].append(idx)
    stlsm_by_window[window]["idx_skip"].append(idx_skip)
    stlsm_by_window[window]["fs"].append(fs)
    stlsm_by_window[window]["fc"].append(fc)

# === TMan results ===
tman = {
    "time_ms": {"avg": 629, "median": 451, "p90": 1682},
    "indexRanges": {"avg": 3244},
    "candidates": {"avg": 34574},
    "finalSize": {"avg": 1078}
}

# Print comparison table
print("=" * 80)
print("ST-LSM vs TMan Comparison (tdrive + wuxi, 5 repeats)")
print("=" * 80)
print()
print(f'{"Metric":<30} {"TMan":>15} {"ST-LSM":>15} {"Ratio":>10}')
print("-" * 70)

# Wall time
stlsm_all_walls = []
for w, d in stlsm_by_window.items():
    stlsm_all_walls.extend(d["walls"])
stlsm_avg_wall_us = sum(stlsm_all_walls) / len(stlsm_all_walls)
stlsm_avg_wall_ms = stlsm_avg_wall_us / 1000
print(f'{"Wall time (ms)":<30} {629:>15.1f} {stlsm_avg_wall_ms:>15.1f} {629/stlsm_avg_wall_ms:>9.0f}x')

# Results found
stlsm_all_keys = []
for w, d in stlsm_by_window.items():
    stlsm_all_keys.extend(d["keys"])
stlsm_avg_keys = sum(stlsm_all_keys) / len(stlsm_all_keys)
print(f'{"Results found":<30} {1078:>15.0f} {stlsm_avg_keys:>15.0f} {"N/A":>10}')

# Index entries
stlsm_all_idx = []
for w, d in stlsm_by_window.items():
    stlsm_all_idx.extend(d["idx"])
stlsm_avg_idx = sum(stlsm_all_idx) / len(stlsm_all_idx)
print(f'{"Index entries examined":<30} {3244:>15.0f} {stlsm_avg_idx:>15.0f} {3244/stlsm_avg_idx:>9.1f}x')

# Block reads
stlsm_all_blocks = []
for w, d in stlsm_by_window.items():
    stlsm_all_blocks.extend(d["blocks"])
stlsm_avg_blocks = sum(stlsm_all_blocks) / len(stlsm_all_blocks)
print(f'{"Block reads":<30} {"N/A":>15} {stlsm_avg_blocks:>15.0f} {"N/A":>10}')

print()
print("=== Per-Window ST-LSM Results ===")
print(f'{"Window":<25} {"Wall(us)":>10} {"Keys":>8} {"Blocks":>8} {"IdxExam":>10} {"IdxSkip":>10} {"FileSkip":>10}')
print("-" * 85)
for window in sorted(stlsm_by_window.keys()):
    d = stlsm_by_window[window]
    avg_w = sum(d["walls"]) / len(d["walls"])
    avg_k = sum(d["keys"]) / len(d["keys"])
    avg_b = sum(d["blocks"]) / len(d["blocks"])
    avg_i = sum(d["idx"]) / len(d["idx"])
    avg_is = sum(d["idx_skip"]) / len(d["idx_skip"])
    avg_fs = sum(d["fs"]) / len(d["fs"])
    fc = d["fc"][0] if d["fc"] else 0
    print(f'{window:<25} {avg_w:>10.0f} {avg_k:>8.0f} {avg_b:>8.0f} {avg_i:>10.0f} {avg_is:>10.0f} {avg_fs:>3.0f}/{fc}')
