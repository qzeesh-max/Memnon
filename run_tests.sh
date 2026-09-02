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
