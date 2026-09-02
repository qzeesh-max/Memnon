#!/usr/bin/env bash
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================================="
echo "Running Sanitizers (ASAN, UBSAN, TSAN)"
echo "=========================================================="

"${ROOT_DIR}/tests/test_sanitizers.sh"

echo "=========================================================="
echo "Sanitizers completed successfully."
echo "=========================================================="
