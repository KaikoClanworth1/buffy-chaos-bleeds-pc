#!/usr/bin/env bash
# Run the dev build against the extracted disc: bash tools/run.sh <name> [seconds] [ENV=VAL ...]
# Writes runs/<name>.out and runs/<name>.err, then prints ICALL problems and a symbolized crash.
PORT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:-run}"; SECS="${2:-60}"; shift 2 2>/dev/null
cd "$PORT"
mkdir -p runs
export BUFFY_GAME_DIR="$(cd "$PORT/../game_files" && pwd -W | tr '/' '\\')"
export RECOMP_WATCHDOG_SECS="${RECOMP_WATCHDOG_SECS:-600}"
export BUFFY_NO_WINDOW=1
export BUFFY_MUTE="${BUFFY_MUTE:-1}"   # test runs are silent
export BUFFY_MOVIE_DIR="${BUFFY_MOVIE_DIR:-$(cd "$PORT/../GAME/Movies" 2>/dev/null && pwd -W | tr '/' '\\')}"
for kv in "$@"; do export "$kv"; done
timeout "$SECS" build/Release/buffy_chaos_bleeds.exe > "runs/$NAME.out" 2> "runs/$NAME.err"
echo "exit=$?"
grep -E "^\[ICALL\]|\[CRASH\]|WATCHDOG|Cannot open" "runs/$NAME.err" | head -25
python tools/sym.py "runs/$NAME.err" | head -16
