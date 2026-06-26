@echo off
REM ============================================================================
REM Ingest unified dataset (Wuxi + CD + TDrive + Geolife) into RocksDB with
REM ST-compaction enabled.
REM
REM Prereqs: cmake --build rocks-demo/build must have produced:
REM   rocks-demo/build/st_bucket_ingest_build.exe
REM
REM Output DB: data/runs/UnifiedST
REM Data:       data/processed/unified/segments_meta.csv + segments_points.csv
REM
REM Usage:
REM   tools\run_unified_st_ingest.bat
REM   tools\run_unified_st_ingest.bat reset    (remove existing DB first)
REM ============================================================================
setlocal enabledelayedexpansion

set "PROJECT=%~dp0.."
cd /d "%PROJECT%"

set "BENCH_EXE=rocks-demo\build\st_bucket_ingest_build.exe"
set "META_CSV=data\processed\unified\segments_meta.csv"
set "POINTS_CSV=data\processed\unified\segments_points.csv"
set "SST_DIR=data\runs\UnifiedST\_sst_stage"
set "TARGET_DB=data\runs\UnifiedST"
set "ENVELOPE_JSON=data\processed\unified\envelope.json"

set "BUCKET_SEC=86400"

if not exist "%BENCH_EXE%" (
    echo ERROR: Missing %BENCH_EXE%
    echo Build first: cmake --build rocks-demo/build
    exit /b 1
)

if not exist "%META_CSV%" (
    echo ERROR: Missing %META_CSV%
    echo Run: python tools/unify_datasets_streaming.py
    exit /b 1
)

if not exist "%POINTS_CSV%" (
    echo ERROR: Missing %POINTS_CSV%
    exit /b 1
)

if "%1"=="reset" (
    echo Removing existing DB: %TARGET_DB%
    if exist "%TARGET_DB%" rd /s /q "%TARGET_DB%"
    if exist "%SST_DIR%" rd /s /q "%SST_DIR%"
)

if exist "%TARGET_DB%\CURRENT" (
    echo DB already exists: %TARGET_DB%\CURRENT
    echo Run with "reset" to re-ingest:
    echo   tools\run_unified_st_ingest.bat reset
    exit /b 1
)

echo ===============================================================================
echo Unified Dataset Ingestion with ST-Compaction
echo ===============================================================================
echo Meta:      %META_CSV%
echo Points:     %POINTS_CSV%
echo Target DB:  %TARGET_DB%
echo Bucket sec: %BUCKET_SEC%
echo ===============================================================================

"%BENCH_EXE%" ^
  --segment-meta-csv "%META_CSV%" ^
  --segment-points-csv "%POINTS_CSV%" ^
  --out-sst-dir "%SST_DIR%" ^
  --target-db "%TARGET_DB%" ^
  --reset-out-sst-dir ^
  --reset-target-db ^
  --bucket-sec %BUCKET_SEC% ^
  --experimental-st-compaction ^
  --stream-ingest

if %ERRORLEVEL% neq 0 (
    echo.
    echo INGEST FAILED with exit code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo.
echo ===============================================================================
echo Ingestion complete. DB: %TARGET_DB%
echo.
echo Quick verify:
echo   rocks-demo\build\st_prune_stats.exe --db "%TARGET_DB%"
echo   rocks-demo\build\st_meta_read_bench.exe --db "%TARGET_DB%" --no-prune-scan
echo ===============================================================================

endlocal
