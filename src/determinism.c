/*
 * Frame-count based runs and vblank lockstep (docs/determinism.md).
 *
 * Reached through recomp_lookup_manual: EXApp::MainUpdate is only called
 * through the vtable and XBVerticalBlankCallback only through D3D's callback
 * pointer, so both are indirect calls and need no regeneration of
 * src/recomp/gen.
 *
 *   CJ_LOCKSTEP=K     the game sees exactly K vblanks per MainUpdate: the vblank
 *                     callback stops counting and MainUpdate's counter
 *                     (EXDisplay+0x980, total at +0x984) is set to K before each
 *                     call. Frame timing then no longer depends on host speed.
 *   CJ_STOP_FRAME=N   after the Nth MainUpdate: print the swap report, give the
 *                     GPU thread a moment to dump its last flips, exit(0).
 *   CJ_DET_HASH=1     per frame: hash of .data and of the guest heap window,
 *                     plus a cumulative swap report every CJ_DET_REPORT frames.
 *   CJ_DET=1          full determinism: synchronous file I/O (det_file.c, the
 *                     file thread is parked) and a clock derived from the frame
 *                     count (rdtsc, KeQuerySystemTime, KeTickCount; toolkit
 *                     patch 0003). Implies CJ_LOCKSTEP=2 unless set. Also
 *                     parks xbMixerThread and runs its loop body on the main
 *                     thread before every MainUpdate (CJ_DET_MIX=0: off), and
 *                     sets RECOMP_HELPER_TIB_POOL=1 (toolkit patch 0004).
 *   CJ_DET_DUMP=n,..  raw .data/heap dumps before MainUpdate n into (CJ_DET_DUMP_CONT=1:
 *                     also contiguous memory, cont_f<n>.bin) into
 *                     $CJ_DET_DUMP_DIR (data_f<n>.bin, heap_f<n>.bin).
 *                     Also turns on stack hygiene (CJ_DET_STACK0=0: off): every
 *                     guest stack allocation, lifted (toolkit patch 0005) or native
 *                     (guest_rt.c cjg_set_esp), starts zeroed.
 *   CJ_DET_WATCH=VA   hardware write watchpoint on a guest dword in every
 *                     thread (perf_event), printing a backtrace per write.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "recomp_types.h"

typedef void (*det_fn)(void);

uint32_t cj_det_frame;

extern void m_MainUpdate_EXApp__UAECXZ_00217730(void);
extern void m_XBVerticalBlankCallback__YAXPAU_D3DVBLANKDATA_xb___Z_002238E0(void);
void cj_swap_report(const char *tag) __attribute__((weak));
void cj_det_file_pump(void);
extern uint64_t cj_det_pumps, cj_det_updates, cj_det_offthread;
/* toolkit (patches/v0.15/0003-det-time-hooks.patch) */
extern uint64_t (*xbox_det_tsc_hook)(void);
extern int64_t (*xbox_det_systime_hook)(void);
extern volatile int xbox_det_time;

#define DISPLAY_PTR_VA 0x0049E110u   /* EXBaseDisplay::m_pDisplay */
#define DATA_LO 0x00394740u          /* .data */
#define DATA_HI 0x00500E54u
#define HEAP_LO 0x00F80000u          /* toolkit heap window (threads' stacks included) */
#define HEAP_HI 0x04000000u

static int s_init, s_lockstep, s_hash, s_report = 16, s_det;
static uint32_t s_stop;

#define KETICKCOUNT_THUNK 0x00386610u   /* import slot: VA of KeTickCount */
#define TSC_HZ 733333333ull
#define SYSTIME_ANCHOR 0x01C4EF94D6B90000ll  /* 2005-01-01 00:00 UTC, 100 ns since 1601 */

/* Frame-derived clock: frame * (one game frame) plus a per-thread call count,
 * so each thread sees a strictly increasing clock whose values depend only on
 * its own call sequence and the frame count. */
static __thread uint64_t t_tsc_n, t_sys_n;
static uint64_t det_frame_vblanks(void) { return (uint64_t)cj_det_frame * (uint64_t)(s_lockstep ? s_lockstep : 2); }
static uint64_t det_tsc(void) { return det_frame_vblanks() * (TSC_HZ / 60u) + (++t_tsc_n) * 64u; }
static int64_t det_systime(void) { return SYSTIME_ANCHOR + (int64_t)(det_frame_vblanks() * (10000000ull / 60u) + (++t_sys_n)); }
int cj_det_enabled(void) { return s_det; }
static int s_det_movie;
int cj_det_movie(void) { return s_det_movie; }      /* det_file.c: movie pacing/audio under CJ_DET */
uint32_t cj_det_vblanks(void) { return (uint32_t)det_frame_vblanks(); }

/* KeTickCount under CJ_DET: at least frame * frame length, plus 1 ms for every
 * main-thread file pump (EXFileSys::Poll, Sleep, wake-up): boot code waits
 * for loads before frame 1, and the file manager's throttles and stats read
 * the millisecond timer, so it must move without frames. Monotonic, and a
 * function of the main thread's call sequence only. */
static uint32_t s_tick;
static int s_det_time, s_det_mix;
static void det_tick_set(uint32_t t)
{
    if (!s_det_time) return;
    uint32_t va = MEM32(0x00386610u);      /* import slot: VA of KeTickCount */
    if (t > s_tick) s_tick = t;
    if (va) MEM32(va) = s_tick;
}
void cj_det_tick_pump(void) { det_tick_set(s_tick + 1); }
/* The clock state outside guest RAM (this thread's rdtsc / system-time call
 * counts, the tick), for the dual-execution verifier (swap_verify.c): saved
 * before the original runs and put back before the native version, so both
 * read the same clock. */
void cj_det_clock_save(uint64_t out[3]) { out[0] = t_tsc_n; out[1] = t_sys_n; out[2] = s_tick; }
void cj_det_clock_restore(const uint64_t in[3]) { t_tsc_n = in[0]; t_sys_n = in[1]; s_tick = (uint32_t)in[2]; }

/* The guest main thread is the host main thread (main.c calls the XBE entry
 * directly), recorded by det_ctor. */
static pthread_t s_main_thr;
static int s_main_set;
int cj_det_is_main(void) { return s_main_set && pthread_equal(s_main_thr, pthread_self()); }
static uint64_t s_sleep_pumps, s_sleep_other;

/* Before any guest code: the hooks must be in place for the first rdtsc. */
__attribute__((constructor)) static void det_ctor(void)
{
    const char *e = getenv("CJ_DET"), *r = getenv("CJ_REC"), *rp = getenv("CJ_REC_PLAY");
    s_main_thr = pthread_self(); s_main_set = 1;
    /* A session recording or replay (src/cj_rec.c) is a deterministic run. Decided
     * here, before main: the clock hooks must be in place for the first rdtsc and the
     * helper TIB pool is reserved at load (so CJ_REC must be in the environment, as
     * PLAY-REC.bat sets it, not only in pcj.ini). */
    if ((r && *r == '1') || (rp && *rp)) e = "1";
    if (e && *e && *e != '0') {
        const char *t = getenv("CJ_DET_TIME");     /* CJ_DET_TIME=0: synchronous I/O only */
        s_det = 1;
#ifdef _WIN32
        if (!getenv("RECOMP_HELPER_TIB_POOL")) _putenv("RECOMP_HELPER_TIB_POOL=1");
#else
        setenv("RECOMP_HELPER_TIB_POOL", "1", 0);
#endif   /* toolkit: helper-thread TIBs off the shared heap */
        { const char *m = getenv("CJ_DET_MIX"); s_det_mix = !(m && *m == '0'); }  /* CJ_DET_MIX=0: mixer thread runs free */
        { const char *m = getenv("CJ_DET_MOVIE"); s_det_movie = !(m && *m == '0'); }  /* CJ_DET_MOVIE=0: movie paced by host vblanks/audio */
        { const char *m = getenv("CJ_DET_STACK0"); g_recomp_stack_zero = !(m && *m == '0'); }  /* stack hygiene (toolkit patch 0005); CJ_DET_STACK0=0: off */
        if (!(t && *t == '0')) {
            s_det_time = 1;
            xbox_det_tsc_hook = det_tsc;
            xbox_det_systime_hook = det_systime;
            xbox_det_time = 1;
        }
    }
}

/* CJ_DET_WATCH=<guest VA>: debug aid. A hardware write breakpoint (perf_event,
 * sigtrap) on that guest dword in every thread, armed at the first lookup and
 * inherited by threads created later. Each write prints the thread, value and a
 * host backtrace. */
#ifdef __linux__
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#include <signal.h>
#include <string.h>
#include <dirent.h>
#include <execinfo.h>
static uintptr_t s_watch_host;
static void det_watch_trap(int sig, siginfo_t *si, void *uc)
{
    void *bt[24];
    char buf[96];
    int n = snprintf(buf, sizeof buf, "[WATCH] tid %ld value %08x frame %u\n", (long)syscall(SYS_gettid),
                     *(volatile uint32_t *)s_watch_host, cj_det_frame);
    (void)sig; (void)si; (void)uc;
    write(2, buf, (size_t)n);
    n = backtrace(bt, 24);
    backtrace_symbols_fd(bt, n, 2);
}
static void det_watch_arm(uint32_t va)
{
    struct perf_event_attr a;
    struct sigaction sa;
    DIR *d;
    struct dirent *de;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = det_watch_trap;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGTRAP, &sa, NULL);
    s_watch_host = (uintptr_t)XBOX_PTR(va);
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_BREAKPOINT;
    a.size = sizeof a;
    a.bp_type = HW_BREAKPOINT_W;
    a.bp_addr = s_watch_host;
    a.bp_len = HW_BREAKPOINT_LEN_4;
    a.sample_period = 1;
    a.inherit = 1;
    a.sigtrap = 1;
    a.remove_on_exec = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    if ((d = opendir("/proc/self/task"))) {
        while ((de = readdir(d)))
            if (de->d_name[0] != '.') {
                long fd = syscall(SYS_perf_event_open, &a, atoi(de->d_name), -1, -1, 0);
                fprintf(stderr, "[WATCH] arm %08x on tid %s: fd %ld\n", va, de->d_name, fd);
            }
        closedir(d);
    }
}
#else  /* Windows (MinGW): no perf_event; CJ_DET_WATCH is a Linux debug aid */
static void det_watch_arm(uint32_t va) { fprintf(stderr, "[WATCH] CJ_DET_WATCH %08x: Linux only\n", va); }
#endif

static void det_init(void)
{
    const char *e;
    s_init = 1;
    if ((e = getenv("CJ_DET_WATCH")) && *e) det_watch_arm((uint32_t)strtoul(e, NULL, 16));
    if ((e = getenv("CJ_LOCKSTEP")) && *e) s_lockstep = atoi(e);
    if ((e = getenv("CJ_STOP_FRAME")) && *e) s_stop = (uint32_t)strtoul(e, NULL, 0);
    if ((e = getenv("CJ_DET_HASH")) && *e && *e != '0') s_hash = 1;
    if ((e = getenv("CJ_DET_REPORT")) && *e) s_report = atoi(e);
    if (s_det && !s_lockstep) s_lockstep = 2;
    if (s_lockstep || s_stop || s_hash || s_det)
        fprintf(stderr, "[DET] lockstep=%d stop_frame=%u hash=%d det=%d\n", s_lockstep, s_stop, s_hash, s_det);
}

extern uint32_t g_helper_tib_pool, g_helper_tib_pool_end;
static uint64_t range_hash(uint32_t lo, uint32_t hi);
/* The heap window minus the helper-thread TIB pool (its contents follow host
 * helper threads: APU interrupt, OHCI, timer DPCs). */
static uint64_t heap_hash(void)
{
    if (g_helper_tib_pool && g_helper_tib_pool >= HEAP_LO && g_helper_tib_pool_end <= HEAP_HI)
        return range_hash(HEAP_LO, g_helper_tib_pool) * 31u + range_hash((g_helper_tib_pool_end + 7u) & ~7u, HEAP_HI);
    return range_hash(HEAP_LO, HEAP_HI);
}

static uint64_t range_hash(uint32_t lo, uint32_t hi)
{
    const uint64_t *p = (const uint64_t *)XBOX_PTR(lo), *e = (const uint64_t *)XBOX_PTR(hi & ~7u);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (; p < e; p++) h = (h ^ *p) * 0x100000001B3ull + (h >> 29);
    return h;
}

/* CJ_DET_DUMP=<n>[,<n>..]: write the raw .data and heap windows before
 * MainUpdate number n (0 = end of boot) to $CJ_DET_DUMP_DIR/{data,heap}_f<n>.bin
 * for tools/det_cmp.py --dump. */
static void det_dump(void)
{
    const char *e = getenv("CJ_DET_DUMP"), *d = getenv("CJ_DET_DUMP_DIR");
    char want[16], path[512];
    if (!e || !d) return;
    snprintf(want, sizeof want, ",%u,", cj_det_frame);
    snprintf(path, sizeof path, ",%s,", e);
    if (!strstr(path, want)) return;
    snprintf(path, sizeof path, "%s/data_f%u.bin", d, cj_det_frame);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite((void *)XBOX_PTR(DATA_LO), 1, DATA_HI - DATA_LO, f); fclose(f); }
    snprintf(path, sizeof path, "%s/heap_f%u.bin", d, cj_det_frame);
    if ((f = fopen(path, "wb"))) { fwrite((void *)XBOX_PTR(HEAP_LO), 1, HEAP_HI - HEAP_LO, f); fclose(f); }
    if (getenv("CJ_DET_DUMP_CONT")) {   /* contiguous (D3D/FE) memory 0x80000000..0x84000000 */
        snprintf(path, sizeof path, "%s/cont_f%u.bin", d, cj_det_frame);
        if ((f = fopen(path, "wb"))) { fwrite((void *)XBOX_PTR(0x80000000u), 1, 0x04000000u, f); fclose(f); }
    }
    if (g_helper_tib_pool) fprintf(stderr, "[DET] helper TIB pool %08x..%08x\n", g_helper_tib_pool, g_helper_tib_pool_end);
}

/* CJ_DET_REF=<baseline log>: tools/promote.py early exit. The per-frame [DET] hashes of a
 * baseline run are loaded, and the first frame whose .data hash (any frame) or heap hash
 * (not frame 1: the documented APU transient word) differs ends the run with exit status 3
 * after "[DET] DIVERGE f=N region=data|heap" and a final swap report. */
#define REF_MAX 4096
static uint64_t s_ref_data[REF_MAX], s_ref_heap[REF_MAX]; static int s_ref_have[REF_MAX], s_ref_loaded;
static void det_ref_load(void)
{
    const char *p = getenv("CJ_DET_REF"); char l[256]; FILE *f;
    s_ref_loaded = 1;
    if (!p || !*p || !(f = fopen(p, "r"))) return;
    while (fgets(l, sizeof l, f)) {
        unsigned fr; unsigned long long d, h;
        if (sscanf(l, "[DET] f=%u data=%llx heap=%llx", &fr, &d, &h) == 3 && fr < REF_MAX)
        { s_ref_data[fr] = d; s_ref_heap[fr] = h; s_ref_have[fr] = 1; }
    }
    fclose(f);
}
static void det_ref_check(unsigned fr, uint64_t d, uint64_t h)
{
    const char *reg = NULL;
    if (!s_ref_loaded) det_ref_load();
    if (fr >= REF_MAX || !s_ref_have[fr]) return;
    if (d != s_ref_data[fr]) reg = "data"; else if (fr != 1 && h != s_ref_heap[fr]) reg = "heap";
    if (!reg) return;
    fprintf(stderr, "[DET] DIVERGE f=%u region=%s\n", fr, reg);
    if (cj_swap_report) cj_swap_report("diverge");
    fflush(stderr);
    _exit(3);
}

static void det_mix_step(void);
extern uint64_t cj_det_mixes;
static void det_main_update(void)
{
    uint32_t disp = MEM32(DISPLAY_PTR_VA);
    char tag[32];
    if (!s_init) det_init();
    if (s_lockstep && disp) {
        MEM32(disp + 0x980) = (uint32_t)s_lockstep;
        MEM32(disp + 0x984) += (uint32_t)s_lockstep;
    }
    if (s_det) {
        det_tick_set((uint32_t)(det_frame_vblanks() * 1000u / 60u));
        cj_det_file_pump();
        if (s_det_mix) det_mix_step();
    }
    det_dump();
    m_MainUpdate_EXApp__UAECXZ_00217730();
    cj_det_frame++;
    {   /* session recording / replay: heap hash every 60 frames, real-time pacing (src/cj_rec.c) */
        extern int cj_rec_mode(void);
        extern void cj_rec_frame(unsigned frame, uint64_t heap);
        if (cj_rec_mode()) cj_rec_frame(cj_det_frame, cj_det_frame % 60 == 0 ? heap_hash() : 0);
    }
    if (s_hash) {
        uint64_t dh = range_hash(DATA_LO, DATA_HI), hh = heap_hash();
        static int s_cont = -1;
        if (s_cont < 0) s_cont = getenv("CJ_DET_HASH_CONT") && *getenv("CJ_DET_HASH_CONT") != '0';
        if (s_cont)   /* contiguous memory (D3D/FE: vertex buffers, textures, push buffer): image differences
                       * whose cause is outside .data and the heap window (docs/promote.md, failure class 4) */
            fprintf(stderr, "[DET] f=%u data=%016llx heap=%016llx cont=%016llx\n", cj_det_frame,
                    (unsigned long long)dh, (unsigned long long)hh, (unsigned long long)range_hash(0x80000000u, 0x84000000u));
        else
        fprintf(stderr, "[DET] f=%u data=%016llx heap=%016llx\n", cj_det_frame,
                (unsigned long long)dh, (unsigned long long)hh);
        det_ref_check(cj_det_frame, dh, hh);
        if (cj_swap_report && s_report > 0 && cj_det_frame % (uint32_t)s_report == 0) {
            snprintf(tag, sizeof tag, "f%u", cj_det_frame);
            cj_swap_report(tag);
        }
    }
    if (s_stop && cj_det_frame >= s_stop) {
        fprintf(stderr, "[DET] stop at frame %u (file pumps %llu, Update calls %llu, off-main wake-ups %llu, sleep pumps %llu, main %d, mixes %llu)\n", cj_det_frame,
                (unsigned long long)cj_det_pumps, (unsigned long long)cj_det_updates,
                (unsigned long long)cj_det_offthread, (unsigned long long)s_sleep_pumps, cj_det_is_main(), (unsigned long long)cj_det_mixes);
        if (cj_swap_report) cj_swap_report("final");
        fflush(stderr);
        sleep(3);           /* the GPU thread dumps the last flips */
        fflush(stderr);
        _exit(0);
    }
}

/* KeDelayExecutionThread (kernel thunk slot 75, 0xFE00012C) under CJ_DET:
 * the main thread pumps the file manager first, then sleeps as before. */
#define KE_DELAY_THUNK 0xFE00012Cu
static void det_delay(void)
{
    recomp_func_t k;
    if (cj_det_is_main()) {
        uint32_t b0 = g_ebx, s0 = g_esi, d0 = g_edi;
        cj_det_file_pump();
        if ((g_ebx != b0 || g_esi != s0 || g_edi != d0) && getenv("CJ_DET_DEBUG"))
            fprintf(stderr, "[DET] sleep pump clobbered ebx %08x->%08x esi %08x->%08x edi %08x->%08x\n",
                    b0, g_ebx, s0, g_esi, d0, g_edi);
        if ((++s_sleep_pumps & (s_sleep_pumps - 1)) == 0 && getenv("CJ_DET_DEBUG"))
            fprintf(stderr, "[DET] sleep pump %llu: Update calls %llu, frame %u\n", (unsigned long long)s_sleep_pumps,
                    (unsigned long long)cj_det_updates, cj_det_frame);
    } else if (getenv("CJ_DET_DEBUG") && (++s_sleep_other & 0xFFFF) == 1) fprintf(stderr, "[DET] off-main sleep\n");
    /* recomp_lookup_kernel stores the slot in a per-thread variable that the
     * dispatcher reads at call time. The pump above makes kernel calls of its
     * own (RtlEnterCriticalSection, ReadFile...), so look the slot up again
     * right before the call: a cached dispatcher would run the last kernel
     * function the pump used (a 1-argument one pops 8 bytes instead of 16 and
     * never sleeps). That was the CJ_DET boot hang. */
    k = recomp_lookup_kernel(KE_DELAY_THUNK);
    {
        uint32_t s0 = g_esi, r0 = MEM32(g_esp), e0 = g_esp;
        k();
        if (cj_det_is_main() && s_sleep_pumps < 4 && getenv("CJ_DET_DEBUG"))
            fprintf(stderr, "[DET] main sleep: ret %08x esp %08x->%08x esi %08x->%08x\n", r0, e0, g_esp, s0, g_esi);
    }
}

/* xbMixerThread (0x002B2520) under CJ_DET: parked, and its loop body runs on
 * the main thread before every MainUpdate (det_mix_step) with a play cursor
 * that advances a fixed amount per game frame instead of reading the
 * DirectSound buffer position. The original loop, every 4 ms:
 *   lock gpMixer+0x1C; play = GetCurrentPosition(speaker buffer)/2;
 *   played += (play - lastplay) mod 0x1000; lastplay = play;
 *   ahead = mixed - played; if (ahead < A) { n = ahead > B ? B : A;
 *   Mix(n) in up to two pieces around the 0x1000-sample ring at gpMixer+0x14;
 *   mixed += n; } unlock; Sleep(4);
 * The audio that reaches the host is not meaningful under CJ_DET; the game
 * state that depends on mixing (voice and stream progress) advances by the
 * same amount every frame. */
#define MIX_PTR      0x004DE328u   /* gpMixer */
#define MIX_MIXED    0x004DE350u   /* s64 samples mixed */
#define MIX_PLAYED   0x004DE358u   /* s64 samples played */
#define MIX_LASTPLAY 0x004DE360u
#define MIX_A        0x00472424u
#define MIX_B        0x00472420u
#define MIX_FN       0x002C71E0u   /* AudioMixer::Mix(unsigned), __thiscall, ret 4 */
#define MIX_ITERS    8             /* original loop iterations per 33 ms frame */
#define MIX_STEP     200           /* samples per iteration: 48 kHz x 33.3 ms / 8 */
static void det_park_mixer(void)
{
    fprintf(stderr, "[DET] xbMixerThread parked (mixing runs on the main thread)\n");
    for (;;) sleep(1000);
}
static void det_mix_call(uint32_t n)
{
    static recomp_func_t mix;
    uint32_t esp0 = g_esp;
    if (!mix) mix = recomp_lookup(MIX_FN);
    PUSH32(g_esp, n);
    PUSH32(g_esp, 0x0000C0DEu);
    g_ecx = MEM32(MIX_PTR);
    mix();
    g_esp = esp0;
}
uint64_t cj_det_mixes;
static void det_mix_step(void)
{
    uint32_t mixer = MEM32(MIX_PTR), eax0 = g_eax, ecx0 = g_ecx, edx0 = g_edx;
    static int s_started;
    if (!mixer) return;
    if (!s_started) { MEM32(mixer + 0x14) = 0; s_started = 1; }   /* the thread's prologue */
    for (int it = 0; it < MIX_ITERS; it++) {
        int64_t played = (int64_t)((uint64_t)MEM32(MIX_PLAYED) | (uint64_t)MEM32(MIX_PLAYED + 4) << 32);
        int64_t mixed = (int64_t)((uint64_t)MEM32(MIX_MIXED) | (uint64_t)MEM32(MIX_MIXED + 4) << 32);
        int32_t a = (int32_t)MEM32(MIX_A), b = (int32_t)MEM32(MIX_B), wpos, n, n2;
        played += MIX_STEP;
        MEM32(MIX_LASTPLAY) = (MEM32(MIX_LASTPLAY) + MIX_STEP) & 0xFFFu;
        MEM32(MIX_PLAYED) = (uint32_t)played; MEM32(MIX_PLAYED + 4) = (uint32_t)((uint64_t)played >> 32);
        if (mixed - played >= a) continue;
        n = (mixed - played > b) ? b : a;
        wpos = (int32_t)MEM32(mixer + 0x14) / 2;
        n2 = n + wpos > 0x1000 ? n + wpos - 0x1000 : 0;
        if (n - n2 > 0) det_mix_call((uint32_t)(n - n2));
        if (n2 > 0) det_mix_call((uint32_t)n2);
        mixed += n;
        MEM32(MIX_MIXED) = (uint32_t)mixed; MEM32(MIX_MIXED + 4) = (uint32_t)((uint64_t)mixed >> 32);
        cj_det_mixes++;
    }
    g_eax = eax0; g_ecx = ecx0; g_edx = edx0;
}

/* xbFileThread (0x00284E70) under CJ_DET: the main thread does its work. */
static void det_park_thread(void)
{
    fprintf(stderr, "[DET] xbFileThread parked\n");
    for (;;) sleep(1000);
}

static void det_vblank_cb(void)
{
    /* cdecl, returns with `ret`: pop the return address the caller pushed. */
    g_esp += 4;
}

/* Called first by recomp_lookup_manual. */
det_fn cj_det_lookup(uint32_t va)
{
    if (!s_init) det_init();
    if (va == 0x00217730u && (s_lockstep || s_stop || s_hash || s_det)) return det_main_update;
    if (va == 0x00284E70u && s_det) return det_park_thread;
    if (va == 0x002B2520u && s_det && s_det_mix) return det_park_mixer;
    if (va == KE_DELAY_THUNK && s_det) return det_delay;
    if (va == 0x002238E0u && s_lockstep) return det_vblank_cb;
    return 0;
}
