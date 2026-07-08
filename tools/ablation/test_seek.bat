@echo off
set ROCKSDB_ST_USER_KEY_SCHEMA=e7
set ST_DISABLE_FORWARD_TRUNCATION=1
set ST_DISABLE_BACKWARD_TRUNCATION=0

echo === backward_only ===
D:\Project\rocks-demo\build\st_meta_read_bench.exe --db D:\Project\data\db\tdwx_full --no-full-scan --prune-mode sst --event-time-bucket-width-sec 86400 --prune-t-min 1201930244 --prune-t-max 1201933844 --prune-x-min 116.0 --prune-x-max 117.0 --prune-y-min 39.0 --prune-y-max 41.0 --block-cache-mb 512 --iterator-repeat 1 2>&1 | findstr "index_examined"

set ST_DISABLE_FORWARD_TRUNCATION=0
set ST_DISABLE_BACKWARD_TRUNCATION=1

echo === forward_only ===
D:\Project\rocks-demo\build\st_meta_read_bench.exe --db D:\Project\data\db\tdwx_full --no-full-scan --prune-mode sst --event-time-bucket-width-sec 86400 --prune-t-min 1201930244 --prune-t-max 1201933844 --prune-x-min 116.0 --prune-x-max 117.0 --prune-y-min 39.0 --prune-y-max 41.0 --block-cache-mb 512 --iterator-repeat 1 2>&1 | findstr "index_examined"

set ST_DISABLE_FORWARD_TRUNCATION=0
set ST_DISABLE_BACKWARD_TRUNCATION=0

echo === both ===
D:\Project\rocks-demo\build\st_meta_read_bench.exe --db D:\Project\data\db\tdwx_full --no-full-scan --prune-mode sst --event-time-bucket-width-sec 86400 --prune-t-min 1201930244 --prune-t-max 1201933844 --prune-x-min 116.0 --prune-x-max 117.0 --prune-y-min 39.0 --prune-y-max 41.0 --block-cache-mb 512 --iterator-repeat 1 2>&1 | findstr "index_examined"
