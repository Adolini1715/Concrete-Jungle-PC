# Predator: Concrete Jungle, PC port

A native PC port of **Predator: Concrete Jungle** (Eurocom, 2005, original Xbox), made by
static recompilation: the game's Xbox program is translated to C and compiled for x86-64,
with parts of it replaced by decompiled and modified C++.

**This repository contains no game code or data.** You build the port from your own copy of
the game, and the built game reads your own disc files when it runs.

## What you get

- **Runs natively on Windows** (and Linux), OpenGL 3.3, no emulator.
- **Up to 5x internal resolution** (3200x2400, for 4K screens), widescreen (the game's own 16:9 mode), 60 fps option.
- **Modern controls** (default style 1): a modern pad layout, a modern third-person camera
  with optional follow, mouse look, keyboard play, faster turning, a pivot instead of the
  skid, air control, a quick landing, jump charging while you move, attacks that keep their
  momentum, controller rumble. The original control styles stay in the Control Style list.
- **PC button prompts**: keyboard keys or modern controller icons in place of the Xbox ones,
  switching to whichever you used last.
- **Rebindable keyboard and mouse**: on keyboard, the Controller pages (main menu and pause
  menu) list every action with its key; pick one and press a new key or mouse button (the
  wheel works too). Mouse Look, Mouse Speed and Invert Mouse are on the same page.
- **Mouse in the menus**: point to select, click to choose, right click to go back, wheel to
  scroll; the on-screen button prompts and the Hunting Gear rack are clickable too.
- **Saves**: a quick save (F5) and quick load (F8), plus Save Game / Load Game in the pause
  menu with five slots per chapter.
- **Sharp UI, built in**: the fonts, menu frames, buttons, radar and HUD redrawn by hand at
  4x, crisp at any resolution. On by default.
- **Texture packs**: drop folders or .zip packs into `textures/`; F3 toggles them in play.
  HD packs for Scarface, his weapons and the alternate costumes (Jungle Hunter and City Hunter
  so far) are released separately.
- **Options > PC Settings** inside the game's own menus.
- **Menu fixes**: a Quit Game option, PC wording for the save messages, menu letters no longer
  clipped (an off-by-one in the game's font data), even letter spacing, and both Controller
  pages redone with a modern Xbox controller.
- **A desktop-friendly fullscreen**: Alt+Tab, the Snipping Tool and HDR displays work without
  flicker; F12 saves a screenshot.

For the full list of changes, the bugs fixed, and the work behind them, see
[docs/what-changed.md](docs/what-changed.md).

## Building

You need the **USA retail** disc, extracted to a folder (or as an .iso), and a Linux
environment: Ubuntu 24.04, or **WSL2 on Windows** (`wsl --install` in an
administrator PowerShell, then open Ubuntu).

```
git clone <this repo> pcj && cd pcj
./build.sh /mnt/c/path/to/your/extracted/game        # or the .iso
```

`build.sh` installs the build tools (apt), fetches the recompiler toolkit
([xboxrecomp](https://github.com/sp00nznet/xboxrecomp) v0.15.0, patched by
`patches/xboxrecomp-v0.15.0-pcj.patch`) and the decompilation it builds on ([Concrete-Jungle-Decomp](https://github.com/Adolini1715/Concrete-Jungle-Decomp)), checks that your
`default.xbe` is the release it supports, recompiles it, and builds `pcj.exe`. The first run
takes 30 to 60 minutes. The result is `dist/PCJ-PC/`: copy that folder to Windows and run
`pcj.exe`. `--linux` also builds a Linux binary.

## Safety

- There are **no official prebuilt downloads**: build the port yourself from your own copy.
  Do not run a `pcj.exe` someone else hands you.
- Install texture packs and mods only from people you trust. See `SECURITY.md`.

## Playing

Run `pcj.exe`. The first time it asks for your game folder (the extracted disc) and
remembers it in `pcj.ini`. See `dist/pkgsrc/README-PC.txt` for keys and settings.

## How it works

- `xboxrecomp` lifts every function of `default.xbe` to C (`src/recomp/gen`, generated, not
  in this repo) and supplies the Xbox kernel, memory layout, audio and an NV2A pushbuffer
  executor.
- `src/gl_backend.c` draws the NV2A pushbuffer with OpenGL (register combiners and vertex
  programs as GLSL).
- `tools/hybridize` compiles functions from the pcj-decomp decompilation as native code in
  place of their recompiled versions; `mods/` overlays changed versions of some of them
  (movement, camera, menus). See `docs/hybridize.md` and `mods/README.md`.
- `docs/` has notes on each part (texture packs, saves, prompts, settings, determinism,
  recording and replay).

## Known issues

- Only the USA retail release is supported.
- HD textures cover Scarface, his weapons, Jungle Hunter and City Hunter so far.

## Credits

- Recompiler toolkit: [xboxrecomp](https://github.com/sp00nznet/xboxrecomp).
- Third-party code in `third_party/` (stb, openlibm) under their own licences.
- Licence: MIT for this project's own code, with the exceptions listed in `NOTICE.md`.
- Predator: Concrete Jungle is (c) its respective owners. This project is not affiliated
  with or endorsed by them.
