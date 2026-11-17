# Patch 1 (v0.2.0)

The first update to the Predator: Concrete Jungle PC port. It adds a launcher, makes updating a
one-click job from here on, fixes the bugs reported since v0.1, and makes the game a good deal
faster.

## Read this first

- **Quick saves and pause-menu saves from v0.1 will not load.** They are snapshots of the running
  game and only load in the build that made them. Your chapter progress, unlocks and settings (the
  game's own save) are kept. Finish or abandon a mid-chapter save before updating.
- **If anything looks wrong after updating**, open the launcher, go to PC Settings and turn on
  **Compatibility mode**. It switches this patch's renderer speedups off. Then please tell us
  (Report a Problem in the launcher makes a zip to attach to a GitHub issue).

## Updating from v0.1

v0.1 had no updater, so this one time you bring it in yourself. Close the game first.

1. Download **Update-from-v0.1.bat** from the release page.
2. Put it in the folder you unzipped v0.1 into (the one with `Install.bat`). Your
   `Predator Concrete Jungle PC` game folder works too.
3. Double-click it. If Windows says it protected your PC, choose **More info**, then **Run
   anyway**.

It downloads this release, rebuilds only what changed (about 10 minutes; the build tools are
already there) and moves your game folder, with its saves, settings and texture packs, inside the
install folder. The desktop shortcut is replaced with one that opens the launcher.

By hand, if you prefer: download **Concrete-Jungle-PC-v0.2.0.zip**, open it, drag everything in it
into the folder you unzipped v0.1 into, choose **Replace** for every file, and run `Install.bat`.

If you deleted the v0.1 install folder: unzip this release where the old one was, next to your
`Predator Concrete Jungle PC` folder, put your disc in `PUT YOUR GAME HERE` and run `Install.bat`.
It finds the old game folder and keeps your saves and settings. This is a full build (about an
hour).

From now on, keep that folder: **Update** in the launcher (or `Update.bat`) fetches the latest
release and rebuilds for you.

New players: follow the README. Nothing about a first install has changed except that the game
now lives inside the folder you unzipped.

## New

- **Launcher** (`launcher.exe`, and the desktop shortcut now opens it): Play, Update, PC Settings,
  Texture Packs, Check for Updates and Report a Problem. It is drawn with the game's own logo and
  loading-screen art, read from your disc, and the game, launcher and shortcut get an icon made
  from the menu's red clan glyph (also read from your disc; issue #5).
- **HD texture packs from the launcher**: Texture Packs lists the packs available to download
  (Scarface, Weapons and Gear, City Hunter, Alien Hunter, Ritual, Dark Blade and Bad Blood),
  one click each, checked against a known hash. Packs you already have can be switched on and off
  there.
- **Drag and drop mods onto the launcher** (issue #4): drop a texture pack for this port, or a PS2
  texture mod made for PCSX2 (a folder, zip, 7z or rar), on the launcher window. PS2 mods are
  converted into packs you can switch on and off: PS2 alpha fixed, each texture resized to the Xbox
  shape, Xbox transparency used where the PS2 cutouts were wrong. It knows the 1059 PS2 textures
  the "Definitive Edition" remaster replaces; the remaster itself comes out as separate modules
  (levels, characters, props and UI, each costume, and an Xbox-colours alternative that is off by
  default). Multi-part rars need 7-Zip installed, or unpack them yourself and drop the folder. The
  packs are for your own use: don't share them, point people to the mod's page.
- **Report a Problem** collects the game's logs into one zip in the install's `logs` folder. Logs
  never contain your Windows user name.
- **Compatibility mode** and an optional **Native renderer (experimental)** toggle in PC Settings.
  The native renderer draws from the game's own Direct3D calls instead of its GPU command stream.
  It may run faster. It is off by default; turn it off again if anything looks wrong.
- **Controller only** (PC Settings): the keyboard and mouse never control the game, so a click
  while taking a screenshot cannot fire a weapon. Menus and the F-key hotkeys still work. F12 takes
  a screenshot without leaving the game.
- **Screen trails** (PC Settings, on by default): the game's glow and soft screen blur mix in the
  last few frames, which leaves a ghost of everything when the camera moves. Turn it off to keep
  the glow without the ghost.

## Fixed

- **Music was silent during most chapters' intro cutscenes** and only started with gameplay. A
  request to start a track that arrived while the previous one was still fading out was dropped.
- **A tap of Jump sometimes gave a fully charged jump** after chaining charged jumps, or when Jump
  was pressed again just after letting go of a running charge.
- **The Predator could vanish and the game carry on without him** after being blown back by an
  explosion (seen with the depot's power sources). He is now put back where he was hit.
- **Settings and controls reset when a save was loaded.** Once you change a setting, loading a
  save no longer puts the save's old settings back.
- **Stutter the first time an effect appears**: compiled shaders are now kept on disk, so each
  one only costs time once, not once per session. (Shader cache, on by default.)
- The HUD energy count no longer slides left out of its field.
- A boss's health bar shows when the boss takes damage, not only while you are locked on.
- Rain streaks are drawn again. The renderer dropped every line the game drew, so the rain was missing
  from the title fly-in, the menu and rainy levels.
- New controller buttons for quick save and quick load: hold both bumpers (LB + RB) and press d-pad
  up to save, d-pad down to load. Clicking both sticks and pressing both bumpers alone no longer do
  anything, so they cannot fire by accident. Pressing or letting go of the bumpers for the combo no
  longer reaches the game as a bumper press (which used an item).
- In menus, the fixed menu keys ignore your key bindings (Enter bound to an action no longer
  switches tabs), and a left click while rebinding cancels instead of binding Mouse Left.
- With mouse look on, a menu no longer loses the pointer while you are still using it.
- Installer: clearer errors when the download or unzip went wrong, only real compile errors are
  shown, and the desktop shortcut works when the Desktop folder is redirected (OneDrive).

## Faster

Measured on the same route through the first level, the CPU side of the game runs about 74%
faster than v0.1 (40.5 against 23.2 frames a second on our two-core test machine with drawing
switched off). What changed:

- The game's audio hardware emulation no longer takes thousands of trapped exceptions a second.
- The Xbox GPU command stream is decoded in blocks instead of one word at a time, and vertex data
  goes to your graphics card as the game stored it, converted there instead of on the CPU.
- OpenGL state, textures and shaders are only set when they change, and the buffers that carry
  each frame's geometry stay mapped instead of being locked and unlocked for every draw.
- HD texture packs are decoded on background threads from the moment the game starts, so a level
  no longer freezes while its replacements load.
- At high Resolution Scale, the depth buffer is shrunk on the GPU before the game reads it back.

Your mileage depends on your graphics driver. If this patch is slower or draws something wrong
on your PC, Compatibility mode puts the old paths back, and we would like to hear about it.

## Known issues

- Only the USA retail Xbox release is supported.
- Quick saves do not survive updates (see above). Making them survive is planned.
- The native renderer has been checked in the first chapters only.
- The white puddle reflection and the car-glow quads in the first mission's intro are in the
  original game too.
