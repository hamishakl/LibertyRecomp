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
| 6 | Remaining PAL gaps: two US data addresses (cloud/timecycle), hooks on TU5-changed functions, data-table scan | done | all three data addresses resolved 2026-10-10 (radar vtable `0x820131E4`, cloud/timecycle `0x82B30620`/`0x82B30624`) and confirmed in play. Data-table scan reviewed (CRT unwind pointers). The TU5-changed hook review list stays as a note in docs/PAL-PORT.md |
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
| 17 | Render-pass breaks from resolves | todo | gameplay runs 32 resolves per frame (frame summary log) and the existing resolve-reuse path never triggers (`resolve-reuse=0`), while `#end-pass@resolve.mm:184` ends a pass 25x/frame and resolve-draw costs ~3 ms exclusive. Next: log the resolve list for one frame (source/destination sizes, formats, what consumes each) to find the ones that can alias or be skipped |
| 18 | CPU submit spikes to 30+ ms in some areas | todo | slow frames have ~2x the CPU per draw of fast ones (15.6 vs 7.2 ms CPU for 4.0k vs 3.1k draws) with GPU unchanged; a sample taken while parked in such a spot is still needed to see which draw kind is expensive |
| 19 | Thermal | done | no decline over a 5-minute drive (GPU 14-17 ms flat); not a factor at parity/1440p |
| 20 | Hidden window pacing | needs play test | occluded/minimized windows now pace at 2 fps (`FramePublicationGate::SetHidden`) instead of 18 |
| 21 | Startup warning "could not set vsync=true" | done | the flag belongs to the Vulkan module; startup flags now skip unregistered names |

