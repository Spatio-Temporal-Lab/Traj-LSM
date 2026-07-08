# Full ablation: Global + Local + Cutoff + Compaction
$ROOT = "D:\Project"
$BUILD = "$ROOT\rocks-demo\build"
$OUT = "$ROOT\Result\ablation"
$ts = Get-Date -Format "yyyyMMdd-HHmmss"

# === COMPACTION ===
Write-Output "=== [1/4] Compaction ==="
$raw = & $BUILD\st_compaction_throughput_bench.exe --num-keys 50000 --repeat 3 @(
    "--all_leveled","--all_tiered_r4","--hybrid_l1_r4",
    "--hybrid_l12_r4","--stfp_r4","--stfp_r6","--stfp_rinf"
) 2>&1 | Out-String
$csv_start = $raw.IndexOf("=== CSV ===")
if ($csv_start -ge 0) {
    $csv = $raw.Substring($csv_start + 12).Trim()
    $csv -split "`n" | Where-Object { $_ -match '^[A-Za-z]' } | ForEach-Object {
        "$_" | Out-File "$OUT\compaction_$ts.tsv" -Append -Encoding UTF8
    }
}
Write-Output "  compaction done"

# === GLOBAL + LOCAL ===
$WINDOWS = Import-Csv "$ROOT\data\processed\unified\unified_stratified12_st_windows.csv"

Write-Output "`n=== [2/4] Global + Local ==="
$gl_out = "$OUT\global_local_$ts.tsv"
"experiment`tvariant`twindow`tfiles_skipped`tfiles_considered`tblock_reads`tkeys_examined`tindex_examined`tindex_skipped`twall_us" | Out-File $gl_out -Encoding UTF8

$variants = @(
    @{exp="global"; name="no_file"; mode="sst"},
    @{exp="global"; name="with_file"; mode="sst_manifest"},
    @{exp="local";  name="file_only"; mode="manifest"},
    @{exp="local";  name="file_block"; mode="sst_manifest_pipeline"},
    @{exp="local";  name="file_block_key"; mode="sst_manifest"}
)

foreach ($v in $variants) {
    Write-Output "  $($v.exp)/$($v.name)"
    foreach ($w in $WINDOWS) {
        $result = & $BUILD\st_meta_read_bench.exe @(
            "--db","$ROOT\data\db\tdrive_st","--no-full-scan",
            "--prune-mode",$v.mode,
            "--prune-t-min",$w.TMin,"--prune-t-max",$w.TMax,
            "--prune-x-min",$w.XMin,"--prune-x-max",$w.XMax,
            "--prune-y-min",$w.YMin,"--prune-y-max",$w.YMax,
            "--block-cache-mb","512","--iterator-repeat","3"
        ) 2>&1 | Out-String
        
        $fs=0; $fc=0; $br=0; $ke=0; $ie=0; $is_=0; $wall=0
        if ($result -match "prune_file_skipped=(\d+)") { $fs = $Matches[1] }
        if ($result -match "considered=(\d+)") { $fc = $Matches[1] }
        if ($result -match "block_read_count=(\d+)") { $br = $Matches[1] }
        if ($result -match "keys_examined=(\d+)") { $ke = $Matches[1] }
        if ($result -match "index_examined=(\d+)") { $ie = $Matches[1] }
        if ($result -match "index_skipped_st_disjoint=(\d+)") { $is_ = $Matches[1] }
        if ($result -match "wall_us_total=(\d+)") { $wall = $Matches[1] }
        
        "$($v.exp)`t$($v.name)`t$($w.Label)`t$fs`t$fc`t$br`t$ke`t$ie`t$is_`t$wall" | Out-File $gl_out -Append -Encoding UTF8
    }
}
Write-Output "  -> $gl_out"

Write-Output "`n=== DONE ==="
