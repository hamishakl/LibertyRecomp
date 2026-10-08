"""Report how many guest addresses referenced by the hook sources resolve through a US->PAL map.
usage: hook_coverage.py map.json <src files/dirs...>"""
import json, re, sys, os, glob, bisect, collections
m = json.load(open(sys.argv[1]))
fn = {int(k,16): int(v,16) for k,v in m['functions'].items()}
data = {int(k,16): int(v,16) for k,v in m['data'].items()}
lr = {int(k,16): int(v,16) for k,v in m['lr'].items()}
exact = set(fn) - {int(k,16) for k in m['function_confidence']}
starts = sorted(fn)
files = []
for p in sys.argv[2:]:
    files += [p] if os.path.isfile(p) else [f for f in glob.glob(p + '/**/*', recursive=True) if f.endswith(('.cpp','.h','.inc','.toml','.mm'))]
refs = collections.Counter(); kinds = collections.Counter(); miss = collections.Counter()
for f in files:
    t = open(f, errors='ignore').read()
    for a in set(int(x,16) for x in re.findall(r'(?:sub_|0x)(8[2-3][0-9A-Fa-f]{6})\b', t)):
        refs[a] += 1
for a in refs:
    if a in fn: k = 'function'
    elif a in lr: k = 'lr/return'
    elif a in data: k = 'data'
    else:
        i = bisect.bisect_right(starts, a) - 1
        k = 'inside-exact-function' if i >= 0 and starts[i] in exact and a - starts[i] < 0x20000 and a < 0x82A90000 else 'UNMAPPED'
    kinds[k] += 1
    if k == 'UNMAPPED': miss[a] = refs[a]
print(f"{len(refs)} distinct guest addresses referenced in {len(files)} files")
for k, n in kinds.most_common(): print(f"  {k:24s} {n}")
print("unmapped sample:", ' '.join(f"{a:08X}" for a,_ in miss.most_common(25)))
