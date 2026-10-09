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
- [ ] Disc mirror (running; ~1 MB/s over the 360's network)
- [x] `rexglue` codegen tool built (`out/build/codegen`; needs `-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0` and libc++
      on `CMAKE_SHARED_LINKER_FLAGS`/`CMAKE_MODULE_LINKER_FLAGS` as well as the documented exe flags)
- [x] **Phase 1** — PAL codegen succeeds (`gta4_pal_manifest.toml`; 5 cross-function branch targets added).
      US 38,060 functions / 2,398,670 insns vs PAL 38,062 / 2,398,230 — same program, small edits shift addresses.
- [x] **Phase 2** — `tools/xex/match_functions.py` (in-order alignment + unique-hash + call-graph propagation):
      37,318/38,060 functions mapped (37,156 exact), 37,255 data addresses, 151,885 return addresses.
      `tools/xex/hook_coverage.py`: of 2,280 guest addresses the hooks/config reference, **2,178 (95.5%) resolve**;
      102 unmapped (mostly .data/.bss globals not reached via `lis` pairs) — next: infer from neighbouring mapped data.
- [x] **Phase 3** — sources + config remapped.
  - Pipeline: `gta4_pal_raw_manifest.toml` (analysis-only reference) → `match_functions.py` →
    `remap_sources.py` (+ `manual_map.json`) → `build_pal_config.py` → `rexglue codegen gta4_pal_manifest.toml`.
  - App CMake option `LIBERTY_RECOMP_PAL` (default ON) builds `generated_pal/`.
  - Hook sources: 4,140 references rewritten; **all 562 hook overrides and 635 `__imp__` calls exist in PAL code**.
  - Hand-copied guest code (`*_guest.inc`) copies functions that are identical in PAL — remapped addresses are enough.
- [x] **App builds and links against PAL** (`macos-release`, bundle verification passes).
- [x] **Phase 4** — installer/inspector accept PAL: media `0x7CF4679F`, region `XEX_REGION_PAL`, base version 7,
      base XXH3 `15674128280634689956`, RSA-signature SHA-1 = TU5 `digest_source` (`24bdc3d4…`),
      TU5 target `0x507`, TU5 XEXP SHA-256 `602f1c58…`. The US pre-patched-v8 shortcut is inert.
- [x] Disc mirrored (169 files / 6.47 GiB, sizes verified), installed through the PAL installer.
- [x] **First boots (2026-10-09)**
  1. abort at unregistered vtable target `0x8219ACC0` (bad manual guess) → fixed, plus 13 undiscovered entry points
     found by `find_unregistered_targets.py`;
  2. abort at `0x8219FED8` → registered;
  3. legal screen forever: streaming hook copy spun on access violations — copied guest code still had US
     `lis`/lo immediates → `fix_guest_immediates.py` (388 lines);
  4. loads + renders the world on `gta4-native` until **MoltenVK device lost**;
  5. **`--gpu_plugin=gta4-metal`: plays the opening cutscene.** 24 PAL shaders missing from the Metal archive
     (121 draws skipped so far).
- [x] **PAL executable-embedded shaders** — the 24 misses are the "runtime_captured" shaders (Bink video etc.)
      that live inside default.xex, not in any .fxc; all 1,332 PAL .fxc shaders were already in the US cache.
      Extracted the 24 containers from the patched PAL image, compiled with XenosRecomp, merged with
      `tools/xex/merge_shader_cache.py` (existing 1,356 entries + blobs byte-identical; 1,380 total).
      Verified: 0 archive misses, 0 rejected draws.
  - Follow-up: `shader_overrides/manifest.json` keys the Bink overrides by US hashes (`9E76B68B60127349`,
    `A6C9E2B8B2A59D7A`, `156BAD4A9EE62726`); PAL Bink shaders currently use the stock translation.

### Free-roam abort 2026-10-10: address-taken vcall thunks (fixed)
Driving in Hove Beach aborted with `Call to invalid or unregistered function` (SIGABRT on the main
guest thread; `sub_82882A90` → `sub_828A6638` → `sub_8288FE70` → bctrl). The callbacks those callers
pass by `lis/addi` are four-instruction virtual-call thunks laid out back to back
(`lwz r12,0(r3); lwz r11,N(r12); mtctr r11; bctr`), and codegen had only registered the first thunk
of each run. Registered in `gta4_pal_config.toml`: `0x82882A70`, `0x82882A80`, `0x82893C98`,
`0x82893CB8`.
- Found with **`tools/xex/find_address_taken_targets.py <image> generated_pal`** — the data scanner
  (`find_unregistered_targets.py`) cannot see these because the addresses live in code immediates.
- Only thunk/prologue-shaped targets qualify. `0x8290001C` looked address-taken but is a branch in a
  loop body; registering it split the function (codegen: unresolved conditional branch to
  `0x828FFFD0`). Re-run the scan after any config change; it should print 0.
- `InvalidFunctionTrap` now also prints the target and `lr` to stderr, so a crash outside a
  `--diagnostics` run still leaves the address in the terminal.

### Known gaps (left as US values, see `tools/xex/manual_map.json` "unresolved")
| US address | Used by | Impact |
|---|---|---|
| `0x82B307A0`, `0x82B307A4` | native renderer cloud double-buffer / postfx timecycle index, bulb trace | clouds/timecycle selection in `gta4-native`/`gta4-metal` may misbehave |
| `0x82013C9C` | touch radar vtable | touch controls only — irrelevant on Mac |
| `0x82055F8C`, `0x82055F7C` | MP proximity weight thresholds | multiplayer tuning only |

### Review list — hooks on functions PAL's TU5 changed (similarity < 1.0)
Pairing verified by similarity; the hook logic may still depend on changed internals.
`82141F00`, `82145968`, `8214B640` (transition hooks), `8215CB10`, `821C1C78`, `82205C30`, `822343C0`,
`8223F9F0`, `82253370`, `82257450`, `8229D8A8` (+ interior `8229E044`/`8229E200`/`8229E7C0`, touch HUD caller),
`823A8F70`, `826DD580`, `82A3BE18` (0.886 — lowest). (US addresses; PAL targets via the resolver.)

### Lessons
- Inline jump tables mean instruction index ≠ address; track `loc_` labels.
- Fuzzy function pairs can be confidently wrong (US `823710B8`: fuzzy said −0x4F78, truth +0xD870);
  the local code-shift between exact anchors is the better arbiter.
- Neighbour inference for data is unsafe (data is reordered); two such guesses pointed at unrelated floats.
  Verify data guesses against the PAL image before trusting them.
