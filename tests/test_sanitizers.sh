#!/usr/bin/env bash
set -e

# Test runner script for sanitizers
# Requires a clean build for each sanitizer type as it modifies compiler flags

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_sanitizer() {
    local san_name=$1
    local san_flags=$2

    echo "=========================================================="
    echo "Running with ${san_name} Sanitizer"
    echo "=========================================================="
    
    local build_dir="${ROOT_DIR}/build_${san_name}"
    mkdir -p "${build_dir}"
    cd "${build_dir}"

    cmake -DCMAKE_CXX_FLAGS="${san_flags}" -DCMAKE_BUILD_TYPE=Debug ..
    make -j$(sysctl -n hw.ncpu || nproc)

    echo "--> Running tests"
    ./tests/test_ncrit_trie
    ./tests/test_segmented_offset_ptr
    ./tests/test_segmented_managed_memory
    ./tests/test_shm_multithreaded
    ./tests/test_multithreaded
}

# Run ASAN/UBSAN combined
run_sanitizer "ASAN_UBSAN" "-fsanitize=address,undefined -fno-omit-frame-pointer"

# Run TSAN (mutually exclusive with ASAN)
run_sanitizer "TSAN" "-fsanitize=thread -fno-omit-frame-pointer"

echo "=========================================================="
echo "All sanitizer runs passed successfully!"
echo "=========================================================="
