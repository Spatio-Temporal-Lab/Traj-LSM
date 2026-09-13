param(
  [string]$Root = "D:\Project",
  [int]$Rounds = 10
)

$ErrorActionPreference = 'Stop'
Set-Location $Root

$py = Join-Path $Root 'tools\benchmark\run_tdrive_st_query_bench.py'
$verify = Join-Path $Root 'scripts\verify-repeat-results.ps1'
if (-not (Test-Path $py)) { throw "Missing tool script: $py" }
if (-not (Test-Path $verify)) { throw "Missing verify script: $verify" }

function New-TimestampId {
  Get-Date -Format 'yyyyMMdd_HHmmss'
}

$outRoot = Join-Path $Root 'Result\TSV\tdrive_st_query_repeat'
New-Item -ItemType Directory -Force $outRoot | Out-Null
Get-ChildItem -Path $outRoot -Directory -Filter '*_round_*' | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path $outRoot 'repeat_index.json') -Force -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path $outRoot 'repeat_summary.json') -Force -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path $outRoot 'verify_summary.json') -Force -ErrorAction SilentlyContinue

$index = @()
for ($i = 1; $i -le $Rounds; $i++) {
  $stamp = New-TimestampId
  $runDir = Join-Path $outRoot ("{0}_round_{1:D2}" -f $stamp, $i)
  New-Item -ItemType Directory -Force $runDir | Out-Null
  $queryRoot = Join-Path $Root 'data\processed\tman_tdrive\tdrive_query_st'
  python $py --query-root $queryRoot --suites unified_12 --out-dir $runDir
  if ($LASTEXITCODE -eq 0) {
    & powershell -ExecutionPolicy Bypass -File $verify -Root $Root -RoundDir $runDir | Out-Null
  }
  $index += [ordered]@{
    timestamp = $stamp
    round = $i
    path = $runDir
  }
  Start-Sleep -Milliseconds 250
}

@"
import csv
import json
from pathlib import Path

root = Path(r'$outRoot')
runs = sorted([p for p in root.iterdir() if p.is_dir() and '_round_' in p.name])
metric = 'unified_12'
times = []
finals = []
for run in runs:
    csv_path = run / f'st_prune_{metric}.csv'
    if not csv_path.is_file():
        continue
    with csv_path.open('r', encoding='utf-8') as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        if row['type'] == 'time':
            times.append(float(row['avg']))
        elif row['type'] == 'finalSize':
            finals.append(float(row['avg']))

verify = root / 'verify_summary.json'
verify_data = json.loads(verify.read_text(encoding='utf-8')) if verify.is_file() else {}
summary = {
    metric: {
        'runs': len(times),
        'time_avg_of_avg': sum(times) / len(times) if times else 0.0,
        'finalSize_avg_of_avg': sum(finals) / len(finals) if finals else 0.0,
    },
    'run_count': len(runs),
    'verification': verify_data,
    'generated_at': __import__('datetime').datetime.now().isoformat(),
}

out = root / 'repeat_summary.json'
out.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
idx = root / 'repeat_index.json'
idx.write_text(json.dumps({
    'generated_at': __import__('datetime').datetime.now().isoformat(),
    'run_count': len(runs),
    'runs': [
        {'timestamp': p.name.split('_round_')[0], 'round': int(p.name.split('_round_')[1]), 'path': str(p)}
        for p in runs
    ],
}, indent=2) + '\n', encoding='utf-8')
print(out)
print(idx)
"@ | python -

& powershell -ExecutionPolicy Bypass -File $verify -Root $Root | Out-Null
