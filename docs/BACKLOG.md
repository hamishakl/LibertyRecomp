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
| 6 | Remaining PAL gaps: two US data addresses (cloud/timecycle), hooks on TU5-changed functions, data-table scan | deferred | data scan reviewed: the 4 pointers at 0x82106Dxx point into the middle of CRT functions (unwind/pdata-style table), not call targets. The two US data addresses need the cloud/timecycle code read against the PAL image; `find_address_taken_targets.py` reports 0 |
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
| 12 | Verify console / settings / achievements overlays under the GTA IV theme | needs play test | F4, backtick, F7; only the installer was screenshotted |
| 13 | Stale docs: DEV-NOTES installer section (USA/TU8), dumping guide (PAL/TU5) | done | |
