/*
 * Synchronous file I/O under CJ_DET (docs/determinism.md, "Full determinism").
 *
 * XBFileManager::FileThreadWakeUp (0x00284E60, __thiscall, no args, direct
 * calls only) normally does SetEvent(s_hFileEvent) so xbFileThread runs
 * gFileManager.Update() until it returns 0. The order in which those reads
 * finish relative to the main thread decided the boot heap layout. With
 * CJ_DET=1 the wake-up instead pumps EXFileManager::Update (0x002C8B00,
 * ecx = &gFileManager) on the guest main thread, xbFileThread is parked (see
 * cj_det_lookup in determinism.c), and the main thread also pumps before every
 * MainUpdate, in every main-thread Sleep (KeDelayExecutionThread) and in
 * every EXFileSys::Poll (0x00221400, below), so queued loads progress in a
 * fixed order.
 *
 * Without CJ_DET the wrapper calls the generated body unchanged.
 *
 * This file is also part of the recompiler's --exclude-manual input
 * (cat build/hybrid/hybrid_swap.c src/det_file.c > build/exclude_manual.c, RUN.md):
 * the `extern ..._gen` line below makes tools.recomp emit the original body as
 * sub_00284E60_gen and route direct calls to this sub_00284E60.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include "recomp_types.h"

extern void sub_00284E60_gen(void);
extern void sub_00221400_gen(void);
extern int cj_det_enabled(void);

#define GFILEMANAGER_VA 0x004CE038u
#define FM_UPDATE_VA    0x002C8B00u     /* EXFileManager::Update, __thiscall, returns int */
#define DET_RET_VA      0x0000C0DEu

uint64_t cj_det_pumps, cj_det_updates;
static __thread int s_in_pump;
static pthread_mutex_t s_pump_mx = PTHREAD_MUTEX_INITIALIZER;
uint64_t cj_det_offthread;
extern int cj_det_is_main(void);
extern void cj_det_tick_pump(void);

/* Run gFileManager.Update() until it reports no more work. Re-entrant calls
 * (a load callback that queues another file and wakes the thread) return at
 * once: the outer loop keeps calling Update until it returns 0 anyway. */
void cj_det_file_pump(void)
{
    static recomp_func_t upd;
    uint32_t eax0 = g_eax, ecx0 = g_ecx, edx0 = g_edx, esp0 = g_esp;
    int n = 0;
    if (s_in_pump) return;
    s_in_pump = 1;
    pthread_mutex_lock(&s_pump_mx);
    if (cj_det_is_main()) cj_det_tick_pump();
    if (!upd) upd = recomp_lookup(FM_UPDATE_VA);
    cj_det_pumps++;
    for (;;) {
        PUSH32(g_esp, DET_RET_VA);
        g_ecx = GFILEMANAGER_VA;
        upd();
        g_esp = esp0;
        cj_det_updates++;
        if (!g_eax) break;
        if (++n > 1000000) { fprintf(stderr, "[DET] file pump: Update never returned 0\n"); break; }
    }
    g_eax = eax0; g_ecx = ecx0; g_edx = edx0;
    pthread_mutex_unlock(&s_pump_mx);
    s_in_pump = 0;
}

#ifndef CJ_NO_FILE_WRAP
void sub_00284E60(void)
{
    if (!cj_det_enabled()) { sub_00284E60_gen(); return; }
    /* Only the guest main thread pumps: a wake-up from another thread is left
     * to the main thread's next Sleep or MainUpdate, so the order of loads
     * does not depend on thread timing. */
    if (cj_det_is_main()) cj_det_file_pump(); else cj_det_offthread++;
    g_esp += 4;     /* ret */
}

/* EXFileSys::Poll (0x00221400, __thiscall, no args): EXFileSys::WaitSyncOnFile
 * busy-waits on Poll() with no Sleep and no wake-up, relying on xbFileThread's
 * 8 ms poll. Under CJ_DET the main thread pumps here first. */
void sub_00221400(void)
{
    if (cj_det_enabled() && cj_det_is_main()) cj_det_file_pump();
    sub_00221400_gen();
}

/* Intro movie under CJ_DET (docs/determinism.md, round 5). XMVDecoder_GetNextFrame
 * paces video by D3DDevice_GetDisplayFieldStatus's VBlankCount (D3D device +0x1DE8,
 * incremented by the vblank ISR on host time) and, with an audio stream enabled,
 * by that DirectSound stream (packet completion and SynchonizationCallback's offset,
 * both APU = host time). With CJ_DET (and CJ_DET_MOVIE not 0):
 *  - GetDisplayFieldStatus reports VBlankCount = frame x lockstep vblanks;
 *  - XMVDecoder_EnableAudioStream does nothing and returns S_OK, so the movie plays
 *    without its audio stream and GetNextFrame paces by the vblank count only;
 *  - XMVDecoder_GetAudioStream returns NULL for a stream that is not enabled
 *    (the original dereferences it for AddRef; xbMovieUpdate passes NULL on to
 *    xbAudioDSS_SetMixBins and the close path, which both test for it).
 * The movie's file reads already go EXBaseSoundManager::StreamRead -> command block
 * -> EXAudio::psiAsyncReadFile -> FileThreadWakeUp, i.e. the synchronous pump above. */
extern void sub_002EE440_gen(void);
extern void sub_00356BA9_gen(void);
extern void sub_00356D49_gen(void);
extern int cj_det_movie(void);
extern uint32_t cj_det_vblanks(void);

/* D3DDevice_GetDisplayFieldStatus(D3DFIELD_STATUS *) stdcall: {Field, VBlankCount} */
void sub_002EE440(void)
{
    uint32_t p = MEM32(g_esp + 4);
    sub_002EE440_gen();
    if (cj_det_movie()) {
        /* xbMovieUpdate's first update busy-waits on GetNextFrame until a frame
         * comes out, and the read-ahead behind it only sends its read command when
         * GetTickCount has moved 16 ms (EXBaseSoundManager::CommandUpdate). Both
         * clocks are frozen within a CJ_DET frame, so more than 4 calls in one
         * frame count as waiting: each further call is 1 ms of tick, and every
         * 16 ms is one more vblank. Deterministic: a function of the call count. */
        extern uint32_t cj_det_frame;
        static uint32_t s_frame = ~0u, s_calls, s_wait_ms;
        uint32_t n;
        if (cj_det_frame != s_frame) { s_frame = cj_det_frame; s_calls = 0; }
        if (++s_calls > 4 && cj_det_is_main()) { s_wait_ms++; cj_det_tick_pump(); }
        n = cj_det_vblanks() + s_wait_ms / 16u;
        MEM32(p + 4) = n;
        if (MEM32(p) != 3) MEM32(p) = 1 + (n & 1);   /* interlaced: field parity from the count */
    }
}

/* Movie audio in normal play (CJ_MOVIE_AUDIO). With its audio stream on, an XMV
 * movie fetches its next packet only when DirectSound has completed the last
 * one, and SynchonizationCallback shifts presentation times by the audio
 * offset: a host whose audio output underruns (the APU frame thread starved,
 * e.g. Wine/Rosetta on a busy Mac) makes the video stutter and can stall it.
 * Without the stream the video is paced by the vblank count alone (as under
 * CJ_DET) and the movie is silent.
 *   CJ_MOVIE_AUDIO=1     always play the audio stream (the original behaviour)
 *   CJ_MOVIE_AUDIO=0     never: movies silent, paced by vblanks only
 *   unset / auto         play it unless the host audio has already underrun
 *                        CJ_MOVIE_AUDIO_UNDERRUNS times (default 3) when the
 *                        movie enables its stream; decided per movie. */
extern unsigned xa2_underruns(void);
static int s_movie_noaudio;
static int movie_audio_off(void)
{
    const char *e = getenv("CJ_MOVIE_AUDIO"), *t = getenv("CJ_MOVIE_AUDIO_UNDERRUNS");
    unsigned lim = t && atoi(t) > 0 ? (unsigned)atoi(t) : 3u, u = xa2_underruns();
    int off;
    if (e && *e == '0') off = 1;
    else if (e && *e == '1') off = 0;
    else off = u >= lim;
    fprintf(stderr, "[MOVIE] audio stream %s (CJ_MOVIE_AUDIO=%s, host audio underruns so far %u)\n",
            off ? "off: video paced by vblanks only" : "on", e ? e : "auto", u);
    return off;
}

/* XMVDecoder_EnableAudioStream(decoder, stream, flags, mixbins, &out) stdcall, ret 0x14 */
void sub_00356BA9(void)
{
    if (!cj_det_movie()) {
        s_movie_noaudio = movie_audio_off();
        if (!s_movie_noaudio) { sub_00356BA9_gen(); return; }
    }
    if (MEM32(g_esp + 20)) MEM32(MEM32(g_esp + 20)) = 0;
    g_eax = 0;
    g_esp += 4 + 20;
}

/* XMVDecoder_GetAudioStream(decoder, stream, &out) stdcall, ret 0xc */
void sub_00356D49(void)
{
    uint32_t dec = MEM32(g_esp + 4), idx = MEM32(g_esp + 8), out = MEM32(g_esp + 12);
    if ((cj_det_movie() || s_movie_noaudio) && MEM32(MEM32(dec + 0x134) + idx * 4) == 0) {
        MEM32(out) = 0;
        g_esp += 4 + 12;
        return;
    }
    sub_00356D49_gen();
}
#endif
