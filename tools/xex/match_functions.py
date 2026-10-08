"""Build a US -> PAL guest address map from two rexglue codegen trees.

Both trees carry the same disassembler's per-instruction comments, so each function is fingerprinted
by its instruction stream with absolute addresses stripped. The function lists are then aligned in
order (difflib), equal-length replaced runs are paired when similar, and the matched pairs vote on
data addresses (lis + addi/lwz/... pairs) and instruction addresses (ctx.lr return values).

usage: match_functions.py <us_generated_dir> <pal_generated_dir> <out.json>
"""
from __future__ import annotations
import collections, difflib, glob, hashlib, json, re, sys

FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', re.M)
INSN = re.compile(r'^\t// (.+)$', re.M)
ADDR = re.compile(r'0x8[0-9a-fA-F]{7}')
LABEL = re.compile(r'^loc_([0-9A-F]{8}):')
LR = re.compile(r'ctx\.lr = 0x([0-9A-F]{8});')


LIS = re.compile(r'^lis (r\d+),-?\d+$')
LO_ADDI = re.compile(r'^(addi|addic|ori) (r\d+),(r\d+),-?\d+$')
LO_MEM = re.compile(r'^(\w+) ([rfv]\d+),-?\d+\((r\d+)\)$')


def normalise(insns):
    """Strip absolute code addresses and lis-built data addresses (hi + paired lo immediate)."""
    out, hi_regs = [], set()
    for s in insns:
        s = ADDR.sub('A', s)
        if LIS.match(s):
            reg = LIS.match(s).group(1)
            hi_regs.add(reg)
            out.append(f'lis {reg},H')
            continue
        m = LO_ADDI.match(s)
        if m and m.group(3) in hi_regs:
            out.append(f'{m.group(1)} {m.group(2)},{m.group(3)},L')
            if m.group(2) != m.group(3):
                hi_regs.discard(m.group(2))
            continue
        m = LO_MEM.match(s)
        if m and m.group(3) in hi_regs:
            out.append(f'{m.group(1)} {m.group(2)},L({m.group(3)})')
            continue
        dst = re.match(r'^\w+\.? (r\d+),', s)
        if dst:
            hi_regs.discard(dst.group(1))
        out.append(s)
    return out


def load(gen_dir):
    funcs = {}
    for path in sorted(glob.glob(gen_dir + '/gta4_recomp.*.cpp')):
        text = open(path).read()
        starts = [(m.start(), int(m.group(1), 16)) for m in FUNC.finditer(text)]
        for i, (pos, addr) in enumerate(starts):
            body = text[pos:starts[i + 1][0] if i + 1 < len(starts) else len(text)]
            insns, addrs, cur = [], [], addr
            for line in body.split('\n'):
                lab = LABEL.match(line)
                if lab:
                    cur = int(lab.group(1), 16)
                elif line.startswith('\t// '):
                    insns.append(line[4:]); addrs.append(cur); cur += 4
            norm = normalise(insns)
            funcs[addr] = {
                'insns': insns,
                'addrs': addrs,
                'hash': hashlib.sha1('\n'.join(norm).encode()).hexdigest()[:16],
                'lrs': [int(x, 16) for x in LR.findall(body)],
            }
    return funcs


def similarity(a, b):
    if abs(len(a['insns']) - len(b['insns'])) > max(8, len(a['insns']) // 4):
        return 0.0
    na, nb = normalise(a['insns']), normalise(b['insns'])
    return difflib.SequenceMatcher(None, na, nb, autojunk=False).ratio()


def data_refs(insns):
    """Absolute addresses built with lis rX,hi + addi/ori/load/store rY,lo(rX). Returns list in order."""
    hi = {}
    out = []
    for s in insns:
        m = re.match(r'lis (r\d+),(-?\d+)$', s)
        if m:
            hi[m.group(1)] = int(m.group(2)) & 0xFFFF
            continue
        m = re.match(r'addi (r\d+),(r\d+),(-?\d+)$', s)
        if m and m.group(2) in hi:
            out.append(((hi[m.group(2)] << 16) + int(m.group(3))) & 0xFFFFFFFF)
            continue
        m = re.match(r'\w+ [rf]\d+,(-?\d+)\((r\d+)\)$', s)
        if m and m.group(2) in hi:
            out.append(((hi[m.group(2)] << 16) + int(m.group(1))) & 0xFFFFFFFF)
    return out


def build_map(us_dir, pal_dir, log=print):
    us, pal = load(us_dir), load(pal_dir)
    us_order, pal_order = sorted(us), sorted(pal)
    log(f'functions: US {len(us)}, PAL {len(pal)}')

    sm = difflib.SequenceMatcher(None, [us[a]['hash'] for a in us_order],
                                 [pal[a]['hash'] for a in pal_order], autojunk=False)
    fmap, conf = {}, {}
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            for k in range(i2 - i1):
                fmap[us_order[i1 + k]] = pal_order[j1 + k]; conf[us_order[i1 + k]] = 1.0
        elif tag == 'replace':
            # greedy in-order pairing of similar functions inside the replaced window
            j = j1
            for i in range(i1, i2):
                best, best_j = 0.0, None
                for jj in range(j, min(j2, j + 4)):
                    s = similarity(us[us_order[i]], pal[pal_order[jj]])
                    if s > best:
                        best, best_j = s, jj
                if best_j is not None and best >= 0.6:
                    fmap[us_order[i]] = pal_order[best_j]; conf[us_order[i]] = round(best, 3); j = best_j + 1

    # Pass 2: globally unique fingerprints among the still-unmatched functions.
    taken = set(fmap.values())
    us_left = collections.defaultdict(list); pal_left = collections.defaultdict(list)
    for a in us_order:
        if a not in fmap: us_left[us[a]['hash']].append(a)
    for a in pal_order:
        if a not in taken: pal_left[pal[a]['hash']].append(a)
    for h, ua in us_left.items():
        if len(ua) == 1 and len(pal_left.get(h, [])) == 1:
            fmap[ua[0]] = pal_left[h][0]; conf[ua[0]] = 1.0; taken.add(pal_left[h][0])

    # Pass 3: call-graph propagation. In an exact pair the k-th call targets correspond.
    call = re.compile(r'^bl (0x[0-9a-f]{8})$')
    changed = True
    while changed:
        changed = False
        for ua, pa in list(fmap.items()):
            if conf[ua] != 1.0: continue
            uc = [int(m.group(1), 16) for s in us[ua]['insns'] if (m := call.match(s))]
            pc = [int(m.group(1), 16) for s in pal[pa]['insns'] if (m := call.match(s))]
            if len(uc) != len(pc): continue
            for x, y in zip(uc, pc):
                if x in us and y in pal and x not in fmap and y not in taken:
                    fmap[x] = y; conf[x] = round(similarity(us[x], pal[y]), 3) or 0.01; taken.add(y); changed = True

    exact = sum(1 for v in conf.values() if v == 1.0)
    log(f'mapped functions: {len(fmap)}/{len(us)} ({exact} exact, {len(fmap) - exact} fuzzy)')

    # Data addresses + lr return addresses voted from exact matches with identical instruction counts.
    dvotes = collections.defaultdict(collections.Counter)
    lrmap = {}
    for ua, pa in fmap.items():
        u, p = us[ua], pal[pa]
        if conf[ua] == 1.0:
            for x, y in zip(data_refs(u['insns']), data_refs(p['insns'])):
                if x >= 0x82000000 and y >= 0x82000000:
                    dvotes[x][y] += 1
            if len(u['lrs']) == len(p['lrs']):
                for x, y in zip(u['lrs'], p['lrs']):
                    lrmap[x] = y
    dmap, dconflict = {}, 0
    for x, c in dvotes.items():
        (y, n), = c.most_common(1)
        total = sum(c.values())
        if n == total or (n >= 3 and n >= 0.75 * total):
            dmap[x] = y
        else:
            dconflict += 1

    # Branch/call targets: in exact pairs every code address operand corresponds pairwise. Covers code
    # addresses that are not separate functions on one side (CRT natives, forced boundaries, thunks).
    tvotes = collections.defaultdict(collections.Counter)
    for ua, pa in fmap.items():
        if conf[ua] != 1.0:
            continue
        ut = [int(x, 16) for s_ in us[ua]['insns'] for x in ADDR.findall(s_)]
        pt = [int(x, 16) for s_ in pal[pa]['insns'] for x in ADDR.findall(s_)]
        if len(ut) == len(pt):
            for x, y in zip(ut, pt):
                tvotes[x][y] += 1
    tmap = {}
    for x, c in tvotes.items():
        (y, n), = c.most_common(1)
        if n == sum(c.values()):
            tmap[x] = y
    log(f'branch/call targets: {len(tmap)}')
    log(f'data addresses: {len(dmap)} unanimous, {dconflict} conflicting; lr return addresses: {len(lrmap)}')

    return {'us': us, 'pal': pal, 'us_order': us_order, 'fmap': fmap, 'conf': conf, 'dmap': dmap, 'lrmap': lrmap,
            'tmap': tmap}


def main():
    us_dir, pal_dir, out_path = sys.argv[1:4]
    m = build_map(us_dir, pal_dir)
    hexd = lambda d: {f'0x{k:08X}': f'0x{v:08X}' for k, v in sorted(d.items())}
    json.dump({'functions': hexd(m['fmap']),
               'function_confidence': {f'0x{k:08X}': v for k, v in sorted(m['conf'].items()) if v < 1.0},
               'data': hexd(m['dmap']), 'lr': hexd(m['lrmap']),
               'unmapped_us_functions': [f'0x{a:08X}' for a in m['us_order'] if a not in m['fmap']]},
              open(out_path, 'w'), indent=1)
    print('wrote', out_path)


if __name__ == '__main__':
    main()
