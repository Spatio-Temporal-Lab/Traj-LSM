# ST-Compaction Ablation
# ======================
# Hypothesis: STFP-R4 outperforms all baselines on read throughput and
#   provides the best read/write balance. Each trigger (A/B/score/run-count)
#   contributes measurably to compaction quality.
#
# Part 1: Strategy comparison (all 7 strategies)
# Part 2: Per-trigger breakdown (STFP-R4 only)
#   - Full STFP-R4 (all triggers on)
#   - No Trigger A (stfp_enable=false → A+B disabled)
#   - No Trigger B (IDLE-only check removed → not easily switchable)
#   - Score only (stfp_enable=false, real_tiered=false → standard leveled)

param([int]$Repeats = 10, [int]$Keys = 200000)

. "$PSScriptRoot\config.ps1"

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out_dir = "$OUT_DIR\compaction_$timestamp"
New-Item -ItemType Directory -Force -Path $out_dir | Out-Null

Write-Output "=== ST-Compaction Ablation ==="
Write-Output "Output dir: $out_dir"
Write-Output "Keys: $Keys"
Write-Output "Repeats: $Repeats"
Write-Output ""

# Part 1: Strategy comparison
Write-Output "--- Part 1: Strategy Comparison ---"
Write-Output "Strategies: all_leveled, all_tiered_r4, hybrid_l1_r4,"
Write-Output "            hybrid_l12_r4, stfp_r4, stfp_r6, stfp_rinf"
Write-Output ""

$strategy_args = @(
    "--all_leveled",
    "--all_tiered_r4",
    "--hybrid_l1_r4",
    "--hybrid_l12_r4",
    "--stfp_r4",
    "--stfp_r6",
    "--stfp_rinf"
)

$strategy_names = @(
    "all_leveled",
    "all_tiered_r4", 
    "hybrid_l1_r4",
    "hybrid_l12_r4",
    "stfp_r4",
    "stfp_r6",
    "stfp_rinf"
)

for ($r = 1; $r -le $Repeats; $r++) {
    Write-Output "--- Round $r / $Repeats ---"
    for ($i = 0; $i -lt $strategy_args.Count; $i++) {
        $name = $strategy_names[$i]
        Write-Output "  Strategy: $name"
        
        $out_csv = "$out_dir\strategy_${name}_r${r}.csv"
        $result = & $COMPACTION_BENCH @(
            "--keys", $Keys,
            "--out-csv", $out_csv,
            $strategy_args[$i]
        ) 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Output "  FAILED: $result"
        }
    }
}

# Part 2: Per-trigger breakdown (STFP-R4 only)
Write-Output ""
Write-Output "--- Part 2: Per-Trigger Breakdown ---"
Write-Output "NOTE: Trigger decomposition requires separate builds or"
Write-Output "  runtime options. Currently supported via:"
Write-Output "  --stfp_enable=0  → disable Trigger A+B"
Write-Output "  Additional granularity needs code-level switches."
Write-Output ""
Write-Output "Variants for STFP-R4:"
Write-Output "  1. full_stfp_r4: all triggers on (baseline)"
Write-Output "  2. no_trigger_ab:  stfp_enable=false (score+run-count only)"
Write-Output "  3. score_only:     stfp_enable=false, real_tiered=false, tiered=false"
Write-Output "                     (pure leveled -> NOT STFP)"
Write-Output ""
Write-Output "For a true per-trigger comparison, consider:"
Write-Output "  - Trigger A effect = full_stfp_r4 vs no_trigger_ab (difference)"
Write-Output "  - Trigger B effect = IDLE-only behavior, measured by idle compaction rate"

# Summary
Write-Output ""
Write-Output "=== Results ==="
Write-Output "Results saved to: $out_dir"
Write-Output ""
Write-Output "To plot: python tools\plot\plot_ablation.py --dir $out_dir"
