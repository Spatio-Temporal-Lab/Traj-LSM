# Build TDrive ST fork DB: stream ingest + experimental ST-compaction (hourly buckets).
# Default: unified EOF flush (not one-SST-per-bucket). Use -PerBucketFlush for legacy 149×L1.
#
# Prerequisites: rocks-demo st_bucket_ingest_build.exe; data/processed/tdrive/*.csv
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\build_tdrive_stream_st.ps1

param(
  [string]$SegmentMetaCsv = "",
  [string]$SegmentPointsCsv = "",
  [string]$OutSstDir = "",
  [string]$TargetDb = "",
  [int]$BucketSec = 3600,
  [string]$StBucketIngestBuildExe = "",
  [switch]$ExperimentalStCompaction = $true,
  [switch]$NoStCompaction,
  [switch]$PerBucketFlush
)

$ErrorActionPreference = "Stop"
$ToolsDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ToolsDir)) {
  $ToolsDir = Split-Path -LiteralPath $MyInvocation.MyCommand.Path -Parent
}
$Root = [System.IO.Path]::GetFullPath((Join-Path $ToolsDir ".."))
if ([string]::IsNullOrWhiteSpace($SegmentMetaCsv)) {
  $SegmentMetaCsv = Join-Path $Root "data\processed\tdrive\segments_meta.csv"
}
if ([string]::IsNullOrWhiteSpace($SegmentPointsCsv)) {
  $SegmentPointsCsv = Join-Path $Root "data\processed\tdrive\segments_points.csv"
}
if ([string]::IsNullOrWhiteSpace($OutSstDir)) {
  $OutSstDir = Join-Path $Root "data\runs\TDrive_sst_tmp"
}
if ([string]::IsNullOrWhiteSpace($TargetDb)) {
  $TargetDb = Join-Path $Root "data\runs\TDrive_st_unified_20260527"
}
if ([string]::IsNullOrWhiteSpace($StBucketIngestBuildExe)) {
  $StBucketIngestBuildExe = Join-Path $Root "rocks-demo\build\st_bucket_ingest_build.exe"
}
if (-not (Test-Path -LiteralPath $StBucketIngestBuildExe)) {
  throw "Missing $StBucketIngestBuildExe — cmake --build rocks-demo/build --target st_bucket_ingest_build"
}
if (-not (Test-Path -LiteralPath $SegmentMetaCsv)) {
  throw "Missing segment meta CSV: $SegmentMetaCsv"
}
if (-not (Test-Path -LiteralPath $SegmentPointsCsv)) {
  throw "Missing segment points CSV: $SegmentPointsCsv"
}

$stc = $ExperimentalStCompaction.IsPresent -and -not $NoStCompaction.IsPresent
# Single MANIFEST + per-SST st_file_meta (Mind.md); ingest clears legacy shard-route env.
$env:ROCKSDB_ST_USER_KEY_SCHEMA = "tman_st"
Write-Host "=== TDrive ST ingest: key=tman_st (C++) | Global/Local=ST fork | ST-compaction=$stc | bucket_sec=$BucketSec ===" -ForegroundColor Cyan
Write-Host "    -> $TargetDb" -ForegroundColor Cyan
$ingestArgs = @(
  "--segment-meta-csv", $SegmentMetaCsv,
  "--segment-points-csv", $SegmentPointsCsv,
  "--out-sst-dir", $OutSstDir,
  "--target-db", $TargetDb,
  "--bucket-sec", "$BucketSec",
  "--user-key-schema", "tman_st",
  "--reset-out-sst-dir",
  "--reset-target-db"
)
if ($stc) {
  $ingestArgs += "--experimental-st-compaction", "--stream-ingest"
  if ($PerBucketFlush.IsPresent) {
    $ingestArgs += "--per-bucket-flush"
    Write-Host "    flush: per-bucket (legacy)" -ForegroundColor Yellow
  } else {
    Write-Host "    flush: unified EOF + L0 drain + ST ladder (default)" -ForegroundColor Cyan
  }
} else {
  Write-Host "WARN: ST-compaction OFF (-NoStCompaction)" -ForegroundColor Yellow
}
& $StBucketIngestBuildExe @ingestArgs
if ($LASTEXITCODE -ne 0) {
  throw "st_bucket_ingest_build failed (exit $LASTEXITCODE)"
}

$n = 0
if (Test-Path -LiteralPath $TargetDb) {
  $n = (Get-ChildItem -LiteralPath $TargetDb -Filter "*.sst" -File -ErrorAction SilentlyContinue | Measure-Object).Count
}
Write-Host "Live *.sst count under TDrive DB: $n" -ForegroundColor Green
Write-Host "DB path: $TargetDb" -ForegroundColor Green

$StLevelStats = Join-Path $Root "rocks-demo\build\st_level_stats.exe"
$TrajDbRepair = Join-Path $Root "rocks-demo\build\traj_db_repair.exe"
if (Test-Path -LiteralPath $TrajDbRepair) {
  Write-Host "`n=== Post-ingest RepairDB (drop stale manifest SST refs) ===" -ForegroundColor Cyan
  & $TrajDbRepair --db $TargetDb
  if ($LASTEXITCODE -ne 0) {
    throw "traj_db_repair failed (exit $LASTEXITCODE)"
  }
} else {
  Write-Host "WARN: skip traj_db_repair — build rocks-demo traj_db_repair first" -ForegroundColor Yellow
}
if (Test-Path -LiteralPath $StLevelStats) {
  Write-Host "`n=== Post-ingest offline verify (st_level_stats, no DB::Open) ===" -ForegroundColor Cyan
  & $StLevelStats --db $TargetDb
  if ($LASTEXITCODE -ne 0) {
    throw "st_level_stats failed (exit $LASTEXITCODE)"
  }
} else {
  Write-Host "WARN: skip st_level_stats — build rocks-demo st_level_stats first" -ForegroundColor Yellow
}
Write-Host "NOTE: run traj_db_repair after ingest before benchmarks; audit may rewrite M_manager shards." -ForegroundColor Yellow
