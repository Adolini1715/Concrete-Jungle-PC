# GL back end: pixel pipeline, render targets as textures, and the spike fix

`src/gl_backend.c` (CJ_GL=1) + toolkit patches `patches/v0.15/0007-backend-pixel-pipeline.patch`
and `0008-backend-vertex-programs.patch` (later 0010, 0011).
Follows docs/gpu-census.md section 5 (which listed what the back end lacked).

## Teal ground = clear colour format; W-buffering (patch 0011, 2026-10-20)

- **Blue/teal ground (fixed): not fog.** Root cause: SET_COLOR_CLEAR_VALUE is in the colour
  surface's own format (xemu `pgraph_get_clear_color`; XDK D3D's Clear converts the D3DCOLOR to the
  render target's format). Every level-1 frame clears the 256x256 **R5G6B5** light target at
  0x00EA9000 with **0x0000FFFF = 5:6:5 white**; the executor passed it on as A8R8G8B8, i.e. cyan
  with alpha 0, and the world draws (stage 1 = that target, and the full-screen pass that
  writes destination alpha from it) tinted everything not explicitly lit teal.
  `patches/v0.15/0011-clear-colour-surface-format.patch`: `clear_argb()` in nv2a_pb_exec.c decodes
  the clear value per SURFACE_FORMAT colour type (X1R5G5B5, R5G6B5, G8B8; 32-bit unchanged) for
  the back end and the software path (`RECOMP_CLEAR_RAW=1` = the old reading). Found with
  `CJ_GL_DIAG_AT` (stage-1 texel view was solid cyan) and the `[CLEAR]` drawlog line.
  Frames: `19_level1_teal_before_g01500.png` (run F1) / `20_level1_clearfix_after_g01500.png`
  (run F5): pavement now grey, buildings neutral blue-grey night; Predator unchanged.
- **The fog-colour writes are real, and harmless.** `RECOMP_TRACE_METHOD=2A8 RECOMP_TRACE_FLIP=1560`
  (new, segment-scan walk) shows 326 writes in one gameplay flip, every one a proper
  single-method header `0x000402A8` (count 1, incrementing, top level, preceded by
  BLEND_ENABLE/BLEND_DFACTOR) with float params (1.0, 0.996, 240.0, 15466.6, ...): the engine
  writes them, no decode slip. All 1371 draws of that flip have FOG_ENABLE 0 and a final combiner
  of `D = spare0` (CW0 0x0000000C), so fog colour is never read; `CJ_GL_FOG_ARGB` is moot.
- **W-buffering (implemented)**: CONTROL0 bit 16 (Z_PERSPECTIVE) is set on almost every 3D draw
  (~1.9M over the route). Each combiner/legacy fragment shader now writes
  `gl_FragDepth = clamp(w / zscale)` when it is set, with w = 1/gl_FragCoord.w (the
  perspective-correct vertex w, as D3D scales it into depth units) and zscale = SET_CLIP_MAX
  (16777215 for D24), as xemu's psh.c does for an integer depth buffer; otherwise
  `gl_FragCoord.z` (so every program writes depth on every path). Depth is now linear in view
  distance (constant 1/2^24 of the range) instead of hyperbolic, which is what the title's depth
  tests and decal offsets were tuned for. Result: the level renders intact with it (no missing or
  inverted geometry, run F4/F5/F6). A/B with a still camera (`CJ_GL_WBUF=2`: on for even flips
  only; run F6, `tools/route.py F6 --skip-for 230`): `21_level1_wbuf_on_g01601.png` /
  `21_level1_wbuf_off_g01602.png`. In that view the difference is below the rain animation
  (adjacent frames 2.1-2.6 mean abs diff, same-mode pairs 3.0, no on/off pattern): **no z-fighting
  was visible in either mode there, so the shimmer reduction is not yet demonstrated on a frame**;
  check far facades while the camera moves (the field report) on a real GPU.
  Cost: writing gl_FragDepth disables early-z (llvmpipe flips/s unchanged within noise).

| env | effect |
|---|---|
| `CJ_GL_WBUF=0` / `=2` | z-buffer depth always / W-buffer on even flips only (A/B) |
| `RECOMP_CLEAR_RAW=1` | clear value read as A8R8G8B8 regardless of surface format (old) |
| `RECOMP_TRACE_METHOD=<hex>` [`RECOMP_TRACE_FLIP=<n>`] | print each write of that method: pushbuffer address, header, index/count, increment mode, CALL, 4 words before the header (segment scan only) |
| `CJ_GL_DIAG_AT=<flip>` | flips +1..+4 output one combiner input each (stage-0 texel, diffuse, stage-1 texel, specular), dumped as g(flip+2..+5) |
| `CJ_GL_DUMP_AT=<flip>` | also dump that flip and the next five |
| `CJ_GL_DRAWLOG` | VS batches also print fog enable/mode/params, final combiner words and SET_CLIP_MAX |

Remaining colour question: the reference shows warm orange-yellow street light pools; this
plaza at the level start reads neutral/cool. Whether lamp pools exist further on (they should be
drawn into the same 0x00EA9000 light target, cm RGB) was not checked.

## Clip state, texture sampling, movie audio (patches 0009-0010, 2026-10-20)

Field report (Whisky on an M-series Mac): radar map spilling out of the radar, blocky textures,
blue ground, stuttering intro movies. `patches/v0.15/0010-backend-texture-sampling-clip-state.patch`
extends `Nv2aSurface` (clip origin), `Nv2aRenderState` (window clip, polygon offset, CONTROL0,
clear rect) and `Nv2aTexture` (CONTROL0, border colour/source, cube) and adds
`nv2a_backend_texture_level()`.

- **Radar (fixed)**: not stencil (stencil is never enabled in level 1, `stc 0` on every draw) and
  not the window clip (SET_WINDOW_CLIP is always one inclusive rectangle the size of the surface).
  The HUD draws the radar through a **116x116 surface clip at (70, 75)** (SET_SURFACE_CLIP origin),
  which the back end never saw: it only knew the clip size, so the 256x256 map quad drew over the
  whole screen (the orange blocks at the top). Now every draw is scissored to the surface clip
  (origin + size) and every clear to SET_CLEAR_RECT (else the clip). The window clip and polygon
  offset are implemented too (`atest()` discards outside/inside up to 8 window rectangles, scaled by
  AA; `glPolygonOffset`), both inactive in this title so far (counters in the `[GL] tex:` line).
  Frames: `18_radar_before_crop.png` / `18_radar_after_crop.png`, `16_level1_gameplay_before_g01620.png`,
  `17_level1_gameplay_after_g01620.png`.
- **Mip levels and sampler state (implemented; little visible change in level 1)**: every level the
  format word declares is decoded (per level, swizzled at its own size, DXT in blocks) and uploaded
  with `GL_TEXTURE_MAX_LEVEL`; MIN/MAG/mip filter from SET_TEXTURE_FILTER (point, linear, the four
  mip modes; convolution as linear), LOD bias (bits 0-12, signed 5.8), CONTROL0 LOD clamps and
  MAX_ANISOTROPY (GL_EXT_texture_filter_anisotropic), wrap modes incl. border colour
  (`GL_CLAMP_TO_BORDER` when FORMAT's BORDER_SOURCE is colour). FBO textures stay single-level.
  Finding: the gameplay frame's textures **all declare one level** (`lv1` on 4233 of 4233 stage
  binds at flip 1560; 6 mip-chained textures over the whole route), filter 0x0204/0x0206 (linear,
  mip point/linear), anisotropy 1. So the soft/blocky pavement is the content's level 0 magnified,
  not missing mips; `CJ_GL_ANISO=n` forces anisotropy, `CJ_GL_LOD_BIAS=x` adds a bias,
  `CJ_GL_MIPS=0` restores level 0 only.
- **Blue ground (fixed in patch 0011, see above; the analysis below was the wrong lead)**: checked and ruled out: texture/RTT channel order (decode is 0xAARRGGBB
  uploaded as BGRA/8_8_8_8_REV; FBOs sample RGBA as written), vertex D3DCOLOR order, combiner
  factors (ARGB, as D3D sends them). Fog colour: XDK `D3DDevice_SetRenderState_FogColor`
  (0x002E91F0) swaps R and B before writing SET_FOG_COLOR, so R is in bits 0-7 as the back end reads
  it; **the CPU combiner path (`nv2a_rc_unpack(s_gpu.fog_color)`) reads it as ARGB, i.e. the
  software renderer has fog red/blue swapped**, so it is not a valid reference for this. In level 1
  SET_FOG_COLOR changes per draw to values that look like floats (0x46719FFA, 0x3F7C0400,
  0x00004A57): something besides D3D's setter writes method 0x2A8 (an engine pushbuffer or a
  method-decode slip in the executor). `CJ_GL_FOG_ARGB=1` reads it as ARGB for an A/B (not yet compared: run R3 ended on the loading screen).
- **Depth / shimmer**: SET_CONTROL0 = 0x00110001 on 3D draws: bit 16 (Z_PERSPECTIVE_ENABLE, i.e.
  **W-buffering**) is on; the back end writes a z-buffer depth, so depth precision differs from the
  hardware (distant z-fighting). Not fixed (needs a linear-w depth via gl_FragDepth or a reversed
  float depth buffer). Polygon offset: never enabled in level 1. Level 1 renders into a 640x480
  back buffer (no AA), so the downsample is not involved there.

| env | effect |
|---|---|
| `CJ_GL_MIPS=0` | level 0 only (old behaviour) |
| `CJ_GL_ANISO=n` | force anisotropy n (default: the title's MAX_ANISOTROPY) |
| `CJ_GL_LOD_BIAS=x` | add x to the title's LOD bias |
| `CJ_GL_CLIPSCISSOR=0` / `CJ_GL_WCLIP=0` / `CJ_GL_POLYOFF=0` | ignore surface clip origin / window clip / polygon offset |
| `CJ_GL_FOG_ARGB=1` | read SET_FOG_COLOR as ARGB (diagnostic) |
| `CJ_GL_DRAWLOG=<flip>` | now prints clip origin, stencil func/ops, CONTROL0, polygon offset, window clip, fog colour, per-stage levels/filter/CONTROL0/address, and `[CLEAR]` lines with the clear rect |

## GPU vertex programs (patch 0008)

NV2A vertex programs now run in GLSL on the GL side; the CPU interpreter is the fallback.

| | CPU-VS (run C2, `CJ_GL_VSH=0`) | GPU-VS (run V2, default) | |
|---|---|---|---|
| front-end fly-in (window to flip 704) | 4.47 | 8.69 | x1.9 |
| main menu, New Game dialogs, save, hub | 3.91 (3.6-4.3) | 6.34 (5.9-7.9) | x1.6 |
| level load + mission intro (`--skip`, mostly loading screens) | 9.3 | 9.7 | |
| level 1 gameplay, player control | 4.47 (4.0-5.4) | 6.69 (5.3-8.5) | x1.5 |

flips/s, harmonic mean of the route's 64-flip windows, llvmpipe on 2 cores, both runs the same
binary and `tools/route.py <tag> --until level --after 500 --dump-every 120 --skip`. (GPU-VS shows
more batches/flip, 2500 vs 1900 in the menu: the CPU path only calls draw() when a triangle
survives its clip, the GPU path sends every program batch.)

**Coverage**: 21 distinct programs over the whole route (3 to 61 instructions: D3D's screen-space
pass-throughs, the lit/skinned model programs reading v0-v14, the water/foliage ones), all 21
translated, 2.31M batches on the GPU, **0 fell back** (`[GL] vsh:` line every 16 flips). Every MAC
op (mov mul add mad dp3 dph dp4 dst min max slt sge arl, 14/15 as mov) and ILU op (mov rcp rcc rsq
exp log lit), a0-relative constant reads, all output registers (oPos oD0 oD1 oFog oPts oB0 oB1
oT0-3) are handled. The only refusal rule (never hit): a program that writes c[] while
CXT_WRITE_EN is on stays on the CPU; so does a program whose GLSL does not compile.

**A/B check** (`CJ_GL_VSH_AB=1`, run AB1, `run/AB1.cmp.txt`): every program batch is drawn twice in
the same run, GPU-VS into the real surfaces and the CPU path's triangles into a shadow FBO set (all
other draws and clears go to both), so both images show the exact same game state; dumps are
`gNNNNN.ppm` / `gcNNNNN.ppm`, compared by `tools/frame_cmp.py <dir> --ab`. 46 flips (every 30th) from boot to
gameplay: FMV/title/loading 0 difference; fly-in mean abs diff 0.05-0.15 (0.1-0.25% of pixels off
by more than 8); menus 0.002-0.06 (PSNR 48-60 dB, <= 0.11% > 8); mission intro 0-0.0004 (max 1);
gameplay 0.01-0.04 (PSNR 44-57 dB, <= 0.08% > 8). The differences are edge pixels and +-1-8
texel-level noise in textured windows (texcoords interpolated from float vs the CPU's float with a
different operation order, depth clamped per fragment instead of per vertex), no missing or extra
geometry and no culling/winding change. Frames: `docs/frames/14_difficulty_select_{gpuvs,cpuvs}_g00810.png`,
`15_level1_gameplay_{gpuvs,cpuvs}_g01380.png`.

### How

- **Interface (patch 0008)**: `Nv2aBackend.vsh_accept(program, ninstr, cxt_write, &inputs)` is
  asked for every vertex-program batch (TRANSFORM_EXECUTION_MODE = program) before the CPU touches
  it. 0: CPU path as before. Nonzero: the executor fetches only the inputs the program reads, as
  float4 rows (`fetch_attr`, every format decoded exactly as the CPU path does; attributes with no
  array give their SET_VERTEX_DATA value), one row per vertex of the index range when it is dense
  (indexed meshes) else one per index, decomposes the primitive into a triangle list exactly as the
  CPU path (odd strip triangles swap their first two vertices, quads, quad strips, fans), and calls
  `draw()` with `Nv2aBatch.vsh` (`Nv2aVshBatch`: program, c[0..191], rows, indices, AA scale, oT0
  texel scale, near plane z, fog mode). Return 2 also runs the CPU path and draws its result with
  `vsh_twin = 1` (the A/B mode). `nv2a_vsh_interp` gained three accessors (program from the start
  slot to FINAL, constants, CXT_WRITE). Fixed-function and screen-space batches are unchanged.
- **Translator** (`gen_vs`, src/gl_backend.c): a rule-for-rule port of the toolkit's interpreter
  (the oracle) rather than of d3d8_vsh.c or the fork's HLSL generator: both of those translate D3D
  shader tokens (`d3d8_vsh_parse`), while what reaches the executor is NV2A microcode, the same
  field table the interpreter decodes. Same quirks: paired MAC+ILU writes the ILU result to R1, R12
  is oPos, mux 0 reads a constant, out-of-range constants read c[0], rcp(0) = +inf, rcc clamps,
  arl = floor(x + 0.001), lit's pow(0, 0) = 1, unwritten colours/texcoords (0,0,0,1).
- **Post-program** (the executor's job on the CPU path, now in the VS epilogue): oD0/oD1 clamped
  and quantised to 8 bits like `pack_color`, fog factor from oFog.x (`fog_factor` ported, mode and
  params as uniforms), oPos.w = +0 -> 1, x/y times the AA scale, then `gl_Position = (ndc * w, w)`
  with ndc from the surface-pixel position (y flipped once, as the screen-space VS). That is the
  same affine image of clip space the CPU near-plane clipper works in, so **clipping and culling
  need nothing special**: `gl_ClipDistance[0] = w - 1e-3` and `[1] = (z - c[59].z) * w` (the CPU
  clipper's two planes) with `GL_DEPTH_CLAMP` on (no GL near/far clip; depth clamped to [0,1] as
  the old VS clamped z/zscale). Winding is that of the executor's triangle list in window space,
  so the existing cull pass-through applies unchanged.
- **Caching**: one VS per program content (FNV hash + full compare; the previous program is
  compared first, which hits almost always), linked with the combiner FS per (program, combiner
  key) pair (512-entry LRU; 78 linked programs over the route). c[] is a std140 UBO at binding 0,
  re-sent (orphaning `glBufferData`, no llvmpipe stall) only when its 3 KB differ from the last
  upload. Inputs go in one interleaved float4 VBO, indices in an element buffer, in their own VAO.

| env | effect |
|---|---|
| `CJ_GL_VSH=0` | vertex programs on the CPU interpreter (the old path) |
| `CJ_GL_VSH_AB=1` | A/B: GPU-VS to the real surfaces, the CPU path to shadow surfaces, both dumped (`<prefix>c` for CPU) |
| `CJ_GL_SHADER_LOG=1` | also prints each generated vertex shader |

## Result

| | before (run pad3, docs/frames/04, 06) | now |
|---|---|---|
| main menu / sub-menus | 3D scene only, no text, no highlight, no logo after the fly-in; shard/spike triangles everywhere | text, highlight, logo, button legend, dialogs all drawn (frames 07, 08, 12); geometry intact |
| level 1 | spikes, mostly black (stage 1 / RT sampled stale memory) | mission intro (in-engine cutscene) with lighting, foliage, stained glass, subtitles; pause menu; **gameplay with player control**: Predator, fountain, water, rain, HUD (health/energy, radar, weapon) (frames 09-11, 13). Run L5 reached control unattended (`--skip` presses A through the intro lines) |
| route | blind, live presses by hand | `tools/route.py` (unattended, keyed on `[STATE]`) |

## What was wrong (in order of visual impact)

1. **"Spike" triangles were strip winding, not clipping.** The executor decomposed
   TRIANGLE_STRIP as (i, i+1, i+2) for every i, so every odd triangle had reversed winding.
   The software rasteriser does not cull, so it never showed; GL culls, and dropped every
   other triangle of every strip: saw-tooth shards on every model and building. Fixed in the
   executor (odd triangles swap their first two vertices, both the program path and the
   screen-space path; patch 0007). The CPU near-plane clip and the `rhw` use were checked and
   are fine.
2. **Cull face was inverted in the back end.** The FBO holds the image upright (guest row 0 at
   the top, NDC y flipped once), so window-space winding equals the NV2A's; the old code
   flipped `glFrontFace` "because y is flipped" and culled every front face. With strips
   fixed, that still culled all menu text/logo/legend quads (D3D draws them with CULL on) and
   showed model back faces (the hazy over-bright look of the first combiner run). Now passed
   straight through. `CJ_GL_DBG=2` (cull off) was the A/B that found it.
3. **Inline-array vertex size** (executor): shorts (types 1/5) were sized as floats and the
   packed normal as `4*size`. Not visible in the menus checked, fixed anyway (patch 0007).

## Implemented

### Render targets as textures (priority 1)
- Every texture stage looks up its guest address (masked to 28 bits so 0x8xxxxxxx and
  0x0xxxxxxx compare equal) against the FBOs drawn or cleared in the last 30 flips
  (`RTT_LIVE_FLIPS`). A hit inside a surface (not only its base) gives a sub-rect origin
  `x0 = (off - va) % pitch / bpp`, `y0 = (off - va) / pitch`.
- Coordinates: one per-stage transform `GL = texel * scale + offset` with the y flip and
  the FBO size folded in (`u_tx`); swizzled/DXT stages arrive normalised and are taken to
  texels first (`u_tn`, `u_twh`), linear ones arrive in texels. Swizzled vs linear does not
  matter for an FBO (it is never swizzled host-side); the format does: R5G6B5 / X8R8G8B8 /
  X1R5G5B5 targets (0x03 0x05 0x07 0x11 0x1C 0x1E) sample alpha = 1 (`u_aone`).
- A stage that names the surface currently being drawn is not bound (feedback); it falls
  back to decoding guest memory.
- Bound: the bloom chain (640x480 -> 160x120 -> 80x60 -> 40x30 -> 20x15 -> 10x7 and the
  320x240 recombine, 9-tap additive blurs), the 256x256 R5G6B5 shadow/light target at
  0x00EA9000 on stage 1 of nearly every 3D draw, the in-level 128/256 targets. In the menu
  ~85% of draws sample an FBO (`rtt binds` in the `[GL] flip` line).
- FBO grow keeps guest row 0 at the top (the old blit put it at the bottom).

### Texture stages 0-3 + register combiners (priority 2)
- Patch 0007 extends `Nv2aBatch` minimally: `ext` (per-vertex oD1, fog factor, oT0-oT3 with
  q, as `Nv2aVertexExt`, parallel to `vertices`), `stages[4]` (`Nv2aTexture` or NULL),
  `rc` (the executor's own `Nv2aCombiner`, NULL until the title programs the combiners),
  `fog_color`, `clip_plane_mode`. Screen-space-path batches send `ext = NULL` and draw with
  the old stage-0 x diffuse shader.
- GLSL generator ported rule for rule from the toolkit's `nv2a_combiner.c` (input mappings,
  dot products, mux on r0.a, output scale/bias, blue-to-alpha, per-stage or shared c0/c1, the
  final combiner with V1R0 sum and E*F, r0.a seeded from t0.a). Texture stage modes:
  PROJECT2D/3D (divide by q), PASSTHRU, CLIPPLANE (discard per `clip_plane_mode`), NONE.
  One program per key (all ICW/OCW words, final words, control, stage program, used
  clip-plane bits, which stages are bound); constants are uniforms, so constant changes
  never compile. 128-entry LRU cache; the whole menu + level uses 10-25 programs
  (`CJ_GL_SHADER_LOG=1` prints each generated shader).

### Spike triangles (priority 3)
See "What was wrong" 1 and 2.

### Route + regression check (priority 4)
- `tools/route.py <tag> --until menu|level [--after N] [--dump-every N] [--skip]`: starts Xvfb +
  run.sh, Start-repeat through FMVs, then reacts to `[STATE]`: A at fe=Main (re-pressed
  every 20 s), A through New_Game (difficulty, save dialog), at the "overwrite previous
  save?" prompt (New_Game/13, defaults to No) up+A, in the hub down+A to Chapter Select
  (backs out of any other sub-menu and moves on), A at Story_Level_Select, then waits for
  level_loaded+control and runs N more flips (`--skip`: A every 8 s, which steps through the
  mission intro to player control around 260 flips after the level loads). About 5 min of
  wall time from boot to gameplay. Prints milestones and windowed flips/s.
  The emulated HDD keeps the save between runs (run/save), hence the overwrite prompt.
- `tools/gl_vs_sw.sh <tag> [N]` runs the first N frames under `CJ_DET=1 CJ_LOCKSTEP=2`
  (frame-count time, so both renderers see the same content) with the GL back end and with
  the software NV2A, and `tools/frame_cmp.py` reports mean abs diff / PSNR per 5th flip
  (nearest of flips N-3..N+1, since the dump numbering differs by one or two).
  The old run/sw frames are unusable for this (no CJ_DET; the 0.6 fps software run shows
  black where the GL run shows the FMV).

## Diagnostics added (all off by default)

| env | effect |
|---|---|
| `CJ_GL_RTT=0` / `CJ_GL_RC=0` | old behaviour: never sample FBOs / stage 0 x diffuse only |
| `CJ_GL_DBG=1` | depth test off for draws that do not write depth; `=2` culling off; `=3` both |
| `CJ_GL_SURF_DIAG=n` | every nth flip: each surface drawn, draws, legacy-path draws, RTT draws, and the presented one |
| `CJ_GL_DRAWLOG=<flip>` | one line per draw in that flip: surface, combiner control / stage program, blend, depth, alpha, cull, first vertex, stage textures |
| `CJ_GL_SHADER_LOG=1` | print each generated combiner shader |
| `RECOMP_FETCH_DIAG=1` (toolkit) | log vertex-attribute fetch failures (none in the menu) |

Present: the presented surface is the last one drawn whose pitch is >= 640 pixels and whose
clip is >= 400 rows (it used to be "FBO >= 640x480", and the 320x240 bloom target, which
shares an address with an earlier 640x480 target and so has a 640x480 FBO, was presented:
the blurred level intro in run L3). The blit takes only the largest clip drawn into it in
that flip (a menu drawn into the grown 1280x960 back buffer was shown in a quarter of the
screen; a HUD widget's 116x116 clip must not count either).

## Frame rate before patch 0008 (llvmpipe, 2 cores, CPU vertex programs)

| phase (run L4, tools/route.py) | flips | batches/flip | flips/s (windowed, mean) | pad3, old back end |
|---|---|---|---|---|
| FMV / logos (2D) | 64-350 | 4-5 | 18-21 (6-9 in windows with FMV decode) | 11.4 (20-23 steady) |
| title / front-end fade | 384-512 | 900 | 13-21 | 8.5 |
| loading screen | 576-640 | 41-65 | 4-19 | 18.3 |
| front-end fly-in | 704 | 2270 | 4.1 | 4.7 |
| main menu, New Game dialogs, save, hub | 768-1150 | 1850-2350 | 3.8 (3.6-4.0) | 4.4 |
| level 1 load + mission intro cutscene | 1216-2160 | 64-1014 (avg 508) | 6.7 (4.3-10.6) | 6.0 (791 batches/flip) |
| level 1 gameplay, player control (run L5) | 1431-1856 | 950-1210 | 5.3 (4.6-6.1) | not reached |

The menu is about 10% slower than before: twice the draws survive culling (text, front faces),
every batch uploads the 72-byte-per-vertex `Nv2aVertexExt` stream, and up to four textures are
bound. Still dominated by the CPU vertex-program interpreter (now on the GPU: see the top section). 21 combiner
programs were generated over the whole run; 1.03M of 1.67M draws sampled an FBO.

Regression check `tools/gl_vs_sw.sh cmp1 40` (CJ_DET, frames 5-40, intro FMV): GL and the
software NV2A agree to a mean absolute difference of 0.0 (PSNR 72 dB to identical) on every
compared frame. It covers the 2D path only: without pad input the deterministic run stays in
the FMV, and the software renderer is too slow (0.6 fps) to reach 3D.

## What remains (ranked)

1. **Per-batch CPU cost now dominates** (2500 batches/flip in the menu, ~6 flips/s): attribute
   fetch into float4 rows on the CPU, the executor's pushbuffer decode, and llvmpipe's per-draw
   overhead (state validation, three orphaned buffers, uniforms per draw). Next steps in order of
   payoff: upload guest vertex arrays as they are (GL formats for float/short/ubyte/D3DCOLOR via
   BGRA, the packed normal unpacked in the VS) keyed by address+hash instead of re-fetching;
   skip batches whose state did not change between draws; on a real GPU driver this per-draw cost
   is far smaller than on llvmpipe, so measure there before optimising further.
2. **Stage modes the generator leaves out**: cube maps (mode 3, white now), bump-env and
   dot-product modes (zero now), as in the CPU path. Check with `CJ_GL_SHADER_LOG` which
   in-level effects (cloak, heat vision, water) use them.
3. **Mip levels and filters**: done (see the top section); W-buffering (CONTROL0 bit 16) implemented (patch 0011 section).
4. **RTT edge cases**: liveness is a 30-flip window, not a write tracker (a CPU upload into
   memory an old target used would be shadowed by the FBO); a stage that samples the
   target being drawn decodes stale guest memory; depth surfaces as textures
   (`SetRenderState_ShadowFunc`) are not handled; guest readback (`LockRect`,
   `XBBlitBackBufferToTexture` CPU paths) still sees stale memory.
5. **Present**: AA resolve is a linear blit; the PVIDEO overlay / gamma ramp are missing; the
   front buffer is guessed (last big surface drawn), not taken from the flip.
6. **HUD radar**: fixed (surface clip origin scissor, see the top section).
7. **Vertex programs, small**: fixed-function batches (none seen in this title) still transform on
   the CPU; points/lines are not drawn by either path; a program writing c[] with CXT_WRITE on
   falls back (never seen); oPts (point size) is computed but unused.
8. Route: the hub cursor position after New Game is assumed (Training); other entry
   points cycle through sub-menus until Chapter Select.

## Frames (640x480 PNG, docs/frames/)

| before (old back end, run pad3) | after (this back end) |
|---|---|
| `04_main_menu_3d_g00810.png` (no text, spikes) | `12_main_menu_gl_g00790.png` (main menu), `07_difficulty_select_gl_g00780.png` (highlight), `08_hub_menu_gl_g01110.png` (hub) |
| CPU-VS vs GPU-VS, same flip of one A/B run | `14_difficulty_select_cpuvs_g00810.png` / `14_..._gpuvs_g00810.png`, `15_level1_gameplay_cpuvs_g01380.png` / `15_..._gpuvs_g01380.png` |
| `06_level1_ingame_g02490.png` (spikes, black) | `13_level1_gameplay_gl_g01590.png` (player control), `09_level1_intro_cutscene_gl_g01500.png`, `11_level1_intro_predator_gl_g01890.png` (mission intro), `10_level1_pause_menu_gl_g01680.png` |
