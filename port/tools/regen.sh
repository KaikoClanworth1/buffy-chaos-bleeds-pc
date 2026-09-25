#!/usr/bin/env bash
# Full XBE -> C regeneration for Buffy: Chaos Bleeds.
# Run from anywhere: bash port/tools/regen.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PORT="$ROOT/port"
TK="$ROOT/xboxrecomp"
GF="$TK/game_files"
mkdir -p "$GF"
# The XBE from your own disc (the launcher's Install puts the disc in game_files/).
if [ ! -f "$GF/default.xbe" ]; then
    for x in "$ROOT/game_files/DEFAULT.XBE" "$ROOT/game_files/default.xbe"; do
        [ -f "$x" ] && cp "$x" "$GF/default.xbe" && break
    done
fi
[ -f "$GF/default.xbe" ] || { echo "No default.xbe: install the game from your disc image into game_files/ first"; exit 1; }
[ -f "$ROOT/game_files/Buffy.map" ] || { echo "No game_files/Buffy.map (it is on the game disc)"; exit 1; }
cd "$TK"

python -m tools.xbe_parser "$GF/default.xbe" --json "$GF/default_analysis.json" > /dev/null
python "$PORT/tools/map_seeds.py" "$ROOT/game_files/Buffy.map" "$GF/default_analysis.json" "$GF"
python -m tools.disasm "$GF/default.xbe" --force --seed-functions "$GF/map_seeds.json" > /dev/null

# DOLBY is DSP microcode, not x86.
python - <<'EOF'
import json
p = 'tools/disasm/output/functions.json'
f = [x for x in json.load(open(p)) if x['section'] != 'DOLBY']
json.dump(f, open(p, 'w'), indent=1)
EOF

python tools/ghidra_naming/merge_names.py --names-json "$GF/map_cnames.json" --out "$GF/applied_names.json" --apply > /dev/null
python "$PORT/tools/fix_bounds.py" tools/disasm/output/functions.json "$GF/map_cnames.json"
python -m tools.func_id "$GF/default.xbe" > /dev/null
python -m tools.abi_analysis "$GF/default.xbe" > /dev/null

EXTRA=()
[ -f "$PORT/src/manual_functions.json" ] && EXTRA+=(--manual-functions "$PORT/src/manual_functions.json")
python -m tools.recomp "$GF/default.xbe" --all --split 1000 \
    --game-name "Buffy the Vampire Slayer: Chaos Bleeds" \
    --gen-dir "$PORT/src/recomp/gen" "${EXTRA[@]}" 2>&1 | grep -E "functions \(|unresolved|unimplemented"
