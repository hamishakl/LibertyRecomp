#!/usr/bin/env python3
"""Compile one Metal source into a metallib and embed it in a C header.

Recreated locally: the upstream script is excluded by the repo's `*.py`
.gitignore rule, so it never reached the remote. Consumers use the symbol with
sizeof(), so it is emitted as a fixed-size byte array.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(args: list[str]) -> str:
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=300)
    if result.returncode:
        raise SystemExit(f"Tool failed ({result.returncode}): {' '.join(args)}\n{result.stdout}")
    return result.stdout


def write_changed(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == data:
        return
    path.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--symbol', required=True)
    parser.add_argument('--sdk', default='macosx')
    parser.add_argument('--deployment-target', default='26.0')
    parser.add_argument('--strict-math', action='store_true')
    args = parser.parse_args()

    sdk_path = run(['xcrun', '--sdk', args.sdk, '--show-sdk-path']).strip()
    metal = run(['xcrun', '--sdk', args.sdk, '--find', 'metal']).strip()
    linker = run(['xcrun', '--sdk', args.sdk, '--find', 'metallib']).strip()
    min_flag = ('-mmacosx-version-min=' if args.sdk == 'macosx'
                else '-mios-version-min=') + args.deployment_target

    with tempfile.TemporaryDirectory() as temporary:
        air = Path(temporary)/'library.air'
        library = Path(temporary)/'library.metallib'
        command = [metal, '-c', str(args.source), '-o', str(air), '-isysroot', sdk_path,
                   '-std=metal3.0', '-I', str(args.source.parent), min_flag]
        if args.strict_math:
            command.append('-fno-fast-math')
        run(command)
        run([linker, str(air), '-o', str(library)])
        data = library.read_bytes()
    if not data.startswith(b'MTLB'):
        raise SystemExit('Metal linker output is not a library')

    lines = [f'// Generated from {args.source.name} by compile_metal_library.py. Do not edit.',
             '#pragma once', '',
             f'alignas(16) static const unsigned char {args.symbol}[] = {{']
    for offset in range(0, len(data), 16):
        lines.append('  ' + ', '.join(f'0x{byte:02x}' for byte in data[offset:offset+16]) + ',')
    lines += ['};', '']
    write_changed(args.output, '\n'.join(lines).encode())


if __name__ == '__main__':
    main()
