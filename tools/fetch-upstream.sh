#!/bin/bash
# Clones the upstream NFSIISE sources into repo/ at the commits this port was built against (tools/pins.env).
# repo/ stays pristine: tools/port.py patches a copy into work/src on every build.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
source "$HERE/tools/pins.env"
R=$HERE/repo
clone_at() {  # url dir commit
    [ -e "$2/.git" ] || git clone -q "$1" "$2"
    git -C "$2" cat-file -e "$3^{commit}" 2>/dev/null || git -C "$2" fetch -q origin
    git -C "$2" checkout -q --detach "$3"
}
clone_at "$NFSIISE_REPO" "$R" "$NFSIISE_COMMIT"
clone_at "$NFSIISE_ASM_REPO" "$R/src/Asm" "$NFSIISE_ASM_COMMIT"
clone_at "$NFSIISE_CPP_REPO" "$R/src/Cpp" "$NFSIISE_CPP_COMMIT"
echo "repo/ ready ($(wc -l < "$R/src/Cpp/NFS2SE.cpp") lines in NFS2SE.cpp)"
