#!/usr/bin/env bash
# Build the Android APK: the launcher (java/, javac + d8), its resources
# (res/, aapt2) and the game (libbuffy.so, the NDK), aligned and signed.
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
LIB="$OUT/cmake/libbuffy.so"
rm -f "$LIB"                       # (a failed build must not package the last one)
cmake --build "$OUT/cmake" -j 4 2>&1 | grep -E "error:|warning: implicit|Error" | head -40 || true
[ -f "$LIB" ] || { echo "no libbuffy.so"; exit 1; }

# 2. the launcher: Java 8 bytecode (lambdas desugared by d8) into classes.dex
rm -rf "$OUT/classes" "$OUT/dex"
mkdir -p "$OUT/classes" "$OUT/dex"
find "$ROOT/port/android/java" -name "*.java" | while read -r f; do echo "\"$(cygpath -m "$f")\""; done > "$OUT/sources.txt"
(cd "$OUT" && "$JDK/bin/javac.exe" -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 \
    -classpath "$(cygpath -w "$JAR")" -d classes @sources.txt)
CLASSES=$(cd "$OUT" && find classes -name "*.class")
(cd "$OUT" && "$JDK/bin/java.exe" -cp "$(cygpath -w "$BT/lib/d8.jar")" com.android.tools.r8.D8 --release \
    --min-api 33 --lib "$(cygpath -w "$JAR")" --output dex $CLASSES > d8.log 2>&1) || { cat "$OUT/d8.log"; exit 1; }
[ -f "$OUT/dex/classes.dex" ] || { echo "no classes.dex"; cat "$OUT/d8.log"; exit 1; }

# 3. the APK: manifest and resources (aapt2), the code, the library stored
#    uncompressed and 16 KB aligned (extractNativeLibs=false: loaded straight
#    from the APK)
#    The app icon: the game's own logo, from the game files here (game_files/,
#    or BUFFY_ICON_XBX=<buffytitle.xbx>) -- made at build time into a copy of
#    res/, never committed; without them, the stand-in icon (make_icon.py).
rm -rf "$OUT/res"
cp -r "$ROOT/port/android/res" "$OUT/res"
ICON_XBX="${BUFFY_ICON_XBX:-$ROOT/game_files/Buffy/Binary/_bin_xb/buffytitle.xbx}"
if [ -f "$ICON_XBX" ]; then
    python "$ROOT/port/android/tools/game_icon.py" "$ICON_XBX" "$OUT/res"
else
    echo "app icon: the stand-in (no $ICON_XBX)"
fi
"$BT/aapt2.exe" compile --dir "$OUT/res" -o "$OUT/res.zip"
"$BT/aapt2.exe" link -o "$OUT/base.apk" --manifest "$ROOT/port/android/AndroidManifest.xml" -I "$JAR" "$OUT/res.zip"
# (libadrenotools' hook libraries for custom GPU drivers: beside libbuffy.so)
HOOKS="$OUT/cmake/adrenotools/src/hook"
python - "$OUT/base.apk" "$LIB" "$OUT/dex/classes.dex" "$OUT/unaligned.apk" "$HOOKS" <<'EOF'
import sys, zipfile, shutil, os
base, lib, dex, out, hooks = sys.argv[1:6]
shutil.copy(base, out)
with zipfile.ZipFile(out, 'a') as z:
    z.write(dex, 'classes.dex', compress_type=zipfile.ZIP_DEFLATED)
    z.write(lib, 'lib/arm64-v8a/libbuffy.so', compress_type=zipfile.ZIP_STORED)
    for h in ('libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so'):
        p = os.path.join(hooks, h)
        if not os.path.isfile(p):
            sys.exit('missing ' + p)
        z.write(p, 'lib/arm64-v8a/' + h, compress_type=zipfile.ZIP_STORED)
EOF
"$BT/zipalign.exe" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"

# 4. signed with the local debug key
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
    "$ADB" shell am start -n io.github.kaikoclanworth1.buffy/.InstallActivity
fi
