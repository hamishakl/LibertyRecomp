"""Apply a title update (a bare default.xexp, or an STFS TU package holding one) to a decrypted XEX basefile.
Mirrors rexglue XexModule::ApplyPatch. usage: apply_tu.py base.xex basefile.bin tu.stfs|default.xexp lzxdelta out_image.bin"""
import struct, sys, subprocess, hashlib, tempfile, os
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
base_xex, basefile, stfs_path, lzx_tool, out_path = sys.argv[1:6]
RETAIL = bytes.fromhex('20B185A59D28FDC340583FBB0896BF91')
def aes(k, data, mode): c = Cipher(algorithms.AES(k), mode).decryptor(); return c.update(data) + c.finalize()
u24le = lambda b: b[0] | b[1] << 8 | b[2] << 16
be32 = lambda b, o: struct.unpack('>I', b[o:o+4])[0]

# --- the patch: a bare default.xexp, or an STFS title-update package holding one ---
xexp = None
s = open(stfs_path, 'rb').read()
if s[:4] == b'XEX2':
    xexp = s
    print(f"bare xexp {stfs_path} size {len(s)}")
    s = b''
if xexp is None:  # STFS package: find default.xexp
    hdr = (be32(s, 0x340) + 0xFFF) & ~0xFFF
    vd = s[0x379:0x379+0x24]
    per_table = 1 if vd[2] & 1 else 2
    ft_count = struct.unpack('<H', vd[3:5])[0]; ft_block = u24le(vd[5:8])
    def blk_off(b):
        blk = b
        for lvl in (0xAA, 0x70E4, 0x4AF768):
            blk += ((b + lvl) // lvl) * per_table
            if b < lvl: break
        return hdr + (blk << 12)
    entries = b''.join(s[blk_off(ft_block+i):blk_off(ft_block+i)+0x1000] for i in range(ft_count))
    for i in range(0, len(entries), 0x40):
        e = entries[i:i+0x40]; nlen = e[0x28] & 0x3F
        if not nlen: continue
        name = e[:nlen].decode(); start = u24le(e[0x2F:0x32]); size = be32(e, 0x34)
        print(f"stfs entry {name} size {size} contiguous={bool(e[0x28]&0x40)}")
        if name.lower() == 'default.xexp':
            nblk = (size + 0xFFF) // 0x1000
            xexp = b''.join(s[blk_off(start+k):blk_off(start+k)+0x1000] for k in range(nblk))[:size]
assert xexp and xexp[:4] == b'XEX2', 'default.xexp not found/invalid'

def opt(x, key):
    for i in range(be32(x, 0x14)):
        k, v = struct.unpack('>II', x[0x18+i*8:0x20+i*8])
        if k == key: return v
pd = opt(xexp, 0x000005FF); ffp = opt(xexp, 0x000003FF)
tgt_ver, src_ver = be32(xexp, pd+4), be32(xexp, pd+8)
print(f"xexp: target version {tgt_ver:#x}  source version {src_ver:#x}")
(sz_tgt_hdr, h_src_off, h_src_sz, h_tgt_off, i_src_off, i_src_sz, i_tgt_off) = struct.unpack('>7I', xexp[pd+0x30:pd+0x4C])
window = be32(xexp, ffp+8)

# --- header patch ---
bx = open(base_xex, 'rb').read()
header = bytearray(bx[:be32(bx, 8)])
target = sz_tgt_hdr or (h_tgt_off + h_src_sz)
if target > len(header): header += b'\0' * (target - len(header))
if h_src_off: header[h_tgt_off:h_tgt_off+h_src_sz] = header[h_src_off:h_src_off+h_src_sz]
if target < len(header): header[target:] = b'\0' * (len(header) - target)
info_len = struct.unpack('>H', xexp[pd+0x4C+10:pd+0x4C+12])[0] + 0xC
def run_lzx(dest, patch):
    with tempfile.TemporaryDirectory() as t:
        dp, pp = os.path.join(t, 'd'), os.path.join(t, 'p')
        open(dp, 'wb').write(dest); open(pp, 'wb').write(patch)
        r = subprocess.run([lzx_tool, str(window), dp, pp], capture_output=True, text=True)
        if r.returncode: raise SystemExit('lzxdelta failed: ' + r.stderr)
        return bytearray(open(dp, 'rb').read())
header = run_lzx(bytes(header), xexp[pd+0x4C:pd+0x4C+info_len])[:target]

sec = be32(header, 0x10); new_img = be32(header, sec+4)
old_key = aes(RETAIL, bx[be32(bx,0x10)+0x150:be32(bx,0x10)+0x160], modes.ECB())
base_key = aes(RETAIL, bytes(header[sec+0x150:sec+0x160]), modes.ECB())
xsec = be32(xexp, 0x10)
patch_key = aes(base_key, xexp[xsec+0x150:xsec+0x160], modes.ECB())
assert aes(base_key, xexp[pd+0x20:pd+0x30], modes.ECB()) == old_key, 'image key check failed'
print(f"new image size {new_img:#x}; keys verified")

# --- image patch ---
img = bytearray(open(basefile, 'rb').read())
if new_img > len(img): img += b'\0' * (new_img - len(img))
if i_src_off: img[i_tgt_off:i_tgt_off+i_src_sz] = img[i_src_off:i_src_off+i_src_sz]
data = xexp[be32(xexp, 8):]; data = data[:len(data)//16*16]
data = aes(patch_key, data, modes.CBC(b'\0'*16))
blk_size, blk_hash = be32(xexp, ffp+12), xexp[ffp+16:ffp+36]
p = 0; nblocks = 0
while blk_size:
    block = data[p:p+blk_size]
    assert hashlib.sha1(block).digest() == blk_hash, f'block {nblocks} hash mismatch'
    next_size, next_hash = be32(block, 0), block[4:24]
    img = run_lzx(bytes(img), block[24:])
    p += blk_size; blk_size, blk_hash = next_size, next_hash; nblocks += 1
img = img[:new_img]
open(out_path, 'wb').write(img)
open(out_path + '.xexp', 'wb').write(xexp)
print(f"applied {nblocks} blocks (all SHA-1 verified) -> {out_path} ({len(img):#x} bytes)")
