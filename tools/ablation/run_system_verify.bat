@echo off
set ROCKSDB_ST_USER_KEY_SCHEMA=e7
set ST_DISABLE_FORWARD_TRUNCATION=0
set ST_DISABLE_BACKWARD_TRUNCATION=0

set B=D:\Project\rocks-demo\build\st_meta_read_bench.exe
set DB=D:\Project\data\db\tdwx_full

echo ============================================================
echo  Local Index System Verification
echo ============================================================

echo.
echo === td_narrow: sst (Local only) ===
%B% --db %DB% --no-full-scan --prune-mode sst --event-time-bucket-width-sec 86400 --prune-t-min 1201930244 --prune-t-max 1201933844 --prune-x-min 116.0 --prune-x-max 117.0 --prune-y-min 39.0 --prune-y-max 41.0 --block-cache-mb 512 --iterator-repeat 1 2>&1 | findstr "BENCH_CONTRIB file_considered file_skipped index_examined block_read_count wall_us_total"

echo.
echo === td_narrow: sst_manifest (Global + Local) ===
%B% --db %DB% --no-full-scan --prune-mode sst_manifest --event-time-bucket-width-sec 86400 --prune-t-min 1201930244 --prune-t-max 1201933844 --prune-x-min 116.0 --prune-x-max 117.0 --prune-y-min 39.0 --prune-y-max 41.0 --block-cache-mb 512 --iterator-repeat 1 2>&1 | findstr "BENCH_CONTRIB file_considered file_skipped index_examined block_read_count wall_us_total"

echo.
echo === wx_narrow: sst_manifest (Global + Local) ===
%B% --db %DB% --no-full-scan --prune-mode sst_manifest --event-time-bucket-width-sec 86400 --prune-t-min 1596499200 --prune-t-max 1596502800 --prune-x-min 119.7 --prune-x-max 120.7 --prune-y-min 31.0 --prune-y-max 32.5 --block-cache-mb 512 --iterator-repeat 1 2>&1 | findstr "BENCH_CONTRIB file_considered file_skipped index_examined block_read_count wall_us_total"

echo.
echo === wx_wide: sst_manifest (Global + Local) ===
%B% --db %DB% --no-full-scan --prune-mode sst_manifest --event-time-bucket-width-sec 86400 --prune-t-min 1596499200 --prune-t-max 1596542400 --prune-x-min 118.9 --prune-x-max 121.0 --prune-y-min 30.6 --prune-y-max 32.9 --block-cache-mb 512 --iterator-repeat 1 2>&1 | findstr "BENCH_CONTRIB file_considered file_skipped index_examined block_read_count wall_us_total"
