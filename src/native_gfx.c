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
#include "recomp_types.h"
#include "frame_pacer.h"

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
    cj_pace_before_swap();      /* src/frame_pacer.c: even present cadence, no catch-up */
    sub_002EFFB0_gen();
    cj_pace_after_swap();
    if (++n % 600 == 0 && cj_gfx_mode() == 2)
        fprintf(stderr, "[GFX] sync: flushes %llu bytes %llu wraps %llu odd %llu\n",
                (unsigned long long)s_flushes, (unsigned long long)s_words_b,
                (unsigned long long)s_wraps, (unsigned long long)s_odd);
    if (s_census > 0 && n % 600 == 0)
        census_report();
}

/* ---- pass-throughs (census only, for now) ---- */
void sub_002E7BF0(void) { COUNT(F_BPUSH); sub_002E7BF0_gen(); }
void sub_002E7C10(void) { COUNT(F_EPUSH); sub_002E7C10_gen(); }
void sub_002E9A50(void) { COUNT(F_CVIEW); sub_002E9A50_gen(); }
void sub_002EA910(void) { COUNT(F_SELD); sub_002EA910_gen(); }
void sub_002EAD70(void) { COUNT(F_C1); sub_002EAD70_gen(); }
void sub_002EADD0(void) { COUNT(F_C1F); sub_002EADD0_gen(); }
void sub_002EAE20(void) { COUNT(F_C4); sub_002EAE20_gen(); }
void sub_002EAED0(void) { COUNT(F_CNIF); sub_002EAED0_gen(); }
void sub_002EAFB0(void) { COUNT(F_CNI); sub_002EAFB0_gen(); }
void sub_002EB260(void) { COUNT(F_STREAM); sub_002EB260_gen(); }
void sub_002EB390(void) { COUNT(F_LVS); sub_002EB390_gen(); }
void sub_002EB3F0(void) { COUNT(F_LVSP); sub_002EB3F0_gen(); }
void sub_002EB460(void) { COUNT(F_SVS); sub_002EB460_gen(); }
void sub_002EB550(void) { COUNT(F_CMODE); sub_002EB550_gen(); }
void sub_002EB7C0(void) { COUNT(F_SETVS); sub_002EB7C0_gen(); }
void sub_002EBD00(void) { COUNT(F_VSIN); sub_002EBD00_gen(); }
void sub_002ECE50(void) { COUNT(F_RT); sub_002ECE50_gen(); }
void sub_002ED0D0(void) { COUNT(F_RTF); sub_002ED0D0_gen(); }
void sub_002ED950(void) { COUNT(F_VIEW); sub_002ED950_gen(); }
void sub_002EDFD0(void) { COUNT(F_TEX); sub_002EDFD0_gen(); }
void sub_002EE240(void) { COUNT(F_IDX); sub_002EE240_gen(); }
void sub_002F02E0(void) { COUNT(F_CLEAR); sub_002F02E0_gen(); }
void sub_002F09C0(void) { COUNT(F_DUP); sub_002F09C0_gen(); }
void sub_002F0B30(void) { COUNT(F_DIUP); sub_002F0B30_gen(); }
void sub_002F0C70(void) { COUNT(F_DV); sub_002F0C70_gen(); }
void sub_002F0D10(void) { COUNT(F_DIV); sub_002F0D10_gen(); }
void sub_002F11E0(void) { COUNT(F_BEGIN); sub_002F11E0_gen(); }
void sub_002F1220(void) { COUNT(F_END); sub_002F1220_gen(); }
void sub_002F2510(void) { COUNT(F_PS); sub_002F2510_gen(); }

/* ---- quick save (src/quicksave.c): where the synchronous executor has got to.
 * A load puts D3D's push pointer back where it was at the save; the executor
 * must resume from the position it had then, not from where it is now (it
 * would replay or skip a stretch of the ring as commands, fence writes
 * included, and D3D's fence bookkeeping then reads garbage). */
#include "../xboxrecomp/src/kernel/xbox_snapshot.h"
int cj_gfx_snap_regions(XboxSnapRegion *r, int max)
{
    if (max < 1) return 1;
    r[0].p = (void *)&s_last; r[0].n = sizeof s_last; r[0].name = "native_gfx.s_last";
    return 1;
}
