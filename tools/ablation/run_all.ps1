# ST-LSM Master Ablation Runner
# =============================
# Runs all three ablation experiments.
# Usage: .\run_all.ps1 [-Repeats 10] [-SkipGlobal] [-SkipLocal] [-SkipCutoff] [-SkipCompaction]

param(
    [int]$Repeats = 10,
    [switch]$SkipGlobal,
    [switch]$SkipLocal,
    [switch]$SkipCutoff,
    [switch]$SkipCompaction
)

. "$PSScriptRoot\config.ps1"

Write-Output "============================================="
Write-Output "  ST-LSM Ablation Experiments"
Write-Output "============================================="
Write-Output "DB:       $DB"
Write-Output "Windows:  $WINDOWS_CSV"
Write-Output "Repeats:  $Repeats"
Write-Output "Output:   $OUT_DIR"
Write-Output "============================================="
Write-Output ""

$start_time = Get-Date

# 1. Global Index Ablation
if (-not $SkipGlobal) {
    Write-Output ">>> [1/4] Global Index Ablation <<<"
    & "$PSScriptRoot\run_global_ablation.ps1" -Repeats $Repeats
    Write-Output ""
}

# 2. Local Index Ablation
if (-not $SkipLocal) {
    Write-Output ">>> [2/4] Local Index Ablation <<<"
    & "$PSScriptRoot\run_local_ablation.ps1" -Repeats $Repeats
    Write-Output ""
}

# 3. Cutoff Ablation
if (-not $SkipCutoff) {
    Write-Output ">>> [3/4] Cutoff Ablation <<<"
    Write-Output "(requires two separate builds - see run_cutoff_ablation.ps1)"
    & "$PSScriptRoot\run_cutoff_ablation.ps1" -Repeats $Repeats
    Write-Output ""
}

# 4. ST-Compaction Ablation
if (-not $SkipCompaction) {
    Write-Output ">>> [4/4] ST-Compaction Ablation <<<"
    & "$PSScriptRoot\run_compaction_ablation.ps1" -Repeats $Repeats
    Write-Output ""
}

$elapsed = (Get-Date) - $start_time
Write-Output "============================================="
Write-Output "  All experiments complete"
Write-Output "  Elapsed: $($elapsed.TotalMinutes.ToString('F1')) min"
Write-Output "  Results: $OUT_DIR"
Write-Output "============================================="
