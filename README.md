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

## Installing on Windows

You need the **USA retail** Xbox disc of the game, as a disc image (`.iso`) or extracted to a
folder (the one with `default.xbe` in it). See [Getting your game files](#getting-your-game-files).

1. Download this project (the release's **Source code (zip)**) and unzip it, for example to
   `C:\Games\PCJ`.
2. Put your disc image, or your extracted disc folder, in the **`PUT YOUR GAME HERE`** folder.
3. Double-click **`Install.bat`**. If Windows asks whether to run it, choose Run.

The installer builds the game on your PC from your own copy: it downloads the build tools
(about 300 MB: Python, GCC with CMake and Ninja, MinGit, SDL2, libepoxy, each checked against
a pinned SHA-256 or git commit), the recompiler toolkit
([xboxrecomp](https://github.com/sp00nznet/xboxrecomp) v0.15.0, patched by
`patches/xboxrecomp-v0.15.0-pcj.patch`) and the decompilation it builds on
([Concrete-Jungle-Decomp](https://github.com/Adolini1715/Concrete-Jungle-Decomp)), checks that
your `default.xbe` is the release it supports, recompiles it and compiles `pcj.exe`. The first
run takes a while (see the window for progress); if it stops, run it again and it carries on.

When it is done, the game is in the **`Predator Concrete Jungle PC`** folder, with your game
files copied into it, and there is a desktop shortcut. Nothing is installed on the system and
no administrator rights are needed: everything stays in this folder (or in `C:\PCJ-build` when
the folder's path has a space in it). To uninstall, delete the folder.

## Getting your game files

This project does not provide the game and does not link to copies of it. Make a disc image
from your own disc: with a modded Xbox (copy the disc to its hard disk and transfer it over
FTP), or with a PC DVD drive that can read Xbox discs. Either way the result is a folder with
`default.xbe` and the game's data, or an `.iso` of the disc. Only the USA retail release is
supported for now; the installer checks.

## Building on Linux

On Ubuntu 24.04 (or WSL2), `build.sh` does the same with the system's packages:

```
git clone <this repo> pcj && cd pcj
./build.sh /path/to/your/extracted/game        # or the .iso
```

The result is `dist/PCJ-PC/` with `pcj.exe` for Windows; `--linux` also builds a Linux binary.

## Safety

- There are **no official prebuilt downloads**: build the port yourself from your own copy.
  Do not run a `pcj.exe` someone else hands you.
- Install texture packs and mods only from people you trust. See `SECURITY.md`.

## Playing

Start the game from the desktop shortcut, or run `pcj.exe` in the `Predator Concrete Jungle PC`
folder. It finds the game files in its `game` folder (a `pcj.exe` built with `build.sh` asks for
the folder the first time and remembers it in `pcj.ini`). See `dist/pkgsrc/README-PC.txt` for
keys and settings.

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
