param(
  [string]$Root = "D:\Project",
  [int]$Jobs = 8
)

$ErrorActionPreference = 'Stop'
Set-Location $Root

$buildDir = Join-Path $Root 'rocks-demo\build'
if (-not (Test-Path $buildDir)) {
  New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
}

Push-Location $buildDir
try {
  cmake -S .. -B .
  cmake --build . --config Release -j $Jobs
}
finally {
  Pop-Location
}
