#!/usr/bin/env bash
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================================="
echo "Running Tests"
echo "=========================================================="

if [ ! -d "${ROOT_DIR}/build" ]; then
    echo "Build directory not found. Running build.sh first..."
    "${ROOT_DIR}/build.sh"
fi

cd "${ROOT_DIR}/build"
make test

echo "=========================================================="
echo "All tests passed successfully."
echo "=========================================================="
