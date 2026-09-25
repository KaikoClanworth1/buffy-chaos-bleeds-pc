#!/usr/bin/env bash
# A release folder (and zip) with no game data: the player installs the game
# from their own Xbox disc image with the launcher.
#   bash port/tools/package.sh   ->  dist/Buffy Chaos Bleeds PC/ and dist/Buffy Chaos Bleeds PC.zip
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/port/build/Release"
OUT="$ROOT/dist/Buffy Chaos Bleeds PC"
rm -rf "$OUT"
mkdir -p "$OUT/ShaderCache"
cp -f "$BIN/Buffy Launcher.exe" "$BIN/buffy_chaos_bleeds.exe" "$OUT/"
cp -f "$BIN/buffy_chaos_bleeds.pdb" "$OUT/" 2>/dev/null || true
cp -f "$ROOT/port/tools/ReadMe.txt" "$OUT/Read Me.txt"
# Visual C++ runtime, app-local, for PCs without the redistributable.
CRT="$(ls -d "/c/Program Files (x86)/Microsoft Visual Studio/2019/BuildTools/VC/Redist/MSVC/"*/x64/Microsoft.VC142.CRT 2>/dev/null | tail -1)"
[ -n "$CRT" ] && cp -f "$CRT/vcruntime140.dll" "$CRT/vcruntime140_1.dll" "$OUT/" || echo "warning: VC++ runtime DLLs not found"
cp -f "$BIN/ShaderCache/"*.cso "$OUT/ShaderCache/" 2>/dev/null || true
cp -f "$ROOT/GAME/ShaderCache/"*.cso "$OUT/ShaderCache/" 2>/dev/null || true
mkdir -p "$OUT/mods" && cp -r "$ROOT/port/mods/"* "$OUT/mods/"
( cd "$ROOT/dist" && rm -f "Buffy Chaos Bleeds PC.zip" && powershell -NoProfile -Command \
    "Compress-Archive -Path 'Buffy Chaos Bleeds PC' -DestinationPath 'Buffy Chaos Bleeds PC.zip'" )
echo "packaged: $OUT ($(du -sh "$OUT" | cut -f1)), zip $(du -h "$ROOT/dist/Buffy Chaos Bleeds PC.zip" | cut -f1)"
