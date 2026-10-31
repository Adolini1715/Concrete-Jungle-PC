Sharp UI texture pack
=====================
High-resolution redraws of the front end's flat 2D art. Nothing here is a
filtered upscale of the original pixels; the originals only supply positions.

  8662cbf2181cf340  menu font (bold)        rebuilt 4x from Liberation Sans Bold
  531ef27bd120d318  title font (bold italic) rebuilt 4x from Liberation Sans Bold Italic (+weight)
  9594b0b78805ac05  small font               rebuilt 4x from Liberation Sans Bold (+weight)
  395ad90729c73c4e  save-slot frame          traced to vectors, redrawn 4x
  3b59f8081124d76c  box frame                traced to vectors, redrawn 4x
  1e8915c2572c0f4e / 248359337bd916d5 / d56d95b022fd4e65   X / A / B buttons, drawn at 128px
  2aea95cb700e0049 / 54b06f707c177670                      d-pad (plain / Scroll), drawn at 256px

In-game HUD:
  93f9cab9e029acd6  HUD font                 rebuilt 4x from Liberation Sans Bold
  2ed2e074fb667bc3  radar ring               traced to vectors, redrawn 8x
  c81879552d133bb1 / 4b1408a4f24be74b  health/energy/weapon panels and their backs, 8x
  971064eb47f72b46  energy bolt, 8x;  8c9d3a9be7a4eacd  radar N, 8x

Each glyph sits exactly where the original glyph's ink was, so line breaks,
alignment and spacing are the game's own (including its uneven letter spacing
in some words, which comes from the font metrics, not the texture).

Install: copy the PNGs into textures\replace\ next to pcj.exe (subfolders are
fine). Rebuild: tools/art/build_sharp_ui.sh <CJ_TEX_DUMP dump folder>.
Not included: the PREDATOR / Concrete Jungle logo (96ee9703a985c1ba).
Best with Resolution Scale 2 or more.
