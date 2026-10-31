# Determinism and frame-count based A/B

## Switches (src/determinism.c, no regeneration needed)

`EXApp::MainUpdate` (0x00217730) is only reached through the vtable and
`XBVerticalBlankCallback` (0x002238E0) only through D3D's callback pointer, so
both are hooked from `recomp_lookup_manual`.

| env | effect |
|---|---|
| `CJ_LOCKSTEP=K` | vblank lockstep: the callback stops counting, and `EXDisplay+0x980` (vblanks since last frame) is set to K before every MainUpdate (`+0x984` total += K). MainUpdate computes the game's frame rate as refresh/vblanks, so with K fixed every game frame has the same dt regardless of host speed. K=2 (30 fps game time) works. |
| `CJ_STOP_FRAME=N` | after the Nth MainUpdate: `cj_swap_report("final")`, 3 s for the GPU thread to dump the last flips, `_exit(0)`. Replaces the 50 s wall-clock watchdog as the end of a run. |
| `CJ_DET_HASH=1` | per frame `[DET] f=<n> data=<.data hash> heap=<0x00F80000..0x04000000 hash>`, and a swap report every `CJ_DET_REPORT` (16) frames. |

`hybrid_swap.c` now exports `cj_swap_report(tag)` (per target: calls, gen-body
entries, esp/csr failures, run hash) and tags every logged call with `f=<frame>`.

Compare two runs: `tools/det_cmp.py <tagA> <tagB>` (per-frame hashes, report lines,
first differing logged call, common flipped frames).

Standard run: `CJ_LOCKSTEP=2 CJ_STOP_FRAME=100 CJ_DET_HASH=1 RECOMP_WATCHDOG_SECS=240 tools/swap_ab.sh <tag> <0|1|VA,VA..> [bin]`
(about 70 s per run).

Toolkit switches tried: there is no virtual-time mode in v0.15. `RECOMP_GUEST_LOCK=1`
(+ `RECOMP_GUEST_QUANTUM=1000`) does not remove the boot heap race (3 runs, 2 heap
classes). `RECOMP_WORKERS=inline` cannot be used (both worker threads loop forever).
`RECOMP_RASTER_THREADS` is already 1 on this 2-core host.

## Result

Without lockstep (the old 50 s runs) the call counts at the end of a run depend
on host speed, and frames differ from about f45 on (h2off1 vs h2off2: 18/64).

With `CJ_LOCKSTEP=2`, to 100 frames:

- Call counts of every wrapped function are identical at every 16-frame report
  in all off/off and off/on pairs (4- and 8-target builds, 12+ runs).
- Flipped frames: ls2 vs ls3 (4-target, off) 101/101 identical. In general the
  frames are identical up to about f60 and can differ after f70 (t8off vs t8off2:
  31 differ, f70..f100). r1off vs ls2 differed from f2, so some heap classes also
  change earlier frames.
- `.data` and heap hashes differ from frame 1 in every pair.

### Residual: first divergence

The heap layout race happens before frame 1, on the main thread. A gdb trace of
`_EXAlloc`/`EXFree` (all calls are on the guest main thread) in two runs diverges at
call ~2289: the 0xF8-byte allocation from `EXAudio::psiInitialiseAudioMemory`
(0x002BD880) comes before or after allocations from `EXGeoHeader::LoadSectionFile`
and `EXFile::CreateMXFile`. The order depends on when the file thread (`xbFileThread`,
0x00284E70, polling `gFileManager.Update()` every 8 ms) finishes a read, because
`EXFileManager` runs the load callbacks as reads finish. Every later heap address
follows from that (AddMarker's `this` is 0x803439B0, 0x80343B40, 0x80343ED0 ... by class).
The `.data` hash also covers timing fields (XBBenchMark rdtsc timers,
`DEBUG_GetMillisecondTimer` load stats).

Next fix (needs one regeneration): make file I/O synchronous and ordered. Add
`XBFileManager::FileThreadWakeUp` (0x00284E60, direct calls only) to
`game/manual_functions.json` with a body that runs `EXFileManager::Update`
(0x002C8B00, ecx = &gFileManager) until it returns 0, park `xbFileThread` through
`recomp_lookup_manual` (it is started indirectly by `_XapiThreadStartup`), and pump
once more at each MainUpdate so loads that were queued without a wake-up still
progress. Then make `xbox_ReadTimeStampCounter` / `GetTickCount` frame-derived under
lockstep (a toolkit patch).

## Bug found on the way

`hybrid_swap.c` selected native mode with `e[0] != '0'`, and every VA list starts with
'0', so `CJ_NATIVE_SWAP=<VA list>` ran fully generated (the log said
`generated hybrid swaps: generated`). Only `CJ_NATIVE_SWAP=1` worked. Fixed:
the selection now tests `strcmp(e, "0")`. Round-1/round-2 results used `=1` and are
unaffected; any per-VA run before this fix is invalid.

## Full determinism attempt (CJ_DET, round 3)

Implemented (default off; without `CJ_DET` the build behaves as before, see the 7-target
A/B in docs/hybridize.md, which ran on this binary):

| piece | where |
|---|---|
| `CJ_DET=1` (implies `CJ_LOCKSTEP=2`), `CJ_DET_TIME=0` (synchronous I/O only), `CJ_DET_DEBUG=1` | src/determinism.c |
| `XBFileManager::FileThreadWakeUp` 0x00284E60 wrapped (sub_X_gen): under CJ_DET the guest main thread runs `EXFileManager::Update` (0x002C8B00, ecx=&gFileManager 0x004CE038) until it returns 0; a wake-up from another thread is only counted (the main thread pumps soon anyway) | src/det_file.c |
| `EXFileSys::Poll` 0x00221400 wrapped: main thread pumps first. Needed because `EXFileSys::WaitSyncOnFile` busy-waits on Poll with no Sleep and no wake-up (the only wake-up caller is XBAudio), relying on xbFileThread's 8 ms poll | src/det_file.c |
| `xbFileThread` 0x00284E70 parked (lookup hook; it is started through `_XapiThreadStartup`) | determinism.c |
| main-thread `KeDelayExecutionThread` (kernel thunk slot 75 = 0xFE00012C, reached through recomp_lookup_manual) pumps, then sleeps | determinism.c |
| pump before every MainUpdate | determinism.c |
| clock: rdtsc = frame-vblanks x 733 MHz/60 + 64 x (per-thread call count); KeQuerySystemTime = 2005-01-01 + frame time + per-thread count (100 ns); KeTickCount = max(frame x 33 ms, previous) + 1 ms per main-thread pump, and 0 at boot. Toolkit hooks in patches/v0.15/0003-det-time-hooks.patch (`xbox_det_tsc_hook`, `xbox_det_systime_hook`, `xbox_det_time` stops the timer thread writing KeTickCount) | determinism.c + toolkit |
| guest main thread = host main thread (main.c calls the XBE entry), recorded in a constructor | determinism.c |

Regeneration: the `--exclude-manual` input is now `cat build/hybrid/hybrid_swap.c
src/det_file.c > build/exclude_manual.c` (RUN.md). CMake option `CJ_DET_FILE` (ON) for gen
dirs made that way.

### Result: not deterministic yet

- `CJ_DET=1` with the clock hooks: boot never reaches frame 1 (3 runs, 150-240 s
  watchdogs). The main thread spins in `EXFileSys::WaitSyncOnFile` (stack: XApp::InitGame ->
  EXFileSys::AddFileList -> EXFileList ctor -> OnLoadFile -> WaitSyncOnFile, guest esp
  0x00F7F1F8) with pumps running. Not yet narrowed to rdtsc vs KeQuerySystemTime vs
  KeTickCount; the tick advance per pump did not fix it.
- `CJ_DET=1 CJ_DET_TIME=0` (synchronous I/O, host clock): d1nt reached frame 100 (721k
  pumps, 0 off-main wake-ups, 1 main-thread sleep pump), dA reached 131+ frames, but dB (run
  at the same time as dA) hung at boot in the same wait. d1nt vs dA: .data and heap differ
  from f1, call counts differ from the first report (FELayout::Update 12 vs 14 at f16;
  plain lockstep had equal counts in all pairs), frames differ from f9.
- So the synchronous file path alone makes things worse, not better: the boot wait
  still depends on another thread. First suspect: the sound thread.
  `WaitSyncOnFile` also calls `EXBaseSoundManager::CommandUpdate(1)` in the loop, and
  XBAudio is the one caller of FileThreadWakeUp. Under CJ_DET its loads are done only when
  the main thread pumps, and the audio thread may be holding state the main thread
  waits for (a deadlock in timing classes, which fits the 1-in-2 hang).

Next: (1) trace the hung boot (which EXFile, what its m_Status is, which thread owns the
sound manager's command queue); (2) give the audio thread's wake-ups a deterministic
service point, or run the audio/sound thread in lockstep with the main thread too
(the toolkit's RECOMP_GUEST_LOCK at frame granularity); (3) bisect the clock hooks
(rdtsc only / systime only / tick only) with CJ_DET_TIME extended to a bit mask.

## Round 4: boot hang fixed, mixer on the main thread, deterministic TIBs

### Root cause of the CJ_DET boot hang (not the clock, not audio timing)

`det_delay` (the main-thread `KeDelayExecutionThread` hook) cached the dispatcher returned by
`recomp_lookup_kernel(0xFE00012C)`. That function does not return a per-slot function: it
stores the slot in a thread-local (`g_kernel_dispatch_slot`) and returns the generic
`kernel_thunk_dispatch`, which reads the slot at call time. `det_delay` pumps the file
manager first, and the pump makes kernel calls of its own (RtlEnter/LeaveCriticalSection in
`FileQueue::GetActive`, file I/O), so the later `k()` ran the last kernel function the pump
had used instead of the delay. That function takes one argument and popped 8 bytes instead
of 16. The one main-thread Sleep during boot is `Sleep(1)` in `XBFileManager::Startup`,
called from `EXSoundManager::OpenSoundManager` right before `this->Initialise()`. With esp
off by 8, `OpenSoundManager` never called `Initialise` (checked with a lookup hook on the
virtual 0x00278F40: never entered in hung runs), so `m_pAudioStatus` (+0x748) stayed NULL.
`EXFile::GetFileLoadedStatus` then read the file status from guest address 0 (0x12, never
0x83), and `WaitSyncOnFile` waited forever on Filelist.bin (its AsyncFileData was already
closed and finished, `gpEXAudio->m_FileStatus[0]` = 0x83). Whether the pump made a kernel
call before that Sleep depended on thread timing, hence 1-in-2 / 1-in-3 hangs; gdb runs
never hung. Fix: look the slot up again right before the call (determinism.c).
Found with the dump script (gdb Python over `g_xbox_mem_offset`) and an in-process
hardware watchpoint (`CJ_DET_WATCH`, below).

Because of this, the clock bisection (step 2 of the plan) was not needed: the boot now
completes in every run with all three frame-derived clocks on (20+ runs, 2 at a time).

### Remaining thread races removed

| race | fix |
|---|---|
| `xbMixerThread` (0x002B2520, the only other game thread: lock, read DirectSound play cursor, `AudioMixer::Mix` 0x002C71E0, unlock, `Sleep(4)`) mixes by host time and holds the lock `CheckForStoppedVoices` takes | parked under CJ_DET (lookup hook on its start routine); its loop body runs on the main thread before every MainUpdate, 8 iterations with the play cursor advanced 200 samples each (48 kHz x 33 ms / 8). Same arithmetic as the original (played/mixed 64-bit counters at 0x004DE350/58, limits at 0x00472420/24, ring of 0x1000 samples at gpMixer+0x14). Host audio output is not meaningful under CJ_DET; game state advances by the same amount every frame. `CJ_DET_MIX=0` lets the thread run free. |
| a spawned guest thread allocated its TIB from the shared guest heap when it started (toolkit `bridge_thread_main`), racing the main thread's next heap allocation (xbFileThread's stack landed at 0x0118CFF0 or 0x0118DFF0) | toolkit patch 0004: the creating thread allocates it |
| host helper threads that run guest code (APU interrupt, OHCI, timer DPCs) allocated their TIBs from the same heap whenever they first ran; every later heap block (DirectSound buffers, MU device object, `gpMixer+4`, `MU_DriverExtension`) moved with them | patch 0004: with `RECOMP_HELPER_TIB_POOL=1` (CJ_DET sets it) they come from a 16-entry pool reserved on the loader thread; the heap hash skips the pool (0x00F80000..0x00F80A00) because its contents follow those threads |

### Result

Two CJ_DET runs at the same time (zA/zB, qA/qB, 7-target build, swap off):

- boot end (dump before MainUpdate 1): .data and the whole heap window byte-identical;
- frames 2..11 (zA/zB) and 2..13 (qA/qB): .data and heap hashes identical; call counts identical
  at every report; flipped frames identical (zA/zB 31/31, qA/qB 14 of 17);
- f1 heap: one word, 0x0108232C (0 vs 9), inside a DirectSound/APU block written by the APU
  interrupt thread; gone again by f2 (transient).

Residual (first real divergence, f11..f15): the intro movie. `xbMovieUpdate`
(0x00253290) -> `xbMovieCreateTexture` -> `XGSetTextureHeader` writes a texture header that
lies at g_MemCard+0x14C (0x004BECAC); one run wrote it at frame 11 (CJ_DET_WATCH), the pair
differs from f14/f15 on, and .data/heap/frames follow. `XMVDecoder::GetNextFrame` paces
video by its DirectSound stream (APU, host time) and reads through xbRAC (overlapped
ReadFile completions on toolkit threads), neither of which CJ_DET controls yet.

### Tools added

| env | effect |
|---|---|
| `CJ_DET_DUMP=n[,n..]` + `CJ_DET_DUMP_DIR=dir` | raw `.data` and heap window before MainUpdate n (0 = end of boot) to `dir/{data,heap}_f<n>.bin` |
| `CJ_DET_WATCH=<VA>` | hardware write watchpoint (perf_event, sigtrap, inherited by new threads) on one guest dword; each write prints tid, value, frame and a host backtrace (`m_<name>_<VA>` frames). Unlike gdb it does not change the timing enough to hide races |
| `CJ_DET_DEBUG=1` | also reports a main-thread Sleep's esp/esi and any callee-saved register the sleep pump changed |

### All 7 targets on vs off under CJ_DET (det7off / det7on, stopped at 43 common frames)

- call counts identical for all 7 targets at f16 and f32 (only `enter(gen body)` differs, as
  expected); esp_bad = csr_bad = 0;
- heap hash identical f1..f13 (diverges with the movie residual at f14);
- `.data` differs from f1: the run hashes of FELayout::Update (0x000DB970) and MathmosUpdate
  (0x000F97D0) differ (first differing logged call: MathmosUpdate #1, f=9, `pre` hash), the
  other five are equal. So the native and generated bodies of those two leave different bytes
  in `.data` (likely float rounding, x87 in the recompiled original vs SSE in the native
  build); not visible in frames f1..f13 (identical). Needs a per-call `.data` diff of those
  two functions next;
- frames: 43 common, f1..f13 identical, 29 differ from f14 (movie residual).

The 300-frame A/B was not run to the end: before the movie is deterministic it compares
two runs that already differ from f14, so it would not decide anything.

### TweakPosition (target 8): not done

Its gate (determinism reached) was not met. Prepared: tools/hybridize/targets8.txt (7 +
0x00107A90) and build-t8/hybrid + build-t8/exclude_manual.c (10 functions wrapped as
sub_X_gen). Next: `tools.recomp ... --gen-dir ../work/gen8 --exclude-manual
../build-t8/exclude_manual.c` (RUN.md), `cmake -S . -B build-t8 -DRECOMP_GEN_DIR=work/gen8
-DCJ_TARGETS=tools/hybridize/targets8.txt -DRECOMP_ABI_CHECK=ON`, then CJ_DET A/B to f13
(the deterministic window) and later to 300.

### Next

1. Movie: make `XMVDecoder::GetNextFrame` pacing and xbRAC reads deterministic under CJ_DET
   (e.g. complete overlapped reads synchronously, pace by frame count instead of the
   DirectSound stream position) or, if that fails, document a CJ_DET movie skip.
2. APU interrupt thread writes (the f1 transient at 0x0108232C) - harmless so far.
3. FELayout::Update / MathmosUpdate `.data` difference native vs generated.

## Round 5: movie deterministic, 300 identical frames, FELayout/Mathmos difference explained

### Movie (src/det_file.c, needs the regenerated gen dir; `CJ_DET_MOVIE=0` turns it off)

What the intro movie actually depends on (disassembly of xbMovieUpdate 0x00253290 and
`XMVDecoder_GetNextFrame` 0x003571A2):

- Video pacing is not the DirectSound play position: GetNextFrame compares the frame's
  presentation time against `D3DDevice_GetDisplayFieldStatus` (0x002EE440) VBlankCount, i.e.
  D3D device +0x1DE8, which the D3D vblank ISR increments on host time. The start time is
  `QueryPerformanceCounter` (rdtsc, already frame-derived under CJ_DET).
- With an audio stream enabled, the DirectSound stream adds host time twice: packet
  completion (status != 0x8000000A) gates fetching the next packet, and
  `XMV::SynchonizationCallback` (APU side) writes an audio offset into decoder+0xC8 that
  shifts every presentation time.
- The file reads are already synchronous under CJ_DET: xbRAC reads through
  `EXBaseSoundManager::StreamRead` -> command block -> `EXAudio::psiAsyncReadFile` ->
  `FileThreadWakeUp` (the main-thread pump); the toolkit's NtReadFile completes inline. No
  change was needed there.
- The first `xbMovieUpdate` busy-waits on GetNextFrame until a frame comes out, and the
  read-ahead only sends its read command when `GetTickCount` has moved 16 ms
  (`CommandUpdate(0x10)`). Both clocks are frozen within a CJ_DET frame, so a naive
  frame-derived VBlankCount hangs at frame 11 (first attempt, mA/mB).

Fix (three more `sub_X_gen` wrappers; the gen dirs were regenerated, 12 wrapped functions for
7 targets, 13 for 8):

| wrapper | under CJ_DET |
|---|---|
| 0x002EE440 D3DDevice_GetDisplayFieldStatus | VBlankCount = frame x lockstep vblanks + wait/16; after the 4th call within one frame each further call counts as 1 ms of waiting and advances KeTickCount by 1 ms (cj_det_tick_pump). Field parity from the count. |
| 0x00356BA9 XMVDecoder_EnableAudioStream | does nothing, returns S_OK: the movie plays without its audio stream, so it is paced by the vblank count alone |
| 0x00356D49 XMVDecoder_GetAudioStream | returns NULL for a stream that is not enabled (the original dereferences it); xbMovieUpdate's SetMixBins and close path test for NULL |

The movie plays (every flipped frame differs from the previous one, one movie frame per
game frame at lockstep 2). Host movie audio is silent under CJ_DET. Without CJ_DET the
wrappers call the generated bodies unchanged.

### 300-frame result

nA/nB (7-target build, swap off, CJ_DET=1, two runs at the same time): `.data` 300/300 and
heap 300/300 identical, all 133 swap report lines identical, flipped frames 301/301
identical, same pump/mix counts (659 pumps, 939 mixes). The goal is reached.

### FELayout::Update / MathmosUpdate difference: guest stack residue, not x87 vs SSE

Tools: `CJ_SWAP_DUMP=<VA>[,<VA>..] CJ_SWAP_DUMP_DIR=dir [CJ_SWAP_DUMP_N=8]` (raw `.data`
before/after each of the first N calls, generated into hybrid_swap.c) and
`CJ_DET_DUMP_CONT=1` (contiguous memory 0x80000000..0x84000000 with CJ_DET_DUMP).

- Neither function writes `.data` in its first 6 calls, in either mode. The `.data`
  difference exists before FELayout::Update #1 (f9), and already at the f1 dump (boot end:
  identical). Heap: identical on vs off for 40 frames.
- The f1 difference is 199 bytes in the sound command buffer (0x004E25D8.., written by
  `XB_EXSfxCmd_Parser::SendBatchCommands` via CommandUpdate): guest stack addresses,
  off = 0x00F7F0AC, on = 0x00F7F0B0, everything shifted by one word. Commands are built
  with fields the game never initialises, so they carry stack residue. Contiguous memory
  shows the same pattern (file-name buffers, FE data), which is what FELayout's
  pointer-following run hash picked up.
- Cause: the original EXBaseApp::Update (0x001B97B0) starts with `push esi`; the native
  adapter called its body without it, so all of its guest callees (the whole game frame)
  ran 4 bytes higher on the guest stack and read differently shifted residue.
- Fix (tools/hybridize/hybridize.py `orig_prologue`, runtime `cjg_push_reg`): each cjn_
  adapter repeats the original's leading `push reg` / `sub esp, imm` on the guest stack
  (same register values the original saves) and restores esp after. Now in the adapters
  of EXBaseApp::Update, FELayout::Update (push esi), MathmosUpdate (sub esp,8; push esi),
  ScriptAdd (push ebx/esi/edi). `HY_PROLOGUE=0` turns it off. The FEScript targets start
  with other instructions and are not mirrored yet.
- Result (pOff/pOn, all 7 on vs off, 40 frames): `.data` 40/40 and heap 40/40 identical
  (was: `.data` differs from f1), FELayout::Update run hash now equal. MathmosUpdate's run
  hash still differs: call #35 has equal pre and post, call #36 (f9, this=0x80348E40)
  already has a different *pre* hash, so the difference arises between the two calls,
  outside MathmosUpdate, in contiguous memory behind its pointers (neither .data nor heap).
  No call has been seen where equal pre-state gives a different post-state, so there is no
  evidence of an x87/SSE rounding difference, and -mfpmath=387 was not tried. Most likely
  the same residue mechanism through a target whose prologue is not mirrored (the FEScript
  ones) or through native locals that the original keeps in its own stack frame.

### TweakPosition (target 8): passed, promoted

work/gen8 regenerated (8 targets + 5 CJ_DET wrappers = 13 `sub_X_gen`), build-t8 rebuilt
(before the prologue fix). t8off (`CJ_NATIVE_SWAP=0`) vs t8tp (`=00107A90`, only
TweakPosition native), CJ_DET, 300 frames: `.data` 300/300 identical, frames 301/301
identical, every swap report line identical apart from the gen-body entry counts
(TweakPosition 25 calls, same run hash f6a3f2bd... in both), esp/csr 0. Heap: 299/300; the
one difference is the known f1 transient word written by the APU interrupt thread, before
TweakPosition is first called (f9). It is a run-to-run difference, not caused by the mode.

Promoted: tools/hybridize/targets.txt now has the 8 targets (the 7-target list is in
work/targets7.txt), src/recomp/gen = work/gen8 (the 7-target gen with the movie wrappers is
in work/gen7m). After changing targets.txt, rerun `cmake -S . -B build`: the target list is
read at configure time. Sanity A/B of the promoted build/pcj (with the prologue fix), all 8
on vs off, CJ_DET, 40 frames (e8off/e8on): `.data` 40/40, frames 41/41, heap 39/40 (the f1
transient), final call counts all equal, only MathmosUpdate's run hash differs (above), no
FATAL/[HYB].

### Next

1. Mathmos #36: dump contiguous memory around 0x80348E40 before calls #35/#36 in both modes
   (CJ_DET_DUMP_CONT at f9 plus a per-call dump of `this` + pointees) to find the writer;
   then extend `orig_prologue` past the first non-push instruction (or reserve the
   original's full frame size).
2. 300-frame all-on vs all-off with the prologue fix (only 40 frames run so far).
3. 300-frame all-on vs all-off on the promoted 8-target build/pcj.
4. The f1 APU-thread heap transient (0x0108232C) is the only remaining run-to-run difference.

## Round 6: stack hygiene (CJ_DET only)

Round 2 of docs/promote.md tried to reproduce the original's exact guest stack residue per call site
(return VA, esp depth, late saves, callee-saved register values); 281 of 521 call sites stayed unresolved.
That approach is now secondary. Instead, **under CJ_DET every guest stack allocation starts zeroed**, in the
recompiled code and in the native code alike, so a read of an uninitialised stack word (the sound command
buffer at 0x004E25D8 is the known consumer) yields 0 no matter which function, native or recompiled, ran
there before.

**Scope, stated plainly:** this changes the original game's uninitialised-stack contents, but only in test
mode (CJ_DET), and identically for recompiled and native code, so an A/B comparison under CJ_DET still
compares like with like. The normal build is unaffected: without CJ_DET the switch is 0 and the lifted code
only pays one predictable branch per esp-lowering instruction.

Mechanism (chosen over "zero the dead area below esp at every call/return": that needs a bound on how deep
the dead area goes, runs at every call, and would also wipe the pushed return VA / arguments region between
a call and the callee's prologue; zeroing at the allocation is exact, cheap and needs one regeneration):

| piece | what |
|---|---|
| toolkit patch `patches/v0.15/0005-det-stack-hygiene.patch` | lifter.py: every instruction that writes esp explicitly (`sub esp,N`, `add esp,-N`, `and esp,-A`, `mov esp,r`, `lea esp,[..]`, `xchg r,esp`) is bracketed by `RECOMP_ESP_MARK(); ... RECOMP_ESP_LOWER();` (recomp_types.h), which zero `[new esp, old esp)` when `g_recomp_stack_zero` is set and esp moved down by at most 1 MB. This covers `sub esp` frames and the `_chkstk` probe (0x002DA560, `xchg eax,esp`). push/pop/call/ret are not bracketed: they write what they allocate. `recomp_stack_zero` / `g_recomp_stack_zero` live in xbox_memory_layout.c. |
| src/native/hybrid/guest_rt.c | `cjg_set_esp` zeroes when it lowers esp: covers `cjg::Frame` (both forms), the adapters' mirrored `sub esp, imm` (orig_prologue/orig_frame, so the Area form's locals are zero too) and the call-site depth shift in `cjg_call_raw` (zeroed before the late saves are written) |
| src/determinism.c | CJ_DET sets `g_recomp_stack_zero = 1` in its constructor (before any guest code); `CJ_DET_STACK0=0` turns hygiene off |
| `CJ_SITE_PARITY=0` (guest_rt.c, runtime) | turns off round 2's call-site parity (original return VA -> 0xC0DE, no esp depth, no callee-saved register setup, no late saves). Default stays on (1); see docs/promote.md round 3 |

Regenerated: work/gen-batch (build-batch) and src/recomp/gen (build), both with the patched lifter (about 210 s each).

### Hygiene is itself deterministic

- Two runs at once, build-batch, core targets on, 300-frame attempt: both hit the 800 s watchdog (the
  concurrent rebuild of build/ slowed them), at 282 and 286 frames. Over the 282 common frames `.data`,
  heap (including f1, where the APU word transient used to be), all 282 images and every swap report are
  identical. Logs: work/promote/hyg-300/.
- Two runs at once, 150 frames (promote.py test baselines): rc 0, `.data`/heap/images/report lines identical
  (266 s). The game boots and runs the intro and front end normally with hygiene on.
- Not done: a swap-off pair (all generated) to 300 frames; the core-on pair above already exercises both the
  lifted and the native zeroing paths.
