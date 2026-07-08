# Global Index Ablation
# =====================
# Hypothesis: st_file_meta file-level pruning (time+MBR) significantly
#   reduces the number of SST files opened during query.
# Variants:
#   A (baseline): file_level_enable=false, block/key enabled
#   B (full):     file_level_enable=true,  block/key enabled

param([int]$Repeats = 10)

. "$PSScriptRoot\config.ps1"

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out_tsv = "$OUT_DIR\global_ablation_$timestamp.tsv"

$variants = @(
    @{name="baseline_no_file"; args=@("--prune-mode","sst")},
    @{name="with_file_level";  args=@("--prune-mode","sst_manifest")}
)

Write-Output "=== Global Index Ablation ==="
Write-Output "Output: $out_tsv"
Write-Output "Repeats: $Repeats"
Write-Output ""

# TSV header
"experiment`tvariant`twindow_id`tmetric`tvalue" | Out-File $out_tsv

for ($r = 1; $r -le $Repeats; $r++) {
    Write-Output "--- Round $r / $Repeats ---"
    foreach ($v in $variants) {
        $name = $v.name
        Write-Output "  Variant: $name"
        
        $args = $COMMON_FLAGS + $v.args + @(
            "--prune-mode-output-tsv", "$OUT_DIR\tmp_global_${name}_r${r}.tsv"
        )
        
        $result = & $READ_BENCH @args 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Output "  FAILED: $result"
            continue
        }
        
        # Parse output TSV and add experiment + variant columns
        $tmp_tsv = "$OUT_DIR\tmp_global_${name}_r${r}.tsv"
        if (Test-Path $tmp_tsv) {
            Get-Content $tmp_tsv | ForEach-Object {
                if ($_ -match '^[a-z]') {
                    "global`t$name`t$_" | Out-File $out_tsv -Append
                }
            }
        }
    }
}

Write-Output "Done: $out_tsv"
# Cleanup tmp files
Remove-Item "$OUT_DIR\tmp_global_*" -ErrorAction SilentlyContinue
