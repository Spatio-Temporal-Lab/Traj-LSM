<#
Reproducible JSONL -> vanilla RocksDB ingest + summary bundle.

Conventions:
- DB output under D:/Project/db_ingest/... (git-ignored)
- Results under D:/Project/Result (capitalized)

Example:
  powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\ingest_jsonl_and_summarize.ps1 `
    -SplitPolicy p80_120 -BlockSize 65536 -Compression snappy
#>

param(
  [ValidateSet("unsplit", "p80_120", "p200_300")]
  [string]$SplitPolicy = "p80_120",

  [string]$JsonlPath = "",

  [ValidateSet("none", "snappy", "zstd", "lz4")]
  [string]$Compression = "snappy",

  [UInt64]$BlockSize = 65536,

  [switch]$Compact,

  [string]$DbRoot = "",

  [string]$ResultRoot = "",

  [string]$RunTag = "",

  [string]$RocksDemoBuildDir = "",

  [string]$SstDumpExe = "",

  [switch]$SstDumpSample10Blocks
)

$ErrorActionPreference = "Stop"

function Resolve-AbsPath {
  param([string]$Path)
  if ([string]::IsNullOrWhiteSpace($Path)) { return "" }
  try { return (Resolve-Path -LiteralPath $Path).Path } catch { return $Path }
}

function Ensure-Dir {
  param([string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) {
    New-Item -ItemType Directory -Path $Path | Out-Null
  }
}

function Get-DirSizeBytes {
  param([string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) { return 0 }
  $sum = 0L
  Get-ChildItem -LiteralPath $Path -Recurse -File | ForEach-Object { $sum += $_.Length }
  return [Int64]$sum
}

function Write-JsonFile {
  param([string]$Path, [object]$Obj)
  $json = $Obj | ConvertTo-Json -Depth 20
  [System.IO.File]::WriteAllText($Path, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))
}

$ToolsDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ToolsDir)) {
  $ToolsDir = Split-Path -LiteralPath $MyInvocation.MyCommand.Path -Parent
}
$ProjectRoot = Resolve-AbsPath (Join-Path $ToolsDir "..")

if ([string]::IsNullOrWhiteSpace($DbRoot)) {
  $DbRoot = Join-Path $ProjectRoot "db_ingest"
}
if ([string]::IsNullOrWhiteSpace($ResultRoot)) {
  $ResultRoot = Join-Path $ProjectRoot "Result"
}
Ensure-Dir $DbRoot
Ensure-Dir $ResultRoot

if ([string]::IsNullOrWhiteSpace($JsonlPath)) {
  if ($SplitPolicy -eq "unsplit") {
    $JsonlPath = Join-Path $ProjectRoot "data\generated_trajs_segments.jsonl"
  } elseif ($SplitPolicy -eq "p80_120") {
    $JsonlPath = Join-Path $ProjectRoot "data\generated_trajs_segments_p80_120.jsonl"
  } elseif ($SplitPolicy -eq "p200_300") {
    $JsonlPath = Join-Path $ProjectRoot "data\generated_trajs_segments_p200_300.jsonl"
  }
}
$JsonlPath = Resolve-AbsPath $JsonlPath
if (-not (Test-Path -LiteralPath $JsonlPath)) {
  throw "JSONL not found: $JsonlPath"
}

if ([string]::IsNullOrWhiteSpace($RunTag)) {
  $RunTag = (Get-Date).ToString("yyyyMMdd_HHmmss")
}

if ([string]::IsNullOrWhiteSpace($RocksDemoBuildDir)) {
  $RocksDemoBuildDir = Join-Path $ProjectRoot "rocks-demo\build"
}
$RocksDemoBuildDir = Resolve-AbsPath $RocksDemoBuildDir

$IngestExe = Join-Path $RocksDemoBuildDir "st_vanilla_kv_stream_ingest.exe"
$PropsExe = Join-Path $RocksDemoBuildDir "st_vanilla_table_props.exe"

function Ensure-RocksDemoTools {
  param([string]$BuildDir, [string[]]$Targets)

  $cmakeLists = Join-Path $ProjectRoot "rocks-demo\CMakeLists.txt"
  if (-not (Test-Path -LiteralPath $cmakeLists)) {
    throw "Missing rocks-demo/CMakeLists.txt at $cmakeLists"
  }

  $needBuild = $false
  $srcProps = Join-Path $ProjectRoot "rocks-demo\st_vanilla_table_props.cpp"
  $srcIngest = Join-Path $ProjectRoot "rocks-demo\st_vanilla_kv_stream_ingest.cpp"
  $srcNewest = $null
  foreach ($s in @($srcProps, $srcIngest)) {
    if (Test-Path -LiteralPath $s) {
      $t = (Get-Item -LiteralPath $s).LastWriteTimeUtc
      if ($null -eq $srcNewest -or $t -gt $srcNewest) { $srcNewest = $t }
    }
  }
  foreach ($t in $Targets) {
    $exe = Join-Path $BuildDir $t
    if (-not (Test-Path -LiteralPath $exe)) {
      $needBuild = $true
      continue
    }
    if ($null -ne $srcNewest) {
      $et = (Get-Item -LiteralPath $exe).LastWriteTimeUtc
      if ($srcNewest -gt $et) { $needBuild = $true }
    }
  }
  if (-not $needBuild) { return }

  Ensure-Dir $BuildDir

  $cmake = "cmake"
  $cache = Join-Path $BuildDir "CMakeCache.txt"

  Push-Location $ProjectRoot
  try {
    if (-not (Test-Path -LiteralPath $cache)) {
      # Do NOT force Ninja: many Windows setups don't have it. Let CMake choose a default generator.
      & $cmake -S "rocks-demo" -B $BuildDir -DCMAKE_BUILD_TYPE=Release | Out-Host
      if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
    }

    & $cmake --build $BuildDir --config Release --target st_vanilla_kv_stream_ingest --target st_vanilla_table_props | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cmake --build failed ($LASTEXITCODE)" }
  } finally {
    Pop-Location
  }
}

Ensure-RocksDemoTools -BuildDir $RocksDemoBuildDir -Targets @("st_vanilla_kv_stream_ingest.exe", "st_vanilla_table_props.exe")
if (-not (Test-Path -LiteralPath $IngestExe)) { throw "Missing: $IngestExe" }
if (-not (Test-Path -LiteralPath $PropsExe)) { throw "Missing: $PropsExe" }

$DbOutDir = Join-Path $DbRoot ("vanilla_jsonl\" + $SplitPolicy + "\" + $Compression + "\b" + $BlockSize + "\" + $RunTag)
Ensure-Dir $DbOutDir

$RunOutDir = Join-Path $ResultRoot ("vanilla_jsonl_ingest\" + $SplitPolicy + "\" + $Compression + "\b" + $BlockSize + "\" + $RunTag)
Ensure-Dir $RunOutDir

$cmdFile = Join-Path $RunOutDir "commands.txt"
$ingestLog = Join-Path $RunOutDir "ingest_stdout.txt"
$propsTsv = Join-Path $RunOutDir "table_props.tsv"
$propsJson = Join-Path $RunOutDir "table_props.json"
$summaryJson = Join-Path $RunOutDir "summary.json"

$ingestArgs = @(
  "--db", $DbOutDir,
  "--jsonl", $JsonlPath,
  "--compression", $Compression,
  "--block-size", "$BlockSize"
)
if ($Compact.IsPresent) { $ingestArgs += "--compact" }

$propsArgs = @("--db", $DbOutDir, "--out-tsv", $propsTsv, "--out-json", $propsJson)

@(
  "st_vanilla_kv_stream_ingest: `"$IngestExe`" " + ($ingestArgs -join " "),
  "st_vanilla_table_props:     `"$PropsExe`" " + ($propsArgs -join " ")
) | Set-Content -LiteralPath $cmdFile -Encoding UTF8

Push-Location $ProjectRoot
try {
  $cap = New-Object System.Collections.Generic.List[string]
  & $IngestExe @ingestArgs 2>&1 | ForEach-Object {
    if ($_ -is [System.Management.Automation.ErrorRecord]) { $cap.Add($_.ToString()) }
    else { $cap.Add("$($_)") }
  } | Out-Null
  $capText = ($cap -join "`n").TrimEnd()
  Set-Content -LiteralPath $ingestLog -Value $capText -Encoding UTF8
  if ($LASTEXITCODE -ne 0) { throw "ingest failed ($LASTEXITCODE). See $ingestLog" }

  & $PropsExe @propsArgs | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "table props failed ($LASTEXITCODE)" }
  if (-not (Test-Path -LiteralPath $propsTsv) -or -not (Test-Path -LiteralPath $propsJson)) {
    throw "table props did not produce expected outputs ($propsTsv, $propsJson). Rebuild rocks-demo tools and retry."
  }
} finally {
  Pop-Location
}

# Optional: pick one .sst and sample 10 blocks from sst_dump --command=raw output
$sstDumpRun = $null
if ($SstDumpSample10Blocks.IsPresent) {
  if ([string]::IsNullOrWhiteSpace($SstDumpExe)) {
    $cand = @(
      (Join-Path $ProjectRoot "rocksdb_vanilla\build_snappy3\tools\sst_dump.exe"),
      (Join-Path $ProjectRoot "rocksdb_vanilla\build\tools\sst_dump.exe")
    )
    foreach ($c in $cand) { if (Test-Path -LiteralPath $c) { $SstDumpExe = $c; break } }
  }
  if (-not (Test-Path -LiteralPath $SstDumpExe)) {
    throw "SstDumpSample10Blocks requested but sst_dump.exe not found. Pass -SstDumpExe <path>."
  }

  $sstFiles = @(Get-ChildItem -LiteralPath $DbOutDir -Recurse -File -Filter "*.sst" | Sort-Object Length -Descending)
  if ($sstFiles.Count -eq 0) {
    throw "No .sst files found under $DbOutDir"
  }
  $pick = $sstFiles[0].FullName
  $dumpTxt = ($pick -replace '\.sst$', '_dump.txt')
  $rawDump = Join-Path $RunOutDir "sst_dump_raw_dump.txt"
  $sampleOut = Join-Path $RunOutDir "sst_dump_sampled_10_blocks.txt"
  $sampleMeta = Join-Path $RunOutDir "sst_dump_sampled_10_blocks.json"

  # Note: rocksdb's sst_dump --command=raw writes to a sibling *_dump.txt file, not stdout.
  & $SstDumpExe "--file=$pick" "--command=raw" | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "sst_dump failed ($LASTEXITCODE)" }
  if (-not (Test-Path -LiteralPath $dumpTxt)) {
    throw "sst_dump did not produce expected dump file: $dumpTxt"
  }
  Copy-Item -LiteralPath $dumpTxt -Destination $rawDump -Force

  $py = "python"
  $sampler = Join-Path $ProjectRoot "tools\sst_dump_sample_blocks.py"
  & $py $sampler --in $rawDump --out $sampleOut --count 10 --meta-out $sampleMeta | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "sst_dump_sample_blocks.py failed ($LASTEXITCODE)" }

  $sstDumpRun = [PSCustomObject]@{
    sst_dump_exe = (Resolve-AbsPath $SstDumpExe)
    sampled_sst = $pick
    raw_dump_txt = $rawDump
    sampled_blocks_txt = $sampleOut
    sampled_blocks_meta_json = $sampleMeta
  }
}

# Parse ingest counts from stdout (best-effort)
$ingestWritten = $null
$ingestSkipped = $null
$ingestText = Get-Content -LiteralPath $ingestLog -Raw
if ($ingestText -match 'vanilla_jsonl_ingest wrote (\d+) keys .* \(skipped=(\d+)\)') {
  $ingestWritten = [Int64]$Matches[1]
  $ingestSkipped = [Int64]$Matches[2]
}

$dbBytes = Get-DirSizeBytes $DbOutDir
$sstList = @(Get-ChildItem -LiteralPath $DbOutDir -Recurse -File -Filter "*.sst" | Sort-Object FullName)
$sstBytes = 0L
foreach ($f in $sstList) { $sstBytes += $f.Length }

$propsObj = $null
if (Test-Path -LiteralPath $propsJson) {
  $propsObj = Get-Content -LiteralPath $propsJson -Raw | ConvertFrom-Json
}

$summary = [PSCustomObject]@{
  run_tag = $RunTag
  split_policy = $SplitPolicy
  jsonl_path = $JsonlPath
  compression = $Compression
  block_size = [Int64]$BlockSize
  compact = [bool]$Compact.IsPresent

  db_out_dir = $DbOutDir
  result_out_dir = $RunOutDir

  ingest = [PSCustomObject]@{
    written_keys = $ingestWritten
    skipped_lines = $ingestSkipped
    ingest_stdout_txt = $ingestLog
  }

  sizes = [PSCustomObject]@{
    db_total_bytes = $dbBytes
    sst_count = $sstList.Count
    sst_total_bytes = $sstBytes
  }

  table_props = [PSCustomObject]@{
    tsv = $propsTsv
    json = $propsJson
    parsed = $propsObj
  }

  sst_dump_sampling = $sstDumpRun
  commands_txt = $cmdFile
}

Write-JsonFile -Path $summaryJson -Obj $summary

Write-Host "OK"
Write-Host "DB:      $DbOutDir"
Write-Host "Result:  $RunOutDir"
Write-Host "Summary: $summaryJson"

