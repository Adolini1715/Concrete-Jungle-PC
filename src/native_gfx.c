/*
 * Native graphics seam over the XDK 5849 D3D8 library (docs/native-gfx.md).
 *
 * This file is part of the recompiler's --exclude-manual input (RUN.md): each
 * `extern void sub_<VA>_gen(void)` below makes tools.recomp emit that D3D
 * function's original body as sub_<VA>_gen and route every caller to the
 * sub_<VA> defined here. With nothing switched on, every wrapper is a plain
 * pass-through, so the build behaves exactly as before.
 *
 * Modes (CJ_NULLHW, set in main.c before the title starts):
 *   0      the original path: D3D kicks the real (emulated) GPU, the NV2A poll
 *          thread executes the pushbuffer.
 *   1      no GPU at all: D3D__NullHardware retires every kickoff, nothing is
 *          drawn (headless soak runs).
 *   2      (default) no GPU, synchronous executor: D3D__NullHardware is set and, at every
 *          kickoff (and every BlockOnTime, recomp_manual.c), the pushbuffer
 *          written since the last one is executed here, on the game thread,
 *          by the same executor and back end. No poll-thread race, no fence
 *          wait: what the CPU reuses has always been drawn.
 *
 * Mode 2 is also the base for the API-level renderer: a draw call can be
 * taken natively by flushing the executor up to the call, drawing it, and
 * skipping the commands the original body pushed for it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "recomp_types.h"
#include "frame_pacer.h"
extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ---- wrapped D3D entry points (retail VAs, symbols/functions.csv) ---- */
extern void sub_002E7C70_gen(void);   /* D3D::CDevice::KickOff (thiscall) */
extern void sub_002E7BF0_gen(void);   /* D3DDevice_BeginPush */
extern void sub_002E7C10_gen(void);   /* D3DDevice_EndPush */
extern void sub_002E9A50_gen(void);   /* D3D::CommonSetViewport */
extern void sub_002EA910_gen(void);   /* D3DDevice_SelectVertexShaderDirect */
extern void sub_002EAD70_gen(void);   /* D3DDevice_SetVertexShaderConstant1 */
extern void sub_002EADD0_gen(void);   /* D3DDevice_SetVertexShaderConstant1Fast */
extern void sub_002EAE20_gen(void);   /* D3DDevice_SetVertexShaderConstant4 */
extern void sub_002EAED0_gen(void);   /* D3DDevice_SetVertexShaderConstantNotInlineFast */
extern void sub_002EAFB0_gen(void);   /* D3DDevice_SetVertexShaderConstantNotInline */
extern void sub_002EB260_gen(void);   /* D3DDevice_SetStreamSource */
extern void sub_002EB390_gen(void);   /* D3DDevice_LoadVertexShader */
extern void sub_002EB3F0_gen(void);   /* D3DDevice_LoadVertexShaderProgram */
extern void sub_002EB460_gen(void);   /* D3DDevice_SelectVertexShader */
extern void sub_002EB550_gen(void);   /* D3DDevice_SetShaderConstantMode */
extern void sub_002EB7C0_gen(void);   /* D3DDevice_SetVertexShader */
extern void sub_002EBD00_gen(void);   /* D3DDevice_SetVertexShaderInput */
extern void sub_002ECE50_gen(void);   /* D3DDevice_SetRenderTarget */
extern void sub_002ED0D0_gen(void);   /* D3DDevice_SetRenderTargetFast */
extern void sub_002ED950_gen(void);   /* D3DDevice_SetViewport */
extern void sub_002EDFD0_gen(void);   /* D3DDevice_SetTexture */
extern void sub_002EE240_gen(void);   /* D3DDevice_SetIndices */
extern void sub_002EFFB0_gen(void);   /* D3DDevice_Swap */
extern void sub_002F02E0_gen(void);   /* D3DDevice_Clear */
extern void sub_002F09C0_gen(void);   /* D3DDevice_DrawVerticesUP */
extern void sub_002F0B30_gen(void);   /* D3DDevice_DrawIndexedVerticesUP */
extern void sub_002F0C70_gen(void);   /* D3DDevice_DrawVertices */
extern void sub_002F0D10_gen(void);   /* D3DDevice_DrawIndexedVertices */
extern void sub_002F11E0_gen(void);   /* D3DDevice_Begin */
extern void sub_002F1220_gen(void);   /* D3DDevice_End */
extern void sub_002F2510_gen(void);   /* D3DDevice_SetPixelShader */

#define D3D_PDEVICE      0x00308218u   /* D3D__pDevice */
#define D3D_NULLHW       0x00307D70u   /* D3D__NullHardware */
#define DEV_PUT          0x00u         /* push pointer (write-combined VA) */
#define DEV_FLAGS        0x08u         /* 0x4 recording a push buffer, 0x2000 kickoffs never touch the GPU */
#define DEV_RING_START   0x24u         /* the ring: wraps JUMP back here */
#define DEV_RING_LIMIT   0x28u
#define DEV_REC_PUT      0x770u        /* push pointer while recording (flag 0x4) */

/* ---- call census (CJ_GFX_CENSUS=1: one line per 600 swaps) ---- */
enum { F_KICK, F_BPUSH, F_EPUSH, F_CVIEW, F_SELD, F_C1, F_C1F, F_C4, F_CNIF, F_CNI,
       F_STREAM, F_LVS, F_LVSP, F_SVS, F_CMODE, F_SETVS, F_VSIN, F_RT, F_RTF, F_VIEW,
       F_TEX, F_IDX, F_SWAP, F_CLEAR, F_DUP, F_DIUP, F_DV, F_DIV, F_BEGIN, F_END, F_PS, F_N };
static const char *const s_fname[F_N] = {
    "kick", "bpush", "epush", "cview", "selvsd", "c1", "c1f", "c4", "cnif", "cni",
    "stream", "lvs", "lvsp", "selvs", "cmode", "setvs", "vsin", "rt", "rtf", "view",
    "tex", "idx", "swap", "clear", "dup", "diup", "dv", "div", "begin", "end", "ps" };
static uint64_t s_calls[F_N];
static int s_census = -1;

static void census_report(void)
{
    char line[1024];
    int n = 0;
    for (int i = 0; i < F_N; i++)
        if (s_calls[i])
            n += snprintf(line + n, sizeof line - n, " %s=%llu", s_fname[i],
                          (unsigned long long)s_calls[i]);
    fprintf(stderr, "[GFX] census%s\n", n ? line : " (none)");
    memset(s_calls, 0, sizeof s_calls);
}

#define COUNT(f) (s_calls[f]++)

/* ---- API trace (CJ_D3D_TRACE=<swap number>[,<count>]): every wrapped D3D call of those frames,
 * arguments decoded, written to d3dtrace_<swap>.txt. The first tool of the API-level renderer
 * (docs/native-renderer-plan.md): it shows what the game asks for per frame (render targets,
 * textures, shaders, draws) instead of what the NV2A command stream implies. All the traced
 * entry points are __stdcall: at entry [esp] is the return address, arguments follow. */
static FILE *s_tf;
static uint32_t s_tr_from = ~0u, s_tr_n, s_tr_seq;
static uint32_t s_swap_n;
#define ARG(i) MEM32(g_esp + 4u + 4u * (uint32_t)(i))
/* D3D resource header: Common, Data, Lock, then (pixel containers) Format, Size. */
static void tr_res(const char *tag, uint32_t r)
{
    if (!r) { fprintf(s_tf, " %s=null", tag); return; }
    uint32_t common = MEM32(r), data = MEM32(r + 4), fmt = MEM32(r + 12), size = MEM32(r + 16);
    uint32_t type = (common >> 16) & 7, f = (fmt >> 8) & 0xff, w, h;
    if (size) { w = (size & 0xfff) + 1; h = ((size >> 12) & 0xfff) + 1; }
    else { w = 1u << ((fmt >> 20) & 15); h = 1u << ((fmt >> 24) & 15); }
    fprintf(s_tf, " %s=%08X{type %u data %08X fmt %02X %ux%u mips %u%s}", tag, r, type, data, f, w, h,
            (fmt >> 16) & 15, size ? " linear" : "");
}
static void tr_begin(const char *name)
{
    fprintf(s_tf, "%6u %s", s_tr_seq++, name);
}
#define TR(name, body) do { if (s_tf) { tr_begin(name); body; fputc('\n', s_tf); } } while (0)
/* Draw calls while tracing: run the executor up to the call, let the original body push its
 * commands, run the executor again, and log how many back-end draws that call became. That maps
 * each API draw to its NV2A draws (the take-over map); outside a trace nothing changes. */
extern uint64_t cj_gl_draw_count(void);
void cj_gfx_flush(void);
#define TRDRAW(name, args, gen) do { \
        if (s_tf) { uint64_t d0; tr_begin(name); args; cj_gfx_flush(); d0 = cj_gl_draw_count(); gen(); \
                    cj_gfx_flush(); fprintf(s_tf, " -> %llu\n", (unsigned long long)(cj_gl_draw_count() - d0)); } \
        else gen(); } while (0)
static const char *prim_name(uint32_t p)
{
    static const char *n[] = { "?", "points", "lines", "lineloop", "linestrip", "tris", "tristrip",
                               "trifan", "quads", "quadstrip", "polygon" };
    return p < 11 ? n[p] : "?";
}
static void trace_swap(void)
{
    if (s_tr_from == ~0u) {
        const char *e = getenv("CJ_D3D_TRACE");
        s_tr_from = e ? (uint32_t)strtoul(e, (char **)&e, 10) : 0u;
        s_tr_n = (e && *e == ',') ? (uint32_t)strtoul(e + 1, 0, 10) : 1u;
        if (!s_tr_from) s_tr_from = ~1u;
    }
    if (s_tf && s_swap_n >= s_tr_from + s_tr_n) {
        fprintf(s_tf, "# end\n");
        fclose(s_tf); s_tf = 0;
        fprintf(stderr, "[GFX] API trace written (%u calls)\n", s_tr_seq);
    }
    /* or on demand: the file named by CJ_D3D_TRACE_GO appearing starts a trace (checked every 15 swaps) */
    if (!s_tf && s_swap_n % 15 == 0) {
        const char *g = getenv("CJ_D3D_TRACE_GO");
        if (g && remove(g) == 0) s_tr_from = s_swap_n;
    }
    if (s_swap_n == s_tr_from && !s_tf) {
        char nm[64];
        snprintf(nm, sizeof nm, "d3dtrace_%u.txt", s_swap_n);
        s_tf = fopen(nm, "w");
        s_tr_seq = 0;
        if (s_tf) fprintf(stderr, "[GFX] API trace: swaps %u..%u -> %s\n", s_tr_from, s_tr_from + s_tr_n - 1, nm);
    }
    {   /* back-end draws over the whole frame, to set against the ones the API draws account for */
        static uint64_t d_last;
        extern uint64_t cj_gl_draw_count(void);
        uint64_t d = cj_gl_draw_count();
        if (s_tf) fprintf(s_tf, "# swap %u (back-end draws in the previous frame: %llu)\n", s_swap_n,
                          (unsigned long long)(d - d_last));
        d_last = d;
    }
}

/* ---- API state at draw time, checked against the executor (CJ_API_VERIFY=1) ----
 * The draw-takeover step needs every draw's state from the API side alone. This decodes it at
 * DrawIndexedVertices / DrawVertices (bound textures, vertex shader object, streams, pixel
 * shader definition, triangle count) and, while the executor runs that call's commands
 * synchronously, gl_backend hands each resulting batch to cj_api_check(), which compares field
 * by field. A clean frame means the API state is sufficient to draw from.
 * XDK 5849 layouts (from the disassembly): device+0x794 vertex shader object (handle - 1;
 * +0x0c program size in dwords, +0x14 16 attribute slots of 16 bytes, +0x114 the program as
 * pushbuffer words), device+0x784 pixel shader handle (+8 -> D3DPIXELSHADERDEF),
 * D3D::g_Stream 0x30A6C0 (12 bytes per stream: stride, offset?, vertex buffer). */
#define D3D_G_STREAM 0x0030A6C0u
static uint32_t s_tex[4];            /* resource per stage, from SetTexture */
static uint32_t s_vp[4] = { 0, 0, 0x7FFFFFFFu, 0x7FFFFFFFu };
static uint32_t s_view[4] = { 0, 0, 0x7FFFFFFFu, 0x7FFFFFFFu };   /* SetViewport only (Clear covers it, not the scissor) */   /* the viewport the game set: x, y, width, height */
static uint64_t s_cl_cmp, s_cl_ok, s_cl_skip, s_cl_taken, s_pp_const, s_pd_draws;   /* Clear verifier counters */
static uint32_t s_rt_c, s_rt_z;      /* the render target and depth surface the game last set (resources) */
static uint32_t s_rt_seen[64][2]; static int s_rt_nseen;   /* data addresses the game drew into: colour 0, depth 1 */
static uint64_t s_rtt_ok, s_rtt_bad;
static void rt_note_va(uint32_t va, int z);
typedef struct {
    int armed;
    uint32_t prim, n, tris, tex[4], vs, vs_dwords, ps, idx;
    uint32_t seen;                   /* batches the executor produced for this call */
    uint32_t up, up_stride;          /* DrawVerticesUP / DrawIndexedVerticesUP: the caller's vertex data and stride */
    int unfetched;                   /* the executor skipped its vertex fetch for this call (takeover must draw it) */
} ApiDraw;
static ApiDraw s_ad;
extern int (*nv2a_skip_vertex_fetch)(uint32_t n, uint32_t prim);
static int skip_fetch(uint32_t n, uint32_t prim);
static int s_verify = -1;
static uint64_t s_v_draws, s_v_batches, s_v_bad[15];
static const char *const s_v_name[15] = { "triangles", "tex0", "tex1-3", "vs", "nobatch", "extra", "combiners", "blend", "depth", "vertex", "texture", "sampler", "state", "surface", "clear" };
static FILE *s_vlog;
static uint32_t prim_tris(uint32_t prim, uint32_t n)
{
    switch (prim) {
    case 5: return n / 3;                  /* list */
    case 6: case 7: return n >= 3 ? n - 2 : 0;   /* strip, fan */
    case 8: return n / 4 * 2;              /* quads */
    case 9: return n >= 4 ? (n - 2) / 2 * 2 : 0; /* quad strip */
    default: return 0;
    }
}
static int verify_on(void)
{
    if (s_verify < 0) {
        const char *e = getenv("CJ_API_VERIFY"), *t = getenv("CJ_API_TAKEOVER");
        s_verify = (e && *e == '1') || (t && *t == '1');
    }
    return s_verify;
}
static int s_full = -1;
static int full_verify(void)   /* CJ_API_VERIFY=1: compare everything with the executor (slow); the toggle alone only takes over */
{
    if (s_full < 0) { const char *e = getenv("CJ_API_VERIFY"); s_full = e && *e == '1'; }
    return s_full;
}
static void api_arm(uint32_t prim, uint32_t n, uint32_t idx)
{
    s_ad.idx = idx;
    uint32_t dev = MEM32(D3D_PDEVICE), vs = dev ? MEM32(dev + 0x794) : 0;
    s_ad.prim = prim; s_ad.n = n; s_ad.tris = prim_tris(prim, n);
    for (int i = 0; i < 4; i++) s_ad.tex[i] = s_tex[i] ? (MEM32(s_tex[i] + 4) & 0x07FFFFFFu) : 0;
    s_ad.vs = vs; s_ad.vs_dwords = vs ? MEM32(vs + 0x0c) : 0;
    s_ad.ps = dev ? MEM32(dev + 0x784) : 0;
    s_ad.seen = 0; s_ad.unfetched = 0; s_ad.armed = 1;
    nv2a_skip_vertex_fetch = skip_fetch;
}
static void bad(int k, const char *fmt, ...)
{
    s_v_bad[k]++;
    if (s_vlog && s_v_bad[k] <= 20) {
        va_list ap; va_start(ap, fmt);
        fprintf(s_vlog, "[%s] ", s_v_name[k]); vfprintf(s_vlog, fmt, ap); fputc('\n', s_vlog);
        va_end(ap);
    }
}
#include "../xboxrecomp/src/kernel/nv2a_backend.h"
/* gl_backend.c: called with every batch while a verified API draw is running. */
static void check_ps_rs(const Nv2aBatch *b);
static float s_vc[192][4]; static uint8_t s_vc_set[192];
/* D3D's own startup values for the reserved constants nobody sets through the API (read off the
 * executor: c[0], c[56] 0 0 0 1; c[57] 0 0 1 0; c[60] 0 .5 1 2; c[61] -1 0 1 2; c[62] 0 0 -1 0).
 * The game's bone matrices later overwrite c[0..56]; the shadow follows them. */
static void vc_seed(void)
{
    static int done;
    static const struct { int r; float v[4]; } seed[] = {
        { 0, { 0, 0, 0, 1 } }, { 56, { 0, 0, 0, 1 } }, { 57, { 0, 0, 1, 0 } },
        { 60, { 0, 0.5f, 1, 2 } }, { 61, { -1, 0, 1, 2 } }, { 62, { 0, 0, -1, 0 } } };
    if (done) return;
    done = 1;
    for (unsigned i = 0; i < sizeof seed / sizeof seed[0]; i++) { memcpy(s_vc[seed[i].r], seed[i].v, 16); s_vc_set[seed[i].r] = 1; }
}
void nv2a_debug_attr(uint32_t a, uint32_t out[4]);
/* API-side vertex fetch, checked on the first vertex of each vertex-program draw.
 * Attribute a: the vertex shader object's slot a = {stream, offset, NV2A format}; stream s:
 * SetVertexShaderInput's table when it is set (device+0xB04 count, +0xB08 12-byte entries
 * {vertex buffer, stride, offset}), else D3D::g_Stream {stride, ?, vertex buffer}. Format: type in
 * bits 0-3 (2 float, 0/4 D3DCOLOR bytes, 1 normalised short, 5 short, 6 packed 11:11:10),
 * size in bits 4-7. The executor's row for that vertex is in vsh->verts in ascending attribute
 * order. */
static uint64_t s_vx_ok, s_vx_attr, s_vx_bad_fvf, s_vx_bad_other;
static uint32_t s_vx_p, s_vx_stride, s_vx_slot, s_vx_fmt;
static int api_attr(uint32_t a, uint32_t index, float out[4])
{
    /* as D3D::CDevice::LazySetStateVB: NV2A attribute a reads the vertex shader's slot
     * g_SlotMapping[(flags & 0x10) + a] (0x10: FVF order), slot = {stream, offset, format};
     * address = stream vertex buffer data + slot offset + g_Stream offset + index * stride */
    uint32_t dev = MEM32(D3D_PDEVICE), vs, slot, sl, stream, off, fmt, type, size;
    uint32_t vb, stride, soff, base, p, i;
    if (s_ad.up) {
        /* user-pointer draw: D3D programs stream 0 with the caller's data (offset 0, its stride) */
        vs = MEM32(dev + 0x794);
        slot = MEM8(0x00473DB8u + (MEM32(vs + 4) & 0x10) + a); sl = vs + 0x14 + 16 * slot;
        stream = MEM32(sl); off = MEM32(sl + 4); fmt = MEM32(sl + 8);
        if (fmt == 2 || !((fmt >> 4) & 15)) return 0;
        if (stream != 0) return 0;
        type = fmt & 15; size = (fmt >> 4) & 15;
        p = s_ad.up + off + index * s_ad.up_stride;
        s_vx_p = p - off; s_vx_stride = s_ad.up_stride; s_vx_slot = slot; s_vx_fmt = fmt;
        goto decode;
    }
    if (MEM32(0x00307D78u) & 0x40000000u) {
        /* SetVertexShaderInput(Direct) is in force: it pushed the arrays itself from the shader
         * handle it was given (device+0xB00; attribute a = slot a, no slot mapping) and its
         * stream table (device+0xB04 count, +0xB08 {vertex buffer, stride, offset}), and
         * LazySetStateVB leaves them alone. */
        vs = MEM32(dev + 0xb00) - 1;
        slot = a; sl = vs + 0x14 + 16 * slot;
        stream = MEM32(sl); off = MEM32(sl + 4); fmt = MEM32(sl + 8);
        if (fmt == 2 || !((fmt >> 4) & 15)) return 0;
        {   uint32_t e = dev + 0xb08 + 12 * stream;
            vb = MEM32(e); stride = MEM32(e + 4); soff = MEM32(e + 8); }
    } else {
        vs = MEM32(dev + 0x794);
        slot = MEM8(0x00473DB8u + (MEM32(vs + 4) & 0x10) + a); sl = vs + 0x14 + 16 * slot;
        stream = MEM32(sl); off = MEM32(sl + 4); fmt = MEM32(sl + 8);
        if (fmt == 2 || !((fmt >> 4) & 15)) return 0;
        {   uint32_t e = D3D_G_STREAM + 12 * stream;   /* g_Stream: {stride, offset, vertex buffer} */
            stride = MEM32(e); soff = MEM32(e + 4); vb = MEM32(e + 8); }
    }
    type = fmt & 15; size = (fmt >> 4) & 15;
    if (!vb) return -1;
    base = (MEM32(vb + 4) & 0x0FFFFFFFu) | 0x80000000u;
    p = base + soff + off + index * stride;
    s_vx_p = p - off; s_vx_stride = stride; s_vx_slot = slot; s_vx_fmt = fmt;
decode:
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    for (i = 0; i < size && i < 4; i++) {
        switch (type) {
        case 2: { uint32_t w = MEM32(p + 4 * i); memcpy(&out[i], &w, 4); break; }
        case 0: case 4: {   /* D3DCOLOR: B G R A in memory -> x = R */
            static const int sw[4] = { 2, 1, 0, 3 };
            out[i] = MEM8(p + (uint32_t)sw[i]) / 255.0f; break; }
        case 1: out[i] = (int16_t)MEM16(p + 2 * i) / 32767.0f; break;
        case 5: out[i] = (float)(int16_t)MEM16(p + 2 * i); break;
        case 6: {   /* packed normal: x 11, y 11, z 10 bits, signed, normalised */
            int32_t w = (int32_t)MEM32(p);
            out[0] = (float)((w << 21) >> 21) / 1023.0f; out[1] = (float)((w << 10) >> 21) / 1023.0f;
            out[2] = (float)(w >> 22) / 511.0f; i = 4; break; }
        default: return -2;
        }
    }
    return 1;
}
static void check_vertex(const Nv2aBatch *b)
{
    if (!b->vsh || !b->vsh->nverts || !b->vsh->indices || b->vsh->raw) return;   /* raw: no float rows to compare */
    uint32_t mask = b->vsh->array_mask, cols = (uint32_t)__builtin_popcount(mask), row = b->vsh->indices[0];
    /* indexed: the first index; non-indexed: idx carries 0x80000000 | the start vertex */
    uint32_t index = (s_ad.idx & 0x80000000u) && !(s_ad.idx & 0x7F000000u) ? (s_ad.idx & 0xFFFFFFu) : s_ad.idx ? MEM16(s_ad.idx) : 0, a, c = 0;
    if (row >= b->vsh->nverts) return;
    for (a = 0; a < 16; a++) if (mask & (1u << a)) {
        float api[4]; const float *ex = b->vsh->verts + (size_t)(row * cols + c) * 4;
        int r = api_attr(a, index, api);
        c++;
        if (r <= 0) { bad(9, "attr %u: no API fetch (%d)", a, r); continue; }
        s_vx_attr++;
        for (int k = 0; k < 4; k++) if (fabsf(api[k] - ex[k]) > 1e-3f * (1 + fabsf(ex[k]))) {
            bad(9, "attr %u index %u (vs flags %X slot %u fmt %X stride %u): API (%g %g %g %g) executor (%g %g %g %g) | vertex %08X %08X %08X %08X %08X %08X %08X %08X",
                a, index, MEM32(MEM32(MEM32(D3D_PDEVICE) + 0x794) + 4), s_vx_slot, s_vx_fmt, s_vx_stride,
                api[0], api[1], api[2], api[3], ex[0], ex[1], ex[2], ex[3],
                MEM32(s_vx_p), MEM32(s_vx_p + 4), MEM32(s_vx_p + 8), MEM32(s_vx_p + 12), MEM32(s_vx_p + 16),
                MEM32(s_vx_p + 20), MEM32(s_vx_p + 24), MEM32(s_vx_p + 28));
            if (MEM32(0x00307D78u) & 0x40000000u) s_vx_bad_fvf++; else s_vx_bad_other++;
            {   static int dumped;   /* once: the runtime slot mapping table and the object's 16 slots */
                if (s_vlog && !dumped++) {
                    uint32_t vs = MEM32(MEM32(D3D_PDEVICE) + 0x794), q;
                    fprintf(s_vlog, "g_SlotMapping at runtime:");
                    for (q = 0; q < 32; q++) fprintf(s_vlog, " %u", MEM8(0x00473DB8u + q));
                    fprintf(s_vlog, "\nobject %08X slots:", vs);
                    for (q = 0; q < 16; q++) fprintf(s_vlog, " [%u: s%u o%u f%X]", q, MEM32(vs + 0x14 + 16 * q),
                                                    MEM32(vs + 0x18 + 16 * q), MEM32(vs + 0x1c + 16 * q));
                    fprintf(s_vlog, "\nexecutor array_mask %X\n", b->vsh->array_mask);
                    for (q = 0; q < 16; q++) {
                        uint32_t at[4]; nv2a_debug_attr(q, at);
                        if (at[2]) fprintf(s_vlog, "  executor attr %u: offset %08X type %u size %u stride %u (API slot base %08X)\n",
                                           q, at[0], at[1], at[2], at[3], s_vx_p);
                    }
                }
            }
            goto next;
        }
        s_vx_ok++;
    next:;
    }
}
/* API-side texture: the resource bound with SetTexture (XDK 5849 D3DPixelContainer: +4 Data,
 * +0xC Format as D3D writes it to SET_TEXTURE_FORMAT, +0x10 Size for linear formats: width-1
 * bits 0-11, height-1 bits 12-23, pitch/64-1 bits 24-31). The image comes from the resource;
 * the sampler fields (address modes, filter, CONTROL0 with the stage enable, border) are still
 * the executor's (texture stage states, a later step). 0: nothing bound on that stage. */
extern uint32_t nv2a_dma_resolve(uint32_t offset);
#include "../xboxrecomp/src/d3d/d3d8_swizzle.h"
static int api_texture(int st, const Nv2aTexture *ex, Nv2aTexture *out)
{
    uint32_t r = s_tex[st], fmt, size, color;
    if (!r) return 0;
    *out = ex ? *ex : (Nv2aTexture){0};
    fmt = MEM32(r + 0x0c); size = MEM32(r + 0x10);
    color = (fmt >> 8) & 0xFF;
    out->offset = nv2a_dma_resolve(MEM32(r + 4));
    out->color = color;
    out->cube = (fmt >> 2) & 1;
    out->border_color = (fmt >> 3) & 1;
    if (d3d8_format_is_swizzled(color) || d3d8_format_dxt_block_bytes(color)) {
        out->width = 1u << ((fmt >> 20) & 0xF);
        out->height = 1u << ((fmt >> 24) & 0xF);
        out->pitch = 0;
        out->levels = (fmt >> 16) & 0xF;
    } else {
        out->width = (size & 0xFFF) + 1;
        out->height = ((size >> 12) & 0xFFF) + 1;
        out->pitch = (((size >> 24) & 0xFF) + 1) * 64;
        out->levels = 1;
    }
    return 1;
}
/* Step 3, sampler state from D3D's texture-state words (TS = 0x00307D80 + (stage*32 + i)*4;
 * layout read off the probe: 0..2 address U/V/W (1 wrap 2 mirror 3 clamp 4 border 5 clamp to
 * edge, the chip's codes), 3 mag filter, 4 min filter, 5 mip filter (D3DTEXF 0 none 1 point
 * 2 linear), 23 mip LOD bias, 24 max mip level, 25 max anisotropy, 29 border colour). The
 * chip's filter word: mag bits 24-27 (1 box, 2 tent), min bits 16-19 (1/2 level 0 only, 3/4
 * nearest level, 5/6 blended levels, box/tent), 0x2000 kernel. Returns 0 when D3D set something
 * this mapping doesn't cover (bias, anisotropy, odd modes): the executor's fields stay. */
static int api_sampler(int st, Nv2aTexture *t)
{
    uint32_t ts[32], i, mag, min, mip, fmin, fmag;
    for (i = 0; i < 32; i++) ts[i] = MEM32(0x00307D80u + (st * 32 + i) * 4);
    mag = ts[3]; min = ts[4]; mip = ts[5];
    if (ts[0] < 1 || ts[0] > 5 || ts[1] < 1 || ts[1] > 5) return 0;
    if (mag < 1 || mag > 2 || min < 1 || min > 2 || mip > 2) return 0;
    if (ts[23] || ts[24] || ts[25] > 1) return 0;
    fmag = mag; fmin = (mip == 0 ? 1 : mip == 1 ? 3 : 5) + (min == 2);
    t->addr_u = ts[0]; t->addr_v = ts[1];
    t->filter = (fmag << 24) | (fmin << 16) | 0x2000;
    t->control0 = 0x4003FFC0u;
    t->border = ts[29];
    return 1;
}
static uint64_t s_sm_ok, s_sm_cmp, s_sm_skip, s_sm_taken;
static uint64_t s_tx_ok, s_tx_cmp;
static void check_textures(const Nv2aBatch *b)
{
    for (int st = 0; st < 4; st++) {
        const Nv2aTexture *ex = b->stages[st];
        Nv2aTexture a;
        if (!ex || !api_texture(st, ex, &a)) continue;
        s_tx_cmp++;
        {   /* probe: D3D's texture-state words (0x00307D80 + (stage*32 + i)*4) next to the executor's
             * sampler fields, once per distinct executor combination */
            static uint64_t seen[64]; static int nseen;
            uint64_t sig = ((uint64_t)ex->addr_u << 60) ^ ((uint64_t)ex->addr_v << 56) ^ ((uint64_t)ex->filter << 24) ^ ex->control0 ^ ((uint64_t)st << 52) ^ ((uint64_t)ex->border << 8);
            int k, dup = 0;
            for (k = 0; k < nseen; k++) if (seen[k] == sig) { dup = 1; break; }
            if (!dup && nseen < 64 && s_vlog) {
                seen[nseen++] = sig;
                fprintf(s_vlog, "[sampler] st %d exec: u %u v %u filter %08X ctrl0 %08X border %08X bcol %08X | TS:", st, ex->addr_u, ex->addr_v, ex->filter, ex->control0, ex->border, ex->border_color);
                for (k = 0; k < 32; k++) fprintf(s_vlog, " %X", MEM32(0x00307D80u + (st * 32 + k) * 4));
                fputc('\n', s_vlog);
            }
        }
        /* the executor keeps the last linear texture's pitch for a swizzled or compressed one and
         * then draws it with one level (levels = pitch ? 1 : levels): that stale pitch is its bug,
         * the format word's level count is what the title asked for */
        int stale = !a.pitch && ex->pitch && ex->levels == 1;
        {   Nv2aTexture m = *ex;
            if (!api_sampler(st, &m)) s_sm_skip++;
            else {
                s_sm_cmp++;
                if (m.addr_u != ex->addr_u || m.addr_v != ex->addr_v || m.filter != ex->filter || m.control0 != ex->control0 || m.border != ex->border)
                    bad(11, "stage %d sampler: API u %u v %u filter %08X ctrl0 %08X border %08X | executor u %u v %u filter %08X ctrl0 %08X border %08X",
                        st, m.addr_u, m.addr_v, m.filter, m.control0, m.border, ex->addr_u, ex->addr_v, ex->filter, ex->control0, ex->border);
                else s_sm_ok++;
            }
        }
        if (a.offset != ex->offset || a.color != ex->color || a.width != ex->width || a.height != ex->height ||
            (!stale && (a.pitch != ex->pitch || a.levels != ex->levels)) || a.cube != ex->cube || a.border_color != ex->border_color)
            bad(10, "stage %d: API off %08X fmt %02X %ux%u pitch %u lv %u cube %u | executor off %08X fmt %02X %ux%u pitch %u lv %u cube %u",
                st, a.offset, a.color, a.width, a.height, a.pitch, a.levels, a.cube,
                ex->offset, ex->color, ex->width, ex->height, ex->pitch, ex->levels, ex->cube);
        else s_tx_ok++;
    }
}

/* ---------- Draw takeover, step 1 (CJ_API_TAKEOVER=1) ----------
 * For each vertex-program batch of a verified API draw, gl_backend draws a copy of the batch
 * whose vertex rows and triangle list are built here from the API side (the index data the
 * game passed to DrawIndexedVertices or the DrawVertices range, the vertex buffers and the
 * attribute layout), instead of the executor's fetch and primitive decomposition. Everything
 * else still comes from the executor batch; later steps replace it field by field. Rows are
 * one per API vertex in draw order; triangles as the executor builds them (odd strip triangles
 * swap their first two vertices). */
static int api_state(const Nv2aRenderState *ex, Nv2aRenderState *out);
/* Step 10a: the executor asks before fetching a vertex-program batch's vertices (patch 0026). With the
 * toggle on and no full verify, an armed API draw the takeover will rebuild is not fetched at all. */
static int s_to_on_early(void);
static uint64_t s_sk_skipped;
static int skip_fetch(uint32_t n, uint32_t prim)
{
    if (!s_ad.armed || s_ad.seen || full_verify() || s_to_on_early() <= 0) return 0;
    if (n != s_ad.n || prim != s_ad.prim || prim < 5 || prim > 10) return 0;
    s_ad.unfetched = 1; s_sk_skipped++;
    return 1;
}
static uint64_t s_st_taken, s_to_na_vsh, s_to_na_fixed, s_to_na_novsh;
static int s_to_on = -1;
static int s_to_on_early(void) { if (s_to_on < 0) { const char *t = getenv("CJ_API_TAKEOVER"); s_to_on = t && *t == '1'; } return s_to_on; }
static uint64_t s_to_raw;   /* taken with the executor's raw vertices */
static uint64_t s_to_taken, s_to_nofetch, s_to_prim, s_to_other, s_to_cmp, s_to_cmp_bad, s_to_tex;
static float *s_to_verts; static size_t s_to_vcap;
static uint32_t *s_to_idx; static size_t s_to_icap;
static Nv2aBatch s_to_b; static Nv2aVshBatch s_to_v;
static const Nv2aBatch *takeover_inner(const Nv2aBatch *b);
static uint64_t s_to_drop;
const Nv2aBatch *cj_api_takeover(const Nv2aBatch *b)
{
    const Nv2aBatch *r = takeover_inner(b);
    if (!r && s_ad.armed && s_ad.unfetched) {   /* not fetched and not rebuilt: drop rather than draw garbage */
        static Nv2aBatch e;
        e = *b; e.count = 0; s_to_drop++;
        return &e;
    }
    return r;
}
static const Nv2aBatch *takeover_inner(const Nv2aBatch *b)
{
    if (s_to_on < 0) { const char *t = getenv("CJ_API_TAKEOVER"); s_to_on = t && *t == '1'; }
    if (s_to_on > 0 && (!s_ad.armed || !b->vsh)) {   /* why a draw is not taken: not from DrawVertices/DrawIndexedVertices, or no vertex program */
        if (!s_ad.armed) { if (b->vsh) s_to_na_vsh++; else s_to_na_fixed++; } else s_to_na_novsh++;
    }
    if (!s_to_on || !s_ad.armed || !b->vsh || s_ad.seen != 1) return NULL;   /* one batch per call */
    uint32_t n = s_ad.n, prim = s_ad.prim, mask = b->vsh->array_mask, cols = (uint32_t)__builtin_popcount(mask);
    uint32_t i, a, c, nidx = 0;
    if (prim < 5 || prim > 10 || !n || !cols) { s_to_prim++; return NULL; }
    /* The executor's raw batch (guest vertex bytes, converted on the GPU): its vertices and triangles
     * are the ones this would rebuild (verified identical, takeover step 1), so draw them and take only
     * the rest from the API. Rebuilding them here converted every vertex again, element by element.
     * CJ_API_VERIFY=1 still rebuilds and compares. */
    if (b->vsh->raw && !full_verify()) {
        s_to_v = *b->vsh; s_to_b = *b; s_to_b.vsh = &s_to_v;
        s_to_raw++;
        goto have_verts;
    }
    if ((size_t)n * cols * 4 > s_to_vcap) {
        s_to_vcap = (size_t)n * cols * 4 + 1024; s_to_verts = (float *)realloc(s_to_verts, s_to_vcap * sizeof(float));
    }
    if ((size_t)n * 3 > s_to_icap) { s_to_icap = (size_t)n * 3 + 256; s_to_idx = (uint32_t *)realloc(s_to_idx, s_to_icap * sizeof(uint32_t)); }
    if (!s_to_verts || !s_to_idx) { s_to_other++; return NULL; }
    int direct = (s_ad.idx & 0x80000000u) && !(s_ad.idx & 0x7F000000u);   /* DrawVertices: start vertex */
    /* Indexed draws: one row per vertex in the index range when it is dense (meshes share vertices),
     * not one per index: less to fetch and less to upload. */
    uint32_t lo = 0, hi = 0, rows = n, ranged = 0;
    if (!direct) {
        lo = hi = MEM16(s_ad.idx);
        for (i = 1; i < n; i++) { uint32_t x = MEM16(s_ad.idx + 2 * i); if (x < lo) lo = x; if (x > hi) hi = x; }
        if (hi - lo + 1 <= n) { ranged = 1; rows = hi - lo + 1; }
    }
    for (i = 0; i < rows; i++) {
        uint32_t index = direct ? (s_ad.idx & 0xFFFFFFu) + i : ranged ? lo + i : MEM16(s_ad.idx + 2 * i);
        float *row = s_to_verts + (size_t)i * cols * 4;
        for (a = 0, c = 0; a < 16; a++) if (mask & (1u << a)) {
            if (api_attr(a, index, row + c * 4) <= 0) { s_to_nofetch++; return NULL; }
            c++;
        }
    }
#define VI(k) (ranged ? (uint32_t)MEM16(s_ad.idx + 2 * (k)) - lo : (k))
#define TRI(x, y, z) do { s_to_idx[nidx++] = VI(x); s_to_idx[nidx++] = VI(y); s_to_idx[nidx++] = VI(z); } while (0)
    switch (prim) {
    case 5: for (i = 0; i + 2 < n; i += 3) TRI(i, i + 1, i + 2); break;
    case 6: for (i = 0; i + 2 < n; i++) { if (i & 1) TRI(i + 1, i, i + 2); else TRI(i, i + 1, i + 2); } break;
    case 7: case 10: for (i = 1; i + 1 < n; i++) TRI(0, i, i + 1); break;
    case 8: for (i = 0; i + 3 < n; i += 4) { TRI(i, i + 1, i + 2); TRI(i, i + 2, i + 3); } break;
    case 9: for (i = 0; i + 3 < n; i += 2) { TRI(i, i + 1, i + 3); TRI(i, i + 3, i + 2); } break;
    }
#undef TRI
#undef VI
    if (nidx != b->count) { s_to_other++; return NULL; }   /* the executor saw a different draw */
    {   /* with CJ_API_VERIFY=1 too: every corner of every triangle against the executor's */
        if (full_verify() && !s_ad.unfetched && !b->vsh->raw) {
            uint32_t k, q, bad_ = 0;
            for (k = 0; k < nidx && !bad_; k++) {
                const float *x = s_to_verts + (size_t)s_to_idx[k] * cols * 4, *y = b->vsh->verts + (size_t)b->vsh->indices[k] * cols * 4;
                for (q = 0; q < cols * 4; q++) if (fabsf(x[q] - y[q]) > 1e-4f * (1 + fabsf(y[q]))) { bad_ = k + 1; break; }
            }
            s_to_cmp++;
            if (bad_) { s_to_cmp_bad++; if (s_vlog && s_to_cmp_bad <= 10) fprintf(s_vlog, "[takeover] prim %u n %u: corner %u differs from the executor\n", prim, n, bad_ - 1); }
        }
    }
    s_to_v = *b->vsh; s_to_v.verts = s_to_verts; s_to_v.nverts = rows; s_to_v.indices = s_to_idx; s_to_v.raw = NULL;
    s_to_b = *b; s_to_b.vsh = &s_to_v; s_to_b.count = nidx;
have_verts:
    {   /* step 2: each bound stage's image from SetTexture's resource */
        static Nv2aTexture tx[4];
        for (int st = 0; st < 4; st++)
            if (b->stages[st] && api_texture(st, b->stages[st], &tx[st])) {
                if (api_sampler(st, &tx[st])) s_sm_taken++;
                s_to_b.stages[st] = &tx[st];
                if (st == 0 && b->texture) s_to_b.texture = &tx[0];
                s_to_tex++;
            }
    }
    {   /* step 6: the vertex constants from the shadow of the game's own writes (verified identical),
         * the executor's only for registers nobody set through the API (the particle block) */
        static float merged[192][4];
        if (b->vsh->constants) {
            vc_seed();
            for (int i = 0; i < 192; i++) memcpy(merged[i], s_vc_set[i] ? s_vc[i] : b->vsh->constants[i], 16);
            s_to_v.constants = merged;
        }
    }
    {   /* step 4: blend, alpha test, depth, colour mask and culling from D3D's render-state table */
        static Nv2aRenderState st;
        if (b->state && api_state(b->state, &st)) { s_to_b.state = &st; s_st_taken++; }
    }
    s_to_taken++;
    return &s_to_b;
}


/* Step 9: the surface a draw goes to, from the render target the game set (SetRenderTarget
 * resource: +4 data, +0xC format word with the colour format in bits 8-15 and, for swizzled
 * targets, log2 width/height in bits 20-27; +0x10 size word for linear ones: width-1 bits 0-11,
 * height-1 bits 12-23, pitch/64-1 bits 24-31) and the viewport (which is the NV2A's surface clip).
 * The executor's format word stays (it carries depth and swizzle bits). 0: no target known. */
static int api_surface(const Nv2aSurface *ex, Nv2aSurface *o)
{
    uint32_t rt = s_rt_c, fmt, size, color, w, h, pitch, bpp;
    if (!rt) return 0;
    fmt = MEM32(rt + 12); size = MEM32(rt + 16); color = (fmt >> 8) & 0xFF;
    if (d3d8_format_is_swizzled(color)) { w = 1u << ((fmt >> 20) & 0xF); h = 1u << ((fmt >> 24) & 0xF); pitch = 0; }
    else { w = (size & 0xFFF) + 1; h = ((size >> 12) & 0xFFF) + 1; pitch = (((size >> 24) & 0xFF) + 1) * 64; }
    bpp = (color == 0x05 || color == 0x03 || color == 0x02 || color == 0x01 || color == 0x11) ? 2 : 4;
    if (!pitch) pitch = w * bpp;
    *o = *ex;
    o->color_va = nv2a_dma_resolve(MEM32(rt + 4));
    rt_note_va(o->color_va, 0);   /* the address as drawn (Swap repoints the back buffer object at the other buffer) */
    o->pitch = pitch; o->bytes_per_pixel = bpp;
    {   uint32_t x = s_vp[0] < w ? s_vp[0] : w, y = s_vp[1] < h ? s_vp[1] : h, vw = s_vp[2], vh = s_vp[3];
        if (vw > w - x) vw = w - x;
        if (vh > h - y) vh = h - y;
        o->clip_x = x; o->clip_y = y; o->width = vw; o->height = vh;
    }
    return 1;
}
static uint64_t s_sf_cmp, s_sf_ok, s_sf_skip, s_sf_taken;
const Nv2aSurface *cj_api_takeover_surface(const Nv2aSurface *sf)
{
    static Nv2aSurface a;
    if (s_to_on <= 0 || !s_ad.armed || !api_surface(sf, &a)) return NULL;
    s_sf_taken++;
    return &a;
}
void cj_api_check_surface(const Nv2aSurface *sf)
{
    Nv2aSurface a;
    if (!s_ad.armed || !full_verify()) return;
    if (!api_surface(sf, &a)) { s_sf_skip++; return; }
    s_sf_cmp++;
    if (a.color_va != sf->color_va || a.width != sf->width || a.height != sf->height || a.pitch != sf->pitch ||
        a.bytes_per_pixel != sf->bytes_per_pixel || a.clip_x != sf->clip_x || a.clip_y != sf->clip_y)
        bad(13, "API va %08X %ux%u pitch %u bpp %u clip %u,%u | executor va %08X %ux%u pitch %u bpp %u clip %u,%u | game rt %08X fmt %08X size %08X viewport %u,%u %ux%u",
            a.color_va, a.width, a.height, a.pitch, a.bytes_per_pixel, a.clip_x, a.clip_y,
            sf->color_va, sf->width, sf->height, sf->pitch, sf->bytes_per_pixel, sf->clip_x, sf->clip_y,
            s_rt_c, MEM32(s_rt_c + 12), MEM32(s_rt_c + 16), s_vp[0], s_vp[1], s_vp[2], s_vp[3]);
    else s_sf_ok++;
    if (s_vlog && (a.clip_x != sf->clip_x || a.width != sf->width || a.clip_y != sf->clip_y || a.height != sf->height)) {   /* probe: where does D3D keep the scissor rectangle? */
        static int shown; static uint32_t seen_[8]; static int ns;
        uint32_t key = sf->clip_x | (sf->clip_y << 10) | (sf->width << 20), dev = MEM32(D3D_PDEVICE), i, j, v[4] = { sf->clip_x, sf->clip_y, sf->clip_x + sf->width, sf->clip_y + sf->height };
        for (j = 0; j < (uint32_t)ns && seen_[j] != key; j++) ;
        if (j == (uint32_t)ns && ns < 8 && shown < 8) {
            seen_[ns++] = key; shown++;
            fprintf(s_vlog, "[scissor] exec clip %u,%u %ux%u: device words equal to x,y,x2,y2 in a row at:", sf->clip_x, sf->clip_y, sf->width, sf->height);
            for (i = 0; i < 0x1400; i += 4) {
                if (MEM32(dev + i) == v[0] && MEM32(dev + i + 4) == v[1] && MEM32(dev + i + 8) == v[2] && MEM32(dev + i + 12) == v[3]) fprintf(s_vlog, " +%X(xyXY)", i);
                if (MEM32(dev + i) == v[0] && MEM32(dev + i + 4) == v[2] && MEM32(dev + i + 8) == v[1] && MEM32(dev + i + 12) == v[3]) fprintf(s_vlog, " +%X(xXyY)", i);
                if (MEM32(dev + i) == (v[0] | (v[2] - 1) << 16) && MEM32(dev + i + 4) == (v[1] | (v[3] - 1) << 16)) fprintf(s_vlog, " +%X(packed)", i);
            }
            fputc('\n', s_vlog);
        }
    }
}

void cj_api_check(const Nv2aBatch *b)
{
    if (!s_ad.armed) return;
    if (!full_verify()) { s_v_batches++; s_ad.seen++; return; }
    s_v_batches++;
    check_ps_rs(b);
    check_vertex(b);
    check_textures(b);
    {   /* probe (first 6 vertex-program draws): the vertex shader object's attribute slots, the
         * streams, and the executor's first fetched vertex, to work out API-side vertex fetch */
        static int probes;
        if (s_vlog && b->vsh && probes < 6) {
            uint32_t dev = MEM32(D3D_PDEVICE), vs = MEM32(dev + 0x794), i;
            probes++;
            fprintf(s_vlog, "PROBE draw prim %u n %u vs %08X flags %08X | array_mask %X nverts %u\n", s_ad.prim, s_ad.n,
                    vs, MEM32(vs + 4), b->vsh->array_mask, b->vsh->nverts);
            for (i = 0; i < 16; i++) {
                uint32_t a = vs + 0x14 + 16 * i;
                if (MEM32(a) | MEM32(a + 4) | MEM32(a + 8) | MEM32(a + 12))
                    fprintf(s_vlog, "  slot %2u: %08X %08X %08X %08X\n", i, MEM32(a), MEM32(a + 4), MEM32(a + 8), MEM32(a + 12));
            }
            for (i = 0; i < 4; i++) {
                uint32_t st = D3D_G_STREAM + 12 * i, vb = MEM32(st + 8);
                if (vb) fprintf(s_vlog, "  stream %u: %08X %08X vb %08X data %08X\n", i, MEM32(st), MEM32(st + 4), vb, MEM32(vb + 4));
            }
            if (b->vsh->nverts && !b->vsh->raw) {
                uint32_t cols = (uint32_t)__builtin_popcount(b->vsh->array_mask), c;
                fprintf(s_vlog, "  executor vertex 0:");
                for (c = 0; c < cols && c < 6; c++)
                    fprintf(s_vlog, " (%g %g %g %g)", b->vsh->verts[c * 4], b->vsh->verts[c * 4 + 1], b->vsh->verts[c * 4 + 2], b->vsh->verts[c * 4 + 3]);
                fprintf(s_vlog, "\n  first index %u\n", b->vsh->indices ? b->vsh->indices[0] : 0);
            }
        }
    }
    if (s_ad.seen++) { bad(5, "draw prim %u n %u produced more than one batch", s_ad.prim, s_ad.n); return; }
    uint32_t tris = b->count / 3;
    if (b->vsh) tris = b->count / 3;
    if (tris > s_ad.tris || tris + 2 < s_ad.tris / 2)
        bad(0, "prim %u n %u: API %u triangles, executor %u", s_ad.prim, s_ad.n, s_ad.tris, tris);
    {
        const Nv2aTexture *t0 = b->stages[0] ? b->stages[0] : b->texture;
        uint32_t ex0 = t0 ? (t0->offset & 0x07FFFFFFu) : 0;
        if (s_ad.tex[0] && ex0 && ex0 != s_ad.tex[0])
            bad(1, "stage 0: API data %08X, executor %08X", s_ad.tex[0], ex0);
        for (int i = 1; i < 4; i++) {
            uint32_t ex = b->stages[i] ? (b->stages[i]->offset & 0x07FFFFFFu) : 0;
            if (s_ad.tex[i] && ex && ex != s_ad.tex[i])
                bad(2, "stage %d: API data %08X, executor %08X", i, s_ad.tex[i], ex);
        }
    }
    if (b->vsh && s_ad.vs) {
        /* the object's program is pushbuffer words: a load header, then 4 dwords per instruction */
        /* D3D shadows the vertex program memory in the device (LoadVertexShaderProgram copies each
         * program to device + 0x10a8 + slot * 16 unless device flag 0x10 is set; SelectVertexShader
         * keeps the start slot at device + 0x79c), so the API side reads the running program there. */
        uint32_t dev = MEM32(D3D_PDEVICE), slot = MEM32(dev + 0x79c), k, n = b->vsh->ninstr;
        if (MEM8(dev + 8) & 0x10) bad(3, "device does not shadow vertex programs (flag 0x10)");
        else if (slot + n > 136) bad(3, "start slot %u + %u instructions past program memory", slot, n);
        else {
            const uint32_t *api = (const uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + dev + 0x10a8 + slot * 16);
            for (k = 0; k < n; k++) if (memcmp(api + 4 * k, b->vsh->program[k], 16)) {
                bad(3, "vertex program differs at instruction %u of %u (start slot %u)", k, n, slot); break; }
        }
    }
}
/* Pixel shader and render state against the executor's view of the same draw. */
#define D3D_RS 0x00307F80u
#define RS(i) MEM32(D3D_RS + 4u * (i))
/* Step 4. D3D's render-state table (D3D__RenderState 0x00307F80, index * 4; indices read off the
 * probe and the blend/depth verifier): 0x39 depth func, 0x3A alpha func, 0x3B blend enable,
 * 0x3C alpha test enable, 0x3D alpha ref, 0x3E/0x3F blend src/dst, 0x40 depth write, 0x43 colour
 * mask (the chip's byte layout), 0x4A blend equation, 0x4B blend colour, 0x8F depth test enable,
 * 0x92 front face, 0x93 cull (0: off, 0x901: on, the back face). Stencil, polygon offset, the
 * window clip, control0 and the depth surface are not mapped yet and stay the executor's. */
static int api_state(const Nv2aRenderState *ex, Nv2aRenderState *o)
{
    uint32_t cull = RS(0x93);
    if (cull != 0 && cull != 0x901) return 0;
    if (ex->cull_face != 0x405) return 0;
    *o = *ex;
    o->blend_enable = RS(0x3b) != 0; o->blend_src = RS(0x3e); o->blend_dst = RS(0x3f);
    o->blend_eq = RS(0x4a); o->blend_color = RS(0x4b);
    o->alpha_test_enable = RS(0x3c) != 0; o->alpha_func = RS(0x3a); o->alpha_ref = RS(0x3d);
    o->depth_test_enable = RS(0x8f) != 0 && !(RS(0x39) == 0x207 && !RS(0x40));
    o->depth_func = RS(0x39); o->depth_write = RS(0x40) != 0;
    o->color_mask = RS(0x43);
    o->cull_enable = cull != 0; o->front_face = RS(0x92);
    return 1;
}
static uint64_t s_st_ok, s_st_cmp, s_st_skip;
static void check_state(const Nv2aRenderState *r)
{
    Nv2aRenderState a;
    if (!api_state(r, &a)) { s_st_skip++; return; }
    s_st_cmp++;
    /* depth func and blend factors only count while their test or blend is on */
    int bad_ = a.blend_enable != r->blend_enable || (a.blend_enable && (a.blend_src != r->blend_src || a.blend_dst != r->blend_dst || a.blend_eq != r->blend_eq || a.blend_color != r->blend_color)) ||
        a.alpha_test_enable != r->alpha_test_enable || (a.alpha_test_enable && (a.alpha_func != r->alpha_func || a.alpha_ref != r->alpha_ref)) ||
        a.depth_test_enable != r->depth_test_enable || (a.depth_test_enable && (a.depth_func != r->depth_func || a.depth_write != r->depth_write)) ||
        (!a.depth_test_enable && a.depth_write != r->depth_write) ||
        a.color_mask != r->color_mask || a.cull_enable != r->cull_enable || (a.cull_enable && a.front_face != r->front_face);
    if (bad_)
        bad(12, "state: API blend %u %X %X %X %X atest %u %X %u z %u %X %u cmask %08X cull %u front %X | executor blend %u %X %X %X %X atest %u %X %u z %u %X %u cmask %08X cull %u front %X",
            a.blend_enable, a.blend_src, a.blend_dst, a.blend_eq, a.blend_color, a.alpha_test_enable, a.alpha_func, a.alpha_ref, a.depth_test_enable, a.depth_func, a.depth_write, a.color_mask, a.cull_enable, a.front_face,
            r->blend_enable, r->blend_src, r->blend_dst, r->blend_eq, r->blend_color, r->alpha_test_enable, r->alpha_func, r->alpha_ref, r->depth_test_enable, r->depth_func, r->depth_write, r->color_mask, r->cull_enable, r->front_face);
    else s_st_ok++;
}
static uint64_t s_vc_cmp, s_vc_ok, s_vc_bad_set[192], s_vc_bad_unset[192];
static void check_constants(const Nv2aBatch *b)
{
    vc_seed();
    if (!b->vsh || !b->vsh->constants) return;
    s_vc_cmp++;
    int bad_ = 0;
    for (int i = 0; i < 192; i++) {
        if (memcmp(b->vsh->constants[i], s_vc[i], 16) == 0) continue;
        if (s_vc_set[i]) s_vc_bad_set[i]++; else s_vc_bad_unset[i]++;
        {   static int shown[192];
            if (s_vlog && shown[i] < 3) {
                shown[i]++;
                fprintf(s_vlog, "[const] c[%d] (reg %d) %s: executor %g %g %g %g | shadow %g %g %g %g\n", i, i - 96, s_vc_set[i] ? "SET" : "unset", b->vsh->constants[i][0], b->vsh->constants[i][1], b->vsh->constants[i][2], b->vsh->constants[i][3], s_vc[i][0], s_vc[i][1], s_vc[i][2], s_vc[i][3]);
            }
        }
        bad_ = 1;
    }
    if (!bad_) s_vc_ok++;
}
static void check_ps_rs(const Nv2aBatch *b)
{
    uint32_t h = s_ad.ps;
    if (b->rc) {   /* how many distinct combiner programs, and how many draws carry a pixel shader handle */
        static uint64_t tab[8192]; static uint64_t ndist, ndraw, nps; static int done_;
        const Nv2aCombiner *c = b->rc; uint64_t hh = 1469598103934665603ull; uint32_t i, *w;
        const uint32_t *ws[] = { c->color_icw, c->alpha_icw, c->color_ocw, c->alpha_ocw };
        for (int g = 0; g < 4; g++) for (i = 0; i < 8; i++) { hh ^= ws[g][i]; hh *= 1099511628211ull; }
        hh ^= c->final0; hh *= 1099511628211ull; hh ^= c->final1; hh *= 1099511628211ull; hh ^= c->control; hh *= 1099511628211ull; hh ^= c->stage_program; hh *= 1099511628211ull;
        hh |= 1; ndraw++; if (h) nps++;
        for (i = (uint32_t)(hh & 8191); tab[i] && tab[i] != hh; i = (i + 1) & 8191) ;
        if (!tab[i] && ndist < 4000) {
            tab[i] = hh; ndist++; (void)w;
            if (s_vlog && ndist <= 60) {
                fprintf(s_vlog, "[rc] #%llu control %08X stage_prog %08X final %08X %08X n %u | rgb icw/ocw:", (unsigned long long)ndist, c->control, c->stage_program, c->final0, c->final1, c->control & 0xF);
                for (i = 0; i < (c->control & 0xF) && i < 8; i++) fprintf(s_vlog, " %08X/%08X", c->color_icw[i], c->color_ocw[i]);
                fprintf(s_vlog, " | TS st0:");
                for (i = 0; i < 32; i++) fprintf(s_vlog, " %X", MEM32(0x00307D80u + i * 4));
                fprintf(s_vlog, " st1:");
                for (i = 0; i < 32; i++) fprintf(s_vlog, " %X", MEM32(0x00307D80u + (32 + i) * 4));
                fputc('\n', s_vlog);
            }
        }
        if (!(ndraw & 0xFFFFF) && s_vlog) fprintf(s_vlog, "[rc] after %llu draws: %llu distinct combiner programs, %llu with a pixel shader handle\n", (unsigned long long)ndraw, (unsigned long long)ndist, (unsigned long long)nps);
        (void)done_;
    }
    if (h && b->rc && s_vlog) {   /* probe: pixel shader definition words next to the executor's combiner state, once per shader+constants */
        static uint64_t seen[96]; static int nseen;
        const Nv2aCombiner *c = b->rc;
        uint32_t d = MEM32(h + 8), i, k;
        uint64_t sig = ((uint64_t)h << 20) ^ ((uint64_t)c->control << 40) ^ c->factor0[0] ^ ((uint64_t)c->factor1[0] << 17) ^ ((uint64_t)c->final_c0 << 9) ^ ((uint64_t)c->final_c1 << 33) ^ ((uint64_t)c->stage_program << 5);
        int dup = 0;
        for (k = 0; k < (uint32_t)nseen; k++) if (seen[k] == sig) { dup = 1; break; }
        if (!dup && nseen < 96) {
            seen[nseen++] = sig;
            fprintf(s_vlog, "[ps] exec: control %08X stage_prog %08X final_c %08X %08X f0:", c->control, c->stage_program, c->final_c0, c->final_c1);
            for (i = 0; i < 8; i++) fprintf(s_vlog, " %08X", c->factor0[i]);
            fprintf(s_vlog, " f1:");
            for (i = 0; i < 8; i++) fprintf(s_vlog, " %08X", c->factor1[i]);
            fprintf(s_vlog, " | def +28:");
            for (i = 0; i < 8; i++) fprintf(s_vlog, " %08X", MEM32(d + 0x28 + 4 * i));
            fprintf(s_vlog, " +48:");
            for (i = 0; i < 8; i++) fprintf(s_vlog, " %08X", MEM32(d + 0x48 + 4 * i));
            fprintf(s_vlog, " A8 %08X AC %08X B0 %08X D4 %08X D8 %08X DC %08X E0 %08X | RS 0..0x28:", MEM32(d + 0xa8), MEM32(d + 0xac), MEM32(d + 0xb0), MEM32(d + 0xd4), MEM32(d + 0xd8), MEM32(d + 0xdc), MEM32(d + 0xe0));
            for (i = 0; i < 0x29; i++) fprintf(s_vlog, " %X", RS(i));
            fputc('\n', s_vlog);
        }
    }
    if (h && b->rc) {
        /* D3DPIXELSHADERDEF: AlphaInputs[8] +0, FinalABCD +0x20, FinalEFG +0x24, AlphaOutputs[8] +0x68,
         * RGBInputs[8] +0x88, RGBOutputs[8] +0xb4, CombinerCount +0xd4 (stages in the low byte) */
        uint32_t d = MEM32(h + 8), n = MEM32(d + 0xd4) & 0xff, i;
        if (n > 8) n = 8;
        for (i = 0; i < n; i++)
            if (MEM32(d + 0x88 + 4 * i) != b->rc->color_icw[i] || MEM32(d + 0xb4 + 4 * i) != b->rc->color_ocw[i] ||
                MEM32(d + 0x00 + 4 * i) != b->rc->alpha_icw[i] || MEM32(d + 0x68 + 4 * i) != b->rc->alpha_ocw[i]) {
                bad(6, "stage %u: API rgb %08X/%08X alpha %08X/%08X, executor %08X/%08X %08X/%08X", i,
                    MEM32(d + 0x88 + 4 * i), MEM32(d + 0xb4 + 4 * i), MEM32(d + 4 * i), MEM32(d + 0x68 + 4 * i),
                    b->rc->color_icw[i], b->rc->color_ocw[i], b->rc->alpha_icw[i], b->rc->alpha_ocw[i]);
                break;
            }
        if (i == n && (MEM32(d + 0x20) != b->rc->final0 || MEM32(d + 0x24) != b->rc->final1))
            bad(6, "final combiner: API %08X %08X, executor %08X %08X", MEM32(d + 0x20), MEM32(d + 0x24),
                b->rc->final0, b->rc->final1);
    }
    if (b->state && s_vlog) {   /* probe: D3D's whole render-state table next to the executor's fields, once per distinct combination */
        static uint64_t seen[96]; static int nseen;
        const Nv2aRenderState *r = b->state;
        uint64_t sig = r->alpha_test_enable * 0x9E3779B97F4A7C15ull ^ ((uint64_t)r->alpha_func << 8) ^ ((uint64_t)r->alpha_ref << 20) ^
            ((uint64_t)r->cull_enable << 30) ^ ((uint64_t)r->cull_face << 32) ^ ((uint64_t)r->front_face << 40) ^ ((uint64_t)r->color_mask * 0x2545F4914F6CDD1Dull) ^
            ((uint64_t)r->stencil_enable << 50) ^ ((uint64_t)r->stencil_func << 3) ^ ((uint64_t)r->stencil_ref << 12) ^ ((uint64_t)r->stencil_write_mask << 24) ^
            ((uint64_t)r->stencil_fail << 5) ^ ((uint64_t)r->stencil_zpass << 17) ^ ((uint64_t)r->stencil_zfail << 29) ^ ((uint64_t)r->poly_offset_fill << 55) ^
            ((uint64_t)(r->poly_offset_factor != 0) << 56) ^ ((uint64_t)r->control0 << 38);
        int k, dup = 0;
        for (k = 0; k < nseen; k++) if (seen[k] == sig) { dup = 1; break; }
        if (!dup && nseen < 96) {
            seen[nseen++] = sig;
            fprintf(s_vlog, "[rs] exec: atest %u func %X ref %u cull %u face %X front %X cmask %08X sten %u f %X ref %X fm %X wm %X sf %X szf %X szp %X poly %u %g %g ctl0 %08X | RS:",
                    r->alpha_test_enable, r->alpha_func, r->alpha_ref, r->cull_enable, r->cull_face, r->front_face, r->color_mask, r->stencil_enable,
                    r->stencil_func, r->stencil_ref, r->stencil_func_mask, r->stencil_write_mask, r->stencil_fail, r->stencil_zfail, r->stencil_zpass,
                    r->poly_offset_fill, r->poly_offset_factor, r->poly_offset_units, r->control0);
            for (k = 0; k < 0xA0; k++) fprintf(s_vlog, " %X", RS(k));
            fputc('\n', s_vlog);
        }
    }
    if (b->state) check_state(b->state);
    check_constants(b);
    if (b->state) {
        const Nv2aRenderState *r = b->state;
        uint32_t be = RS(0x3b) != 0;
        if (be != (r->blend_enable != 0) || (be && (RS(0x3e) != r->blend_src || RS(0x3f) != r->blend_dst)))
            bad(7, "API enable %u src %X dst %X, executor %u %X %X", be, RS(0x3e), RS(0x3f),
                r->blend_enable, r->blend_src, r->blend_dst);
        /* D3D sends "test on, compare ALWAYS, no writes" to the chip as depth off: the same thing */
        uint32_t ze = RS(0x8f) != 0 && !(RS(0x39) == 0x207 && !RS(0x40));
        if (ze != (r->depth_test_enable != 0) || (ze && (RS(0x39) != r->depth_func || (RS(0x40) != 0) != (r->depth_write != 0))))
            bad(8, "API z %u func %X write %u, executor %u %X %u", ze, RS(0x39), RS(0x40) != 0,
                r->depth_test_enable, r->depth_func, r->depth_write);
    }
}
static void api_done(void)
{
    if (!s_ad.armed) return;
    s_v_draws++;
    if (!s_ad.seen && s_ad.tris) bad(4, "prim %u n %u produced no batch", s_ad.prim, s_ad.n);
    s_ad.armed = 0;
}
static void verify_report(void)
{
    fprintf(stderr, "[APIV] draws %llu batches %llu vertex attrs ok %llu/%llu | mismatches:", (unsigned long long)s_v_draws,
            (unsigned long long)s_v_batches, (unsigned long long)s_vx_ok, (unsigned long long)s_vx_attr);
    for (int k = 0; k < 15; k++) fprintf(stderr, " %s=%llu", s_v_name[k], (unsigned long long)s_v_bad[k]);
    fputc('\n', stderr);
    fprintf(stderr, "[APIV] textures from SetTexture matching the executor: %llu/%llu\n", (unsigned long long)s_tx_ok, (unsigned long long)s_tx_cmp);
    fprintf(stderr, "[APIV] samplers from D3D's texture states matching the executor: %llu/%llu (not mapped, executor's kept: %llu; taken over %llu)\n",
            (unsigned long long)s_sm_ok, (unsigned long long)s_sm_cmp, (unsigned long long)s_sm_skip, (unsigned long long)s_sm_taken);
    fprintf(stderr, "[APIV] render states from D3D's table matching the executor: %llu/%llu (not mapped %llu; taken over %llu)\n",
            (unsigned long long)s_st_ok, (unsigned long long)s_st_cmp, (unsigned long long)s_st_skip, (unsigned long long)s_st_taken);
    fprintf(stderr, "[APIV] vertex constants (all 192) matching the shadow of the game's SetVertexShaderConstant calls: %llu/%llu batches\n", (unsigned long long)s_vc_ok, (unsigned long long)s_vc_cmp);
    for (int i = 0; i < 192; i++) if (s_vc_bad_set[i] || s_vc_bad_unset[i])
        fprintf(stderr, "[APIV]   c[%d] (reg %d): differs in %llu batches where the game set it, %llu where it did not\n", i, i - 96, (unsigned long long)s_vc_bad_set[i], (unsigned long long)s_vc_bad_unset[i]);
    fprintf(stderr, "[APIV] draw surfaces from SetRenderTarget and SetViewport matching the executor: %llu/%llu (no target known %llu; taken over %llu)\n",
            (unsigned long long)s_sf_ok, (unsigned long long)s_sf_cmp, (unsigned long long)s_sf_skip, (unsigned long long)s_sf_taken);
    fprintf(stderr, "[APIV] render-to-texture: back-end surfaces sampled that the game set as render targets: %llu/%llu\n", (unsigned long long)s_rtt_ok, (unsigned long long)(s_rtt_ok + s_rtt_bad));
    fprintf(stderr, "[APIV] particle draws from the game's own push-buffer writes (INLINE_ARRAY): %llu\n", (unsigned long long)s_pd_draws);
    fprintf(stderr, "[APIV] particle constants written straight into the push buffer (c36-c40 blocks): %llu\n", (unsigned long long)s_pp_const);
    fprintf(stderr, "[APIV] clears from D3D Clear matching the executor: %llu/%llu (with rectangles, executor's kept: %llu; taken over %llu)\n",
            (unsigned long long)s_cl_ok, (unsigned long long)s_cl_cmp, (unsigned long long)s_cl_skip, (unsigned long long)s_cl_taken);
    fprintf(stderr, "[APIV] vertex value mismatches: SetVertexShaderInput in force %llu, other %llu\n",
            (unsigned long long)s_vx_bad_fvf, (unsigned long long)s_vx_bad_other);
    if (s_to_on > 0)
        fprintf(stderr, "[TAKEOVER] vertex data and triangles from the API: %llu batches, left to the executor %llu (no API fetch %llu, primitive %llu, other %llu)\n",
                (unsigned long long)s_to_taken, (unsigned long long)(s_to_nofetch + s_to_prim + s_to_other),
                (unsigned long long)s_to_nofetch, (unsigned long long)s_to_prim, (unsigned long long)s_to_other);
    if (s_to_on > 0)
        fprintf(stderr, "[TAKEOVER] all back-end draws this run: %llu, drawn from the API: %llu (%llu not from an API draw call: particles, direct pushbuffer writers)\n",
                (unsigned long long)cj_gl_draw_count(), (unsigned long long)s_to_taken,
                (unsigned long long)(cj_gl_draw_count() > s_to_taken ? cj_gl_draw_count() - s_to_taken : 0));
    if (s_to_on > 0)
        fprintf(stderr, "[TAKEOVER] not taken: vertex-program draws outside DrawVertices/DrawIndexedVertices %llu, fixed-function draws outside them %llu, API draws without a vertex program %llu\n",
                (unsigned long long)s_to_na_vsh, (unsigned long long)s_to_na_fixed, (unsigned long long)s_to_na_novsh);
    if (s_to_on > 0)
        fprintf(stderr, "[TAKEOVER] drawn with the executor's raw vertices (GPU-converted): %llu\n", (unsigned long long)s_to_raw);
    if (s_to_on > 0)
        fprintf(stderr, "[TAKEOVER] textures from SetTexture: %llu stage bindings\n", (unsigned long long)s_to_tex);
    if (s_to_on > 0)
        fprintf(stderr, "[TAKEOVER] executor vertex fetches skipped: %llu (dropped, not rebuilt: %llu)\n", (unsigned long long)s_sk_skipped, (unsigned long long)s_to_drop);
    if (s_to_cmp)
        fprintf(stderr, "[TAKEOVER] whole batches compared with the executor: %llu, differing %llu\n",
                (unsigned long long)s_to_cmp, (unsigned long long)s_to_cmp_bad);
}
#define VERIFY_DRAW(prim, n, idx_, gen) do { \
        if (verify_on() && cj_gfx_mode() == 2) { cj_gfx_flush(); api_arm(prim, n, idx_); gen(); cj_gfx_flush(); api_done(); } \
        else gen(); } while (0)

/* ---- mode ---- */
static int s_mode = -1;
int cj_gfx_mode(void)
{
    if (s_mode < 0) {
        /* Default 2 (the synchronous executor) since 2026-10-26;
         * CJ_NULLHW=0 restores the poll-thread executor. */
        const char *e = getenv("CJ_NULLHW");
        s_mode = (e && *e >= '0' && *e <= '2') ? *e - '0' : 2;
        e = getenv("CJ_GFX_CENSUS");
        s_census = e && *e == '1';
    }
    return s_mode;
}

/* ---- the synchronous executor (mode 2) ---- */
extern void nv2a_pb_scan_segment(uint32_t start_va, uint32_t end_va);
extern int nv2a_pb_seg_jumped(void);
extern uint32_t nv2a_pb_seg_jump_to(void);
#define CONTIG(p) (0x80000000u | ((p) & 0x0FFFFFFFu))

static uint32_t s_last;                /* pushbuffer position executed up to (contiguous VA) */
static uint64_t s_flushes, s_words_b, s_wraps, s_odd;
int cj_gfx_in_flush;                   /* gl_backend: calls arriving from the game thread */

/* Execute everything D3D has pushed since the last flush. D3D only moves its
 * push pointer at the end of whole commands, so any point outside D3D's own
 * push code is a consistent cut; a method that a kickoff split anyway is
 * carried by the segment scan. */
void cj_gfx_flush(void)
{
    uint32_t mem_dev, put, start, limit;

    if (cj_gfx_mode() != 2)
        return;
    mem_dev = MEM32(D3D_PDEVICE);
    if (!mem_dev)
        return;
    if (MEM32(mem_dev + DEV_FLAGS) & 4u) {      /* recording a push buffer: not ours */
        s_odd++;
        return;
    }
    put = CONTIG(MEM32(mem_dev + DEV_PUT));
    start = CONTIG(MEM32(mem_dev + DEV_RING_START));
    limit = CONTIG(MEM32(mem_dev + DEV_RING_LIMIT));
    if (!s_last) {
        /* First flush: everything D3D pushed since it built the ring (the
         * device's initial state). */
        s_last = start;
        fprintf(stderr, "[GFX] sync executor: ring %08X..%08X, first flush to %08X\n",
                start, limit, put);
    }
    if (put == s_last)
        return;
    cj_gfx_in_flush = 1;
    s_flushes++;
    if (put > s_last) {
        nv2a_pb_scan_segment(s_last, put);
        s_words_b += put - s_last;
        if (nv2a_pb_seg_jumped()) {
            /* A JUMP inside what should be one straight piece: D3D wrapped
             * and has already caught up past where it was. Not expected. */
            s_odd++;
        }
    } else {
        /* The ring wrapped: D3D wrote a JUMP to the start where it ran out of
         * room. Walk on to it, then run the new lap from the jump's target. */
        uint32_t lo = start;
        nv2a_pb_seg_jumped();
        nv2a_pb_scan_segment(s_last, limit + 0x4000u);
        if (nv2a_pb_seg_jumped())
            lo = nv2a_pb_seg_jump_to();
        else
            s_odd++;
        if (put > lo)
            nv2a_pb_scan_segment(lo, put);
        s_wraps++;
    }
    s_last = put;
    cj_gfx_in_flush = 0;
}

/* D3D::CDevice::KickOff, thiscall (ecx = device). In modes 1 and 2 the device
 * is told that kickoffs never touch the GPU before the first one (otherwise
 * the first kickoff waits for the real GPU to go idle). */
void sub_002E7C70(void)
{
    COUNT(F_KICK);
    if (cj_gfx_mode()) {
        uint32_t dev = g_ecx;
        MEM32(dev + DEV_FLAGS) |= 0x2000u;
        cj_gfx_flush();
    }
    sub_002E7C70_gen();
}

void sub_002EFFB0(void)
{
    static uint32_t n;
    COUNT(F_SWAP);
    s_swap_n++;
    trace_swap();
    cj_pace_before_swap();      /* src/frame_pacer.c: even present cadence, no catch-up */
    sub_002EFFB0_gen();
    cj_pace_after_swap();
    if (++n % 600 == 0 && cj_gfx_mode() == 2)
        fprintf(stderr, "[GFX] sync: flushes %llu bytes %llu wraps %llu odd %llu\n",
                (unsigned long long)s_flushes, (unsigned long long)s_words_b,
                (unsigned long long)s_wraps, (unsigned long long)s_odd);
    if (s_census > 0 && n % 600 == 0)
        census_report();
    if (s_to_on > 0 && n % 600 == 0)   /* the native renderer at work (player logs) */
        fprintf(stderr, "[TAKEOVER] %llu batches drawn natively, %llu of them with the executor's raw vertices\n",
                (unsigned long long)s_to_taken, (unsigned long long)s_to_raw);
    if (full_verify() && n % 300 == 0) {   /* the report only with CJ_API_VERIFY=1: takeover alone compares nothing */
        if (!s_vlog && (s_vlog = fopen("apiverify.txt", "w"))) {
            /* Which build and settings made this report: a log sent in from a player's PC is only
             * evidence for the exe that wrote it. */
            const char *ev[] = {"CJ_API_VERIFY", "CJ_API_TAKEOVER", "CJ_GL_RING", "CJ_GL", "CJ_WIDESCREEN"};
            int k;
#ifndef CJ_BUILD_ID
#define CJ_BUILD_ID "unknown"
#endif
            fprintf(s_vlog, "[APIV] build %s compiled %s %s |", CJ_BUILD_ID, __DATE__, __TIME__);
            for (k = 0; k < (int)(sizeof ev / sizeof ev[0]); k++) {
                const char *v = getenv(ev[k]);
                fprintf(s_vlog, " %s=%s", ev[k], v ? v : "-");
            }
            fprintf(s_vlog, "\n");
        }
        verify_report();
        if (s_vlog) fflush(s_vlog);
    }
}

/* ---- pass-throughs (census only, for now) ---- */
/* The particle code (XBParticle) writes constants straight into the push buffer between BeginPush and
 * EndPush: SET_TRANSFORM_CONSTANT_LOAD (0x1EA4, the first constant) then SET_TRANSFORM_CONSTANT (0xB80..)
 * data. Walk the dwords it wrote at EndPush and keep them in the constant shadow like the API writes. */
static uint32_t s_bp_start;
static void vc_write(int32_t first, uint32_t ptr, uint32_t count);
void sub_002E7BF0(void) { COUNT(F_BPUSH); TR("BeginPush", fprintf(s_tf, " dwords=%u", ARG(0))); sub_002E7BF0_gen(); s_bp_start = g_eax; }
static void push_constants(uint32_t from, uint32_t to)
{
    uint32_t p = from, idx = 0xFFFFFFFFu;
    if (from < 0x10000u || to <= from || to - from > 4096 * 4 || to >= 0x84000000u) return;
    while (p + 4 <= to) {
        uint32_t h = MEM32(p), method = h & 0x1FFC, n = (h >> 18) & 0x7FF, k = 0;
        p += 4;
        if (!n || p + 4 * n > to) break;
        if (method == 0x1EA4) idx = MEM32(p);                       /* SET_TRANSFORM_CONSTANT_LOAD (4 dwords per constant, index in constants) */
        else if (method >= 0xB80 && method < 0xC00 && idx < 192) {  /* SET_TRANSFORM_CONSTANT: n dwords, 4 per constant */
            vc_write((int32_t)idx, p, n / 4);
            if (idx >= 132 && idx <= 136) s_pp_const++;
            idx += n / 4; k = n;
        }
        (void)k;
        p += 4 * n;
    }
}
/* A push that draws (SET_BEGIN_END prim, INLINE_ARRAY data, SET_BEGIN_END 0): its primitive, data and size. */
static int push_draw(uint32_t from, uint32_t to, uint32_t *prim, uint32_t *data, uint32_t *dwords)
{
    uint32_t p = from, pr = 0, d = 0, nd = 0, draws = 0;
    if (from < 0x10000u || to <= from || to - from > 4096 * 4 || to >= 0x84000000u) return 0;
    while (p + 4 <= to) {
        uint32_t h = MEM32(p), method = h & 0x1FFC, n = (h >> 18) & 0x7FF;
        p += 4;
        if (!n || p + 4 * n > to) return 0;
        if (method == 0x17FC) { uint32_t v = MEM32(p); if (v) { if (pr) return 0; pr = v; } else if (pr) draws++; }
        else if (method == 0x1818) { if (!pr || d) return 0; d = p; nd = n; }
        p += 4 * n;
    }
    if (draws != 1 || !d) return 0;
    *prim = pr; *data = d; *dwords = nd;
    return 1;
}
/* The bytes one vertex takes in the current vertex shader's declaration (stream 0, packed). */
static uint32_t vs_stride(void)
{
    uint32_t dev = MEM32(D3D_PDEVICE), vs = dev ? MEM32(dev + 0x794) : 0, a, end = 0;
    if (!vs) return 0;
    for (a = 0; a < 16; a++) {
        uint32_t slot = MEM8(0x00473DB8u + (MEM32(vs + 4) & 0x10) + a), sl = vs + 0x14 + 16 * slot;
        uint32_t stream = MEM32(sl), off = MEM32(sl + 4), fmt = MEM32(sl + 8), type = fmt & 15, size = (fmt >> 4) & 15, b;
        if (fmt == 2 || !size) continue;
        if (stream) return 0;
        b = type == 2 ? 4 * size : (type == 0 || type == 4) ? size : (type == 1 || type == 5) ? 2 * size : type == 6 ? 4 : 0;
        if (!b) return 0;
        if (off + b > end) end = off + b;
    }
    return end;
}

void sub_002E7C10(void)
{
    uint32_t prim, data, dw, stride;
    COUNT(F_EPUSH);
    if (verify_on() && s_bp_start) {
        push_constants(s_bp_start, ARG(0));
        if (!s_tf && cj_gfx_mode() == 2 && push_draw(s_bp_start, ARG(0), &prim, &data, &dw) && (stride = vs_stride()) != 0 && (dw * 4) % stride == 0) {
            s_bp_start = 0;
            cj_gfx_flush();
            api_arm(prim, dw * 4 / stride, 0x80000000u); s_ad.up = data; s_ad.up_stride = stride;
            sub_002E7C10_gen();
            cj_gfx_flush();
            api_done(); s_ad.up = 0; s_pd_draws++;
            return;
        }
    }
    s_bp_start = 0;
    TRDRAW("EndPush", (void)0, sub_002E7C10_gen);
}
void sub_002E9A50(void) { COUNT(F_CVIEW); sub_002E9A50_gen(); }
void sub_002EA910(void) { COUNT(F_SELD); sub_002EA910_gen(); }
/* constant shadow: the game's SetVertexShaderConstant* calls, register + 96 as the chip numbers them */
/* first: the chip's register number (0..191). The Fast calls are __fastcall (ecx register, edx data,
 * NotInlineFast's dword count on the stack) and take the chip number; the others are stdcall with
 * the register as the shader sees it (-96..95) */
static void vc_write(int32_t first, uint32_t ptr, uint32_t count)
{
    {   static uint32_t seen[400]; static int n; uint32_t key = ((uint32_t)first << 12) | count; int k; for (k = 0; k < n && seen[k] != key; k++) ; if (k == n && n < 400 && s_vlog) { seen[n++] = key; fprintf(s_vlog, "[vcw] first %d ptr %08X count %u\n", first, ptr, count); } }
    if (first < 0 || first >= 192 || ptr < 0x10000u || ptr >= 0x84000000u || count > 192) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t r = (uint32_t)first + i, k;
        if (r >= 192) break;
        for (k = 0; k < 4; k++) { uint32_t w = MEM32(ptr + 16 * i + 4 * k); memcpy(&s_vc[r][k], &w, 4); }
        s_vc_set[r] = 1;
    }
}
void sub_002EAD70(void) { COUNT(F_C1); vc_write((int32_t)ARG(0) + 96, ARG(1), 1); sub_002EAD70_gen(); }
void sub_002EADD0(void) { COUNT(F_C1F); vc_write((int32_t)g_ecx, g_edx, 1); sub_002EADD0_gen(); }
void sub_002EAE20(void) { COUNT(F_C4); vc_write((int32_t)ARG(0) + 96, ARG(1), 4); sub_002EAE20_gen(); }
void sub_002EAED0(void) { COUNT(F_CNIF); vc_write((int32_t)g_ecx, g_edx, ARG(0) / 4); sub_002EAED0_gen(); }
void sub_002EAFB0(void) { COUNT(F_CNI); vc_write((int32_t)ARG(0) + 96, ARG(1), ARG(2) / 4); sub_002EAFB0_gen(); }
void sub_002EB260(void) { COUNT(F_STREAM); TR("SetStreamSource", (fprintf(s_tf, " stream=%u stride=%u", ARG(0), ARG(2)), tr_res("vb", ARG(1)))); sub_002EB260_gen(); }
void sub_002EB390(void) { COUNT(F_LVS); sub_002EB390_gen(); }
void sub_002EB3F0(void) { COUNT(F_LVSP); sub_002EB3F0_gen(); }
void sub_002EB460(void) { COUNT(F_SVS); sub_002EB460_gen(); }
void sub_002EB550(void) { COUNT(F_CMODE); sub_002EB550_gen(); }
void sub_002EB7C0(void) { COUNT(F_SETVS); TR("SetVertexShader", fprintf(s_tf, " handle=%08X", ARG(0))); sub_002EB7C0_gen(); }
void sub_002EBD00(void) { COUNT(F_VSIN); sub_002EBD00_gen(); }
static void rt_note(uint32_t res, int z)
{
    if (res) rt_note_va(nv2a_dma_resolve(MEM32(res + 4)), z);
}
static void rt_note_va(uint32_t va, int z)
{
    int i;
    va &= 0x0FFFFFFFu;
    for (i = 0; i < s_rt_nseen; i++) if (s_rt_seen[i][0] == va && s_rt_seen[i][1] == (uint32_t)z) return;
    if (s_rt_nseen < 64) { s_rt_seen[s_rt_nseen][0] = va; s_rt_seen[s_rt_nseen][1] = (uint32_t)z; s_rt_nseen++; }
}
/* Aliasing check: every time the back end samples one of its surfaces as a texture, the surface must be one
 * the game set with SetRenderTarget (colour, or depth for a shadow map). */
void cj_api_check_rtt(uint32_t surf_va, uint32_t tex_va, int zeta)
{
    int i;
    if (!full_verify()) return;
    for (i = 0; i < s_rt_nseen; i++) if (s_rt_seen[i][0] == (surf_va & 0x0FFFFFFFu) && s_rt_seen[i][1] == (uint32_t)zeta) { s_rtt_ok++; return; }
    s_rtt_bad++;
    bad(13, "texture %08X sampled from back-end surface %08X (%s) that the game never set as a render target", tex_va, surf_va, zeta ? "depth" : "colour");
}
void sub_002ECE50(void) { COUNT(F_RT); rt_note(ARG(0), 0); rt_note(ARG(1), 1); s_rt_c = ARG(0); s_rt_z = ARG(1); s_vp[0] = s_vp[1] = 0; s_vp[2] = s_vp[3] = 0x7FFFFFFFu; memcpy(s_view, s_vp, sizeof s_view); TR("SetRenderTarget", (tr_res("rt", ARG(0)), tr_res("z", ARG(1)))); sub_002ECE50_gen(); }
void sub_002ED0D0(void) { COUNT(F_RTF); rt_note(ARG(0), 0); rt_note(ARG(1), 1); s_rt_c = ARG(0); s_rt_z = ARG(1); s_vp[0] = s_vp[1] = 0; s_vp[2] = s_vp[3] = 0x7FFFFFFFu; memcpy(s_view, s_vp, sizeof s_view); TR("SetRenderTargetFast", (tr_res("rt", ARG(0)), tr_res("z", ARG(1)), fprintf(s_tf, " flags=%X", ARG(2)))); sub_002ED0D0_gen(); }

/* D3DDevice_SetScissors (the game's own calls reach it from other translation units, so the linker's
 * --wrap sees them; SetViewport's internal call stays inside its own unit and is covered by the
 * SetViewport hook): the first rectangle is the NV2A surface clip. */
extern void __real_m_D3DDevice_SetScissors_002EEA50(void);
void __wrap_m_D3DDevice_SetScissors_002EEA50(void)
{
    uint32_t count = ARG(0), excl = ARG(1), r = ARG(2);
    if (count >= 1 && !excl && r) {
        int32_t x1 = (int32_t)MEM32(r), y1 = (int32_t)MEM32(r + 4), x2 = (int32_t)MEM32(r + 8), y2 = (int32_t)MEM32(r + 12);
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        s_vp[0] = (uint32_t)x1; s_vp[1] = (uint32_t)y1; s_vp[2] = x2 > x1 ? (uint32_t)(x2 - x1) : 0; s_vp[3] = y2 > y1 ? (uint32_t)(y2 - y1) : 0;
    } else if (!count) { s_vp[0] = s_vp[1] = 0; s_vp[2] = s_vp[3] = 0x7FFFFFFFu; }
    __real_m_D3DDevice_SetScissors_002EEA50();
}
void sub_002ED950(void) { COUNT(F_VIEW); { uint32_t v = ARG(0); if (v) { s_vp[0] = MEM32(v); s_vp[1] = MEM32(v + 4); s_vp[2] = MEM32(v + 8); s_vp[3] = MEM32(v + 12); } else { s_vp[0] = s_vp[1] = 0; s_vp[2] = s_vp[3] = 0x7FFFFFFFu; } memcpy(s_view, s_vp, sizeof s_view); } TR("SetViewport", { uint32_t v = ARG(0); if (v) fprintf(s_tf, " %u,%u %ux%u", MEM32(v), MEM32(v + 4), MEM32(v + 8), MEM32(v + 12)); }); sub_002ED950_gen(); }
void sub_002EDFD0(void) { COUNT(F_TEX); if (ARG(0) < 4) s_tex[ARG(0)] = ARG(1); TR("SetTexture", (fprintf(s_tf, " stage=%u", ARG(0)), tr_res("tex", ARG(1)))); sub_002EDFD0_gen(); }
void sub_002EE240(void) { COUNT(F_IDX); TR("SetIndices", fprintf(s_tf, " ib=%08X base=%u", ARG(0), ARG(1))); sub_002EE240_gen(); }
/* Step 9b: Clear(Count, pRects, Flags, Color, Z, Stencil). While the executor runs the clear this call
 * becomes, s_cl holds the game's arguments; gl_clear asks cj_api_clear to check them against what the
 * chip was told and, with CJ_API_TAKEOVER=1, to use them. Calls with rectangles stay the executor's. */
static struct { int armed; uint32_t count, flags, color, zbits, stencil; } s_cl;
static uint32_t s_cl_rect[2];
int cj_api_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t *flags, uint32_t *argb, uint32_t *zs, Nv2aSurface *so, Nv2aRenderState *rso)
{
    Nv2aSurface a; float zf; uint32_t fl = s_cl.flags & 0xF3, z, ok = 1, have;
    if (!s_cl.armed) return 0;
    if (s_cl.count) { s_cl_skip++; return 0; }
    s_cl_cmp++;
    memcpy(&zf, &s_cl.zbits, 4);
    if (!(zf > 0.f)) zf = 0.f;
    if (zf > 1.f) zf = 1.f;
    z = ((uint32_t)(zf * 16777215.0f) << 8) | (s_cl.stencil & 0xFF);
    if (fl != (*flags & 0xF3)) { bad(14, "flags API %X executor %X", fl, *flags & 0xF3); ok = 0; }
    if ((fl & 0xF0) && s->bytes_per_pixel == 4 && s_cl.color != *argb) { bad(14, "colour API %08X executor %08X", s_cl.color, *argb); ok = 0; }
    if ((fl & 1) && (z >> 8) != (*zs >> 8)) { bad(14, "depth API %06X executor %06X (z %08X)", z >> 8, *zs >> 8, s_cl.zbits); ok = 0; }
    if ((fl & 2) && (z & 0xFF) != (*zs & 0xFF)) { bad(14, "stencil API %02X executor %02X", z & 0xFF, *zs & 0xFF); ok = 0; }
    have = api_surface(s, &a);
    if (have && (a.color_va != s->color_va || a.width != s->width || a.height != s->height || a.pitch != s->pitch ||
                 a.bytes_per_pixel != s->bytes_per_pixel || a.clip_x != s->clip_x || a.clip_y != s->clip_y)) {
        bad(14, "surface API va %08X %ux%u clip %u,%u | executor va %08X %ux%u clip %u,%u | viewport %u,%u %ux%u", a.color_va, a.width, a.height, a.clip_x, a.clip_y,
            s->color_va, s->width, s->height, s->clip_x, s->clip_y, s_vp[0], s_vp[1], s_vp[2], s_vp[3]);
        ok = 0;
    }
    {   /* the area: the viewport within the surface (inclusive bounds as the chip takes them) */
        uint32_t x0 = s_view[0], y0 = s_view[1], x1, y1, sw = have ? (a.pitch / (a.bytes_per_pixel ? a.bytes_per_pixel : 4)) : 0, sh;
        uint32_t fmt = s_rt_c ? MEM32(s_rt_c + 12) : 0, size = s_rt_c ? MEM32(s_rt_c + 16) : 0, color = (fmt >> 8) & 0xFF;
        if (d3d8_format_is_swizzled(color)) { sw = 1u << ((fmt >> 20) & 0xF); sh = 1u << ((fmt >> 24) & 0xF); }
        else { sw = (size & 0xFFF) + 1; sh = ((size >> 12) & 0xFFF) + 1; }
        x1 = s_view[2] > sw ? sw : s_view[2]; y1 = s_view[3] > sh ? sh : s_view[3];
        if (x0 > sw) x0 = sw;
        if (y0 > sh) y0 = sh;
        x1 = x0 + (x1 > sw - x0 ? sw - x0 : x1); y1 = y0 + (y1 > sh - y0 ? sh - y0 : y1);
        s_cl_rect[0] = x0 | ((x1 ? x1 - 1 : 0) << 16); s_cl_rect[1] = y0 | ((y1 ? y1 - 1 : 0) << 16);
        if (rs && (rs->clear_rect_set & 3) == 3 && (!have || rs->clear_rect_h != s_cl_rect[0] || rs->clear_rect_v != s_cl_rect[1])) {
            bad(14, "clear rect executor %u..%u x %u..%u, API viewport %u..%u x %u..%u", rs->clear_rect_h & 0xFFFF, rs->clear_rect_h >> 16, rs->clear_rect_v & 0xFFFF, rs->clear_rect_v >> 16,
                s_cl_rect[0] & 0xFFFF, s_cl_rect[0] >> 16, s_cl_rect[1] & 0xFFFF, s_cl_rect[1] >> 16);
            ok = 0;
        }
    }
    if (ok) s_cl_ok++;
    if (s_to_on > 0 && ok && have) {
        *flags = (*flags & ~0xF3u) | fl;
        if ((fl & 0xF0) && s->bytes_per_pixel == 4) *argb = s_cl.color;
        *zs = ((fl & 1) ? (z & ~0xFFu) : (*zs & ~0xFFu)) | ((fl & 2) ? (z & 0xFF) : (*zs & 0xFF));
        *so = a;
        if (rs) { *rso = *rs; rso->clear_rect_h = s_cl_rect[0]; rso->clear_rect_v = s_cl_rect[1]; rso->clear_rect_set = 3; }   /* the viewport, from the API */
        s_cl_taken++;
        return 1;
    }
    return 0;
}
void sub_002F02E0(void)
{
    COUNT(F_CLEAR);
    {   /* CJ_TEST_GADGETS=1 (test routes only): give the player every vision mode so a scripted run can
         * cycle them (XItemHandler_Player::m_pInstance 0x48FC98, m_VisionModeFlags +0xC94) */
        static int g = -1;
        if (g < 0) { const char *e = getenv("CJ_TEST_GADGETS"); g = e && *e == '1'; }
        if (g) {
            uint32_t pl = MEM32(0x0048FC98u);
            if (pl >= 0x10000u && pl < 0x84000000u) {
                static uint32_t last;
                uint32_t h0 = MEM32(0x00483728u), h1 = MEM32(0x0048372Cu), held = MEM32(pl + 0xF4);
                uint32_t key = held ^ (MEM8(pl + 0x30C) << 24) ^ (MEM8(pl + 0xC70) << 16);
                MEM32(pl + 0xC94) = 0xFFFFFFFFu;
                if (key != last) {
                    last = key;
                    fprintf(stderr, "[GADGETS] held %08X control %d tint %u next %u hud %08X/%08X b8c %d/%d\n", held, (int8_t)MEM8(pl + 0x30C), MEM8(pl + 0xC70), MEM8(pl + 0xC71),
                            h0, h1, h0 >= 0x10000u && h0 < 0x84000000u ? (int32_t)MEM32(h0 + 0xB8C) : -1, h1 >= 0x10000u && h1 < 0x84000000u ? (int32_t)MEM32(h1 + 0xB8C) : -1);
                }
            }
        }
    }
    if (verify_on() && !s_tf && cj_gfx_mode() == 2) {
        cj_gfx_flush();
        s_cl.count = ARG(0); s_cl.flags = ARG(2); s_cl.color = ARG(3); s_cl.zbits = ARG(4); s_cl.stencil = ARG(5); s_cl.armed = 1;
        sub_002F02E0_gen();
        cj_gfx_flush();
        s_cl.armed = 0;
        return;
    }
    TRDRAW("Clear", fprintf(s_tf, " rects=%u flags=%X colour=%08X z=%08X stencil=%u", ARG(0), ARG(2), ARG(3), ARG(4), ARG(5)), sub_002F02E0_gen);
}
void sub_002F09C0(void) { COUNT(F_DUP); if (verify_on() && !s_tf) { uint32_t p_ = ARG(0), n_ = ARG(1); s_ad.up = ARG(2); s_ad.up_stride = ARG(3); VERIFY_DRAW(p_, n_, 0x80000000u, sub_002F09C0_gen); s_ad.up = 0; return; } TRDRAW("DrawVerticesUP", fprintf(s_tf, " %s n=%u data=%08X stride=%u", prim_name(ARG(0)), ARG(1), ARG(2), ARG(3)), sub_002F09C0_gen); }
void sub_002F0B30(void) { COUNT(F_DIUP); if (verify_on() && !s_tf) { uint32_t p_ = ARG(0), n_ = ARG(1), i_ = ARG(2); s_ad.up = ARG(3); s_ad.up_stride = ARG(4); VERIFY_DRAW(p_, n_, i_, sub_002F0B30_gen); s_ad.up = 0; return; } TRDRAW("DrawIndexedVerticesUP", fprintf(s_tf, " %s n=%u idx=%08X data=%08X stride=%u", prim_name(ARG(0)), ARG(1), ARG(2), ARG(3), ARG(4)), sub_002F0B30_gen); }
void sub_002F0C70(void) { COUNT(F_DV); if (verify_on() && !s_tf) { uint32_t p_ = ARG(0), n_ = ARG(2); VERIFY_DRAW(p_, n_, 0x80000000u | ARG(1), sub_002F0C70_gen); return; } TRDRAW("DrawVertices", fprintf(s_tf, " %s start=%u n=%u", prim_name(ARG(0)), ARG(1), ARG(2)), sub_002F0C70_gen); }
void sub_002F0D10(void) { COUNT(F_DIV); if (verify_on() && !s_tf) { uint32_t p_ = ARG(0), n_ = ARG(1); VERIFY_DRAW(p_, n_, ARG(2), sub_002F0D10_gen); return; } TRDRAW("DrawIndexedVertices", fprintf(s_tf, " %s n=%u idx=%08X", prim_name(ARG(0)), ARG(1), ARG(2)), sub_002F0D10_gen); }
void sub_002F11E0(void) { COUNT(F_BEGIN); TR("Begin", fprintf(s_tf, " %s", prim_name(ARG(0)))); sub_002F11E0_gen(); }
void sub_002F1220(void) { COUNT(F_END); TR("End", (void)0); sub_002F1220_gen(); }
void sub_002F2510(void) { COUNT(F_PS); TR("SetPixelShader", fprintf(s_tf, " handle=%08X", ARG(0))); sub_002F2510_gen(); }

/* ---- quick save (src/quicksave.c): where the synchronous executor has got to.
 * A load puts D3D's push pointer back where it was at the save; the executor
 * must resume from the position it had then, not from where it is now (it
 * would replay or skip a stretch of the ring as commands, fence writes
 * included, and D3D's fence bookkeeping then reads garbage). */
#include "../xboxrecomp/src/kernel/xbox_snapshot.h"
int cj_gfx_snap_regions(XboxSnapRegion *r, int max)
{
    /* Also the API shadows the native renderer draws from (the game's SetTexture, viewport,
     * render targets and vertex constants): a load rewinds the game to the save, and these must
     * rewind with it or taken-over draws use the state of the moment before the load (TK46:
     * 827 batches with stale vertex constants after a quick load). */
    static const struct { void *p; size_t n; const char *name; } k[] = {
        { &s_last, sizeof s_last, "native_gfx.s_last" },
        { s_tex, sizeof s_tex, "native_gfx.s_tex" },
        { s_vp, sizeof s_vp, "native_gfx.s_vp" },
        { s_view, sizeof s_view, "native_gfx.s_view" },
        { &s_rt_c, sizeof s_rt_c, "native_gfx.s_rt_c" },
        { &s_rt_z, sizeof s_rt_z, "native_gfx.s_rt_z" },
        { s_rt_seen, sizeof s_rt_seen, "native_gfx.s_rt_seen" },
        { &s_rt_nseen, sizeof s_rt_nseen, "native_gfx.s_rt_nseen" },
        { s_vc, sizeof s_vc, "native_gfx.s_vc" },
        { s_vc_set, sizeof s_vc_set, "native_gfx.s_vc_set" },
    };
    int i, n = (int)(sizeof k / sizeof k[0]);
    for (i = 0; i < n && i < max; i++) { r[i].p = k[i].p; r[i].n = k[i].n; r[i].name = k[i].name; }
    return n;
}
