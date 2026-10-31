# 4:3 menus, text and HUD in widescreen

CJ_WIDESCREEN uses the game's anamorphic 16:9 mode. The Xbox design stretched
all 2D art across a 16:9 TV; src/widescreen_ui.c keeps it at 4:3 proportions:

- EXWnd sprite calls (hooked in recomp_lookup_manual): element mode (default) records the commands each FE item queues between two clip rects and narrows the group by 3/4 around its own centre, so items keep their place like the game's 3D logo does; CJ_WS_UI_MODE=centre squeezes everything around the screen centre instead. FEHud/FERadar windows are left alone (the original lays them out for 16:9). CJ_WS_UI_SCALE sets the factor.
  through the vtable, hooked in recomp_lookup_manual): x is squeezed by 3/4
  around the centre (or the nearer edge for a window in an outer third; in
  practice every game window is full screen, so the centre). Sprites spanning
  the whole width (fades, backdrops) stay wide. Covers text, HUD, menu frames.
- FEObjectScript::SetScale multiplies x by 1.33 in widescreen when the script
  has state 0x1000000 (all start with it): masked for the call (linker wrap).
- FEObjectScript::SetSize(w, h): w asked for at 3/4 unless w >= 600.

Result (cloud screenshots): menus, logo, text and HUD at 4:3 shape. Not yet:
the loading screen title card stays wide.

Off by default (the original stretched layout was preferred, 2026-10-25); CJ_WS_UI=1 in pcj.ini turns it on. CJ_WS_LOG=1 lists the windows seen.
