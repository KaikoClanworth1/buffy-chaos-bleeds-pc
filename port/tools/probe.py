"""Insert call-logging probes into generated functions (debugging aid).

    python tools/probe.py FuncName_ADDR [FuncName_ADDR ...]
    python tools/probe.py --blocks FuncName_ADDR   # also trace every basic block
    python tools/probe.py --clear                   # remove all probes

Each probe prints, when BUFFY_PROBE=<seconds> is set and that many seconds
have passed since process start, the function name, its guest return
address, ecx (this), the first three stack arguments and a microsecond
timestamp (buffy_probe_us, main.c), up to BUFFY_PROBE_MAX (default 200)
lines per function. --blocks adds a [BLK] line (label, eax) per basic block,
50 per block. A regen removes all probes too.
"""
import glob
import os
import re
import sys

GEN = os.path.join(os.path.dirname(__file__), '..', 'src', 'recomp', 'gen')
TAG = '/*PROBE*/'
BS = chr(92)
NL = BS + 'n'   # the two characters backslash-n, for C string literals


def gate(counter, limit):
    return ('extern unsigned long long buffy_probe_us(void); static int ' + counter + '; '
            'if (getenv("BUFFY_PROBE") && ' + counter + ' < ' + limit + ' && buffy_probe_us() >= '
            '(unsigned long long)atoi(getenv("BUFFY_PROBE")) * 1000000ull) { ' + counter + '++; ')


def probe_line(name):
    limit = '(getenv("BUFFY_PROBE_MAX") ? atoi(getenv("BUFFY_PROBE_MAX")) : 200)'
    return ('    ' + TAG + ' { ' + gate('_pc', limit) +
            'fprintf(stderr, "[PROBE] ' + name + ' ret=%08X this=%08X a=%08X %08X %08X us=%llu' + NL + '", '
            'MEM32(esp), ecx, MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 12), buffy_probe_us()); } }\n')


def block_line(label):
    return ('    ' + TAG + ' { ' + gate('_bc', '50') +
            'fprintf(stderr, "[BLK] ' + label + ' eax=%08X ecx=%08X' + NL + '", eax, ecx); } }\n')


def main():
    files = sorted(glob.glob(os.path.join(GEN, 'recomp_0*.c')))
    clear = '--clear' in sys.argv
    blocks = '--blocks' in sys.argv
    names = [a for a in sys.argv[1:] if not a.startswith('--')]
    for f in files:
        s = open(f, encoding='utf-8').read()
        orig = s
        s = ''.join(l for l in s.splitlines(True) if TAG not in l)
        for name in names:
            m = re.search(r'^void ' + re.escape(name) + r'\(void\)\n\{\n', s, re.M)
            if not m:
                continue
            addr = re.search(r'_([0-9A-F]{8})(?:_orig)?$', name).group(1)
            lab = 'loc_' + addr + ': ;\n'
            k = s.find(lab, m.end())
            if k < 0:
                continue
            k += len(lab)
            s = s[:k] + probe_line(name) + s[k:]
            if blocks:
                end = s.find('\n}\n', k)
                body = s[k:end]
                body = re.sub(r'(loc_([0-9A-F]{8}): ;\n)',
                              lambda mm: mm.group(1) + block_line(mm.group(2)), body)
                s = s[:k] + body + s[end:]
            print('probed', name, 'in', os.path.basename(f), '(blocks)' if blocks else '')
        if s != orig:
            if '#include <stdio.h>' not in s:
                s = s.replace('#include <math.h>', '#include <math.h>\n#include <stdio.h>\n#include <stdlib.h>', 1)
            open(f, 'w', encoding='utf-8').write(s)
    if clear:
        print('probes cleared')


if __name__ == '__main__':
    main()
