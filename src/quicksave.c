/* Quick save / quick load (save states).
 *
 * Safe point: the entry of EXBaseApp::Update (0x1B97B0), reached once per
 * frame from the game's main loop through its vtable. recomp_lookup_manual
 * hands out qs_update_entry for that address, so a save or load runs with the
 * game thread at a function boundary at the top of the frame: every guest
 * register is in the TLS globals (the toolkit keeps none in host locals except
 * the caller's frame pointer, which points into the guest stack and is the
 * same every frame), the previous frame's pushbuffer has been walked, and no
 * per-frame work is in flight.
 *
 * A state is: the game thread's guest registers (integer, x87, SSE), guest RAM
 * (64 MB), the GPU MMIO page, the pushbuffer walk, the GPU register shadow,
 * the vertex program and the kernel allocator tables (xbox_snapshot.h), and
 * the guest clock. A load puts all of it back, except the live worker-thread
 * stacks (the audio mixer and file threads keep running their own code
 * through the copy) and shifts the guest clock so no time appears to have
 * passed since the save.
 *
 * Keys (gl_backend.c): F5 quick save, F8 quick load (F9 is the gamelog burst). One slot, in memory for
 * now. CJ_QS_TEST="flip:save,flip:load,..." drives it for tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "recomp_types.h"
#include "../xboxrecomp/src/kernel/xbox_snapshot.h"
#include "quicksave.h"
#include "pcsettings.h"
extern char *cjmod_pc_block(void);

extern void *xbox_GetMemoryOffset(void);
extern uint64_t (*xbox_det_tsc_hook)(void);
extern uint64_t xbox_ReadTimeStampCounter(void);
extern uint64_t xbox_HostNowNs(void);
extern void sub_001B97B0(void);
extern unsigned int cj_gl_flips(void);

#define QS_RAM 0x04000000u
#define QS_MAX_REGIONS 64

typedef struct { int level, cut, control, depth, have_player; float x, y, z, health, energy; } QsGame;

typedef struct {
    uint32_t eax, ecx, edx, esp, ebx, esi, edi, ebp, seh_ebp, mxcsr;
    int fp_top;
    double fp[8];
    RecompXmm xmm[8];
} QsRegs;

typedef struct {
    int valid;
    unsigned flip;
    uint64_t tsc;
    QsRegs r;
    uint8_t *ram;
    XboxSnapRegion reg[QS_MAX_REGIONS];
    uint8_t *data[QS_MAX_REGIONS];
    int nreg;
    QsGame game;
    uint32_t map_hash, nwork, wlo[8], whi[8];
    uint32_t pc_blk;    /* guest VA of the PC Settings block at save time (0: none) */
    int nfiles;
    XboxSnapFile files[64];   /* open files' read positions */
    int slot_no;              /* which quick save slot this snapshot is */
} QsState;

static QsState s_slot;
static volatile int s_request;
static int s_request_now;
static int s_slot_disk;                /* the in-memory slot came from read_file */
static const char *s_read_path;        /* replay: read_file takes this file instead of the slot's */
static const char *s_write_path;       /* record: write_file writes here instead */          /* the request the safe point acts on this update */
static int s_verify, s_force = -1, s_nodisk = -1;          /* 1 save, 2 load (set from the key handler) */
static unsigned s_saves, s_loads;
static char s_msg[96];
static unsigned s_msg_until, s_msg_gen;

/* ---- guest clock: continuous across a load ------------------------------ */
static int s_clock_on;
static int64_t s_tsc_off;               /* subtracted from the host-derived count */
static uint64_t host_tsc(void)
{
    uint64_t ns = xbox_HostNowNs();
    return ns / 1000000000ull * 733333333ull + (ns % 1000000000ull) * 733333333ull / 1000000000ull;
}
static uint64_t qs_tsc(void) { return host_tsc() - (uint64_t)s_tsc_off; }
static void clock_take_over(void)
{
    if (s_clock_on || xbox_det_tsc_hook) return;       /* CJ_DET owns the clock */
    {
        uint64_t now = xbox_ReadTimeStampCounter();
        s_tsc_off = (int64_t)(host_tsc() - now);       /* continue from the kernel's count */
        xbox_det_tsc_hook = qs_tsc;
        s_clock_on = 1;
    }
}

/* ---- registers ---------------------------------------------------------- */
static void regs_get(QsRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->esp = g_esp;
    r->ebx = g_ebx; r->esi = g_esi; r->edi = g_edi; r->ebp = g_ebp;
    r->seh_ebp = g_seh_ebp; r->mxcsr = g_mxcsr;
    r->fp_top = g_fp_top; memcpy(r->fp, g_fp_stack, sizeof r->fp);
    r->xmm[0] = g_xmm0; r->xmm[1] = g_xmm1; r->xmm[2] = g_xmm2; r->xmm[3] = g_xmm3;
    r->xmm[4] = g_xmm4; r->xmm[5] = g_xmm5; r->xmm[6] = g_xmm6; r->xmm[7] = g_xmm7;
}
static void regs_put(const QsRegs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_esp = r->esp;
    g_ebx = r->ebx; g_esi = r->esi; g_edi = r->edi; g_ebp = r->ebp;
    g_seh_ebp = r->seh_ebp; g_mxcsr = r->mxcsr;
    g_fp_top = r->fp_top; memcpy(g_fp_stack, r->fp, sizeof r->fp);
    g_xmm0 = r->xmm[0]; g_xmm1 = r->xmm[1]; g_xmm2 = r->xmm[2]; g_xmm3 = r->xmm[3];
    g_xmm4 = r->xmm[4]; g_xmm5 = r->xmm[5]; g_xmm6 = r->xmm[6]; g_xmm7 = r->xmm[7];
}

/* xbMixerThread (0x2B2520) mixes under the lock at gpMixer+0x1C (a host
 * shadow critical section, so taking it here really blocks the thread).
 * Holding it keeps the mixer out of the voice and heap state while RAM is
 * copied either way. */
extern void __attribute__((stdcall)) xbox_RtlEnterCriticalSection(void *cs);
extern void __attribute__((stdcall)) xbox_RtlLeaveCriticalSection(void *cs);
#define GP_MIXER 0x004DE328u
static uint32_t mixer_lock(void)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t mixer, cs;
    memcpy(&mixer, mem + GP_MIXER, 4);
    if (!mixer || mixer >= QS_RAM - 0x40) return 0;
    cs = mixer + 0x1C;
    xbox_RtlEnterCriticalSection(mem + cs);
    return cs;
}
static void mixer_unlock(uint32_t cs)
{
    if (cs) xbox_RtlLeaveCriticalSection((uint8_t *)xbox_GetMemoryOffset() + cs);
}

extern int cj_gfx_snap_regions(XboxSnapRegion *r, int max);
static int list_regions(XboxSnapRegion *r, int max)
{
    int n = 0;
    n += xbox_layout_snap_regions(r + n, max - n);
    n += nv2a_pb_scan_snap_regions(r + n, max - n);
    n += nv2a_pb_exec_snap_regions(r + n, max - n);
    n += nv2a_vsh_snap_regions(r + n, max - n);
    if (n < max) n += cj_gfx_snap_regions(r + n, max - n);   /* CJ_NULLHW=2 executor position */
    if (n < max) n += mcpx_apu_snap_regions(r + n, max - n);
    return n > max ? max : n;
}

/* ---- game state (addresses as in gamelog.c) ------------------------------ */
/* RAM (0..64 MB) or the contiguous window (0x80000000..0x84000000), where the
 * game's objects live too */
static uint32_t g32(uint32_t va)
{
    uint32_t v = 0;
    if ((va && va < QS_RAM - 4) || (va >= 0x80000000u && va < 0x84000000u - 4))
        memcpy(&v, (uint8_t *)xbox_GetMemoryOffset() + va, 4);
    return v;
}
static float gf(uint32_t va) { uint32_t v = g32(va); float f; memcpy(&f, &v, 4); return f; }
/* CJ_QS_ANYMAP=1: load a save made in another map of the chapter (scene library; the snapshot
   holds the whole machine, the check is only a safety net for players) */
static int any_map(void) { static int a = -1; if (a < 0) { const char *e = getenv("CJ_QS_ANYMAP"); a = e && *e == '1'; } return a; }
static uint8_t g8(uint32_t va) { return (uint8_t)(g32(va & ~3u) >> ((va & 3u) * 8)); }
static void game_state(QsGame *g)
{
    uint32_t app = g32(0x004931E8u), p = g32(0x0048FC98u);
    memset(g, 0, sizeof *g);
    if (app) {
        g->depth = (int)g32(app + 0xC98);
        g->level = (int)(g32(app + 0xCBC) & 0xFF);
        g->control = (int)(g32(app + 0xD00) & 0xFF);
        g->cut = (int)(g32(app + 0xE30) & 0xFF);
    }
    if (p && g->level) {
        g->have_player = 1;
        g->x = gf(p + 0x2C8); g->y = gf(p + 0x2CC); g->z = gf(p + 0x2D0);
        g->health = gf(p + 0xC5C); g->energy = gf(p + 0xC60);
    }
}
/* in play: a level is loaded, the player exists and has control, no cutscene (the dual-execution
 * verifier, swap_verify.c, only judges calls made in play: loaders allocate through the kernel,
 * whose bookkeeping is host-side and cannot be rolled back between its runs) */
int cj_in_play(void) { QsGame g; game_state(&g); return g.level && g.have_player && g.control && !g.cut; }

static void note(const char *m);
/* A status-line message from elsewhere in the port (texture pack toggle, ...). */
void qs_osd_note(const char *m) { note(m); }
static void note(const char *m)
{
    {   /* CJ_OSD_QUIET=1: no status notes (clean screenshots) */
        static int q = -1;
        if (q < 0) q = getenv("CJ_OSD_QUIET") && *getenv("CJ_OSD_QUIET") == '1';
        if (q) return;
    }
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_until = cj_gl_flips() + 120;
    s_msg_gen++;
    fprintf(stderr, "[QS] %s (flip %u)\n", m, cj_gl_flips());
}

static QsGame s_pending_game;
/* Slots: 0 is the one quick save (F5 / F8, both sticks / both bumpers, any chapter); 1..5 are the
 * pause menu's save slots, five per chapter (file names carry the chapter's map hash). s_target is
 * the slot the next save or load uses. */
static int s_target;
int qs_slot(void) { return s_target; }
static const char *qs_dir(void);
static void slot_path(char *path, size_t n, int slot, uint32_t map)
{
    if (slot <= 0) snprintf(path, n, "%s/quicksave.pcjs", qs_dir());
    else snprintf(path, n, "%s/chapter_%08x_slot%d.pcjs", qs_dir(), map, slot);
}
static uint32_t cur_map(void) { return g32(0x0048E874u); }   /* gCurrentMap_GeoHash */
/* "Empty", "just now", "4 min ago": the age of a save slot of this chapter. */
void qs_slot_age(int slot, char *out, size_t n)
{
    char path[600];
    struct stat sb;
    slot_path(path, sizeof path, slot, cur_map());
    if (stat(path, &sb) != 0) { snprintf(out, n, "Empty"); return; }
    {
        long age = (long)(time(NULL) - sb.st_mtime);
        if (age < 60) snprintf(out, n, "just now");
        else if (age < 3600) snprintf(out, n, "%ld min ago", age / 60);
        else if (age < 86400 * 2) snprintf(out, n, "%ld h ago", age / 3600);
        else snprintf(out, n, "%ld days ago", age / 86400);
    }
}
/* Old builds kept five quick save slots (quicksave1..5.pcjs); the first one found becomes the
 * quick save. */
static void migrate_old(void)
{
    static int done;
    char now[600], old[600];
    int k;
    struct stat sb;
    if (done) return;
    done = 1;
    slot_path(now, sizeof now, 0, 0);
    if (stat(now, &sb) == 0) return;
    for (k = 1; k <= 5; k++) {
        snprintf(old, sizeof old, "%s/quicksave%d.pcjs", qs_dir(), k);
        if (stat(old, &sb) == 0) { rename(old, now); fprintf(stderr, "[QS] %s is now the quick save\n", old); return; }
    }
}
static void wait_writer(void);
static void write_async(void);
static int read_file(int slot);

static void do_save(void)
{
    QsState *st = &s_slot;
    int i;
    uint32_t mcs;
    size_t total = QS_RAM;
    wait_writer();     /* the previous save is still going to disk */
    st->valid = 0;
    st->slot_no = s_target;
    if (!st->ram && !(st->ram = malloc(QS_RAM))) { note("quick save failed: out of memory"); return; }
    clock_take_over();
    if (!st->nreg) {
        st->nreg = list_regions(st->reg, QS_MAX_REGIONS);
        for (i = 0; i < st->nreg; i++)
            if (!(st->data[i] = malloc(st->reg[i].n))) { note("quick save failed: out of memory"); st->nreg = 0; return; }
    }
    if (!xbox_nv2a_pause(1, 1)) fprintf(stderr, "[QS] the GPU walk did not park in time\n");
    mcs = mixer_lock();
    if (!xbox_snap_gate(1, 1000)) fprintf(stderr, "[QS] another guest thread kept running\n");
    mcpx_apu_snap_lock(1);
    regs_get(&st->r);
    st->map_hash = g32(0x0048E874u);   /* gCurrentMap_GeoHash */
    st->nfiles = xbox_snap_files(st->files, 64);
    st->pc_blk = cjmod_pc_block() ? (uint32_t)(cjmod_pc_block() - (char *)xbox_GetMemoryOffset()) : 0;
    st->nwork = (uint32_t)xbox_worker_stacks(st->wlo, st->whi, 8);
    memcpy(st->ram, xbox_GetMemoryOffset(), QS_RAM);
    for (i = 0; i < st->nreg; i++) { memcpy(st->data[i], st->reg[i].p, st->reg[i].n); total += st->reg[i].n; }
    st->tsc = xbox_ReadTimeStampCounter();
    mcpx_apu_snap_lock(0);
    xbox_snap_gate(0, 0);
    mixer_unlock(mcs);
    xbox_nv2a_pause(0, 0);
    st->flip = cj_gl_flips();
    st->game = s_pending_game;
    st->valid = 1;
    s_slot_disk = 0;
    s_saves++;
    if (!s_nodisk) write_async();
    {
        char m[96];
        if (st->slot_no <= 0) snprintf(m, sizeof m, "Quick saved");
        else snprintf(m, sizeof m, "Saved to slot %d", st->slot_no);
        note(m);
        fprintf(stderr, "[QS] %.1f MB, %d host regions\n", total / 1048576.0, st->nreg);
    }
}

static int do_load(void)
{
    QsState *st = &s_slot;
    uint32_t lo[8], hi[8], mcs;
    int nw, i;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    wait_writer();
    {
        int cur = s_target;
        if (!st->valid || st->slot_no != cur || (cur > 0 && st->map_hash != cur_map())) {   /* only the last used slot is kept in memory */
            st->valid = 0;
            if (s_nodisk && !s_read_path) { note("No quick save yet (F5 saves)"); return 0; }
            if (!read_file(cur)) return 0;
        }
    }
    if (st->map_hash != g32(0x0048E874u) && !any_map()) { note("That quick save is from another chapter: start that chapter, then load"); return 0; }
    {   /* the threads' stacks must be where they were (they keep running their own code through a load) */
        uint32_t clo[8], chi[8];
        int cn = xbox_worker_stacks(clo, chi, 8), j, same = cn == (int)st->nwork;
        for (j = 0; same && j < cn && j < 8; j++) same = clo[j] == st->wlo[j] && chi[j] == st->whi[j];
        if (!same) { note("This quick save needs a restart of the game to load (thread layout differs)"); return 0; }
    }
    clock_take_over();   /* a save read from disk carries another session's clock */
    if (g_esp != st->r.esp || g_ebp != st->r.ebp)   /* the main loop's frame should be the same every frame */
        fprintf(stderr, "[QS] note: esp %08X ebp %08X now, %08X %08X at the save\n", g_esp, g_ebp, st->r.esp, st->r.ebp);
    if (!xbox_nv2a_pause(1, 1)) fprintf(stderr, "[QS] the GPU walk did not park in time\n");
    mcs = mixer_lock();
    if (!xbox_snap_gate(1, 1000)) fprintf(stderr, "[QS] another guest thread kept running\n");
    mcpx_apu_snap_lock(1);
    nw = xbox_worker_stacks(lo, hi, 8);
    if (nw > 8) nw = 8;
    /* guest RAM, minus the live worker stacks */
    {
        uint32_t a = 0;
        while (a < QS_RAM) {
            uint32_t next = QS_RAM;
            int skip = -1;
            for (i = 0; i < nw; i++) {
                if (a >= lo[i] && a < hi[i]) { skip = i; break; }
                if (lo[i] > a && lo[i] < next) next = lo[i];
            }
            if (skip >= 0) { a = hi[skip]; continue; }
            memcpy(mem + a, st->ram + a, next - a);
            a = next;
        }
    }
    for (i = 0; i < st->nreg; i++) memcpy(st->reg[i].p, st->data[i], st->reg[i].n);
    regs_put(&st->r);
    {
        int nf = xbox_snap_files_restore(st->files, st->nfiles);
        if (nf != st->nfiles) fprintf(stderr, "[QS] %d of %d open files put back at their saved read position\n", nf, st->nfiles);
    }
    pcs_rebind(st->pc_blk ? (char *)xbox_GetMemoryOffset() + st->pc_blk : NULL);
    mcpx_apu_snap_lock(0);
    xbox_snap_gate(0, 0);
    mixer_unlock(mcs);
    xbox_nv2a_pause(0, 0);
    if (s_clock_on) s_tsc_off = (int64_t)(host_tsc() - st->tsc);
    s_loads++;
    {
        char m[96];
        if (st->slot_no <= 0) snprintf(m, sizeof m, "Quick save loaded");
        else snprintf(m, sizeof m, "Slot %d loaded", st->slot_no);
        note(m);
        fprintf(stderr, "[QS] loaded slot %d (saved at frame %u, %d worker stacks kept)\n", st->slot_no, st->flip, nw);
    }
    return 1;
}


/* ---- on disk -------------------------------------------------------------
 * <dir>/quicksave.pcjs, written by a background thread after each save so
 * the game does not wait for the disk. Pages of 4 KB that are all zero are
 * stored as one byte. A file only loads into the build that wrote it (the
 * region list must match by name and size) and into the same chapter (map
 * hash); after a restart that means: start the chapter, then load. */
#define QS_PAGE 4096u
static char s_dir[512];
static volatile int s_writing;
#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define QS_MKDIR(d) _mkdir(d)
#else
#include <pthread.h>
#include <sys/stat.h>
#define QS_MKDIR(d) mkdir(d, 0755)
#endif

static const char *qs_dir(void)
{
    if (!s_dir[0]) {
        const char *e = getenv("CJ_QS_DIR");
        snprintf(s_dir, sizeof s_dir, "%s", e && *e ? e : "saves");
        QS_MKDIR(s_dir);
    }
    return s_dir;
}

static uint64_t build_key(const QsState *st)
{
    uint64_t h = 1469598103934665603ull;
    int i;
    const char *k = "pcj-qs-1";
    for (; *k; k++) h = (h ^ (uint8_t)*k) * 1099511628211ull;
    for (i = 0; i < st->nreg; i++) {
        const char *n = st->reg[i].name;
        for (; *n; n++) h = (h ^ (uint8_t)*n) * 1099511628211ull;
        h = (h ^ (uint64_t)st->reg[i].n) * 1099511628211ull;
    }
    return h;
}

typedef struct {
    char magic[8];
    uint32_t version, nreg;
    uint64_t key, tsc;
    uint32_t map_hash, flip, nwork, wlo[8], whi[8], pc_blk;
    int32_t nfiles;
    XboxSnapFile files[64];
    QsRegs r;
    QsGame game;
} QsFileHdr;

static int write_blob(FILE *f, const uint8_t *p, size_t n)
{
    size_t off;
    static const uint8_t zero[QS_PAGE];
    for (off = 0; off < n; off += QS_PAGE) {
        size_t len = n - off < QS_PAGE ? n - off : QS_PAGE;
        uint8_t tag = (len == QS_PAGE && !memcmp(p + off, zero, QS_PAGE)) ? 0 : 1;
        if (fputc(tag, f) == EOF) return 0;
        if (tag && fwrite(p + off, 1, len, f) != len) return 0;
    }
    return 1;
}
static int read_blob(FILE *f, uint8_t *p, size_t n)
{
    size_t off;
    for (off = 0; off < n; off += QS_PAGE) {
        size_t len = n - off < QS_PAGE ? n - off : QS_PAGE;
        int tag = fgetc(f);
        if (tag == 0) memset(p + off, 0, len);
        else if (tag == 1) { if (fread(p + off, 1, len, f) != len) return 0; }
        else return 0;
    }
    return 1;
}

static void write_file(void)
{
    QsState *st = &s_slot;
    char path[600], tmp[620];
    FILE *f;
    QsFileHdr h;
    int i, ok;
    slot_path(path, sizeof path, st->slot_no, st->map_hash);
    if (s_write_path) snprintf(path, sizeof path, "%s", s_write_path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) { fprintf(stderr, "[QS] cannot write %s\n", tmp); return; }
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "PCJQS\0\1", 8);
    h.version = 2; h.nreg = (uint32_t)st->nreg; h.key = build_key(st); h.tsc = st->tsc;
    h.map_hash = st->map_hash; h.flip = st->flip; h.nwork = st->nwork; h.pc_blk = st->pc_blk; h.nfiles = st->nfiles; memcpy(h.files, st->files, sizeof h.files);
    memcpy(h.wlo, st->wlo, sizeof h.wlo); memcpy(h.whi, st->whi, sizeof h.whi);
    h.r = st->r; h.game = st->game;
    ok = fwrite(&h, sizeof h, 1, f) == 1 && write_blob(f, st->ram, QS_RAM);
    for (i = 0; ok && i < st->nreg; i++) ok = write_blob(f, st->data[i], st->reg[i].n);
    if (fclose(f) || !ok) { fprintf(stderr, "[QS] writing %s failed\n", tmp); remove(tmp); return; }
#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp, path)) fprintf(stderr, "[QS] cannot replace %s\n", path);
    else fprintf(stderr, "[QS] wrote %s\n", path);
}
#ifdef _WIN32
static DWORD WINAPI writer(LPVOID a) { (void)a; write_file(); s_writing = 0; return 0; }
static void write_async(void) { HANDLE t; s_writing = 1; t = CreateThread(NULL, 0, writer, NULL, 0, NULL); if (t) CloseHandle(t); else { write_file(); s_writing = 0; } }
#else
static void *writer(void *a) { (void)a; write_file(); s_writing = 0; return NULL; }
static void write_async(void) { pthread_t t; s_writing = 1; if (pthread_create(&t, NULL, writer, NULL) == 0) pthread_detach(t); else { write_file(); s_writing = 0; } }
#endif
static void wait_writer(void)
{
    while (s_writing) {
#ifdef _WIN32
        Sleep(5);
#else
        struct timespec ts = {0, 5000000}; nanosleep(&ts, NULL);
#endif
    }
}

/* Read the file into the slot; 0 with a message when it does not fit. */
static int read_file(int slot)
{
    QsState *st = &s_slot;
    char path[600];
    FILE *f;
    QsFileHdr h;
    int i, ok;
    migrate_old();
    slot_path(path, sizeof path, slot, cur_map());
    if (s_read_path) snprintf(path, sizeof path, "%s", s_read_path);
    if (!(f = fopen(path, "rb"))) {
        char m[64];
        if (slot <= 0) snprintf(m, sizeof m, "No quick save yet (F5 saves)");
        else snprintf(m, sizeof m, "Slot %d is empty", slot);
        note(m);
        return 0;
    }
    if (!st->ram && !(st->ram = malloc(QS_RAM))) { fclose(f); note("Quick load failed: out of memory"); return 0; }
    if (!st->nreg) {
        st->nreg = list_regions(st->reg, QS_MAX_REGIONS);
        for (i = 0; i < st->nreg; i++)
            if (!(st->data[i] = malloc(st->reg[i].n))) { fclose(f); st->nreg = 0; note("Quick load failed: out of memory"); return 0; }
    }
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "PCJQS\0\1", 8) || h.version != 2
            || h.key != build_key(st) || h.nreg != (uint32_t)st->nreg) {
        fclose(f);
        note("That quick save is from a different build of the game and cannot be loaded");
        return 0;
    }
    if (h.map_hash != g32(0x0048E874u) && !any_map()) {
        fclose(f);
        note("That quick save is from another chapter: start that chapter, then load");
        return 0;
    }
    ok = read_blob(f, st->ram, QS_RAM);
    for (i = 0; ok && i < st->nreg; i++) ok = read_blob(f, st->data[i], st->reg[i].n);
    fclose(f);
    if (!ok) { st->valid = 0; note("Quick load failed: the file is damaged"); return 0; }
    /* A save file can come from anywhere: keep the header's pointers and paths in bounds. */
    if (h.pc_blk >= QS_RAM || (h.pc_blk & 3)) h.pc_blk = 0;           /* a guest offset inside RAM, or none */
    if (h.nfiles < 0) h.nfiles = 0;
    if (h.nfiles > 64) h.nfiles = 64;
    if (h.nwork > 8) h.nwork = 8;
    for (i = 0; i < h.nfiles; i++) {                                   /* only plain paths to reopen read-only */
        h.files[i].path[sizeof h.files[i].path - 1] = 0;
        if (strstr(h.files[i].path, "..")) h.files[i].path[0] = 0;
    }
    st->tsc = h.tsc; st->map_hash = h.map_hash; st->flip = h.flip; st->nwork = h.nwork; st->pc_blk = h.pc_blk; st->nfiles = h.nfiles; memcpy(st->files, h.files, sizeof st->files);
    memcpy(st->wlo, h.wlo, sizeof st->wlo); memcpy(st->whi, h.whi, sizeof st->whi);
    st->r = h.r; st->game = h.game;
    st->valid = 1;
    st->slot_no = slot;
    s_slot_disk = 1;
    fprintf(stderr, "[QS] read %s\n", path);
    return 1;
}

/* ---- stall report ----------------------------------------------------------
 * From the first frame, a host thread watches the flip count. If the game stops presenting
 * for 15 s (not while it sleeps on purpose because the window lost focus) it writes what
 * the game thread is doing (recent indirect-call targets, twice, the GPU queue, and the
 * guest stack words that look like return addresses) to the log and to pcj-freeze.txt,
 * once per freeze. For bug reports. */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
static volatile uint32_t *s_game_esp;
static volatile int s_mon_started;
static FILE *s_rep;   /* pcj-freeze.txt while a report is written (a freeze can lose the log's tail) */
#define REP(...) do { fprintf(stderr, __VA_ARGS__); if (s_rep) fprintf(s_rep, __VA_ARGS__); } while (0)
static void stall_report(void)
{
    int pass, k;
    for (pass = 0; pass < 2; pass++) {
        REP( "[QS] stall: recent indirect calls:");
        for (k = 0; k < 16; k++) REP( " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
        REP( "\n");
        if (pass == 0) {
#ifdef _WIN32
            Sleep(1500);
#else
            struct timespec ts = {1, 500000000}; nanosleep(&ts, NULL);
#endif
        }
    }
    {   /* the GPU side: ring registers, the walk's own position, D3D's fence */
        XboxSnapRegion r[QS_MAX_REGIONS];
        int nr = list_regions(r, QS_MAX_REGIONS), k;
        const uint8_t *mmio = 0;
        uint32_t lastput = 0, getw = 0, semva = 0;
        for (k = 0; k < nr && k < QS_MAX_REGIONS; k++) {
            if (!strcmp(r[k].name, "nv2a.mmio")) mmio = (const uint8_t *)r[k].p;
            else if (!strcmp(r[k].name, "ack.last_put")) lastput = *(const uint32_t *)r[k].p;
            else if (!strcmp(r[k].name, "ack.get_written")) getw = *(const uint32_t *)r[k].p;
            else if (!strcmp(r[k].name, "pb_exec.s_sem_va")) semva = *(const uint32_t *)r[k].p;
        }
        if (mmio)
            REP( "[QS] stall: USER PUT %08X GET %08X, PFIFO PUT %08X GET %08X, walk last_put %08X get_written %08X\n",
                    *(const uint32_t *)(mmio + 0x800040), *(const uint32_t *)(mmio + 0x800044),
                    *(const uint32_t *)(mmio + 0x3240), *(const uint32_t *)(mmio + 0x3244), lastput, getw);
        REP( "[QS] stall: semaphore va %08X = %08X; D3D device +0x30 -> %08X = %08X\n",
                semva, semva ? g32(semva) : 0, g32(0x00308220u + 0x30), g32(g32(0x00308220u + 0x30)));
    }
    if (s_game_esp) {
        uint32_t esp = *s_game_esp, a;
        int n = 0;
        REP( "[QS] stall: game esp %08X, code addresses on the stack:", esp);
        for (a = esp; a && a < esp + 0x2000 && n < 40; a += 4) {
            uint32_t v = g32(a);
            if (v >= 0x00011000u && v < 0x00390000u) { REP( " %08X", v); n++; }
        }
        REP( "\n");
    }
    fflush(stderr);
    if (s_rep) fflush(s_rep);
}
static double mono_s(void)
{
#ifdef _WIN32
    return GetTickCount64() / 1000.0;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}
/* Sleeps can return early here (the recompiler's fault handling signals this process all the
 * time), so time the stall by the clock, not by counting sleeps. */
/* The GPU's write-back flush bit (NV2A register 0x100410, bit 16). D3D::CDevice::KickOff sets it
 * and spins until the hardware clears it whenever the device has not yet passed its first kickoff
 * (device+8 flag 0x2000). The port's register file is plain memory, so nothing cleared it and the
 * game hung (seen after Frame Rate 60 -> 30 -> 60: stack KickOff <- SetFence <- D3DDevice_Swap).
 * Like the hardware, clear it once it has been set for a moment; KickOff then sets the flag again. */
static void wbc_flush_tick(void)
{
    static volatile uint32_t *reg;
    static int tries, seen;
    static unsigned long cleared;
    if (!reg && tries < 50) {
        XboxSnapRegion r[QS_MAX_REGIONS];
        int nr = list_regions(r, QS_MAX_REGIONS), k;
        tries++;
        for (k = 0; k < nr && k < QS_MAX_REGIONS; k++)
            if (!strcmp(r[k].name, "nv2a.mmio")) reg = (volatile uint32_t *)((uint8_t *)r[k].p + 0x100410);
    }
    if (!reg) return;
    if (*reg & 0x10000u) {
        if (seen) { *reg &= ~0x10000u; if (cleared++ < 4) fprintf(stderr, "[GPU] write-back flush bit cleared for D3D (KickOff wait)\n"); seen = 0; }
        else seen = 1;
    } else seen = 0;
}
static void stall_loop(void)
{
    uint32_t last = cj_gl_flips();
    double since = mono_s();
    int reported = 0;
    for (;;) {
        int k;
        for (k = 0; k < 50; k++) {   /* a second, in 20 ms steps */
#ifdef _WIN32
            Sleep(20);
#else
            struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);
#endif
            wbc_flush_tick();
        }
        uint32_t f = cj_gl_flips();
        extern volatile int cj_gl_focus_frozen;
        if (f != last || cj_gl_focus_frozen) { last = f; since = mono_s(); reported = 0; continue; }   /* (switched away: not a freeze) */
        if (!reported && mono_s() - since >= 15.0) {
            reported = 1;
            {   /* the report also goes straight to pcj-freeze.txt next to the exe */
                char path[1024] = "pcj-freeze.txt";
#ifdef _WIN32
                char *sl;
                GetModuleFileNameA(NULL, path, sizeof path - 20);
                if ((sl = strrchr(path, '\\'))) strcpy(sl + 1, "pcj-freeze.txt");
#endif
                s_rep = fopen(path, "w");
            }
            REP("[QS] stall: no frame for 15 s (flip %u, %d loads)\n", f, s_loads);
            stall_report();
            if (s_rep) { fclose(s_rep); s_rep = NULL; }
        }
    }
}
#ifdef _WIN32
static DWORD WINAPI stall_thread(LPVOID a) { (void)a; stall_loop(); return 0; }
static void stall_monitor(void) { HANDLE t; if (s_mon_started) return; s_mon_started = 1; t = CreateThread(NULL, 0, stall_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
#else
static void *stall_thread(void *a) { (void)a; stall_loop(); return NULL; }
static void stall_monitor(void) { pthread_t t; if (s_mon_started) return; s_mon_started = 1; if (pthread_create(&t, NULL, stall_thread, NULL) == 0) pthread_detach(t); }
#endif

static int s_req_slot;
void qs_request(int what) { s_request = what; s_req_slot = 0; }   /* the quick save */
void qs_request_slot(int what, int slot) { s_request = what; s_req_slot = slot; }
/* From the pause menu (cjmod_pause_qs): act once the menu has closed. The menu calls
 * qs_pause_alive every frame it is up; the request fires after 6 updates without it, with the
 * player in play. Gives up after ~10 s (the menu did not close). */
static int s_after, s_after_slot, s_after_quiet, s_after_age, s_pause_seen;
void qs_after_pause(int what, int slot) { s_after = what; s_after_slot = slot; s_after_quiet = 0; s_after_age = 0; }
int qs_after_pause_pending(void) { return s_after != 0; }
void qs_pause_alive(void) { s_pause_seen = 1; }

unsigned qs_message_gen(void) { return s_msg_gen; }
const char *qs_message(void) { return s_msg[0] && cj_gl_flips() < s_msg_until ? s_msg : 0; }


/* Kill every actor within r of the player through the game's own path:
   Actor::SetHealth(0, 1) (death objective), then StateMachine::ExtGosub(0, STATE_StaggerAndDie). */
static void test_kill_near(float r)
{
    uint32_t pl = g32(0x0048FC98u), it = pl ? g32(pl + 4) : 0, i, n = 0;
    float pp[3];
    uint8_t *m = (uint8_t *)xbox_GetMemoryOffset();
    if (!it) return;
    memcpy(pp, m + it + 0xC8, 12);
    for (i = 0; i < 128; i++) {   /* the actor base table (gamelog.c) */
        uint32_t ab = 0x0047C3F0u + i * 0x18u, a = g32(ab + 0x10);
        float ap[3], d, hp;
        if (!a) continue;
        memcpy(ap, m + ab, 12);
        memcpy(&hp, m + a + 0x7DC, 4);
        d = sqrtf((ap[0] - pp[0]) * (ap[0] - pp[0]) + (ap[1] - pp[1]) * (ap[1] - pp[1]) + (ap[2] - pp[2]) * (ap[2] - pp[2]));
        if (d > r || !(hp > 0.0f)) continue;
        {
            uint32_t esp0 = g_esp, eax0 = g_eax, ecx0 = g_ecx, edx0 = g_edx;
            PUSH32(g_esp, 1u); PUSH32(g_esp, 0u); PUSH32(g_esp, 0x0000C0DEu);
            g_ecx = a; recomp_lookup(0x00016720u)();
            g_esp = esp0;
            PUSH32(g_esp, 0u); PUSH32(g_esp, 0u); PUSH32(g_esp, 0u); PUSH32(g_esp, 0u);
            PUSH32(g_esp, 0x000669D0u); PUSH32(g_esp, 0u); PUSH32(g_esp, 0x0000C0DEu);
            g_ecx = a + 0xE8u; recomp_lookup(0x00080490u)();
            g_esp = esp0; g_eax = eax0; g_ecx = ecx0; g_edx = edx0;
        }
        n++;
    }
    fprintf(stderr, "[QS] test: %u actors within %.0f killed\n", n, r);
}

/* CJ_POS_TRACK=<file>: move the player along a recorded track (tools/pos_track.py makes one from a
   frames.csv). Starts at the first update with the player in control after a level load. Lines:
   "<n> <x> <y> <z>" a position key, n = game updates since the start (positions in between are
   interpolated; the clock stops while a cutscene runs, so leave the recorded cutscenes out); "<n> K <r>" kill actors within r;
   "<n> H" health back to full; "<n> S <slot>" quick save to that slot (scene library). */
static int s_autoload_state;   /* 0 off, 1 waiting, 2 loaded */
#define TRACK_MAX 4096
static void pos_track(void)
{
    static int state = -1, nk, next_ev;
    static unsigned n;
    static struct { unsigned n; char op; float v[3]; } k[TRACK_MAX];
    uint32_t app = g32(0x004931E8u), pl = g32(0x0048FC98u), it = pl ? g32(pl + 4) : 0;
    uint8_t *m = (uint8_t *)xbox_GetMemoryOffset();
    int i;
    if (state < 0) {
        const char *e = getenv("CJ_POS_TRACK");
        FILE *f = e ? fopen(e, "r") : NULL;
        char line[128];
        state = 0;
        if (!f) return;
        while (nk < TRACK_MAX && fgets(line, sizeof line, f)) {
            char op[4] = "";
            if (sscanf(line, "%u %3s", &k[nk].n, op) != 2) continue;
            if (op[0] == 'K') { k[nk].op = 'K'; sscanf(line, "%*u %*s %f", &k[nk].v[0]); }
            else if (op[0] == 'S') { k[nk].op = 'S'; sscanf(line, "%*u %*s %f", &k[nk].v[0]); }
            else if (op[0] == 'H') k[nk].op = 'H';
            else { k[nk].op = 'P'; if (sscanf(line, "%*u %f %f %f", &k[nk].v[0], &k[nk].v[1], &k[nk].v[2]) != 3) continue; }
            nk++;
        }
        fclose(f);
        state = nk ? 1 : 0;
        fprintf(stderr, "[QS] track: %d keys from %s\n", nk, e);
    }
    if (state == 0 || !app || !it) return;
    if (state == 1) {
        if (s_autoload_state == 1) return;     /* scene runs: start the track after the scene is loaded */
        if (!g8(app + 0xCBC) || !g8(app + 0xD00) || g8(app + 0xE30)) return;
        state = 2; n = 0; next_ev = 0;
        fprintf(stderr, "[QS] track: start at flip %u\n", cj_gl_flips());
    }
    if (state != 2) return;
    for (; next_ev < nk && k[next_ev].n <= n; next_ev++) {
        if (k[next_ev].op == 'K') test_kill_near(k[next_ev].v[0]);
        else if (k[next_ev].op == 'H') { float h = 100.0f; memcpy(m + pl + 0xC5C, &h, 4); }
        else if (k[next_ev].op == 'S') { s_target = (int)k[next_ev].v[0]; s_request = 1; s_req_slot = s_target; }
    }
    if (!g8(app + 0xE30)) {
        int a = -1, b = -1;
        for (i = 0; i < nk; i++) {
            if (k[i].op != 'P') continue;
            if (k[i].n <= n) a = i;
            else { b = i; break; }
        }
        if (a >= 0) {
            float v[3], t = 0.0f;
            if (b >= 0 && k[b].n > k[a].n) t = (float)(n - k[a].n) / (float)(k[b].n - k[a].n);
            for (i = 0; i < 3; i++) v[i] = k[a].v[i] + (b >= 0 ? (k[b].v[i] - k[a].v[i]) * t : 0.0f);
            memcpy(m + it + 0xC8, v, 12);
            m[it + 0x7E] = 0;
        }
        if (b < 0 && next_ev >= nk) { state = 3; fprintf(stderr, "[QS] track: done at flip %u\n", cj_gl_flips()); }
        n++;   /* the track clock stops while a cutscene runs (tracks leave the recorded cutscenes out) */
    }
}

static void test_script(void)
{
    static const char *e = (const char *)1;
    unsigned f;
    const char *p;
    if (e == (const char *)1) e = getenv("CJ_QS_TEST");
    if (!e) return;
    f = cj_gl_flips();
    for (p = e; *p;) {
        unsigned at = (unsigned)strtoul(p, (char **)&p, 10);
        if (*p++ != ':') break;
        if (at == f || at + 1 == f) {      /* the safe point may skip a flip */
            static unsigned done_at = ~0u;
            if (done_at != at) {
                done_at = at;
                if (*p == 'k') {          /* "kill": the player's health to 0 (death / mission restart tests) */
                    uint32_t pl = g32(0x0048FC98u);
                    float z = 0.0f;
                    if (pl) memcpy((uint8_t *)xbox_GetMemoryOffset() + pl + 0xC5C, &z, 4);
                    fprintf(stderr, "[QS] test: player health set to 0\n");
                } else if (*p == 'd') {   /* "die": a real fatal fall through the game's damage path */
                    uint32_t pl = g32(0x0048FC98u);
                    if (pl) {
                        recomp_func_t fall = recomp_lookup(0x0016A3B0u);   /* XItemHandler_Player::InflictDamageFromFall */
                        uint32_t esp0 = g_esp, eax0 = g_eax, ecx0 = g_ecx, edx0 = g_edx;
                        float sp = -40.0f;
                        uint32_t spu;
                        memcpy(&spu, &sp, 4);
                        PUSH32(g_esp, spu);
                        PUSH32(g_esp, 0x0000C0DEu);
                        g_ecx = pl;
                        fall();
                        g_esp = esp0; g_eax = eax0; g_ecx = ecx0; g_edx = edx0;
                        fprintf(stderr, "[QS] test: fatal fall damage\n");
                    }
                } else if (*p == 't') {   /* "tp=x/y/z": move the player there (test routes) */
                    uint32_t pl = g32(0x0048FC98u), it = pl ? g32(pl + 4) : 0;
                    float v[3] = {0, 0, 0};
                    const char *q = strchr(p, '=');
                    if (it && q && sscanf(q + 1, "%f/%f/%f", &v[0], &v[1], &v[2]) == 3) {
                        uint8_t *m = (uint8_t *)xbox_GetMemoryOffset();
                        memcpy(m + it + 0xC8, v, 12);
                        m[it + 0x7E] = 0;
                        fprintf(stderr, "[QS] test: player moved to %.2f %.2f %.2f\n", v[0], v[1], v[2]);
                    }
                } else if (*p == 'K') {   /* "Killnear=R": kill every actor within R of the player */
                    const char *q = strchr(p, '=');
                    test_kill_near(q ? (float)atof(q + 1) : 20.0f);
                } else if (*p == 'n') {   /* "next": next slot */
                    s_request = 4;
                } else
                    s_request = (*p == 's') ? 1 : 2;
            }
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
}

/* EXBaseApp::Update entry (vtable call from the main loop). */
/* ---- quick save / load inside recordings ---------------------------------
 * Recording (CJ_REC=1): every save, load and slot change the safe point acts on
 * is logged to <rec>/qs.txt as "<poll> <request> <slot>". A load whose slot came
 * from disk (not saved earlier in the session) also copies that save into the
 * recording as qs-load-<poll>.pcjs, so the replay does not depend on the save
 * folder. Replay (CJ_REC_PLAY): keys, pad combos and CJ_QS_TEST requests are
 * ignored; each logged request runs at its poll, saves stay in memory (the
 * player's slots on disk are never touched), loads use the session's own save
 * or the copy. Saves and loads run at the same point of the same frame, so the
 * replay's heap hashes match through them. */
extern int cj_rec_mode(void);
extern const char *cj_rec_dir(void);
extern unsigned long cj_rec_poll(void);
static FILE *s_rec_qs;
static unsigned long s_ev_poll = ~0ul;
static int s_ev_req, s_ev_slot, s_ev_force;
static void rec_next_event(void)
{
    char line[128];
    s_ev_poll = ~0ul;
    s_ev_force = 0;
    if (s_rec_qs && fgets(line, sizeof line, s_rec_qs)
            && sscanf(line, "%lu %d %d %d", &s_ev_poll, &s_ev_req, &s_ev_slot, &s_ev_force) < 3) s_ev_poll = ~0ul;
}
/* The request to act on at this safe point (0 none). */
static int rec_request(int r)
{
    static int init;
    int mode = cj_rec_mode();
    if (!mode) return r;
    if (!init) {
        char p[1200];
        init = 1;
        snprintf(p, sizeof p, "%s/qs.txt", cj_rec_dir());
        s_rec_qs = fopen(p, mode == 1 ? "w" : "r");
        if (mode == 2) { s_nodisk = 1; rec_next_event(); if (s_ev_poll != ~0ul) fprintf(stderr, "[QS] replay: quick saves/loads from %s\n", p); }
    }
    if (mode == 1) {
        if (r && s_rec_qs) { fprintf(s_rec_qs, "%lu %d %d %d\n", cj_rec_poll(), r, s_req_slot, s_force > 0); fflush(s_rec_qs); }
        return r;
    }
    if (s_ev_poll != ~0ul && cj_rec_poll() >= s_ev_poll) {
        int q = s_ev_req;
        if (cj_rec_poll() != s_ev_poll) fprintf(stderr, "[QS] replay: request %d logged at poll %lu runs at %lu\n", q, s_ev_poll, cj_rec_poll());
        if (q == 1 || q == 2) s_req_slot = s_ev_slot;   /* the slot it used then */
        s_force = s_ev_force;    /* CJ_QS_FORCE as it was then (saves in cutscenes) */
        rec_next_event();
        return q;
    }
    return 0;
}
/* Record: a load that used a save from disk keeps a copy in the recording. */
static void rec_after_load(unsigned long poll)
{
    char p[1200];
    if (cj_rec_mode() != 1 || !s_slot_disk) return;
    snprintf(p, sizeof p, "%s/qs-load-%lu.pcjs", cj_rec_dir(), poll);
    s_write_path = p;
    write_file();
    s_write_path = NULL;
}
static int rec_load(void)
{
    unsigned long poll = cj_rec_poll();
    char p[1200];
    int ok;
    if (cj_rec_mode() == 2) {   /* a copy exists when the recorded load came from disk */
        FILE *f;
        snprintf(p, sizeof p, "%s/qs-load-%lu.pcjs", cj_rec_dir(), poll);
        if ((f = fopen(p, "rb"))) { fclose(f); s_slot.valid = 0; s_read_path = p; }
    }
    ok = do_load();
    s_read_path = NULL;
    if (ok) rec_after_load(poll);
    return ok;
}

static void qs_update_entry(void)
{
    if (s_force < 0) s_force = getenv("CJ_QS_FORCE") != NULL;   /* tests: allow saves in cutscenes */
    if (s_nodisk < 0) { const char *e = getenv("CJ_QS_DISK"); s_nodisk = e && *e == '0'; }
    test_script();
    {   /* CJ_QS_AUTOLOAD=1 (scene library, tools/scene.py): load the current slot once the player first
           has control in a level, CJ_QS_AUTOLOAD_DELAY updates later (default 30) */
        static int al = -1, wait = -1;
        if (al < 0) { const char *e = getenv("CJ_QS_AUTOLOAD"); al = e && *e == '1'; e = getenv("CJ_QS_AUTOLOAD_DELAY"); wait = e ? atoi(e) : 30; s_autoload_state = al; }
        if (al == 1) {
            QsGame g;
            game_state(&g);
            if (g.level && g.have_player && g.control && !g.cut && --wait <= 0) {
                al = 2; s_autoload_state = 2;
                s_request = 2;
                s_req_slot = 0;
                fprintf(stderr, "[QS] autoload: loading the quick save at flip %u\n", cj_gl_flips());
            }
        }
    }
    pos_track();
    if (s_verify) {      /* one frame after a load: did the world come back as saved? */
        QsGame g;
        game_state(&g);
        fprintf(stderr, "[QS] verify: player %.2f %.2f %.2f health %.1f energy %.1f (saved %.2f %.2f %.2f / %.1f / %.1f)%s\n",
                g.x, g.y, g.z, g.health, g.energy, s_slot.game.x, s_slot.game.y, s_slot.game.z, s_slot.game.health,
                s_slot.game.energy,
                (fabsf(g.x - s_slot.game.x) < 0.5f && fabsf(g.z - s_slot.game.z) < 0.5f && fabsf(g.health - s_slot.game.health) < 0.01f)
                    ? " OK" : " MISMATCH");
        s_verify = 0;
    }
    if (s_after) {
        QsGame g;
        game_state(&g);
        if (s_pause_seen) s_after_quiet = 0; else s_after_quiet++;
        s_pause_seen = 0;
        if (++s_after_age > 600) { s_after = 0; note("Pause menu did not close"); }
        else if (s_after_quiet >= 6 && g.level && g.have_player && g.control && !g.cut) { qs_request_slot(s_after, s_after_slot); s_after = 0; }
    } else
        s_pause_seen = 0;
    if (!s_mon_started) { s_game_esp = &g_esp; stall_monitor(); }   /* freeze reports from any point, not only after loads */
    {
        int r0 = s_request;
        if (r0 || cj_rec_mode()) { s_request = 0; s_request_now = rec_request(r0); }
        else s_request_now = 0;
    }
    if (s_request_now == 3 || s_request_now == 4) s_request_now = 0;   /* old F6 / F7 slot keys */
    if (s_request_now) s_target = s_req_slot;
    if (s_request_now) {
        int r = s_request_now;
        QsGame g;
        s_request_now = 0;
        game_state(&g);
        migrate_old();
        if (!g.level || !g.have_player) note(r == 1 ? "Can't save here (no level loaded)" : "Loading works in a level for now");
        else if (r == 1 && (g.cut || !g.control) && !s_force) note("Can't quick save during a cutscene");
        else if (r == 1) { s_pending_game = g; do_save(); }
        else { s_game_esp = &g_esp; s_verify = rec_load(); if (s_verify) stall_monitor(); }
    }
    sub_001B97B0();
}

recomp_func_t qs_lookup(uint32_t va)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("CJ_QUICKSAVE"); on = !(e && *e == '0'); }
    return (on && va == 0x001B97B0u) ? (recomp_func_t)qs_update_entry : (recomp_func_t)0;
}
