# verify.py: batch verification of unproven decomp functions against the recordings

`tools/verify.py` takes every written-but-unproven (status DIFF) function in pcj-decomp that
`hybridize.py` can translate, builds one binary with all of them swappable, and judges each one
against the recompiled original on the gameplay recordings (tests/fixtures/rec-*). Functions that
pass get a `REPLAY_VERIFIED` row in `pcj-decomp/status/verify_overrides.csv`, which
`pcj-decomp/tools/coverage.py` counts as verified. No git commits are made by the tool.

## The pipeline (each step resumes from files under work/verify)

    cd /home/claude/pcj-native
    units=$(ls /home/claude/pcj-decomp/src/*/*.cpp | xargs -n1 basename | sed 's/\.cpp$//')
    python3 tools/hybridize/hybridize.py scan -v --diff $units > work/verify/scan.txt   # 4 min, once per decomp change
    python3 tools/verify.py plan                 # candidates -> plan.json (--units, --min-size, --max, --match)
    python3 tools/verify.py build                # compile check (bisects bad units) -> clean.json; one regen + build-verify/ (~15 min)
    python3 tools/verify.py run                  # dual execution on every fixture -> results.json (~2 min per fixture, 2 at once)
    python3 tools/verify.py confirm              # identical candidates run native through every fixture; peels culprits
    python3 tools/verify.py report --write-overrides   # table + REPLAY_VERIFIED rows into pcj-decomp
    python3 tools/verify.py status

`plan` skips anything already in targets.txt, anything with a `// MOD` section in mods/*.cpp
(deliberate behaviour changes) and the CJ_DET wrappers. `--match` adds byte-matched functions
that are not promoted yet (useful to grow the native set; promote.py's boot-only test is the older
way).

## What "verified" means here

Two layers, both required for REPLAY_VERIFIED:

1. **Dual execution** (src/native/hybrid/swap_verify.c, `CJ_SWAP_VERIFY`): on each checked call the
   runtime snapshots all 64 MB of guest RAM, the registers and the deterministic clock, runs the
   recompiled original twice (A, B) from that snapshot, then the native version (C), and compares
   every byte of RAM, the callee-saved registers, esp and the return value of C with B. With
   `CJ_SWAP_VERIFY_KEEP=orig` the original's outcome (B) is put back afterwards, so a wrong native
   version cannot derail the replay: every candidate is judged on its own calls for the whole
   recording. Checked calls: the first N (`--calls`, 16) and every 50th up to 50N (hot functions
   get samples across the run).
2. **Confirm**: candidates with no difference on any call then run native (outcome kept) through
   every fixture, which must replay exact (heap hash every 60 frames). A divergence is peeled:
   the group is halved (two runs at a time) until the culprit(s) are known; a host crash or hang
   names its function from the backtrace (`cjn_<VA>`).

Bytes that are not judged, and why (printed per call as `unjudged=N`):
- bytes where A and B already differ: the interrupt helper threads (vblank, APU, timer DPCs)
  write a few guest words at any moment, and a function that reads them (XItemPhysics::DoCollisions
  reads the vblank state and fills `ScoopFaceData` differently) is not reproducible there even
  against itself;
- the async words themselves, measured at start by idling 6 x 25 ms (D3D__Device vblank fields,
  `g_dwDirectSoundDeltaPanicCount`, ...), padded 16 bytes;
- the helper threads' TIB pool (0x00F80000..0x00F80A00) and stack slices (stack region base + 4 MB,
  0x00780000..0x00B80000 here; the main stack is at the region's top, esp ~0x00F7F000);
- the callee's own locals below the caller's esp (the native version keeps them on the host stack).
- the callee's own argument slots (esp+4 .. esp+4+argument bytes, from the demangled signature in
  symbols/functions.csv; skipped for by-value structs): MSVC code stores into its parameters
  (SlerpConst writes the clamped t back, Vec_AngleDifference the wrapped angle) and those slots are
  dead after the return. Both were reported as DIFFERS before this filter; the logic was right.

Everything else is compared byte for byte. A function whose writes are mostly unjudged (large
`unjudged=`) still has to pass the confirm layer, which hashes the heap it affects.

Coverage is what the recordings exercise: chapter 1, movement, strafing, quick save/load. A path
no recording takes is not verified; `report` prints the calls per fixture so you can see how much
each function was exercised. More recordings = more coverage (docs/recording.md).

## Verdicts

| verdict | meaning |
|---|---|
| REPLAY_VERIFIED | identical on every checked call and every fixture replays exact with it native |
| IDENTICAL | dual execution clean, confirm not run yet (or not reached in confirm) |
| DIVERGES | dual execution clean but a fixture diverges with it native (noise it is sensitive to, host state, or a call beyond the checked ones) |
| DIFFERS | at least one checked call wrote different bytes / registers / return value: the decomp is wrong on that path. `work/verify/results.json` -> fixtures.<name>.verify.<VA>.first has the call, frame and ranges |
| CRASH | the native version crashed or hung the host while under dual execution (dropped from the run, listed in results.dropped) |
| UNREACHED | no recording calls it |
| UNCHECKED | called, but no dual-execution line (verifier off: out of memory) |

## Files

| file | content |
|---|---|
| work/verify/scan.txt | hybridize scan -v --diff listing (OK/NO per function with the reason) |
| work/verify/plan.json | candidates (va, unit, name, size, status) |
| work/verify/clean.json | VA -> '' (compiles) or the gen/compile error |
| work/verify/targets.txt | targets.txt + candidates as `mod=` groups (what build-verify/ was built from) |
| work/verify/batch.json | the clean candidates in the build |
| work/verify/results.json | per fixture: end, seconds, calls per VA, verify per VA (checked, differs, first difference) |
| work/verify/confirm.json | pass list, fail reasons |
| work/verify/report.csv | the report table |
| run/det/verify-<fixture>-<tag>/log.txt | every replay's log ([VERIFY] lines, [SWAP] end report) |

## Runtime pieces added for this (all off unless set)

| env | effect |
|---|---|
| `CJ_SWAP_VERIFY_KEEP=orig` | keep the original's outcome after each dual execution |
| `CJ_SWAP_VERIFY_EVERY=K` | also check every Kth call up to K*N |
| `CJ_REC_EXIT=2` | a replay exits at the first divergence (replay.sh's default now; REC_EXIT=1 runs to the end) |
| `[SWAP] end` / `[SWAP] diverge` report | call counts of every wrapper at the end of a replay |
| `cj_det_clock_save/restore` (determinism.c) | the per-thread rdtsc/system-time counts and the tick, restored between the runs |

`replay.sh` takes `PCJ_BIN=<binary>` (default build/pcj) and `NODRAW=1`; `soak.sh` runs with both.

## Making more fixtures (coverage) without a controller

    tools/det/make_fixture.py ch1-run-600 --mods 0 --press-at 60 lup:200 --press-at 300 lup+a:3 --after 200 --check
    tools/det/make_fixture.py ch1-fight --mods 0 --scene ch1-fight --press-at 120 x --press-at 160 x --after 300 --check

Boots through tools/route.py to chapter 1 with CJ_REC=1, sends the presses (flips after control,
route.py syntax), moves the recording to tests/fixtures/rec-<name> with env.txt, and with --check
replays it once. `--mods 0` records the original game (what verification should compare against);
`--mods default` records with the non-behaviour mods like the other fixtures. `--scene` starts
from a quick save in scenes/scenes.json (the first fight, the courtyard, the hallway) so a fixture
can cover combat or a later part of the chapter without a long route. The fixture's `qs-load`
copy makes the load part of the recording.

## Translator additions (hybridize.py, 2026-10-27)

- String literals passed to guest calls or constructors are now the retail binary's own copies:
  the literal's bytes are looked up in the XBE sections (`GuestStrings`) and the call gets
  `cjg::h<char>(0xVA)`. A literal the XBE does not hold (a stripped debug string) still fails.
- A call to an inline function with an empty body (ACTOR_TRACE and the like) is dropped: its
  non-literal arguments are still evaluated (guest calls with side effects stay in order).
- `scan --diff` lists DIFF functions too; the OK lines end with the status.

## State at hand-off (2026-10-27 evening): read this first

What is proven:
- The dual-execution verifier is sound on gameplay code run one or a few functions at a time:
  XItemPhysics::DoCollisions, MoveStrafe, MoveJump, DoJump: 30/30 checked calls identical on
  rec-ch1-1600 once the asynchronous words, the helper threads' memory and the A/B calibration were
  in place (the sequence of fixes is in the git log of src/native/hybrid/swap_verify.c).
- The whole pipeline (plan, build, run, confirm, report --write-overrides) runs end to end; the
  fixture maker made rec-ch1-run-short and it replays exact.

What is NOT working: the full batch (608 candidates in one binary). The verify runs crash or hang
within seconds of play on every long fixture, and the blamed functions (IntersectRect,
FEObject::MakeSafeRect, FEBaseWnd::AlignRectToRect: pure rectangle arithmetic) cannot be the
cause. So the rerun machinery itself breaks the game in the front-end / HUD update context, in a way
the smoke tests on movement code never hit. Not diagnosed. Candidates: the pushbuffer executor
thread reading guest memory (pushbuffers, vertex data) while the main thread restores 64 MB over
it; SEH or other host-side per-call state in the recompiled FE code; the pad ring or file-position
restore interacting with something FE code does. The last batch: 1 REPLAY_VERIFIED (FEWnd::Init),
1 DIVERGES (EXPlacementItem::EXPlacementItem, a constructor: promote.md already lists constructors
as a known weak spot of the native adapter), 12 wrongly blamed CRASH, 141 UNCHECKED (the runs died
before their calls), 437 UNREACHED (chapter 1 never calls them: coverage, not a bug).

How to use it until that is fixed:
- Verify by area, not the whole pool: `plan --units Player XItemPhysics Actor ActorCombat ...`
  (gameplay units), `build`, `run`, `confirm`, `report`. The FE*/EXWnd/EXDisplay/EXRect/EXSoundTag/
  EXGeoFile/SetFog/Rain families are on the deny list (DENY_NAME in verify.py) for this reason.
- Or verify one function the manual way, which is what the smoke tests did:
  `NODRAW=1 PCJ_BIN=build-verify/pcj CJ_MODS=<the fixture's list> CJ_SWAP_VERIFY=<VA> CJ_SWAP_VERIFY_N=16
  CJ_SWAP_VERIFY_EVERY=100 CJ_SWAP_VERIFY_KEEP=orig DISP=:121 sh tools/det/replay.sh tests/fixtures/rec-ch1-move-2880 linux t1`
  then read the [VERIFY] lines in run/det/t1/log.txt. CJ_SWAP_VERIFY_DEBUG=1 prints where the two
  original runs disagree.
- Coverage is the other half: 437 of 608 candidates are never called by the chapter-1 fixtures.
  make_fixture.py with --scene (the first fight, the courtyard) and later-chapter routes is how to
  reach them.
- Debugging the batch crash: take one blamed function (IntersectRect 00252B60), run it alone with
  CJ_SWAP_VERIFY=00252B60 on rec-ch1-move-2880, confirm it crashes alone, then bisect the machinery
  (CJ_SWAP_VERIFY_FULLRESTORE=1 restores all 64 MB; drop fds_restore; drop the pad ring; N=1) to see
  which piece breaks the FE context.

## Known limits / next steps

- Only translatable functions are candidates: `scan.txt` NO lines give the reason per function
  (unsupported field types, function pointers, sret returns, macro-generated bodies, ...). Fixing
  the most common reasons in hybridize.py grows the pool.
- Candidates are also limited by what compiles under clang as guest-form C++ (`clean.json`
  reasons); the gen/compile bisection isolates each failure to one function.
- 650 candidates in one build: fine for dual execution (the verifier costs ~70 ms per checked
  call); if a run gets slow, split with `plan --units` or `--max`.
- The confirm stage peels by halving; with many DIVERGES it costs log2(n) replays per culprit.
- `verify_overrides.csv` rows are appended, never rewritten: a function that later changes in
  pcj-decomp keeps its old row. Re-run `report --write-overrides` after a re-verification only
  for new VAs; delete stale rows by hand when a verified function is edited.
