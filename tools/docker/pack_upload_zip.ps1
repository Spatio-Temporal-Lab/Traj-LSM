# Pack rocksdbjni jar (if built) + server instructions into one zip.
# Run AFTER run_build_rocksdbjni.ps1 succeeded, or to repack existing jar.
param(
    [string]$ProjectRoot = "D:\Project"
)

$ErrorActionPreference = "Stop"
$OutHost = Join-Path $ProjectRoot "dist\tman_rocksdb_server_upload"
$ZipPath = Join-Path $ProjectRoot "dist\tman_rocksdb_server_upload.zip"
$Readme = Join-Path $ProjectRoot "tools\docker\SERVER_README.txt"

New-Item -ItemType Directory -Force -Path $OutHost | Out-Null

$jar = Get-ChildItem $OutHost -Filter "rocksdbjni-*-linux64.jar" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $jar) {
    Write-Warning "No rocksdbjni-*-linux64.jar in $OutHost — run run_build_rocksdbjni.ps1 first."
}

Copy-Item -Force $Readme (Join-Path $OutHost "SERVER_README.txt")
Copy-Item -Force (Join-Path $ProjectRoot "tools\docker\version_set-popcnt.patch") (Join-Path $OutHost "version_set-popcnt.patch") -ErrorAction SilentlyContinue

if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path (Join-Path $OutHost "*") -DestinationPath $ZipPath -Force
Write-Host "ZIP: $ZipPath"
Get-ChildItem $ZipPath | Format-List Name, Length, LastWriteTime
