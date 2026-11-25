#!/bin/bash
# Build the patched NFSIISE C++ core as x86-64 Linux inside the amd64 container (spike; runs under emulation on Apple silicon).
set -e
cd "$(dirname "$0")/.."
python3 tools/port.py
docker run --rm --platform linux/amd64 -e EXTRA_CFLAGS -v "$PWD":/w nfs2-x64 bash -c '
set -e; cd /w/work/src; rm -f *.o
echo "C++ core..."; time clang++ -c -m64 -fno-pie -std=c++14 -O1 -Wno-c++11-narrowing -fno-rtti -fno-exceptions Cpp/NFS2SE.cpp -o NFS2SE.o 2>&1 | grep -E "error" | head -20
echo "C wrappers..."; clang -c -m64 -fno-pie -std=gnu89 -O1 $EXTRA_CFLAGS -DNFS_CPP -Wno-incompatible-function-pointer-types -Wno-int-conversion -w *.c ../../ps5/lowmem.c ../../ps5/segv.c ../../ps5/guestfmt.c 2>&1 | head -20
echo "Vulkan Glide..."; clang++ -c -m64 -fno-pie -std=c++17 -O2 -I../../ps5/glide ../../ps5/glide/vkglide.cpp -o vkglide.o 2>&1 | grep -E "error|warning: unused" | head -20
clang -no-pie -fuse-ld=lld -o /w/work/nfs2se *.o -lSDL2 -lGL -lm -lpthread -ldl -lstdc++ -rdynamic && ls -la /w/work/nfs2se && readelf -lW /w/work/nfs2se | grep LOAD'
