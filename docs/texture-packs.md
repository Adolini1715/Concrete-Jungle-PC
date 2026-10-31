# Texture packs

The port can write out every texture the game uses and load edited or higher
resolution replacements.

1. Dump: set `CJ_TEX_DUMP=1` (pcj.ini) and play through the parts you want.
   Each distinct texture is written once to `textures/dump/` next to pcj.exe as
   `<key>_<width>x<height>_<format>.png`.
2. Edit: copy the PNGs you want to change anywhere under `textures/` (its own
   folder per pack is tidiest) and edit them. Keep the first 16 characters of the name (the
   key); anything after it can be renamed. Any size works: draw at 2x or 4x for
   an HD pack. Transparency is kept (RGBA PNG).
3. Play: replacements load automatically (`CJ_TEX_REPLACE=0`, or Texture Packs
   in PC Settings, turns them off). Turn the dump off again; it costs time.

## Installing packs

Drop a pack into `textures/` next to pcj.exe, as a folder or as the `.zip` it
came in (no need to unzip). Every PNG named `<key>...png` in any subfolder or zip
is picked up; `textures/dump/` and `textures/glyphs/` are skipped. Packs load in
name order and where two replace the same texture the later name wins, so call
your own tweaks `zz_mine` to put them on top. The log lists each pack found.

Press **F3** in game to switch all texture packs off and on for a quick
comparison (the status line says which). It lasts for the session; the PC
Settings switch is the permanent one.

The key is a hash of the texture's format, size and pixels as stored on the
disc, so it is the same every run and on every machine. Video frames and the
effects the GPU renders each frame (glow, heat vision, shadows, radar layers)
are not textures here. `CJ_TEX_DIR=<folder>` moves the textures folder.
