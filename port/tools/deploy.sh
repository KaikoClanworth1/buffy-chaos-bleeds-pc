#!/usr/bin/env bash
# Refresh the playable folder:  bash port/tools/deploy.sh
#   GAME/buffy_chaos_bleeds.exe (+ .pdb for readable crash reports)
#   GAME/default.xbe, GAME/Buffy/...   (disc data; hard-linked, same drive)
#   GAME/Read Me.txt
# Run after every rebuild. Saves (GAME/SaveData) are never touched.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/game_files"
DST="$ROOT/GAME"
BIN="$ROOT/port/build/Release"

mkdir -p "$DST"
cp -f "$BIN/buffy_chaos_bleeds.exe" "$DST/"
cp -f "$BIN/buffy_chaos_bleeds.pdb" "$DST/" 2>/dev/null || true
cp -f "$BIN/Buffy Launcher.exe" "$DST/"
cp -f "$ROOT/port/tools/ReadMe.txt" "$DST/Read Me.txt"

# Disc data: link once (only missing files), so reruns are instant.
cd "$SRC"
# Never the save/state folders: a hard link there would let dev test runs
# (which use game_files) overwrite the player's saves.
find . -type f ! -name 'Buffy.map' ! -name 'xbox_kernel.log' \
     ! -path './SaveData/*' ! -path './UDATA/*' ! -path './TDATA/*' | while read -r f; do
    t="$DST/${f#./}"
    if [ ! -e "$t" ]; then
        mkdir -p "$(dirname "$t")"
        ln "$f" "$t" 2>/dev/null || cp "$f" "$t"
    fi
done
# Movies: XMV (WMV2 + Xbox ADPCM) converted once to Movies/<name>.mp4
# (H.264 + AAC), which the game plays through Media Foundation
# (src/buffy_movie.c). Needs ffmpeg (FFMPEG, PATH, or C:/ffmpeg/bin).
FF="${FFMPEG:-$(command -v ffmpeg || echo C:/ffmpeg/bin/ffmpeg)}"
mkdir -p "$DST/Movies"
for f in "$SRC"/Buffy/Binary/_bin_xb/_Movies/*.xmv; do
    [ -e "$f" ] || continue
    b="$(basename "$f" .xmv | tr 'A-Z' 'a-z')"
    o="$DST/Movies/$b.mp4"
    if [ ! -s "$o" ]; then
        echo "converting movie $b"
        "$FF" -hide_banner -loglevel quiet -y -i "$f" -c:v libx264 -preset medium -crf 18             -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart "$o" || echo "  (failed: $b)"
    fi
done
# Mods that come with the port (port/mods): added once, never overwritten.
for m in "$ROOT/port/mods"/*/; do
    [ -d "$m" ] || continue
    n="$(basename "$m")"
    [ -e "$DST/mods/$n" ] || { mkdir -p "$DST/mods"; cp -r "$m" "$DST/mods/$n"; }
done
# Shaders compiled during test runs: ship them so first sightings don't stall.
if [ -d "$BIN/ShaderCache" ]; then
    mkdir -p "$DST/ShaderCache"
    cp -n "$BIN/ShaderCache/"*.cso "$DST/ShaderCache/" 2>/dev/null || true
fi
echo "deployed to $DST"
