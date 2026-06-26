param(
  [string]$Root = "D:\Project",
  [string]$Db = "D:\Project\data\runs\TDrive",
  [string]$Input = "D:\Project\data\generated_trajs_segments.jsonl"
)

$ErrorActionPreference = 'Stop'
Set-Location $Root

$exe = Join-Path $Root 'rocks-demo\build\st_bucket_ingest_build.exe'
if (-not (Test-Path $exe)) {
  throw "Missing build artifact: $exe"
}

& $exe --db $Db --input $Input
