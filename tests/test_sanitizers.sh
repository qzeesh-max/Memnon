#!/usr/bin/env bash
# Copyright (C) 2026 Zeeshan Qazi
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

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
