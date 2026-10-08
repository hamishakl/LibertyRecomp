# PAL port plan

Goal: run Liberty Recompiled from the **PAL (NZ/AU/EU) disc + Title Update 5** instead of the US build
upstream targets. Started 2026-10-08. Background and measurements: `tools/xex/README.md`.

| | Upstream target | Our target |
|---|---|---|
| Disc media ID | `0x6AC07221` (US) | `0x7CF4679F` (PAL) |
| Base XEX version | 5 | 7 |
| Title update | TU8 → `0x805` | TU5 → `0x507` (XboxUnity id 21180) |
| Patched image size | `0x1300000` | `0x1300000` |

Patched PAL is a **different compile**: only ~7% of the US build's call sites line up, function targets drift by
small varying amounts. Every guest address the project hard-codes has to be remapped.

## What carries addresses

| Where | Kind | Count (approx.) |
|---|---|---|
| `glue/rexglue-sdk-main/gta4-recomp/src/*` hooks | `sub_XXXXXXXX` names, `ctx.lr` return addresses, global data addresses | ~577 overrides, ~1,250 `REX_LOAD/STORE` |
| `gta4_native_hooks.cpp` (renderer) | same | ~102 overrides |
| `gta4-recomp/gta4_config.toml` | `[functions]` boundaries, `[rexcrt]` CRT addresses | 142 + ~10 |
| Installer (`src/install/`) | media ID, base XXH3, TU SHA-256, target version | 4 constants |

## Phases

0. **Inputs** — PAL disc mirrored from the 360 (`~/Games/GTAIV-360/pal_disc/`), TU5 `default.xexp`
   (extracted by `tools/xex/apply_tu.py`). Build the `rexglue` codegen tool (`out/build/codegen`).
1. **Raw PAL codegen** — new manifest pointing at the PAL `default.xex` with `default.xexp` beside it
   (codegen auto-applies the `<file>p` sibling). Config with the US-only `[functions]`/`[rexcrt]` removed.
   Output to a separate directory so the US tree stays as the matching reference.
2. **US→PAL address map** (`tools/xex/match_functions.py`) — both generated trees carry the same
   disassembler's per-instruction comments, so:
   - fingerprint each function by its normalised instruction stream (addresses/branch targets stripped);
   - unique fingerprints = anchors; propagate through the call graph (the k-th `bl` of a matched pair maps its callees);
   - map data addresses from `lis`/`addi`/`lwz` operand pairs at the same position in matched functions;
   - map `ctx.lr` constants as function + offset when bodies are identical, flag the rest for hand review.
   Output a JSON map with a confidence per entry.
3. **Remap** — rewrite `[functions]`/`[rexcrt]`, regenerate PAL code; rewrite hook addresses with the map
   (scripted, reviewed diff). Fork becomes PAL-only.
4. **Installer** — accept PAL media ID, PAL base hash, TU5 hash, target `0x507`.
5. **Boot + debug** — `xenos` renderer first if the native hooks are shaky, then `gta4-native` / `gta4-metal`.

## Status
- [x] PAL XEX + TU5 obtained and verified (TU blocks SHA-1 checked)
- [ ] Disc mirror
- [ ] `rexglue` codegen tool built
- [ ] Phase 1 …
