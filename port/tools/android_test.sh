#!/usr/bin/env bash
# A scripted game run on the phone (USB debugging), as tools/run.sh on the PC:
#   bash port/tools/android_test.sh <name> <seconds> [KEY=VALUE ...]
# Starts the game with the KEY=VALUE pairs as its environment (the app's
# entry passes BUFFY_* / RECOMP_* extras to the game), muted, with the
# game's own frame captures every 2.5 s (BUFFY_SHOTS, full size) into the
# phone's games/buffy_test; after <seconds> stops it and pulls the log to
# port/runs/android/<name>.log and the frames to port/runs/android/<name>/.
# The frames are the game's output, not screenshots of the phone.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ADB="$ROOT/_local/android/sdk/platform-tools/adb.exe"
PKG=io.github.kaikoclanworth1.buffy
NAME="${1:-test}"; SECS="${2:-60}"; shift 2 2>/dev/null
OUT="$ROOT/port/runs/android"
DEV=/sdcard/games/buffy_test
mkdir -p "$OUT"
rm -rf "$OUT/$NAME"
MSYS_NO_PATHCONV=1 "$ADB" shell "rm -rf $DEV; mkdir -p $DEV"
"$ADB" shell am force-stop $PKG
"$ADB" logcat -c
ARGS=(--es BUFFY_MUTE 1 --es BUFFY_SKIP_MOVIES 1 --es BUFFY_SHOTS $DEV --es BUFFY_SHOTS_FULL 1 --es BUFFY_SHOTS_MS 2500 --es BUFFY_FPS_LOG 1)
for kv in "$@"; do ARGS+=(--es "${kv%%=*}" "${kv#*=}"); done
MSYS_NO_PATHCONV=1 "$ADB" shell am start -W -n $PKG/.InstallActivity "${ARGS[@]}" > /dev/null
sleep "$SECS"
"$ADB" logcat -d -s buffy:* DEBUG:* libc:F > "$OUT/$NAME.log"
"$ADB" shell am force-stop $PKG
mkdir -p "$OUT/$NAME"
MSYS_NO_PATHCONV=1 "$ADB" pull $DEV/. "$(cygpath -w "$OUT/$NAME")" > /dev/null
MSYS_NO_PATHCONV=1 "$ADB" shell "rm -rf $DEV"
echo "$NAME: $(ls "$OUT/$NAME" | wc -l) frames; $(grep -c "\[FPS\]" "$OUT/$NAME.log") fps lines"
grep -E "signal [0-9]|Abort|backtrace|FATAL" "$OUT/$NAME.log" | head -5
