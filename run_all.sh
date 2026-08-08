#!/usr/bin/env bash
set -euo pipefail

cmake --build build -j"${JOBS:-3}"
ctest --test-dir build --output-on-failure

if [[ "${1:-}" == "--network" ]]; then
    ./tests/network_integration.sh ./build/media_server ./network_test_output
fi
