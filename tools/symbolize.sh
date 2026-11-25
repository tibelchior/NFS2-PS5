#!/bin/bash
# Names the functions in a crash log: ./tools/symbolize.sh ~/Downloads/nfs2se.log
# Reads the CRASH-INFO / CRASH-CALL lines (image offsets) and looks them up in the linked binary of the last build.
# (Logs from before the offsets were logged: the load base was 0x400000, subtract it from rip and from the stack words.)
set -e
LOG=${1:?usage: symbolize.sh <nfs2se.log>}
VOLUME=${BUILD_VOLUME:-dirt2ps5-build-data}
grep -E "CRASH-INFO|CRASH-CALL|CRASH:" "$LOG" | tail -80 > /tmp/nfs2-crash-lines.txt
PYSRC=$(cat <<'PY'
import sys, subprocess, bisect, re
nm = subprocess.run(["llvm-nm", "-n", "-C", "/build/game-nfs2/title-work/llvm-pie.elf"], capture_output=True, text=True).stdout
syms = []
for l in nm.splitlines():
    p = l.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        try: syms.append((int(p[0], 16), p[2]))
        except ValueError: pass
syms.sort(); keys = [s[0] for s in syms]
def name(a):
    i = bisect.bisect_right(keys, a) - 1
    return f"{syms[i][1][:100]} +{a - syms[i][0]:#x}" if i >= 0 else "?"
for l in sys.stdin:
    m = re.search(r"rip-offset ([0-9a-f]+)", l)
    if m: print("crash at   ", name(int(m.group(1), 16)), "|", l.strip()); continue
    m = re.search(r"CRASH-CALL ([0-9a-f]+)", l)
    if m: print("  called from", name(int(m.group(1), 16))); continue
    print(l.strip())
PY
)
docker run --rm -i -v "$VOLUME":/build dirt2ps5-build python3 -c "$PYSRC" < /tmp/nfs2-crash-lines.txt
