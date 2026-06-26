@echo off
REM Build linux64 rocksdbjni in Docker and create upload zip.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_build_rocksdbjni.ps1"
if errorlevel 1 (
  echo.
  echo BUILD FAILED. If docker pull failed, run:
  echo   docker pull docker.m.daocloud.io/library/ubuntu:jammy
  echo Then:
  echo   powershell -File "%~dp0run_build_rocksdbjni.ps1" -BaseImage docker.m.daocloud.io/library/ubuntu:jammy
  pause
  exit /b 1
)
echo.
echo Upload this file to the server:
echo   D:\Project\dist\tman_rocksdb_server_upload.zip
pause
