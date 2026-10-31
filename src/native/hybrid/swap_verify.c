/* Dual-execution verifier for hybrid swaps and mods (CJ_SWAP_VERIFY).
 *
 * For a selected function, each of its first CJ_SWAP_VERIFY_N calls (default 8):
 * snapshot all guest RAM and the guest registers, run the recompiled original
 * TWICE from that snapshot (A, B), then the native (decompiled or mod) version
 * (C), and compare every byte of guest RAM, the callee-saved registers, esp and
 * the return value of C with B. Bytes where A and B already differ are not
 * judged: the interrupt helper threads (vblank, APU, timer DPCs) write a few
 * guest words at any time, and a function that reads them is not reproducible
 * there even against itself; their count is printed (unjudged=N). Differences
 * are printed as address ranges ([VERIFY] lines). The native result is kept.
 *
 * CJ_SWAP_VERIFY=<va,va,...> (8 hex digits, as in the [SWAP] lines) or "all".
 * CJ_SWAP_VERIFY_N=N (8): calls checked per function; CJ_SWAP_VERIFY_EVERY=K: also every Kth call up to K*N.
 * CJ_SWAP_VERIFY_FROM=F (1): calls before MainUpdate frame F are not checked (boot threads are live).
 * The guest stack below the caller's esp is ignored (callee locals: the native
 * version keeps them on the host stack). Other guest threads (file, mixer) and
 * the GPU executor can touch RAM between the two runs; such noise shows as
 * ranges far from the function's data and is reported like any other.
 * CJ_SWAP_VERIFY_KEEP=orig: after the comparison the ORIGINAL's outcome is put
 * back (RAM and registers), so a wrong native version cannot derail the run:
 * every selected function is then judged on every one of its first N calls
 * of a whole replay (tools/verify.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "recomp_types.h"

#define CJV_RAM 0x04000000u
extern void *xbox_GetMemoryOffset(void);
extern uint32_t cj_det_frame;   /* src/determinism.c */
extern uint32_t g_xbox_stack_base;   /* toolkit: stack region base (0x00780000 here) */
void cj_det_clock_save(uint64_t out[3]); void cj_det_clock_restore(const uint64_t in[3]);
int cj_in_play(void);   /* src/quicksave.c: level loaded, player in control, no cutscene */

static const char *s_sel; static int s_init; static uint64_t s_max = 8, s_every;
static uint64_t s_from = 1;   /* CJ_SWAP_VERIFY_FROM: first frame verified (boot has other threads live and unparked) */
static uint8_t *s_pre, *s_gen, *s_genA;   /* before, original run B (judged against), original run A */
typedef struct { uint32_t eax, ecx, edx, esp, ebx, esi, edi, ebp; int fp_top; double fp[8]; } cjv_regs;
static cjv_regs s_rpre, s_rgen, s_rgenA; static uint64_t s_cpre[3], s_cgen[3];
/* Host file positions: a function that reads from the disc (synchronous under
 * CJ_DET) would read on past its own first run. Every open regular file's
 * offset is saved before run A and put back before B and C (Linux: /proc/self/fd). */
#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#define FDS_MAX 256
typedef struct { int n; int fd[FDS_MAX]; off_t off[FDS_MAX]; } fdpos;
static fdpos s_fpre, s_fgen;
static void fds_save(fdpos *p)
{
    DIR *d = opendir("/proc/self/fd"); struct dirent *e; p->n = 0;
    if (!d) return;
    while ((e = readdir(d)) && p->n < FDS_MAX) {
        int fd = atoi(e->d_name); struct stat st;
        if (fd <= 2 || e->d_name[0] == '.' || fd == dirfd(d) || fstat(fd, &st) || !S_ISREG(st.st_mode)) continue;
        off_t o = lseek(fd, 0, SEEK_CUR);
        if (o < 0) continue;
        p->fd[p->n] = fd; p->off[p->n] = o; p->n++;
    }
    closedir(d);
}
static void fds_restore(const fdpos *p) { for (int i = 0; i < p->n; i++) lseek(p->fd[i], p->off[i], SEEK_SET); }
#else
typedef int fdpos; static fdpos s_fpre, s_fgen;
static void fds_save(fdpos *p) { (void)p; }
static void fds_restore(const fdpos *p) { (void)p; }
#endif
/* Kernel calls whose effect lives in host memory (allocations, handles,
 * threads, sections): a run that made one cannot be undone, so the function's
 * first run is kept as the real one and the comparison is skipped. */
extern const unsigned long long *xbox_KernelOrdinalCalls(void);   /* toolkit patch 0025 */
static const int k_side_ords[] = {14, 15, 17, 119, 165, 166, 167, 169, 171, 172, 177, 183, 184, 187, 189, 190, 192, 193, 197, 199,
                                  202, 203, 224, 231, 246, 247, 250, 251, 255, 258, 327, 328, 0};
static unsigned long long s_ord_pre[40]; static uint64_t s_skipped;
static void ords_save(void) { const unsigned long long *c = xbox_KernelOrdinalCalls(); for (int i = 0; k_side_ords[i]; i++) s_ord_pre[i] = c[k_side_ords[i]]; }
static int ords_changed(void) { const unsigned long long *c = xbox_KernelOrdinalCalls(); for (int i = 0; k_side_ords[i]; i++) if (c[k_side_ords[i]] != s_ord_pre[i]) return k_side_ords[i]; return 0; }
static uint64_t s_checked, s_clean; static int s_keep_orig;
static int s_pass;   /* 0 none, 1 original run A, 2 original run B, 3 native run C */
int cjv_pass(void) { return s_pass; }
void cj_rec_verify_pass(int pass) __attribute__((weak));
static void set_pass(int p) { s_pass = p; if (cj_rec_verify_pass) cj_rec_verify_pass(p); }
/* CJ_SWAP_VERIFY_SELF=1: run the original twice instead (noise baseline:
 * whatever differs there comes from other threads, not from the function). */
int cjv_self(void) { static int v = -1; if (v < 0) v = getenv("CJ_SWAP_VERIFY_SELF") != NULL; return v; }

static void get(cjv_regs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->esp = g_esp; r->ebx = g_ebx; r->esi = g_esi;
    r->edi = g_edi; r->ebp = g_ebp; r->fp_top = g_fp_top; memcpy(r->fp, g_fp_stack, sizeof r->fp);
}
static void put(const cjv_regs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_esp = r->esp; g_ebx = r->ebx; g_esi = r->esi;
    g_edi = r->edi; g_ebp = r->ebp; g_fp_top = r->fp_top; memcpy(g_fp_stack, r->fp, sizeof r->fp);
}

int cjv_want(const char *va, uint64_t n)
{
    if (!s_init) {
        s_init = 1;
        s_sel = getenv("CJ_SWAP_VERIFY");
        if (getenv("CJ_SWAP_VERIFY_N")) s_max = strtoull(getenv("CJ_SWAP_VERIFY_N"), NULL, 0);
        if (getenv("CJ_SWAP_VERIFY_EVERY")) s_every = strtoull(getenv("CJ_SWAP_VERIFY_EVERY"), NULL, 0);
        if (getenv("CJ_SWAP_VERIFY_FROM")) s_from = strtoull(getenv("CJ_SWAP_VERIFY_FROM"), NULL, 0);
        s_keep_orig = getenv("CJ_SWAP_VERIFY_KEEP") && strcmp(getenv("CJ_SWAP_VERIFY_KEEP"), "orig") == 0;
        if (s_sel) {
            s_pre = malloc(CJV_RAM); s_gen = malloc(CJV_RAM); s_genA = malloc(CJV_RAM);
            if (!s_pre || !s_gen || !s_genA) s_sel = NULL;
            fprintf(stderr, "[VERIFY] dual execution for %s, first %llu calls each\n", s_sel ? s_sel : "(off: no memory)",
                    (unsigned long long)s_max);
        }
    }
    if (!s_sel || cj_det_frame < s_from || !cj_in_play()) return 0;
    if (!(strcmp(s_sel, "all") == 0 || strstr(s_sel, va) != NULL)) return 0;
    /* per function, counting only calls made in play: the first N, and with CJ_SWAP_VERIFY_EVERY=K every Kth up to K*N
     * (hot functions: samples across the run). The wrapper's own n counts boot and menu calls too. */
    {
        static struct { const char *va; uint64_t n; } tab[4096]; unsigned h = (unsigned)((uintptr_t)va >> 4) & 4095;
        while (tab[h].va && tab[h].va != va) h = (h + 1) & 4095;
        tab[h].va = va; uint64_t k = ++tab[h].n; (void)n;
        if (k > s_max && !(s_every && k % s_every == 0 && k <= s_every * s_max)) return 0;
        fprintf(stderr, "[VERIFY] begin %s #%llu f=%u\n", va, (unsigned long long)n, cj_det_frame);
        return 1;
    }
}

/* Words the interrupt helper threads write at any time (D3D's vblank state, a
 * DirectSound counter, an interrupt thread's stack): measured once by idling
 * with no guest code on this thread, padded, and never judged. */
#define ASYNC_MAX 64
#define ASYNC_PAD 16u
static struct { uint32_t a, n; } s_async[ASYNC_MAX]; static unsigned s_nasync; static int s_async_done;
static int is_async(uint32_t x)
{
    if (x >= 0x00F80000u && x < 0x00F80A00u) return 1;   /* helper threads' TIB pool (docs/determinism.md): theirs, not the function's */
    /* the helper threads' stack slices (toolkit xbox_memory_layout.h: 16 x 256 KB from the stack region's base; the
     * main stack is at the region's top) */
    if (x >= g_xbox_stack_base && x < g_xbox_stack_base + 16u * 256u * 1024u) return 1;
    for (unsigned i = 0; i < s_nasync; i++) if (x + ASYNC_PAD >= s_async[i].a && x < s_async[i].a + s_async[i].n + ASYNC_PAD) return 1;
    return 0;
}
static void async_probe(void)
{
    const uint8_t *m = (const uint8_t *)xbox_GetMemoryOffset(); char buf[1024] = ""; int bl = 0;
    for (int pass = 0; pass < 6; pass++) {
        memcpy(s_genA, m, CJV_RAM);
        struct timespec ts = {0, 25000000}; nanosleep(&ts, NULL);
        for (uint32_t a = 0; a < CJV_RAM; a++) {
            if (m[a] == s_genA[a]) continue;
            uint32_t b = a; while (b < CJV_RAM && m[b] != s_genA[b]) b++;
            if (!is_async(a) && s_nasync < ASYNC_MAX) { s_async[s_nasync].a = a; s_async[s_nasync].n = b - a; s_nasync++; }
            a = b;
        }
    }
    for (unsigned i = 0; i < s_nasync && bl < (int)sizeof buf - 24; i++) bl += snprintf(buf + bl, sizeof buf - bl, " %08X+%u", s_async[i].a, s_async[i].n);
    fprintf(stderr, "[VERIFY] async writers (not judged, +-%u bytes): %u ranges;%s; helper stacks %08X..%08X, main esp %08X\n", ASYNC_PAD, s_nasync, buf,
            g_xbox_stack_base, g_xbox_stack_base + 16u * 256u * 1024u, g_esp);
    s_async_done = 1;
}

/* Put a snapshot back, except the other threads' own memory (TIB pool, helper
 * stack slices, the async words): an interrupt thread may be running on it at
 * this moment, and rolling its stack back under it crashes the run. */
static void restore(const uint8_t *src)
{
    uint8_t *m = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t a = 0;
    static int full = -1; if (full < 0) full = getenv("CJ_SWAP_VERIFY_FULLRESTORE") != NULL;
    if (full) { memcpy(m, src, CJV_RAM); return; }
    while (a < CJV_RAM) {
        if (is_async(a)) { a += 4; continue; }
        uint32_t b = a + 4;
        while (b < CJV_RAM && !is_async(b)) b += 4;
        memcpy(m + a, src + a, b - a);
        a = b;
    }
}

void cjv_begin(void)
{
    if (!s_async_done) async_probe();
    get(&s_rpre); cj_det_clock_save(s_cpre); fds_save(&s_fpre); ords_save();
    memcpy(s_pre, xbox_GetMemoryOffset(), CJV_RAM);
    set_pass(1);
}

/* after the first original run (A): keep its outcome, put the pre state back.
 * Returns 0 when the comparison must be skipped (A made a kernel call with
 * host-side effects): A then stands as the function's one real run. */
static int s_skip_ord;
int cjv_mid(void)
{
    if ((s_skip_ord = ords_changed()) != 0) { set_pass(0); return 0; }
    get(&s_rgenA);
    memcpy(s_genA, xbox_GetMemoryOffset(), CJV_RAM);
    restore(s_pre);
    put(&s_rpre); cj_det_clock_restore(s_cpre); fds_restore(&s_fpre);
    set_pass(2);
    return 1;
}
/* the comparison was skipped after run A */
void cjv_skip(const char *va, const char *name, uint64_t n)
{
    s_skipped++;
    fprintf(stderr, "[VERIFY] %s %s #%llu f=%u SKIPPED (kernel ordinal %d: host-side effect)\n", va, name, (unsigned long long)n, cj_det_frame, s_skip_ord);
}
/* after the second original run (B): keep its outcome (the reference), put the pre state back for the native run */
void cjv_mid2(void)
{
    get(&s_rgen); cj_det_clock_save(s_cgen); fds_save(&s_fgen);
    memcpy(s_gen, xbox_GetMemoryOffset(), CJV_RAM);
    restore(s_pre);
    put(&s_rpre); cj_det_clock_restore(s_cpre); fds_restore(&s_fpre);
    set_pass(3);
}

void cjv_end(const char *va, const char *name, uint64_t n, int has_ret)
{
    set_pass(0);
    const uint8_t *m = (const uint8_t *)xbox_GetMemoryOffset();
    cjv_regs r; get(&r);
    uint32_t lo = s_rpre.esp > 0x10000u ? s_rpre.esp - 0x10000u : 0, hi = s_rpre.esp;  /* callee locals */
    unsigned nr = 0, nnear = 0; uint64_t bytes = 0, unj = 0;
    uint32_t self = s_rpre.ecx & 0x03FFFFFFu;
    char buf[2048]; int bl = 0;
#define JUDGED(x) ((x) >= CJV_RAM || (s_genA[x] == s_gen[x] && !is_async(x)))
    static int dbg = -1; if (dbg < 0) dbg = getenv("CJ_SWAP_VERIFY_DEBUG") != NULL;
    if (dbg) {   /* where the two original runs disagree */
        char db[1024] = ""; int dl = 0; unsigned k = 0;
        for (uint32_t a = 0; a < CJV_RAM; a++) {
            if (s_genA[a] == s_gen[a] || is_async(a)) continue;
            uint32_t b = a; while (b < CJV_RAM && s_genA[b] != s_gen[b]) b++;
            if (k < 24 && dl < (int)sizeof db - 48) dl += snprintf(db + dl, sizeof db - dl, " %08X+%u(A %02X%02X%02X%02X B %02X%02X%02X%02X)", a, b - a,
                                                                 s_genA[a], s_genA[a+1], s_genA[a+2], s_genA[a+3], s_gen[a], s_gen[a+1], s_gen[a+2], s_gen[a+3]);
            k++; a = b;
        }
        fprintf(stderr, "[VERIFY]   A/B differ: %u ranges;%s; eax A %08X B %08X esp A %08X B %08X\n", k, db, s_rgenA.eax, s_rgen.eax, s_rgenA.esp, s_rgen.esp);
    }
    for (uint32_t a = 0; a < CJV_RAM; ) {
        if (!JUDGED(a)) { if (!is_async(a)) unj++; a++; continue; }
        if (m[a] == s_gen[a] || (a >= lo && a < hi)) { a++; continue; }
        uint32_t b = a;
        while (b < CJV_RAM && JUDGED(b) && (m[b] != s_gen[b] || (b + 1 < CJV_RAM && m[b + 1] != s_gen[b + 1])) && !(b >= lo && b < hi)) b++;
        bytes += b - a;
        if (self && a >= self && a < self + 0x400u) nnear++;
        if (nr < 8 && bl < (int)sizeof buf - 64)
            bl += snprintf(buf + bl, sizeof buf - bl, " %08X+%u(orig %02X%02X%02X%02X mine %02X%02X%02X%02X)", a, b - a,
                           s_gen[a], s_gen[a + 1], s_gen[a + 2], s_gen[a + 3], m[a], m[a + 1], m[a + 2], m[a + 3]);
        else if (nr < 64 && bl < (int)sizeof buf - 24)
            bl += snprintf(buf + bl, sizeof buf - bl, " %08X+%u", a, b - a);
        nr++;
        a = b;
    }
    /* ebp is not compared: recompiled bodies keep it in a local (the swap
     * wrapper's own check covers ebx/esi/edi and esp, as here). */
    int regbad = r.esp != s_rgen.esp || r.ebx != s_rgen.ebx || r.esi != s_rgen.esi || r.edi != s_rgen.edi;
    int retunj = has_ret && s_rgenA.eax != s_rgen.eax;   /* the original itself returns different values: not judged */
    int regunj = s_rgenA.esp != s_rgen.esp || s_rgenA.ebx != s_rgen.ebx || s_rgenA.esi != s_rgen.esi || s_rgenA.edi != s_rgen.edi;
    if (retunj || regunj) {
        /* host-side state the function depends on (kernel allocations, handles): the original is not reproducible
         * against itself here, so nothing about the native version can be concluded from this call */
        fprintf(stderr, "[VERIFY] %s %s #%llu f=%u UNREPRODUCIBLE (original runs A and B differ in %s%s)\n", va, name,
                (unsigned long long)n, cj_det_frame, retunj ? "return value " : "", regunj ? "callee-saved/esp" : "");
        s_checked++;
        return;
    }
    int retbad = has_ret && !retunj && r.eax != s_rgen.eax;
    s_checked++;
    if (s_keep_orig) { restore(s_gen); put(&s_rgen); cj_det_clock_restore(s_cgen); fds_restore(&s_fgen); }
    if (!nr && !regbad && !retbad) {
        s_clean++;
        fprintf(stderr, "[VERIFY] %s %s #%llu f=%u identical unjudged=%llu%s this=%08X (%llu/%llu clean so far)\n", va, name, (unsigned long long)n,
                cj_det_frame, (unsigned long long)unj, retunj ? " ret" : "", self, (unsigned long long)s_clean, (unsigned long long)s_checked);
        return;
    }
    fprintf(stderr, "[VERIFY] %s %s #%llu f=%u DIFFERS: %u RAM ranges (%u inside this+0x400), %llu bytes, unjudged=%llu%s esp=%08X;%s%s%s\n", va, name, (unsigned long long)n, cj_det_frame, nr, nnear,
            (unsigned long long)bytes, (unsigned long long)unj, retunj ? " ret" : "", s_rpre.esp, nr ? buf : "",
            regbad ? " callee-saved/esp differ" : "", retbad ? " return value differs" : "");
    if (retbad) fprintf(stderr, "[VERIFY]   eax orig %08X mine %08X\n", s_rgen.eax, r.eax);
    if (regbad)
        fprintf(stderr, "[VERIFY]   regs orig/mine: esp %08X/%08X ebx %08X/%08X esi %08X/%08X edi %08X/%08X ebp %08X/%08X\n",
                s_rgen.esp, r.esp, s_rgen.ebx, r.ebx, s_rgen.esi, r.esi, s_rgen.edi, r.edi, s_rgen.ebp, r.ebp);
}
