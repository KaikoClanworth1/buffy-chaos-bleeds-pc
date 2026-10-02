"""The port's own source for another release of the game.

The port's C names the game's functions and globals by their addresses in
the European release (XApp_StartGameMode_0002E2C0, MEM32(0x26DC54), the
keys of manual_functions.json). For another release's build, each becomes
the same function's or global's address there (release_map.py's addrmap):

  - an identifier ending _<8 hex digits>(_orig) that is a mapped address
  - a hex literal in the game's image (0x11000..0x400000) that is a mapped
    address -- other numbers (masks, sizes, constants) are left alone

    python tools/translate_release.py <addrmap.json> <src_dir> <out_dir> <report.txt>

Copies <src_dir>'s .c, .h and .json files to <out_dir> (only the ones that
change are rewritten, so a rebuild touches no more than it must). An
identifier with an address the map does not have stops it: the build would
otherwise call the European release's address in the other one.
"""
import json
import os
import re
import sys

IDENT = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*?)_([0-9A-F]{8})(_orig)?\b')
HEX = re.compile(r'\b0[xX]([0-9A-Fa-f]{5,8})([uU]?)\b')


def main():
    amap = {int(a, 16): int(b, 16) for a, b in json.load(open(sys.argv[1])).items()}
    src, out, report = sys.argv[2], sys.argv[3], sys.argv[4]
    os.makedirs(out, exist_ok=True)
    lines, missing, unsure = [], [], []
    for name in sorted(os.listdir(src)):
        if not name.endswith(('.c', '.h', '.json')):
            continue
        text = open(os.path.join(src, name), encoding='utf-8', errors='surrogateescape').read()

        comments = [(c.start(), c.end()) for c in re.finditer(r'/\*.*?\*/|//[^\n]*', text, re.S)]

        def in_comment(pos):
            return any(a <= pos < b for a, b in comments)

        def ident(m):
            a = int(m.group(2), 16)
            if a < 0x11000 or a >= 0x400000:
                return m.group(0)
            if a not in amap:
                if not in_comment(m.start()):
                    missing.append('%s: %s' % (name, m.group(0)))
                return m.group(0)
            b = amap[a]
            if a != b:
                lines.append('%s: %s -> %08X' % (name, m.group(0), b))
            return '%s_%08X%s' % (m.group(1), b, m.group(3) or '')

        def hexlit(m):
            digits, suf = m.group(1), m.group(2)
            a = int(digits, 16)
            if a < 0x11000 or a >= 0x400000:
                return m.group(0)
            if a not in amap:
                if not in_comment(m.start()):
                    unsure.append('%s: 0x%s' % (name, digits))
                return m.group(0)
            b = amap[a]
            if a == b:
                return m.group(0)
            nd = ('%0' + str(len(digits)) + 'X') % b
            if digits.islower():
                nd = nd.lower()
            lines.append('%s: 0x%s -> 0x%s' % (name, digits, nd))
            return m.group(0)[:2] + nd + suf

        new = IDENT.sub(ident, text)
        new = HEX.sub(hexlit, new)
        new = new.replace('"recomp/gen/', '"recomp/gen_usa/')      # this release's generated code
        dst = os.path.join(out, name)
        old = open(dst, encoding='utf-8', errors='surrogateescape').read() if os.path.exists(dst) else None
        if old != new:
            open(dst, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(new)
    open(report, 'w').write('\n'.join(lines) + '\n' + '\n'.join('UNMAPPED ' + x for x in missing) + '\n'
                            + '\n'.join('UNSURE ' + x for x in unsure) + '\n')
    print('%d substitutions, %d unmapped, %d literals in the image not mapped (UNSURE in the report)'
          % (len(lines), len(missing), len(unsure)))
    if missing:
        print('\n'.join(missing[:20]))
        sys.exit(1)


if __name__ == '__main__':
    main()
