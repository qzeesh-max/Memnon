@echo off
REM Copyright (C) 2026 Zeeshan Qazi
REM License: GNU Affero General Public License v3.0

echo ==========================================================
echo Building Boost Interprocess Growing Memory Segment
echo ==========================================================

if not exist build mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release

echo ==========================================================
echo Build completed successfully.
echo ==========================================================
