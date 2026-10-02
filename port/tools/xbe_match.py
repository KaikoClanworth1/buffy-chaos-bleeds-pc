"""Match one release's default.xbe to another's: functions and the addresses
they use (globals, tables, strings), for building the port from another release.

The two are the same game built twice (the European release, version 2, and
the North American, version 1): the same functions in nearly the same order,
at shifted addresses, with a few changed. Each function is fingerprinted by its
instructions with addresses masked out; unique fingerprints pair first, and the
functions between two paired ones pair by order when they look alike. Within
each pair, instructions line up, so the absolute addresses one uses map to the
other's (votes over all pairs).

    python tools/xbe_match.py <a.xbe> <a functions.json> <b.xbe> <b functions.json> <out.json>

out.json: {"functions": {"0xA": "0xB", ...}, "data": {"0xA": "0xB", ...},
           "unmatched_a": [...], "stats": {...}}
"""
import collections
import difflib
import hashlib
import json
import struct
import sys

import capstone

LO, HI = 0x10000, 0x00800000          # the image: addresses worth mapping


def sections(xbe):
    base = struct.unpack_from('<I', xbe, 0x104)[0]
    n = struct.unpack_from('<I', xbe, 0x11C)[0]
    hdr = struct.unpack_from('<I', xbe, 0x120)[0] - base
    out = []
    for i in range(n):
        flags, va, vsize, raw, rsize = struct.unpack_from('<5I', xbe, hdr + i * 0x38)
        out.append((va, vsize, raw, rsize))
    return out


def reader(xbe):
    secs = sections(xbe)

    def read(va, n):
        for sva, vsize, raw, rsize in secs:
            if sva <= va < sva + rsize:
                return xbe[raw + (va - sva): raw + (va - sva) + min(n, sva + rsize - va)]
        return b''
    return read


MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = True


def norm(read, start, end):
    """[(token, [abs addresses in operand order])] for the function's instructions."""
    code = read(start, end - start)
    out = []
    for ins in MD.disasm(code, start):
        addrs = []
        parts = [ins.mnemonic]
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_IMM:
                v = op.imm & 0xFFFFFFFF
                if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL):
                    parts.append('T')
                    addrs.append(('t', v))
                elif LO <= v < HI:
                    parts.append('A')
                    addrs.append(('a', v))
                else:
                    parts.append('i%x' % v)
            elif op.type == capstone.x86.X86_OP_MEM:
                m = op.mem
                d = m.disp & 0xFFFFFFFF
                tag = 'm%d,%d,%d' % (m.base, m.index, m.scale)
                if LO <= d < HI and m.base == 0:
                    parts.append(tag + 'A')
                    addrs.append(('a', d))
                elif LO <= d < HI:
                    parts.append(tag + 'A')                 # [reg + table]
                    addrs.append(('a', d))
                else:
                    parts.append(tag + 'd%x' % d)
            else:
                parts.append('r%d' % op.reg)
        out.append((' '.join(parts), addrs))
    return out


def main():
    a_xbe, a_funcs, b_xbe, b_funcs, out_path = sys.argv[1:6]
    A, B = open(a_xbe, 'rb').read(), open(b_xbe, 'rb').read()
    ra, rb = reader(A), reader(B)
    fa = sorted((int(f['start'], 16), int(f['end'], 16), f.get('name', '')) for f in json.load(open(a_funcs)))
    fb = sorted((int(f['start'], 16), int(f['end'], 16), f.get('name', '')) for f in json.load(open(b_funcs)))
    na = {s: norm(ra, s, e) for s, e, _ in fa}
    nb = {s: norm(rb, s, e) for s, e, _ in fb}

    def fp(n):
        return hashlib.sha1('\n'.join(t for t, _ in n).encode()).hexdigest()
    ha = collections.defaultdict(list)
    hb = collections.defaultdict(list)
    for s, _, _ in fa:
        ha[fp(na[s])].append(s)
    for s, _, _ in fb:
        hb[fp(nb[s])].append(s)
    pair = {}
    for h, xs in ha.items():
        if len(xs) == 1 and len(hb.get(h, [])) == 1 and na[xs[0]]:
            pair[xs[0]] = hb[h][0]

    # Between consecutive pairs (in a's order), pair the rest by order when alike.
    a_starts = [s for s, _, _ in fa]
    b_starts = [s for s, _, _ in fb]
    b_index = {s: i for i, s in enumerate(b_starts)}
    anchors = sorted(pair.items())
    prev_a, prev_b = -1, -1
    for ai, (sa, sb) in enumerate(anchors + [(None, None)]):
        ia = a_starts.index(sa) if sa is not None else len(a_starts)
        ib = b_index[sb] if sb is not None else len(b_starts)
        ga = [s for s in a_starts[prev_a + 1:ia] if s not in pair]
        gb = [s for s in b_starts[prev_b + 1:ib] if s not in pair.values()]
        j = 0
        for s in ga:
            best, bj = 0.0, -1
            for k in range(j, min(len(gb), j + 4)):
                ta = [t for t, _ in na[s]]
                tb = [t for t, _ in nb[gb[k]]]
                r = difflib.SequenceMatcher(None, ta, tb, autojunk=False).ratio() if ta and tb else 0.0
                if r > best:
                    best, bj = r, k
            if bj >= 0 and best >= 0.75:
                pair[s] = gb[bj]
                j = bj + 1
        prev_a, prev_b = ia, ib

    # Call targets of paired functions pair too (a call in one is the same
    # call in the other), round after round while new pairs turn up.
    a_set = set(a_starts)
    a_end = {s: e for s, e, _ in fa}
    b_after = sorted(b_starts)
    import bisect
    for _round in range(6):
        tv = collections.defaultdict(collections.Counter)
        for sa, sb in pair.items():
            xa, xb = na[sa], nb.get(sb)
            if not xb:
                continue
            sm = difflib.SequenceMatcher(None, [t for t, _ in xa], [t for t, _ in xb], autojunk=False)
            for tag, i1, i2, j1, j2 in sm.get_opcodes():
                if tag != 'equal':
                    continue
                for k in range(i2 - i1):
                    for (ka, va), (kb, vb) in zip(xa[i1 + k][1], xb[j1 + k][1]):
                        if ka == kb == 't' and va in a_set and va not in pair:
                            tv[va][vb] += 1
        added = 0
        used = set(pair.values())
        for va, c in tv.items():
            vb, n = c.most_common(1)[0]
            if vb in used:
                continue
            if vb not in nb:
                i = bisect.bisect_right(b_after, vb)
                end = b_after[i] if i < len(b_after) else vb + (a_end[va] - va)
                nb[vb] = norm(rb, vb, end)
            pair[va] = vb
            used.add(vb)
            added += 1
        if not added:
            break

    # Addresses each pair uses, lined up instruction by instruction.
    votes = collections.defaultdict(collections.Counter)
    for sa, sb in pair.items():
        xa, xb = na[sa], nb.get(sb, [])
        sm = difflib.SequenceMatcher(None, [t for t, _ in xa], [t for t, _ in xb], autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag != 'equal':
                continue
            for k in range(i2 - i1):
                for (ka, va), (kb, vb) in zip(xa[i1 + k][1], xb[j1 + k][1]):
                    if ka == kb:
                        votes[va][vb] += 1
    data = {}
    for va, c in votes.items():
        vb, n = c.most_common(1)[0]
        if n * 2 > sum(c.values()) or n >= 3:
            data[va] = vb

    # Tables of function pointers (vtables, callbacks): a paired table's
    # slots pair the functions in them, which nothing calls directly.
    used = set(pair.values())
    added_t = 0
    for va, vb in list(data.items()):
        for k in range(256):
            wa, wb = ra(va + 4 * k, 4), rb(vb + 4 * k, 4)
            if len(wa) < 4 or len(wb) < 4:
                break
            fa_ptr = struct.unpack('<I', wa)[0]
            fb_ptr = struct.unpack('<I', wb)[0]
            if fa_ptr not in a_set:
                break
            if fa_ptr not in pair and fb_ptr not in used and LO <= fb_ptr < HI:
                pair[fa_ptr] = fb_ptr
                used.add(fb_ptr)
                added_t += 1
    print('table slots paired %d more functions' % added_t)

    # Between two paired neighbours that moved by the same amount, a function
    # moved by that amount too -- if the code there is the same (small ones
    # share fingerprints, so the unique pairing above leaves them).
    added_n = 0
    paired_a = sorted(pair)
    for s0, e0, _ in fa:
        if s0 in pair:
            continue
        i = bisect.bisect_left(paired_a, s0)
        if i == 0 or i >= len(paired_a):
            continue
        p0, q0 = paired_a[i - 1], paired_a[i]
        d = pair[p0] - p0
        if pair[q0] - q0 != d or (s0 + d) in used:
            continue
        cand = norm(rb, s0 + d, e0 + d)
        if [t for t, _ in cand] == [t for t, _ in na[s0]] and cand:
            pair[s0] = s0 + d
            nb[s0 + d] = cand
            used.add(s0 + d)
            added_n += 1
    print('neighbours paired %d more functions' % added_n)
    for sa, sb in pair.items():
        data[sa] = sb                          # a function is an address too
    unmatched = [hex(s) for s, _, _ in fa if s not in pair]
    json.dump({"functions": {'0x%08X' % a: '0x%08X' % b for a, b in sorted(pair.items())},
               "data": {'0x%08X' % a: '0x%08X' % b for a, b in sorted(data.items())},
               "unmatched_a": unmatched,
               "stats": {"a_functions": len(fa), "b_functions": len(fb), "paired": len(pair),
                         "addresses": len(data)}},
              open(out_path, 'w'), indent=1)
    print('functions: %d of %d paired (%d unmatched), %d addresses mapped'
          % (len(pair), len(fa), len(unmatched), len(data)))


if __name__ == '__main__':
    main()
