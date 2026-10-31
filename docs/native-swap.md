# Native function swap (checkpoint 1)

One function reconstructed in pcj-decomp replaces its recompiled body in the
running game, selected at startup, with the x64 runtime unchanged.

## Toolkit version

xboxrecomp v0.15.0 (6376eb8). Our two patches re-apply with `git apply -3`
(only context drift in kernel_path.c); the refreshed copies are
`patches/v0.15/`. Gate: v0.15 boot vs v0.14 boot, same 50 s headless run, 33
common flipped frames, 1 differs (f00002, which also differs between two v0.14
runs). Fallback: checkout eb0aa1d + `patches/000*.patch` + regenerate.

## Target

`0x001077C0 FEObjectScript::AddMarker(FEScriptMarker &)`, MATCH in
pcj-decomp (src/game/FEScript.cpp:117). 66 bytes, leaf, `__thiscall`, `ret 4`,
returns `this`. It writes guest memory: copies a 24-byte marker into
`m_pMarkers[m_nMarkers]` (pointer at +0x24) and increments `m_nMarkers` (+0x28).
Chosen from 282 small leaf MATCH functions with no call, x87 or string ops by
setting gdb tbreaks on all of them during a boot: 11 were reached, and this was
the one with the most stores and a pointer field.

## Files

- `src/native/native_swap.c`: passed to `tools.recomp --exclude-manual`.
  `extern void sub_001077C0_gen(void);` renames the generated body. `sub_001077C0`
  is the wrapper reached by direct calls and dispatch. `sub_001077C0_enter` is
  the entry hook inside the generated body, so its count must be 0 in native
  mode. The wrapper logs the first `CJ_SWAP_LOG` calls (default 48) and prints
  checkpoints at calls 2^k and every 1000: call count, gen-body entries,
  appends, exact-esp failures (esp must come back +8), return failures (eax must
  equal this), and a running FNV hash over the pre/post state hash plus
  eax/ecx/edx. It also checks that ebx/esi/edi are preserved.
- Adapter: `this = g_ecx`, `&marker = MEM32(g_esp+4)`, calls C++, sets `g_eax`,
  reproduces the original's ecx/edx scratch values, then `g_esp += 8`.
- `src/native/fe_script_native.cpp`: the C++ implementation. The method body is
  the pcj-decomp text verbatim. The class is the pcj-decomp declaration
  rewritten with the single-source macros, with static_asserts on the Xbox
  offsets.
- `src/native/hybrid/guest.h`: `GuestPtr<T>` (a 4-byte guest VA with pointer
  syntax), `guest_to_host` and `host_to_guest`, plus the `GPTR(T)`,
  `EX_VIRTUAL` and `EX_VPTR` macros.
- `tools/swap_ab.sh <tag> <0|1> [bin]`: one headless boot. Output goes to
  run/ab/.
- CMake: `-DRECOMP_ABI_CHECK=ON` is applied to both the game and the toolkit.

`CJ_NATIVE_SWAP=1` selects native. It is read once on the first call; the
default is generated. To remove the swap, regenerate without `--exclude-manual`
and drop the two sources.

## A/B result (v0.15, RECOMP_ABI_CHECK on, 50 s watchdog, 2 runs per mode)

| run | calls | gen-body entries | esp_bad | ret_bad | run_hash |
|---|---|---|---|---|---|
| off3 | 2 | 2 | 0 | 0 | 47f0dab602980f11 |
| off4 | 2 | 2 | 0 | 0 | 47f0dab602980f11 |
| on3  | 2 | 0 | 0 | 0 | 6d0155bcc24dd659 (*) |
| on4  | 2 | 0 | 0 | 0 | 47f0dab602980f11 |

The per-call logs of off3, off4 and on4 are byte-identical (same this, arg,
pre and post hashes). The v0.14 runs off1/off2/on1/on2 also all give
47f0dab602980f11.

(*) on3 got different heap addresses before the first AddMarker call: its
`[HEAP] #2` allocation order is swapped at log line ~85. That is a runtime
thread race at boot, not caused by the swap. Its two calls still appended
correctly, with esp and eax exact. The same race shows up in frame hashes
between two swap-off runs.

ABI check: 4 `[ABI]` reports in every run, both modes. All 4 are in CRT/library
code (sub_002DBA38, 002DBA73, 002DB850, 002DA560). None is in sub_001077C0.

Limitation: AddMarker runs only twice during the first 50 s of boot, so the
state comparison covers 2 calls per run.

## Can one pcj-decomp .cpp build both VC7.1 (matching) and the x64 hybrid?

Tested concretely:

1. The real `src/game/FEScript.cpp` compiles for x64 under clang with a
   4-line prelude (`__int64`, calling-convention keywords). Only 4 errors
   remain: `operator new(unsigned int)` in EXMemory.h and EXArray.h. So the
   language itself is not the obstacle.
2. The layout is. With the unmodified headers on x64, FEObjectScript has
   m_Id at 0x18 (needs 0x10), m_pMarkers at 0x38 (needs 0x24), m_nMarkers at
   0x40 (needs 0x28), and sizeof 0x3F0 (needs 0x3B8). There are two causes:
   pointers are 8 bytes, and the host vptr is 8 bytes. (Side note:
   clang -m32 with the Itanium ABI is also wrong, m_Id at 0x0C, because
   i386 SysV aligns `__int64` to 4. Only an MSVC-layout target matches.)
3. With `GPTR(T)` pointer fields, the verbatim AddMarker body compiles
   unchanged, runs in the game, and gives identical results.
   `m_pMarkers[m_nMarkers] = m;` works through `GuestPtr::operator[]`.
4. What broke or needs more than a macro:
   - Virtuals: a polymorphic class cannot keep `virtual` on x64 without
     gaining an 8-byte host vptr. `EX_VIRTUAL` has to drop to nothing and the
     guest vptr becomes a `uint32_t` field.
   - Base-class placement: MSVC places the vptr ahead of the
     non-polymorphic base EXDListItem. Once the hybrid has no virtuals, it
     can only reproduce that by turning the base into a member (`m_Base`).
     Source-incompatible: `EXDListItem*` upcasts and inherited member access
     (`Next()`) stop working, so this needs `#ifdef` in the class head, not a
     field macro.
   - Virtual calls in bodies (`p->Foo()`) must become guest dispatch
     (MEM32(this) -> slot -> recomp_lookup -> call with a guest stack frame).
     That rewrites every call site, or a generated per-class thunk header
     per vtable slot. Not implemented here: AddMarker has no virtual calls.
   - Host pointers into guest memory: `host_to_guest` is valid only for
     addresses inside the guest window. Taking the address of a local or
     static native object and storing it in a GPTR field silently corrupts
     guest state. Locals that guest code sees must be allocated on the guest
     stack or heap.
   - `new`/`delete` must allocate from the guest heap, and constructors must
     write the guest vtable VA. Typed allocation therefore needs
     per-class vtable VAs, which the decomp knows but does not expose as
     data.
   - Matching risk: `GPTR(T)` expands to `T*` under _M_IX86, so codegen for
     plain pointer fields is unchanged. But every class head with virtuals
     or a base needs the `#ifdef` form, and VC7.1 is the judge. Not
     verified here: no VC7.1 in this environment, and pcj-decomp was not
     edited.

Verdict: partially yes. Leaf and data-only methods on non-polymorphic or
field-only access can come from the same source with a pointer-field macro,
as AddMarker shows. For polymorphic classes, the class declaration and every
virtual call site need per-class adaptation (the `#ifdef` class head plus
dispatch thunks), so for object-heavy code the single source becomes the
method bodies only, not the headers.
