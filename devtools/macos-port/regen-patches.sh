#!/bin/zsh
# Regenerate every patch in bar-overrides/ from the deployed override tree.
#
# The deployed tree (<write-dir>/games/bar-macos.sdd/) is gitignored and BAR
# rewrites parts of the write dir, so the patches are the durable copy. Edit the
# deployed file, verify in-game, then run this.
#
#   ./devtools/macos-port/regen-patches.sh [write-dir]
set -u
ROOT=${0:a:h:h:h}
WDIR=${1:-$ROOT/build-all}
SDD=$WDIR/games/bar-macos.sdd
OUT=$ROOT/devtools/macos-port/bar-overrides
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# deployed path -> patch name
typeset -A MAP=(
  'LuaUI/Widgets/api_unit_tracker_gl4.lua'            unit-tracker-ungate.patch
  'LuaUI/Widgets/gui_healthbars_gl4.lua'              healthbars-widget.patch
  'LuaUI/Shaders/HealthbarsGL4_nogs.vert.glsl'        healthbars-nogs-vert.patch
  'LuaUI/Shaders/DrawPrimitiveAtUnit_nogs.vert.glsl'  drawprimitiveatunit-nogs-vert.patch
  'modules/graphics/luashader.lua'                    luashader-quaterniondefs.patch
  'LuaUI/Shaders/sensor_ranges_los.vert.glsl'         sensor-ranges-los-vert.patch
  'LuaUI/Shaders/sensor_ranges_los.frag.glsl'         sensor-ranges-los-frag.patch
  'LuaUI/Widgets/gui_sensor_ranges_radar.lua'         sensor-ranges-radar-widget.patch
  'LuaUI/Widgets/gui_sensor_ranges_sonar.lua'         sensor-ranges-sonar-widget.patch
  'LuaUI/Shaders/reclaim_field_gl4.vert.glsl'         reclaim-field-vert.patch
  'LuaUI/Include/reclaim_field_renderer_gl4.lua'      reclaim-field-renderer.patch
  'LuaUI/Shaders/map_grass_gl4.vert.glsl'             map-grass-vert.patch
  'LuaUI/Widgets/map_grass_gl4.lua'                   map-grass-widget.patch
  'LuaUI/Widgets/gfx_airjets_gl4.lua'                 airjets-widget.patch
  'LuaUI/Widgets/gfx_orb_effects_gl4.lua'             orb-effects-widget.patch
  'LuaUI/Widgets/gfx_paralyze_effect.lua'             paralyze-effect-widget.patch
)

rc=0
for p in ${(k)MAP}; do
  if [ ! -f "$SDD/$p" ]; then
    echo "MISSING from the deployed tree: $p"; rc=1; continue
  fi
  mkdir -p "$TMP/${p:h}"
  if ! "$ROOT/devtools/macos-port/bar-extract.py" "$WDIR" "$p" > "$TMP/$p" 2>/dev/null; then
    echo "NOT IN THE BAR ARCHIVE (renamed upstream?): $p"; rc=1; continue
  fi
  diff -u "$TMP/$p" "$SDD/$p" \
    | sed -e "1s|.*|--- a/$p|" -e "2s|.*|+++ b/$p|" > "$OUT/${MAP[$p]}"
  printf "%-44s %s\n" "${MAP[$p]}" "$(( $(wc -l < "$OUT/${MAP[$p]}") )) lines"
done

echo "--- dry-run re-apply ---"
cd "$TMP"
for p in ${(k)MAP}; do
  [ -f "$TMP/$p" ] || continue
  patch -p1 --dry-run -f < "$OUT/${MAP[$p]}" >/dev/null 2>&1 \
    || { echo "FAILS TO RE-APPLY: ${MAP[$p]}"; rc=1; }
done
[ $rc -eq 0 ] && echo "all patches re-apply cleanly"
exit $rc
