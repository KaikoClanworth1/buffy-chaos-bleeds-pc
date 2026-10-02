"""From xbe_match.py's pairing, what another release's build needs.

    python tools/release_map.py <match.json> <pal map_cnames.json> <out_dir> [fixups.json]

Writes <out_dir>/map_seeds.json   the release's function starts (disassembler seeds)
       <out_dir>/map_cnames.json  its functions named as the European release's
                                  (same name, its own address: Name_<address>)
       <out_dir>/addrmap.json     {"0x<european>": "0x<this release>"} for every
                                  paired function and address (translate_release.py)
fixups.json: {"0x<european>": "0x<this release>"} pairs found by hand, which win.
"""
import json
import re
import sys

SUFFIX = re.compile(r'_([0-9A-F]{8})$')


def main():
    match = json.load(open(sys.argv[1]))
    names = json.load(open(sys.argv[2]))
    out = sys.argv[3]
    fix = json.load(open(sys.argv[4])) if len(sys.argv) > 4 else {}
    funcs = {int(a, 16): int(b, 16) for a, b in match['functions'].items()}
    data = {int(a, 16): int(b, 16) for a, b in match['data'].items()}
    for a, b in fix.items():
        funcs[int(a, 16)] = int(b, 16)
        data[int(a, 16)] = int(b, 16)
    seeds, cnames = [], {}
    for a, b in sorted(funcs.items(), key=lambda x: x[1]):
        n = names.get('0x%08X' % a)
        if not n:
            continue
        base = SUFFIX.sub('', n)
        cnames['0x%08X' % b] = '%s_%08X' % (base, b)
        seeds.append({'start': '0x%08X' % b})
    json.dump(seeds, open(out + '/map_seeds.json', 'w'), indent=0)
    json.dump(cnames, open(out + '/map_cnames.json', 'w'), indent=0)
    json.dump({'0x%08X' % a: '0x%08X' % b for a, b in sorted(data.items())}, open(out + '/addrmap.json', 'w'), indent=0)
    print('%d named functions, %d addresses' % (len(cnames), len(data)))


if __name__ == '__main__':
    main()
