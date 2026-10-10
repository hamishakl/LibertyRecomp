"""Review the fork's hooks on functions that PAL's TU5 changed.

Every `extern "C" void sub_XXXXXXXX(` or `..._HOOK(sub_XXXXXXXX)` in the hook sources names a PAL
function. For each one whose US pair (match_functions.build_map) is not an exact match, the US and
PAL instruction streams are diffed and every hunk is classified:

  relocated-global   one instruction, same mnemonic and registers, only a displacement or immediate
                     differs, and the base register holds a `lis` value (data moved between builds)
  relocated-global?  same shape, but the base register came from a spill or a saved register the
                     linear tracker lost; the offset delta is printed so it can be matched by eye
  structural         anything else (inserted/removed/reordered code) - read the hook

A hook that only wraps the call (reads argument registers, lr and the result) is unaffected by
relocated globals. Guest constants the hook reads (`constexpr uint32_t k... = 0x8...`) are checked
against the globals each function actually touches: a constant whose address the PAL function
references is confirmed; the same for any src `0x8...` literal that falls inside the function body
(call-site return addresses used as `ctx.lr` keys), which must sit at the same instruction index.

usage: review_changed_hooks.py <gta4-recomp dir> [--src <dir>] [--pairs US:PAL,...] [--all]
  --all    also list exact pairs that are hooked (normally skipped)
"""
from __future__ import annotations
import bisect, collections, difflib, glob, os, re, sys

sys.path.insert(0, os.path.dirname(__file__))
import match_functions as mf

HOOK = re.compile(r'(?:extern "C" void|_HOOK\()\s*(sub_8[23][0-9A-F]{6})\b')
LITERAL = re.compile(r'0x(8[23][0-9A-F]{6})\b')
LIS = re.compile(r'^lis (r\d+),(-?\d+)$')
ADDI = re.compile(r'^(?:addi|addic) (r\d+),(r\d+),(-?\d+)$')
ORI = re.compile(r'^ori (r\d+),(r\d+),(\d+)$')
MEM = re.compile(r'^(\w+) ([rfv]\d+),(-?\d+)\((r\d+)\)$')
DST = re.compile(r'^\w+\.? (r\d+),')
SHAPE = re.compile(r'-?\d+')


def lis_refs(f):
    """Absolute data addresses formed by lis + lo-half pairs, tracked linearly through the body."""
    regs, out = {}, {}
    for addr, s in zip(f['addrs'], f['insns']):
        m = LIS.match(s)
        if m:
            regs[m.group(1)] = (int(m.group(2)) << 16) & 0xffffffff
            continue
        m = ADDI.match(s)
        if m and m.group(2) in regs:
            out[addr] = (regs[m.group(2)] + int(m.group(3))) & 0xffffffff
            if m.group(1) != m.group(2):
                regs.pop(m.group(1), None)
            continue
        m = ORI.match(s)
        if m and m.group(2) in regs:
            out[addr] = regs[m.group(1)] = regs[m.group(2)] | int(m.group(3))
            continue
        m = MEM.match(s)
        if m and m.group(4) in regs:
            out[addr] = (regs[m.group(4)] + int(m.group(3))) & 0xffffffff
            if m.group(1).endswith('u'):
                regs.pop(m.group(4), None)
        d = DST.match(s)
        if d and not s.startswith('st'):
            regs.pop(d.group(1), None)
        if s.startswith(('bl ', 'bctrl', 'blrl')):
            for r in ('r0', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12'):
                regs.pop(r, None)
    return out


def classify(u, p, i, j, ru, rp):
    su, sp = u['insns'][i], p['insns'][j]
    if SHAPE.sub('N', su) != SHAPE.sub('N', sp):
        return 'structural', ''
    au, ap = u['addrs'][i], p['addrs'][j]
    if au in ru and ap in rp:
        return 'relocated-global', f'{ru[au]:08X} -> {rp[ap]:08X}'
    nu, np_ = [int(x) for x in SHAPE.findall(su)], [int(x) for x in SHAPE.findall(sp)]
    delta = [b - a for a, b in zip(nu, np_) if a != b]
    return 'relocated-global?', f'delta {delta[0]:+#x}' if len(delta) == 1 else 'delta ' + str(delta)


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    root = args[0]
    src = root + '/src'
    pairs_arg, show_all = None, False
    for k, a in enumerate(args):
        if a == '--src':
            src = args[k + 1]
        if a == '--pairs':
            pairs_arg = [tuple(int(x, 16) for x in s.split(':')) for s in args[k + 1].split(',')]
        if a == '--all':
            show_all = True

    print('building US->PAL map ...', file=sys.stderr)
    m = mf.build_map(f'{root}/generated', f'{root}/generated_pal_raw', log=lambda *_: None)
    us, pal, fmap, conf = m['us'], m['pal'], m['fmap'], m['conf']
    inverse = {p: u for u, p in fmap.items()}

    files = [f for f in glob.glob(src + '/**/*', recursive=True)
             if f.endswith(('.cpp', '.h', '.inc', '.mm'))]
    hooked, literals, constants = set(), collections.defaultdict(set), {}
    for f in files:
        text = open(f, errors='ignore').read()
        for mm in HOOK.finditer(text):
            hooked.add(int(mm.group(1)[4:], 16))
        for line in text.splitlines():
            for mm in LITERAL.finditer(line):
                literals[int(mm.group(1), 16)].add(os.path.basename(f))
            cm = re.search(r'constexpr\s+\w+\s+(k\w+)\s*=\s*0x(8[23][0-9A-F]{6})', line)
            if cm:
                constants.setdefault(int(cm.group(2), 16), set()).add(cm.group(1))

    if pairs_arg:
        pairs = pairs_arg
    else:
        pairs = sorted((inverse[p], p) for p in hooked if p in inverse
                       and (show_all or conf.get(inverse[p], 1.0) < 1.0))
        missing = sorted(p for p in hooked if p not in inverse and p in pal)
        if missing:
            print('hooked PAL functions with no US pair (PAL-only or unmatched): ' +
                  ' '.join(f'{p:08X}' for p in missing))
    print(f'{len(hooked)} hooked PAL functions in {len(files)} files; {len(pairs)} pairs to review\n')

    pal_starts = sorted(pal)
    verdicts = collections.Counter()
    for ua, pa in pairs:
        u, p = us[ua], pal[pa]
        ru, rp = lis_refs(u), lis_refs(p)
        sm = difflib.SequenceMatcher(None, mf.normalise(u['insns']), mf.normalise(p['insns']), autojunk=False)
        hunks = [op for op in sm.get_opcodes() if op[0] != 'equal']
        kinds = collections.Counter()
        lines = []
        for tag, i1, i2, j1, j2 in hunks:
            if tag == 'replace' and i2 - i1 == 1 and j2 - j1 == 1:
                kind, note = classify(u, p, i1, j1, ru, rp)
                kinds[kind] += 1
                lines.append(f'    {kind:18s} @{u["addrs"][i1]:08X}/{p["addrs"][j1]:08X}  '
                             f'{u["insns"][i1]}  |  {p["insns"][j1]}  {note}')
            else:
                kinds['structural'] += 1
                lines.append(f'    structural         {tag} US[{i1}:{i2}] PAL[{j1}:{j2}]')
                for k in range(i1, i2):
                    lines.append(f'      - US  {u["addrs"][k]:08X} {u["insns"][k]}')
                for k in range(j1, j2):
                    lines.append(f'      + PAL {p["addrs"][k]:08X} {p["insns"][k]}')
        verdict = 'STRUCTURAL' if kinds['structural'] else 'globals-only'
        verdicts[verdict] += 1
        print(f'== US {ua:08X} -> PAL {pa:08X}  similarity {conf.get(ua, 1.0):.3f}  '
              f'{len(u["insns"])}/{len(p["insns"])} insns  hunks {len(hunks)}  [{verdict}]')
        print('\n'.join(lines))

        # Hook constants that this function references (by its PAL globals).
        touched = sorted({a for a in rp.values() if a in constants})
        if touched:
            print('    constants referenced by the PAL body: ' +
                  ', '.join(f'{a:08X} ({"/".join(sorted(constants[a]))})' for a in touched))
        # Source literals inside the function body: return-address keys.
        inside = [a for a in literals if pa < a < pa + 4 * len(p['addrs'])]
        for a in sorted(inside):
            k = bisect.bisect_left(p['addrs'], a)
            at = p['addrs'][k] == a if k < len(p['addrs']) else False
            same_len = len(u['insns']) == len(p['insns'])
            ret = k > 0 and p['insns'][k - 1].startswith('bl ')
            print(f'    literal {a:08X} ({"/".join(sorted(literals[a]))}): '
                  f'{"instruction boundary" if at else "NOT an instruction boundary"}, '
                  f'{"after " + p["insns"][k - 1] if ret else "not a call return"}, '
                  f'{"same index as US " + format(u["addrs"][k], "08X") if same_len and at else "lengths differ - check index"}')
        print()
    print('summary:', dict(verdicts))


if __name__ == '__main__':
    main()
