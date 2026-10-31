# Frame pacing and background file writing (2026-10-26)

## What the 60 fps test logs showed
- ~5% of gameplay frames at 19-23 ms, followed by ~13 ms frames: EngineX's vblank gate.
  EXApp::MainUpdate returns at once until EXDisplay::m_VerticalBlanks (display+0x980) reaches
  60 / max rate; the vblanks come from the kernel's 60 Hz schedule, unrelated to when a frame
  finishes. A 20 ms frame starts the next one at the following vblank, 13 ms later.
- Multi-frame 20-50 ms stalls at the moments of the automatic "effects" bookmarks: each fired
  8 screenshots plus per-draw logging, written synchronously into the OneDrive folder.
- The log (~120 KB/s) and gamelog files were written and flushed on the game thread.
- Steady exceptions: ~1,700 access faults/s + ~250 single-steps/s, the emulated APU's trapped
  registers (goes away with the DirectSound replacement).
- Game thread 100% busy: the gate spins; 16% of samples in `__emutls_get_address`.

## Changes
- `src/frame_pacer.c` (CJ_PACE, default on; off in CJ_DET): in the D3DDevice_Swap wrapper,
  wait until last present + 1/cap (high-resolution waitable timer, then spin), never catch up;
  after Swap set m_VerticalBlanks to the elapsed vblanks (rounded, >= 60/cap, <= 4) so the gate
  passes at once with the true count. The kernel vblank callback stays (no starvation risk).
- `src/async_io.c` (CJ_ASYNC_IO, default on): pcj-log.txt, frames.csv and events.jsonl are
  pipe-backed FILEs drained by background threads; screenshots saved by a worker thread;
  drained at exit and before the crash handler terminates.
- Auto bursts are opt-in: CJ_AUTO_BURST=1.
- frames.csv: new columns work_ms (previous Swap end to this Swap), wait_ms (pacer),
  present_ms (Swap itself: remaining draws and the GL present).
- CMakeLists: winpthread linked explicitly in the Windows build.

## Verified
- Linux route to the first level with the pacer on; chapter-1 recording replays 25/25.
- Windows build under Wine: boots to the menus; log/frames/events written via the pipes.
- Not measurable in the cloud (software GL: 6-16 fps in levels, ~12 in menus): the real test
  is a 60 fps session on a test PC. Read work_ms/wait_ms/present_ms for slow frames.
