#!/usr/bin/env bash
# Build the Android APK: libbuffy.so with the NDK, packaged, aligned and signed.
#   bash port/android/build_apk.sh [install]
# Tools (not in git): _local/android/ -- android-ndk-r30, sdk/{platform-tools,
# build-tools/android-37.0,platforms/android-36}, jdk-21*. The debug key is
# made there on the first run. With "install", the APK goes onto the phone
# over adb and starts.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLS="$ROOT/_local/android"
NDK="$TOOLS/android-ndk-r30"
SDK="$TOOLS/sdk"
BT="$SDK/build-tools/android-37.0"
JAR="$SDK/platforms/android-36/android.jar"
JDK="$(ls -d "$TOOLS"/jdk-21* | head -1)"
OUT="$ROOT/port/android/build"
APK="$ROOT/dist/Buffy-Chaos-Bleeds-Android.apk"
mkdir -p "$OUT" "$ROOT/dist"

# 1. the native library
# Ninja (the NDK's make cannot take the spaces in this path): Visual Studio's
NINJA="$(ls "/c/Program Files (x86)/Microsoft Visual Studio"/*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe 2>/dev/null | head -1)"
cmake -S "$ROOT/port/android" -B "$OUT/cmake" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DCMAKE_BUILD_TYPE=Release > "$OUT/cmake.log"
cmake --build "$OUT/cmake" -j 4 2>&1 | grep -E "error|warning: implicit|Error" | head -40 || true
LIB="$OUT/cmake/libbuffy.so"
[ -f "$LIB" ] || { echo "no libbuffy.so"; exit 1; }

# 2. the APK: manifest (aapt2), the library stored uncompressed and 16 KB
#    aligned (extractNativeLibs=false: it is loaded straight from the APK)
"$BT/aapt2.exe" link -o "$OUT/base.apk" --manifest "$ROOT/port/android/AndroidManifest.xml" -I "$JAR"
python - "$OUT/base.apk" "$LIB" "$OUT/unaligned.apk" <<'EOF'
import sys, zipfile, shutil
base, lib, out = sys.argv[1:4]
shutil.copy(base, out)
with zipfile.ZipFile(out, 'a') as z:
    z.write(lib, 'lib/arm64-v8a/libbuffy.so', compress_type=zipfile.ZIP_STORED)
EOF
"$BT/zipalign.exe" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"

# 3. signed with the local debug key
KS="$TOOLS/debug.keystore"
if [ ! -f "$KS" ]; then
    "$JDK/bin/keytool.exe" -genkeypair -keystore "$KS" -storepass android -keypass android -alias debug \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Buffy Android Debug" > /dev/null
fi
"$JDK/bin/java.exe" -jar "$BT/lib/apksigner.jar" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out "$APK" "$OUT/aligned.apk"
echo "built $APK ($(du -h "$APK" | cut -f1))"

if [ "${1:-}" = "install" ]; then
    ADB="$SDK/platform-tools/adb.exe"
    "$ADB" install -r "$APK"
    "$ADB" logcat -c
    "$ADB" shell am start -n io.github.kaikoclanworth1.buffy/android.app.NativeActivity
fi
