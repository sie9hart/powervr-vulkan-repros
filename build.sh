#!/bin/sh
# Cross-compile the repros for arm64 Android with the NDK.
# Usage: ANDROID_NDK=/path/to/ndk ./build.sh      -> build/repro_*
set -eu
cd "$(dirname "$0")"
: "${ANDROID_NDK:?set ANDROID_NDK to the NDK root (r25 or newer)}"
CMAKE=${CMAKE:-cmake}
"$CMAKE" -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
"$CMAKE" --build build
ls -l build/repro_*
