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
| ~~`0x82B307A0`, `0x82B307A4`~~ | cloud double-buffer producer / selected (timecycle) index | **resolved 2026-10-10: `0x82B30620`, `0x82B30624`.** Words +4032/+4036 of a struct whose base moved 0x82B2F7E0 → 0x82B2F660; confirmed by four accessor functions paired US↔PAL by the function map. The buffer at `0x82D4A280` did not move |
| ~~`0x82013C9C`~~ | radar render-phase vtable | **resolved 2026-10-10: `0x820131E4`** (constructor with phase id 7, PAL `sub_8236C140`; slot 4 = `sub_8236C750`). Was NOT touch-only: it gates the radar's aspect fit for every input, and left the minimap a vertical oval on the 1.54:1 panel |
| `0x82055F8C`, `0x82055F7C` | MP proximity weight thresholds | multiplayer tuning only |

### Hooks on functions PAL's TU5 changed — REVIEWED 2026-10-10, nothing to fix
`tools/xex/review_changed_hooks.py <gta4-recomp dir>` finds every hooked PAL function whose US pair
is not an exact match (13 today), diffs the instruction streams and classifies each hunk; it also
lists which hook `constexpr` globals the PAL body references and checks `ctx.lr` key literals that
fall inside a body. Re-run it after any config or hook change. Result: **12 pairs differ only in
relocated globals** (same code, different `lis`/lo pair) and **1 has a real code change** that the
hook does not depend on. Every guest constant a reviewed hook reads was confirmed against the
global the PAL body itself touches, and whole-tree reference counts match US↔PAL for the rest
(player-info generation table, secondary player id, loading flags, command arena).

| US → PAL | Hook | Finding |
|---|---|---|
| `82141F00` → `82141F50` | world activation (transition) | **TU5 dropped a 9-instruction probe** (`sub_821B5600` on a string literal, conditional `sub_821B53D0`) before `sub_821CC6B8`; PAL passes the literal straight through. Hook reads r3/r4/lr and the result only |
| `82145968` → `82145998` | loading-screen parser (presentation) | globals only; `kScreenCount` `831D51C4`, `kDefinitions` `831D5318` referenced by the body; `kParserCaller` `82145588` is the return of `bl 0x82145998` in `sub_82145450` |
| `8214B640` → `8214AB18` | state dispatch (transition) | globals only; `kFrontendStoredStateGlobal` `82C30C0C` referenced |
| `8215CB10`, `821C1C78`, `82205C30`, `823A8F70`, `826DD580` | primary-player-info alias | globals only; every body reads `82A938A8` (primary id) and `82B61DF0` (pointer table), which is exactly what the alias swaps |
| `822343C0` → `822720B0` | explosion observe (Sony) | two episode-global reads moved; r7 → `mr r14,r7` then `lvx128`, unchanged. Ghidra truncates this body at 20 bytes (VMX), use the generated code |
| `8223F9F0` → `821CFCB8` | storage dialog failure reasons | one global; the switch still carries cases 0x11–0x16 and 0x20 |
| `82253370` → `82265730`, `82257450` → `82269810` | adjust dispatch / cancel (frontend) | globals only; `kCurrentScreenAddress` `82C30BF4`, `kScreenDescriptorsAddress` `831D6A20`, `kFrontendWidgetIndex` `82C30BDC`, `kPauseMenuActiveAddress` `82C309C4` referenced |
| `8229D8A8` → `822B09D8` | frontend draw / touch HUD caller | 20 hunks, all the four video width/height globals (`82B0B310…1C`); widget tables `82CD056C`/`82CD0578` referenced. The three `ctx.lr` keys `822B1174`/`822B1330`/`822B18F0` sit at the same instruction index as US `8229E044`/`8229E200`/`8229E7C0`, each after `bl sub_82226068` (US `sub_821F6E38`) |
| `82A3BE18` → `82A3BA08` | guest call from the native resolve-batch hook (`sub_82A3E998`) | 0.886 only because the US tree swallowed one instruction of the `D3DDevice_SetViewportF` stub after the `blr`; the 31-instruction body is identical. Argument `0x820B0274` (US `0x820B0314`) is the same literal the PAL caller builds (`lis r11,-32245; addi r4,r11,628`) and holds a full viewport `{0,0,0xFFFF,0xFFFF,0.0f,1.0f}` |
| `82A4A600` → `82A4A1F0` | resource unlock (native) | not in the map at all (ratio 0.687): the US tree merged the `D3DResource_AddRef`/`Release` bodies (config names them one instruction late, `82A4A710`/`82A4A788`) into the unlock function; PAL names them at `82A4A300`/`82A4A378`, so the PAL body is the first 67 instructions, identical. Resolved by local shift, correct |

### Lessons
- Inline jump tables mean instruction index ≠ address; track `loc_` labels.
- Fuzzy function pairs can be confidently wrong (US `823710B8`: fuzzy said −0x4F78, truth +0xD870);
  the local code-shift between exact anchors is the better arbiter.
- Neighbour inference for data is unsafe (data is reordered); two such guesses pointed at unrelated floats.
  Verify data guesses against the PAL image before trusting them.
