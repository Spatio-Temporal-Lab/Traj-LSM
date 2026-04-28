param(
  [Parameter(Mandatory = $true)]
  [string]$DbPath,
  [string]$CompactExe = "",
  [int]$ExpectedBucketWidthSec = 86400,
  [switch]$AllowMissingHistorical,
  [switch]$RepairMissingHistorical,
  [switch]$SkipCompactCheck
)

$ErrorActionPreference = "Stop"

function Fail([string]$Msg) {
  Write-Host ""
  Write-Host "[PREFLIGHT FAIL] $Msg"
  exit 2
}

function Ok([string]$Msg) {
  Write-Host "[OK] $Msg"
}

if ([string]::IsNullOrWhiteSpace($CompactExe)) {
  $CompactExe = Join-Path (Get-Location) "rocks-demo\\build\\st_meta_compact_existing.exe"
}

if (-not (Test-Path -LiteralPath $DbPath)) {
  Fail "DB path not found: $DbPath"
}

$currentPath = Join-Path $DbPath "CURRENT"
if (-not (Test-Path -LiteralPath $currentPath)) {
  Fail "Missing CURRENT file under: $DbPath"
}

$currentTarget = (Get-Content -LiteralPath $currentPath -ErrorAction Stop | Select-Object -First 1).Trim()
if ([string]::IsNullOrWhiteSpace($currentTarget)) {
  Fail "CURRENT is empty"
}

if (-not $currentTarget.StartsWith("MANIFEST_SET-")) {
  Fail "CURRENT must point to MANIFEST_SET-*. Found: $currentTarget"
}
Ok "CURRENT -> $currentTarget"

$setPath = Join-Path $DbPath $currentTarget
if (-not (Test-Path -LiteralPath $setPath)) {
  Fail "CURRENT points to missing file: $setPath"
}

$setLines = Get-Content -LiteralPath $setPath -ErrorAction Stop
$entryLines = @($setLines | Where-Object { $_.Trim().StartsWith("entry=") })
if ($entryLines.Count -eq 0) {
  Fail "MANIFEST_SET has no entry= lines (expect v2 manifest-set). File: $currentTarget"
}
Ok ("MANIFEST_SET entries={0}" -f $entryLines.Count)

# Validate every listed MANIFEST exists before attempting to open the DB.
$entries = @()
$badTimeRangeEntries = @()
$unknownRangeCount = 0
foreach ($ln in $entryLines) {
  $body = $ln.Trim().Substring("entry=".Length)
  $comma = $body.IndexOf(",")
  if ($comma -lt 0) {
    Fail "Bad entry line (missing comma): $ln"
  }
  # entry format: entry=MANIFEST-xxxxxx,tmin=...,tmax=...
  $manifestName = $body.Substring(0, $comma).Trim()
  if ([string]::IsNullOrWhiteSpace($manifestName)) {
    Fail "Bad entry line (empty manifest name): $ln"
  }
  $mPath = Join-Path $DbPath $manifestName
  $exists = (Test-Path -LiteralPath $mPath)

  $tmin = $null
  $tmax = $null
  $parts = $body.Split(",")
  foreach ($part in $parts) {
    $kv = $part.Trim()
    if ($kv.StartsWith("tmin=")) {
      $v = $kv.Substring("tmin=".Length)
      [uint64]$tmp = 0
      if ([uint64]::TryParse($v, [ref]$tmp)) {
        $tmin = $tmp
      }
    } elseif ($kv.StartsWith("tmax=")) {
      $v = $kv.Substring("tmax=".Length)
      [uint64]$tmp = 0
      if ([uint64]::TryParse($v, [ref]$tmp)) {
        $tmax = $tmp
      }
    }
  }
  if ($null -eq $tmin -or $null -eq $tmax) {
    Fail "Bad entry line (missing/invalid tmin/tmax): $ln"
  }
  $isUnknown = ($tmin -eq 0 -and $tmax -eq [uint64]::MaxValue)
  if ($isUnknown) {
    $unknownRangeCount++
  } elseif ($tmin -gt $tmax) {
    $badTimeRangeEntries += $ln
  }
  $entries += [pscustomobject]@{
    Name = $manifestName
    Path = $mPath
    Exists = $exists
    TMin = $tmin
    TMax = $tmax
    RawLine = $ln
  }
}
Write-Host "[DIAG] MANIFEST_SET entry details:"
for ($i = 0; $i -lt $entries.Count; $i++) {
  $e = $entries[$i]
  $tag = if ($i -eq ($entries.Count - 1)) { "active" } else { "history" }
  $existsText = if ($e.Exists) { "exists" } else { "missing" }
  Write-Host ("  [{0}] {1} ({2}) tmin={3} tmax={4} path={5}" -f $tag, $e.Name, $existsText, $e.TMin, $e.TMax, $e.Path)
}

$missingEntries = @($entries | Where-Object { -not $_.Exists })
if ($missingEntries.Count -gt 0) {
  $activeEntry = $entries[$entries.Count - 1]
  if (-not $activeEntry.Exists) {
    Fail ("Active MANIFEST missing: {0}" -f $activeEntry.Name)
  }
  $missingHistorical = @($missingEntries | Where-Object { $_.Name -ne $activeEntry.Name })
  if ($missingHistorical.Count -gt 0) {
    $uniq = ($missingHistorical | Select-Object -ExpandProperty Name | Sort-Object -Unique) -join ", "
    if ($RepairMissingHistorical) {
      $survivorEntryLines = @($entries | Where-Object { $_.Exists } | ForEach-Object { $_.RawLine.Trim() })
      if ($survivorEntryLines.Count -eq 0) {
        Fail "Repair requested, but no existing MANIFEST entries survived."
      }
      $metaLines = @($setLines | Where-Object {
        $t = $_.Trim()
        ($t.StartsWith("version=") -or $t.StartsWith("generation="))
      })
      $newSetLines = @()
      $newSetLines += $metaLines
      $newSetLines += $survivorEntryLines
      $backupContent = ($setLines -join "`n") + "`n"
      $newContent = ($newSetLines -join "`n") + "`n"
      $backupPath = "${setPath}.bak"
      $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
      [System.IO.File]::WriteAllText($backupPath, $backupContent, $utf8NoBom)
      [System.IO.File]::WriteAllText($setPath, $newContent, $utf8NoBom)
      Write-Host ("[REPAIR] Backed up original MANIFEST_SET to: {0}" -f $backupPath)
      Write-Host ("[REPAIR] Rewrote MANIFEST_SET with existing entries only: {0}" -f ($survivorEntryLines -join ", "))
      Ok "MANIFEST_SET repaired (dropped missing historical references)"
    } elseif ($AllowMissingHistorical) {
      Write-Host ("[WARN] MANIFEST_SET references missing historical MANIFEST file(s): {0}" -f $uniq)
      Write-Host "[WARN] Continue because -AllowMissingHistorical is enabled."
    } else {
      Fail ("MANIFEST_SET references missing historical MANIFEST file(s): {0}. Rerun with -RepairMissingHistorical to rewrite MANIFEST_SET, or -AllowMissingHistorical for diagnostics only." -f $uniq)
    }
  }
}
Ok "MANIFEST_SET active MANIFEST exists"
if ($badTimeRangeEntries.Count -gt 0) {
  $detail = ($badTimeRangeEntries | Select-Object -First 3) -join " | "
  Fail "MANIFEST_SET has invalid time range entry(tmin>tmax). Example: $detail"
}
Ok "MANIFEST_SET time ranges are valid (tmin<=tmax or unknown)"
if ($unknownRangeCount -gt 1) {
  Fail "MANIFEST_SET has multiple unknown-range fallback shards (${unknownRangeCount}). Expect at most one."
}
Ok "fallback shard count is valid (<=1 unknown-range entry)"

if (-not $SkipCompactCheck) {
  if (-not (Test-Path -LiteralPath $CompactExe)) {
    Fail "Missing compact tool: $CompactExe"
  }

  Write-Host ""
  Write-Host "=== Checking CF compaction settings (dry-run) ==="
  $out = & $CompactExe --db $DbPath --dry-run --st-time-split --st-bucket-sec $ExpectedBucketWidthSec 2>&1
  if ($LASTEXITCODE -ne 0) {
    Write-Host $out
    Fail "st_meta_compact_existing --dry-run failed (exit=$LASTEXITCODE)"
  }

# Normalize output records (stdout/stderr may contain ErrorRecord objects).
$outLines = @()
foreach ($o in $out) {
  if ($null -eq $o) {
    continue
  }
  $line = [string]$o
  if (-not [string]::IsNullOrWhiteSpace($line)) {
    $outLines += $line.Trim()
  }
}

$optsLine = ($outLines | Where-Object { $_ -match "^AFTER_SETOPTIONS_OPTS\b" } | Select-Object -First 1)
if ($null -eq $optsLine) {
  $optsLine = ($outLines | Where-Object { $_ -match "^BEFORE_OPTS\b" } | Select-Object -First 1)
}
if ($null -eq $optsLine) {
  Write-Host ($outLines -join "`n")
  Fail "Could not find AFTER_SETOPTIONS_OPTS/BEFORE_OPTS line from dry-run output"
}
Write-Host $optsLine

if ($optsLine -notmatch "st_event_time_split=1") {
  Fail "experimental_compaction_event_time_split must be enabled (st_event_time_split=1). Fix: enable event-time split in CF options during DB build/compaction."
}
Ok "event-time split enabled"

$bucketWidth = $null
# NOTE: PowerShell string does not treat backslash as escape. Use `\d` directly
# for regex digit class (do NOT write `\\d` which would match a literal "\d").
if ($optsLine -match "bucket_width=(\d+)") {
  $bucketWidth = [int]$Matches[1]
}
if ($null -eq $bucketWidth -or $bucketWidth -le 0) {
  Fail ("Could not parse bucket_width from options line: {0}" -f $optsLine)
}

if ($bucketWidth -ne $ExpectedBucketWidthSec) {
  Fail "bucket_width must be ${ExpectedBucketWidthSec}s. Found: ${bucketWidth}. Fix: set experimental_compaction_event_time_bucket_width=${ExpectedBucketWidthSec}."
}
Ok "bucket width = ${bucketWidth}s (expected)"

}

Write-Host ""
Write-Host "[PREFLIGHT PASS]"
exit 0

