# Development notes (spike and rounds 1 to 4, kept as written)

Source: https://github.com/zaps166/NFSIISE (wrapper MIT; src/Asm and src/Cpp are submodules, cloned in repo/). Disc: pc-source/NFS-II-SE/Need for Speed II SE.iso (7zz x works; names must be lowercased).

## Spike findings (2026-11-24)
- Core = `src/Cpp/NFS2SE.cpp` (249k lines, machine translation of the x86 asm). Guest address == host pointer cast to int32_t; esp/eax etc. are int32_t.
- 64-bit syntax: only 2284 `(int32_t)<pointer>` casts + 1 push32 size assert fail. `sed 's/(int32_t)/(int32_t)(intptr_t)/g'` + relaxing push32's assert (spike/NFS2SE64b.cpp) compiles cleanly (clang -O1, 7.5 MB object).
- Wrapper C files: only DInput.c (2), EAcsnd.c (4), User32.c (4 warnings) need pointer/int fixes.
- Runtime NOT tested (Docker here is arm64; needs x86-64 or the console).
## Remaining hard problem: everything the game dereferences must sit below 2 GB
Static _data/_bss (link non-PIE at a low base), the guest stack (malloc), malloc_wrap/calloc_wrap, Game ctx, and every pointer the wrappers hand to the game (DInput structs, EAcsnd buffers). Use MAP_32BIT/hint-mmap arenas on Prospero; verify the title ELF base address and that low mappings are allowed.
## Then
GL: Glide emulation emits OpenGL 2 (src/Glide2x*, Wrapper.c) -> needs GL-to-Vulkan (RADV) layer or direct port. Reuse glue from ../NFS-PS5/ps5 (SDL dummy video/audio -> ps5_present/sceAudioOut, virtual pad).

## Round 2 (2026-11-24): low-memory strategy proven on x86-64 Linux (under Docker emulation)
- `tools/port.py` (patches repo/src -> work/src), `tools/build-x64.sh` (amd64 container `nfs2-x64`, docker-x64/), `ps5/lowmem.c` (malloc replacement in a 1 GB arena below 2 GB; MAP_32BIT first, else hinted mmap), `ps5/guestfmt.c` (vsprintf over a 32-bit guest va_list), `ps5/segv.c` (debug backtrace).
- Non-PIE link puts _data/_bss at 0x200000..0x17e0000, so static guest pointers fit in int32.
- Wrapper entry points take `intptr_t` args (guest ints sign-extend to host pointers; fine below 2 GB).
- Game data: disc extracted to pc-source/NFS-II-SE/extract, lowercased into work/game (fedata, gamedata + repo's install.win/text.*).
- Result: the game boots, passes WrapperInit, runs into the translated main (sub_430200 -> sub_49D020 ...). Needed fixes so far: pointer casts, push32 assert, vsprintf varargs, stack-allocated structs passed to guest code (DInput, EAcsnd).
- MAP_32BIT is ignored by the emulation layer here; hinted mmap at 0x10000000 worked.
- Open: with Xvfb+llvmpipe the game crashes in sub_49AE90 (guest null-ish pointer 0x20; file load or wrapper return value wrong) after fixing a heap corruption seen with the real free() (use-after-free by the game or a wrapper writing past a block; `-DLOWMEM_NOFREE` hides it). Upstream also fclose(NULL) when ~/.nfs2se is missing.
- Not yet: any frame rendered; GL->Vulkan; Prospero link/low-mmap check; audio/pad glue.

## Round 3 (2026-11-25): Vulkan Glide + PS5 title PPSA99789
- The 64-bit port renders a race (OpenGL path, then Vulkan path) on x86-64 Linux; screenshots in screenshots/ and work/dumpvk1.
- `ps5/glide/vkglide.cpp` replaces Glide2x.c/OpenGL2.c: offscreen Vulkan render (NFS_SCALE, default 2 => 1280x960), texture array of 768 RGBA8 256x256 layers, readback to the presenter (ut_ps5_present). Host test: NFS_DUMP=<dir> writes PPM frames. Shaders: ps5/glide/shaders (glslangValidator -V --vn spv_glide_vert ... -o glide_vert.h).
- PS5 build: `./build.sh title` then `./build.sh package` -> dist/PPSA99789 (copy to /data/homebrew/PPSA99789). Pieces: SDL2 2.28.5 (UT99 tree, patched dummy video/audio), glue in ps5/glue (presenter exports the Vulkan device), ps5/lowmem.c provides __wrap_malloc & co so every allocation stays below 2 GB (the eboot loads at 0x400000), link uses --allow-multiple-definition to override the platform heap's wrappers.
- NOT TESTED ON THE CONSOLE. Things to read first in data/nfs2se.log: lowmem arena (mmap below 2 GB may be refused), Vulkan setup lines "[glide-vk] ...", controller mapping (nfs2se.conf in data/home/.nfs2se).

## Round 4: running on the console (PPSA99789)

Status: the Vulkan Glide title runs on a real PS5 at 60 fps in races; menus, races, camera and pause work with the pad.

Install: copy `dist/PPSA99789` to `/data/homebrew/PPSA99789`; settings, saves and the log live in `/data/nfs2se` (`nfs2se.log`).

Pad (real SDL key events from `ps5/glue/ps5_io.cpp`): D-pad / left stick = arrows, R2 gas, L2 brake, R1/L1 shift up/down (A/Z), Square handbrake (Space), Triangle camera (C), Options pause/back (Esc), L3 car reset (F11), Cross/Circle menu select/back (joystick buttons). `ps5/glue/ps5_launch.cpp` rewrites `home/.nfs2se/nfs2se.conf` on every start.

Lessons:
- The console only grants a 256 MB mapping below 2 GB; `ps5/lowmem.c` tries 1.5 GB down to 128 MB and the launcher logs every attempt. The game uses about 60 MB.
- Texture bookkeeping bug: `textures[]` was zero-initialised, so every texture began with layer 0 instead of "none"; layer 0 was released repeatedly and shared. The game keeps about 1100 textures alive: 1280 layers of 256x256 (335 MB).
- The game reads the keyboard state itself: pushed SDL events do not set it, `SDL_SendKeyboardKey` does.
- Debug by logging: the renderer prints texture/draw statistics every 300 frames (`vkglide.cpp`); the launcher logs mmap attempts, crash signal, registers and stack (resolve with llvm-addr2line on `title-work/llvm-pie.elf`, PIE base 0x400000).
- Host test with a live view: container `nfs2vnc` (Xvfb + x11vnc + noVNC on port 6080); `./build.sh compile` wipes the host objects (port.py), rebuild them with `tools/build-x64.sh`.

Texture storage (Round 4, later): textures <= 64 px live in a 64x64 array of 3072 layers, larger ones in a 256x256 array of 512 layers (178 MB in total; the game keeps 1100-1300 textures alive, one track needed 1317 and overflowed the older 1280-layer single array, which skipped uploads and drew wrong textures). The renderer log also lists every blend/combine/depth-bias/chroma-key call the game makes ("call ..." lines) and the number of vertices with second-TMU coordinates, to find what the car shine needs. The game only uses two combine modes (iterated colour, texture x iterated colour) and additive/normal alpha blends.
