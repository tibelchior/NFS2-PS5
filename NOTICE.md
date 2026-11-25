# Notice

This repository is a **PS5 port kit** for [NFSIISE](https://github.com/zaps166/NFSIISE) (zaps166), a
cross-platform wrapper around a machine translation of *Need for Speed II SE* (Electronic Arts / Pioneer
Productions, 1997).

- It contains **no game files and no translated game code**. `tools/fetch-upstream.sh` downloads the upstream
  sources into `repo/` and `tools/port.py` patches a copy into `work/src`; both folders are ignored by git
  and must not be committed or shared. The game data comes from *your own* CD (`tools/prepare-game.sh`).
- **NFSIISE** (wrapper MIT licence, Błażej Szczygieł) and its `NFSIISE-ASM` / `NFSIISE-CPP` submodules are
  fetched at the commits pinned in `tools/pins.env`. The translated code in `NFSIISE-CPP` derives from the
  original game executable: check its terms before sharing anything built from it.
- `ps5/` (Vulkan Glide renderer, low-memory allocator, launch glue, scePad and audio) and `tools/` (build
  scripts) are this port's own files.
- **PS5 toolchain, payload SDK and Vulkan driver**: mihawk-99/PS5_Vulkan, PS5_PayloadSDK and PS5_Mesa, fetched
  at the pinned commits in `tools/pins.env` (GPL-3.0 and Mesa's licences). A finished title links their code,
  so share built titles only together with the source obligations those licences require.
- **SDL2 2.28.x** (zlib licence), patched at build time by `ps5/patches/sdl2/apply.py`.
- **Need for Speed** is the property of its rights holders. This project is not affiliated with or endorsed
  by Electronic Arts or Sony. You need your own legal copy of the game.
