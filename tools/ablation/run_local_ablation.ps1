# Local Index Ablation
# ====================
# Hypothesis: Index block-level ST meta hard-decision pruning reduces
#   data blocks read vs file-level-only pruning.
# Variants:
#   file_only:      file_level=true,  block_level=false, key_level=false
#   file_block:     file_level=true,  block_level=true,  key_level=false
#   file_block_key: file_level=true,  block_level=true,  key_level=true   (full)

param([int]$Repeats = 10)

. "$PSScriptRoot\config.ps1"

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out_tsv = "$OUT_DIR\local_ablation_$timestamp.tsv"

$variants = @(
    @{name="file_only";      args=@("--prune-mode","manifest")},
    @{name="file_block";     args=@("--prune-mode","sst_manifest_pipeline")},
    @{name="file_block_key"; args=@("--prune-mode","sst_manifest")}
)

Write-Output "=== Local Index Ablation ==="
Write-Output "Output: $out_tsv"
Write-Output "Repeats: $Repeats"
"experiment`tvariant`twindow_id`tmetric`tvalue" | Out-File $out_tsv

for ($r = 1; $r -le $Repeats; $r++) {
    Write-Output "--- Round $r / $Repeats ---"
    foreach ($v in $variants) {
        Write-Output "  Variant: $($v.name)"
        $args = $COMMON_FLAGS + $v.args + @(
            "--prune-mode-output-tsv", "$OUT_DIR\tmp_local_$($v.name)_r${r}.tsv"
        )
        $result = & $READ_BENCH @args 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Output "  FAILED"; continue }
        $tmp = "$OUT_DIR\tmp_local_$($v.name)_r${r}.tsv"
        if (Test-Path $tmp) {
            Get-Content $tmp | ForEach-Object {
                if ($_ -match '^[a-z]') { "local`t$($v.name)`t$_" | Out-File $out_tsv -Append }
            }
        }
    }
}
Write-Output "Done: $out_tsv"
Remove-Item "$OUT_DIR\tmp_local_*" -ErrorAction SilentlyContinue
