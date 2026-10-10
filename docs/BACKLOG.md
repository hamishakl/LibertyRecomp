# Backlog — PAL / macOS fork

Started 2026-10-10 after the stutter, UI and input sessions. Status: `todo`, `doing`, `done`,
`needs play test` (built, waiting on a gameplay check), `deferred` (reason given).

## Performance
| # | Item | Status | Notes |
|---|---|---|---|
| 1 | Cap the automatic render size at the panel's native pixel size instead of the 2x backing store | done | `gta4_native_panel_resolution_cap` (default on); verified: automatic 3840x2486 → 2880x1864. An explicit preset still wins |
| 2 | Draw-distance / population defaults for Apple Silicon (A/B 1x, 2x, 3x on the same route) | done | same route: default 34 fps (p99 77 ms, submit 23 ms), apple-silicon 39 fps (p99 72), parity 50 fps (p99 33). Parity also spawned more traffic. Parity is now the default; upstream values in `presets/upstream-defaults.toml` |
| 3 | Make the GPU pass timer additive per category | done | `exclusive_ms_per_frame` column shares overlapped intervals between active passes; verified: exclusive sum 7.3 ms vs 6.4 ms measured GPU (inclusive sum said 15.6) |

## Stability and diagnosability
| # | Item | Status | Notes |
|---|---|---|---|
| 4 | Always-on warnings/errors log in the user directory | done | `log_quiet_errors` (default on): `~/Library/Application Support/LibertyRecomp/logs/Liberty Recompiled-errors.log`, warnings and above, path printed to stderr at start |
| 5 | "Resolve source has no produced content" rejections (64 per run) | deferred | reviewed: all 64 land in a 3 s burst at world load, one 480x310 MSAA source (exposure/luminance chain, phase 6) resolved before its first production; none during play. Cosmetic log noise at most |
| 6 | Remaining PAL gaps: two US data addresses (cloud/timecycle), hooks on TU5-changed functions, data-table scan | done | all three data addresses resolved 2026-10-10 (radar vtable `0x820131E4`, cloud/timecycle `0x82B30620`/`0x82B30624`) and confirmed in play. Data-table scan reviewed (CRT unwind pointers). TU5-changed hook review done 2026-10-10 with Ghidra + `tools/xex/review_changed_hooks.py`: 13 hooked pairs, nothing to fix (12 globals-only, 1 code change the hook does not read); table in docs/PAL-PORT.md |
| 7 | Game Center "title-profile fetch failed; retrying" every 5 s all session | done | exponential back-off 5 s → 5 min, later failures at info level |

## Input feel
| # | Item | Status | Notes |
|---|---|---|---|
| 8 | Trackpad sensitivity curve / default | done | 2.0 chosen by feel (was 1.0); default changed and a **Trackpad Sensitivity** row (0.5x–4x) added to the pause-menu settings. Delayed swing-back after 2 s hold judged fine |
| 9 | Trackpad gestures: two-finger scroll for weapon cycle / radar zoom | done | already mapped: two-finger scroll arrives as the wheel, which cycles weapons on foot and the radio while driving (gta4_input_hooks.cpp `wheel_route`). Nothing to add |
| 10 | Mouse-look hold for the two cameras still on retail idle logic | needs play test | which mode drifts decides which function |

## Polish
| # | Item | Status | Notes |
|---|---|---|---|
| 11 | Bring the game window to the front on launch | done | macOS ignores `activateIgnoringOtherApps` from a terminal-launched process; the launch scripts activate via LaunchServices (`osascript ... activate`) instead |
| 12 | Verify console / settings / achievements overlays under the GTA IV theme | done | achievements: fine. Settings: setting names were green/yellow/red by lifecycle, now white/amber/grey, and the window opens at 960x640 instead of 620x480. Console: level colours only (white/yellow/red), theme-driven otherwise |
| 13 | Stale docs: DEV-NOTES installer section (USA/TU8), dumping guide (PAL/TU5) | done | |
| 14 | Minimap drawn as a vertical oval ("game feels squashed") | done | `kRadarRenderPhaseVtable` still held the US address, so the radar viewport was never fitted to the 1.54:1 display. Resolved to PAL `0x820131E4`; confirmed round in play |

## From the 2026-10-10 render-thread profile (parity defaults, 1440p, M4)
Render thread per 20 ms frame at ~50 fps: ~6.4 ms waiting for frame admission (vsync pacing), ~6.9 ms in the
game's own recompiled draw-list code, ~6.5 ms in the Metal submit path. GPU exclusive ~13.4 ms. Both halves
are just under a 60 Hz tick, so jitter on either turns 17 ms frames into 33 ms ones (p50 17, p95 33).

| # | Item | Status | Notes |
|---|---|---|---|
| 15 | Run one frame ahead to absorb tick jitter | done | same route: doubled frames 24% → 8% of displayed frames, 48 → 55 fps on screen; latency not noticeable in play. Now the default (`present_frames_ahead=1`, Metal ring grows to 3 automatically); pause-menu row **Frame Pacing**: Smooth / Low Latency |
| 16 | Metal submit hot spots | doing | per-draw/per-texture by-name setting lookups now cached per frame: profile 2 shows setting lookups 5.2% → 0.1% and mutex waits 2.4% → 0.1% of submit work (~0.4 ms/frame). Remaining: ObjC retain/release ~13% (per-draw `id<>` locals; needs lifetime review before `__unsafe_unretained`), byte compares ~11% (dirty constant banks + rolling vertex/index validation), memmove into the upload arena ~5%, XXH3 ~3.5%. Metal CPU cost is ~3 µs per draw |
| 17 | Render-pass breaks from resolves | done as far as it goes (parts 1 + 2a); no GPU saving yet | resolve profiler run 2026-10-10 (`--diagnostics_categories=...,native-profiler --gta4_metal_profile_delay_seconds=75 --gta4_metal_profile_duration_seconds=3`, log lines `gta4-metal-profile-resolve`): **28 resolves per frame**, 17 direct blits + 11 conversion draws (fmt 115 and 55 chains); 6 per frame unread in one scene, 2 (`0xD9132A60`, `0xD9132D60`) in the three later runs ≈ 0.2 ms. **Part 1: deferred resolves** — `Resolve()` records a `PendingResolve` and does its bookkeeping, `ExecuteResolve` encodes it later; `SettlePendingResolves(image, writing)` runs at every GPU read/write site (attachment bind in `BeginRender`, texture bind, DoF half-scene input, present, packed depth alias, depth handoff, readback, partial clears, folded-clear materialisation, colour copies, temporal depth capture / composite recovery) with a dependency closure (earlier writer of my source/target, earlier reader of my target; sibling readers of one source are independent); a full resolve into a subresource with an unread pending record drops it. **Part 2a:** records outlive their submission; `Flush()` runs only records from an earlier submission (a waiting flush runs all). Cvar `gta4_metal_defer_resolves` (default on; off while the fire trace is active). Frame summary prints `resolve-deferred/executed/dropped`; profiler line `gta4-metal-profile-resolve-execute ... source-pending-clear= source-initialized= at=file:line` names the forcing flush point and how the source is about to be overwritten. **Result over four play runs (~13 min): 0 renderer errors, no visual change, 0 drops.** The unread resolves are forced at `draw.mm` BeginRender because their *source surface is redrawn next frame with `MTLLoadActionLoad`* (no pending clear, initialised) before the resolve into the same destination recurs. A snapshot swap (fresh image for the surface, record keeps the old one) only works when the redraw starts from a full clear, and every full-clear case in the profile is a resolve that *is* read. Skipping these would need to assume the guest overwrites every pixel, which is speculation with corrupt frames as the failure mode. Not pursued. The deferral stays in as tested plumbing; `gta4_metal_defer_resolves = false` restores the immediate path if anything ever looks stale. Design (b), render-target aliasing by prediction, remains the only route to the 17 blits and is a large change |
| 18 | CPU submit spikes to 30+ ms in some areas | done | sampled while parked in a slow spot: the render thread was idle 40% (33% waiting for admission, 7% guest sync) and the main thread idle too; the title command buffer averages 13 ms GPU and peaks past the tick, so dense spots are GPU-bound, not CPU. The "CPU spike" was admission wait inside submit_ms. Budget there: scene 2224x1440 4.2 ms, resolves 2.9, post-processing 2.7 (SMAA 1.3 of that over off, FXAA saves 1.0), shadow map 0.9, rest ~2. Levers in order: resolves (17), AA preset, resolution. Caveat: the F4 overlay itself costs frames while open |
| 19 | Thermal | done | no decline over a 5-minute drive (GPU 14-17 ms flat); not a factor at parity/1440p |
| 20 | Hidden window pacing | needs play test | occluded/minimized windows now pace at 2 fps (`FramePublicationGate::SetHidden`) instead of 18 |
| 21 | Startup warning "could not set vsync=true" | done | the flag belongs to the Vulkan module; startup flags now skip unregistered names |

