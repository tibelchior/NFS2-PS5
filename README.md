# Need for Speed II SE (1997) for PS5

*Need for Speed II SE* running natively on a jailbroken PS5, using the
[NFSIISE](https://github.com/zaps166/NFSIISE) machine translation of the PC game (64-bit patched), a Vulkan
replacement for its Glide renderer, and the platform layer from this repository on top of
[mihawk-99's PS5 Vulkan stack](https://github.com/mihawk-99/PS5_Vulkan). Title ID `PPSA99789`.

**You bring your own game.** This repository contains no game files and no translated game code. The upstream
sources are downloaded and patched on your computer.

## Status

Tested on a console: menus, races, camera and pause work with the DualSense at **60 fps**. Known issues:

- Rare crash in the middle of a race (game-side, probably a thread race). The log records the thread and the
  function chain; see *Crash log* below.
- The window glass of the cars is lighter than in the original Glide version.
- Draw distance pops in at the original limit.

## What you need

- A PS5 with homebrew enabled, a way to start titles from `/data/homebrew` and FTP access.
- A computer with **Docker** (macOS or Linux, 16 GB memory, about 60 GB free for the toolchain), Python 3,
  git, rsync and [7-Zip](https://www.7-zip.org/) (`7zz`) for the disc image.
- Your own *Need for Speed II SE* PC CD (`.iso`, or a folder with its files).
- The shared Docker volume `dirt2ps5-build-data`, which holds the PS5 toolchain and RADV. It is created by the
  `toolchain` step of the sibling PS5 ports that share it (for example the DiRT 2 or Motorhead port), once,
  1 to 2 hours. Use `BUILD_VOLUME=<name>` for another volume.
- An SDL2 **2.28.x** source tree (the build patches a copy). By default `../../pc-source/UT99/UT99-Android/third_party/SDL2`
  relative to this folder; point `SDL2_SRC=<folder>` at your own checkout of SDL 2.28.5 otherwise.

## Build

```bash
# 1. upstream sources (pinned commits in tools/pins.env) into repo/
./tools/fetch-upstream.sh

# 2. your game files into work/game (lowercased, as the game expects)
./tools/prepare-game.sh "/path/to/Need for Speed II SE.iso"

# 3. patch, cross-compile, link and sign the title
./build.sh title

# 4. copy the game files next to the title
./build.sh package
```

The result is `dist/PPSA99789/`. Run `./build.sh package` again after every `./build.sh title`. Add
`art/icon.png` and `art/wallpaper.png` (your own images) before step 3 for the home-screen art.

Other steps: `./build.sh port` (only patch `repo/src` into `work/src`), `./build.sh sdl2`, `./build.sh compile`.
Options: `BUILD_VOLUME`, `GAME_DIR`, `SDL2_SRC`.

## Install

Copy `dist/PPSA99789` to `/data/homebrew/PPSA99789`, rescan, start **Need for Speed II SE (Glide)**.

```
PPSA99789/
  eboot.bin, sce_sys/, sce_module/   the title
  nfs2se/                            your game files
  data/                              what the port writes (if the title folder is read-only: /data/nfs2se)
    nfs2se.log                       the log
    home/.nfs2se/                    settings, saves and statistics (nfs2se.conf is rewritten on every start)
```

## Controls

| Pad | Game |
|---|---|
| D-pad / left stick | menus and steering (arrow keys) |
| R2 / L2 | gas / brake |
| R1 / L1 | shift up / down (A / Z) |
| Square | handbrake (Space) |
| Triangle | camera (C) |
| L3 | reset car (F11) |
| Options | pause / back (Esc) |
| Cross / Circle | menu select / back |

## How it works

- `tools/port.py` copies `repo/src` to `work/src` and makes it 64-bit clean. The translated game keeps guest
  addresses in 32-bit registers, so every pointer it sees must be below 2 GB.
- `ps5/lowmem.c` replaces malloc and friends with an allocator in a mapping below 2 GB (the console grants
  256 MB there; the game needs about 60 MB). The launcher logs every mmap attempt.
- `ps5/glide/vkglide.cpp` implements the Glide 2 calls the game makes on Vulkan: offscreen colour and depth
  targets, texture arrays (about 1100 to 1300 textures stay alive), paletted textures with Glide's palette
  semantics, texture wrap and clamp, fog, and the additive reflection pass of the cars. Shaders are in
  `ps5/glide/shaders` (rebuild the headers with `glslangValidator -V --vn <name> ... -o <file>.h`).
- `ps5/glue/` starts the game, maps scePad to the keyboard state the game reads, forwards audio to sceAudioOut
  and presents frames. `ps5/patches/sdl2` patches SDL2's dummy video and audio drivers for this.
- `tools/container/*.sh` run inside the build container (SDL2, compile, link and sign).

## Debugging

**Host test** (x86-64 Linux in Docker, software Vulkan, runs slowly on Apple silicon):
`tools/build-x64.sh` builds `work/nfs2se` in the `nfs2-x64` image (`docker build -t nfs2-x64 docker-x64`).
Useful variables: `NFS_NOSIG=1`, `NFS_SCALE=1`, `NFS_DUMP=<dir>` (PPM frames), `NFS_TRACE_FRAME=<n>` (draw
call trace), `NFS_TEXDUMP=<dir>` (raw texture uploads). `./build.sh compile` wipes the host objects, so
rebuild them with `tools/build-x64.sh` afterwards.

**Crash log**: after a crash `nfs2se.log` has `CRASH`, `CRASH-INFO` (thread, image base, offset), `CRASH-REGS`
and `CRASH-CALL` lines. Name the functions with:

```bash
./tools/symbolize.sh ~/Downloads/nfs2se.log
```

(Logs from older builds have no offsets: subtract the load base 0x400000 from the addresses.)

The renderer prints statistics every 300 frames, and the log lists the Glide blend, combine and depth calls
the game makes. Earlier notes are in [docs/NOTES.md](docs/NOTES.md).

See [NOTICE.md](NOTICE.md) for licences and credits.
