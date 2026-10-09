"""List code addresses that recompiled code materialises with lis/addi (address-taken callbacks,
vtable thunks) but that the recompilation does not register as functions. Calling one through
bctrl aborts with 'Call to invalid or unregistered function'. Complements find_unregistered_targets.py,
which scans pointers stored in DATA; this scans immediates in CODE.
usage: find_address_taken_targets.py <patched image.bin> <generated dir>
Only entry-point-shaped targets are listed (virtual-call thunk, adjustor thunk, mflr prologue), and
never an address that is already a label inside a generated function. A bare branch is NOT an entry
point: 0x8290001C looked address-taken but was a loop-body branch, and registering it split its
function (codegen reported an unresolved conditional branch).
"""
import collections, glob, re, struct, sys

img = open(sys.argv[1], 'rb').read()
gen = sys.argv[2]
B = 0x82000000
init = open(gen + '/gta4_init.h').read()
code_base = int(re.search(r'REX_CODE_BASE (0x[0-9A-Fa-f]+)', init).group(1), 16)
code_size = int(re.search(r'REX_CODE_SIZE (0x[0-9A-Fa-f]+)', init).group(1), 16)
registered = {int(a, 16) for a in re.findall(r'\{ ?(0x8[23][0-9A-Fa-f]{6}), ?[A-Za-z_]',
                                              open(gen + '/gta4_init.cpp').read())}
hits, labels = collections.defaultdict(set), set()
for path in sorted(glob.glob(gen + '/gta4_recomp.*.cpp')):
    text = open(path).read()
    labels |= {int(a, 16) for a in re.findall(r'^loc_([0-9A-F]{8}):', text, re.M)}
    func, lis = None, {}
    for line in text.split('\n'):
        m = re.match(r'DEFINE_REX_FUNC\((\w+)\)', line)
        if m:
            func, lis = m.group(1), {}
            continue
        m = re.match(r'\s*// lis (r\d+),(-?\d+)', line)
        if m:
            lis[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            continue
        m = re.match(r'\s*// addi (r\d+),(r\d+),(-?\d+)', line)
        if m and m.group(2) in lis:
            v = (lis[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
            if code_base <= v < code_base + code_size and v % 4 == 0 and v not in registered:
                hits[v].add(func)

word = lambda a: struct.unpack('>I', img[a - B:a - B + 4])[0]

def shape(a):
    w0, w1, w2, w3 = (word(a + 4 * k) for k in range(4))
    if w0 == 0x81830000 and (w1 >> 16) == 0x816C and w2 == 0x7D6903A6 and w3 == 0x4E800420:
        return f'vcall thunk slot {w1 & 0xFFFF:#x}'
    if (w0 >> 16) == 0x3863 and (w1 >> 26) == 18:
        return 'adjustor thunk (addi r3; b)'
    if w0 in (0x7C0802A6, 0x7D8802A6):
        return 'prologue (mflr)'
    return None

found = 0
for v in sorted(hits):
    if v in labels:
        continue
    kind = shape(v)
    if not kind:
        continue
    found += 1
    users = sorted(hits[v])
    print(f'0x{v:08X}  {kind:30s} used by {", ".join(users[:4])}{" ..." if len(users) > 4 else ""}')
print(f'{found} unregistered address-taken entry points ({len(hits)} raw lis/addi code addresses)')
