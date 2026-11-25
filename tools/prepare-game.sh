#!/bin/bash
# Builds work/game (the game folder the title ships) from your own Need for Speed II SE CD.
#   ./tools/prepare-game.sh "/path/to/Need for Speed II SE.iso"     (or the folder with the CD's files)
# Needs 7zz / 7z (brew install sevenzip) for an .iso, and tools/fetch-upstream.sh run first (the config files come from repo/).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?usage: prepare-game.sh <disc .iso or folder with FEDATA and GAMEDATA>}
OUT=${GAME_OUT:-$HERE/work/game}
UP="$HERE/repo/Need For Speed II SE"
[ -d "$UP" ] || { echo "no $UP: run tools/fetch-upstream.sh first" >&2; exit 1; }
if [ -f "$SRC" ]; then
    TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
    SZ=$(command -v 7zz || command -v 7z || command -v 7za || true); [ -n "$SZ" ] || { echo "install 7-Zip (7zz) to read the .iso" >&2; exit 1; }
    "$SZ" x -y -o"$TMP" "$SRC" > /dev/null
    SRC=$TMP
fi
D=$(find "$SRC" -maxdepth 2 -type d -iname FEDATA | head -1); D=$(dirname "$D")
[ -d "$D" ] && [ "$D" != . ] || { echo "no FEDATA/GAMEDATA in $SRC" >&2; exit 1; }
mkdir -p "$OUT"
rsync -a "$UP/" "$OUT/"
for n in FEDATA GAMEDATA; do
    rm -rf "$OUT/$(echo $n | tr A-Z a-z)"
    cp -R "$(find "$D" -maxdepth 1 -type d -iname $n | head -1)" "$OUT/$(echo $n | tr A-Z a-z)"
done
# every name must be lowercase (the game opens lowercase paths); same job as upstream's convert_to_lowercase
export LC_ALL=C
cd "$OUT"
find fedata gamedata -depth | while IFS= read -r p; do
    d=$(dirname "$p"); b=$(basename "$p"); l=$(printf '%s' "$b" | tr '[:upper:]' '[:lower:]')
    [ "$b" = "$l" ] || mv "$d/$b" "$d/$l"
done
chmod -R u+rwX,go+rX fedata gamedata
echo "work/game ready: $(du -sh "$OUT" | cut -f1)"
