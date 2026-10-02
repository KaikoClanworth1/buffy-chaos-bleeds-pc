#!/usr/bin/env bash
# (Android bring-up) compile the libbuffy.so objects, not the generated code;
# keep going past errors. Errors summarised from port/android/build/p.log.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/port/android/build"
NINJA="$(ls "/c/Program Files (x86)/Microsoft Visual Studio"/*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe | head -1)"
NDK="$ROOT/_local/android/android-ndk-r30"
cmake -S "$ROOT/port/android" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DCMAKE_BUILD_TYPE=Release > "$OUT/cmake.log" 2>&1 || { tail "$OUT/cmake.log"; exit 1; }
objs=$("$NINJA" -C "$OUT/cmake" -t targets all | grep -oE "CMakeFiles/(buffy|xbox_kernel|platform)\.dir/[^:]*\.o" | sort -u)
"$NINJA" -C "$OUT/cmake" -k 0 -j 6 $objs > "$OUT/p.log" 2>&1
echo "errors: $(grep -c "error:" "$OUT/p.log")"
grep "error:" "$OUT/p.log" | sed 's/.*port\/src\///; s/.*android\/src\///; s/.*xboxrecomp\/src\///' | sed 's/:[0-9]*:[0-9]*: error:/:/' | sort | uniq -c | sort -rn | head -${1:-60}
