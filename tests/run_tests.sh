#!/usr/bin/env bash
# Host-side test suite. Validates the pieces that cannot be debugged inside the
# game: instruction decoding, .eh_frame parsing, the inline hooker, and the
# end-to-end discovery pipeline.
#
# Requires a host C++20 compiler. Does not need the NDK.
set -euo pipefail

cd "$(dirname "$0")/.."

CXX="${CXX:-g++}"
BUILD_DIR="${BUILD_DIR:-build/host}"
mkdir -p "$BUILD_DIR"

SOURCES=(
    src/x64_decoder.cpp
    src/eh_frame.cpp
    src/elf_image.cpp
    src/inline_hook.cpp
    src/discover.cpp
    src/log.cpp
)

CONFIG_SOURCES=(
    src/config.cpp
    src/json.cpp
    src/log.cpp
    src/oreui_config.cpp
    src/menu.cpp
)

echo "== building host tests with $CXX =="
"$CXX" -std=c++20 -g -O1 -Wall -Isrc \
    tests/host_test.cpp "${SOURCES[@]}" \
    -o "$BUILD_DIR/host_test" -ldl

"$CXX" -std=c++20 -g -O1 -Wall -Isrc \
    tests/discovery_test.cpp "${SOURCES[@]}" \
    -o "$BUILD_DIR/discovery_test" -ldl

echo
echo "== host_test =="
"$BUILD_DIR/host_test"

"$CXX" -std=c++20 -g -O1 -Wall -Isrc \
    tests/config_test.cpp "${CONFIG_SOURCES[@]}" \
    -o "$BUILD_DIR/config_test"

echo
echo "== discovery_test =="
"$BUILD_DIR/discovery_test"

echo
echo "== config_test =="
"$BUILD_DIR/config_test"

echo
echo "all host tests passed"
