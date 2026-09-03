@echo off
REM Copyright (C) 2026 Zeeshan Qazi
REM License: GNU Affero General Public License v3.0

echo ==========================================================
echo Running Tests
echo ==========================================================

if not exist build (
    echo Build directory not found. Running build.bat first...
    call "%~dp0build.bat"
)

cd build
ctest -C Release --output-on-failure

echo ==========================================================
echo All tests passed successfully.
echo ==========================================================
