param(
  [string]$Root = "D:\Project"
)

$ErrorActionPreference = 'Stop'
Set-Location $Root

$py = Join-Path $Root 'tools\benchmark\run_tdwx_ablation.py'
if (-not (Test-Path $py)) {
  throw "Missing ablation script: $py"
}

python $py
