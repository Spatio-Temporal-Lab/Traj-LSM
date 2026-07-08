# ST-LSM Forward/Backward Cutoff Ablation
# 3 variants: backward-only, forward-only, both
# 6 windows x 3 repeats each

$BENCH = "D:\Project\rocks-demo\build\st_meta_read_bench.exe"
$DB    = "D:\Project\data\db\tdwx_full"
$WIN   = "D:\Project\data\td_wx\windows6.csv"
$OUT   = "D:\Project\Result\ablation\cutoff_verify.tsv"

New-Item -ItemType Directory -Force -Path (Split-Path $OUT) | Out-Null
"variant`twindow`trepeat`tblock_read_count`tindex_examined`tindex_skipped`tindex_suffix_max_stops`twall_us" | Out-File $OUT

$windows = Import-Csv $WIN

$variants = @(
    @{Name="backward_only"; Fwd="1"; Bwd="0"},
    @{Name="forward_only";  Fwd="0"; Bwd="1"},
    @{Name="both";          Fwd="0"; Bwd="0"}
)

$env:ROCKSDB_ST_USER_KEY_SCHEMA = "e7"

foreach ($v in $variants) {
    $env:ST_DISABLE_FORWARD_TRUNCATION  = $v.Fwd
    $env:ST_DISABLE_BACKWARD_TRUNCATION = $v.Bwd
    Write-Output "=== $($v.Name) ==="
    foreach ($w in $windows) {
        for ($r = 0; $r -lt 3; $r++) {
            $result = & $BENCH --db $DB --no-full-scan --prune-mode sst `
                --event-time-bucket-width-sec 86400 `
                --prune-t-min $w.TMin --prune-t-max $w.TMax `
                --prune-x-min $w.XMin --prune-x-max $w.XMax `
                --prune-y-min $w.YMin --prune-y-max $w.YMax `
                --block-cache-mb 512 --iterator-repeat 1 2>&1 | Out-String

            $bc = 0; $ie = 0; $is_ = 0; $ss = 0; $wu = 0
            if ($result -match "block_read_count=(\d+)")          { $bc = $Matches[1] }
            if ($result -match "index_examined=(\d+)")            { $ie = $Matches[1] }
            if ($result -match "index_skipped_st_disjoint=(\d+)") { $is_ = $Matches[1] }
            if ($result -match "index_suffix_max_stops=(\d+)")    { $ss = $Matches[1] }
            if ($result -match "wall_us_total=(\d+)")             { $wu = $Matches[1] }

            "$($v.Name)`t$($w.Label)`t$r`t$bc`t$ie`t$is_`t$ss`t$wu" | Out-File $OUT -Append
            Write-Output "  $($w.Label) r$r block=$bc idx_exam=$ie idx_skip=$is_ stop=$ss wall_us=$wu"
        }
    }
}

Write-Output ""
Write-Output "Done. Results: $OUT"
