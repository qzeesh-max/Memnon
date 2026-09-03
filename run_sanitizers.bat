@echo off
REM Copyright (C) 2026 Zeeshan Qazi
REM License: GNU Affero General Public License v3.0

echo ==========================================================
echo Running ASAN Sanitizer (MSVC)
echo ==========================================================

if not exist build_asan mkdir build_asan
cd build_asan

cmake -DCMAKE_BUILD_TYPE=Debug -DSI_SANITIZE=address ..
cmake --build . --config Debug

echo --^> Running tests
ctest -C Debug --output-on-failure

echo ==========================================================
echo Sanitizers completed successfully.
echo ==========================================================
