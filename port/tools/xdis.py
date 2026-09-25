"""Disassemble guest code from DEFAULT.XBE with names from the recompiled output.

    python tools/xdis.py 0x00044CE0 [count]       # disassemble count instructions (default 80)
    python tools/xdis.py Name_Or_Substring [count]  # look up a recompiled function by name
    python tools/xdis.py --str 0x001A0000          # print a C/UTF-16 string at an address
"""
import glob
import os
import re
import struct
import sys

import capstone
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
XBE = os.path.join(HERE, '..', '..', 'game_files', 'DEFAULT.XBE')
GEN = os.path.join(HERE, '..', 'src', 'recomp', 'gen')


def load_xbe():
    d = open(XBE, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    nsec, sec_addr = struct.unpack_from('<II', d, 0x11C)
    secs = []
    for i in range(nsec):
        o = sec_addr - base + i * 56
        _, va, vsz, raw, rsz = struct.unpack_from('<IIIII', d, o)
        secs.append((va, vsz, raw, rsz))
    return d, secs


def read(d, secs, va, n):
    for sva, vsz, raw, rsz in secs:
        if sva <= va < sva + vsz:
            off = va - sva
            b = d[raw + off: raw + min(off + n, rsz)]
            return b + bytes(max(0, min(n, vsz - off) - len(b)))
    return b''


_names = None


def names():
    global _names
    if _names is None:
        _names = {}
        for f in glob.glob(os.path.join(GEN, 'recomp_*.c')):
            for m in re.finditer(r'^void (\w+?_([0-9A-F]{8}))\(void\)', open(f, encoding='utf-8').read(), re.M):
                _names[int(m.group(2), 16)] = m.group(1)
    return _names


def main():
    d, secs = load_xbe()
    a = sys.argv[1]
    if a == '--str':
        va = int(sys.argv[2], 16)
        b = read(d, secs, va, 256)
        print(repr(b.split(b'\0')[0]), '|', b[:128].decode('utf-16le', 'replace').split('\0')[0])
        return
    if a.startswith('0x'):
        va = int(a, 16)
    else:
        hits = [(k, v) for k, v in names().items() if a in v]
        for k, v in sorted(hits)[:20]:
            print('%08X %s' % (k, v))
        if len(hits) != 1:
            return
        va = hits[0][0]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = read(d, secs, va, count * 15)
    nm = names()
    for i, ins in enumerate(md.disasm(code, va)):
        if i >= count:
            break
        if ins.address in nm and ins.address != va:
            print('%s:' % nm[ins.address])
        note = ''
        m = re.search(r'0x([0-9a-f]+)', ins.op_str)
        if m and int(m.group(1), 16) in nm:
            note = '  ; ' + nm[int(m.group(1), 16)]
        print('  %08X  %-6s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
        if ins.mnemonic == 'ret' and len(sys.argv) <= 2:
            break


if __name__ == '__main__':
    main()
