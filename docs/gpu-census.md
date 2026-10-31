# GPU census and first hardware back end

Goal: start the hardware-graphics path. Three parts: what the game asks the GPU
for (census), what apexJCL's fork already covers (D3D11/Metal), and the cheapest
way to exercise a non-software back end in this Linux container.

## Tools added

| piece | where | default |
|---|---|---|
| `RECOMP_GPU_CENSUS=<file>` (`RECOMP_GPU_CENSUS_EVERY=n`, default 32 flips) | `xboxrecomp/src/kernel/nv2a_gpu_census.c`, one call at the top of `nv2a_pb_exec_method`; `patches/v0.15/0006-gpu-census.patch` | off; read-only shadow of the method stream, never touches executor state |
| `CJ_GL=1` OpenGL 3.3 back end (`CJ_GL_DUMP=<prefix>` writes each presented frame as PPM, `CJ_GL_TEXCACHE=0`) | `src/gl_backend.c` (registers itself from a constructor through v0.15 `nv2a_backend.h`); CMake links SDL2 + epoxy | off |
| static D3D census script | `tools/d3d_census.py` (map + `work/disasm/xrefs.json`), output summarised below | |

Census file format is in the header of `nv2a_gpu_census.c` (M methods, O object
classes, P vertex programs, R combiner configs, T textures, S surfaces, V vertex
attribute formats, D primitive types, X counters).

## 1. Census

### Runtime coverage (honest scope)

(Superseded for menus and 3D by section 5, which drives the game past the intro
with a scripted pad.) Runs: software executor 200 s (`run/census/`, 196 flips) and the GL back end
240 s (`run/gl2/`, 3488 flips). Both stop inside the intro movie: boot, loading
screen, Sierra logo, intro FMV. **No menu or 3D gameplay was reached** without pad
input (the FMV advances slowly and is not skipped headless). So the runtime census
is the 2D/FMV path only; the 3D picture below comes from the static D3D census.

### Runtime numbers (GL run, 3488 flips)

| item | value |
|---|---|
| distinct methods | 369 (subch 0 = Kelvin 3D; objects bound: subch0 0x0D, 1 0x0E, 2 0x10, 3 0x11, 5 0x12) |
| draws | 17 436, all `SET_TRANSFORM_EXECUTION_MODE` = program (D3D's screen-space pass-through programs; 0 fixed-function) |
| vertex programs | 2 (3 and 6 instructions) |
| register-combiner configs | 2, both 1 stage |
| primitive types | QUADS (7) and QUAD_STRIP (8); all vertex data via `INLINE_ARRAY` (362k words); 0 indexed draws |
| vertex attributes | v0 float3, v2 D3DCOLOR, v3 float2 |
| textures sampled | SZ_A8R8G8B8 16x16, DXT (0x0C) 64x64/64x16 with mips, **YUY2 linear 640x480** (the FMV, 0x24), linear 32-bit 640x480 |
| colour surfaces | 2, both 640x480 A8R8G8B8 pitch 2560 (0x03400000, 0x0352C000), no AA |
| render-to-texture draws | 0 |
| fences / reports | `BACK_END_WRITE_SEMAPHORE_RELEASE` 0x1D70 per kick, FLIP_STALL/INCREMENT per frame, no GET_REPORT in this span |
| bulk | program uploads 251k words, constant uploads 362k words (D3D re-uploads the pass-through program and c[] every quad) |

### Static D3D census (what the 3D game will need)

D3D8 is statically linked (737 d3d8 functions in the map); 92 are called from game
or engine code, plus 10 D3DX and 2 XGraphics functions. Ranked by call sites:

- **Vertex shaders**: `CreateVertexShader` 530 sites, `SetVertexShaderConstant*Fast` 129, `LoadVertexShader` 37, `SetVertexShader` 26, `SetShaderConstantMode` 2. The 3D game is vertex-program driven: a CPU vertex interpreter is the bottleneck to remove.
- **Pixel shaders**: `SetPixelShader` 9, `CreatePixelShader` 2, `SetPixelShaderConstant` 4, `SetRenderState_PSTextureModes` 15: register-combiner programs (more than stage 0 x diffuse).
- **Textures**: `SetTexture` 81, `SetPalette` 7 (P8), `SetTextureState_BumpEnv` 13 (bump env map), `BorderColor` 15, `ColorKeyColor` 13, `TexCoordIndex` 13, `XGSwizzleRect` 3, `XGSetTextureHeader` 5 (movie texture).
- **Render to texture / readback**: `GetSurfaceLevel2` 18, `SetRenderTarget` 2, `GetRenderTarget2` 12, `GetDepthStencilSurface2` 16, `GetBackBuffer2` 6, `CreateSurface2` 2, `D3DTexture_LockRect` 17, `D3DSurface_LockRect` 1; engine functions `StartRenderToTexture`, `XBBlitBackBufferToTexture`, `XBBlitWindowToTexture`, `XBCreateBackBufferTexture`, `ShadowTexture`, `XBGenerateShadowProjectionVector` (shadow maps, `SetRenderState_ShadowFunc` 15).
- **Depth / stencil**: `StencilEnable`/`StencilFail` 15 each, `SetupWBufferProjection` (W-buffer), `SetDepthClipPlanes` 3, `ZBias` 15, `OcclusionCullEnable` 16 (no explicit visibility tests found).
- **Raw pushbuffer / sync**: `BeginPush`/`EndPush` 2 (engine writes methods itself), `InsertCallback` 3, `BlockUntilIdle` 2, `BlockUntilNotBusy` 9, `SetVerticalBlankCallback` 2.
- **Display**: `EnableOverlay`/`UpdateOverlay`/`GetOverlayUpdateStatus` (PVIDEO overlay), `SetSoftDisplayFilter`, `SetGammaRamp` 3, `YuvEnable` 17, `SetScreenSpaceOffset`, `MultiSample*` render states 15 each.
- Fixed-function lighting/transform states (`TwoSidedLighting`, `NormalizeNormals`, `VertexBlend` 15 each) are set but D3D compiles them to programs; the runtime span saw 0 fixed-function draws.

### Ranked gaps

| rank | need | v0.15 `nv2a_backend.h` (our GL back end) | fork D3D11/Metal |
|---|---|---|---|
| 1 | vertex programs on the GPU (530 creates) | missing: executor runs them on the CPU and hands pixels | yes (HLSL/MSL from `d3d8_vsh_parse`) |
| 2 | register combiners, texture stages 1-3, bump env, PSTextureModes | missing (stage 0 x diffuse only) | yes (HLSL/MSL combiners, BUMPENVMAP) |
| 3 | render-to-texture + shadow maps (`StartRenderToTexture`, `ShadowTexture`, blits) | missing: textures are always decoded from guest memory, which a GPU surface never reaches | partial: `rt_texture` binds a rendered surface; no guest readback, shadow compare present |
| 4 | CPU `LockRect` of rendered surfaces / back buffer blits | missing | missing ("not yet: guest readback"; Metal has `sync_guest`) |
| 5 | YUY2 FMV texture, DXT, P8 | yes via `nv2a_backend_decode_texture` (verified: FMV renders) | yes |
| 6 | stencil, depth, blend, alpha, cull, colour mask | yes (passed through, GL enums) | yes |
| 7 | W-buffer, depth clip planes, z bias, fog | z passed as-is; no fog | fog, clip planes, CLIPPLANE kill |
| 8 | PVIDEO overlay, gamma ramp, soft display filter | missing | missing |
| 9 | per-quad program/constant re-upload churn | n/a (CPU) | memoised constant buffers |

## 2. apexJCL fork (PR #184)

- Head: `apexJCL/xboxrecomp` branch `blinx2/portability` = upstream `refs/pull/184/head`
  `06c97d1` (draft index PR, "not meant to merge"). Fetched into
  `/home/claude/xbr-fork` (branches `pr184`, `main` = v0.15 6376eb8); pcj-native/xboxrecomp untouched.
- Base: **v0.14.0 (eb0aa1d) + 68 commits**, not v0.15. 351 files, +64.5k/-8.6k lines.
- Seam: a richer interface than v0.15's. `struct nv2a_pb_backend` (`src/kernel/nv2a_pb_state.h`):
  `on_clear`, `on_zclear`, `on_draw(const struct nv2a_pb_draw *)` (raw prim, vertex attribute
  streams, indices or inline data, program flag; the backend reads the decoded register
  state itself), `on_flip(surface, pitch)`, `on_zpass/on_zpass_clear/on_report/on_poll`
  (occlusion queries), `writes_back` and `sync_guest` (guest readback hook). Selected with
  `RECOMP_PB_BACKEND=d3d11|metal`; CPU rasteriser is itself a backend
  (`nv2a_pb_cpu_backend`). Shared, API-free state decode in `nv2a_backend_common.{c,h}`;
  shader front ends split into parse + HLSL + MSL emitters (`d3d8_vsh_*`, `d3d8_combiners_*`),
  so a GLSL/SPIR-V emitter for a Linux backend is a third emitter, not a rewrite.
- Files: `src/d3d/nv2a_pb_d3d11.c` (4076 lines, `#ifdef _WIN32`), `src/d3d/nv2a_pb_metal.m`
  (3234, Apple only). No OpenGL or Vulkan pushbuffer backend; on Linux the fork has only the
  CPU backend + SDL present.
- Applying to us: a trial merge of `pr184` into v0.15 conflicts in **68 files** (apu, input,
  kernel_bridge/hal/thread/path, `nv2a_pb_exec.c` rewritten, `nv2a_vsh_interp.c` deleted in
  the fork but modified in v0.15, flip_hold/host_time add/add, 23 test files). Our patches on
  top of `pr184`: 0001 and 0002 apply; 0003 (det time hooks), 0004 (TIB pool), 0005 (det
  stack hygiene) conflict. The D3D11 backend cannot be cherry-picked alone: it needs the
  fork's executor, state header, common decode, tex_pack and the split shader emitters.
- Build: the PR says the fork branch **does not build with plain MSVC**:
  `nv2a_backend_common.c` and `nv2a_tex_pack.c` use C11 `_Atomic`, which needs
  `/experimental:c11atomics` (fix "queued"). The author builds with clang or MinGW; tested on
  Windows 11 (D3D11), Proton, macOS arm64 (Metal); games BLiNX 2 and Burnout 3.

## 3. Testing strategy and result

Chosen: an OpenGL back end on the existing v0.15 seam, run under Xvfb with Mesa llvmpipe
(LLVM 20, GL 4.5 core). Cheapest because SDL2, epoxy, Xvfb and Mesa were already installed,
it needs no toolkit rebase and no Windows port of pcj (main.c's POSIX fault handler, the
patches and the hybrid build are all Linux). The D3D11-under-Wine route needs the fork rebase
first plus a MinGW cross build of the whole game (mingw-w64 is installable, wine 9.0 is
present), which is several runs of work before a first frame.

Result (same build, same disc, same scene: boot, loading screen, intro FMV, 70 s watchdog,
one run at a time, every flip dumped):

| back end | flips in 70 s | rate | frames |
|---|---|---|---|
| software NV2A (`RECOMP_PB_EXEC=1`) | 77 | toolkit reports 0.59 fps | dumped frames are flat (2,2,2) during the FMV |
| OpenGL / llvmpipe (`CJ_GL=1`) | 962 | 13.9 flips/s (14.8 over 240 s, 3488 flips) | correct FMV frames (run/gl/frames, run/gl2/frames) |

So about 23x on this scene, on a CPU rasteriser behind GL. Caveat: the scene is 5 textured
full-screen quads a frame, so it measures fill and texture sampling, which is exactly where
the software path spends its time; 3D scenes will also pay the CPU vertex-program interpreter,
which this v0.15 seam keeps on the CPU. The software path's FMV frames being blank while the
GL path shows the movie is a separate software-raster bug (not chased).

To reproduce:

    Xvfb :91 -screen 0 1024x768x24 &
    DISPLAY=:91 CJ_GL=1 CJ_GL_DUMP=run/gl/frames/g ./run.sh

### What a Windows build needs

The fork's D3D11 path is the one tested on real Windows. He needs: Windows 10/11 with a
D3D11 GPU, LLVM clang (or MSYS2 MinGW-w64 UCRT) + CMake + Ninja, Python 3 with capstone,
SDL2 dev libs, and `d3dcompiler_47.dll` (ships with Windows). Plain MSVC fails until the
`_Atomic` fix lands (or add `/experimental:c11atomics`). Before that is useful, pcj-native
must be ported off POSIX: `src/main.c` (SIGSEGV/SIGBUS MMIO handler; the toolkit's Windows
path uses VEH), patches 0002-0005 re-done on the fork, and the gen dir regenerated with the
fork's recompiler. On Linux/Steam Deck the native path is the GL (or a future Vulkan)
backend, not D3D11.

## 4. Next steps

1. **Rebase pcj-native onto the fork (1-2 agent runs).** Merge `blinx2/portability` with v0.15
   (or wait for upstream v0.16 if the GPU PRs land), re-do patches 0003-0005, keep the census as
   a backend-agnostic hook, regenerate, CJ_DET A/B to the f13 baseline. Gate: same frames as now
   on the CPU backend.
2. **GL backend on the fork's seam (2-3 runs).** Port `src/gl_backend.c` to
   `struct nv2a_pb_backend`, add a GLSL emitter beside the HLSL/MSL ones (vertex programs +
   combiners), modelled on `nv2a_pb_metal.m` (the non-Windows sibling, which also has
   `sync_guest` for readback). This is the Linux/Steam Deck path and testable here under
   llvmpipe. Gate: FMV + first menu identical to CPU backend, fps logged.
3. **Reach 3D and re-census (1 run, can run in parallel with 1).** Script pad input
   (the fork has `kbm_script.c`; upstream branch `feat/pad-script` is another option) to skip the FMV and enter the first level, rerun
   `RECOMP_GPU_CENSUS` there: vertex program count, combiner configs, RTT/shadow-map draws and
   LockRect readbacks decide whether `sync_guest` readback is needed before the game is playable.

## 5. Past the intro: scripted pad, menus and the first mission (run pad3)

### Input

The toolkit's `RECOMP_PAD_SCRIPT` drives its emulated OHCI controller, whose MMIO trap has
no x86-64 POSIX decoder (`ohci.c`: "no trapped-MMIO decoder for this host"), so on this host
the game never saw a pad. `src/cj_pad.c` replaces the five XAPI calls that
`EXGamePad::Update` (0x0026F310, their only caller, in `recomp_0044.c`) makes, with
`ld --wrap` (CMakeLists.txt; no regeneration): `XGetDevices` reports port 0, `XInputOpen`
returns a fake handle, `XInputGetState` returns the scripted state, `XInputSetState` (rumble)
completes at once, `XInputClose` is a no-op. Inert unless one of these is set:

| env | effect |
|---|---|
| `CJ_PAD_SCRIPT=<t>:<btn>[+<btn>][:<hold>],...` (or `@file`) | timeline in pad polls (1 poll = 1 game frame; `CJ_PAD_UNIT=ms` for wall ms) |
| `CJ_PAD_REPEAT=<btn>,<period>,<hold>` | pulse a button forever (after the script) |
| `CJ_PAD_LIVE=<file>` | lines `<btn>[+<btn>][:<hold>]` appended while running are pressed at the next poll; `stop` ends the repeat |

Buttons: `up down left right start back lthumb rthumb a b x y black white lt rt`, sticks
`lleft lright lup ldown rleft rright rup rdown`. Every poll also runs a state monitor that
logs `[STATE]` on change: FEMainMenu state/substate (names from `Debug_StateName`, 0x000E0FA0),
XApp level-loaded / player-control / mission / cutscene flags.

Route that reached the first mission (GL back end, `CJ_PAD_REPEAT=start,90,6` then live presses):
Start through Sierra/Fox/Eurocom FMVs and the title (each Start press skips one), the 3D front-end
level loads (PREDATOR logo loading screen), city fly-in, **main menu** (`fe=Main`, flip 755);
A = New Game (profile + save created on the emulated HDD, Save_Game substates 3/12/13/16);
in the hub (`fe=Loaded`) the cursor order is [Save, Training, Story, ..., Options]: down, down, A =
Story_Level_Select, A = first story level, `fe=Done` then level load (flip 1848), mission intro,
level_loaded=1, mission=1, then in-level 3D at 1280x960 (flip ~2000-2528, when the run was stopped).
Menu text and highlight are **invisible** in the GL back end (see gaps), so navigation was blind,
steered by the `[STATE]` log.

Frames (640x480 PNG, GL back end): `docs/frames/01_eurocom_fmv_skip_g00330.png`,
`02_title_logo_loading_g00660.png`, `03_frontend_city_intro_g00720.png`,
`04_main_menu_3d_g00810.png`, `05_level1_loading_g01935.png`, `06_level1_ingame_g02490.png`.

Reproduce (helper copies the census every 10 s into `run/<tag>/cs/`):

    Xvfb :91 -screen 0 1024x768x24 &
    DISPLAY=:91 CJ_GL=1 CJ_GL_DUMP=run/p/fr/g CJ_GL_DUMP_EVERY=15 RECOMP_GPU_CENSUS=run/p/census.txt \
      RECOMP_FB_DUMP= CJ_PAD_REPEAT=start,90,6 CJ_PAD_LIVE=run/p/live RECOMP_WATCHDOG_SECS=900 ./run.sh
    # at fe=Main:   printf 'stop\na\n' >> run/p/live ; then a (repeat until fe=Loaded), down, down, a, a
    python3 tools/census_phases.py run/p/cs 0,768,1824,2528

### GL back end changes made for this

- FBOs keyed by surface address only, sized from the pitch (the front end changes the clip to
  540x408 and in-level the back buffer becomes 1280x960; keying by clip size had sent those
  draws to fresh black FBOs), grown with a blit when a larger clip appears. Present uses the
  last >=640x480 surface drawn, not the last surface (an RTT).
- Texture cache keyed by a sampled signature of the raw guest bytes (80 words) instead of
  decoding + hashing every texture on every batch: 3D menu went from 0.83 to 4.7 flips/s.
- `CJ_GL_DUMP_EVERY=n`; per-16-flip windowed rate and batches/flip in the log.

### Census by phase (`tools/census_phases.py run/pad3/cs 0,768,1824,2528`)

| | boot+FMV+title+FE load (0-768) | main menu / front end 3D (768-1824) | level 1 load + intro + in-level (1824-2528) |
|---|---|---|---|
| draws / flip | 594 | 2722 | 774 |
| fixed-function draws | 0 | 0 | 0 |
| vertex programs (distinct) | 13 | 11 | 15 (2 new) |
| dominant program | 22 instr (5491405d, 65%) | same, 86% | same 77%; 6-instr pass-through 20% |
| combiner configs | 8 | 7 | 11; new: **5-stage** (ctl 0x11105, stage program 0x420 = stages 1+2 2D) |
| draws with 2 texture stages (stage prog 0x21) | 62% | 82% | 34% |
| draws sampling a render target | 62% | 84% | 42% |
| colour surfaces drawn | 23 | 96 (menu widgets as small clips of the back buffer) | 48 |
| indexed vs inline words | 5.7M / 3.8M | 48M / 8.4M | 10.5M / 2.7M |
| texture formats sampled (whole run) | DXT1 0x0C, DXT2/3 0x0E, R5G6B5 swizzled 0x05, A8R8G8B8 swizzled 0x06 and linear 0x12, YUY2 0x24, A4R4G4B4 0x04, 0x3F (1 key, not identified); phase split not broken out | | |

What the 3D actually exercises:

1. **Vertex programs everywhere** (0 fixed-function draws in all phases). 15 programs, one
   22-instruction world program does 65-86% of draws. Vertex layout for the world: stride 24,
   float3 position, packed normal (type 6, NORMPACKED3), D3DCOLOR, float2 uv; skinned/other
   meshes add attrs 4-7.
2. **Two texture stages + register combiners on most 3D draws.** Stage 1 samples a
   256x256 R5G6B5 swizzled render target (surface 0x00EA9000, fmt 0x08080213) drawn each frame:
   the projected shadow / light texture (`ShadowTexture`, `XBGenerateShadowProjectionVector`).
   In-level adds a 5-stage combiner with three textures.
3. **Render-to-texture, heavily**: a bloom/glow chain (linear A8R8G8B8 targets 160x120,
   80x60, 40x30, 20x15, 10x7, 320x240, sampled back as 0x12), the 256x256 shadow target, and
   in-level 256x256 and 128x128 A8R8G8B8 swizzled targets (0x03784000, 0x00EC9000;
   reflection / cloak / heat-vision candidates). 42-84% of draws sample something the GPU drew.
4. **4x supersampled back buffer in-level**: the colour surface at 0x03400000 becomes 1280x960
   (the front end and FMV are 640x480), downsampled at present.
5. Depth/stencil clears with explicit rects every frame; no GET_REPORT / occlusion-query
   methods in any phase (none of 0x17C8-0x17D0 seen). CPU LockRect readback is invisible to the
   census (it is a CPU read of guest memory); with the GL back end any such read sees stale memory.

### What the GL back end lacks for a playable first mission (ranked)

(Items 1 and 2 and the spike triangles of item 3 are done: docs/gl-backend.md. The spikes were
strip winding plus an inverted cull face, not the clip.)

1. **Render targets as textures** (rank 3 in the table above becomes rank 1 at runtime): the back end
   decodes textures from guest memory, which GPU-drawn surfaces never reach, so the shadow/light
   texture, bloom chain and in-level effect targets sample stale memory (mostly black). Bind
   the FBO colour texture when a texture's offset matches a live render target (swizzle/format
   aside), or write the FBO back to guest memory on surface switch. This is also the most likely
   reason the **menu text/highlight is invisible** (menus are lit by the same chain), so it gates
   readable menus too.
2. **Texture stages 1-3 and register combiners**: 34-82% of 3D draws use stage 1; today only
   stage 0 x diffuse is drawn, so lighting/shadow passes are wrong or missing. Needs `Nv2aBatch`
   extended (the v0.15 header says so) or the fork's richer seam.
3. **Vertex programs on the GPU**: the executor interprets them on the CPU; at 2000-2700
   batches/flip that, not GL, is now the cost (3D 4.4-6 flips/s on llvmpipe vs 18-23 in 2D).
   Also the CPU path/clip produces long spike triangles in 3D (visible in frames 04 and 06):
   near-plane/w handling in the CPU clip or our `rhw` use.
4. Multisample resolve (1280x960 -> 640x480) and correct present source: works by accident
   (blit with linear filter); the PVIDEO overlay/gamma path is still missing.
5. Guest readback (`LockRect`/`XBBlitBackBufferToTexture`) for any CPU consumer of rendered pixels.

### Frame rate by phase (GL back end, llvmpipe, 2 cores, run pad3)

| phase | flips | batches/flip | flips/s |
|---|---|---|---|
| FMV / logos (2D quads) | 32-336 | 4 | 11.4 (20-23 in steady windows) |
| title / front-end fade (3D behind black) | 368-512 | 900 | 8.5 |
| loading screen (logo) | 528-640 | 62 | 18.3 |
| front-end fly-in | 672-752 | 2190 | 4.7 |
| main menu and sub-menus | 768-1824 | 2009 | 4.4 |
| level 1 loading | 1904-1984 | 97 | 8.1 |
| level 1 intro / in-level | 2000-2528 | 791 | 6.0 |
| same menu, before the texture-cache change (run pad2) | 672-768 | ~2200 | 0.83 |

Software NV2A (`RECOMP_PB_EXEC` without CJ_GL) runs the FMV at 0.59 fps (section 3); it was
not run into the menus here (at that rate the front end would take hours of wall time to reach).

### Next steps

1. RTT in the GL back end (bind FBO textures by guest offset, including the swizzled 256x256
   targets) and re-check whether menu text appears; then stages 1-3 + combiners via an
   extended `Nv2aBatch` (cheap on v0.15) or the fork seam (docs above).
2. Chase the spike triangles (CPU clip / rhw) on frame 06's scene.
3. Turn the blind menu route into a `CJ_PAD_SCRIPT` keyed on `[STATE]` (or add `CJ_PAD_WAIT=fe=Main`
   style waits) so a run reaches gameplay unattended, and add CJ_LOCKSTEP for repeatable timing.
4. Measure in-level gameplay with player control (this run stopped in the mission intro area).
