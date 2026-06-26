# TDrive ST fork ingest with TMan-tsp LocSIndex ST keys (byte-identical to TrajPutUtil.getPut).
#
# 1) python tools/emit_tdrive_tman_st_keys.py  -> segments_st_key.csv
# 2) st_bucket_ingest_build --user-key-schema tman_locs --tman-st-keys-csv ...
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\build_tdrive_stream_st_tman_locs.ps1
#   ... -SkipEmitKeys   # CSV already exists

param(
  [string]$SegmentMetaCsv = "",
  [string]$SegmentPointsCsv = "",
  [string]$TmanStKeysCsv = "",
  [string]$OutSstDir = "",
  [string]$TargetDb = "",
  [int]$BucketSec = 3600,
  [string]$StBucketIngestBuildExe = "",
  [string]$Mvn = "",
  [switch]$SkipEmitKeys,
  [switch]$RunExtraLadderDrain
)

$ErrorActionPreference = "Stop"
$ToolsDir = $PSScriptRoot
$Root = [IO.Path]::GetFullPath((Join-Path $ToolsDir ".."))
if ([string]::IsNullOrWhiteSpace($SegmentMetaCsv)) {
  $SegmentMetaCsv = Join-Path $Root "data\processed\tdrive\segments_meta.csv"
}
if ([string]::IsNullOrWhiteSpace($SegmentPointsCsv)) {
  $SegmentPointsCsv = Join-Path $Root "data\processed\tdrive\segments_points.csv"
}
if ([string]::IsNullOrWhiteSpace($TmanStKeysCsv)) {
  $TmanStKeysCsv = Join-Path $Root "data\processed\tdrive\segments_st_key.csv"
}
if ([string]::IsNullOrWhiteSpace($OutSstDir)) {
  $OutSstDir = Join-Path $Root "data\runs\TDrive_sst_tmp_locs"
}
if ([string]::IsNullOrWhiteSpace($TargetDb)) {
  $TargetDb = Join-Path $Root "data\runs\TDrive_st_tman_locs_20260603"
}
if ([string]::IsNullOrWhiteSpace($StBucketIngestBuildExe)) {
  $StBucketIngestBuildExe = Join-Path $Root "rocks-demo\build\st_bucket_ingest_build.exe"
}

if (-not $SkipEmitKeys.IsPresent) {
  Write-Host "=== Emit TMan LocS ST keys (Java EmitSegmentStKeys) ===" -ForegroundColor Cyan
  $emitArgs = @("tools\emit_tdrive_tman_st_keys.py", "--out", $TmanStKeysCsv)
  if ($Mvn) { $emitArgs += @("--mvn", $Mvn) }
  & python @emitArgs
  if ($LASTEXITCODE -ne 0) { throw "emit_tdrive_tman_st_keys failed" }
}
if (-not (Test-Path -LiteralPath $TmanStKeysCsv)) {
  throw "Missing keys CSV: $TmanStKeysCsv (run emit or drop -SkipEmitKeys)"
}

if (-not (Test-Path -LiteralPath $StBucketIngestBuildExe)) {
  cmake --build (Join-Path $Root "rocks-demo\build") --target st_bucket_ingest_build
  if ($LASTEXITCODE -ne 0) { throw "cmake build st_bucket_ingest_build failed" }
}

$env:ROCKSDB_ST_USER_KEY_SCHEMA = "tman_st"
Write-Host "=== ST fork ingest: tman_locs keys | DB=$TargetDb ===" -ForegroundColor Cyan
& $StBucketIngestBuildExe `
  --segment-meta-csv $SegmentMetaCsv `
  --segment-points-csv $SegmentPointsCsv `
  --out-sst-dir $OutSstDir `
  --target-db $TargetDb `
  --bucket-sec $BucketSec `
  --user-key-schema tman_locs `
  --tman-st-keys-csv $TmanStKeysCsv `
  --reset-out-sst-dir `
  --reset-target-db `
  --experimental-st-compaction `
  --stream-ingest
if ($LASTEXITCODE -ne 0) { throw "st_bucket_ingest_build failed" }

$LevelStats = Join-Path $Root "rocks-demo\build\st_level_stats.exe"
if (Test-Path -LiteralPath $LevelStats) {
  Write-Host "`n=== st_level_stats (post-ingest) ===" -ForegroundColor Cyan
  & $LevelStats --db $TargetDb
}

# Ingest EOF already runs final_st_drain -> L1+; extra --force-l1-to-l2 can regress levels.
if ($RunExtraLadderDrain.IsPresent) {
  $ladderPs1 = Join-Path $ToolsDir "run_tdrive_st_l2_compact.ps1"
  if (Test-Path -LiteralPath $ladderPs1) {
    Write-Host "`n=== ST ladder drain (optional) ===" -ForegroundColor Cyan
    & $ladderPs1 -DbPath $TargetDb -BucketSec $BucketSec
  }
}

$Repair = Join-Path $Root "rocks-demo\build\traj_db_repair.exe"
if (Test-Path -LiteralPath $Repair) {
  Write-Host "`n=== traj_db_repair ===" -ForegroundColor Cyan
  & $Repair --db $TargetDb
}

Write-Host "`nDone. DB=$TargetDb" -ForegroundColor Green
