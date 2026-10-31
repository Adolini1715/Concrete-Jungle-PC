# Button prompts

The game's text names buttons with `~X` codes (`~E Select`, `~F Back`, `Press ~J to
continue`), drawn as 16x16 Xbox icons. The port draws PC prompts instead
(src/glyphs.c):

- **Keyboard keycaps** (Space, Enter, Esc, Shift, Q, E, R, F, C, Z, arrows, WASD, a
  mouse) after a key is pressed, matching the keyboard map in src/gl_backend.c
  (`host_input`).
- **Modern controller icons** after a pad input: A/B/X/Y, LB/RB for White/Black,
  LT/RT, Menu/View for Start/Back, LS/RS for the sticks, a d-pad. Drawn at 64 px,
  so they stay sharp at 4K.

They switch live with the last input. Options > PC Settings > PC Button Prompts
(pcj.ini `CJ_GLYPHS`, read at startup) turns them off for the original icons.
`CJ_GLYPHS=kb` or `=pad` pins one set.

## How it works

- Every 2D sprite texture passes `EXWnd::_SelectSprite2DTexture` (0x0024DDD0,
  always through the vtable), wrapped in src/recomp_manual.c. `glyph_seen` reads
  the EXTexture's hash (+0x18) and D3D texture (+0x4C) and, for a button icon
  (XGamePad `GraphicArray` at 0x398818, 56 slots of 0x10: texture hash, text
  hash, width scale), notes where its texels are. Its neighbours in the file's
  texture array (0x50 bytes each) are noted at the same time.
- The GL back end (`decode_bind`) asks `glyph_override(offset)` before binding a
  texture and binds our art for a noted icon address.
- Each slot's width in text is its `m_Scale`; `glyph_input` sets it to the art's
  aspect ratio every frame, so wide keys lay out correctly.
- `~E`/`~F` (the menus' Select/Back) share A's and B's textures but mean Enter
  and Esc on a keyboard, so while keycaps show those slots point at spare
  textures the Xbox build carries but never draws (PS2 cross/square).
- `~J`/`~K` (START/BACK) and `~M`/`~O` (left/right thumbstick) are words on
  Xbox. Their slots are pointed at spare textures (the unused Xbox Start/Back
  art, PS2 L1/R1) with the text hash cleared, before the text is loaded, so the
  text decoder leaves the code for the icon renderer.
- Two executable literals name pad buttons directly: "~B Back" and "~V Scroll
  ~A Select". They are patched to ~F and ~E at startup.

## Art

`tools/make_glyphs.py` draws every image with Pillow (DejaVu Sans Bold) and writes
src/glyph_art.h. `--png DIR` also writes the PNGs for a look.
`CJ_GLYPH_DUMP=1` writes the game's own icon textures to textures/glyphs/ (make
the folder first).
