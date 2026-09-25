"""Turn the disc's Buffy.map into disassembler seeds and readable C names.

Only symbols the linker flagged as functions (' f ') in code sections are used,
so data labels never become function starts.

    python tools/map_seeds.py <Buffy.map> <default_analysis.json> <out_dir>

Writes <out_dir>/map_seeds.json   [{"start": "0x..."}]         (tools.disasm --seed-functions)
       <out_dir>/map_cnames.json  {"0x0001A2B0": "Class__Method_0001A2B0"}
"""
import json
import re
import sys

SYM_RE = re.compile(r"^\s*([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{8})\s+(f?)\s*(i?)\s+(\S+)", re.I)
CODE_SECTIONS = {".text", "D3D", "D3DX", "XGRPH", "DSOUND", "XMV", "XPP"}

SPECIAL = {"0": "ctor", "1": "dtor", "_G": "sdtor", "_E": "vdtor", "_H": "vec_ctor_iter",
           "_I": "vec_dtor_iter", "_F": "default_ctor_closure", "4": "op_assign"}


def cname(mangled):
    """Cheap, readable (not complete) MSVC demangle to a C identifier."""
    n = mangled
    if n.startswith("??"):
        body = n[2:]
        tag = "_" + body[1] if body.startswith("_") else body[0]
        body = body[len(tag):]
        parts = body.split("@@")[0].split("@")
        cls = "__".join(reversed([p for p in parts if p]))
        n = "%s__%s" % (cls, SPECIAL.get(tag, "op" + tag.strip("_")))
    elif n.startswith("?"):
        parts = n[1:].split("@@")[0].split("@")
        n = "__".join(reversed([p for p in parts if p]))
    else:
        n = n.lstrip("_@")
        n = n.split("@")[0]
    n = re.sub(r"[^A-Za-z0-9_]", "_", n)
    n = re.sub(r"_+", "_", n).strip("_") or "fn"
    return n[:80]


def main():
    map_path, analysis_path, out_dir = sys.argv[1:4]
    secs = json.load(open(analysis_path))["sections"]
    seeds, names = [], {}
    for ln in open(map_path, errors="replace"):
        m = SYM_RE.match(ln)
        if not m or not m.group(5):
            continue
        idx, off = int(m.group(1), 16), int(m.group(2), 16)
        if idx < 1 or idx > len(secs) or secs[idx - 1]["name"] not in CODE_SECTIONS:
            continue
        va = int(secs[idx - 1]["virtual_addr"], 16) + off
        key = "0x%08X" % va
        if key in names:
            continue
        seeds.append({"start": key})
        names[key] = "%s_%08X" % (cname(m.group(3)), va)
    json.dump(seeds, open(out_dir + "/map_seeds.json", "w"), indent=0)
    json.dump(names, open(out_dir + "/map_cnames.json", "w"), indent=1, sort_keys=True)
    print("functions:", len(seeds))


if __name__ == "__main__":
    main()
