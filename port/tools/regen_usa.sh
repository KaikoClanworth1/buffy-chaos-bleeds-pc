#!/usr/bin/env bash
# The North American release (version 1): its default.xbe -> C, for the
# second executable (buffy_chaos_bleeds_usa.exe). Run after regen.sh (the
# European release's), with the North American disc unpacked in
# game_files_usa/ (python -m tools.xiso unpack "<usa>.iso" -o ../game_files_usa).
#
#   1. xbe_match.py pairs its functions and addresses with the European
#      release's (whose names come from the disc's Buffy.map)
#   2. release_map.py names its functions as the European release's and
#      writes the address map
#   3. translate_release.py turns the port's source (European addresses) into
#      this release's, in game_files_usa/work/src_usa
#   4. the same pipeline as regen.sh, into port/src/recomp/gen_usa
#
# The tools write to their own output folders, which regen.sh's build and
# tools/sym.py use: they are put back afterwards.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PORT="$ROOT/port"
TK="$ROOT/xboxrecomp"
GU="$ROOT/game_files_usa"
W="$GU/work"
[ -f "$GU/DEFAULT.XBE" ] || [ -f "$GU/default.xbe" ] || { echo "No game_files_usa/default.xbe"; exit 1; }
[ -f "$TK/game_files/map_cnames.json" ] || { echo "Run regen.sh (the European release) first"; exit 1; }
mkdir -p "$W"
XBE="$W/default.xbe"
[ -f "$XBE" ] || cp "$GU"/DEFAULT.XBE "$XBE" 2>/dev/null || cp "$GU"/default.xbe "$XBE"
cd "$TK"

# Keep the European outputs.
SAVE="$W/pal_outputs"
rm -rf "$SAVE"; mkdir -p "$SAVE"
for d in disasm func_id abi_analysis recomp; do
    [ -d "tools/$d/output" ] && cp -r "tools/$d/output" "$SAVE/$d"
done
restore() {
    for d in disasm func_id abi_analysis recomp; do
        if [ -d "$SAVE/$d" ]; then rm -rf "tools/$d/output"; cp -r "$SAVE/$d" "tools/$d/output"; fi
    done
}
trap restore EXIT

# 1-2. Pair with the European release; its names, this release's addresses.
python -m tools.xbe_parser "$XBE" --json "$W/default_analysis.json" > /dev/null
python -m tools.disasm "$XBE" --force > /dev/null
rm -rf "$W/usa_out0"; cp -r tools/disasm/output "$W/usa_out0"
python "$PORT/tools/xbe_match.py" "$ROOT/game_files/DEFAULT.XBE" "$SAVE/disasm/functions.json" \
    "$XBE" "$W/usa_out0/functions.json" "$W/match.json"
[ -f "$W/fixups.json" ] || printf '{\n "0x00099C40": "0x000971B0",\n "0x00110AB0": "0x0010FDD0"\n}\n' > "$W/fixups.json"
python "$PORT/tools/release_map.py" "$W/match.json" "$TK/game_files/map_cnames.json" "$W" "$W/fixups.json"

# 3. The port's source in this release's addresses.
python "$PORT/tools/translate_release.py" "$W/addrmap.json" "$PORT/src" "$W/src_usa" "$W/translate_report.txt"

# 4. The pipeline, seeded with the paired functions.
python -m tools.disasm "$XBE" --force --seed-functions "$W/map_seeds.json" > /dev/null
python - <<'EOF'
import json
p = 'tools/disasm/output/functions.json'
f = [x for x in json.load(open(p)) if x['section'] != 'DOLBY']
json.dump(f, open(p, 'w'), indent=1)
EOF
python tools/ghidra_naming/merge_names.py --names-json "$W/map_cnames.json" --out "$W/applied_names.json" --apply > /dev/null
python "$PORT/tools/fix_bounds.py" tools/disasm/output/functions.json "$W/map_cnames.json"
python -m tools.func_id "$XBE" > /dev/null
python -m tools.abi_analysis "$XBE" > /dev/null
python -m tools.recomp "$XBE" --all --split 1000 \
    --game-name "Buffy the Vampire Slayer: Chaos Bleeds" \
    --gen-dir "$PORT/src/recomp/gen_usa" --manual-functions "$W/src_usa/manual_functions.json" 2>&1 \
    | grep -E "functions \(|unresolved|unimplemented"
cp tools/disasm/output/functions.json "$W/usa_functions.json"     # (tools/sym.py --usa)
