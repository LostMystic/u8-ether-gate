#!/bin/sh
# Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
# Builds the native engine and the tests into build/ (binary: build/bin/pagan).
#   ./build.sh [Debug|Release|RelWithDebInfo]
set -e
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE="${1:-RelWithDebInfo}" >/dev/null
cmake --build build -j"$(nproc)"
echo "== built build/bin/pagan; run ./run.sh, test with: ctest --test-dir build"
