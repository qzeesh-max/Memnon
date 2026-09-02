#!/usr/bin/env bash
set -e

echo "=========================================================="
echo "Building Boost Interprocess Growing Memory Segment"
echo "=========================================================="

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(sysctl -n hw.ncpu || nproc)

echo "=========================================================="
echo "Build completed successfully."
echo "=========================================================="
