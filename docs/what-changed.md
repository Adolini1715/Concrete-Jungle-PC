# What this port changes, and what it took

This is a record of everything in the v0.1 release of the Predator: Concrete Jungle PC port:
what it adds to the original Xbox game, what it fixes, and the work behind it. It only covers
what ships in this repository. Experiments that were tried and dropped are noted at the end.

## At a glance

- **Two weeks of work**, October 17 to November 1, 2026.
- **240 commits** to the port and **687 commits** to the decompilation it builds on.
- **12,072 of the game's 13,795 functions** rewritten as C++ by the decompilation, across
  244 source units, about 79% of the game's code by size. **10,386** of them compile back to the
  exact bytes of the original.
- **138 functions** run as native code compiled from that decompiled C++, in place of their
  machine-translated versions. **18** of those are deliberately modified (movement, camera,
  menus) in `mods/`.
- **7 bugs fixed in the recompiler toolkit and runtime**, and **6 in the original game itself**, two
  of them in its own 60 fps support.

The whole project was built with AI assistance (Claude), directed and play-tested by one person.
It is a first release. More eyes on it are welcome, and so are bug reports.

## How the port works

The game ships as one Xbox program, `default.xbe`. The port does not emulate it. Instead:

1. **Static recompilation.** The [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit
   translates every function in your copy of `default.xbe` into C, which is compiled for 64-bit
   Windows or Linux. The toolkit also stands in for the Xbox kernel, memory layout and audio
   hardware.
2. **A real GPU back end.** The Xbox draws through its NV2A GPU by writing command buffers. The
   port executes those buffers on the PC and draws them with OpenGL 3.3, turning the NV2A's
   register combiners and vertex programs into GLSL shaders that run on your graphics card.
3. **Decompiled code swapped in.** A companion decompilation project rewrites the game as
   readable C++ that compiles back to the original bytes. A tool in this repository
   (`tools/hybridize`) takes the functions that match and compiles them as native code in place
   of their machine translations. That is what makes real changes possible: the movement,
   camera and menu changes below are edits to that C++, not binary patches.

No game code or data is in this repository. You build the port from your own disc.

## What you get

### Running on a PC

- Native Windows build (and Linux), no emulator.
- Internal resolution up to 5x (3200x2400), for 1440p and 4K screens.
- Widescreen: the game's own 16:9 mode, presented at 16:9.
- 60 fps: the original caps gameplay at 30 and its menus ran at 20. The engine already scales
  its gameplay by the measured frame rate, so lifting the cap needed no game changes.
- Fullscreen that behaves on a modern desktop: Alt+Tab, the Snipping Tool and HDR displays all
  work without flicker or brightness jumps, and the game pauses while you are away.
- F12 screenshots, saved next to the game.
- Controller rumble, and an analog stick dead zone that no longer snaps to the four directions.
- A Windows installer: put your disc image or extracted disc in a folder and double-click
  `Install.bat`. It fetches pinned build tools into that folder (no WSL, no administrator
  rights), checks that it is the Xbox release it supports (it tells you if you gave it the
  PlayStation 2 disc), builds the game from your copy in about 10 minutes, and leaves it in a
  game folder next to the installer's with a desktop shortcut.
- Sensible first-run defaults: widescreen on, the internal resolution matched to your screen
  (3x for 1080p and 1440p, 5x for 4K), and vertical look the right way up (the original inverted
  it, in third and first person). All of them are in Options.
- F7 switches between 30 and 60 fps at any time, to compare.
- A first-run folder picker for your game files; settings saved to `pcj.ini`.

### Controls and movement

- A modern control layout is the default. The original control styles are still in the list.
- A modern third-person camera with an optional follow mode, and adjustable camera distance and
  sensitivity.
- Full keyboard and mouse play with mouse look. Every action can be rebound to any key, mouse
  button or the wheel, from the Controller page. Esc pauses, Tab opens weapon select.
- Movement reworked in the decompiled code, not patched:
  - faster turning, and a pivot instead of the long skid when you reverse;
  - a short stop instead of a slide;
  - air control;
  - charging a jump while running, with a rumble pulse when the strongest jump is ready;
  - a quick landing after hard drops;
  - attacks that keep your momentum instead of stopping you dead.

### Menus and interface

- **Options > PC Settings**, built by the game's own menu system, for resolution, widescreen,
  frame rate, camera, mouse and saves.
- **Mouse in every menu**: point to highlight, click to choose, right click to go back, wheel to
  scroll. Clicks land anywhere on an item, not just its exact box (a save slot's own box is
  only its title line). The on-screen button prompts are clickable too (Select, Back, Quit, and
  "Press Start" on the title). The Windows pointer shows when you use the mouse and steps aside
  when you use the pad or keys.
- **Mouse on the Hunting Gear rack and in the trophy room**: click any slot or trophy, unlocked
  or not, to send the lasers to it, and click again to choose it.
- **Mouse in play's menus**: the pause menu's tabs switch on a click (not on hover); in weapon
  select a click picks a row and the arrows beside each weapon change it.
- **PC button prompts**: keyboard keys or modern controller icons in place of the Xbox ones,
  switching to whichever you used last, and following your key bindings. "Scroll" reads
  "Navigate", shown as the arrow keys.
- **Controller pages redone** with a modern Xbox controller, or a two-column key list when you
  play on keyboard.
- **A Quit Game option**, PC wording for the save messages, and "Special Move" renamed to what it
  actually does ("Flip / Ledge Grab").
- **Sharp UI**, built in and on by default: the fonts, menu frames, buttons, radar and HUD
  redrawn by hand at 4x, crisp at any resolution.

### Saves

- Quick save (F5) and quick load (F8), also on the controller (click both sticks to save, press
  both bumpers to load). A quick save is a snapshot of the running game, so it only loads into
  the exact build of `pcj.exe` that made it; another build says so instead of loading it.
- Save Game and Load Game in the pause menu, with five slots per chapter.

### Textures and mods

- **Texture packs**: drop folders or `.zip` files into `textures/` and they replace the game's
  textures at any size. F3 toggles them during play.
- **Texture and text dumping**, to make your own packs.
- HD packs for Scarface, his weapons and the alternate costumes are released separately.

## Bugs fixed

### In the original game

- **Clipped menu letters.** Every glyph in the game's fonts is mapped with its texture coordinates
  computed as pixel / 255 instead of pixel / 256. The error grows across the font texture, so
  letters on its right half lose most of their first column: 'L' starts drawing at column 199.78
  while its ink starts at 199. At 480p on an Xbox it blurs away; at 1440p it is plain to see. The
  port corrects the coordinates when each font loads. This was checked against all three menu
  fonts: every glyph lands exactly on its ink.
- **A frame-rate change that froze the game.** The engine resets the Direct3D device whenever its
  frame cap changes, for a new vsync interval, and that reset never frees the old back buffers: the
  engine still holds them. On the Xbox it ran a few times a session. Switching Frame Rate a few
  times ran the 64 MB graphics pool out and the game drew frames that were never shown. The port
  paces frames itself, so the reset is skipped after the first.
- **Text layout that never ended.** When a button icon is wider than its text box, the game's word
  wrap breaks off an empty line and starts again at the same place, forever, writing a new line
  record each time until it overruns memory. No Xbox icon was wide enough to trigger it; the PC
  keycaps follow the key's width, and "Tab" did, in the weapon selector. The layout now sees a box
  wide enough for its widest icon or letter.
- **Ledge grabs at 60 fps.** The game has its own 60 fps mode, scaled by a frame-rate multiplier
  that is 2 at 30 fps and 1 at 60. The automatic ledge catch scaled two distances by it as if they
  were per-frame steps: how far it searches for a ledge, and how far above the hands a ledge can be
  caught at the top of a jump. At 60 fps both halved, and a jump that peaks just under a ledge (a
  charged jump onto the high walls of the first level) caught it at 30 fps and missed it at 60.
  Perch landings had the same fault. Both now keep their 30 fps distances at any frame rate. Found
  by playing the same jump at 30 and 60 fps (F7) with the catch logged frame by frame.
- **Uneven letter spacing.** Text is laid out in whole 640x480 units, so any scaled text gained
  random gaps ("Ne w Ga me"). Spacing is now computed per letter from the font's actual ink, at
  whatever size the text is drawn, with a little more room between letters than before.
- **Clipped text.** Right-aligned text ends exactly on its box's edge, where the menus clip, so
  numbers in the chapter statistics lost their last column. And the PC keycaps are wider than the
  Xbox icons the footers were laid out for: three prompts in the pause menu's footer did not fit.
  Right-aligned text keeps a few units clear of the edge, and a prompt too wide for its box is drawn
  slightly smaller instead of cut off.

### In the recompiler toolkit and runtime

These are faults in the translation and its runtime, found by playing the port and tracing them
back. The toolkit fixes ship as `patches/xboxrecomp-v0.15.0-pcj.patch`.

- **162 missing AI states.** The translator read one of the AI's jump tables (439 entries) only up
  to entry 256, so every enemy behaviour past that point silently did nothing.
- **Signed comparisons** after addition and subtraction took the wrong branch in some cases.
- **8- and 16-bit multiply and divide** were translated at the wrong width.
- **A one-frame flicker** in draws, from the GPU command buffer wrapping around mid-command.
- **Distorted, clipping audio**, because the audio processor read its sample buffers from the
  wrong place.
- **A wrong C runtime result** (`fmod(0, x)`) that the game depends on, fixed in the port's runtime.
- **A 36-minute disassembly.** Capstone's Python `disasm()` decodes its whole buffer before it
  hands back the first instruction, and the function finder probed thousands of gaps by passing
  it the rest of the section each time. Feeding it small windows and caching decoded addresses
  gives byte-identical output in 3.5 minutes, so a whole build now takes about 10 minutes on a
  2-core machine instead of 30 to 60.

## The work behind it

Most of the two weeks went into making changes safely, not into the changes themselves.

**Proving the native code is the same game.** Swapping a function from translated code to
compiled C++ is only safe if the two behave identically. The port can run fully deterministically:
same inputs, same frames, bit for bit. Gameplay recordings replay exactly, and every swapped
function was checked by running both versions against the same recorded play and comparing the
results. Early on this caught subtle problems, like the glue between translated and native code
failing to preserve a scratch register that the caller still needed.

**Matching the real hardware.** A recorder was built for the xemu emulator that captures what the
real game does (frames, audio, input, and the positions and states of characters during
cutscenes), in the same format the port logs. Comparing the two pinned down GPU accuracy problems
(depth precision, fog, polygon offsets, frame timing) and helped trace enemies behaving
differently from the original.

**Reverse-engineering what the game never documented.** The animation system's data format, the
menu system's layout and input code, the save format, the font tables and the control mapping were
all worked out from the decompiled code, so the new features could be built with the game's own
machinery. The PC Settings page, the save slot screens and the Controller pages are real game
menus, not overlays.

**Fixing what only shows up on a PC.** The last days before release went into problems that never
existed on an Xbox: Alt+Tab and the Snipping Tool fighting the fullscreen window, HDR brightness
jumps, and text that was fine at 480p but visibly broken at 1440p. Each was solved by logging what
Windows was actually doing rather than guessing. The window was being promoted to a direct-to-display
mode that every overlay had to break.

**Giving a console game a mouse.** The menus were built for a d-pad, so mouse support meant finding
each screen's own item list and positions in the decompiled code and driving the game's own focus
and selection. Logs from real play found the problems a test machine could not: clicks shorter than
one frame at 60 fps went unnoticed by the menus, and a timestamp shared between two threads could
read as 49 days old for a moment, which made the key list flicker. The Hunting Gear rack draws its
weapons as 3D objects with no menu items at all; there, the game's selection code and variables were
located in the original machine code, and the click areas were measured from a screenshot and traced
outlines of every slot.

**Testing without a person.** The last days added an automated tour (`tools/tour.py`). It boots the
port, drives it through the menus, PC Settings, a level, weapon select and the pause menu with the
pad, the keyboard and the mouse, adds minutes of random input, screenshots every step and flags any
crash, freeze or wild call; it also runs the Windows build under Wine. A test mode that draws only
around screenshots (`CJ_GL_SKIP=1`) lets it run at full speed on a machine with no GPU. On its first
runs it found two bugs no one had reported: clicks in weapon select that did nothing, and a prompt
cut off on the title screen. Freezes write `pcj-freeze.txt` with the game thread's state, and a
memory watch (`RECOMP_WATCH`) names the code that writes a given address, which is how the text
layout bug above was found.

**Installing on a real PC, and playing at 60 fps.** The release day went into the first install
on a player's Windows PC and a long play session at 60 fps, with the port's own logs read after
each run. That found what no test machine had:
- **A crash that turned out to be a stale quick save.** A quick save is a snapshot of the running
  game's memory. Loading one made by an older build of `pcj.exe` left pointers into code that had
  moved, and the game crashed minutes later at the pause menu. Saves now carry a fingerprint of
  the program's layout, and another build declines them with a message.
- **The 60 fps ledge grabs.** A charged jump onto a high wall on the first level worked at 30 fps
  and never at 60. F7 was added so the same jump could be tried at both rates in one session, and
  the ledge catch logged frame by frame (`CJ_GRAB_LOG=1`). The log showed the ledge 0.76 above the
  hands at the top of the jump: inside the 30 fps window, outside the 60 fps one. Along the way a
  wrong first theory doubled the held-button jump boost at 60 fps (12-unit jumps); the logs caught
  that too, and the boost is the original's.
- **Text cut off by the PC keycaps** in the pause menu footer, and right-aligned numbers clipped
  in the chapter statistics.

## Tried and dropped

- **A widescreen squeeze for the menus.** The original stretches its 4:3 menus to fill a 16:9
  screen. Several approaches to restore their proportions were built and removed; menus are drawn
  exactly as the original.
- **A turn-and-flip move** (flipping forward out of a pivot). It did not feel right in play.
- **Scaling the held-button jump boost by frame rate.** It was already correct: it runs over a
  window counted with the frame-rate multiplier. Scaling it doubled the boost at 60 fps.

## Known issues

- Only the USA retail Xbox disc is supported (the installer says so for other releases and for the
  PlayStation 2 disc).
- Car and building lights appear to be drawn with an extra see-through "glow" copy of the lit windows,
  slightly larger and nearer the camera. On a 2005 TV it blurred into a halo; at high resolution
  it reads as a sharp double image. This is the original game's look; an option to soften it may
  come later.
- In puddle reflections the back of a car can show as plain white. Reported in play and not yet
  looked into, so it is not yet known whether the original game does the same.
- A quick save only loads into the build of `pcj.exe` that made it: after updating, start the
  chapter and save again.
- HD textures so far cover Scarface, his weapons, and the Jungle Hunter and City Hunter costumes.
