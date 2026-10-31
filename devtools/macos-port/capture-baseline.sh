#!/bin/bash
# Capture a regression frame set for the macOS core-profile port.
#
#   ./capture-baseline.sh <output-dir>
#
# Stages the static scenario, screenshots at fixed SIMULATION frames (so the
# capture is reproducible regardless of render rate), and writes the frames plus
# a checksum manifest into <output-dir>. Compare two runs with:
#
#   diff old/manifest.txt new/manifest.txt
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="$REPO/build-all"
OUT="${1:?usage: capture-baseline.sh <output-dir>}"
FRAMES="${FRAMES:-600,900}"
SCRIPT="${SCRIPT:-smalldivide.txt}"

WIDGETS="$BUILD/LuaUI/Widgets_BA"
mkdir -p "$WIDGETS" "$OUT"
cp "$HERE/baseline-scenario.lua" "$WIDGETS/zz_baseline_scenario.lua"
trap 'rm -f "$WIDGETS/zz_baseline_scenario.lua"; rmdir "$WIDGETS" "$BUILD/LuaUI" 2>/dev/null || true' EXIT

pkill -9 -f "spring --write-dir" 2>/dev/null || true
sleep 2
rm -f "$BUILD"/screenshots/screen_*.png

want=$(awk -F, '{print NF}' <<<"$FRAMES")

# NB: use find, not ls - ls exits non-zero when the glob matches nothing, and
# with "set -o pipefail" that failure propagates out of the assignment and
# set -e aborts the script before anything is ever captured
count_shots() { find "$BUILD/screenshots" -maxdepth 1 -name 'screen_*.png' 2>/dev/null | wc -l | tr -d ' '; }
cd "$BUILD"
SPRING_DBG_SHOT="$FRAMES" ./spring --write-dir "$BUILD" "$SCRIPT" > "$OUT/run.log" 2>&1 &
pid=$!

for _ in $(seq 1 90); do
  got=$(count_shots)
  # NB: "[ ... ] && break" would abort the whole script under set -e whenever
  # the test is false, which is every iteration until the frames arrive
  if [ "$got" -ge "$want" ]; then
    break
  fi
  sleep 2
done

kill -9 "$pid" 2>/dev/null || true
sleep 1

got=$(count_shots)
if [ "$got" -lt "$want" ]; then
  echo "FAILED: captured $got of $want frames; see $OUT/run.log" >&2
  exit 1
fi

# The scenario MUST have run, or the frames show an empty default scene and a
# bogus baseline gets recorded. BA registers a widget it has not seen before in
# its own config and only loads it from the next run on, so the very first run
# against a fresh write-dir always skips it - re-run once.
if ! grep -q "BASELINE: staged" "$OUT/run.log"; then
  echo "FAILED: scenario never staged - frames would not show the scene." >&2
  echo "        If this is a fresh write-dir, BA has just registered the widget; re-run." >&2
  exit 1
fi

i=0
: > "$OUT/manifest.txt"
for f in $(find "$BUILD/screenshots" -maxdepth 1 -name 'screen_*.png' | sort); do
  i=$((i + 1))
  name="frame-$(printf '%02d' "$i").png"
  mv "$f" "$OUT/$name"
  shasum -a 256 "$OUT/$name" | awk -v n="$name" '{print n, $1}' >> "$OUT/manifest.txt"
done

echo "captured $got frames into $OUT"
cat "$OUT/manifest.txt"
