# Full ablation runner - single pass, all experiments
$ROOT = "D:\Project"
$BUILD = "$ROOT\rocks-demo\build"
$OUT = "$ROOT\Result\ablation"
$ts = Get-Date -Format "yyyyMMdd-HHmmss"
New-Item -ItemType Directory -Force -Path $OUT | Out-Null

# === 1. COMPACTION ABLATION ===
Write-Output "=== [1/3] Compaction Ablation ==="
$comp_out = "$OUT\compaction_$ts.tsv"
"strategy`trepeat`tread_mbs`twrite_mbs`twrite_amp`tl0`tl1`tl2`tl3" | Out-File $comp_out

$strategies = @("all_leveled","all_tiered_r4","hybrid_l1_r4","hybrid_l12_r4","stfp_r4","stfp_r6","stfp_rinf")
for ($r = 1; $r -le 3; $r++) {
    foreach ($s in $strategies) {
        Write-Output "  $s (round $r/3)"
        $raw = & $BUILD\st_compaction_throughput_bench.exe --num-keys 50000 --repeat 1 --db-root "D:/tmp/ctb_abl" "--$s" 2>&1 | Out-String
        if ($raw -match "$s,([\d.]+),([\d.]+),[\d.]+,[\d.]+,([\d.]+),(\d+),(\d+),(\d+),(\d+)") {
            "$s`t$r`t$($Matches[2])`t$($Matches[1])`t$($Matches[3])`t$($Matches[4])`t$($Matches[5])`t$($Matches[6])`t$($Matches[7])" | Out-File $comp_out -Append
        }
    }
}
Write-Output "  -> $comp_out"

# === 2. GLOBAL INDEX ABLATION ===
Write-Output "`n=== [2/3] Global Index Ablation ==="
$global_out = "$OUT\global_$ts.tsv"
"experiment`tvariant`twindow`tfiles_skipped`tfiles_considered`tblock_reads`twall_us" | Out-File $global_out

$windows = Import-Csv "$ROOT\data\processed\unified\unified_stratified12_st_windows.csv"
$variants = @(
    @{name="no_file"; mode="sst"},
    @{name="with_file"; mode="sst_manifest"}
)

foreach ($v in $variants) {
    Write-Output "  Variant: $($v.name)"
    foreach ($w in $windows) {
        $raw = & $BUILD\st_meta_read_bench.exe @(
            "--db","$ROOT\data\db\tdrive_st","--no-full-scan",
            "--prune-mode",$v.mode,
            "--prune-t-min",$w.TMin,"--prune-t-max",$w.TMax,
            "--prune-x-min",$w.XMin,"--prune-x-max",$w.XMax,
            "--prune-y-min",$w.YMin,"--prune-y-max",$w.YMax,
            "--block-cache-mb","512","--iterator-repeat","3"
        ) 2>&1 | Out-String
        
        $fs = 0; $fc = 0; $br = 0; $wall = 0
        if ($raw -match "prune_file_skipped=(\d+)") { $fs = $Matches[1] }
        if ($raw -match "considered=(\d+)") { $fc = $Matches[1] }
        if ($raw -match "block_read_count=(\d+)") { $br = $Matches[1] }
        if ($raw -match "wall_us_total=(\d+)") { $wall = $Matches[1] }
        
        "global`t$($v.name)`t$($w.Label)`t$fs`t$fc`t$br`t$wall" | Out-File $global_out -Append
    }
}
Write-Output "  -> $global_out"

# === 3. LOCAL INDEX ABLATION ===
Write-Output "`n=== [3/3] Local Index Ablation ==="
$local_out = "$OUT\local_$ts.tsv"
"experiment`tvariant`twindow`tblock_reads`tindex_examined`tindex_skipped`tkeys_examined`twall_us" | Out-File $local_out

$local_variants = @(
    @{name="file_only"; mode="manifest"},
    @{name="file_block"; mode="sst_manifest_pipeline"},
    @{name="file_block_key"; mode="sst_manifest"}
)

foreach ($v in $local_variants) {
    Write-Output "  Variant: $($v.name)"
    foreach ($w in $windows) {
        $raw = & $BUILD\st_meta_read_bench.exe @(
            "--db","$ROOT\data\db\tdrive_st","--no-full-scan",
            "--prune-mode",$v.mode,
            "--prune-t-min",$w.TMin,"--prune-t-max",$w.TMax,
            "--prune-x-min",$w.XMin,"--prune-x-max",$w.XMax,
            "--prune-y-min",$w.YMin,"--prune-y-max",$w.YMax,
            "--block-cache-mb","512","--iterator-repeat","3"
        ) 2>&1 | Out-String
        
        $br = 0; $ie = 0; $is_= 0; $ke = 0; $wall = 0
        if ($raw -match "block_read_count=(\d+)") { $br = $Matches[1] }
        if ($raw -match "index_examined=(\d+)") { $ie = $Matches[1] }
        if ($raw -match "index_skipped_st_disjoint=(\d+)") { $is_ = $Matches[1] }
        if ($raw -match "keys_examined=(\d+)") { $ke = $Matches[1] }
        if ($raw -match "wall_us_total=(\d+)") { $wall = $Matches[1] }
        
        "local`t$($v.name)`t$($w.Label)`t$br`t$ie`t$is_`t$ke`t$wall" | Out-File $local_out -Append
    }
}
Write-Output "  -> $local_out"
Write-Output "`n=== DONE ==="
