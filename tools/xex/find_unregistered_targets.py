"""List code addresses stored in data (vtables, callback tables) that the recompilation does not
register as functions. Indirect calls to them abort with 'Call to invalid or unregistered function'.

usage: find_unregistered_targets.py <patched image.bin> <generated dir> [code_base code_size]
Prints candidates sitting among other registered function pointers (vtable/callback-like) first.
"""
import re, struct, sys

img = open(sys.argv[1], 'rb').read()
gen = sys.argv[2]
B = 0x82000000
init = open(gen + '/gta4_init.h').read()
code_base = int(re.search(r'REX_CODE_BASE (0x[0-9A-Fa-f]+)', init).group(1), 16)
code_size = int(re.search(r'REX_CODE_SIZE (0x[0-9A-Fa-f]+)', init).group(1), 16)
lo, hi = code_base, code_base + code_size
registered = {int(a, 16) for a in re.findall(r'\{ ?(0x8[23][0-9A-Fa-f]{6}), ?[A-Za-z_]',
                                              open(gen + '/gta4_init.cpp').read())}
word = lambda o: struct.unpack('>I', img[o:o + 4])[0]
strong, weak = {}, {}
for off in range(0, len(img) - 4, 4):
    if lo <= B + off < hi:
        continue
    v = word(off)
    if lo <= v < hi and v % 4 == 0 and v not in registered:
        nb = [word(off + d) for d in (-8, -4, 4, 8) if 0 <= off + d < len(img) - 4]
        bucket = strong if sum(1 for x in nb if x in registered) >= 2 else weak
        bucket.setdefault(v, B + off)
print(f'registered functions: {len(registered)}; unregistered code pointers in data: '
      f'{len(strong)} among function pointers, {len(weak)} other')
for v, slot in sorted(strong.items()):
    print(f'  {v:08X}  (slot {slot:08X})')
