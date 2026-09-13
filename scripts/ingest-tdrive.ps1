param(
  [string]$Root = "D:\Project",
  [string]$MetaCsv = "D:\Project\data\processed\base\segments_meta.csv",
  [string]$PointsCsv = "D:\Project\data\processed\base\segments_points.csv",
  [string]$SstDir = "D:\Project\data\sst\tdrive_st",
  [string]$Db = "D:\Project\data\db\tdrive_st",
  [int]$BucketSec = 3600,
  [int]$BlockSize = 32768,
  [double]$BlockCompactionRatio = 0.3
)

$ErrorActionPreference = 'Stop'
Set-Location $Root

$exe = Join-Path $Root 'rocks-demo\build\st_bucket_ingest_build.exe'
if (-not (Test-Path $exe)) {
  throw "Missing build artifact: $exe"
}

& $exe `
  --segment-meta-csv $MetaCsv `
  --segment-points-csv $PointsCsv `
  --out-sst-dir $SstDir `
  --target-db $Db `
  --bucket-sec $BucketSec `
  --block-size $BlockSize `
  --st-set-opt experimental_st_block_compaction_ratio=$BlockCompactionRatio `
  --user-key-schema e7 `
  --experimental-st-compaction `
  --reset-out-sst-dir `
  --reset-target-db
