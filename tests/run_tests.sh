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

FLAGS=(-std=c++20 -g -O1 -Wall -Isrc -pthread)
if [[ -n "${SANITIZERS:-}" ]]; then
    FLAGS+=(-fsanitize="$SANITIZERS" -fno-omit-frame-pointer)
    if "$CXX" --version | grep -qi clang; then
        # Handwritten trampolines and the launcher C ABI have no Clang function
        # type metadata. All other requested sanitizer checks remain enabled.
        FLAGS+=(-fno-sanitize=function)
    fi
fi

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
"$CXX" "${FLAGS[@]}" \
    tests/host_test.cpp "${SOURCES[@]}" \
    -o "$BUILD_DIR/host_test" -ldl

"$CXX" "${FLAGS[@]}" \
    tests/discovery_test.cpp "${SOURCES[@]}" \
    -o "$BUILD_DIR/discovery_test" -ldl

for fixture in pointer_table ambiguous; do
    "$CXX" "${FLAGS[@]}" -D"DISCOVERY_${fixture^^}" \
        tests/discovery_test.cpp "${SOURCES[@]}" \
        -o "$BUILD_DIR/discovery_${fixture}_test" -ldl
done

"$CXX" "${FLAGS[@]}" \
    tests/config_test.cpp "${CONFIG_SOURCES[@]}" \
    -o "$BUILD_DIR/config_test" -rdynamic -ldl -DCLOSEOREUI_TESTING

"$CXX" "${FLAGS[@]}" \
    tests/eh_frame_test.cpp "${SOURCES[@]}" \
    -o "$BUILD_DIR/eh_frame_test" -ldl

"$CXX" "${FLAGS[@]}" -DCLOSEOREUI_TESTING \
    tests/shape_test.cpp "${SOURCES[@]}" \
    -o "$BUILD_DIR/shape_test" -ldl

"$CXX" "${FLAGS[@]}" tests/io_test.cpp src/json.cpp \
    -o "$BUILD_DIR/io_test" -Wl,--wrap=rename -Wl,--wrap=fwrite -Wl,--wrap=fsync

"$CXX" "${FLAGS[@]}" tools/scan_so.cpp "${SOURCES[@]}" \
    -o "$BUILD_DIR/scan_so" -ldl

for test in host discovery discovery_pointer_table discovery_ambiguous config eh_frame shape io; do
    echo
    echo "== ${test}_test =="
    "$BUILD_DIR/${test}_test"
done

echo
echo "all host tests passed"
