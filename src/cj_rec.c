/*
 * Session recording and exact replay (docs/recording.md).
 *
 * Normal play cannot be replayed: the game's timing (frame length from the
 * host clock, file reads finishing on another thread, the mixer thread) makes
 * a replay drift within seconds. A recording session therefore runs in the
 * deterministic mode (CJ_DET, src/determinism.c): synchronous file I/O, clocks
 * derived from the frame count, a fixed number of vblanks per frame. Then the
 * controller state the game read at each poll is all that is needed to run the
 * same session again, on this PC or in the cloud (the Windows and Linux
 * builds give the same guest heap frame for frame).
 *
 *   CJ_REC=1           record: logs/rec-<date>-<time>/ gets input.pcjr (the
 *                      pad at every poll), save/ (a copy of the save folder's
 *                      UDATA and TDATA at boot) and hashes.txt (guest heap hash
 *                      every 60 frames). Implies CJ_DET; one vblank per frame
 *                      at 60 fps (CJ_FPS=60), else two. Frames are paced to
 *                      real time.
 *   CJ_REC_EXIT=1      exit when the input ends (status 3 if it diverged); =2 also
 *                      exit at the first divergence. Both print the swap report.
 *   CJ_REC_PLAY=<dir>  replay that recording (start from a copy of its save/):
 *                      the pad comes from input.pcjr, the heap hash is compared
 *                      every 60 frames ([REC] DIVERGE at the first mismatch),
 *                      frames run as fast as the host allows unless
 *                      CJ_REC_REALTIME=1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <mmsystem.h>
#define MKDIR(p) _mkdir(p)
static void cj_setenv(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
static void cj_setenv(const char *k, const char *v) { setenv(k, v, 1); }
#endif

extern uint64_t xbox_HostNowNs(void);
/* per-function call counts of the hybrid swap wrappers (build/hybrid/hybrid_swap.c), when built in */
void cj_swap_report(const char *tag) __attribute__((weak));
/* dual-execution verifier pass (swap_verify.c): 0 none, 1 first original run, 2 second, 3 native */
int cjv_pass(void) __attribute__((weak));
void cj_rec_verify_pass(int pass);
static uint8_t s_ring[64][18]; static int s_ring_n, s_ring_at;
/* called by the verifier at each pass boundary */
void cj_rec_verify_pass(int pass) { if (pass == 1) s_ring_n = 0; s_ring_at = 0; }

#define REC_MAGIC "PCJREC01"
typedef struct {
    char magic[8];
    uint32_t version, lockstep, row_bytes, reserved;
    char build[48];
} RecHeader;
typedef struct {                /* one XInputGetState poll of port 0 */
    uint16_t dig;
    uint8_t av[8];
    int16_t lx, ly, rx, ry;
} RecRow;                       /* 18 bytes, written field by field */

static int s_mode;              /* 0 off, 1 record, 2 replay */
static char s_dir[1024];
static FILE *s_in, *s_hash;
static RecHeader s_hdr;
static unsigned long s_rows, s_rows_total;
static int s_lockstep = 2, s_realtime;
static unsigned s_diverged, s_checked;

#ifndef CJ_BUILD_ID
#define CJ_BUILD_ID "unknown"
#endif

int cj_rec_mode(void) { return s_mode; }
const char *cj_rec_dir(void) { return s_mode ? s_dir : NULL; }
unsigned long cj_rec_poll(void) { return s_rows; }   /* polls so far: the session's input clock */

static void copy_tree(const char *from, const char *to, int *n)
{
    DIR *d = opendir(from);
    struct dirent *e;
    char a[1200], b[1200];
    if (!d) return;
    MKDIR(to);
    while ((e = readdir(d))) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (stat(a, &st)) continue;
        if (S_ISDIR(st.st_mode)) { copy_tree(a, b, n); continue; }
        {
            FILE *fi = fopen(a, "rb"), *fo = fi ? fopen(b, "wb") : NULL;
            char buf[65536];
            size_t k;
            if (fi && fo) { while ((k = fread(buf, 1, sizeof buf, fi)) > 0) fwrite(buf, 1, k, fo); (*n)++; }
            if (fi) fclose(fi);
            if (fo) fclose(fo);
        }
    }
    closedir(d);
}

/* At boot, before the kernel and the deterministic mode start. save_dir: the
 * emulated hard disk's folder; fps60: the PC frame-rate setting. */
void cj_rec_boot(const char *save_dir, int fps60)
{
    const char *e = getenv("CJ_REC"), *p = getenv("CJ_REC_PLAY");
    char buf[32];
    if (p && *p) {
        char path[1200];
        snprintf(s_dir, sizeof s_dir, "%s", p);
        snprintf(path, sizeof path, "%s/input.pcjr", s_dir);
        s_in = fopen(path, "rb");
        if (!s_in || fread(&s_hdr, sizeof s_hdr, 1, s_in) != 1 || memcmp(s_hdr.magic, REC_MAGIC, 8)) {
            fprintf(stderr, "[REC] cannot read %s\n", path);
            if (s_in) fclose(s_in);
            s_in = NULL;
            return;
        }
        fseek(s_in, 0, SEEK_END);
        s_rows_total = (unsigned long)((ftell(s_in) - (long)sizeof s_hdr) / 18);
        fseek(s_in, (long)sizeof s_hdr, SEEK_SET);
        s_lockstep = (int)s_hdr.lockstep;
        snprintf(path, sizeof path, "%s/hashes.txt", s_dir);
        s_hash = fopen(path, "r");
        s_mode = 2;
        { const char *r = getenv("CJ_REC_REALTIME"); s_realtime = r && *r == '1'; }
        if (strcmp(s_hdr.build, CJ_BUILD_ID))
            fprintf(stderr, "[REC] note: recorded with build %s, this is %s\n", s_hdr.build, CJ_BUILD_ID);
        fprintf(stderr, "[REC] replaying %s: %lu polls, %d vblanks per frame\n", s_dir, s_rows_total, s_lockstep);
    } else if (e && *e == '1') {
        char path[1200], parent[1024];
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        int n = 0;
        const char *g = getenv("CJ_GAMELOG_DIR");
        snprintf(parent, sizeof parent, "%s", g && *g ? g : "logs");
        MKDIR(parent);
        snprintf(s_dir, sizeof s_dir, "%s/rec-%04d%02d%02d-%02d%02d%02d", parent, tm->tm_year + 1900, tm->tm_mon + 1,
                 tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
        MKDIR(s_dir);
        s_lockstep = fps60 ? 1 : 2;
        snprintf(path, sizeof path, "%s/save", s_dir);
        MKDIR(path);
        {
            char a[1200], b[1200];
            snprintf(a, sizeof a, "%s/UDATA", save_dir); snprintf(b, sizeof b, "%s/save/UDATA", s_dir); copy_tree(a, b, &n);
            snprintf(a, sizeof a, "%s/TDATA", save_dir); snprintf(b, sizeof b, "%s/save/TDATA", s_dir); copy_tree(a, b, &n);
        }
        snprintf(path, sizeof path, "%s/input.pcjr", s_dir);
        s_in = fopen(path, "wb");
        if (!s_in) { fprintf(stderr, "[REC] cannot write %s\n", path); return; }
        memcpy(s_hdr.magic, REC_MAGIC, 8);
        s_hdr.version = 1; s_hdr.lockstep = (uint32_t)s_lockstep; s_hdr.row_bytes = 18;
        snprintf(s_hdr.build, sizeof s_hdr.build, "%s", CJ_BUILD_ID);
        fwrite(&s_hdr, sizeof s_hdr, 1, s_in);
        snprintf(path, sizeof path, "%s/hashes.txt", s_dir);
        s_hash = fopen(path, "w");
        {   /* env.txt: settings a replay needs to match (tools/det/replay.sh exports them) */
            FILE *ev;
            const char *m = getenv("CJ_MODS");
            snprintf(path, sizeof path, "%s/env.txt", s_dir);
            if ((ev = fopen(path, "w"))) {
                if (m) fprintf(ev, "CJ_MODS=%s\n", m);
                fclose(ev);
            }
        }
        s_mode = 1;
        s_realtime = 1;
#ifdef _WIN32
        timeBeginPeriod(1);   /* 1 ms sleeps for the frame pacing */
#endif
        fprintf(stderr, "[REC] recording to %s (%d save files copied, %d vblanks per frame)\n", s_dir, n, s_lockstep);
    } else
        return;
    cj_setenv("CJ_DET", "1");
    snprintf(buf, sizeof buf, "%d", s_lockstep);
    cj_setenv("CJ_LOCKSTEP", buf);
}

/* The state one XInputGetState poll of port 0 hands the game. */
void cj_rec_pad(uint16_t *dig, uint8_t av[8], int16_t *lx, int16_t *ly, int16_t *rx, int16_t *ry)
{
    uint8_t row[18];
    if (s_mode == 1 && s_in) {
        memcpy(row, dig, 2); memcpy(row + 2, av, 8);
        memcpy(row + 10, lx, 2); memcpy(row + 12, ly, 2); memcpy(row + 14, rx, 2); memcpy(row + 16, ry, 2);
        fwrite(row, 18, 1, s_in);
        if ((++s_rows & 255) == 0) fflush(s_in);
    } else if (s_mode == 2 && s_in) {
        /* under the dual-execution verifier (swap_verify.c) a function that polls the pad runs three
         * times: the rows its first run read are served again to the second and third, so the
         * replay's input clock does not run ahead */
        int pass = cjv_pass ? cjv_pass() : 0;
        if (pass >= 2 && s_ring_at < s_ring_n) {
            memcpy(row, s_ring[s_ring_at++], 18);
            memcpy(dig, row, 2); memcpy(av, row + 2, 8);
            memcpy(lx, row + 10, 2); memcpy(ly, row + 12, 2); memcpy(rx, row + 14, 2); memcpy(ry, row + 16, 2);
            return;
        }
        if (fread(row, 18, 1, s_in) == 1) {
            memcpy(dig, row, 2); memcpy(av, row + 2, 8);
            memcpy(lx, row + 10, 2); memcpy(ly, row + 12, 2); memcpy(rx, row + 14, 2); memcpy(ry, row + 16, 2);
            s_rows++;
            if (pass == 1 && s_ring_n < 64) memcpy(s_ring[s_ring_n++], row, 18);
        } else {
            fprintf(stderr, "[REC] replay: input ended at poll %lu (%u hash checks, %s)\n", s_rows, s_checked,
                    s_diverged ? "DIVERGED" : "all matched");
            if (cj_swap_report) cj_swap_report("end");
            fclose(s_in);
            s_in = NULL;
            if (getenv("CJ_REC_EXIT")) { fflush(stderr); exit(s_diverged ? 3 : 0); }
        }
        if (s_in == NULL) { *dig = 0; memset(av, 0, 8); *lx = *ly = *rx = *ry = 0; }
    }
}

/* After every MainUpdate under CJ_DET: real-time pacing and the heap check. */
void cj_rec_frame(unsigned frame, uint64_t heap)
{
    static uint64_t next_ns;
    if (!s_mode) return;
    if (frame % 60 == 0) {
        if (s_mode == 1 && s_hash) {
            fprintf(s_hash, "%u %016llx\n", frame, (unsigned long long)heap);
            fflush(s_hash);
        } else if (s_mode == 2 && s_hash && !s_diverged) {
            unsigned f;
            unsigned long long h;
            while (fscanf(s_hash, "%u %llx", &f, &h) == 2) {
                if (f < frame) continue;
                if (f == frame) {
                    s_checked++;
                    if (h != (unsigned long long)heap) {
                        s_diverged = frame;
                        fprintf(stderr, "[REC] DIVERGE at frame %u (poll %lu)\n", frame, s_rows);
                        if (cj_swap_report) cj_swap_report("diverge");
                        /* CJ_REC_EXIT=2: stop at the first divergence (the rest is noise) */
                        if (getenv("CJ_REC_EXIT") && getenv("CJ_REC_EXIT")[0] == '2') { fflush(stderr); exit(3); }
                    } else if (s_checked % 10 == 0)
                        fprintf(stderr, "[REC] frame %u matches (%u checks)\n", frame, s_checked);
                }
                break;
            }
        }
    }
    if (s_realtime) {   /* each frame takes lockstep/60 s of wall time; a late frame is not caught up */
        uint64_t now = xbox_HostNowNs(), step = (uint64_t)s_lockstep * 1000000000ull / 60u;
        if (!next_ns || now > next_ns + 100000000ull) next_ns = now;
        while (now < next_ns) {
            uint64_t left = next_ns - now;
#ifdef _WIN32
            if (left > 2500000ull) Sleep(1); else SwitchToThread();
#else
            if (left > 2000000ull) usleep((useconds_t)((left - 1000000ull) / 1000u));
#endif
            now = xbox_HostNowNs();
        }
        next_ns += step;
    }
}
