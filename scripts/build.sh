#!/usr/bin/env bash
# Builds the mod as an Android x86_64 shared library for mcpelauncher.
#
# Output: build/<abi>/libcloseoreui.so
#
# Environment:
#   ANDROID_NDK_HOME   path to an NDK (r26+). Falls back to common SDK layouts.
#   ANDROID_ABI        target ABI, default x86_64 (the only supported one)
#   ANDROID_PLATFORM   default android-24
#   BUILD_TYPE         default Release
#
# Release labelling:
#   MOD_VERSION        mod version, defaults to `git describe`.
#
# There is intentionally no game-version parameter: this mod is not built for a
# specific Minecraft release. See README "跨版本适配能力".
set -euo pipefail

cd "$(dirname "$0")/.."

find_ndk() {
    if [[ -n "${ANDROID_NDK_HOME:-}" ]]; then
        echo "$ANDROID_NDK_HOME"
        return
    fi
    for candidate in \
        "${ANDROID_SDK_ROOT:-}/ndk/"* \
        "${ANDROID_HOME:-}/ndk/"* \
        "$HOME/Android/Sdk/ndk/"* \
        "$HOME/android-ndk"* \
        /opt/android-ndk*; do
        if [[ -f "$candidate/build/cmake/android.toolchain.cmake" ]]; then
            echo "$candidate"
        fi
    done | sort -V | tail -1
}

find_cmake() {
    if command -v cmake >/dev/null 2>&1; then
        command -v cmake
        return
    fi
    for candidate in \
        "${ANDROID_SDK_ROOT:-}/cmake/"*/bin/cmake \
        "${ANDROID_HOME:-}/cmake/"*/bin/cmake \
        "$HOME/Android/Sdk/cmake/"*/bin/cmake; do
        if [[ -x "$candidate" ]]; then
            echo "$candidate"
        fi
    done | sort -V | tail -1
}

NDK="$(find_ndk)"
if [[ -z "$NDK" ]]; then
    echo "error: no Android NDK found; set ANDROID_NDK_HOME" >&2
    exit 1
fi

CMAKE="$(find_cmake)"
if [[ -z "$CMAKE" ]]; then
    echo "error: no cmake found; install cmake or set PATH" >&2
    exit 1
fi

ABI="${ANDROID_ABI:-x86_64}"
PLATFORM="${ANDROID_PLATFORM:-android-24}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
OUT="${OUT_DIR:-build/$ABI}"

MOD_VERSION="${MOD_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"

echo "NDK:          $NDK"
echo "CMake:        $CMAKE"
echo "ABI:          $ABI"
echo "Platform:     $PLATFORM"
echo "Mod version:  $MOD_VERSION"
echo "Output:       $OUT"

"$CMAKE" -B "$OUT" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$(dirname "$CMAKE")/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="$PLATFORM" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCLOSEOREUI_MOD_VERSION="$MOD_VERSION" \
    -DCLOSEOREUI_TARGET_ABI="$ABI"

"$CMAKE" --build "$OUT"

echo
echo "built: $OUT/libcloseoreui.so"
