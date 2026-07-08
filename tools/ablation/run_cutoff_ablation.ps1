# Forward/Backward Cutoff Ablation
# ===============================
# Hypothesis: suffix-max forward cutoff reduces index entries examined
#   compared to backward-cutoff-only (bucket_id) during forward scan.
# Variants:
#   backward_only:  bucket_id back-cutoff only (disable suffix-max)
#   both_cutoff:    bucket_id back-cutoff + suffix-max forward cutoff
#
# Implementation: suffix-max is controlled by query_t_start_ in IndexBlockIter.
#   When query_t_start_=0, suffix-max is disabled (default behavior before our
#   suffix-max implementation). When query_t_start_>0, suffix-max is active.
#
# To disable suffix-max for comparison, we need a build flag or a runtime flag.
# Temporarily: compile with/without suffix-max via preprocessor define.

param([int]$Repeats = 10)

. "$PSScriptRoot\config.ps1"

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out_tsv = "$OUT_DIR\cutoff_ablation_$timestamp.tsv"

Write-Output "=== Cutoff Ablation ==="
Write-Output "Output: $out_tsv"
Write-Output ""
Write-Output "NOTE: This experiment requires two builds:"
Write-Output "  Build A: suffix-max ENABLED  (current code)"
Write-Output "  Build B: suffix-max DISABLED (SetQueryStartTimeForSuffixMax(0))"
Write-Output ""
Write-Output "Both builds use --prune-mode sst_manifest"
Write-Output ""

$variants = @(
    @{name="suffix_max_on";  build="build_suffix_max";    exe="$BUILD\st_meta_read_bench.exe"},
    @{name="suffix_max_off"; build="build_no_suffix_max"; exe="D:\Project\rocks-demo\build_no_suffix\st_meta_read_bench.exe"}
)

"experiment`tvariant`twindow_id`tmetric`tvalue" | Out-File $out_tsv

Write-Output "To run this experiment:"
Write-Output "  1. Build suffix-max ON (current): cd rocks-demo\build && cmake --build . --config Release"
Write-Output "  2. Build suffix-max OFF: create a separate build dir with SUFFIX_MAX_ENABLED=0"
Write-Output "  3. Run: .\run_cutoff_ablation.ps1"
Write-Output ""
Write-Output "Key metrics:"
Write-Output "  block_level_index_entries_examined  -- total index entries scanned"
Write-Output "  block_level_index_entries_skipped   -- entries skipped by hard-decision"
Write-Output "  block_level_index_prune_ns          -- time spent in block-level pruning"
Write-Output ""
Write-Output "Expected: suffix_max_on should show fewer index_entries_examined"
Write-Output "  for query windows near the end of the data timeline."
