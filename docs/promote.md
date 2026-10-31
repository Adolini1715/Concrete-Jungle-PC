# promote.py: batch promotion of byte-matched functions to native

`tools/promote.py` turns "which byte-matched functions can run as native C++ without changing
anything the game can observe" into a scripted pipeline. It builds on `tools/hybridize/hybridize.py`
(translation), the checkpoint 1 wrapper (`sub_X` / `sub_X_gen`, `CJ_NATIVE_SWAP`) and the CJ_DET
frame-count determinism (docs/determinism.md). It makes no git commits.

## Pipeline

    python3 tools/promote.py scan                    # 4 min: hybridize scan -v over all 216 units -> work/promote/scan.txt
    python3 tools/promote.py reach                   # 9 min: CJ_DET boot to frame 300 under gdb tbreaks -> work/promote/reached.txt
    python3 tools/promote.py compilecheck --jobs 2 --only work/promote/reached.txt
                                                     # per unit gen + `c++ -fsyntax-only` with the real flags,
                                                     # bisects failing units -> work/promote/clean.json
    python3 tools/promote.py batch -n 120            # ONE regeneration: targets-batch.txt (core + 120), tools.recomp -> work/gen-batch,
                                                     # cmake + build in build-batch/
    python3 tools/promote.py test --reuse-base       # baselines, then peel/bisect; writes tools/hybridize/promote-results.{json,csv}
    python3 tools/promote.py promote --regen         # PASS -> targets.txt, regen src/recomp/gen, rebuild build/
    python3 tools/promote.py final                   # 300-frame CJ_DET, all promoted on vs all off (two runs at once)

1. **Candidates.** `scan` lists the MATCH functions that hybridize translates (3486 of 3972). `reach`
   sets a gdb `tbreak` (self-deleting, `silent; printf; continue`) on the host symbol
   (`m_<name>_<VA>`) of every candidate that is not already a target, and runs the normal CJ_DET
   300-frame boot under gdb: 3475 breakpoints, 493 hit. This is cheap (one run, about 9 min) and
   reusable. `compilecheck` generates each unit with all its reached candidates and compiles the
   result with the build's own compiler and flags (`flags.make`), splitting a failing unit in halves
   until each failure is a single function. A candidate must be reached AND compile clean.
2. **One regeneration, runtime selection.** `batch` writes `work/promote/targets-batch.txt` = the 8
   promoted targets plus the batch (merged into one `unit=` line per unit, which `hybridize.py gen`
   requires), runs `hybridize.py gen` and `tools.recomp --exclude-manual` once (about 3 min) into
   `work/gen-batch`, and builds `build-batch/`. Every function is a wrapped `sub_X` and can be toggled
   with `CJ_NATIVE_SWAP=<VA,VA,...>` (the selection is a real list: `strstr` on the 8-hex VA, see the
   fixed bug in docs/determinism.md). The runner always puts the 8 core VAs in the list, so they
   are on in every run, baseline included. `CJ_NATIVE_SWAP=0`/unset is everything generated.
3. **Tests.** Every run is `CJ_DET=1 CJ_LOCKSTEP=2 CJ_STOP_FRAME=300 CJ_DET_HASH=1` with frame dumps.
   - Baseline: candidates off, core on, run twice at the same time; the two must agree on all 300
     `.data` hashes, all heap hashes and all 301 flipped frames, otherwise the tool stops.
   - A group run is judged against the baseline. PASS needs: exit 0 with the stop line, identical
     `.data` hash at every frame, identical heap hash at every frame except the documented frame-1 APU
     word (`CJ_DET_REF` below skips only f=1 heap), identical frame images (count and md5), identical
     call counts of every wrapper in every 16-frame report and the final report, esp_bad = csr_bad = 0,
     no FATAL / `[HYB]`, and no `[ABI]` report the baseline did not have.
   - Calls per run come from the baseline's final report (`calls=`). A candidate with 0 calls is
     NOT_REACHED and is not put in a group (none of the batch was, because reach selected them).
   - **Early exit** (`src/determinism.c`, `CJ_DET_REF=<baseline log>`): the run loads the baseline's per-frame
     hashes and stops at the first mismatch with `[DET] DIVERGE f=N region=data|heap` (status 3) and a swap
     report. A failing group costs seconds instead of 7 minutes; only a passing group runs all 300 frames.
   - **Stall guard.** No new `[DET] f=` line for `PROMOTE_STALL` seconds (default 100, 20 was used
     once boot was known to take about 2 s) means a hang: the runner takes a `gdb -p` backtrace of every
     thread into the run log, then kills the run.
   - **Peeling instead of plain bisection.** A failing group is not halved blindly. The culprit is
     (a) the first `cjn_<VA>` frame of the host crash backtrace or of the gdb dump of a hung run, or
     (b) the first logged wrapper call (`[SWAP] VA #n ... pre= post= ret=`, first 48 calls per function)
     whose pre/post/ret differ from the baseline's: the function itself if its pre-state was equal, otherwise the
     last candidate call before it. That candidate is recorded as FAIL (with the reason) and the group is run
     again without it, until it passes. If no suspect is found the group is halved. Two groups (halves of the
     live set) are processed at the same time, never more than two runs. Because (b) can blame a victim, a FAIL is "not promoted",
     not necessarily "wrong in isolation"; PASS is only given to a function that was inside a group run that passed.
4. **Promotion.** `promote --regen` rewrites `tools/hybridize/targets.txt` (original header kept, one line per unit,
   previous file in `work/promote/targets.before-promote.txt`), reruns `hybridize.py gen` + `tools.recomp` into
   `src/recomp/gen` (RUN.md) and rebuilds `build/`. `final` then runs the 300-frame all-on vs all-off comparison on that
   binary; if it fails, find the group with `test` logic (or `promote --drop VA...`) and repeat.

## Results file

`tools/hybridize/promote-results.json` (and `.csv`): per candidate `va, name, unit, size, status (PASS | FAIL |
NOT_REACHED), calls` (calls over the 300-frame baseline), `frame` (first divergent frame, or the last frame seen
for a crash/hang), `region` (data | heap | image | calls | abi | crash | log), `detail`, `runs` (name of the
group run that decided it). `tools/hybridize/promote-reached.txt` is the reachability list (493 VAs).

## Runtime additions

| piece | what |
|---|---|
| `CJ_DET_REF=<log>` (src/determinism.c) | early exit at the first hash difference vs a baseline log (frame-1 heap word ignored) |
| `CJ_SWAP_HASH_N` (hybrid_swap.c, default 4096) | the pre/post state hashes are only computed for the first N calls of a function; the per-call hash of a hot function (up to 0x400 bytes of `this` plus 0x80 behind each pointer word) otherwise dominates the run |
| `cjg::up<Base>(self, 0)` for a base at +0 | hybridize.py: a native callee that takes a base class pointer (views are flat structs) got `EXItem*` for `EXPlacementItem*`; only showed when two functions that call each other were both targets |
| constructor adapter returns the saved `this` | hybridize.py: `cjn_` used `cjg_ecx()` after the body, but the body's guest calls clobber ecx, so a native constructor returned garbage in eax |

## Notes on cost

300 frames take about 7 to 8 minutes with two runs at once on this 2-core host, which dominates; everything that fails
is decided in seconds to 25 s. One batch (regeneration + build + tests) is therefore about 35 minutes, and the
number of passing group runs, not the number of candidates, is what costs time.

## First run (batch of 120)

Scan 3486 translatable; reach 493 of 3475 (300 frames); 2223 of 2261 checked candidates compile clean (the 38 failures:
`operator=` on GuestPtr, `EXGeoComplexRefObject*` to derived-view conversions, string-literal arguments, ...). The batch is
120 reached and clean functions (20,704 bytes, 86 units, round-robin over units, largest first within a unit). All 120 were
reached in the baseline run (NOT_REACHED = 0 by construction).

Results (`tools/hybridize/promote-results.json`): **11 PASS**, **61 FAIL**, **48 UNTESTED** (the time limit hit while their group runs were
still in flight; they are not failures), 0 NOT_REACHED. Two baseline runs were identical for 300 frames
(.data, heap, 301 images), 487 s with two at once.

PASS (calls over 300 frames): EXGeoFile::Update 0x00209270 (1851), EXPlacementItem::CalcMatrix 0x001D20F0 (70), XGamePad::LookupButton
0x00199F40 (68), FEManager::FEManager 0x000F4AC0 (2), EXAudioCodec::ReleaseEnvStereo, Actor::PreLoadInit, EXSwooshSysMgr::DeleteAllSystems,
EXLightManager::RemoveAllDynLights, ActorTextureSwap::PreLoadInit, VisionWipe::Instance, TrafficDeinit (1 call each).
targets.txt now has 19 functions, 3,952 bytes (the 8 old ones plus the 11).

Failure causes seen (FAIL entries whose reason is a *suspect* from the first differing logged call are not proven culprits):

1. Constructors: 17 of the 61 are constructors, and the ones the tool could pin down differ after call #1 with an equal
   pre-state (EXWatcher, CFormation, AIControl, XNavRoute, EXGamePad::OpenPads ...). One cause is fixed (`cjg_ecx()` read after the
   body, see above; FEManager::FEManager passed after it), the rest of the ctor failures are not diagnosed.
2. Hangs at boot (about 25 FAILs): a native body never returns or the boot loop never reaches frame 1 (`rc=-9`, stall guard).
   Native crashes: 3 (EXDebugTree::SectionHandle / ItemHandle: `cjg::h_strcmp(pName, call_cdecl<char *>(...))` treats a guest
   `char *` return value as a host pointer; EXBaseApp::InitInstance).
3. `.data` divergence at frame 1 (about 25 FAILs): boot-time functions (EXBaseSoundManager, EXStats, XNavMeshHandler::Init, EXAudioBase
   ctor ...) whose native version writes different bytes, or whose guest-visible stack/register residue differs. The prologue
   mirroring (`orig_prologue`) only repeats leading `push reg` / `sub esp`; most of these start differently.
4. A frame image difference at f2 with identical hashes (group Ba): something touches memory outside `.data` and the heap window (GPU/contiguous), which the
   per-frame hash does not cover.

Not yet verified: the `this_in` constructor fix on the other ctor failures (the batch was not rerun after fixing the remaining ones), and which of the
UNTESTED are good.

### Final check on the promoted set

`promote --regen` (hybridize + tools.recomp 188 s + rebuild of build/) and then `tools/promote.py final`: CJ_DET=1 CJ_LOCKSTEP=2
CJ_STOP_FRAME=300, all 19 targets native vs `CJ_NATIVE_SWAP` selecting nothing (all generated), both runs at the same time (518 s):
**PASS**: .data hash and heap hash identical at all 300 frames (frame-1 heap word excluded), all 301 flipped frames identical, call counts of
all wrappers identical at every report, esp_bad = csr_bad = 0, no FATAL / [HYB], no new [ABI]. Logs: run/promote/final_on, final_off.

## Round 2 (generator fixes, partial rerun)

Root causes and fixes per failure class (details in docs/hybridize.md round 6):

1. **Constructors**: the residue fixes below apply; additionally a callee the original really calls (base constructors
   included) is now called by VA, with the original's return VA. EXWatcher, CFormation, AIControl still differ after call
   #1: their originals inline `EXArray<T>` constructors/SetSize that do not translate (template members), so the native
   body calls them by VA and leaves different stack residue. Not fixed.
2. **EXDebugTree::SectionHandle / ItemHandle crash**: not a guest `char *` return. `s.m_Name` goes through the conversion
   operator `EXFixedArray<char,15>::operator char *`, which the member-call path did not accept (`CONVERSION_FUNCTION`), so it
   was emitted as a cdecl call with no `this`. Fixed; SectionHandle in isolation now runs 300 frames with `.data`/heap
   identical, one image differs at f2 (class 4).
3. **Boot-time `.data` divergence / hangs**: guest stack residue. Representative XBFileManager::FileRead (it was the
   "first differing call" of about 40 FAILs). The native call pushed 0xC0DE instead of the original's return VA, ran at a
   different esp (the original saves ebp/esi late, and keeps its local in the `push ecx` slot), and held different values in
   ebx/esi/edi/ebp, which callees save into their frames. Now per call site: original return VA, original esp depth, late
   saves written, callee-saved registers set to this/argument values as in the original (`orig_frame`, `orig_sites_full`,
   `cjg_ctx_*`). FileRead's 33-word difference before call #12 is down to one word (a deeper callee frame); it still
   diverges at f1 in isolation.
4. **Frame image difference with identical hashes**: `CJ_DET_HASH_CONT=1` (src/determinism.c) adds `cont=<hash>` of
   contiguous memory 0x80000000..0x84000000 to every `[DET] f=` line, and promote.py judges it when both logs have it
   (`PROMOTE_CONT=1`). It is off by default: two identical core-only baselines already differ there from f1 (host GPU / push
   buffer writes), so it needs a deterministic sub-range first.

`tools/promote.py test --skip-pass` keeps earlier PASS verdicts. Each runtime change also changes the core targets' residue,
so build-batch was rebuilt and the baselines rerun (still deterministic: `.data`, heap, 301 images over 300 frames).

Rerun (failed + untested, 109 candidates, stopped at the time limit, about 7 min of group runs): 3 new PASS
(EXBaseDisplay::IsWndVisible 0x002168A0, EXBaseWnd::IsChild 0x0021EE80, FERadar::Instance 0x00105330; all three were round-1
FAILs), 13 FAIL (XNavMeshHandler::Init, EXBaseApp::InitInstance, XGameWnd::Update, AIControl, CFormation, EXWatcher ctors,
SectionHandle, _EXRealloc, revmodel::mute, EXAudioVoiceManager ctor, and the hang group A down to a single candidate), 93
UNTESTED. Results file: 14 PASS, 13 FAIL, 93 UNTESTED (round-1 file: work/promote/results-round1.final.json).

Promotion: targets.txt has 22 functions, 4,176 bytes (previous list in work/promote/targets.before-promote.txt).
`final` (300 frames, all 22 on vs all off): `.data` and heap identical at all 300 frames, call counts identical, esp_bad =
csr_bad = 0, but **one flipped frame differs (f2)**: NOT PASSED. Same symptom as SectionHandle in isolation (class 4); the
three new targets each passed inside group Baaaa against the core-on baseline, so the f2 image is either an interaction or
run-to-run variation in contiguous memory. Next: run `final` again to see if f2 repeats, then `promote --drop` the three new
VAs one at a time.

## Round 3: stack hygiene instead of per-site residue parity

Decision (lead): stop reproducing the original's stack residue per call site. Under CJ_DET every guest stack
allocation now starts zeroed, in recompiled and native code alike (toolkit patch 0005 + guest_rt.c; details and
its own determinism check in docs/determinism.md round 6). **Hygiene changes the original's uninitialised-stack
contents only in test mode, equally for recompiled and native code; the normal build is unaffected.** Round 2's
call-site parity code is kept and is now optional at runtime: `CJ_SITE_PARITY=0` turns it off. It stays on by
default because there was no time to measure the off setting against a baseline of its own (each needs a new
baseline pair); with hygiene, parity only still matters for stack *addresses* that the game stores (a deeper esp
gives different pointer values), not for residue.

Run (time-boxed): build-batch and src/recomp/gen regenerated with the patched lifter; `PROMOTE_FRAMES=150
PROMOTE_STALL=40 tools/promote.py test --skip-pass` (300-frame runs were too slow next to a rebuild; see
determinism.md). Baselines: identical (`.data`, heap, 151 images, reports). 106 FAIL/UNTESTED candidates
retested for about 13 min of group runs (timeout), log work/promote/test-hygiene.log; round-2 results file
kept in work/promote/results-round2.final.json.

| | PASS | FAIL | UNTESTED |
|---|---|---|---|
| before (round 2) | 14 | 13 | 93 |
| after (round 3) | 15 | 13 | 92 |

New PASS: GeoSectionItem::Update 0x0012D8F0 (416 bytes), not yet promoted (needs `promote --regen` + `final`).
FAILs (same 13 functions as round 2 apart from naming): with hygiene the first divergence is now at call #1 with
an *equal* pre-state for most, i.e. not residue:

1. Boot hang, group A: EXAudioBase::EXAudioBase 0x002C3D40 alone hangs the boot (isolated by halving). The
   other A failures (EXAudioVoiceManager ctor, revmodel::mute, SectionHandle, CFormation/AIControl/EXWatcher
   ctors) were blamed by the "first differing call" heuristic while that hang was in the group; they are
   possibly innocent and should be retested without 0x002C3D40.
2. Return register width: EXGamePad::OpenPads 0x0026EBF0 is `return 1` (int8_t); the original does `mov al, 1`
   and keeps the upper 24 bits of eax, the adapter writes the full eax (`cjg_set_eax((uint32_t)(int32_t)..)`,
   hybridize.py line ~1984). Likely the same for other bool/char returners. Fix: for 1-byte returns write only
   al (and ax for 2-byte) into g_eax.
3. `.data` DIVERGE f1 with equal pre-state: XNavMeshHandler::Init, EXBaseSoundManager ctor (logic or the
   register-width issue above; not diagnosed).
4. `[HYB]` FATAL/esp reports: EXBaseApp::InitInstance, XGameWnd::Update, EXItemAnimator::UpdateBoundsBox
   (native call ABI mismatch, not residue).
5. Register residue the hygiene cannot cover: a guest callee that reserves a slot with `push ecx` saves whatever
   ecx the (native) caller left; scratch registers at native->guest calls are not modelled.

Final check (build/, 22 targets, hygiene on, `PROMOTE_FRAMES=150 tools/promote.py final`): **PASS**. `.data`
and heap identical at all 150 frames, all 151 images identical (including f2), call counts identical, esp_bad =
csr_bad = 0. The round-2 f2 image difference is gone without dropping any target: it was guest stack residue
reaching contiguous (GPU-visible) memory, which hygiene zeroes. The `--drop` bisection was therefore not needed.
Not yet run: the same check to 300 frames.

## Round 4: return-width fix, EXAudioBase excluded, retest

**1. Return width** (`hybridize.py` adapter, `guest_rt.c/h`: `cjg_eax`, `cjg_set_al`, `cjg_set_ax`). A function whose return type is 1 or 2
bytes (bool, char, signed/unsigned char, short, wchar; not constructors or sret) now reads eax at adapter entry and writes only al / ax
back, keeping the upper bits the caller left. This mirrors the original: EXGamePad::OpenPads 0x0026EBF0 is `mov al, 1 / ret`, i.e. the
upper 24 bits of eax are untouched. Verified in isolation: build-batch regenerated and rebuilt, new baselines (identical), then
OpenPads alone against baseline, `PROMOTE_FRAMES=150`: **PASS** (`.data`, heap, images, call counts, no [HYB]). It was a round-1 FAIL
(same pre-state, different post/ret) and is now promoted.

**2. EXAudioBase::EXAudioBase 0x002C3D40** is excluded from groups and recorded as FAIL (hangs the boot). New `promote.py test --exclude VA:reason`
(also `--only VA..`, `--out file`, `regen-batch`, `final --off2`). The six functions blamed with it in round 3 were retested without
it: they still fail. The group A boot hang persisted without 0x002C3D40 and the peeling again pinned CFormation 0x000ADA40, AIControl
0x000AC8F0, EXWatcher 0x00248F50 (same pre-state, different post/ret, the `EXArray<T>` inlining problem of round 2) and blamed
EXAudioVoiceManager ctor 0x002BFF10, revmodel::mute 0x002CE720, SectionHandle 0x002490D0 (suspects only: "first differing logged call
001ABBE0 #16"). Not innocent as far as the tool can say, not proven culprits either.

**3. Retest** (`PROMOTE_FRAMES=150 PROMOTE_STALL=40 test --skip-pass --reuse-base --exclude 002C3D40:...`, 150 frames as in round 3 because
group runs at 300 frames do not fit the time box). The run was stopped after 44 min of group runs (the remaining time was needed for
promotion and the 300-frame checks); log work/promote/test-r4.out. Group B (about 50 candidates) was peeled one candidate at a time
by the "first differing logged call 001D20F0 #1" suspect rule, which on a `[HYB]` esp-mismatch failure blames a victim each time (a
core function whose call differs only because the run aborted); those FAILs (marked below as `[HYB]`, 9 of them) are weak evidence.

| | PASS | FAIL | UNTESTED |
|---|---|---|---|
| before (round 3 file) | 15 | 13 | 92 |
| after (round 4) | 17 | 30 | 73 |

(OpenPads from the isolation run is counted as PASS; EXAudioBase ctor is among the FAIL.) New PASS: EXGamePad::OpenPads 0x0026EBF0 (16 B),
EXItemAnimatorQueue::Allocate 0x00216070 (single-candidate group Aaaaab). New FAIL (17 functions): see `promote-results.json`; causes below.

Top remaining failure causes (30 FAIL):

1. Native-to-guest call ABI mismatch, `[HYB] esp after call to X`, 12 FAIL: XGameWnd::Update 0x0012A8B0, EXItemAnimator_Script::AddItemThread
   0x001B33A0, EXBaseWnd::CreateDbgMem 0x002582D0, plus 9 victim-blamed ones (EXDList::Remove 0x0020E870, EXItemAnimator::UpdateBoundsBox 0x002154A0 ...). The
   esp is 4 or 8 bytes lower than the adapter expects after calls to 0x002D16E4, 0x002D149D, 0x001A6A70, 0x0020E930, 0x00258210: the generated native
   call assumes callee-pops (or the wrong argument word count) for these guest callees. Needs the callee's real convention (ret imm16 in the original) in `cjg_call_raw` users.
2. Boot hangs (rc=-9, stall), 11 FAIL: the three constructors above, 0x0023A530 (native body hung), 0x000ADA40, 0x00205200 ...
3. `.data` divergence at f1 with equal pre-state, 3 FAIL: XNavMeshHandler::Init 0x001347F0, EXBaseSoundManager ctor 0x001B0830, XGamePad::Init 0x00199CA0.
4. Aborts (rc=-6) with equal pre-state, 2 FAIL: GameData::DataTransfer 0x00129270, FEText::Decompress 0x00173110.
5. Frame image difference at f2, hashes equal: EXWnd::_SelectSprite2DTexture 0x0024DDD0 (also seen as a group Aaa failure).

Promotion: `promote --regen`: targets.txt now 25 functions, 4,976 bytes (22 before, 4,176 bytes; previous list work/promote/targets.before-round4.txt),
adds GeoSectionItem::Update 0x0012D8F0 (416 B), OpenPads, EXItemAnimatorQueue::Allocate. 73 candidates remain UNTESTED (the time box, not failures).

Final check (build/, 25 targets, hygiene on, 300 frames, `promote.py final`, all on vs all off, two runs at once, 532 s): **PASS**. `.data` and
heap identical at all 300 frames, all 301 flipped frames identical, call counts identical, esp_bad = csr_bad = 0, no FATAL / [HYB] / new [ABI].
Logs: run/promote/final_on, final_off.

Swap-off determinism pair (`promote.py final --off2`, build/, everything generated, hygiene on, two runs at once, 300 frames, 548 s): **PASS**.
rc 0,0; `.data` and heap identical at all 300 frames, all 301 images identical, call counts identical, no [HYB]. This closes the gap noted in determinism.md round 6.
Not yet done: the culprit-finding rule for `[HYB]` failures (record the native function active at the esp mismatch), then retest the 73 UNTESTED.

## Round 5: blame attribution, callee cleanup from the binary, retest of everything

**1. Blame attribution** (`guest_rt.c/h`: `cjg_nat_enter/leave/cur`; `hybridize.py` adapter). Every `cjn_<VA>` adapter now pushes its VA on a per-thread
stack (`RECOMP_TLS`, depth 256) on entry and pops it on exit. Every `[HYB]` report (`esp after call`, `FATAL: no recompiled function`,
`FATAL: host pointer`, and the new `call pushes N words but the callee's ret pops M bytes`) ends with `native=cjn_<innermost> chain=<outer>,...,<innermost>`:
the native function that made the offending call. `promote.py culprit()` reads that first: the innermost VA if it is a candidate of the group,
else the innermost candidate on the chain (a core or already-promoted function was the caller), and only then falls back to the `cjn_` frame of a
crash/hang backtrace and the "first differing logged call" heuristic. It is also tried on DIVERGE runs now. For hangs the stall guard sends SIGUSR1
to every thread (tgkill) before the gdb dump; `guest_rt.c nat_usr1` writes `[HYB] STALL native=cjn_<VA> chain=...` per thread, which the same parser
reads (this handler is in `build/` but not in the `build-batch` binary the round 5 group runs used: there the stall kill shows as rc=-10).
It was used for real once: EXItemAnimator_Entity::Initialise 0x001B0C10 was pinned in the first group run
(`[HYB] blamed native 001B0C10`, no peeling by "first differing call").

**2. Native-to-guest call cleanup.** The 12 "esp 4 or 8 lower" FAILs were not a wrong callee convention. `cjg_call_raw` took `esp0` before the call
site's depth adjustment (`Site` depth: the call runs at the original's deeper esp, e.g. after the original's late `push ebp/esi/edi`) and compared the
esp after the call with `esp0`, so every call at a depth of 4 or 8 bytes below the adapter's esp was reported as off by 4 or 8 (0x00258210 from
CreateDbgMem has depth 0x24 vs 0x1c, `_EXAlloc` at depth 0x28 ...). The expected esp is now relative to the call's own base (`esp1`, after the depth).
In addition `hybridize.py callee_pop(va)` reads the callee's real cleanup from the original binary: every `ret` / `ret N` of the function (and of a function
it tail-jumps to, depth 3) must agree, else unknown (-1) and the declared signature decides (so: variadics, `_SetFilePointer@16` 16, `_ReadFile@20` 20,
`_EXAlloc` 0, `EXDList::IsMember` 4, `DbgClear` 0; multiple differing rets or `jmp reg/[mem]` or no size: signature). It is emitted as a fourth `Site` field
(`cjg::Site(va, ret, depth, pop)`) for every direct call (vcalls stay on the signature: the callee is only known at run time) and the runtime computes
`want = esp1 - 4n + pop`; a callee whose `ret N` pops more than the call pushed is reported as `[HYB]`. The guest-side effect is unchanged
(`g_esp = esp0` after every call), so the generated bodies behave exactly as before; only the check is right now.
The three direct cases (alone, `PROMOTE_FRAMES=150`, against the unchanged round 4 baseline): XGameWnd::Update 0x0012A8B0 **PASS**, EXBaseWnd::CreateDbgMem
0x002582D0 **PASS** (it failed once more later in a group run at f2, image only: see 4), EXItemAnimator_Script::AddItemThread 0x001B33A0: the esp error is gone, it failed
once on an f2 image difference (class 4) and passed in the next group run. Its call #1 post-state hash still differs from the baseline's (the original reuses the
`pCmd` argument slot at esp+0x18 for the local `parent`, the native body leaves the argument word): not observable so far, but not mirrored.
`HY_INLINE_HDR=1` (default off, not tested) additionally translates a header-defined accessor that the swap target's original never calls (inlined there) instead of calling it by VA.

**3. Retest.** `build-batch` rebuilt (only bodies changed, no recompile), baselines reused (150 frames, `PROMOTE_FRAMES=150 PROMOTE_STALL=40`, same
settings as rounds 3 and 4; run behaviour is unchanged by the changes above). The 12 esp FAILs plus all victims blamed by the old heuristic were innocent:
group B (52 candidates) lost only 3 real divergers (it was PASS at 49) in the first 10 minutes. Group A then spent 20 minutes on frame-image
differences at f2: a 3-frame **screen** (`work/promote/screen.py`, each candidate alone, boot to f3, hashes + images vs the baseline) took 43 candidates in
5 minutes. 10 of them differed in the f2 image; all 10 passed a second screen. The difference is the loading screen (bottom-left animated texture
strip, 13 KB of the frame) and follows real-time loader progress, not the candidate. `promote.py test` now repeats a group once when the only difference is an
f<=3 image (a flake passes the second time). Then 41 screened candidates ran as two groups of 20 (both PASS, 393 s), and the candidates that earlier rounds
had FAILed by the "first differing call" suspect rule were screened alone: 002CE720 revmodel::mute, 002490D0 EXDebugTree::SectionHandle, 001347F0
XNavMeshHandler::Init, 00199CA0 XGamePad::Init pass (they were victims of EXAudioBase / EXStringArray::SetSize / EXBaseSoundManager in their groups) and
ran as two more PASS groups (263 s).

| | PASS | FAIL | UNTESTED |
|---|---|---|---|
| before (round 4) | 17 | 30 | 73 |
| after (round 5) | 110 | 10 | 0 |

Remaining 10 FAIL, each alone against the baseline (3-frame screen):
1. Boot hang, 3: EXAudioBase::EXAudioBase 0x002C3D40, FEBaseWnd::FEBaseWnd 0x00110220, EXParticleSysMgr::EXParticleSysMgr 0x0023A530.
2. `.data` DIVERGE at f1, 6: EXStringArray::SetSize 0x001ACB80, EXAudioVoiceManager ctor 0x002BFF10, CFormation 0x000ADA40, AIControl 0x000AC8F0, EXWatcher 0x00248F50
   (the `EXArray<T>` inlining problem of round 2), EXBaseSoundManager ctor 0x001B0830.
3. Abort with a `[HYB]` report in the group run, 1: EXItemAnimator_Entity::Initialise 0x001B0C10.
The three former "f1 data" FAILs XNavMeshHandler::Init, XGamePad::Init, and the `[HYB]` ones are now PASS; EXBaseSoundManager ctor still diverges.

Promotion: `promote --regen`: targets.txt 118 functions, 18,928 bytes (before: 25 functions, 4,976 bytes; list in `work/promote/targets.before-round5.txt`), hybridize + recomp 193 s, build about 6 min.

Final check (build/, 118 targets, hygiene on, 300 frames, `promote.py final`, all on vs all off, two runs at once, 510 s): **PASS**. `.data` and heap identical at all 300 frames,
all 301 flipped frames identical, call counts identical, esp_bad = csr_bad = 0, no FATAL / [HYB] / new [ABI]. Logs: run/promote/final_on, final_off.
Not done: the 300-frame group runs themselves (round 5 groups ran 150 frames; the 300-frame final is the only 300-frame check), the 6 f1 divergers and 3 boot hangs above,
a `[HYB]`-blamed hang check with the SIGUSR1 handler (it is only in `build/`), and the AddItemThread argument-slot residue.
