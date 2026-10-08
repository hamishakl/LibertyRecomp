"""Decrypt + decompress (basic compression) a retail XEX2 into its raw image.
usage: extract_basefile.py default.xex out_basefile.bin   (needs: pip install cryptography)"""
import struct, sys, re, glob, random
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
d = open(sys.argv[1], 'rb').read()
_, pe_off, _, sec_off, n = struct.unpack('>IIIII', d[4:0x18])
ff = None
for i in range(n):
    k, v = struct.unpack('>II', d[0x18+i*8:0x20+i*8])
    if k == 0x000003FF: ff = v
img_size = struct.unpack('>I', d[sec_off+4:sec_off+8])[0]
RETAIL = bytes.fromhex('20B185A59D28FDC340583FBB0896BF91')
enc_key = d[sec_off+0x150:sec_off+0x160]
dec = lambda k, data, m: (lambda c: c.update(data) + c.finalize())(Cipher(algorithms.AES(k), m).decryptor())
key = dec(RETAIL, enc_key, modes.ECB())
payload = d[pe_off:]; payload = payload[:len(payload)//16*16]
plain = dec(key, payload, modes.CBC(b'\0'*16))
fsz = struct.unpack('>I', d[ff:ff+4])[0]
out = bytearray(); src = 0
for off in range(ff+8, ff+fsz, 8):
    dsz, zsz = struct.unpack('>II', d[off:off+8])
    out += plain[src:src+dsz] + b'\0'*zsz; src += dsz
print("basefile", hex(len(out)), "expected image", hex(img_size), "MZ" if out[:2]==b'MZ' else "NOT MZ - bad decrypt")
open(sys.argv[2], 'wb').write(out)
