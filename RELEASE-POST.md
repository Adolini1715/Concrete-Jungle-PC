# Predator: Concrete Jungle is running natively on PC (static recompilation, v0.1)

Predator: Concrete Jungle (Eurocom, 2005) never left the original Xbox and PS2. It now runs
as a native Windows (and Linux) program, built by static recompilation: every function of the
Xbox executable is translated to C and compiled for x86-64. No emulator, no game files in the
repo. You build it from your own copy of the game.

**Repo:** <link>  **HD texture packs:** <link>

## Features

**Graphics**
- OpenGL 3.3 renderer for the Xbox GPU (register combiners and vertex programs as GLSL).
- Up to 5x internal resolution (3200x2400, for 4K screens), the game's own 16:9 widescreen, fullscreen.
- 60 fps option (the original is capped at 30).
- Texture packs: drop folders or .zip packs in `textures/`, F3 toggles them live.

**Sharp UI (built in)**
- The whole front end redrawn by hand at 4x: the menu, title and small fonts, menu frames
  and boxes, buttons, the radar ring and the HUD panels. Crisp at any resolution instead of
  stretched 640x480 art. On by default; F3 shows the original.

**Controls**
- A new default "Modern" control style, plus the original styles.
- Modern third-person camera (stops when you let go, optional follow), mouse look.
- Full keyboard play with keyboard key prompts; modern Xbox controller icons on a pad,
  switching to whatever you used last.
- Rebind any action to a key or mouse button (wheel included) right on the Controller page,
  which lists every key when you play on keyboard. Mouse Look, Mouse Speed and Invert Mouse
  sit on the same page.
- Mouse in all the menus: point, click, right click to go back, wheel to scroll.
- Movement: faster turning, a pivot instead of the skid, air control, quick landings,
  charge a jump while moving, attacks that keep momentum, controller rumble.

**Saves**
- Quick save F5, quick load F8.
- Save Game / Load Game in the pause menu: five slots per chapter.

**Menus**
- Options > PC Settings inside the game's own menus (widescreen, fullscreen, v-sync, frame
  rate, resolution scale, mouse, camera, prompts, texture packs).
- Quit Game on the title screen and main menu, PC wording for the save messages, larger menu
  text with even letter spacing (a bug in the original font data), right-sized button icons,
  and both Controller pages redone with a modern Xbox controller.

**HD textures (separate download)**
- Scarface fully redone, all 39 weapon textures, the Jungle Hunter and City Hunter costumes
  (with Scarface's scar and missing eye removed, as in the films), and HD faces for the other
  costumes. More costumes and the HUD to come.

## Building

Needs the USA retail disc (extracted or .iso) and Ubuntu or WSL2 on Windows:

    ./build.sh /mnt/c/path/to/game

First build takes 30 to 60 minutes. Copy `dist/PCJ-PC` to Windows and run `pcj.exe`.

## Known issues

- USA retail only for now.

## Thanks

Built on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and a work-in-progress
decompilation of the game.
