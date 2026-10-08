"""Fix address immediates inside hand-copied guest code (*_guest.inc) after a US->PAL remap.

Copied guest functions build data addresses with `lis rX,HI` + `addi/lwz/... rY,LO(rX)` in DECIMAL, which
remap_sources.py (hex/sub_ tokens only) cannot see. Each instruction comment is given a PAL address
from the anchors remap_sources already converted (`loc_XXXXXXXX:` labels = address of the next
instruction, `ctx.lr = R` = address of the preceding bl + 4), propagated +/-4 across contiguous runs.
Where the PAL instruction at that address differs from the copied comment ONLY in integer immediates,
the comment is replaced with the PAL text and those immediates are rewritten in the C lines up to the
next instruction comment (lis values are also rewritten in their <<16 form). Anything else is reported.

Addresses are computed in US space from the PRE-REMAP copy of each file (--orig-rev), where labels and
ctx.lr values are consistently US; a line is only touched when its copied comment equals the US instruction
at that address exactly, and the PAL instruction comes from the exact function pairing.

usage: fix_guest_immediates.py <us_generated> <pal_reference_generated> --orig-rev <git rev> [--write] <files...>
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(__file__))
import match_functions as mf

NUM = re.compile(r'-?\d+')


def shape(s):
    return NUM.sub('N', mf.ADDR.sub('A', s))


def locate(lines):
    """US address for every instruction comment, from labels and ctx.lr anchors, propagated in runs."""
    insn_idx = [i for i, l in enumerate(lines) if l.startswith('\t// ')]
    pos = {i: k for k, i in enumerate(insn_idx)}
    addr = [None] * len(insn_idx)
    pending = None
    for i, l in enumerate(lines):
        m = mf.LABEL.match(l)
        if m:
            pending = int(m.group(1), 16)
        elif i in pos:
            if pending is not None:
                addr[pos[i]] = pending
                pending = None
        else:
            m = re.search(r'ctx\.lr = 0x([0-9A-F]{8});', l)
            if m:
                j = max((x for x in insn_idx if x < i), default=None)
                if j is not None:
                    addr[pos[j]] = int(m.group(1), 16) - 4
    for k in range(1, len(addr)):
        if addr[k] is None and addr[k - 1] is not None:
            addr[k] = addr[k - 1] + 4
    for k in range(len(addr) - 2, -1, -1):
        if addr[k] is None and addr[k + 1] is not None:
            addr[k] = addr[k + 1] - 4
    return insn_idx, addr


def main():
    import subprocess
    sys.path.insert(0, os.path.dirname(__file__))
    import remap_sources as rs
    args = sys.argv[1:]
    us_dir, pal_dir = args[0], args[1]
    rev = args[args.index('--orig-rev') + 1]
    write = '--write' in args
    files = [a for i, a in enumerate(args[2:], 2) if not a.startswith('--') and args[i - 1] != '--orig-rev']
    m = mf.build_map(us_dir, pal_dir, log=lambda *_: None)
    res = rs.Resolver(m)
    us_at, pal_at = {}, {}
    for f in m['us'].values():
        us_at.update(zip(f['addrs'], f['insns']))
    for f in m['pal'].values():
        pal_at.update(zip(f['addrs'], f['insns']))
    for path in files:
        cur = open(path).read().split('\n')
        orig = subprocess.run(['git', 'show', f'{rev}:{path}'], capture_output=True, text=True, check=True).stdout.split('\n')
        assert len(orig) == len(cur), f'{path}: line count changed since {rev}'
        insn_idx, uaddr = locate(orig)
        verified = changed = 0
        problems = []
        for k, i in enumerate(insn_idx):
            a = uaddr[k]
            copied = orig[i][4:].strip()
            if a is None or us_at.get(a, '').strip() != copied:
                continue
            verified += 1
            pa = res._insn_map(res._owner(a)).get(a) if res._owner(a) in m['fmap'] else None
            new = pal_at.get(pa, '').strip() if pa is not None else ''
            old = cur[i][4:].strip()
            if not new:
                problems.append(f'{os.path.basename(path)}:{i + 1} US {a:08X} "{copied}" has no PAL twin')
                continue
            if new == old:
                continue
            if shape(new) != shape(old):
                problems.append(f'{os.path.basename(path)}:{i + 1} US {a:08X} "{old}" vs PAL "{new}"')
                continue
            on, nn = NUM.findall(mf.ADDR.sub('A', old)), NUM.findall(mf.ADDR.sub('A', new))
            repl = [(o, n) for o, n in zip(on, nn) if o != n]
            if not repl:
                continue
            cur[i] = '\t// ' + new
            is_lis = old.startswith('lis ')
            j = i + 1
            while j < len(cur) and not cur[j].startswith('\t// ') and not mf.LABEL.match(cur[j]):
                for o, n in repl:
                    if is_lis:
                        cur[j] = re.sub(rf'(?<![\w.]){int(o) << 16}(?!\d)', str(int(n) << 16), cur[j])
                    else:
                        cur[j] = re.sub(rf'(?<![\w.]){re.escape(o)}(?!\d)', n, cur[j])
                        if o.startswith('-') or n.startswith('-'):
                            cur[j] = cur[j].replace(f'+ {o}', f'+ {n}').replace(f'+ -{o.lstrip("-")}', f'+ {n}')
                j += 1
            changed += 1
        print(f'{os.path.basename(path)}: {len(insn_idx)} insn comments, {verified} verified against US, '
              f'{changed} rewritten, {len(problems)} problems')
        for p in problems[:15]:
            print('  PROBLEM', p)
        if write:
            open(path, 'w').write('\n'.join(cur))


if __name__ == '__main__':
    main()
