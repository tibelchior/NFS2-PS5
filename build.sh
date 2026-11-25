#!/bin/bash
# Need for Speed II SE (NFSIISE, Glide via Vulkan) for the PS5.
#   ./build.sh port      patch repo/src -> work/src (tools/port.py)
#   ./build.sh sdl2      SDL2 for Prospero (patched source from the UT99 tree)
#   ./build.sh compile   cross-compile the game core, wrappers, allocator and Vulkan Glide renderer
#   ./build.sh title     port + sdl2 + compile + link + sign into dist/<TITLE_ID>
#   ./build.sh package   copy the game files (work/game) into dist/<TITLE_ID>/nfs2se
# Options: BUILD_VOLUME=<name> (default dirt2ps5-build-data), GAME_DIR=<game data dir, default work/game>, SDL2_SRC=<SDL2 2.28.x source folder>
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/tools/pins.env"
STEP=${1:-title}
GAME=${GAME_DIR:-$HERE/work/game}
IMAGE=dirt2ps5-build VOLUME=${BUILD_VOLUME:-dirt2ps5-build-data} NAME=nfs2ps5vk
die() { echo "error: $*" >&2; exit 1; }
if [ "$STEP" = port ]; then python3 "$HERE/tools/port.py"; exit 0; fi
if [ "$STEP" = package ]; then
    OUT=$HERE/dist/$TITLE_ID
    [ -f "$OUT/eboot.bin" ] || die "no title yet: run ./build.sh title first"
    [ -d "$GAME/fedata" ] || die "no $GAME/fedata (see README: game data)"
    mkdir -p "$OUT/nfs2se"
    rsync -a --exclude nfs2se --exclude '*.desktop' --exclude '*.png' --exclude '*.bat' --exclude convert_to_lowercase --exclude '*.log' "$GAME/" "$OUT/nfs2se/"
    chmod -R u+rwX,go+rX "$OUT/nfs2se"
    echo "Done: $OUT  ->  copy to /data/homebrew/$TITLE_ID on the console (read data/nfs2se.log first)"
    exit 0
fi
docker info > /dev/null 2>&1 || die "Docker is not running."
docker image inspect "$IMAGE" > /dev/null 2>&1 || docker build -q -t "$IMAGE" "$HERE/docker" > /dev/null
SDL2_SRC=${SDL2_SRC:-$HERE/../../$SDL2_SRC_REL}
[ -f "$SDL2_SRC/include/SDL.h" ] || die "SDL2 2.28.x source not found at $SDL2_SRC (set SDL2_SRC=<folder>, see README)"
[ -f "$HERE/repo/src/Cpp/NFS2SE.cpp" ] || die "no repo/: run tools/fetch-upstream.sh first"
docker rm -f "$NAME" > /dev/null 2>&1 || true
mkdir -p "$HERE/dist"
python3 "$HERE/tools/port.py" > /dev/null
docker run -d --name "$NAME" -v "$VOLUME":/build -v "$HERE":/work -v "$SDL2_SRC":/sdlsrc:ro "$IMAGE" sleep infinity > /dev/null
trap 'docker rm -f "$NAME" > /dev/null 2>&1 || true' EXIT
run() { docker exec -e VOL=/build "$NAME" bash "/work/tools/container/$1.sh"; }
case $STEP in
    sdl2) run sdl2 ;;
    compile) run sdl2; run compile ;;
    title) run sdl2; run compile; run title ;;
    *) die "unknown step '$STEP'" ;;
esac
