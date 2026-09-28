#!/usr/bin/env bash
# Native vs emulated renderer: the same scripted run through both, screenshots
# side by side (left emulated, right native) in runs/ab_<name>.png.
#   bash tools/ab_compare.sh <name> [seconds] [ENV=VAL ...]
cd "$(dirname "$0")/.."
NAME="${1:-ab}"; SECS="${2:-58}"; shift 2 2>/dev/null
SCRIPT=('BUFFY_PAD_SCRIPT=4:START,8:START,12:START' 'BUFFY_PAD_SCRIPT2=3:A,6:A,9:A,25:START,30:START,33:A,36:A')
for r in emulated native; do
    rm -rf "runs/ab_${NAME}_$r"; mkdir -p "runs/ab_${NAME}_$r"
    bash tools/run.sh "ab_${NAME}_$r" "$SECS" BUFFY_SKIP_MOVIES=1 BUFFY_RENDERER=$r "${SCRIPT[@]}" \
        "BUFFY_SHOTS=runs\ab_${NAME}_$r" BUFFY_SHOTS_MS=6000 "$@" > /dev/null
done
python - "$NAME" <<'PY'
import sys, glob, os
from PIL import Image, ImageDraw
n = sys.argv[1]
a = sorted(glob.glob(f'runs/ab_{n}_emulated/shot[0-9][0-9][0-9].bmp'))
b = sorted(glob.glob(f'runs/ab_{n}_native/shot[0-9][0-9][0-9].bmp'))
rows = min(len(a), len(b)); W, H = 480, 270
out = Image.new('RGB', (W * 2 + 8, rows * (H + 4)), (40, 40, 40))
for i in range(rows):
    for j, f in enumerate((a[i], b[i])):
        im = Image.open(f).convert('RGB'); im = im.resize((W, H))
        out.paste(im, (j * (W + 8), i * (H + 4)))
out.save(f'runs/ab_{n}.png'); print(f'runs/ab_{n}.png', rows, 'rows')
PY
