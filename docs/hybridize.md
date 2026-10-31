# hybridize: guest-form C++ generated from pcj-decomp (option B, prototype)

pcj-decomp stays the single source of truth and is never edited for the PC
build. `tools/hybridize/hybridize.py` reads a pcj-decomp unit plus its headers
and generates, on every build, a guest-form copy for the x64 hybrid: views of
the Xbox object layout, plus translated bodies for selected byte-matched
functions. Those bodies replace their recompiled versions through the
checkpoint 1 wrapper (`sub_X` / `sub_X_gen` / `sub_X_enter`, `CJ_NATIVE_SWAP`).

## Toolchain and approach

- clang 18.1.3 (Ubuntu) + libclang 18, with python bindings `pip install clang==18.1.8`.
  Everything is AST based. There is no regex over C++ (the only regexes read
  clang's layout dump and the generated text).
- The unit is parsed as `-target i386-pc-windows-msvc -fms-extensions
  -fms-compatibility -fms-compatibility-version=13.10`. That gives MSVC record
  layout and MSVC name mangling. `tools/hybridize/shim/` holds declaration-only
  `string.h`/`math.h`/`stdio.h`/`stdlib.h`/`float.h` so units that include
  them parse. They are parse-only and never compiled into the game.
- Layout truth: clang's MSVC layout of the pcj-decomp headers.
  `-fdump-record-layouts` (with `sizeof()` forced on every complete record)
  supplies base offsets, vfptr offsets and sizes. `Type.get_offset` supplies
  field offsets. Each class is cross-checked against `symbols/classes.json`.
  A size mismatch becomes a `// NOTE` in the output; one was found, XItem,
  0x160 vs 372, and XItem is a stand-in class in the decomp.
- Symbols: callee VA = `functions.csv[cursor.mangled_name]`. Global VA =
  `symbols.csv[mangled]`. Virtual slot = index of the callee's mangled name in
  `classes.json` `vtable.slots` of the static class (walking primary bases).
  Dispatch vs. qualified call = `clang_Cursor_isDynamicCall`.

### What is generated (`build/hybrid/`, gitignored, rebuilt by CMake)

- `hy_<Unit>.cpp`, per unit:
  - `namespace gv` mirrors the original names, so type names inside a body need
    no rewriting. Each record gets a packed view struct with explicit padding:
    `uint32_t __vfptr` where MSVC puts a vfptr, bases flattened at their MSVC
    offsets, pointer fields as `GuestPtr<T>` (4 bytes, pointer-to-pointer as
    `GuestPtr<GuestPtr<T>>`), enums as `enum : int32_t`, MSVC bitfield units as
    `struct __attribute__((ms_struct))`, and one view per template
    specialization (`EXStack_T_unsigned_int`). A `static_assert` checks every
    field offset and every `sizeof`; the assert on the EXPlacementItem
    bitfields caught a real bug during development. The guest vftable VA from
    classes.json is a comment on the struct.
  - Translated bodies as `static R N_<Class>__<Method>_<va>(gv::Class *self, ...)`.
    The original source text is spliced and only the AST nodes that need it
    are rewritten:
    - `this` becomes `self`, and an implicit member `m_x` becomes `self->m_x`.
    - A virtual call becomes `cjg::vcall<R>(obj, vfoff, slot, args...)`, which
      reads the guest vptr, reads the slot, sets ecx to the subobject that
      holds the vptr, and calls through `recomp_lookup_manual` / `recomp_lookup`.
    - A call to a non-native function becomes `cjg::call_this|call_cdecl|call_std<R>(VA, ...)`.
      The arguments are written to the guest stack in push order, a return VA
      is pushed, ecx is set, esp is checked against the callee's cleanup
      convention and then restored, and the result is read from eax, edx:eax
      or x87 ST0 (popped).
    - Reference parameters become `cjg::ref()`, which passes a guest VA and
      aborts if the address is a host address. By-value records become
      `cjg::byval()`.
    - An implicit derived-to-base conversion (libclang's UNEXPOSED cast)
      becomes `cjg::up<Base>(p, msvc_offset)`.
    - Globals and static members become `*cjg::g<T>(VA)`.
    - An enumerator becomes its value.
    - An inline header function with no VA, which the original inlined, is
      translated recursively as another `N_` function. An implicit
      `operator=` becomes `cjg::assign` (a memberwise copy of the view).
  - Guest-ABI entries `extern "C" void cjn_<VA>()`: this from ecx, arguments
    from the guest frame (a by-value record argument is bound by reference in
    place, because the MSVC callee owns that copy), and the result to eax,
    edx:eax or the x87 stack.
- `hybrid_swap.c` is the switch for every target, and is also the
  `--exclude-manual` input of tools.recomp. `CJ_NATIVE_SWAP=1` selects
  native for all targets, or a comma list of VAs selects native for those.
  The file has entry hooks and per-call checks (esp, ebx/esi/edi), plus a
  state hash that does not depend on addresses. Words that look like guest
  heap or contiguous pointers are hashed as a token, and the first 0x80 bytes
  behind each such pointer are hashed too. The hash covers `this` (up to
  0x400 bytes), the argument words, and eax.
- `manifest.json`.

Hand-written runtime (reused by every unit): `src/native/hybrid/guest.h`
(checkpoint 1), `src/native/hybrid/guest_rt.h` (marshalling, dispatch,
`GuestPtr<void>`), and `src/native/hybrid/guest_rt.c` (the guest-register
side, which needs the C `recomp_types.h`).

### Build wiring

- `tools/hybridize/targets.txt` lists the swapped functions (`unit=FEScript 001077C0 00107A40` ...).
- CMake option `CJ_HYBRID_GEN` (default ON) adds a custom command that reruns
  the tool whenever the tool, `targets.txt`, the listed pcj-decomp units or any
  pcj-decomp header changes. OFF falls back to checkpoint 1's hand-copied
  `native_swap.c` + `fe_script_native.cpp`.
- Changing the VA set in targets.txt also changes `hybrid_swap.c`. That
  requires a tools.recomp regeneration with `--exclude-manual
  ../build/hybrid/hybrid_swap.c` (RUN.md), which took about 10 min on this
  host. Changing only the bodies requires just a normal rebuild.

## Targets (all MATCH in pcj-decomp, all reached during boot)

Reachability check: I set gdb tbreaks on all 1103 translatable MATCH
functions with a call (`set disable-randomization off` is required, otherwise
the guest memory window does not map). 156 of them were reached in a 60 s
boot.

| VA | function | why |
|---|---|---|
| 0x000DB970 | FELayout::Update | (a) loop over `m_pChildren[i]` (FEObject** field, GuestPtr<GuestPtr<FEObject>>), a virtual call on each child (slot 19), then a virtual call on self (slot 33, PlayEvent with an enum argument) |
| 0x00107A40 | FEObjectScript::SetPosition(EXVector) | (b) a non-native call by VA (0x00012B80, EXPlacementItem method, with a by-reference argument into the guest frame), upcast XItem to EXPlacementItem, field deref `m_pOwner->m_Flags &= ~0x20`, a 16-byte by-value argument, `ret 0x10` |
| 0x001B97B0 | EXBaseApp::Update | extra: the per-frame app update, 8 virtual calls (slots 29 to 36) that run the whole game frame underneath, plus a global write (0x0039A01E) |
| 0x001077C0 | FEObjectScript::AddMarker | checkpoint 1's function, now through the generated path (implicit `FEScriptMarker::operator=` becomes `cjg::assign`) |

I checked the vtable slots against the original disassembly
(`pcj-decomp/build/*.dis`): FELayout::Update has `call [edx+0x4c]` (slot 19)
and `call [eax+0x84]` (slot 33). EXBaseApp::Update has `call [eax+0x80]`
(slot 32) and `call [edx+0x84]` (slot 33).

## A/B result (RECOMP_ABI_CHECK on, 50 s watchdog, separate runs per mode)

There were two full rounds of 2 off and 2 on runs. Round 2 used the final
tool state. In the round 2 table, gen means calls that entered the generated
body.

| run | AddMarker calls / gen / hash | SetPosition calls / gen / hash | FELayout::Update calls / gen | EXBaseApp::Update calls / gen / hash | esp_bad, csr_bad | [ABI] | [HYB] |
|---|---|---|---|---|---|---|---|
| hoff1 | 2 / 2 / 2cc10acd | 4 / 4 / aaed3f00 | 64 / 64 | 32 / 32 / 2eb2aed5 | 0, 0 | 4 | 0 |
| hoff2 | 2 / 2 / fd633e17 | 4 / 4 / 57b3fe69 | 64 / 64 | 32 / 32 / 2eb2aed5 | 0, 0 | 4 | 0 |
| hon1  | 2 / 0 / 53b8d43f | 4 / 0 / fa684438 | 64 / 0  | 32 / 0 / 2eb2aed5  | 0, 0 | 4 | 0 |
| hon2  | 2 / 0 / fd633e17 | 4 / 0 / 57b3fe69 | 64 / 0  | 32 / 0 / 2eb2aed5  | 0, 0 | 4 | 0 |

- Native mode: the generated bodies are never entered (gen = 0), there are no
  esp or callee-saved failures, and the generated guest calls report no
  `[HYB]` esp mismatch.
- ABI: there are the same 4 `[ABI]` reports in every run in both modes, all
  in CRT code (sub_002DBA38, 002DBA73, 002DB850, 002DA560). These are the
  same 4 as in checkpoint 1. None is in or under a swapped function.
- EXBaseApp::Update: the run hash is identical in all 8 runs (both rounds,
  both modes).
- AddMarker and SetPosition: the boot heap race (checkpoint 1) gives each run
  one of a few states, and the same classes show up in both modes. Round 2:
  hon2 equals hoff2 exactly. Round 1: hoff1, hoff2 and hon2 all gave
  53b8d43f / fa684438, the same pair hon1 gives in round 2. No native run
  produced a hash that no generated run produced.
- FELayout::Update: the run hash differs between any two runs, including two
  generated runs. The children's first 0x80 bytes contain data that depends
  on timing. In every run and both modes, all logged calls leave the hashed
  state unchanged, and the call count is the same (64).
- Frames: every common flipped frame is byte-identical across all 4 runs of
  round 2 (50 to 52 common frames vs hoff1, 0 differ), and likewise in
  round 1 (52 common, 0 differ).

Verdict: PASS for all four. The generated native code is
indistinguishable from the recompiled code at every observable point that is
deterministic run to run.

## Coverage (whole pcj-decomp, `hybridize.py scan`)

All 216 units (game + enginex; 2 fail to parse under clang and are skipped). Functions are MATCH functions defined in each
unit's .cpp: 4131 functions, 524,896 bytes.

- The tool emits a translation, without hitting an unsupported construct, for
  2480 functions (60%, 284,672 bytes, 54%).
- I compiled a random sample of 80 of those (seed 7) with clang++: 38 compile
  clean (48%).
- Estimate: about 25 to 30% of byte-matched functions (by count and by bytes)
  translate automatically today.
- Most of the compile failures and the top refusals are mechanical, so 60 to
  70% looks reachable with the fixes listed below. The remainder (ctors/new,
  sret, statics) needs runtime work.

Refusals (top): base not resolved 354 (mostly `EXPtrArray<T>` deriving from
`EXDataArray<void*, void*>`, a template-name normalization gap),
constructor calls for locals/temporaries 310, constructors/destructors 199,
new/delete 166, globals without a VA in symbols.csv 144 (statics like
`s_ASCount`, local data), records never completed in the TU 117, UDT return
via hidden pointer (sret) 97, array-typed locals/params 73, taking a
function's address 39, CRT/intrinsics with no VA (atan2...) 31, vtable slot
not in classes.json 25 (abstract classes), unions 18, function pointers 15,
function-local statics 11, variadic 10.

Compile failures in the sample: a whole function produced by a macro, e.g.
`AU_STATE_ENTER_GOTO` (11; should be refused or macro-expanded); nested
classes emitted as namespaces, e.g. `Actor::ActorStack` (10); pcj typedefs
(`u8`/`s8`/`u32`) not emitted (10); member-pointer and cast shapes (6); one
GuestPtr comparison ambiguity.

## Gap closure, round 2 (gaps 1, 3, 4)

### Gap 1: guest-stack frame for locals, temporaries and constructed objects

- Escape analysis (`Gen.escapes`, AST only) runs per translated function before
  emission. A local or parameter escapes if it is the operand of unary `&`, is bound
  to a reference parameter of any call (member operators included; implicit
  `operator=` and implicit copy constructors excluded), is the object of a `.`
  member call, initialises a reference variable, is array typed, or is a record
  whose constructor or destructor is not a no-op (its constructor sees `this`).
  `lvalue_root` sees through `.`, array subscripts, parens, casts and implicit
  conversions. The analysis is conservative: an escaping local only costs guest
  stack space. Everything else stays a host local.
- Frame (`cjg::Frame`, guest_rt.h): if a function needs guest storage, its body
  starts with `cjg::Frame F(size, align);`. That moves g_esp down by the frame
  size, aligned to the largest member alignment (min 4, max 16); the RAII
  destructor restores g_esp, so every return path releases the frame. Guest
  calls made from the body push below the frame. Offsets and alignment are
  fixed at generation time (`Frame.alloc`, clang's MSVC size/align).
- An escaping local becomes `gv::T &x = *F.at<gv::T>(off);` in guest layout
  (pointer locals become `GuestPtr<T>`), so every use of `x` in the spliced
  text stays the same. An escaping by-value parameter of a native function is
  renamed `__p_x` and copied into its frame slot. Swap-target by-value records
  are still bound in place in the caller's guest frame.
- Construction: native translation if possible, else the recompiled constructor
  by VA with `this` = the guest address (`cjg::call_this`). Constructors are
  translated in MSVC order: base constructors (explicit initialiser or default),
  then the class's guest vftable VA from classes.json (`self->__vfptr = VA`), then
  members (initialiser list or default constructor), then the body. Implicit
  default constructors are synthesised the same way, or emit nothing when the
  class has no vfptr and no non-trivial base or member. Implicit copy
  constructors become `cjg::assign`.
- Destruction: an escaping local with a non-trivial destructor gets
  `auto __dtor_x = cjg::defer([&]{ ... });` right after construction, so it runs in
  reverse declaration order on every exit path, before the frame is released.
  Translated destructors write the vptr first and then run member and base
  destructors after the body, also through `defer`.
- Temporaries: a constructor call that is not a variable initialiser
  (`f(EXVector(1,2,3,0))`, functional casts, materialised temporaries) is built in
  a frame slot and yields an lvalue view. A prvalue bound to a reference
  parameter of a guest call (an inlined `operator+` result, or a literal
  `0.001f` for a `const float &`) is spilled into a frame slot (`cjg::spill`).
  Temporaries whose class has a non-trivial destructor are refused (C++
  destroys them at the end of the full expression).
- Default arguments (no source location) are taken from the parameter
  declaration. `T v = T(args)` is constructed in place (copy elided, as in the
  original).
- The runtime check stays and was widened: `cjg::to_guest` and now also
  `host_to_guest` (every `GuestPtr` assignment) abort through `cjg_escape` if a
  host address outside the guest window would reach guest memory.

### Gap 3: class-type returns through a hidden pointer

- Calls: `cjg::call_this_sret / call_cdecl_sret / vcall_sret(slot, ...)`. The slot
  is a guest-frame address and is pushed as the first stack word (after ecx =
  this for `__thiscall`). The recompiled callee fills it, returns it in eax and
  pops it. The call expression is an lvalue view of the slot. MSVC rule as
  before: member functions always return UDTs through the hidden pointer,
  free functions only when the size is not 1, 2, 4 or 8.
- Swap targets that return a UDT: the adapter reads the hidden pointer from
  the first stack word, stores the native result there, and returns it in eax.
  `hybrid_swap.c` counts the extra stack word (`ret 4*(n+1)`).
- Native callees (inline, no VA) still return gv views by value.

### Gap 4: new and delete through the game's allocator

- `new T(args)`: operator new is resolved like MSVC does it (class-specific in
  the hierarchy first, otherwise the global one in the TU). The inline EXMemory.h
  operator new is translated, so it calls `_EXAlloc(size, 0)` by VA (0x001A6A70),
  which is exactly what the original inlines. Then, if the result is non-null,
  the constructor runs (native, or by VA), and that writes the guest vptr.
  Placement `new (p) T` uses p. `new T[n]` is supported only for element types
  without construction (no array cookie).
- `delete p`: if T has a virtual destructor, it calls the scalar deleting
  destructor (`??_G` slot from classes.json) through the guest vftable with flag
  1, after a null check. Otherwise it runs the destructor (native or by VA) and
  then operator delete (EXFree by VA). `delete[]` is supported for
  non-record element types only.
- Symbol fix found on the way: functions.csv also has `??2@YAPAXI@Z` (and other
  inlines) in XDK libraries (d3dx8, dsound, xapilib). The last row used to win,
  so `operator new` resolved to xapilib's copy at 0x00380D3D. Now the first
  game or enginex row wins.

### Coverage (all 216 units, 4131 MATCH functions; sample = 80 OK functions, seed 7, clang++ -fsyntax-only, `hybridize.py sample`)

| state | translatable (count) | bytes | sample compile clean | translate x clean |
|---|---|---|---|---|
| before (round 1) | 2480 / 4131 (60%) | 284,672 (54%) | 38 / 80 (48%) | ~29% |
| + gap 1 (`HY_GAPS=1`) | 2789 (68%) | 332,800 (63%) | 45 / 80 (56%) | ~38% |
| + gap 3 (`HY_GAPS=13`) | 2885 (70%) | 363,200 (69%) | 42 / 80 (52%) | ~36% |
| + gap 4 (`HY_GAPS=134`, default) | 2985 (72%) | 374,128 (71%) | 38 / 80 (48%) | ~35% |

The sample is redrawn from a different OK population at each step, so +/-11
points of it is sampling noise (n = 80). The compile failures are almost all old
gap-2 mechanics: whole-function macros (14), nested-class views (10), missing
typedefs (8), and `GuestPtr = 0` in constructor bodies, which is ambiguous between
`T*` and `nullptr_t` (5, newly reachable now that constructors translate; the fix
is a `GuestPtr::operator=(int)` overload). The first refusal hit per function
shifts: "base not resolved" grew from 354 to 483 because functions that used to
stop at a constructor now reach their views. Of the round-1 OK functions, 20 now
refuse. Those were wrong before: `MsgItem(MSG_ATTRACT)` and similar functional
casts were spliced as text, so they built a host gv object without running the
constructor, and initialised local arrays were host arrays whose address escaped.

Remaining refusals (top, after all three gaps): base not resolved 483, globals without VA 154, records never completed 145,
array-typed signatures 80, CRT/intrinsics 50, function address taken 40,
unions 39, vtable slot not in classes.json 30, temporaries with destructors
(EXString/EXWString) 28, function pointers 18, destructors with no VA and no body
13, arrays of records in new/delete 12, local statics 11, variadics 10.

## Gaps, in priority order (remaining)

1. Mechanical compile fixes (old gap 2), now the largest loss on both counts:
   nested record views (`Actor::ActorStack` emitted as a namespace gives
   "redefinition of 'Actor'"), typedef emission (`u8`), whole-function macros
   (`AU_STATE_ENTER_GOTO`), and template base-name normalisation ("base not
   resolved", the largest refusal).
2. Statics and globals without a VA (old gap 5).
3. Temporaries with non-trivial destructors (EXString, EXWString): this needs
   end-of-full-expression destruction, i.e. a statement-level defer.
4. Arrays of constructed records (locals, `new T[n]` with cookies).
5. Multiple inheritance and non-primary vfptrs: refused in virtual calls and in
   constructors that would have to write a secondary vfptr.
6. x87 precision, inline-with-VA policy, function pointers, unions,
   variadics, function-local statics, exceptions, dynamic_cast (unchanged).

## Layout note vs checkpoint 1

clang's MSVC layout puts FEObjectScript's non-polymorphic base EXDListItem at
**+0x08**, not +0x04. MSVC pads the vfptr to the class alignment, which is 8
because of `unsigned __int64 m_Id`. Checkpoint 1's hand-written
`m_Base // 0x004` was a guess. AddMarker never touches it, so its result was
unaffected. The generated views are now the reference for this. VC7.1 itself
is still not available to confirm it, and confirming it means checking a
matched function that upcasts FEObjectScript* to EXDListItem*.

## Usage

    python3 tools/hybridize/hybridize.py gen unit=FEScript 001077C0 00107A40 unit=FELayout 000DB970 -o build/hybrid
    python3 tools/hybridize/hybridize.py scan -v FEScript FELayout     # per-function OK/NO + reason, flags V(irtual) C(all) P(tr deref)
    tools/swap_ab.sh hon1 1     # A/B as before (CJ_NATIVE_SWAP=1 or a VA list)

## Round 2 in-game trial (not passed: nothing added permanently)

Trial targets reached at boot (gdb tbreaks on the 473 newly translatable functions:
90 reached in a 60 s boot): 0x00107A90 FEObjectScript::TweakPosition (gap 1 + 3:
inline `operator+` -> sret call to 0x00011380 into a frame slot, then a spilled
temporary passed by reference), 0x00107C30 FEObjectScript::Position (gap 1 + 3:
frame temporaries, UDT returned through the adapter's hidden pointer),
0x000F97D0 FEObject::MathmosUpdate (gap 3: 7 sret calls returning ex::RGBA,
256 calls per run), 0x000F9680 FEObject::ScriptAdd (gap 4: `new FEObjectScript`
through _EXAlloc by VA, then the recompiled constructor, which writes the vptr).

A/B (RECOMP_ABI_CHECK on, 50 s, h2off1/h2off2/h2on1/h2on2, logs in run/ab/):
native mode never entered the generated bodies, with esp_bad = csr_bad = 0,
0 `[HYB]`, 0 FATAL (no host address escaped), and the same 4 CRT `[ABI]` reports in
every run. ScriptAdd's run hash is identical in all 4 runs. Position's on1 hash equals
off1 (the other pair differs by heap race class). Every TweakPosition call leaves
the same post-state in both modes. BUT the call counts differ systematically by mode:
TweakPosition reaches 16-31 calls in both off runs and 128-255 in both on runs,
and EXBaseApp::Update reaches 64+ (off) vs 32+ (on). Frames: off2 vs off1 differ in
18 of 64 common frames (the noise floor is higher this round), on1 in 27 of 58,
on2 in 15 of 63. Until a per-function run (`CJ_NATIVE_SWAP=<VA list>`
with `build/pcj-trial8`) shows which target shifts the timing, none of the four
passes, and targets.txt is back to the round-1 four.

State left behind (superseded, see the bisect section below): `src/recomp/gen` was regenerated with the 8-target
hybrid_swap.c (8 wrapped as sub_X_gen). `build/pcj-trial8` is that build;
`work/hyscan/trial8/hybrid/` has its generated sources. `build/pcj` and
`build/hybrid` are the round-1 binary and sources again. Before the next cmake
build, regenerate src/recomp/gen from the 4-target hybrid_swap.c (RUN.md), or
the 4 extra `sub_X` wrappers will be undefined at link time.

## Round 2 bisect (lockstep, frame-count based; docs/determinism.md)

Build state: `src/recomp/gen` is regenerated from the 4-target `build/hybrid/hybrid_swap.c`
again, so `build/` links with the committed targets.txt. The 8-target set now has its own
build dir: `cmake -S . -B build-trial8 -DRECOMP_ABI_CHECK=ON
-DRECOMP_GEN_DIR=$PWD/work/hyscan/trial8/gen -DCJ_TARGETS=$PWD/tools/hybridize/targets-trial8.txt`
(new CMake cache variables `RECOMP_GEN_DIR`, `CJ_TARGETS`; the hybrid sources now go to
`<build dir>/hybrid`). `work/hyscan/trial8/gen` is the 8-target regeneration.

Round-1 A/B on the rebuilt 4-target binary (r1off vs r1on, 100 frames): identical call
counts at every report, EXBaseApp::Update final hash identical (5d920afe114bf835, also in
ls2/ls3), 0 FATAL/[HYB], the same 4 CRT [ABI] reports, frames differ only from f70
(27, the noise floor). PASS.

Per-target runs (`CJ_NATIVE_SWAP=<VA>` on build-trial8, after the selection fix), vs t8off:

| target | calls (all 8 wrappers) | own final hash | frames | verdict |
|---|---|---|---|---|
| 0x00107A90 TweakPosition | TweakPosition 249 vs 25, rest equal | n/a | 21 differ, f70..f90 | FAIL |
| 0x00107C30 Position | all equal | ff63234d = off | 31 differ, f61..f91 | pass (counts, hash) |
| 0x000F97D0 MathmosUpdate | all equal (after the fix below) | noisy in off/off too | 31 differ, f70..f100 | pass |
| 0x000F9680 ScriptAdd | all equal | c8947f14 = off | 31 differ, f70..f100 | pass |

MathmosUpdate: translation bug, fixed. `MultiplyMathmos(m_Colour, OC_TINT)` passes
`m_Colour` by value through ex::RGBA's implicit copy constructor. clang gives an
implicitly defined copy constructor an empty COMPOUND_STMT, so `Gen.implicit` called it
user-written, `ctor_noop` called the empty body a no-op, and `temp_ctor` produced an
uninitialised frame slot: the native code multiplied stack garbage. `implicit()` now
treats a member located at its class's own location as implicit; the call becomes
`cjg::byval(self->m_Colour)`. The other three units only change in view order.

TweakPosition (culprit, not diagnosed yet): in frame 9 the native version is called 249
times on one FEObjectScript instead of 12 + 13 times on two, so the caller's loop condition
sees different state after the call. Candidates to check next: the `Position() + v` chain
(sret call to 0x00011380 inside the translated inline `EXVector::operator+`, returned by
value out of that function's own Frame, then `cjg::spill` into TweakPosition's frame and
passed by reference to 0x00012B80), compared with the recompiled body at call #1 with
`CJ_SWAP_LOG` and a dump of m_pItem's position before/after.

Done in round 3 (below): Position, MathmosUpdate and ScriptAdd are in targets.txt.

## Round 3: 7 targets permanent; TweakPosition root cause (register liveness)

### Promotion

targets.txt now has 7 targets (round 1's four + 0x00107C30 Position, 0x000F97D0
MathmosUpdate, 0x000F9680 ScriptAdd). src/recomp/gen was regenerated (RUN.md, the
`--exclude-manual` input now also holds the CJ_DET wrappers from src/det_file.c, 9 functions
wrapped; regeneration takes about 3 min, not 10). The previous 4-target gen dir is in
work/gen4-backup.

A/B on build/pcj (RECOMP_ABI_CHECK on, CJ_LOCKSTEP=2, CJ_STOP_FRAME=100, CJ_DET_HASH=1), s7off
(`CJ_NATIVE_SWAP=0`) vs s7on (`=1`):

- call counts identical for all 7 targets at every 16-frame report and final (AddMarker 2,
  SetPosition 4, Position 4, FELayout::Update 182, EXBaseApp::Update 100, MathmosUpdate 491,
  ScriptAdd 2);
- s7on: gen-body entries 0, esp_bad = csr_bad = 0, 0 FATAL / [HYB], the same 4 CRT [ABI]
  reports as s7off;
- run hashes: EXBaseApp::Update and ScriptAdd equal; the others differ in the known heap
  race classes (s7off got fd633e17/57b3fe69, s7on 53b8d43f/fa684438, both classes seen in
  generated-only runs before);
- frames: 101 common, 0 differ (better than the usual noise floor from f60/f70).

PASS: the 7 stay. After the EXFileSys::Poll wrapper was added (the final gen dir), s7on2
(`=1`, no CJ_DET) vs s7on: identical call counts at every report, same heap class
(53b8d43f), 19 of 101 frames differ (noise floor), 4 CRT [ABI].

### TweakPosition (0x00107A90): not a translation bug, a register-liveness bug

Traced with gdb on build-trial8 (breakpoints on the swap wrapper, printing `this`, the
by-value v, m_pItem's position at +0xC8 before and after). The first call is identical in
both modes: pos (-1.30, 0.15, 150, 0) + v (1, 0, 0, 0) = (-0.30, 0.15, 150, 0), same eax.
The translation of `m_pItem->SetPosition(m_pItem->Position() + v)` (inline operator+ ->
sret call to 0x00011380 -> EXVector(vec_xyzw) ctor -> cjg::spill -> 0x00012B80) is correct.
After the call, the native run goes to a different return site (0x00108D28 instead of
0x00108C4A again).

The caller is `FEObjectScript::DoObjectAlign` (0x00108730, same object file, not in
pcj-decomp). It keeps xmm1 (0.0) and xmm3 (the remaining step) live across
`call TweakPosition` and reads them right after the call (`comiss xmm3, xmm1` at
0x00108C24/0x00108C5E). VC7.1 knows that the original TweakPosition writes only xmm0 (it
inlines operator+ and SetPosition with xmm0 only). The native version calls the recompiled
`EXPlacementItem::SetPosition`, which loads the vector through xmm0..xmm2, so xmm1 comes back
non-zero and DoObjectAlign's loop runs a different number of times (249 calls instead of 25).
(The original also writes pos+v back into its by-value argument slot; nothing reads it.)

Fix (tools/hybridize/hybridize.py, `orig_writes`): the tool disassembles each target's
original bytes (orig/DEFAULT.XBE, capstone) and collects the scratch registers it may write
(eax/ecx/edx, xmm0..7), transitively through direct calls and tail jumps; an indirect call or
a callee of unknown size means "may write all". The native path of the swap wrapper saves and
restores every scratch register the original never writes (`/* keeps ... */` in
hybrid_swap.c; eax only for void functions). Per target: TweakPosition and SetPosition keep
xmm1..7, Position keeps edx and xmm3..7, AddMarker keeps xmm0..7, the other four keep nothing.
SetPosition (permanent since round 1) had the same latent bug; it passed only because its
callers did not hold xmm values across it.

Verification (build-trial8 rebuilt with the fix, CJ_NATIVE_SWAP=00107A90, 100 lockstep frames,
vs t8off): every wrapper's call count is identical at every report (TweakPosition 25 vs 25,
was 249 vs 25), gen entries 0, esp/csr 0, no [HYB]. Frames differ from f2, but t8off and tp8n
are in different heap race classes (AddMarker 53b8d43f vs fd633e17), and the docs already
show that classes move early frames. So TweakPosition is not in targets.txt yet: the
pass needs a same-class (or deterministic) comparison, plus the regeneration with 8 targets.

Remaining ABI risk this does not cover: x87 stack depth/contents and flags that a caller
might keep across a call; callee-saved-register use is checked (csr_bad) already.

## Round 4: mechanical gaps

Goal: raise the share of byte-matched functions that the tool translates AND that compile, by fixing
the mechanical gaps only (no runtime redesign). Measured with the commands of the Coverage section:
`hybridize.py scan -v <all 216 units>` (now 3972 MATCH functions: the status table has moved since the
4131 above) and `hybridize.py sample <scan listing> --seed N` (80 random OK functions, each generated alone as a
swap target and compiled with `clang++ -std=c++17 -fsyntax-only`). Seeds 7 (the docs' seed) and 11. Each row is
sampled from that row's own OK population, so a sample of 80 is +/-5 points of noise.

| state | translatable | bytes | clean, seed 7 | clean, seed 11 | translate x clean (seed 7 / 11) |
|---|---|---|---|---|---|
| baseline (before round 4) | 2843 / 3972 (71.6%) | 345,840 (69.8%) | 37 / 80 (46%) | 38 / 80 (48%) | 33.1% / 34.0% |
| first pass (fixes 1-6, before review fixes) | 3519 (88.6%) | 420,368 (84.8%) | 73 / 80 (91%) | 74 / 80 (92%) | 80.9% / 81.9% |
| final | 3486 (87.8%) | 405,824 (81.9%) | 79 / 80 (99%) | 80 / 80 (100%) | 86.7% / 87.8% |

The final row is a little below the first pass in translatable count on purpose: functions that used to
"translate" into code that could not work (string literals handed to guest calls, function template
patterns, calls through function pointers) now refuse, and that is what moved the clean rate from 91% to 99%.
Sample failures in the baseline (160 samples, 85 failures): `expected function body` (macro-generated
functions) 25, `redefinition ... as different kind of symbol` (nested classes as namespaces) 26,
`unknown type name` (u8/u32...) 11, ambiguous overload (GuestPtr `= 0` and friends) 8, the rest single cases.
Final: 1 failure in 160 (`float v[3]` initializer list, a gap 1 decl_stmt bug, see below).

### What each fix does

1. **Base not resolved (476 refusals to 0).** libclang shows no children at all for an implicit template
   specialization: no base specifiers and no fields. `EXPtrArray<T>` is the usual victim (its base is
   `EXDataArray<void*, void*>`), and the old code looked the base up in `get_children()`. Now `Gen.base_decls`
   takes the bases from the layout dump (it already lists them by name) and resolves each name through a registry of every
   record type of the unit's AST (`Gen.registry`), and falls back to `flat_layout_only` (vfptrs and nested bases from the layout, fields as padding)
   if the declaration is not reachable. Side effect: `own_fields` uses `Type.get_fields()` for specializations, so
   the views of `EXStack<unsigned>`, `EXFixedArray<char,33>`, `EXFlag<..>` now carry their real fields instead of one
   opaque pad (each with offset static_asserts). `has_vfptr`, `bases`, `rec_members` and `vslot` use the same helpers.
2. **Nested classes (26 of 160 baseline failures to 0).** `Actor::ActorStack` was emitted as `namespace Actor { struct ActorStack }`.
   A record nested in a record is now flattened to a top-level view `Actor__ActorStack` (C++ cannot forward declare a
   nested class, and a pointer to it may be needed before the outer view), and the outer view gets
   `typedef gv::Actor__ActorStack ActorStack;` so the spliced `Actor::ActorStack` still resolves. Member function
   bodies also get `using ActorStack = gv::Actor__ActorStack;` for the bare nested names (and class-scope typedefs)
   that they use (`scope_aliases`). `qname(c, real=True)` keeps the source spelling for classes.json lookups.
   Unnamed structs/unions used as a member type get a stable name (`__anon_<line>_<col>`), unnamed enums become
   `enum : int32_t`.
3. **pcj typedefs (11 baseline failures to 0).** `namespace gv` now starts with `typedef int8_t s8; ... typedef float f32;`
   (include/types.h). File-scope typedefs that a body names (`StateFn`, `EXListApplyFunc`) are aliased inside the
   function to their guest spelling (function pointer typedefs become `uint32_t`). The file preamble defines
   `__int8/__int16/__int32/__int64` (MSVC keywords that the spliced text spells) and the `i64`/`ui64` literal suffixes are rewritten.
4. **Macros (25 baseline failures to 0, 582 functions now translated through it).** The AST shows the expansion but the
   spliced text is the invocation. A function is detected as macro-using (`Gen.uses_macro`: an identifier of its own tokens names a
   `#define` of the unit from `clang -E -dM`, or the extent lies inside an expansion and has no tokens, or the body does not
   start with `{`). Such functions raise `NeedPP`; `gen` and `scan` then redo the unit with `Gen(unit, pp=True)`, which parses
   `clang -E -P` output (every macro expanded, `SM_GOTO`, `NULL`, `EXRUNTIME_CLASS`, ...) and translates the function from that text.
   The generated file says `(preprocessed: macro-using functions)` in its header. Functions that do not use macros never take this
   path, so the 8 targets are unaffected. A function whose body is still not a `{ }` block in the preprocessed unit is refused.
   Function addresses (`&STATE_X` passed to the state machine, 278 refusals after macro expansion) are now the function's guest VA
   (functions.csv), which is what a guest function pointer is; calls through a function pointer stay refused.
5. **`GuestPtr = 0` ambiguity.** `GuestPtr<T>::operator=(int)` (exact match for the literal 0, traps on any other int). Also added:
   `operator=(const T*)` (a `const char *` into a `char *` field), and `GuestPtr<void>::operator=(const void *)`.
6. **Records never completed, globals without VA, arrays.**
   - Records only declared in the unit (pointer-only uses) are emitted as opaque `struct X;` (`Gen.opaque`) instead of
     refusing (refusals 144 to 29; the rest are real by-value uses). Indexing or dereferencing a pointer to a record now wants its view.
   - Globals: a pointer-typed global was read as a host 8-byte pointer (latent bug); globals now use the guest storage type
     (`GuestPtr<T>`, arrays keep their extent), which also removes the 71 array-typed refusals (`EXMemHeap *[4]`).
     File-statics have no symbol at all (internal linkage), so they refuse with that reason (135, not mechanical).
   - Unions: named unions and unnamed unions/structs inside records are emitted as real C++ unions with explicit padding and
     offset asserts; bitfield groups inside them use the same `ms_struct` unit as in views (71 refusals to 0).

Other fixes that came out of compiling the samples (each is also a safety or correctness improvement):
const-qualified reference parameters stay `const &`; a const method on a const object goes through `cjg::unconst`;
a record prvalue used as `this` or converted to a base is spilled into a frame slot (no address of a temporary);
explicit pointer casts go through `cjg::pcast` (GuestPtr operands, MSVC base offset for derived/base casts);
pointer to integer casts go through `cjg::to_guest` (host address outside the guest window still aborts);
`(T)x` casts to a record type become the converting constructor call; `cjg::ret<R>` reads a UDT of 1/2/4/8 bytes from eax:edx,
and the adapter writes one back; a nested enum makes its outer record a dependency of the view order; view order is now
deterministic (the old order depended on `PYTHONHASHSEED`, so `build/hybrid` differed between runs).

New clean refusals instead of code that could not work: string literal / array arguments to guest calls and constructors
(a host pointer cannot be passed; 56), function template instantiations whose body is the dependent pattern and names `T` (44),
calls through a function pointer, casts to a record without a converting constructor (10).
Safety checks are unchanged: `host_to_guest` / `cjg::to_guest` / `cjg::ref` still abort through `cjg_escape` on a host address,
every static_assert on offsets and sizes is still emitted and was added for the new view shapes, and nothing was relaxed to make code compile.

### Remaining refusals (final scan, 486 of 3972)

| count | reason |
|---|---|
| 135 | file-static without a VA (internal linkage, no symbol; needs a statics table) |
| 56 | string literal / array argument to a guest call (50) or constructor (6) |
| 44 | function template instantiation (body is the dependent pattern) |
| 37 | CRT/intrinsic callee with no VA and no body |
| 32 | record never completed in the unit, used by value (29 + 3) |
| 30 | vtable slot not in classes.json (abstract classes) |
| 27 | temporaries with destructors (EXString 23, EXWString 4) |
| 22 | call through a pointer / function pointer |
| 17 | global without VA in symbols.csv (statics members) |
| 12 | array new/delete of records (8 + 4) |
| 11 | function-local static |
| 10 | variadic; 10 cast to a record without a converting constructor |

### 8 targets and determinism

The 8 targets in targets.txt (unchanged) generate the same bodies and adapters. The generated files are not byte-identical to before, and
cannot be, because the old output order depended on hash seeds. Compared block by block (views as a set, function section as text):
`cjn_` adapters and all target bodies are identical; the differences are (a) the views of template specializations
(`EXStack_T_unsigned_int`, `EXFixedArray_T_char_33`, `EXFlag_T_*`) now have fields and offset asserts instead of one pad, with the same sizes,
(b) the typedef / `__int64` preamble, (c) in FEScript two inline helpers (`EXVector::EXVector(const vec_xyzw&)`, `operator+`) take `const &` and
`TweakPosition` wraps the object of a const method in `cjg::unconst` (a no-op cast). `hybrid_swap.c` and `manifest.json` are byte-identical, so
`src/recomp/gen` did not need regenerating. `build/pcj` rebuilt cleanly (`cmake --build build -j2`).

Determinism check on the final build, `CJ_DET=1 CJ_LOCKSTEP=2 CJ_STOP_FRAME=40 CJ_DET_HASH=1`, `CJ_NATIVE_SWAP=0` (r4offF) vs `=1` (r4onF),
`tools/det_cmp.py`: `.data` 40/40 frames identical, heap 40/40, flipped frames 41/41 identical, all call counts equal, esp_bad = csr_bad = 0, no `[HYB]` or FATAL;
the swap-report lines differ only in the gen-body entry counter (A enters the generated body by design) and in the known MathmosUpdate
run hash (docs/determinism.md round 5). The final-build swap-off run equals the first post-change swap-off run (`.data`, heap, swap reports 0 differ).
One earlier swap-off run (r4off2, under CPU load from a concurrent scan) had a different heap hash at frame 1 only; two repeats were identical.

### Known gaps left

- A local `float v[3]` with an initializer: the gap 1 decl_stmt copy emits `const float __i_v[3] = 3;`, which is not a valid array initializer (EXScriptThread::UpdateThread_AnimDepthController, 1 of 160 samples).
- Bitfield-only unnamed structs are supported, but anonymous structs with a vfptr or bases are refused.
- The sample only proves `-fsyntax-only` on the generated file; running the new bodies needs targets and the A/B procedure above.
- A macro-using function whose preprocessed text still contains host-only constructs refuses with the usual reason; the macros of
  `ActorUtil` state functions now translate (the guest function VAs are passed as state pointers); none of them has been run in a boot yet.
- `scan -v` lines now end with `@unit`, which `sample` uses to pick the unit (inline header functions are listed under the unit that defines them).

## Round 5: TweakPosition promoted; adapters mirror the original prologue

TweakPosition (0x00107A90) passed the CJ_DET 300-frame gate and is in targets.txt (8 targets;
docs/determinism.md round 5). The cjn_ adapters now repeat the original function's leading
`push reg` / `sub esp, imm` on the guest stack (`orig_prologue`, `HY_PROLOGUE=0` turns it
off), so guest callees of a native body run at the original esp. Without that, EXBaseApp::Update
(`push esi`) shifted the whole frame's stack residue by 4 bytes, and the game copies uninitialised
stack words into the sound command buffer and other buffers (the on/off `.data` difference).

## Round 6: the native body runs on the original's stack (call sites, saves, registers)

The promote.py batch (docs/promote.md) showed that most boot-time failures were not translation bugs but guest stack
residue: the game copies uninitialised stack words into `.data` (the sound command buffer at 0x004E25D8 again), so a
native body must leave the guest stack exactly as the original does. Traced on XBFileManager::FileRead (0x00285180) with
`CJ_SWAP_DUMP` (raw `.data` before/after each call, both modes): calls #1..#11 identical, #12 pre differed in 33 words,
all of them a copy of the guest stack. Four separate causes, each fixed in the generator and runtime:

1. **Return address.** `cjg_call_raw` pushed 0x0000C0DE where the original pushes its call site's return VA
   (0x00285238 after `call ReadFile`). Now every guest call carries `cjg::Site(va, ret, depth)` (`cjg::VSlot(slot, ret,
   depth)` for virtual calls). `orig_sites()` disassembles the original and lists its call instructions in address order,
   keyed by callee VA (`call imm`) or vtable byte offset (`call [reg+disp]`, or `call reg` after `mov reg,[x+disp]`); the
   generator emits a `CJ_SITE_<n>` token per call and `Gen.resolve_sites` gives the k-th call to a key in the swap target's
   text the k-th site with that key. Calls in shared helper bodies stay 0 (old behaviour). In the 120-function batch:
   240 call sites resolved, 281 not (helpers, or callees the original inlined).
2. **esp at the call.** VC7.1 shrink-wraps: FileRead saves ebx/edi in the prologue but ebp and esi only after its first
   calls, so the call depth is 12, 16 or 20 bytes. Only the leading pushes were mirrored, and the body's `cjg::Frame`
   (for `read`) went 4 bytes below them: the original keeps `read` in the `push ecx` slot. Now:
   - `orig_frame()` scans the prologue up to the first call/branch and skips interleaved instructions (a push of a register
     written since entry ends it); the first `push ecx` run / `sub esp` is the local area, and a swap target's Frame is
     placed there if it fits (`cjg::Frame::Area`, `cjg_area_set`), so esp does not move;
   - each site has its depth (linear scan of push/pop/sub/add esp; argument pushes since the last call are subtracted,
     a cdecl `add esp` right after the call is not counted twice). The adapter registers a frame context
     (`cjg_ctx_enter(esp_in, saves, regs)`); `cjg_call_raw` runs a call with a depth at `esp_in - depth` (only ever deeper
     than the current esp, so native frame data is never overwritten) and first writes the late saves (`push ebp`/`push esi`
     slots with the caller's register values) that the original has on the stack by then. `cjg_ctx_leave` writes all
     of them, as the original's epilogue leaves them behind.
3. **Callee-saved register values.** The recompiled callees save ebx/esi/edi/ebp in their own frames; the original holds
   `this` in edi and its arguments in ebx/ebp/esi at those calls, the native body held the caller's values. The same scan
   tracks `mov reg, ecx` (this) and `mov reg, [esp+n]` (argument word esp_in+n); per site (keyed by return VA) the adapter's
   `regs[]` table sets them for the call and restores them after (the csr check still sees them unchanged).
4. **Callees the original calls vs inlines.** A callee with a VA that the original really calls is now called by VA
   (`Gen.orig_calls`), also when it is another swap target (it then goes through its own switch, with its own return VA
   and arguments on the guest stack). Before, a target calling a target, or a constructor calling a base constructor with
   a translatable body, ran it natively with nothing on the guest stack.

Result on FileRead: `.data` before/after its first 14 calls identical except one word (was 33 words from #12); the next
difference is a pointer vs size word in a deeper callee frame (not diagnosed). Each of the three steps changed every
core target's residue too; the core-only baselines stayed deterministic (base_a = base_b over 300 frames: `.data`, heap,
301 images) after each rebuild.

Also fixed:
- **Conversion operators lost `this`** (failure class 2 of the batch). `strcmp(pName, s.m_Name)` calls
  `EXFixedArray<char,15>::operator char *` (0x00248FE0); libclang reports it as `CONVERSION_FUNCTION`, which the member-call
  path did not accept, so it became `cjg::call_cdecl<char *>(0x00248fe0u)` with no object and the result was garbage
  (the docs' first guess, a guest `char *` taken as a host pointer, was wrong: `cjg::ret<T*>` already converts). Now
  `cjg::call_this<char *>(..., &(s.m_Name))`. EXDebugTree::SectionHandle no longer crashes: in isolation `.data` and heap
  match for 300 frames, but one frame image differs at f2 (failure class 4).

Not reproduced exactly (remaining residue sources): spills and temporaries the original keeps in its frame, early-return
paths that skip late saves, deferred cdecl cleanup across calls, registers holding computed values, calls inside shared
helper bodies, and callees the original inlined but the native body calls by VA (template members that do not translate,
EXWatcher's `EXArray` constructor and SetSize).

## Round 7: callee cleanup from the binary, native blame stack

- `callee_pop(va)`: bytes popped by the callee, from every `ret`/`ret N` of the original function (following tail `jmp imm`, depth 3); unknown
  when the rets differ, a `jmp reg/[mem]` occurs or the size is not known. Emitted as the 4th `cjg::Site` field (`-1` = unknown, then the declared
  signature decides); `cjg_call_raw` checks `esp == esp1 - 4n + pop`, where `esp1` is the esp of the call itself (after the call site's depth).
  Before, the check used the esp before the depth adjustment, which produced the `[HYB] esp after call` false positives (docs/promote.md round 5).
- Adapters call `cjg_nat_enter(VA)` / `cjg_nat_leave()`; runtime reports end with `native=cjn_<VA> chain=...`.
- `HY_INLINE_HDR=1` (off by default, untested): a header-defined callee that the swap target's original never calls is translated inline.
