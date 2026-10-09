#!/bin/bash
# Launch Liberty Recompiled with a settings preset applied as command-line overrides.
# usage: tools/perf/play_preset.sh <preset name or path> [extra --flags...]
# Presets live in tools/perf/presets/*.toml (flat "key = value" lines). Command-line values beat
# native.toml, so your normal config (and its per-session frame log) is untouched.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
preset="${1:?usage: play_preset.sh <preset> [--flags]}"; shift
[ -f "$preset" ] || preset="$here/presets/$preset.toml"
[ -f "$preset" ] || { echo "no such preset: $preset" >&2; exit 1; }
args=()
while IFS= read -r line; do
  line="${line%%#*}"; [[ "$line" =~ ^[[:space:]]*([A-Za-z0-9_]+)[[:space:]]*=[[:space:]]*(.*[^[:space:]])[[:space:]]*$ ]] || continue
  key="${BASH_REMATCH[1]}"; value="${BASH_REMATCH[2]}"; value="${value%\"}"; value="${value#\"}"
  args+=("--$key=$value")
done < "$preset"
app="$repo/out/build/macos-release/LibertyRecomp/Liberty Recompiled.app/Contents/MacOS/Liberty Recompiled"
echo "Preset $(basename "$preset"): ${args[*]} $*"
# macOS refuses programmatic activation from a terminal-launched process, so bring the game
# forward through LaunchServices once it is up; otherwise it sits behind the terminal and runs
# at the hidden-window pace until clicked.
( sleep 4; osascript -e 'tell application "Liberty Recompiled" to activate' >/dev/null 2>&1 ) &
exec "$app" "${args[@]}" "$@"
