"""Rewrite US guest addresses in source files to their PAL equivalents.

Resolves sub_XXXXXXXX / __imp__sub_XXXXXXXX names and 0x82xxxxxx/0x83xxxxxx literals through the map
built by match_functions.build_map:
  1. function entry           (functions[])
  2. instruction inside a matched function (instruction-level alignment; exact pairs map 1:1)
  3. data address             (unanimous lis-pair votes)
  4. data address inferred    (nearest mapped neighbours below and above agree on the shift, gap <= 64 KiB)

Dry run by default: prints a per-file summary and every unresolved reference. --write applies it.
usage: remap_sources.py <us_generated> <pal_generated> [--write] [--report out.json] <files/dirs...>
"""
from __future__ import annotations
import bisect, collections, difflib, glob, json, os, re, sys

sys.path.insert(0, os.path.dirname(__file__))
import match_functions as mf

TOKEN = re.compile(r'(__imp__sub_|sub_|0x|0X)(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])')
SOURCE_EXT = ('.cpp', '.h', '.inc', '.mm', '.toml')


class Resolver:
    def __init__(self, m):
        self.m = m
        self.us, self.pal, self.fmap, self.conf = m['us'], m['pal'], m['fmap'], m['conf']
        self.starts = sorted(self.us)
        self.data = dict(m['dmap'])
        self.data_keys = sorted(self.data)
        self._insn_cache = {}
        manual = json.load(open(os.path.join(os.path.dirname(__file__), 'manual_map.json')))
        self.manual_code = {int(k, 16): int(v[0], 16) for k, v in manual['code'].items()}
        self.manual_data = {int(k, 16): int(v[0], 16) for k, v in manual['data'].items()}
        self.known_unresolved = {int(k, 16) for k in manual['unresolved']}
        # Instruction anchors from exact pairs: code shifts are piecewise constant, so the nearest
        # anchors either side agreeing on a shift is strong evidence for any code address between them.
        anchors = {}
        for ua, pa in self.fmap.items():
            u, p = self.us[ua], self.pal[pa]
            if self.conf[ua] == 1.0 and len(u['addrs']) == len(p['addrs']):
                anchors.update(zip(u['addrs'], p['addrs']))
        self.anchor_keys = sorted(anchors)
        self.anchors = anchors

    def _insn_map(self, ua):
        if ua in self._insn_cache:
            return self._insn_cache[ua]
        u, p = self.us[ua], self.pal[self.fmap[ua]]
        out = {}
        if self.conf[ua] == 1.0 and len(u['addrs']) == len(p['addrs']):
            out = dict(zip(u['addrs'], p['addrs']))
        else:
            sm = difflib.SequenceMatcher(None, mf.normalise(u['insns']), mf.normalise(p['insns']), autojunk=False)
            for tag, i1, i2, j1, j2 in sm.get_opcodes():
                if tag == 'equal':
                    for k in range(i2 - i1):
                        out[u['addrs'][i1 + k]] = p['addrs'][j1 + k]
        self._insn_cache[ua] = out
        return out

    def local_shift(self, a):
        if a in self.anchors:
            return self.anchors[a]
        i = bisect.bisect_left(self.anchor_keys, a)
        if 0 < i < len(self.anchor_keys):
            lo, hi = self.anchor_keys[i - 1], self.anchor_keys[i]
            if self.anchors[lo] - lo == self.anchors[hi] - hi and hi - lo <= 0x2000:
                return a + (self.anchors[lo] - lo)
        return None

    def code(self, a):
        y, how = self._code(a)
        ls = self.local_shift(a) if 0x82140000 <= a < 0x82B00000 else None
        if how in ('function', 'instruction') and self.conf.get(self._owner(a), 1.0) < 1.0 and ls is not None and ls != y:
            return ls, 'local-shift(overrode fuzzy)'
        if y is None and ls is not None:
            return ls, 'local-shift'
        return y, how

    def _owner(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i] if i >= 0 else None

    def _code(self, a):
        if a in self.fmap:
            return self.fmap[a], 'function'
        if a in self.m['tmap']:
            return self.m['tmap'][a], 'branch-target'
        i = bisect.bisect_right(self.starts, a) - 1
        if i < 0:
            return None, None
        ua = self.starts[i]
        if ua not in self.fmap or not self.us[ua]['addrs'] or a > self.us[ua]['addrs'][-1]:
            return self.m['lrmap'].get(a), 'lr' if a in self.m['lrmap'] else None
        y = self._insn_map(ua).get(a)
        return (y, 'instruction') if y is not None else (None, None)

    def data_addr(self, a):
        if a in self.data:
            return self.data[a], 'data'
        i = bisect.bisect_left(self.data_keys, a)
        if 0 < i < len(self.data_keys):
            lo, hi = self.data_keys[i - 1], self.data_keys[i]
            d_lo, d_hi = self.data[lo] - lo, self.data[hi] - hi
            if d_lo == d_hi and hi - lo <= 0x10000:
                return a + d_lo, 'data-inferred'
        return None, None

    def resolve(self, a, is_func_name):
        if a in self.known_unresolved:
            return None, None
        if a in self.manual_code:
            return self.manual_code[a], 'manual'
        if a in self.manual_data and not is_func_name:
            return self.manual_data[a], 'manual'
        y, how = self.code(a)
        if y is None and not is_func_name:
            y, how = self.data_addr(a)
        return y, how


def rewrite_text(text, res, stats, unresolved, path):
    lines = text.split('\n')
    for n, line in enumerate(lines):
        def sub(m):
            prefix, digits = m.group(1), m.group(2)
            a = int(digits, 16)
            y, how = res.resolve(a, prefix.endswith('sub_'))
            if y is None:
                unresolved.append((path, n + 1, f'{prefix}{digits}', line.strip()[:120]))
                return m.group(0)
            stats[how] += 1
            new = f'{y:08X}' if digits.upper() == digits else f'{y:08x}'
            return prefix + new
        lines[n] = TOKEN.sub(sub, line)
    return '\n'.join(lines)


def main():
    args = sys.argv[1:]
    us_dir, pal_dir = args[0], args[1]
    write = '--write' in args
    report = args[args.index('--report') + 1] if '--report' in args else None
    targets = [a for i, a in enumerate(args[2:], 2) if not a.startswith('--') and args[i - 1] != '--report']
    files = []
    for t in targets:
        files += [t] if os.path.isfile(t) else sorted(f for f in glob.glob(t + '/**/*', recursive=True)
                                                       if f.endswith(SOURCE_EXT))
    res = Resolver(mf.build_map(us_dir, pal_dir, log=lambda *_: None))
    total, unresolved, changed = collections.Counter(), [], 0
    for f in files:
        text = open(f, errors='surrogateescape').read()
        stats = collections.Counter()
        new = rewrite_text(text, res, stats, unresolved, f)
        total.update(stats)
        if new != text:
            changed += 1
            if write:
                open(f, 'w', errors='surrogateescape').write(new)
    print(f'{len(files)} files, {changed} would change{" (written)" if write else " (dry run)"}')
    for k, v in total.most_common():
        print(f'  {k:14s} {v}')
    print(f'  {"UNRESOLVED":14s} {len(unresolved)}')
    by_tok = collections.Counter(u[2] for u in unresolved)
    print('unresolved tokens (distinct):', len(by_tok))
    for tok, c in by_tok.most_common(40):
        ex = next(u for u in unresolved if u[2] == tok)
        print(f'  {tok:16s} x{c:<3d} {os.path.basename(ex[0])}:{ex[1]}  {ex[3][:80]}')
    if report:
        json.dump([{'file': u[0], 'line': u[1], 'token': u[2], 'text': u[3]} for u in unresolved],
                  open(report, 'w'), indent=1)


if __name__ == '__main__':
    main()
