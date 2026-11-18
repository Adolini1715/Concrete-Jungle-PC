# Hotfix v0.2.1

A small fix-up release on top of Patch 1 (v0.2.0).

## Fixes

- **Ghosting on the main menu at higher Resolution Scale.** At 2x and above, the game's glow and
  blur split thin bright things (the menu rain) into rows of faint copies around the clock tower.
  Those effects now run at the original resolution, as they did on the Xbox, while the rest of the
  picture keeps your Resolution Scale.
- **Screen trails Off now removes all of the trail.** It used to stop only the first step of the
  glow. Every step of it now stops blending in the previous frame.
- **Armour lights in remaster conversions.** Dragging the PS2 remaster textures onto the launcher
  turned every costume's red armour lights into a dark banner print (a level texture had been
  matched to the light by mistake). That match is gone. If you already converted the remaster,
  drag it in again to rebuild the packs.
- **Dark Blade HD pack: the feet.** The HD texture the feet use had lost its spots and turned into
  a dark blob. The pack on archive.org is fixed, and Texture Packs in the launcher now offers a pack
  again when the copy you have is an older upload: download Dark Blade HD there to replace it.

## Updating

From v0.2.0: open the launcher and choose **Update**, or run `Update.bat`. Your saves, settings
and texture packs are kept.
