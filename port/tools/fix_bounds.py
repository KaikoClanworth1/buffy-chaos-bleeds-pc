"""Extend map-named functions to the next map symbol.

tools.disasm ends a function at the last byte it could reach by direct flow, so
code reached only through the function's own jump table (MSVC's memmove/memcpy
tails, some switch arms) falls outside it and the lifter turns those jumps into
failing indirect calls. The linker map is authoritative about where functions
start, so a map function owns everything up to the next function start -- unless
the disassembler found another entry inside that range (SEH funclets, CRT
alternate entry points), in which case it is left alone.

    python tools/fix_bounds.py <functions.json> <map_cnames.json>
"""
import json
import sys


def main():
    fpath, npath = sys.argv[1:3]
    funcs = json.load(open(fpath))
    names = json.load(open(npath))
    funcs.sort(key=lambda x: int(x["start"], 16))
    changed = 0
    for i in range(len(funcs) - 1):
        cur, nxt = funcs[i], funcs[i + 1]
        if "0x%08X" % int(cur["start"], 16) not in names:
            continue
        if cur["section"] != nxt["section"]:
            continue
        end, nstart = int(cur["end"], 16), int(nxt["start"], 16)
        if nstart > end:
            cur["end"] = "0x%08X" % nstart
            cur["size"] = nstart - int(cur["start"], 16)
            changed += 1
    json.dump(funcs, open(fpath, "w"), indent=1)
    print("extended %d functions" % changed)


if __name__ == "__main__":
    main()
