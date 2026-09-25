"""Symbolize guest addresses with the map-derived function names.

    python tools/sym.py 0x000BCC40 0x000BCCDB ...
    python tools/sym.py runs/run1.err          # every 0x00XXXXXX in a log's CRASH block
"""
import bisect
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FUNCS = os.path.join(HERE, "..", "..", "xboxrecomp", "tools", "disasm", "output", "functions.json")

_f = sorted((int(x["start"], 16), int(x["end"], 16), x["name"]) for x in json.load(open(FUNCS)))
_starts = [a for a, _, _ in _f]


def sym(va):
    i = bisect.bisect_right(_starts, va) - 1
    if i < 0:
        return "?"
    a, e, n = _f[i]
    tag = "" if va < e else " (past end)"
    return "%s+0x%X%s" % (n, va - a, tag)


def main():
    args = sys.argv[1:]
    if len(args) == 1 and os.path.isfile(args[0]):
        txt = open(args[0], errors="replace").read()
        k = txt.rfind("[CRASH]")
        txt = txt[k:] if k >= 0 else txt
        args = re.findall(r"0x00[0-9A-Fa-f]{6}\b", txt)
    for a in args:
        va = int(a, 16)
        if 0x11000 <= va < 0x195FC0:
            print("0x%08X  %s" % (va, sym(va)))


if __name__ == "__main__":
    main()
