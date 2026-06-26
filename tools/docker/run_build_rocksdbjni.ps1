# Build linux64 rocksdbjni in Docker and zip for server upload.
# Usage: powershell -ExecutionPolicy Bypass -File D:\Project\tools\docker\run_build_rocksdbjni.ps1
# Optional: -BaseImage docker.m.daocloud.io/library/ubuntu:jammy

param(
    [string]$ProjectRoot = "D:\Project",
    [string]$BaseImage = "",
    [switch]$RebuildImage,
    [switch]$Force,
    [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"
$RocksdbSrc = Join-Path $ProjectRoot "rocksdb"
$ToolsDocker = Join-Path $ProjectRoot "tools\docker"
$OutHost = Join-Path $ProjectRoot "dist\tman_rocksdb_server_upload"
$ZipPath = Join-Path $ProjectRoot "dist\tman_rocksdb_server_upload.zip"
$Dockerfile = Join-Path $ToolsDocker "Dockerfile.rocksdbjni"
$ImageTag = "tman-rocksdbjni-builder:local"

if (-not (Test-Path $RocksdbSrc)) {
    throw "Missing rocksdb source: $RocksdbSrc"
}
if (-not (Test-Path $Dockerfile)) {
    throw "Missing Dockerfile: $Dockerfile"
}

function Test-DockerPull {
    param([string]$Image)
    Write-Host "[docker] pull $Image ..."
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & docker pull $Image 2>&1 | ForEach-Object { Write-Host $_ }
    $ok = ($LASTEXITCODE -eq 0)
    $ErrorActionPreference = $oldEap
    return $ok
}

docker version | Out-Null

# Mirror-first (docker.io often fails with registry-mirrors misconfigured)
$BaseCandidates = @(
    "docker.m.daocloud.io/library/ubuntu:jammy",
    "docker.m.daocloud.io/library/ubuntu:22.04",
    "registry.cn-hangzhou.aliyuncs.com/library/ubuntu:jammy",
    "registry.cn-hangzhou.aliyuncs.com/library/ubuntu:22.04",
    "docker.1ms.run/library/ubuntu:jammy",
    "ubuntu:jammy",
    "ubuntu:22.04"
)
if ($BaseImage) {
    $BaseCandidates = @($BaseImage) + $BaseCandidates
}

$ChosenBase = $null
foreach ($img in $BaseCandidates) {
    if (Test-DockerPull $img) {
        $ChosenBase = $img
        break
    }
}
if (-not $ChosenBase) {
    throw @"
Cannot pull any Ubuntu base image.

Try manually (pick one that succeeds):
  docker pull docker.m.daocloud.io/library/ubuntu:jammy
  docker pull registry.cn-hangzhou.aliyuncs.com/library/ubuntu:jammy

Then rerun:
  powershell -File D:\Project\tools\docker\run_build_rocksdbjni.ps1 -BaseImage <image-you-pulled>

Docker Desktop -> Settings -> Docker Engine: registry-mirrors alone may not fix docker.io;
use explicit mirror image name above.
"@
}
Write-Host "[docker] base image: $ChosenBase"

$existingJar = Get-ChildItem $OutHost -Filter "rocksdbjni-*-linux64.jar" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($existingJar -and -not $Force) {
    Write-Host "[skip] jar already exists: $($existingJar.FullName)"
    Write-Host "       use -Force to rebuild, or upload existing ZIP."
    & (Join-Path $ToolsDocker "pack_upload_zip.ps1") -ProjectRoot $ProjectRoot
    exit 0
}

docker image inspect $ImageTag 2>$null | Out-Null
$imageExists = ($LASTEXITCODE -eq 0)
if ($imageExists -and -not $RebuildImage) {
    Write-Host "[docker] reuse builder image $ImageTag (pass -RebuildImage after Dockerfile change)"
} else {
    Write-Host "[docker] building builder image (apt + toolchain + ccache + ninja) ..."
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & docker build -f $Dockerfile -t $ImageTag `
        --build-arg "BASE_IMAGE=$ChosenBase" `
        $ToolsDocker 2>&1 | ForEach-Object { Write-Host $_ }
    $ErrorActionPreference = $oldEap
    if ($LASTEXITCODE -ne 0) {
        throw "docker build failed (exit $LASTEXITCODE)"
    }
}

New-Item -ItemType Directory -Force -Path $OutHost | Out-Null
Get-ChildItem $OutHost -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

if ($Jobs -le 0) {
    $Jobs = [int]($env:NUMBER_OF_PROCESSORS)
    if ($Jobs -gt 16) { $Jobs = 16 }
}
$CcacheHost = Join-Path $ProjectRoot "dist\docker-ccache"
New-Item -ItemType Directory -Force -Path $CcacheHost | Out-Null

Write-Host "[docker] compiling rocksdbjni (linux64), jobs=$Jobs ; first run ~20-50 min ..."
$oldEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& docker run --rm `
    -e "JOBS=$Jobs" `
    -v "${RocksdbSrc}:/src/rocksdb" `
    -v "${OutHost}:/out" `
    -v "${CcacheHost}:/ccache" `
    $ImageTag 2>&1 | ForEach-Object { Write-Host $_ }
$ErrorActionPreference = $oldEap
if ($LASTEXITCODE -ne 0) {
    $log = Join-Path $OutHost "cmake-build.log"
    if (Test-Path $log) {
        Write-Host "--- tail cmake-build.log ---"
        Get-Content $log -Tail 60
    }
    throw "rocksdb compile failed (exit $LASTEXITCODE). See $log"
}

$jar = Get-ChildItem $OutHost -Filter "rocksdbjni-*-linux64.jar" | Select-Object -First 1
if (-not $jar) {
    throw "rocksdbjni linux jar not produced under $OutHost"
}

Copy-Item -Force (Join-Path $ToolsDocker "SERVER_README.txt") (Join-Path $OutHost "SERVER_README.txt")
Copy-Item -Force (Join-Path $ToolsDocker "version_set-popcnt.patch") (Join-Path $OutHost "version_set-popcnt.patch") -ErrorAction SilentlyContinue

if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path (Join-Path $OutHost "*") -DestinationPath $ZipPath -Force

Write-Host ""
Write-Host "Done."
Write-Host "  JAR : $($jar.FullName)"
Write-Host "  ZIP : $ZipPath"
Write-Host "Upload ZIP to server, unzip, follow SERVER_README.txt"
