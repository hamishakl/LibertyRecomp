# Dev notes — macOS (Apple Silicon) fork

Working notes for `hamishakl/LibertyRecomp`. Mapped 2026-10-08 against upstream `1922957d`.
Paths are repo-relative; `SDK` = `glue/rexglue-sdk-main`, `APP` = `SDK/gta4-recomp`.

> **Read this first: the macOS app is NOT `LibertyRecomp/`.**
> `CMakeLists.txt:180-188` — on macOS desktop the top-level build does `add_subdirectory("glue")`
> and `return()`s. The real Mac app is **`APP`** (`APP/CMakeLists.txt:93`). `LibertyRecomp/` and
> most of `LibertyRecompLib/` are the older Windows/Linux/console app — reference only. Much of
> `docs/BUILDING.md` §7–9 and `docs/SHADER_PIPELINE.md` describe that old tree and are stale.

---

## 1. Layout — who owns what

| Path | Role on macOS |
|---|---|
| `SDK/` (ReXGlue SDK) | Runtime, guest memory, kernel/XAM HLE, VFS, GPU plugins, audio, UI/app framework, codegen tool |
| `APP/` | **The Mac game app**: `GTA4App`, installer, all game hooks (`src/gta4_*`), generated code |
| `APP/generated/` | Checked-in recompiled PPC → C++ (84 × `gta4_recomp.N.cpp`, ~170 MB), `gta4_init.{h,cpp}`, `gta4_register.cpp` |
| `LibertyRecompLib/` | Only assets used on Mac: `shader/shader_cache.cpp`, `shader_overrides/`, `aes_key.bin`, `font_atlases/`, `private/button_prompts` |
| `LibertyRecomp/` | Legacy non-Mac app (`main.cpp`, `patches/`, `gpu/video.cpp`, `mod/`). Not compiled on Mac |
| `tools/metal/` | Offline Metal shader tooling (archives, SPIR-V→MSL translator) run at build time |

## 2. Boot flow (macOS)

1. `APP/src/main.cpp:3` `REX_DEFINE_APP(gta4, GTA4App::Create)` → `SDK/src/ui/windowed_app_main_sdl.cpp:150` `main()`
   → cvars, env, diagnostics, logging, SDL → `app->OnInitialize()` → message loop.
2. `SDK/src/ui/rex_app.cpp:90` `ReXApp::OnInitialize`:
   - `SetupEnvironment()` — paths (`GTA4App::OnConfigurePaths`, `APP/src/gta4_app.h:59-92`), TOML config, logging.
   - `SetupPresentation()` — `GTA4App::OnPreSetup` (`gta4_app.cpp:646`) defaults `gpu_plugin` → loads plugin dylib → 1280×720 window.
   - `GTA4App::OnFinalizePaths` (`gta4_app.cpp:589`) — **install check**. Not ready → opens the ImGui `InstallDialog`; boot resumes from its callback.
3. `ReXApp::ConstructRuntime` (`rex_app.cpp:237`) — `Runtime::Setup` (memory, exports, dispatcher, VFS, kernel, input, audio, GPU;
   `SDK/src/system/runtime.cpp:98-210`) → `LoadXexImage("game:\\default.xex")`.
4. `ReXApp::LaunchModule` (`rex_app.cpp:490`) — creates the suspended "Main XThread" at the XEX entry
   (`SDK/src/system/kernel_state.cpp:579`), initialises shader storage, resumes it. App quits when that thread exits.

## 3. How recompiled code runs

- Every guest function: `void sub_XXXXXXXX(PPCContext& ctx, uint8_t* base)`. `ctx` = registers
  (`SDK/include/rex/ppc/context.h:251`), `base` = host pointer to guest address 0.
- Image: `IMAGE_BASE 0x82000000`, `CODE_BASE 0x82140000` (`APP/generated/gta4_init.h:12-16`).
- Guest memory (`SDK/src/system/xmemory.cpp:134`): 4 GB guest virtual space reserved contiguously;
  host = `base + guest_addr` (+0x1000 for ≥ 0xE0000000). **Guest data is big-endian** — use `REX_LOAD_U32` /
  `REX_STORE_*` (`gta4_init.h:107-125`), never raw host reads.
- Indirect calls: function table after the image; `REX_CALL_INDIRECT_FUNC` (`gta4_init.h:248`).
- Threads: each guest `XThread` → its own host thread, 16 MiB stack (`SDK/src/system/xthread.cpp:433-452`).
- Kernel imports: `SDK/src/kernel/{xboxkrnl,xam,xbdm}/*.cpp`, bound by name via `REX_EXPORT`. Stubs log
  `"<name> STUB"` (`SDK/include/rex/hook.h:55-85`).

## 4. Writing a hook

- Generated functions are **weak** (`DEFINE_REX_FUNC`, `APP/generated/gta4_init.h:56-66`). Define a strong
  `extern "C" void sub_X(PPCContext& ctx, uint8_t* base)` and it wins; call `__imp__sub_X(ctx, base)` for the original.
  No registration table, **no codegen re-run needed**.
- Pattern (from `LibertyRecomp/patches/gta4_fps_ladder_fix.cpp`, same idea in `APP/src`):
  ```cpp
  extern "C" void __imp__sub_823FE5F0(PPCContext&, uint8_t*);
  extern "C" void sub_823FE5F0(PPCContext& ctx, uint8_t* base) {
    if (ctx.lr == 0x826A1B68)          // only for this call site
      ctx.f3.f64 = kHeadingAngleThreshold;
    __imp__sub_823FE5F0(ctx, base);
  }
  ```
- New Mac hooks go in `APP/src/` and the source list at `APP/CMakeLists.txt:93`. Keep pure logic in a `*_policy.h`
  so it can be unit-tested. ~577 overrides already exist there — **grep for the `sub_` before adding one**, a
  duplicate strong symbol is a link error.
- Codegen only needed for function-boundary changes (`APP/gta4_config.toml` `[functions]`, `[rexcrt]`) or
  translation bugs. Needs `APP/assets/default_v8.xex` (not in repo). `LibertyRecompLib/config/Liberty.toml` is a
  dead Sonic '06 leftover.

## 5. Graphics

**Three GPU plugins** (`APP/CMakeLists.txt:212`), picked by cvar `gpu_plugin` (restart required) or the in-game Renderer menu:

| Plugin | What | Mac |
|---|---|---|
| `gta4-native` | Native title renderer on **Vulkan** (`SDK/src/graphics/gta4_native/`, `graphics_system.cpp` ~34k lines) | **Default** — runs via bundled MoltenVK |
| `gta4-metal` | Same title-command ABI, **native Metal** (`SDK/src/graphics/gta4_metal/`, ~9.6k lines) | Opt-in: `--gpu_plugin=gta4-metal` |
| `xenos` | Xenia-style PM4/Xenos emulation | "Emulated" fallback |

- "Native" = ~102 hooks in `APP/src/gta4_native_hooks.cpp` replace the game's D3D-style calls and emit typed
  title commands (`SDK/include/rex/graphics/gta4_native/title_commands.h`) instead of PM4. Shader bodies, render
  state semantics and formats stay Xenos-faithful; passes, AA, upscaling, HDR, presentation are host-owned.
- **No runtime shader compilation.** Build time: stock shaders from `LibertyRecompLib/shader/shader_cache.cpp` →
  `tools/metal/shader_archive.py` → `title_shader_archive.bin`; 320 hand overrides in
  `LibertyRecompLib/shader_overrides/` (HLSL→DXC→SPIR-V→`liberty-metal-shader-translator`→metallib). Runtime
  mmaps archives and looks up by hash (`gta4_metal/renderer.mm:49-106,384`).
- Early vs late fragment-test variants: late used when alpha test / alpha-to-mask active (`gta4_metal/pipelines.mm:95`).
- Metal temporal stack: native TAA, MetalFX temporal scaler + frame interpolation (`gta4_metal/temporal/effects.mm`).
- Presentation: `SDK/src/ui/metal/presenter.mm` (CAMetalLayer, CAMetalDisplayLink vsync, EDR/HDR), 2-slot `frame_ring`.
- **Metal work happens in**: `gta4_metal/{draw,pipelines,resolve,attachment_passes,resource_textures}.mm`,
  `temporal/`, `ui/metal/presenter.mm`. **Use `gta4_native/graphics_system.cpp` as the reference** when they disagree.
- Metal drops (doesn't fall back on) unsupported cases and logs `gta4-metal: command rejected type=… reason=…`:
  clip planes, some depth bias/topology, packed depth/stencil CPU uploads, missing shader hash. Those log lines
  are the bug list.
- Vulkan-only: FSR3, DLSS, Vulkan frame gen. Metal-only: TAA, MetalFX.

## 6. Running & debugging

- Run from a terminal to see output:
  `"out/build/macos-relwithdebinfo/LibertyRecomp/Liberty Recompiled.app/Contents/MacOS/Liberty Recompiled" --diagnostics --fullscreen=false`
- **Logging is OFF unless `--diagnostics`** (`SDK/src/core/logging.cpp:148`). Narrow with
  `--diagnostics-categories=logging,presenter,…`; more detail with `--log_level=trace`.
- Logs: `~/Library/Application Support/Liberty Recompiled/logs/` (note: different folder from the install dir).
- cvars: CLI `--name=value` / `--no-name`, env `REX_<NAME>`, or `~/Library/Application Support/LibertyRecomp/native.toml`.
  CLI wins. `fullscreen` defaults **true**.
- In-game keys: F4 settings, F3 debug overlay, backtick console, F7 achievements.
- Metal GPU capture: `MTL_CAPTURE_ENABLED=1` + `--gta4_metal_capture_path=…` → Xcode `.gputrace` (`gta4_metal/renderer.mm:14-29,138`).
- Debugger: `lldb -- "<app>/Contents/MacOS/Liberty Recompiled" --diagnostics --fullscreen=false`.
- Stuck guest thread: backtick console, "No function registered at …" errors, `ctx.last_indirect_target`.

## 7. Game files / installer — what the dump must contain

Installer: `APP/src/install/` (folder, `.iso`, or STFS/SVOD package input). Installs to
`~/Library/Application Support/LibertyRecomp/{game,dlc,saves,shader_cache}`.

- Title ID `0x545407F2`, **USA** media ID `0x6AC07221` (`gta4_source_inspector.cpp:38-39`).
- Base `default.xex` must hash-match **USA retail 1.00** (`gta4_installer.cpp:48`).
- **Title update required**: exactly one `default.xexp` (TU, target version `0x805`), SHA-256 checked
  (`gta4_installer.cpp:46-51,659,874`). → When dumping, also grab the TU from the 360's
  `Content/0000000000000000/545407F2/000B0000/`. A disc-only dump will be rejected.
- Episodes (TLAD/TBOGT) optional. Needs `aes_key.bin` (tracked in `LibertyRecompLib/`).

## 8. Tests & tools

- Python: `python3 -m unittest discover -s tools -p 'test_*.py'` — no game files needed.
- C++ (Catch2): configure with `-DREXGLUE_BUILD_TESTS=ON`, run `ctest`. Off by default.
- `tools/mcp-gta4` (decomp MCP) — needs IDA exports in `gta_iv/` (absent; need the XEX). No `package.json`.
- `tools/ppc-mcp-server` — stale: expects `LibertyRecompLib/ppc/ppc_recomp.*`; real code is `APP/generated/gta4_recomp.*`.
- `tools/*.lldb` — bug-specific scripts, need a running game.
- `tools/analyze_native_*.py` — offline analysers for profiler/xctrace captures.

## 9. Fork changes (branch `macos-build-fixes`)

1. `SDK/tools/compile_metal_library.py` recreated — upstream's `*.py` gitignore rule meant it was never committed.
2. Bink `ps_bink*.hlsl`: NaN alpha-test rewritten as `any(isnan(float2(...)))` — bundled DXC segfaults in SPIRV-Tools ADCE on the chained form.
3. `gta4_shader_override_compiler.py`: no `-Os` for glslang — glslang 16 strips unused push-constant members.
4. `fpscr.h`: FPCR write widened to 64-bit (was UB, 139 warnings).

Undocumented build deps: `brew install cmake ninja llvm spirv-cross glslang` + `xcodebuild -downloadComponent MetalToolchain`.

## 10. Ideas / open questions

- Try `gta4-metal` vs default `gta4-native` (MoltenVK) on first boot — perf + correctness comparison is a natural first task.
- Without game files: fix stale docs, revive `ppc-mcp-server` against `APP/generated`, add a duplicate-hook checker,
  Catch2 tests for `*_policy.h`, delete Sonic '06 leftovers.
- `docs/MOD_SUPPORT.md`'s FusionFix overlay priority table isn't implemented — only `update:` is mounted.
- `sub_821200D0` "slow world init" note in BUILDING.md is from the legacy build (below `CODE_BASE` on Mac).
- Unverified: macOS VFS never mounts `common:`/`platform:`/`audio:` (legacy did) — check on first boot if file opens fail.
