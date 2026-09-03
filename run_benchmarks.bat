@echo off
REM Copyright (C) 2026 Zeeshan Qazi
REM License: GNU Affero General Public License v3.0

echo ==========================================================
echo Running Benchmarks
echo ==========================================================

if not exist build (
    echo Build directory not found. Running build.bat first...
    call "%~dp0build.bat"
)

cd build
cmake --build . --config Release --target benchmarks
if exist benchmarks\Release\benchmarks.exe (
    benchmarks\Release\benchmarks.exe
) else (
    benchmarks\benchmarks.exe
)

echo ==========================================================
echo Benchmarks completed.
echo ==========================================================
