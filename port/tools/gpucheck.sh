#!/usr/bin/env bash
# The GPU layer's regression run: the cemetery (cutscene into play), screenshots
# every 5 s from 40 s, the frame rate, crashes. Writes runs/gpucheck_<name>/
# and runs/gpucheck_<name>.png (a sheet of the shots).
#   bash tools/gpucheck.sh <name> [ENV=VAL ...]
PORT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:-check}"; shift
cd "$PORT"
rm -rf "runs/gpucheck_$NAME"; mkdir -p "runs/gpucheck_$NAME"
bash tools/run.sh "gpucheck_$NAME" 80 BUFFY_SKIP_MOVIES=1 BUFFY_TEST_LEVEL=01000024 BUFFY_FPS_LOG=1 \
    'BUFFY_PAD_SCRIPT=4:START,8:START,12:START,15:A,18:START,21:A,24:START' \
    "BUFFY_SHOTS=runs\\gpucheck_$NAME" BUFFY_SHOTS_MS=5000 BUFFY_SHOTS_FROM=40 "$@" >/dev/null 2>&1
echo "fps: $(grep -a '\[FPS\]' "runs/gpucheck_$NAME.err" | tail -6 | awk '{print $2}' | tr '\n' ' ')"
echo "crashes: $(grep -a -c 'CRASH\|WATCHDOG' "runs/gpucheck_$NAME.err")  shots: $(ls "runs/gpucheck_$NAME" | wc -l)"
grep -a "\[GPU11\]\|\[GPU\]\|\[VK\]\|\[NATIVE\] .*fail" "runs/gpucheck_$NAME.err" | head -8
python - "$NAME" <<'EOF'
import sys, glob
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True
fs = sorted(glob.glob(f'runs/gpucheck_{sys.argv[1]}/shot*.bmp'))[:6]
W, H = 320, 180
s = Image.new('RGB', (W * 3, H * 2))
for i, f in enumerate(fs):
    s.paste(Image.open(f).convert('RGB').resize((W, H)), ((i % 3) * W, (i // 3) * H))
s.save(f'runs/gpucheck_{sys.argv[1]}.png')
EOF
