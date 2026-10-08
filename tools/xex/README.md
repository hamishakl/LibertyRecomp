# XEX tools (fork-local)

Used to check whether a non-US GTA IV disc matches the build LibertyRecomp was generated from.
Python tools need `pip install cryptography` (use a venv).

| Tool | What it does |
|---|---|
| `extract_basefile.py default.xex out.bin` | Decrypt + decompress a retail XEX (basic compression) to its memory image |
| `lzxdelta.c` | Standalone port of the SDK's `lzxdelta_apply_patch` (`SDK/src/system/lzx.cpp`). Build: `cc -O2 -w -I$M lzxdelta.c $M/lzxd.c $M/system.c -o lzxdelta` with `M=glue/rexglue-sdk-main/thirdparty/libmspack/libmspack/mspack` (setup_repo.py's patched copy) |
| `apply_tu.py base.xex basefile.bin tu.stfs ./lzxdelta out.bin` | Pull `default.xexp` out of an STFS title-update package and apply it (mirrors `XexModule::ApplyPatch`; every block SHA-1 verified) |
| `compare_callsites.py image.bin glue/rexglue-sdk-main/gta4-recomp/generated` | Check every recompiled `bl` site against an image: same-address rate + target drift |

## Result 2026-10-08 — PAL (NZ) disc
- PAL disc: media `0x7CF4679F`, version `0x7`. PAL TU5 (XboxUnity id 21180): `0x7 -> 0x507`, image `0x1300000`.
- Project target: US 1.00 + TU, version `0x805`, image `0x1300000`.
- Patched PAL image vs generated US code: **7.4%** of `bl` sites have a `bl` at the same address (≈ chance);
  targets drift by -0x548, -0x400, +0x10… → **different compile**. Every hook/address would need remapping.

## Result 2026-10-08 — Games-on-Demand copy on the 360 HDD (`Hdd1/GAMES/545407F2`)
- media `0x4A53F9F6`, version `0x6` (no TU installed). Same file size as the PAL disc XEX but 55% of image words differ.
- vs generated US code: 5.6% `bl` sites line up (≈ chance) → also a **different compile**.
- Version pattern so far: TU target = `(TU# << 8) | base`: US `0x805` = base 5 + TU8, PAL disc `0x507` = base 7 + TU5.
  So the project needs a **base-version-5 (US 1.00) disc**; base 6 and 7 builds won't match.
