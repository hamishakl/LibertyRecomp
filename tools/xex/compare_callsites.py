import struct, sys, re, glob, collections
img = open(sys.argv[1], 'rb').read(); BASE = 0x82000000
word = lambda a: struct.unpack('>I', img[a-BASE:a-BASE+4])[0] if 0 <= a-BASE < len(img)-4 else 0
pat = re.compile(r'// bl 0x([0-9a-f]+)\n\tctx\.lr = 0x([0-9A-F]+);')
sites = []
for f in glob.glob(sys.argv[2] + '/gta4_recomp.*.cpp'):
    sites += [(int(r,16)-4, int(t,16)) for t, r in pat.findall(open(f).read())]
sites.sort()
isbl = 0; deltas = collections.Counter(); by_region = collections.defaultdict(lambda: [0,0])
for a, tg in sites:
    w = word(a); reg = (a - 0x82140000) // 0x80000
    by_region[reg][1] += 1
    if (w & 0xFC000003) == 0x48000001:
        isbl += 1; by_region[reg][0] += 1
        disp = w & 0x03FFFFFC
        if disp & 0x02000000: disp -= 0x04000000
        deltas[(a + disp) - tg] += 1
print(f"sites where the PAL image also has a bl at the same address: {isbl}/{len(sites)} ({100*isbl/len(sites):.1f}%)")
print("most common target shifts (PAL - US):", [(hex(d), n) for d, n in deltas.most_common(8)])
print("bl-at-same-address rate by 512KB region of code:")
print(' '.join(f"{100*h/t:.0f}" for _, (h, t) in sorted(by_region.items())))
