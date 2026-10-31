# Options > PC Settings

A fifth row on the main menu's Options page opens a page built by the game's own
front-end code, so it looks, sounds and navigates like the other Options pages.

| Row | pcj.ini key | Widget | Applies |
|---|---|---|---|
| Widescreen | CJ_WIDESCREEN | checkbox | live (title flag + window aspect) |
| Fullscreen | CJ_FULLSCREEN | checkbox | live (F11 / Alt+Enter stay in sync) |
| V-Sync | CJ_VSYNC | checkbox | live |
| Texture Packs | CJ_TEX_REPLACE | checkbox | after a restart |
| Frame Rate | CJ_FPS | 30 / 60 | live |
| Resolution Scale | CJ_GL_SCALE | 1-4 | live (render targets are rescaled at the next flip) |
| Mouse Look | CJ_MOUSE | checkbox | live |
| Mouse Speed | CJ_MOUSE_SENS | slider 2-50 | live |
| Invert Mouse | CJ_MOUSE_INVERT | checkbox | live |
| Modern Camera | CJ_CAM_MODERN | checkbox | live |
| Camera Follow | CJ_CAM_FOLLOW | checkbox | live |
| PC Button Prompts | CJ_GLYPHS | checkbox | after a restart (docs/button-prompts.md) |

The page shows 9 rows and scrolls. A spinner or slider draws itself even when
scrolled out of view (FELayoutSlider::Draw resets the clip rectangle), so those
rows sit at 4-8, which every scroll position shows. Keep that in mind when adding
rows.

Changes are written to pcj.ini next to the exe about 1.5 s after the last
change: the first `KEY=` line for each setting (active or commented out with `#`
or `;`) is replaced, missing keys are appended. Other lines are kept. On Linux
the file is `$CJ_PC_INI` (no saving when unset). A value set in the
environment (run.bat) still wins at startup, as before.

## How it is built

- `mods/FEMainMenu.cpp` replaces `FEMainMenu::UpdateStateOptions` (0xE9A00,
  adds the row, data 0x5043) and `UpdateStateOptionsAudio` (0xEAAD0). The PC row
  sets a flag and opens the Audio state (0x10); with the flag set the Audio
  handler builds the PC page instead. Without it both are the originals.
- Footer hints are the stock ones (Toggle for checkboxes, Change for sliders
  and spinners). The Audio state's page is built once and cached by the menu
  (bit 0x10 of `m_Unk1fc`); picking the other variant clears that bit so
  `UpdateState` deletes and rebuilds it.
- The widgets are bound (`FEObject::Bind`) to a 4 KB guest-heap block the mod
  allocates once and hands to `src/pcsettings.c` (`cjmod_pc_attach`). The
  block also holds the strings and column widths, since host data cannot be
  passed to guest code. `pcs_tick()` (once per flip) syncs the block with the
  native values, exports the env var, applies and schedules the save;
  `gl_backend.c` (`pc_settings_apply`) does the window, V-Sync and scale work.
- tools/hybridize gained `statics.csv` (VAs of the unit's file statics
  `s_OptionsGrid` 0x48DE48 and `s_OptionsAudioPage` 0x48DE50, which other
  Options pages read) and support for a constructed temporary passed straight
  into a by-value argument (the callee destroys it, MSVC rules), which the
  original `new FEObject(..., FE_Blank, ...)` calls need. Two translator bugs
  were fixed on the way: a free function returning a non-POD class (EXString
  from `Text_Get`) now uses the hidden result pointer, and a returned `T*&`
  (EXFixedArray/EXPtrArray `operator[]`) is viewed as a 4-byte `GuestPtr<T>&`
  instead of an 8-byte host pointer. The second also affected the existing
  EXLightManager swap (0x27E4A0), whose `= 0` cleared the neighbouring slot.

Under the Xvfb route harness a single d-pad press on these lists also
activates the row (the original code does the same, checked with CJ_MODS=0),
so `to_pcsettings.py` walks Controller, Camera, Audio, Game, then PC Settings.

Testing: `tools/to_pcsettings.py TAG items...` drives a running menu route to
the page; `tools/live.py TAG items...` sends presses and grabs frames
(`shot:NAME` -> run/TAG/shot_NAME.png).

`CJ_PC_TEST="flip:KEY=val,..."` changes settings at those flips the way the
page does (live paths testable without the menu). Verified this way in a
level, Linux and the Windows exe under Wine: Resolution Scale 1/2/3 (all
surfaces rescaled, no broken targets), Frame Rate 30/60, Widescreen on/off
(the HUD layout follows both ways). pcj.ini saving only rewrites keys that
were changed, keeps each line's ending (CRLF or LF), adds its header once,
and flushes a pending save at exit.
