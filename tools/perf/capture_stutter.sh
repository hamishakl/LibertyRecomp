#!/bin/bash
# Launch Liberty Recompiled with stutter diagnostics on, one folder per session:
#   frames.csv     per-frame renderer timing (tools/perf/summarize_frames.py)
#   gpu-passes.csv average GPU ms per render-pass category, every 300 frames
#   game.log       presenter/frame-pacer trace (display-link opportunities, handoff waits, paint results)
# usage: tools/perf/capture_stutter.sh [extra --flags...]    e.g. --gta4_frame_limit=0
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
app="$repo/out/build/macos-release/LibertyRecomp/Liberty Recompiled.app/Contents/MacOS/Liberty Recompiled"
out="$HOME/Library/Application Support/LibertyRecomp/perf/stutter-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
echo "capture -> $out"
exec "$app" --diagnostics=true --diagnostics_categories=logging,presenter \
  --log_file="$out/game.log" --gta4_metal_frame_log="$out/frames.csv" \
  --gta4_metal_gpu_pass_log="$out/gpu-passes.csv" "$@"
