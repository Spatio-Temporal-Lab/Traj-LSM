param(
  [string]$DbRoot = (Join-Path $PSScriptRoot "..\data\experiments"),
  [string]$Csv = (Join-Path $PSScriptRoot "..\data\processed\segments_points.csv"),
  [string]$SmokeExe = (Join-Path $PSScriptRoot "..\rocks-demo\build\st_meta_smoke.exe"),
  [string]$CompactExe = (Join-Path $PSScriptRoot "..\rocks-demo\build\st_meta_compact_existing.exe"),
  [int]$Attempts = 6,
  [int]$MaxPoints = 2000,
  [int]$BucketSec = 86400,
  [int]$BucketCount = 8,
  [int]$TargetFileMb = 16,
  [int]$ExtraCompactRounds = 0,
  [switch]$DisableIncludePrev
)

$ErrorActionPreference = "Stop"

function Assert-Exists([string]$Path, [string]$Label) {
  if (-not (Test-Path -LiteralPath $Path)) {
    throw "Missing ${Label}: $Path"
  }
}

function Invoke-Step([string]$Label, [string]$Exe, [string[]]$ArgumentList) {
  Write-Host ""
  Write-Host "=== $Label ==="
  Write-Host "$Exe $($ArgumentList -join ' ')"
  & $Exe @ArgumentList
  if ($LASTEXITCODE -ne 0) {
    throw "$Label failed with exit code $LASTEXITCODE"
  }
}

function Read-CurrentTarget([string]$DbPath) {
  $current = Join-Path $DbPath "CURRENT"
  if (-not (Test-Path -LiteralPath $current)) {
    return $null
  }
  $line = (Get-Content -LiteralPath $current -ErrorAction Stop | Select-Object -First 1).Trim()
  if ([string]::IsNullOrWhiteSpace($line)) {
    return $null
  }
  return $line
}

function Get-ManifestSetEntryCount([string]$SetPath) {
  $lines = Get-Content -LiteralPath $SetPath -ErrorAction Stop
  $entryCount = ($lines | Where-Object {
      $_.Trim().StartsWith("entry=") -or $_.Trim().StartsWith("MANIFEST-")
    }).Count
  return $entryCount
}

function Get-MaxEntryManifestSet([string]$DbPath) {
  $files = Get-ChildItem -LiteralPath $DbPath -Filter "MANIFEST_SET-*" -File -ErrorAction SilentlyContinue
  if (-not $files -or $files.Count -eq 0) {
    return $null
  }
  $best = $null
  foreach ($f in $files) {
    $count = Get-ManifestSetEntryCount -SetPath $f.FullName
    if ($null -eq $best -or $count -gt $best.EntryCount) {
      $best = [PSCustomObject]@{
        Name = $f.Name
        FullName = $f.FullName
        EntryCount = $count
      }
    }
  }
  return $best
}

function Test-RouteLogTriggered([string]$DbPath) {
  $logPath = Join-Path $DbPath "LOG"
  if (-not (Test-Path -LiteralPath $logPath)) {
    return $false
  }
  $hit = Select-String -LiteralPath $logPath -Pattern "experimental_manifest_shard_route enabled=1" -SimpleMatch -ErrorAction SilentlyContinue
  return ($null -ne $hit)
}

Assert-Exists -Path $Csv -Label "CSV"
Assert-Exists -Path $SmokeExe -Label "st_meta_smoke"
Assert-Exists -Path $DbRoot -Label "DbRoot"
if ($ExtraCompactRounds -gt 0) {
  Assert-Exists -Path $CompactExe -Label "st_meta_compact_existing"
}

$env:ROCKSDB_EXPERIMENTAL_MANIFEST_SET_WRITE = "1"
$env:ROCKSDB_EXPERIMENTAL_MANIFEST_SHARDING_ENABLE = "1"
$env:ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE = "1"
$env:ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC = "$BucketSec"
$env:ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_COUNT = "$BucketCount"
$env:ROCKSDB_EXPERIMENTAL_MANIFEST_SET_INCLUDE_PREV = $(if ($DisableIncludePrev) { "0" } else { "1" })
$env:ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_FORCE = ""
$env:ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_FORCE_NEW_DESCRIPTOR = ""

$runTag = "manifest_set_natural_multi_entry_{0:yyyyMMdd_HHmmss}" -f (Get-Date)
$runRoot = Join-Path $DbRoot $runTag
New-Item -ItemType Directory -Path $runRoot | Out-Null

$summary = @()
$success = $false
$successAttempt = 0
$successDb = $null
$successSet = $null

Write-Host "Run root: $runRoot"
Write-Host "Attempts: $Attempts"

for ($i = 1; $i -le $Attempts; $i++) {
  $db = Join-Path $runRoot ("db_try_{0:D2}" -f $i)
  New-Item -ItemType Directory -Path $db | Out-Null

  Invoke-Step -Label ("try#{0} ingest+compact-after-ingest" -f $i) -Exe $SmokeExe -ArgumentList @(
    "--db", $db,
    "--csv", $Csv,
    "--st-keys",
    "--max-points", "$MaxPoints",
    "--st-compaction-time-split",
    "--st-compaction-bucket-sec", "$BucketSec",
    "--compact-after-ingest"
  )

  for ($round = 1; $round -le $ExtraCompactRounds; $round++) {
    Invoke-Step -Label ("try#{0} extra-compact#{1}" -f $i, $round) -Exe $CompactExe -ArgumentList @(
      "--db", $db,
      "--target-file-mb", "$TargetFileMb",
      "--st-time-split",
      "--st-bucket-sec", "$BucketSec",
      "--st-min-keys", "1"
    )
  }

  $currentTarget = Read-CurrentTarget -DbPath $db
  $routeTriggered = Test-RouteLogTriggered -DbPath $db
  $bestSet = Get-MaxEntryManifestSet -DbPath $db
  $entryCount = 0
  $setName = ""
  if ($null -ne $bestSet) {
    $entryCount = $bestSet.EntryCount
    $setName = $bestSet.Name
  }

  $row = [PSCustomObject]@{
    attempt = $i
    db = $db
    current = $currentTarget
    route_triggered = $routeTriggered
    best_manifest_set = $setName
    best_entry_count = $entryCount
  }
  $summary += $row
  Write-Host ("try#{0}: current={1} route={2} best_set={3} entries={4}" -f $i, $currentTarget, $routeTriggered, $setName, $entryCount)

  if ($entryCount -ge 2) {
    $success = $true
    $successAttempt = $i
    $successDb = $db
    $successSet = $bestSet
    break
  }
}

$summaryPath = Join-Path $runRoot "summary.tsv"
"attempt`tcurrent`troute_triggered`tbest_manifest_set`tbest_entry_count`tdb" | Set-Content -LiteralPath $summaryPath -Encoding utf8
foreach ($r in $summary) {
  ("{0}`t{1}`t{2}`t{3}`t{4}`t{5}" -f $r.attempt, $r.current, $r.route_triggered, $r.best_manifest_set, $r.best_entry_count, $r.db) | Add-Content -LiteralPath $summaryPath -Encoding utf8
}

if ($success) {
  $proofPath = Join-Path $runRoot "success_manifest_set.txt"
  Get-Content -LiteralPath $successSet.FullName | Set-Content -LiteralPath $proofPath -Encoding utf8
  Write-Host ""
  Write-Host "[SUCCESS] natural multi-entry MANIFEST_SET captured."
  Write-Host ("attempt={0}" -f $successAttempt)
  Write-Host ("db={0}" -f $successDb)
  Write-Host ("manifest_set={0}" -f $successSet.Name)
  Write-Host ("entry_count={0}" -f $successSet.EntryCount)
  Write-Host ("summary={0}" -f $summaryPath)
  Write-Host ("proof={0}" -f $proofPath)
  exit 0
}

Write-Host ""
Write-Host "[FAILED] no MANIFEST_SET with >=2 entries found in this run."
Write-Host ("summary={0}" -f $summaryPath)
exit 2
