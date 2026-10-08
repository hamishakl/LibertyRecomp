"""Merge extra XenosRecomp shader-cache entries into an existing shader_cache.cpp.

Existing entries and their compiled blobs are kept byte-for-byte; extra entries (e.g. PAL-only shaders
embedded in the PAL executable) are appended to the decompressed DXIL/SPIR-V/AIR buffers with shifted
offsets, the entry table is re-sorted by hash, and the buffers are recompressed with zstd.
usage: merge_shader_cache.py <base shader_cache.cpp> <extra shader_cache.cpp> <out shader_cache.cpp>
Requires: pip install zstandard
"""
import re, sys
import zstandard

ENTRY = re.compile(r'^\t\{ (0x[0-9A-F]+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), "([^"]*)", nullptr, (\d+) \},$', re.M)
KINDS = ('Dxil', 'Spirv', 'Air')


def parse(path):
    text = open(path).read()
    entries = [list(m.groups()) for m in ENTRY.finditer(text)]
    count = int(re.search(r'g_shaderCacheEntryCount = (\d+);', text).group(1))
    assert len(entries) == count, (path, len(entries), count)
    blobs = {}
    for k in KINDS:
        m = re.search(rf'const uint8_t g_compressed{k}Cache\[\] = \{{([^}}]*)\}};', text)
        if not m:
            continue
        raw = bytes(int(x) for x in m.group(1).split(',') if x.strip())
        dsize = int(re.search(rf'g_{k.lower()}CacheDecompressedSize = (\d+);', text).group(1))
        data = zstandard.ZstdDecompressor().decompress(raw, max_output_size=dsize) if dsize else b''
        assert len(data) == dsize, (path, k, len(data), dsize)
        blobs[k] = bytearray(data)
    return text, entries, blobs


def main():
    base_path, extra_path, out_path = sys.argv[1:4]
    text, base, blobs = parse(base_path)
    _, extra, extra_blobs = parse(extra_path)
    have = {int(e[0], 16) for e in base}
    added = 0
    for e in extra:
        if int(e[0], 16) in have:
            continue
        # fields: hash, dxilOff, dxilSize, spirvOff, spirvSize, lateOff, lateSize, airOff, airSize, mask, name, texmask
        e = list(e)
        for k, off_i, size_i in (('Dxil', 1, 2), ('Spirv', 3, 4), ('Spirv', 5, 6), ('Air', 7, 8)):
            off, size = int(e[off_i]), int(e[size_i])
            if size and k not in blobs:
                e[off_i], e[size_i] = '0', '0'   # base build carries no blob of this kind
            elif size:
                e[off_i] = str(len(blobs[k]))
                blobs[k] += extra_blobs[k][off:off + size]
            else:
                e[off_i] = '0'
        base.append(e)
        added += 1
    base.sort(key=lambda e: int(e[0], 16))
    out = ['#include "shader_cache.h"', 'ShaderCacheEntry g_shaderCacheEntries[] = {']
    for e in base:
        out.append(f'\t{{ {e[0]}, {", ".join(e[1:10])}, "{e[10]}", nullptr, {e[11]} }},')
    out.append('};')
    for k in KINDS:
        if k not in blobs:
            continue
        comp = zstandard.ZstdCompressor(level=19).compress(bytes(blobs[k])) if blobs[k] else b''
        out.append(f'const uint8_t g_compressed{k}Cache[] = {{{",".join(map(str, comp))}}};')
        out.append(f'const size_t g_{k.lower()}CacheCompressedSize = {len(comp)};')
        out.append(f'const size_t g_{k.lower()}CacheDecompressedSize = {len(blobs[k])};')
    out.append(f'const size_t g_shaderCacheEntryCount = {len(base)};')
    open(out_path, 'w').write('\n'.join(out) + '\n')
    print(f'merged: {added} added, {len(base)} total entries')


if __name__ == '__main__':
    main()
