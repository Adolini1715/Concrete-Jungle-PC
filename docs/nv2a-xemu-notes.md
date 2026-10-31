# NV2A behaviour notes from xemu (for gl_backend.c)

Source studied: xemu master 25e1ee2 (2026-10-22), hw/xbox/nv2a (GPL-2). These
notes describe hardware behaviour in our own words; no xemu code is copied
into this project. Register codes and sizes are hardware facts.

## What Concrete Jungle uses (from our logs)

- CONTROL0 = 0x00110001 on almost every 3D draw: W-buffering on (bit 16),
  integer depth (bit 12 clear), Z24S8 zeta. One level run: ~2.8 M w-buffered
  draws, 0 polygon-offset draws.
- Depth tests: LEQUAL with writes (1239 draws in a sample), LEQUAL without
  writes (840: second passes, decals, lighting), ALWAYS (411, 2D/HUD).
- SET_CLIP_MAX (depth max) = 16777215.

## Depth

- W-buffering (CONTROL0 Z_PERSPECTIVE_ENABLE) stores the perspective-correct
  vertex w, in depth-buffer units, not z. xemu rebuilds it per fragment from
  the three vertex positions with exact barycentrics, marked `precise`, so
  every pass over the same triangle gets bit-identical values.
- Integer depth buffers FLOOR the value (hardware truncates). For 24-bit
  depth, d / (2^24 - 1) cannot be produced exactly by a GPU multiply; the
  exact float is bits(d / 2^24) + 1 ulp.
- Float depth (CONTROL0 bit 12) uses a custom 24-bit float (max 1e30) or
  16-bit float (max 511.9375). CJ does not use it.
- Depth clipping against ZCLIPMIN/ZCLIPMAX: discard outside when depth
  clipping is on, else clamp.
- Polygon offset: bias in depth LSBs plus factor times the depth slope, added
  before quantization. Under W-buffering the hardware appears to use a
  constant slope per polygon (taken at its first visible pixel), not a
  per-pixel one; xemu notes this as only partly emulated.
- OpenGL ignores glPolygonOffset for fragments whose shader writes
  gl_FragDepth. Any backend that writes gl_FragDepth must apply the offset
  itself.

### Our gaps (fixed 2026-10-23, all behind switches)
1. Translated vertex programs were compiled independently with no invariance,
   so two passes over one surface could differ by an ulp in position/w; with
   LEQUAL-no-write second passes that drops random pixels (shimmer).
   Fix: `invariant gl_Position` in every vertex shader.
2. Depth was 1/(fragcoord.w * zmax) rounded by the driver; hardware floors.
   Fix: floor + exact 24-bit encode (CJ_GL_DEPTHQ=0 reverts).
3. glPolygonOffset was a no-op (shader writes gl_FragDepth). Fix: offset in
   the shader (u_po). Latent: CJ issues no polygon offset in the runs so far.

## Textures

- xemu tracks guest writes per memory page (dirty bits) and, when any page
  of a bound texture (or its palette) was written, re-hashes the WHOLE
  texture data before deciding to re-upload. Changes are never missed.
- Our cache sampled 80 words over half the span and ignored palettes, so a
  partial rewrite (font page glyphs, HUD texture) could keep a stale copy.
  Fix: whole-span hash including every mip level (CJ_GL_TEXHASH=0 reverts);
  the [GL] texhash line counts stale textures the old check would have kept.
- Bytes per texel by SET_TEXTURE_FORMAT colour code: 1 byte: 0x00 0x01 0x0B
  0x13 0x19 0x1B 0x1F; 2 bytes: 0x02-0x05 0x10 0x11 0x17 0x1A 0x1C 0x1D 0x20
  0x24 0x25 0x27-0x29 0x2C 0x30 0x31 0x32 0x35; 4 bytes: 0x06 0x07 0x12 0x1E
  0x2E 0x2F 0x3A-0x3C 0x3F-0x41. DXT1 0x0C (8 bytes per 4x4), DXT3 0x0E and
  DXT5 0x0F (16). Linear ("LU_IMAGE"/"LC_IMAGE") formats use the pitch and
  have no mips. 0x0B is paletted (I8 into A8R8G8B8).
- Vertex data is also dirty-tracked (re-read only when written).

## Render targets (surfaces)

- xemu keeps a list of surfaces (colour and zeta). CPU reads of memory the
  GPU drew into first download the GPU result (the CPU waits); CPU writes
  mark the surface for re-upload before its next GPU use. A texture over a
  surface's memory is rendered from the surface, not from stale guest RAM.
- Before a semaphore release is written, xemu brings surfaces up to date, so
  a CPU that waited on the fence sees finished pixels.
- Our backend: zeta-as-texture readback and self-sampling copy exist
  (CJ_GL_ZETA_TEX, CJ_GL_SELF_COPY); there is no general CPU-read download
  or CPU-write re-upload of surfaces. Candidate if a flicker involves the CPU
  touching a render target.

## Synchronisation

- BACK_END_WRITE_SEMAPHORE_RELEASE writes the parameter to the semaphore
  context DMA + SEMAPHORE_OFFSET once everything before it has executed.
  XDK D3D reads it through device->m_pGpuTime (device+0x30) as "GPU time".
- D3D::BlockOnTime(time) waits until that time has passed (kicking off the
  push buffer first if `time` is the current, unsubmitted one). D3D calls it
  before the CPU reuses memory the GPU may still read.
- Our gaps (fixed 2026-10-23): BlockOnTime returned at once, and the port
  never told the executor where the semaphore word is, so GPU time sat at 3
  for the whole session. The CPU could overwrite vertices/textures still
  referenced by queued commands. Now: semaphore named from *(device+0x30),
  BlockOnTime waits like the original (yielding, 250 ms timeout;
  CJ_BLOCKONTIME=0 reverts, CJ_BLOCKONTIME_MS sets the timeout).
- FLIP_STALL: the GPU stops at a flip until the display has flipped.
  WAIT_FOR_IDLE / NO_OPERATION are synchronisation points.

## Vertex programs

- RCC clamps 1/x to +-[5.42101e-20, 1.884467e19]; LOG/LIT/fog modes as in our
  translator. Our translator already matches these.

## Test tools worth borrowing (as references, not code)

- xemu-project/nxdk_pgraph_tests: hardware-captured expected images per GPU
  feature (depth, w-buffer, polygon offset, texture formats, combiners).
  Their result images show what the real hardware produces for each case.
