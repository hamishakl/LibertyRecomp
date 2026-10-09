# Backlog — PAL / macOS fork

Started 2026-10-10 after the stutter, UI and input sessions. Status: `todo`, `doing`, `done`,
`needs play test` (built, waiting on a gameplay check), `deferred` (reason given).

## Performance
| # | Item | Status | Notes |
|---|---|---|---|
| 1 | Cap the automatic render size at the panel's native pixel size instead of the 2x backing store | todo | `GetNativeResolutionOverride`, gta4_native_hooks.cpp. Generalises `resolution = "1440p"` |
| 2 | Draw-distance / population defaults for Apple Silicon (A/B 1x, 2x, 3x on the same route) | needs play test | `tools/perf/play_preset.sh xbox360-parity` exists; needs a driven route per variant |
| 3 | Make the GPU pass timer additive per category | todo | pass timestamps overlap on Apple GPUs; today it over-reported SMAA 10x |

## Stability and diagnosability
| # | Item | Status | Notes |
|---|---|---|---|
| 4 | Always-on warnings/errors log in the user directory | todo | logging is off without `--diagnostics`; a crash leaves only "abort() called" |
| 5 | "Resolve source has no produced content" rejections (64 per run) | todo | a rejected renderer command is a dropped draw |
| 6 | Remaining PAL gaps: two US data addresses (cloud/timecycle), hooks on TU5-changed functions, data-table scan | todo | `tools/xex/manual_map.json` "unresolved"; `find_unregistered_targets.py` lists 4 pointers in a table at 0x82106Dxx |
| 7 | Game Center "title-profile fetch failed; retrying" every 5 s all session | done | exponential back-off 5 s → 5 min, later failures at info level |

## Input feel
| # | Item | Status | Notes |
|---|---|---|---|
| 8 | Trackpad sensitivity curve / default | needs play test | only `mnk_trackpad_sensitivity` scales macOS-accelerated deltas |
| 9 | Trackpad gestures: two-finger scroll for weapon cycle / radar zoom | todo | check what the wheel already maps to |
| 10 | Mouse-look hold for the two cameras still on retail idle logic | needs play test | which mode drifts decides which function |

## Polish
| # | Item | Status | Notes |
|---|---|---|---|
| 11 | Bring the game window to the front on launch | done | macOS ignores `activateIgnoringOtherApps` from a terminal-launched process; the launch scripts activate via LaunchServices (`osascript ... activate`) instead |
| 12 | Verify console / settings / achievements overlays under the GTA IV theme | needs play test | F4, backtick, F7; only the installer was screenshotted |
| 13 | Stale docs: DEV-NOTES installer section (USA/TU8), dumping guide (PAL/TU5) | done | |
