param(
  [string]$Root = "D:\Project",
  [string]$RoundDir = ""
)

$ErrorActionPreference = 'Stop'
Set-Location $Root

$py = Join-Path $Root 'tools\verify_repeat_results.py'
if (-not (Test-Path $py)) {
  throw "Missing verify tool script: $py"
}

if ([string]::IsNullOrWhiteSpace($RoundDir)) {
  python $py --root $Root
} else {
  python $py --root $Root --round-dir $RoundDir
}
