# ST-LSM Ablation Experiments

## Goal
Verify independent contribution of each component.

## Environment
- Dataset: unified (284K segs, 80M pts)
- DB config: STFP-R4, 64MB SST, L1=12K/L2=20K/L3=80K
- Windows: unified_stratified12_st_windows.csv (12 windows)
- Repeats: 10 rounds, trimmed mean

---

## Experiment 1: Global Index
**Hypothesis:** st_file_meta file-level pruning reduces SST files opened.

| Variant | file_level | block_level | key_level |
|---------|-----------|-------------|-----------|
| baseline (no file) | off | on | on |
| with file level | on | on | on |

**Key metrics:** files_skipped, files_considered, prune_wall_ms

---

## Experiment 2: Local Index
**Hypothesis:** Each pruning layer (block, key) incrementally reduces data blocks read.

| Variant | file_level | block_level | key_level |
|---------|-----------|-------------|-----------|
| file only | on | off | off |
| file + block | on | on | off |
| file + block + key (full) | on | on | on |

**Key metrics:** data_blocks_read, index_entries_skipped, keys_skipped

---

## Experiment 3: Forward/Backward Cutoff
**Hypothesis:** suffix-max forward cutoff reduces index entries examined vs bucket_id backward-only cutoff.

| Variant | back cutoff | forward cutoff |
|---------|------------|----------------|
| backward only | bucket_id | disabled |
| both (default) | bucket_id | suffix-max |

**Key metrics:** index_entries_examined, block_level_index_prune_ns
**Expected:** Fewer index_entries_examined for queries near data timeline end.

---

## Experiment 4: ST-Compaction
**Hypothesis:** STFP-R4 achieves highest read throughput and best read/write balance.

### Part A: Strategy comparison (7 strategies)
| Strategy | Description |
|----------|-------------|
| all_leveled | Pure RocksDB leveled (baseline) |
| all_tiered_r4 | Full tiered, max_runs=4 |
| hybrid_l1_r4 | L1 tiered, L2+ leveled |
| hybrid_l12_r4 | L1+L2 tiered, L3+ leveled |
| stfp_r4 | ST compaction, max_runs=4 |
| stfp_r6 | ST compaction, max_runs=6 |
| stfp_rinf | ST compaction, unlimited runs |

### Part B: Trigger decomposition (STFP-R4 only)
| Variant | Trigger A | Trigger B | Score | Run-count |
|---------|-----------|-----------|-------|-----------|
| full stfp_r4 | on | on | on | on |
| no trigger AB | off | off | on | on |
| score only | off | off | on | off |

---

## Output Files
```
Result/ablation/
  global_ablation_<ts>.tsv
  local_ablation_<ts>.tsv
  cutoff_ablation_<ts>.tsv
  compaction_<ts>/
    strategy_*.csv
  ablation_summary.tsv
```

## Running
```powershell
# All experiments
.\tools\ablation\run_all.ps1 -Repeats 10

# Individual
.\tools\ablation\run_global_ablation.ps1
.\tools\ablation\run_local_ablation.ps1
.\tools\ablation\run_cutoff_ablation.ps1
.\tools\ablation\run_compaction_ablation.ps1

# Analysis
python tools\ablation\analyze.py --dir Result\ablation
```
