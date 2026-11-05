#!/bin/zsh
# Regenerate the patches in bar-overrides/ from the deployed override tree.
#
# The inverse of deploy-bar-overrides.py: that turns patches into a loadable
# .sdd, this turns an edited .sdd back into patches. The deployed tree is
# gitignored and BAR rewrites parts of the write dir, so the patches are the
# durable copy. Workflow: deploy, edit the deployed file, verify in-game, run
# this, commit.
#
#   ./devtools/macos-port/regen-patches.sh [write-dir]
#
# Each patch names its own target in its "--- a/<path>" header, which is where
# the target comes from here too - the same rule deploy-bar-overrides.py uses,
# so neither script carries a list that can drift from the other.
set -u
ROOT=${0:a:h:h:h}
WDIR=${1:-$ROOT/build-all}
SDD=$WDIR/games/bar-macos.sdd
OUT=$ROOT/devtools/macos-port/bar-overrides
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

rc=0
for patch_file in $OUT/*.patch(N); do
  name=${patch_file:t}
  target=$(sed -n '1s|^--- a/||p' $patch_file)

  if [ -z "$target" ]; then
    echo "$name: no \"--- a/<path>\" header"; rc=1; continue
  fi
  if [ ! -f "$SDD/$target" ]; then
    echo "$name: $target is not in the deployed tree - deploy first"; rc=1; continue
  fi

  mkdir -p "$TMP/${target:h}"
  if ! "$ROOT/devtools/macos-port/bar-extract.py" "$WDIR" "$target" > "$TMP/$target" 2>/dev/null; then
    echo "$name: $target is not in the BAR archive - renamed upstream?"; rc=1; continue
  fi

  diff -u "$TMP/$target" "$SDD/$target" \
    | sed -e "1s|.*|--- a/$target|" -e "2s|.*|+++ b/$target|" > "$patch_file"

  if [ ! -s "$patch_file" ]; then
    echo "$name: EMPTY - the deployed file matches stock BAR, so the override is gone"; rc=1; continue
  fi

  printf "  %-36s %-48s %s lines\n" "$name" "$target" "$(wc -l < $patch_file)"
done

echo "--- dry-run re-apply ---"
cd "$TMP"
for patch_file in $OUT/*.patch(N); do
  target=$(sed -n '1s|^--- a/||p' $patch_file)
  [ -n "$target" ] && [ -f "$TMP/$target" ] || continue
  patch -p1 --dry-run -f < $patch_file >/dev/null 2>&1 \
    || { echo "FAILS TO RE-APPLY: ${patch_file:t}"; rc=1; }
done
[ $rc -eq 0 ] && echo "all patches regenerated and re-apply cleanly"
exit $rc
