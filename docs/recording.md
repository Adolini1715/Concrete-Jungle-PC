# Session recording and exact replay

`CJ_REC=1` (PLAY-REC.bat) records a play session so it can be run again exactly, here or in
the cloud; `CJ_REC_PLAY=<dir>` replays one. src/cj_rec.c, src/determinism.c, src/cj_pad.c.

## Why a special mode
Normal play cannot be replayed: the frame length comes from the host clock, file reads finish on
another thread and the mixer thread runs free, so an open-loop replay drifts > 10 units within
2 s. A recording session runs in the deterministic mode (CJ_DET, docs/determinism.md): synchronous
file I/O, clocks derived from the frame count, a fixed number of vblanks per frame (1 at
CJ_FPS=60, else 2). It is paced to real time (each frame lockstep/60 s; a late frame is not
caught up). Then the pad state at every port-0 poll is the whole input.

The decision is made in determinism.c's constructor (before main: the clock hooks must be in
place for the first rdtsc, and the helper TIB pool is reserved at load), so CJ_REC must be in the
environment (the .bat sets it), not only in pcj.ini.

## Files (logs/rec-<date>-<time>/)
- `input.pcjr`: 80-byte header (magic PCJREC01, version, lockstep, row size, build id), then 18
  bytes per poll: buttons u16, 8 analog buttons, lx ly rx ry s16.
- `save/UDATA`, `save/TDATA`: the save folder at boot.
- `hashes.txt`: `<frame> <heap hash>` every 60 frames (guest heap window minus the helper TIB
  pool; .data also holds host-specific values and is not compared).
- `env.txt`: settings the replay must use, written at record time (`CJ_MODS=` when it was set);
  tools/det/replay.sh exports them unless the caller sets them. The native mods are part of
  the simulation: the modern camera (Camera mod) and the movement dials change it on purpose,
  so a recording made with mods off replays with that set (the fixtures made with mods off
  carry `CJ_MODS=` every mod except the camera and the front-end menu mods, which change
  behaviour on purpose).
- `qs.txt`: quick saves, loads and slot changes, `<poll> <request> <slot>` (1 save, 2 load,
  3/4 previous/next slot), as the safe point acted on them (src/quicksave.c).
- `qs-load-<poll>.pcjs`: for a load whose save came from disk (made before the recording, or in
  another slot), a copy of that save, so the replay does not need the player's save folder.

## Quick save and load inside a recording (2026-10-27)
F5/F8, the pad combos and CJ_QS_TEST work while recording. The replay ignores those triggers and
runs each logged request at its poll instead: a save stays in memory (the player's slot files are
never written by a replay), a load uses the session's own save or the qs-load copy. Saves and
loads happen at the same point of the same frame, so the heap hashes keep matching through them.
Verified: a recording with 2 saves and 3 loads (one refused, other chapter) replays with every
hash matching (run/qsrec, tools/det/replay.sh).

## Replay
Fresh save folder + the recording's save/ copied over it, then
`CJ_REC_PLAY=<dir> ./run.sh` (CJ_REC_REALTIME=1 paces it; CJ_REC_EXIT=1 exits at the end with
status 3 on divergence). Logs `[REC] frame N matches`, `[REC] DIVERGE at frame N`, and a summary.

## Every recording at once
`tools/det/soak.sh [linux|wine] [dirs...]` replays tests/fixtures/rec-* (or the given
recordings) one after another and prints PASS / DIVERGE / HANG per recording
(run/det/soak.txt); exit status 1 on any failure. Environment passes through, so
`CJ_MODS=... tools/det/soak.sh` checks a set of native swaps against every recording.
Speed: draws are skipped (NODRAW=1; the heap hashes do not depend on them, verified on all four
fixtures) and linux recordings run one per core (JOBS). All four fixtures: 192 s on 2 cores, was
about 21 min (one at a time, drawn in software, progress polled once a minute). NODRAW=0 brings
back the frame dumps for looking at a replay.

## Same results on Windows and Linux
- third_party/openlibm-cj: sin, cos, tan, sincos, atan2, log2, expm1 (the recompiled x87 code)
  and the float versions are linked into both builds, so glibc, MinGW and Wine/Windows msvcrt
  rounding differences cannot split them.
- Settings that change the simulation must match (CJ_FPS, CJ_WIDESCREEN ...): a first
  cross-platform test diverged only because the Wine run read a pcj.ini with CJ_FPS=60 and
  widescreen on.
- Verified 2026-10-26: Linux vs Windows build under Wine, same pad script, 500/500 frames identical
  .data and heap hashes and pixel-identical frame dumps; a Linux recording (1600 frames into the
  first level with movement) replayed on Linux: 25/25 checkpoints, and by the Windows build
  under Wine three times: 25/25 each.

## Known limits
- XMV movies play silent in the deterministic mode (CJ_DET movie wrappers, det_file.c).
- The emulated APU still runs on its own host thread; no divergence from it seen so far.
- Two hangs of the Windows build in the deterministic mode under Wine (frames 53 and 906, with
  other heavy runs on the same 2 cores); not reproduced in four later Wine runs. Real Windows
  untested.
