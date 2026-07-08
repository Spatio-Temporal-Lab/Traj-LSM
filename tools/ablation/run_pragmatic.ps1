# Pragmatic ablation runner — matches actual tool interfaces
param([int]$Repeats = 3)

$ROOT = "D:\Project"
$BUILD = "$ROOT\rocks-demo\build"
$DB = "$ROOT\data\db\tdrive_st"
$OUT = "$ROOT\Result\ablation"
$WINDOWS = "$ROOT\data\processed\unified\unified_stratified12_st_windows.csv"
$META = "$ROOT\data\processed\unified\segments_meta.csv"
$POINTS = "$ROOT\data\processed\unified\segments_points.csv"
$BENCH = "$BUILD\st_meta_read_bench.exe"
$CTB = "$BUILD\st_compaction_throughput_bench.exe"

New-Item -ItemType Directory -Force -Path $OUT | Out-Null
$ts = Get-Date -Format "yyyyMMdd-HHmmss"

# Read windows from CSV
$windows = Import-Csv $WINDOWS

# ---- GLOBAL INDEX ABLATION ----
Write-Output "=== [1/3] Global Index Ablation ==="
$global_tsv = "$OUT\global_$ts.tsv"
"experiment`tvariant`twindow`tfiles_skipped`tfiles_considered`tindex_examined`tsuffix_max_stops`twall_ms" | Out-File $global_tsv

foreach ($v in @(
    @{name="no_file"; mode="sst"},
    @{name="with_file"; mode="sst_manifest"}
)) {
    Write-Output "  Variant: $($v.name)"
    foreach ($w in $windows) {
        $label = $w.Label
        for ($r = 0; $r -lt $Repeats; $r++) {
            $result = & $BENCH @(
                "--db", $DB, "--no-full-scan",
                "--prune-mode", $v.mode,
                "--prune-t-min", $w.TMin, "--prune-t-max", $w.TMax,
                "--prune-x-min", $w.XMin, "--prune-x-max", $w.XMax,
                "--prune-y-min", $w.YMin, "--prune-y-max", $w.YMax,
                "--segment-meta-csv", $META,
                "--block-cache-mb", "512",
                "--iterator-repeat", "3"
            ) 2>&1 | Out-String
            
            # Parse output for metrics
            $fs = 0; $fc = 0; $ie = 0; $ss = 0; $wall = 0
            if ($result -match "files_skipped=(\d+)") { $fs = $Matches[1] }
            if ($result -match "files_considered=(\d+)") { $fc = $Matches[1] }
            if ($result -match "index_examined=(\d+)") { $ie = $Matches[1] }
            if ($result -match "suffix_max_stops=(\d+)") { $ss = $Matches[1] }
            if ($result -match "prune_wall_us=(\d+)") { $wall = [int]$Matches[1] / 1000.0 }
            if ($wall -eq 0 -and $result -match "wall_us=(\d+)") { $wall = [int]$Matches[1] / 1000.0 }
            
            "global`t$($v.name)`t$label`t$fs`t$fc`t$ie`t$ss`t$wall" | Out-File $global_tsv -Append
        }
    }
}
Write-Output "  -> $global_tsv"

# ---- LOCAL INDEX ABLATION ----
Write-Output "`n=== [2/3] Local Index Ablation ==="
$local_tsv = "$OUT\local_$ts.tsv"
"experiment`tvariant`twindow`tdata_blocks`tindex_examined`tindex_skipped`tkeys_examined`tkeys_skipped`tsuffix_max_stops`twall_ms" | Out-File $local_tsv

foreach ($v in @(
    @{name="file_only"; mode="manifest"},
    @{name="file_block"; mode="sst_manifest_pipeline"},
    @{name="file_block_key"; mode="sst_manifest"}
)) {
    Write-Output "  Variant: $($v.name)"
    foreach ($w in $windows) {
        for ($r = 0; $r -lt $Repeats; $r++) {
            $result = & $BENCH @(
                "--db", $DB, "--no-full-scan",
                "--prune-mode", $v.mode,
                "--prune-t-min", $w.TMin, "--prune-t-max", $w.TMax,
                "--prune-x-min", $w.XMin, "--prune-x-max", $w.XMax,
                "--prune-y-min", $w.YMin, "--prune-y-max", $w.YMax,
                "--segment-meta-csv", $META,
                "--block-cache-mb", "512",
                "--iterator-repeat", "3"
            ) 2>&1 | Out-String
            
            $db = 0; $ie = 0; $is_ = 0; $ke = 0; $ks = 0; $ss = 0; $wall = 0
            if ($result -match "block_read_count=(\d+)") { $db = $Matches[1] }
            if ($result -match "index_examined=(\d+)") { $ie = $Matches[1] }
            if ($result -match "index_skipped=(\d+)") { $is_ = $Matches[1] }
            if ($result -match "keys_examined=(\d+)") { $ke = $Matches[1] }
            if ($result -match "keys_skipped_disjoint=(\d+)") { $ks = $Matches[1] }
            if ($result -match "suffix_max_stops=(\d+)") { $ss = $Matches[1] }
            if ($result -match "prune_wall_us=(\d+)") { $wall = [int]$Matches[1] / 1000.0 }
            if ($wall -eq 0 -and $result -match "wall_us=(\d+)") { $wall = [int]$Matches[1] / 1000.0 }
            
            "local`t$($v.name)`t$($w.Label)`t$db`t$ie`t$is_`t$ke`t$ks`t$ss`t$wall" | Out-File $local_tsv -Append
        }
    }
}
Write-Output "  -> $local_tsv"

# ---- COMPACTION ABLATION ----
Write-Output "`n=== [3/3] Compaction Ablation ==="
$comp_dir = "$OUT\compaction_$ts"
New-Item -ItemType Directory -Force -Path $comp_dir | Out-Null

$strategy_args = @(
    "--all_leveled", "--all_tiered_r4", "--hybrid_l1_r4",
    "--hybrid_l12_r4", "--stfp_r4", "--stfp_r6", "--stfp_rinf"
)
$strategy_names = @(
    "all_leveled", "all_tiered_r4", "hybrid_l1_r4",
    "hybrid_l12_r4", "stfp_r4", "stfp_r6", "stfp_rinf"
)

$comp_tsv = "$OUT\compaction_$ts.tsv"
"strategy`trepeat`tread_avg_mbs`twrite_avg_mbs" | Out-File $comp_tsv

for ($r = 0; $r -lt $Repeats; $r++) {
    for ($i = 0; $i -lt $strategy_args.Count; $i++) {
        $name = $strategy_names[$i]
        Write-Output "  $name (round $($r+1)/$Repeats)"
        
        $result = & $CTB @(
            "--num-keys", "200000",
            "--repeat", "1",
            "--db-root", "D:/tmp/ctb_abl_$ts"
        ) $strategy_args[$i] 2>&1 | Out-String
        
        $read = 0; $write = 0
        if ($result -match "read_avg_mbs=([\d.]+)") { $read = $Matches[1] }
        if ($result -match "write_avg_mbs=([\d.]+)") { $write = $Matches[1] }
        
        "$name`t$r`t$read`t$write" | Out-File $comp_tsv -Append
    }
}
Write-Output "  -> $comp_tsv"

Write-Output "`n=== DONE ==="
Write-Output "Results: $OUT"
