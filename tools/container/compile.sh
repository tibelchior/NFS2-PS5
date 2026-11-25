#!/bin/bash
# Cross-compiles the patched NFSIISE sources (work/src, made by tools/port.py), the low-memory allocator and the Vulkan Glide renderer for Prospero.
source "$(dirname "$0")/common.sh"
install_crt
SDK=$PS5_SDK_DIR
RADV=$VK/.deps/native/radv-release
SRC=/work/work/src
B=$OUT/nfs2-obj; mkdir -p "$B" "$OUT/logs"
[ -f "$SRC/Cpp/NFS2SE.cpp" ] || { echo "no $SRC: run tools/port.py on the host first" >&2; exit 1; }
SDLINC=(-I"$OUT/sdl2-inc/SDL2" -I"$OUT/sdl2-inc")
fail=0
build() {  # source object flags...
    local src=$1 obj=$2; shift 2
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ /work/work/src/.stamp -nt "$obj" ]; then
        "$@" -c "$src" -o "$obj" 2> "$OUT/logs/$(basename "$obj").log" || { grep -E "error" "$OUT/logs/$(basename "$obj").log" | head -20; fail=1; }
    fi
}
log "C++ game core"
build "$SRC/Cpp/NFS2SE.cpp" "$B/NFS2SE.o" "$SDK/bin/prospero-clang++" -std=c++14 -O1 -fno-rtti -fno-exceptions -Wno-c++11-narrowing -w -DNFS_PS5 -I/work/ps5/shim
log "C wrappers"
for f in "$SRC"/*.c; do
    build "$f" "$B/$(basename "$f" .c).o" "$SDK/bin/prospero-clang" -std=gnu89 -O1 -DNFS_CPP -DNFS_PS5 -w -Wno-incompatible-function-pointer-types -Wno-int-conversion "${SDLINC[@]}"
done
log "allocator, formatting, Vulkan Glide"
build /work/ps5/lowmem.c "$B/lowmem.o" "$SDK/bin/prospero-clang" -std=gnu11 -O2 -w -DNFS_PS5
build /work/ps5/guestfmt.c "$B/guestfmt.o" "$SDK/bin/prospero-clang" -std=gnu11 -O2 -w -DNFS_PS5
build /work/ps5/glide/vkglide.cpp "$B/vkglide.o" "$SDK/bin/prospero-clang++" -std=c++17 -O2 -w -DNFS_PS5 -fno-exceptions -fno-rtti -I"$RADV/include" -I/work/ps5/glide
[ $fail = 0 ] || exit 1
ls "$B"/*.o | wc -l
