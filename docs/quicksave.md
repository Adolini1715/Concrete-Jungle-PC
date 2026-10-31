# Quick save / quick load

One quick save: F5 or clicking both sticks saves, F8 or pressing both bumpers
(LB + RB) loads (saves/quicksave.pcjs). The game does not see those button
pairs. Separately, the pause menu has Save Game / Load Game with five slots per
chapter (saves/chapter_<map hash>_slot<N>.pcjs; see "Pause menu" below).
A status line at the top of the screen (drawn into the frame, so it shows in
fullscreen) says what happened: "Quick saved", "Saved to slot 2", "Slot 3 is
empty". Saves from older builds (quicksave1..5.pcjs) become the quick save.

A quick save is a snapshot of the whole machine: the 64 MB of RAM, the
separate 64 MB contiguous window (D3D data, the game heaps; the player
object lives there), the CPU registers of the game thread, and the host side
of the emulated hardware that guest memory does not hold (push-buffer walk
and executor state, vertex shader state, the APU registers and voice lists,
the kernel/heap tables). It is taken at one safe point: the entry of
`EXBaseApp::Update` (0x1B97B0), which the main loop calls through the
vtable, so `recomp_lookup_manual` can put `qs_update_entry` in front of it.
At that point the game thread holds no locks and is between frames.

## Taking the snapshot

1. Park the GPU thread once it has consumed everything (`xbox_nv2a_pause`).
2. Take the mixer lock (`gpMixer+0x1C`) so xbMixerThread is between mixes.
3. Close the guest-thread gate (`xbox_snap_gate`): other guest threads (the
   file thread, the mixer) stop at their next kernel call; threads already
   waiting in a kernel wait count as stopped.
4. Lock the APU state.
5. Copy; unlock in reverse order.

A load does the same and restores everything except the stacks of the
worker threads that are still running (`xbox_worker_stacks`): they continue
their own code across the load. The TSC is offset so the game clock carries
on from the saved time instead of jumping.

## Rules

- Saving needs a loaded level with the player in control and no cutscene
  (or `CJ_QS_FORCE=1`). Loading needs a loaded level.
- A save only loads into the chapter it was made in (map hash
  `gCurrentMap_GeoHash`, 0x48E874). After dying, the game restarts the
  chapter itself; F8 then puts you back at the save.
- Each save is written to `saves/quicksave<slot>.pcjs` next to the exe
  (`CJ_QS_DIR` overrides; `CJ_QS_DISK=0` keeps saves in memory only) on a
  background thread, zero pages packed, about 60-80 MB. With nothing in
  memory (after a restart), F8 reads it if the build is the same (region
  list hash), the same chapter is loaded, and the worker threads' stacks are
  where they were; otherwise the title explains what to do.
- `CJ_QUICKSAVE=0` turns the feature off.

- Open files: the read position of every open file is saved, and on a load
  each file is put back in its handle slot at that position (reopened by
  path when the slot holds another file by then: a level reloaded since the
  save opens its files in another order). Without this a sequential reader
  (streamed audio, loading) could continue from the wrong place or file.
- PC Settings live in a guest-memory block that the menu binds to; a load
  points the settings code at the block's saved address and writes the
  current values into it, so a load never changes PC settings.
- Only the last saved or loaded slot is kept in memory; loading another slot
  reads its file.

## Testing

`CJ_QS_TEST="flip:action,..."` with actions `save`, `load`, `next` (slot), `kill` (player
health to 0 without the death path) and `die` (a fatal fall through
`XItemHandler_Player::InflictDamageFromFall`, the real death and chapter
restart). One frame after each load a `[QS] verify` line compares the
player position, health and energy with the saved values.

Results (Linux, route harness, chapter 1):
- save, load, load, save, load: all verify OK, run continues to the end.
- save, kill, load: health and position restored.
- save, die, load: the game restarts the chapter after the death, the load
  restores the save.

Known limits: loading from the front end or another chapter is refused;
guest APCs queued inside kernel waits and parts of the APU state are not
captured; a disk save from another session only loads when the worker
thread stacks land at the same addresses.

## Code

- `src/quicksave.c` (state, gating, disk format, test script)
- `patches/v0.15/0021-save-state-hooks.patch` (xboxrecomp: `kernel/xbox_snapshot.h`,
  region exports, GPU pause, guest-thread gate, worker stack list)
- `src/gl_backend.c` (keys, controller chord, status line `osd_draw` with `src/osd_font.h`
  from tools/art/gen_osd_font.py; `CJ_OSD_SHOT=<flip>` saves the finished frame)

## Pause menu

The pause menu's Quit page has **Save Game** and **Load Game** under Level Stats (mods/FEPause.cpp
grows the 4-row list to 6 the first frame). Choosing either opens a slot screen in the same panel:
"Save to which slot?" / "Load from which slot?" and five slots for this chapter with their ages.
B / Back or the title row goes back. Saving over a used slot asks for a second press; loading an
empty slot does nothing. Choosing a slot closes the menu (a Start press, `cj_pad_inject`) and the
save or load runs once the menu has been gone for 6 updates and the player is back in play
(`qs_after_pause`), so a save never captures the menu. The slot screen's six rows are a second
set of FEObjects swapped into the list's child array (`cjmod_pause_show`); the list is emptied
before building them because FELayoutFixedGrid::ObjectAdd deletes the child it replaces. The
quick save (F5 / F8) is a separate file.
