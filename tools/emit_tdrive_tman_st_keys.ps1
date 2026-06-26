# Windows wrapper: emit segments_st_key.csv for tman_st ingest.
#
# Usage (cmd or PowerShell):
#   powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\emit_tdrive_tman_st_keys.ps1
#   powershell ... -MvnCmd "D:\apache-maven-3.9.16\bin\mvn.cmd"

param(
  [string]$MetaCsv = "",
  [string]$PointsCsv = "",
  [string]$OutCsv = "",
  [string]$MvnCmd = ""
)

$ErrorActionPreference = "Stop"
$ToolsDir = $PSScriptRoot
$Root = [System.IO.Path]::GetFullPath((Join-Path $ToolsDir ".."))
$Py = Join-Path $ToolsDir "emit_tdrive_tman_st_keys.py"

if (-not [string]::IsNullOrWhiteSpace($MvnCmd)) {
  $env:MAVEN_HOME = [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetDirectoryName($MvnCmd))
}

$argsPy = @($Py)
if (-not [string]::IsNullOrWhiteSpace($MetaCsv)) { $argsPy += @("--meta", $MetaCsv) }
if (-not [string]::IsNullOrWhiteSpace($PointsCsv)) { $argsPy += @("--points", $PointsCsv) }
if (-not [string]::IsNullOrWhiteSpace($OutCsv)) { $argsPy += @("--out", $OutCsv) }

$mvn = $MvnCmd
if ([string]::IsNullOrWhiteSpace($mvn)) {
  foreach ($c in @(
      "D:\apache-maven-3.9.16\bin\mvn.cmd",
      "D:\apache-maven\bin\mvn.cmd",
      "C:\apache-maven\bin\mvn.cmd"
    )) {
    if (Test-Path -LiteralPath $c) { $mvn = $c; break }
  }
}
if (-not [string]::IsNullOrWhiteSpace($mvn)) {
  $argsPy += @("--mvn", $mvn)
}

& python @argsPy
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
