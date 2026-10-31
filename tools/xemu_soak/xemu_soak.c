/* xemu_soak: reference recorder for Predator: Concrete Jungle running in xemu.
 *
 * Records, on one clock (seconds since the session started):
 *   frames.csv    game state read from guest memory 20 times a second (menu,
 *                 level, mission, cutscene, player position / health / energy /
 *                 cloak / vision / weapon / moves, the game's frame time and
 *                 time scale), through xemu's QMP socket ("x" via
 *                 human-monitor-command, which reads memory while it runs)
 *   events.jsonl  state changes (menus, levels, cutscenes, damage, deaths,
 *                 actors near the player during cutscenes (actors_near, v2.2),
 *                 weapons, vision, cloak, game speed), bookmarks, stream starts
 *   input.csv     the controller as Windows sees it (XInput, ~250 Hz), one row
 *                 per change: buttons, triggers, sticks. Replayable in the PC
 *                 build (tools/xemu_input_to_script.py).
 *   audio.wav     xemu's sound only (Windows process loopback; falls back to
 *                 the whole PC's output on older Windows), 48 kHz stereo,
 *                 sample n = session time n/48000: gaps are filled with silence
 *   video/        the xemu window as 960-wide JPEGs at 15 fps, video.csv gives
 *                 each frame's session time
 *   shots/        full-size PNGs: F8, F9 (bookmark), level starts, deaths
 *   mem/          64 MB guest RAM snapshots: cutscene start +2 s, cutscene end,
 *                 level start +5 s, F9, F7 (CR3 in the memdump event)
 *   session.txt   versions, self-test result, settings; summary.txt at exit
 *
 * v3: direct memory. After the self-test the recorder finds the guest RAM block
 * inside xemu's process and reads it with ReadProcessMemory through the guest
 * page tables (checked against QMP; falls back to QMP if anything disagrees).
 * Polls at 60 Hz, actors 10 Hz in cutscenes and 1 Hz otherwise, flag-channel
 * change events, snapshots without pausing the VM, and the game's own frame
 * counter (gf) on every event and in frames.csv for lining up with the port.
 *
 * Usage: xemu_soak.exe [options] [path\to\xemu.exe] [-- extra xemu args]
 *   --attach          use an xemu already started with
 *                     -qmp tcp:127.0.0.1:4444,server=on,wait=off
 *   --hz N            state polls per second (default 20)
 *   --video-fps N     video frames per second (default 15, 0 = off)
 *   --no-audio, --system-audio, --no-input
 *   --out DIR         where session folders go (default: logs-xemu next to the
 *                     recorder, or Downloads\pcj-xemu-recordings if the
 *                     recorder is in a OneDrive folder)
 *
 * Addresses come from pcj-decomp (symbols/symbols.csv and the include headers):
 * XItemHandler_Player instance 0x0048FC98, FEMainMenu 0x0048CE58, XApp
 * 0x004931E8, XApp statics 0x00396CEC (time scale, frame rate, frame length).
 * Everything is read-only. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wincodec.h>
#include <xinput.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define PORT 4444
#define REC_RATE 48000

static char s_dir[MAX_PATH * 2];

static LARGE_INTEGER s_qpf, s_qpc0;
static PROCESS_INFORMATION s_xemu;
static int s_have_xemu;
static volatile LONG s_stop;
static CRITICAL_SECTION s_evlock;
static FILE *s_events;
static volatile uint32_t s_gf;          /* the game's frame counter (g_Stats.m_FrameCount, 0x49DB48), on every event */

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - s_qpc0.QuadPart) / (double)s_qpf.QuadPart;
}

static void ev(const char *type, const char *fmt, ...)
{
    va_list ap;
    if (!s_events) return;
    EnterCriticalSection(&s_evlock);
    fprintf(s_events, "{\"t\":%.4f,\"gf\":%u,\"type\":\"%s\"", now_s(), s_gf, type);
    if (fmt && *fmt) { fputc(',', s_events); va_start(ap, fmt); vfprintf(s_events, fmt, ap); va_end(ap); }
    fputs("}\n", s_events);
    fflush(s_events);
    LeaveCriticalSection(&s_evlock);
}

/* ======================================================================
 * QMP: one socket, used only by the state thread (after the self-test).
 * ==================================================================== */
static SOCKET s_sock = INVALID_SOCKET;
static char s_rx[1 << 20];
static int s_rxn;

static int qmp_read_obj(char *out, int cap, int timeout_ms)
{
    double end = now_s() + timeout_ms / 1000.0;
    for (;;) {
        int depth = 0, i, instr = 0, esc = 0;
        for (i = 0; i < s_rxn; i++) {
            char c = s_rx[i];
            if (instr) { if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '"') instr = 0; continue; }
            if (c == '"') instr = 1;
            else if (c == '{') depth++;
            else if (c == '}' && --depth == 0) {
                int n = i + 1, k = 0;
                while (k < n && s_rx[k] != '{') k++;
                if (n - k >= cap) return -1;
                memcpy(out, s_rx + k, n - k); out[n - k] = 0;
                memmove(s_rx, s_rx + n, s_rxn - n); s_rxn -= n;
                return n - k;
            }
        }
        {
            fd_set fs; struct timeval tv; int r;
            double left = end - now_s();
            if (left <= 0) return -1;
            FD_ZERO(&fs); FD_SET(s_sock, &fs);
            tv.tv_sec = (long)left; tv.tv_usec = (long)((left - (long)left) * 1e6);
            if (select(0, &fs, NULL, NULL, &tv) <= 0) return -1;
            if (s_rxn >= (int)sizeof s_rx - 1) return -1;
            r = recv(s_sock, s_rx + s_rxn, (int)sizeof s_rx - 1 - s_rxn, 0);
            if (r <= 0) return -1;
            s_rxn += r;
        }
    }
}

static int qmp_send(const char *json)
{
    int n = (int)strlen(json);
    return send(s_sock, json, n, 0) == n ? 0 : -1;
}

static char *hmp(const char *cmd)
{
    static char buf[1 << 18], obj[1 << 18];
    char req[512];
    if (s_sock == INVALID_SOCKET) return NULL;
    snprintf(req, sizeof req, "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"%s\"}}\r\n", cmd);
    if (qmp_send(req)) return NULL;
    for (;;) {
        int r = qmp_read_obj(obj, sizeof obj, 2000);
        char *p, *o;
        if (r < 0) return NULL;
        if (strstr(obj, "\"event\"")) continue;
        p = strstr(obj, "\"return\"");
        if (!p) return NULL;
        p = strchr(p + 8, '"');
        if (!p) { buf[0] = 0; return buf; }
        p++;
        for (o = buf; *p && *p != '"' && o < buf + sizeof buf - 1; p++) {
            if (*p == '\\' && p[1]) { p++; *o++ = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p; }
            else *o++ = *p;
        }
        *o = 0;
        return buf;
    }
}

/* n dwords of guest virtual memory at va (n <= 256) through QMP; returns the count read */
static int qmp_gread(uint32_t va, uint32_t *out, int n)
{
    char cmd[64], *r, *p;
    int k = 0;
    snprintf(cmd, sizeof cmd, "x /%dwx 0x%08x", n, va);
    if (!(r = hmp(cmd))) return 0;
    if (strstr(r, "annot access")) return 0;
    for (p = r; *p && k < n;) {
        char *colon = strchr(p, ':'), *eol;
        if (!colon) break;
        eol = strchr(colon, '\n');
        if (!eol) eol = colon + strlen(colon);
        p = colon + 1;
        while (p < eol && k < n) {
            char *e;
            unsigned long v;
            while (p < eol && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
            if (p >= eol) break;
            v = strtoul(p, &e, 16);
            if (e == p) break;
            out[k++] = (uint32_t)v;
            p = e;
        }
        p = *eol ? eol + 1 : eol;
    }
    return k;
}
/* ======================================================================
 * Direct memory (v3): the guest's RAM is one block in xemu's own address space. Found once by
 * matching physical memory read through HMP "xp" against every large committed region, then read
 * with ReadProcessMemory (microseconds, the VM keeps running). Virtual addresses go through the
 * guest's page tables (32-bit, non-PAE, 4 MB large pages; CR3 from "info registers"). Falls back to
 * QMP if anything does not match.
 * ==================================================================== */
static HANDLE s_proc;
static uint64_t s_ram;                /* host address of guest physical 0 */
static uint32_t s_ram_size, s_cr3;
static int s_dm;                      /* 1 = direct reads on */
static char s_dm_status[200] = "off";
static volatile LONG s_dm_reads, s_dm_fail;

static int pread_raw(uint32_t pa, void *out, uint32_t n)
{
    SIZE_T got = 0;
    if (!s_proc || (uint64_t)pa + n > s_ram_size) return 0;
    if (!ReadProcessMemory(s_proc, (LPCVOID)(uintptr_t)(s_ram + pa), out, n, &got) || got != n) { InterlockedIncrement(&s_dm_fail); return 0; }
    InterlockedIncrement(&s_dm_reads);
    return 1;
}

/* guest virtual -> physical, or 0xFFFFFFFF */
static uint32_t v2p(uint32_t va)
{
    uint32_t pde, pte;
    if (!pread_raw((s_cr3 & 0xFFFFF000u) + (va >> 22) * 4, &pde, 4) || !(pde & 1)) return 0xFFFFFFFFu;
    if (pde & 0x80) return (pde & 0xFFC00000u) | (va & 0x003FFFFFu);
    if (!pread_raw((pde & 0xFFFFF000u) + ((va >> 12) & 0x3FF) * 4, &pte, 4) || !(pte & 1)) return 0xFFFFFFFFu;
    return (pte & 0xFFFFF000u) | (va & 0xFFFu);
}

/* bytes of guest virtual memory, page by page; returns 1 if all were read */
static int vread(uint32_t va, void *out, uint32_t n)
{
    uint8_t *o = (uint8_t *)out;
    while (n) {
        uint32_t chunk = 0x1000u - (va & 0xFFFu), pa;
        if (chunk > n) chunk = n;
        pa = v2p(va);
        if (pa == 0xFFFFFFFFu || !pread_raw(pa, o, chunk)) return 0;
        va += chunk; o += chunk; n -= chunk;
    }
    return 1;
}

static int read_cr3(void)
{
    char *r = hmp("info registers"), *c;
    if (!r || !(c = strstr(r, "CR3="))) return 0;
    s_cr3 = (uint32_t)strtoul(c + 4, NULL, 16);
    return s_cr3 != 0;
}

/* physical dwords through HMP xp (for finding and checking the RAM block) */
static int xp_read(uint32_t pa, uint32_t *out, int n)
{
    char cmd[64], *r, *p;
    int k = 0;
    snprintf(cmd, sizeof cmd, "xp /%dwx 0x%08x", n, pa);
    if (!(r = hmp(cmd))) return 0;
    for (p = r; *p && k < n;) {
        char *colon = strchr(p, ':'), *eol;
        if (!colon) break;
        eol = strchr(colon, '\n'); if (!eol) eol = colon + strlen(colon);
        p = colon + 1;
        while (p < eol && k < n) {
            char *e; unsigned long v;
            while (p < eol && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
            if (p >= eol) break;
            v = strtoul(p, &e, 16); if (e == p) break;
            out[k++] = (uint32_t)v; p = e;
        }
        p = *eol ? eol + 1 : eol;
    }
    return k;
}

static DWORD xemu_pid(void);
static int dm_init(void)
{
    static const uint32_t probe[] = {0x00010000u, 0x00100000u, 0x00400000u, 0x01000000u, 0x01800000u, 0x02400000u, 0x03000000u, 0x03C00000u};
    uint32_t ref[8][16];
    int np, i;
    DWORD pid = xemu_pid();
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *a = 0;
    if (!pid) { snprintf(s_dm_status, sizeof s_dm_status, "off: no xemu process"); return 0; }
    if (!s_proc) s_proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!s_proc) { snprintf(s_dm_status, sizeof s_dm_status, "off: OpenProcess failed (%lu)", GetLastError()); return 0; }
    {
        uint32_t keep[8]; int kn = 0, k;
        for (i = 0; i < 8; i++) {                     /* samples with enough non-zero data to be unique */
            int nz = 0;
            if (xp_read(probe[i], ref[kn], 16) != 16) continue;
            for (k = 0; k < 16; k++) nz += ref[kn][k] != 0;
            if (nz >= 4) keep[kn++] = probe[i];
        }
        if (kn < 2) { snprintf(s_dm_status, sizeof s_dm_status, "off: guest memory too empty to match (%d samples)", kn); return 0; }
        np = kn;
        while (VirtualQueryEx(s_proc, a, &mbi, sizeof mbi) == sizeof mbi) {
            if (mbi.State == MEM_COMMIT && mbi.RegionSize >= 0x04000000u && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
                int okn = 0;
                for (i = 0; i < np; i++) {
                    uint32_t t[16]; SIZE_T got = 0;
                    if (ReadProcessMemory(s_proc, (uint8_t *)mbi.BaseAddress + keep[i], t, 64, &got) && got == 64 && !memcmp(t, ref[i], 64)) okn++;
                }
                if (okn >= 2 && okn * 4 >= np * 3) {          /* live heap may move between the two reads */
                    s_ram = (uint64_t)(uintptr_t)mbi.BaseAddress;
                    s_ram_size = mbi.RegionSize >= 0x08000000u ? 0x08000000u : 0x04000000u;
                    break;
                }
            }
            a = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
            if ((uintptr_t)a < (uintptr_t)mbi.BaseAddress) break;
        }
    }
    if (!s_ram) { snprintf(s_dm_status, sizeof s_dm_status, "off: guest RAM block not found in xemu's memory"); return 0; }
    if (!read_cr3()) { snprintf(s_dm_status, sizeof s_dm_status, "off: no CR3 from info registers"); s_ram = 0; return 0; }
    /* check: virtual reads through the page tables match QMP's */
    {
        static const uint32_t chk[] = {0x00010000u, 0x00010100u, 0x0048FC98u, 0x004931E8u, 0x0048CE58u};   /* static data */
        for (i = 0; i < 5; i++) {
            uint32_t q[8], d[8];
            if (qmp_gread(chk[i], q, 8) != 8) continue;
            if (!vread(chk[i], d, 32) || memcmp(q, d, 32)) {
                snprintf(s_dm_status, sizeof s_dm_status, "off: direct read of %08X disagrees with QMP", chk[i]);
                s_ram = 0; return 0;
            }
        }
    }
    s_dm = 1;
    snprintf(s_dm_status, sizeof s_dm_status, "on: guest RAM %u MB at host %llX, CR3 %08X", s_ram_size >> 20, (unsigned long long)s_ram, s_cr3);
    return 1;
}

/* n dwords of guest virtual memory at va: direct when on, else QMP */
static int gread(uint32_t va, uint32_t *out, int n)
{
    if (s_dm) return vread(va, out, (uint32_t)n * 4) ? n : 0;
    return qmp_gread(va, out, n);
}
static uint32_t g32(uint32_t va) { uint32_t v = 0; return gread(va, &v, 1) == 1 ? v : 0; }
static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return isfinite(f) ? f : 0.0f; }

/* Whole guest RAM (physical 0 up, 64 MB) to mem/NNN_tag.bin, so anything (POIs, AI profiles, flag
 * channels, scripts) can be read offline. With direct memory it is copied out of xemu's process in a
 * few tens of ms without pausing the VM and written by a background thread; otherwise HMP pmemsave
 * (the VM pauses for the write). The event carries CR3: the page tables are in the dump, so virtual
 * addresses translate offline. Capped at 60 dumps a session. */
static unsigned s_ndumps;
static void ev(const char *type, const char *fmt, ...);
static int vread(uint32_t va, void *out, uint32_t n);
typedef struct { char path[MAX_PATH * 2]; uint8_t *buf; uint32_t n; } DumpJob;
static DWORD WINAPI dump_writer(LPVOID arg)
{
    DumpJob *j = (DumpJob *)arg;
    FILE *f = fopen(j->path, "wb");
    if (f) { fwrite(j->buf, 1, j->n, f); fclose(f); }
    free(j->buf); free(j);
    return 0;
}
static void memdump(const char *tag)
{
    char path[MAX_PATH * 2], cmd[MAX_PATH * 2 + 64], *r, *c;
    double t0 = now_s();
    if (s_sock == INVALID_SOCKET || s_ndumps >= 60) return;
    snprintf(path, sizeof path, "%s/mem/%03u_%s.bin", s_dir, ++s_ndumps, tag);
    for (c = path; *c; c++) if (*c == '\\') *c = '/';
    if (s_dm) {
        DumpJob *j = (DumpJob *)malloc(sizeof *j);
        uint32_t off, ok = 1;
        if (j && (j->buf = (uint8_t *)malloc(s_ram_size))) {
            for (off = 0; off < s_ram_size && ok; off += 0x400000u) ok = pread_raw(off, j->buf + off, 0x400000u);
            if (ok) {
                HANDLE th;
                snprintf(j->path, sizeof j->path, "%s", path); j->n = s_ram_size;
                th = CreateThread(NULL, 0, dump_writer, j, 0, NULL);
                if (th) CloseHandle(th); else dump_writer(j);
                ev("memdump", "\"n\":%u,\"tag\":\"%s\",\"ok\":1,\"how\":\"direct\",\"ms\":%.0f,\"cr3\":\"%08X\",\"mb\":%u",
                   s_ndumps, tag, (now_s() - t0) * 1000.0, s_cr3, s_ram_size >> 20);
                return;
            }
            free(j->buf);
        }
        free(j);
    }
    if (!s_cr3) read_cr3();
    snprintf(cmd, sizeof cmd, "pmemsave 0 0x4000000 \\\"%s\\\"", path);
    r = hmp(cmd);
    ev("memdump", "\"n\":%u,\"tag\":\"%s\",\"ok\":%d,\"how\":\"pmemsave\",\"ms\":%.0f,\"cr3\":\"%08X\",\"mb\":64", s_ndumps, tag, r && !*r,
       (now_s() - t0) * 1000.0, s_cr3);
}

static int qmp_connect(int tries)
{
    struct sockaddr_in a;
    int i;
    for (i = 0; i < tries && !s_stop; i++) {
        char obj[4096];
        s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s_sock, (struct sockaddr *)&a, sizeof a) == 0) {
            int one = 1;
            setsockopt(s_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            s_rxn = 0;
            if (qmp_read_obj(obj, sizeof obj, 5000) > 0 && strstr(obj, "QMP")) {
                qmp_send("{\"execute\":\"qmp_capabilities\"}\r\n");
                if (qmp_read_obj(obj, sizeof obj, 5000) > 0) return 0;
            }
        }
        closesocket(s_sock); s_sock = INVALID_SOCKET;
        if (s_have_xemu && WaitForSingleObject(s_xemu.hProcess, 0) == WAIT_OBJECT_0) return -1;
        Sleep(1000);
    }
    return -1;
}

/* ======================================================================
 * Game state
 * ==================================================================== */
typedef struct {
    uint32_t fe, fe_sub, level, control, mission, cut, gs_depth;
    int have_player;
    float x, y, z, speed, health, energy, plasma;
    int cloak, cloak_state, vision, mode, submode, slot, weapon, hand0, hand1, attacking, attack_state, combo, holding;
    uint32_t buttons;
    float frame_len, frame_rate, game_speed;
    uint32_t gf;
    double read_ms;
} GState;

static int ok_ptr(uint32_t p) { uint32_t a = p & 0x7FFFFFFFu; return p && a >= 0x10000u && a < 0x08000000u; }

static void read_state(GState *s)
{
    uint32_t fe, app, p, w[8], pa[0x46], pc[8], fr[4];
    double t0 = now_s();
    memset(s, 0, sizeof *s);
    fe = g32(0x0048CE58u); app = g32(0x004931E8u); p = g32(0x0048FC98u);
    s->gf = s_gf = g32(0x0049DB48u);
    if (gread(0x00396CECu, fr, 4) == 4) { s->game_speed = f32(fr[0]); s->frame_rate = f32(fr[2]); s->frame_len = f32(fr[3]); }
    if (ok_ptr(fe)) {
        uint32_t m = g32(fe + 0x1C0);
        if (ok_ptr(m) && gread(m + 0x404, w, 2) == 2) { s->fe = 1 + w[0]; s->fe_sub = w[1]; }
    }
    if (ok_ptr(app)) {
        uint32_t a[0x68];
        /* XApp +0xC98 .. +0xE34 in one read: depth 0xC98, level 0xCBC, control 0xD00, mission 0xD04, cutscene 0xE30 */
        if (gread(app + 0xC98, a, 0x68) == 0x68) {
            s->gs_depth = a[0];
            s->level = a[(0xCBC - 0xC98) / 4] & 0xFF;
            s->control = a[(0xD00 - 0xC98) / 4] & 0xFF;
            s->mission = a[(0xD04 - 0xC98) / 4];
            s->cut = a[(0xE30 - 0xC98) / 4] & 0xFF;
        }
    }
    if (ok_ptr(p) && s->level) {
        /* player +0x1D4 .. +0x2E8: attack, mode, hands, weapons, position, delta */
        if (gread(p + 0x1D4, pa, 0x46) == 0x46 && gread(p + 0xC5C, pc, 8) == 8) {
#define PA(off) pa[((off) - 0x1D4) / 4]
            float dx = f32(PA(0x2D8)), dy = f32(PA(0x2DC)), dz = f32(PA(0x2E0));
            s->have_player = 1;
            s->attacking = (int8_t)(PA(0x1D4) & 0xFF);
            s->attack_state = (int)PA(0x1DC);
            s->combo = (int)PA(0x1E4);
            s->plasma = f32(PA(0x1F0));
            s->mode = (int)PA(0x20C);
            s->submode = (int)PA(0x210);
            s->hand0 = (int)PA(0x290); s->hand1 = (int)PA(0x294);
            s->slot = (int)PA(0x2AC);
            s->weapon = (s->slot >= 0 && s->slot < 4) ? (int)pa[(0x298 - 0x1D4) / 4 + s->slot] : -1;
            s->x = f32(PA(0x2C8)); s->y = f32(PA(0x2CC)); s->z = f32(PA(0x2D0));
            s->speed = sqrtf(dx * dx + dy * dy + dz * dz);
#undef PA
            s->health = f32(pc[0]); s->energy = f32(pc[1]);
            s->cloak = (int8_t)(pc[3] & 0xFF); s->cloak_state = (int)pc[4]; s->vision = pc[5] & 0xFF;
            {
                uint32_t q[0x3E];   /* +0xF4 buttons .. +0x12C holding */
                if (gread(p + 0xF4, q, 0x3E) == 0x3E) { s->buttons = q[0]; s->holding = q[(0x12C - 0xF4) / 4] & 0xFF; }
            }
        }
    }
    s->read_ms = (now_s() - t0) * 1000.0;
}

static const char *fe_name(uint32_t st)
{
    static const char *n[] = {"None","Main","New_Game","Load_Game","Loaded","Training","Story_Level_Select",
        "Ritual_Challenge_Select","View_Records","Hunting_Gear","Costume_Select","View_Trophies","Save_Game",
        "Options","Options_Controller","Options_Camera","Options_Audio","Options_Game","Autosave","EditName",
        "Credits","Story_Level_Stats","Ritual_Challenge_Times","Done"};
    return st == 0 ? "-" : st - 1 < 24 ? n[st - 1] : "?";
}
static const char *vision_name(int v)
{
    static const char *n[] = {"normal", "thermal", "neuro", "predtech"};
    return v >= 0 && v < 4 ? n[v] : "?";
}

/* ======================================================================
 * Window capture (GDI) + image encoding (WIC)
 * ==================================================================== */
static HWND s_xwin;
static BOOL CALLBACK find_xemu(HWND h, LPARAM lp)
{
    char t[256];
    DWORD pid = 0;
    (void)lp;
    if (!IsWindowVisible(h)) return TRUE;
    GetWindowThreadProcessId(h, &pid);
    GetWindowTextA(h, t, sizeof t);
    /* By process only: a title match also caught File Explorer showing a
     * folder named "xemu soak". */
    if (s_have_xemu && pid == s_xemu.dwProcessId && t[0]) { s_xwin = h; return FALSE; }
    return TRUE;
}
static HWND xemu_window(void)
{
    if (s_xwin && IsWindow(s_xwin) && IsWindowVisible(s_xwin)) return s_xwin;
    s_xwin = NULL;
    EnumWindows(find_xemu, 0);
    return s_xwin;
}

/* Grab the client area into a 24-bit top-down BGR buffer. full: native size;
 * otherwise the 4:3 picture scaled to 640x480. Returns the method used:
 * 1 PrintWindow (works when covered, reads GPU-rendered windows), 2 screen
 * copy (fallback when PrintWindow gave a black frame), 0 failure. */
static int capture(int full, uint8_t **out, int *ow, int *oh, int *ostride)
{
    RECT r;
    HWND w = xemu_window();
    HDC wdc, mdc, odc;
    HBITMAP bmp, obmp;
    int cw, ch, method = 0, tw, th, stride, x0 = 0, y0 = 0, vw, vh;
    (void)x0; (void)y0;
    uint8_t *px;
    if (!w || !GetClientRect(w, &r)) return 0;
    cw = r.right; ch = r.bottom;
    if (cw <= 16 || ch <= 16) return 0;
    wdc = GetDC(w);
    mdc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, cw, ch);
    SelectObject(mdc, bmp);
    if (PrintWindow(w, mdc, 1 | 2)) {
        /* black check on a few pixels: a GL/Vulkan window that DWM could not redirect */
        int k, lit = 0;
        for (k = 1; k < 16; k++) {
            COLORREF c = GetPixel(mdc, cw * k / 16, ch * ((k * 7) % 16) / 16);
            if (c != CLR_INVALID && (GetRValue(c) | GetGValue(c) | GetBValue(c)) > 8) lit++;
        }
        method = lit ? 1 : 0;
    }
    if (!method) {
        POINT o = {0, 0};
        HDC sdc = GetDC(NULL);
        ClientToScreen(w, &o);
        BitBlt(mdc, 0, 0, cw, ch, sdc, o.x, o.y, SRCCOPY | CAPTUREBLT);
        ReleaseDC(NULL, sdc);
        method = 2;
    }
    /* The whole client area, xemu's menu bar and letterboxing included (where
     * the game picture sits depends on xemu's display settings); video frames
     * are scaled to 960 pixels wide, which keeps a 4:3 picture in a 16:9
     * window above the game's own 640x480. */
    vw = cw; vh = ch;
    tw = full ? cw : 960; th = full ? ch : (int)((long long)ch * 960 / cw) & ~1;
    odc = CreateCompatibleDC(wdc);
    obmp = CreateCompatibleBitmap(wdc, tw, th);
    SelectObject(odc, obmp);
    SetStretchBltMode(odc, full ? COLORONCOLOR : HALFTONE);
    SetBrushOrgEx(odc, 0, 0, NULL);
    StretchBlt(odc, 0, 0, tw, th, mdc, 0, 0, vw, vh, SRCCOPY);
    stride = (tw * 3 + 3) & ~3;
    px = malloc((size_t)stride * th);
    if (px) {
        BITMAPINFOHEADER bi = {sizeof bi, tw, -th, 1, 24, BI_RGB};
        if (!GetDIBits(odc, obmp, 0, th, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS)) { free(px); px = NULL; method = 0; }
    } else method = 0;
    DeleteObject(obmp); DeleteDC(odc);
    DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(w, wdc);
    *out = px; *ow = tw; *oh = th; *ostride = stride;
    return px ? method : 0;
}

/* Write a 24-bit BGR image with WIC: jpeg (quality 0..1) or png. One factory per thread. */
static int wic_save(IWICImagingFactory *fac, const char *path, int png, float quality,
                    const uint8_t *px, int w, int h, int stride)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    WCHAR wpath[MAX_PATH * 3];
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    HRESULT hr;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 3);
    hr = IWICImagingFactory_CreateStream(fac, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, wpath, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(fac, png ? &GUID_ContainerFormatPng : &GUID_ContainerFormatJpeg, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props);
    if (SUCCEEDED(hr) && !png && props) {
        PROPBAG2 opt = {0};
        VARIANT v;
        opt.pstrName = L"ImageQuality";
        VariantInit(&v); v.vt = VT_R4; v.fltVal = quality;
        IPropertyBag2_Write(props, 1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, w, h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, h, stride, stride * h, (BYTE *)px);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    if (props) IPropertyBag2_Release(props);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    return SUCCEEDED(hr) ? 0 : -1;
}

/* Full-size PNG into shots/ (main thread: hotkeys and state events) */
static IWICImagingFactory *s_wic_main;
static unsigned s_nshots;
static void shot(const char *reason)
{
    uint8_t *px; int w, h, stride, m;
    char path[MAX_PATH * 3];
    if (!s_wic_main) return;
    m = capture(1, &px, &w, &h, &stride);
    if (!m) return;
    snprintf(path, sizeof path, "%s\\shots\\%04u_%09.3fs_%s.png", s_dir, s_nshots, now_s(), reason);
    if (!wic_save(s_wic_main, path, 1, 0, px, w, h, stride)) {
        ev("screenshot", "\"reason\":\"%s\",\"file\":\"shots/%04u_%09.3fs_%s.png\",\"w\":%d,\"h\":%d,\"method\":%d",
           reason, s_nshots, now_s(), reason, w, h, m);
        s_nshots++;
    }
    free(px);
}

/* ======================================================================
 * Video thread: 640x480 JPEG frames at a fixed rate
 * ==================================================================== */
static int s_video_fps = 15;
static volatile LONG s_video_frames, s_video_fail;
static int s_video_method;
static DWORD WINAPI video_thread(LPVOID arg)
{
    IWICImagingFactory *fac = NULL;
    FILE *idx;
    char path[MAX_PATH * 3];
    double next;
    unsigned n = 0;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac))) {
        ev("video_error", "\"what\":\"no WIC\""); CoUninitialize(); return 0;
    }
    snprintf(path, sizeof path, "%s\\video.csv", s_dir);
    idx = fopen(path, "w");
    if (idx) fputs("frame,t,capture_ms,method\n", idx);
    next = now_s();
    while (!s_stop) {
        double t = now_s(), t1;
        uint8_t *px; int w, h, stride, m;
        if (t < next) { Sleep((DWORD)((next - t) * 1000) + 1); continue; }
        next += 1.0 / s_video_fps;
        if (t > next + 0.5) next = t + 1.0 / s_video_fps;   /* fell behind (window minimised): no catch-up burst */
        m = capture(0, &px, &w, &h, &stride);
        if (!m) { InterlockedIncrement(&s_video_fail); continue; }
        snprintf(path, sizeof path, "%s\\video\\%06u.jpg", s_dir, n);
        if (!wic_save(fac, path, 0, 0.85f, px, w, h, stride)) {
            t1 = now_s();
            if (idx) { fprintf(idx, "%u,%.4f,%.1f,%d\n", n, t, (t1 - t) * 1000.0, m); if (n % 30 == 0) fflush(idx); }
            n++;
            s_video_method = m;
            InterlockedIncrement(&s_video_frames);
        }
        free(px);
    }
    if (idx) fclose(idx);
    IWICImagingFactory_Release(fac);
    CoUninitialize();
    return 0;
}

/* ======================================================================
 * Input thread: XInput, every pad, ~250 Hz, one row per change
 * ==================================================================== */
typedef DWORD (WINAPI *XIGetState)(DWORD, XINPUT_STATE *);
static volatile LONG s_input_rows;
static char s_input_status[128] = "off";
static DWORD WINAPI input_thread(LPVOID arg)
{
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    XIGetState get;
    XINPUT_STATE last[4];
    int conn[4] = {0, 0, 0, 0};
    FILE *f;
    char path[MAX_PATH * 3];
    (void)arg;
    if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
    if (!xi) xi = LoadLibraryA("xinput1_3.dll");
    get = xi ? (XIGetState)(void *)GetProcAddress(xi, "XInputGetState") : NULL;
    if (!get) { snprintf(s_input_status, sizeof s_input_status, "no XInput DLL"); return 0; }
    snprintf(path, sizeof path, "%s\\input.csv", s_dir);
    if (!(f = fopen(path, "w"))) return 0;
    fputs("t,pad,connected,buttons,lt,rt,lx,ly,rx,ry\n", f);
    memset(last, 0, sizeof last);
    snprintf(s_input_status, sizeof s_input_status, "recording (XInput)");
    timeBeginPeriod(1);
    while (!s_stop) {
        DWORD p;
        for (p = 0; p < 4; p++) {
            XINPUT_STATE st;
            int c = get(p, &st) == ERROR_SUCCESS;
            if (c != conn[p]) {
                conn[p] = c;
                fprintf(f, "%.4f,%lu,%d,0000,0,0,0,0,0,0\n", now_s(), p, c);
                ev(c ? "pad_connected" : "pad_removed", "\"pad\":%lu", p);
                memset(&last[p], 0, sizeof last[p]);
                InterlockedIncrement(&s_input_rows);
            }
            if (!c || st.dwPacketNumber == last[p].dwPacketNumber) continue;
            {
                XINPUT_GAMEPAD *a = &st.Gamepad, *b = &last[p].Gamepad;
                /* sticks: ignore jitter under 1/128 of the range unless a button or trigger moved */
                int big = a->wButtons != b->wButtons || a->bLeftTrigger != b->bLeftTrigger || a->bRightTrigger != b->bRightTrigger
                       || abs(a->sThumbLX - b->sThumbLX) > 256 || abs(a->sThumbLY - b->sThumbLY) > 256
                       || abs(a->sThumbRX - b->sThumbRX) > 256 || abs(a->sThumbRY - b->sThumbRY) > 256;
                if (!big) continue;
                fprintf(f, "%.4f,%lu,1,%04X,%u,%u,%d,%d,%d,%d\n", now_s(), p, a->wButtons, a->bLeftTrigger,
                        a->bRightTrigger, a->sThumbLX, a->sThumbLY, a->sThumbRX, a->sThumbRY);
                last[p] = st;
                InterlockedIncrement(&s_input_rows);
            }
        }
        if (s_input_rows % 64 == 0) fflush(f);
        Sleep(4);
    }
    timeEndPeriod(1);
    fclose(f);
    return 0;
}

/* ======================================================================
 * Audio thread: process loopback of xemu (Windows 10 2004+), else the
 * default output's loopback. Written so that sample n is session time
 * n / REC_RATE: silence where nothing was playing.
 * ==================================================================== */
DEFINE_GUID(IID_IActivateCompl_, 0x41D949AB, 0x9862, 0x444A, 0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB);
DEFINE_GUID(IID_IAgileObject_, 0x94EA2B94, 0xE9CC, 0x49E0, 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90);

typedef struct { DWORD TargetProcessId; DWORD ProcessLoopbackMode; } PROC_LB_PARAMS;
typedef struct { DWORD ActivationType; PROC_LB_PARAMS p; } ACT_PARAMS;

typedef struct IActOp IActOp;
typedef struct { HRESULT (STDMETHODCALLTYPE *QI)(IActOp *, REFIID, void **); ULONG (STDMETHODCALLTYPE *AddRef)(IActOp *);
                 ULONG (STDMETHODCALLTYPE *Release)(IActOp *);
                 HRESULT (STDMETHODCALLTYPE *GetActivateResult)(IActOp *, HRESULT *, IUnknown **); } IActOpVtbl;
struct IActOp { const IActOpVtbl *lpVtbl; };

typedef struct Handler Handler;
typedef struct { HRESULT (STDMETHODCALLTYPE *QI)(Handler *, REFIID, void **); ULONG (STDMETHODCALLTYPE *AddRef)(Handler *);
                 ULONG (STDMETHODCALLTYPE *Release)(Handler *);
                 HRESULT (STDMETHODCALLTYPE *ActivateCompleted)(Handler *, IActOp *); } HandlerVtbl;
struct Handler { const HandlerVtbl *lpVtbl; volatile LONG refs; HANDLE done; HRESULT hr; IAudioClient *ac; };

static HRESULT STDMETHODCALLTYPE h_qi(Handler *h, REFIID id, void **out)
{
    if (IsEqualIID(id, &IID_IUnknown) || IsEqualIID(id, &IID_IActivateCompl_) || IsEqualIID(id, &IID_IAgileObject_)) {
        *out = h; InterlockedIncrement(&h->refs); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_addref(Handler *h) { return (ULONG)InterlockedIncrement(&h->refs); }
static ULONG STDMETHODCALLTYPE h_release(Handler *h) { return (ULONG)InterlockedDecrement(&h->refs); }   /* static object */
static HRESULT STDMETHODCALLTYPE h_done(Handler *h, IActOp *op)
{
    IUnknown *u = NULL;
    HRESULT ar = E_FAIL;
    h->hr = op->lpVtbl->GetActivateResult(op, &ar, &u);
    if (SUCCEEDED(h->hr)) h->hr = ar;
    if (SUCCEEDED(h->hr) && u) h->hr = IUnknown_QueryInterface(u, &IID_IAudioClient, (void **)&h->ac);
    if (u) IUnknown_Release(u);
    SetEvent(h->done);
    return S_OK;
}
static const HandlerVtbl s_hvt = {h_qi, h_addref, h_release, h_done};

typedef HRESULT (WINAPI *ActivateAsyncFn)(LPCWSTR, REFIID, PROPVARIANT *, void *, IActOp **);

static char s_audio_status[200] = "off";
static double s_audio_t0 = -1;
static unsigned long long s_audio_frames, s_audio_silence;
static volatile LONG s_audio_peak;

/* Process loopback client for xemu's process tree, initialised for 48 kHz stereo 16-bit. */
static IAudioClient *process_loopback(DWORD pid, HANDLE ev_buf)
{
    static Handler h;
    ACT_PARAMS ap;
    PROPVARIANT pv;
    IActOp *op = NULL;
    HMODULE m = LoadLibraryA("mmdevapi.dll");
    ActivateAsyncFn fn = m ? (ActivateAsyncFn)(void *)GetProcAddress(m, "ActivateAudioInterfaceAsync") : NULL;
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, REC_RATE, REC_RATE * 4, 4, 16, 0};
    if (!fn || !pid) return NULL;
    memset(&h, 0, sizeof h);
    h.lpVtbl = &s_hvt; h.refs = 1; h.done = CreateEventA(NULL, TRUE, FALSE, NULL);
    ap.ActivationType = 1;                 /* AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK */
    ap.p.TargetProcessId = pid;
    ap.p.ProcessLoopbackMode = 0;          /* include the target process tree */
    PropVariantInit(&pv);
    pv.vt = VT_BLOB; pv.blob.cbSize = sizeof ap; pv.blob.pBlobData = (BYTE *)&ap;
    if (FAILED(fn(L"VAD\\Process_Loopback", &IID_IAudioClient, &pv, &h, &op))) return NULL;
    if (WaitForSingleObject(h.done, 5000) != WAIT_OBJECT_0 || FAILED(h.hr) || !h.ac) { if (op) op->lpVtbl->Release(op); return NULL; }
    if (op) op->lpVtbl->Release(op);
    if (FAILED(IAudioClient_Initialize(h.ac, AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                       | 0x80000000 /* AUTOCONVERTPCM */ | 0x08000000 /* SRC_DEFAULT_QUALITY */,
                                       2000000, 0, &wf, NULL)) ||
        FAILED(IAudioClient_SetEventHandle(h.ac, ev_buf))) {
        IAudioClient_Release(h.ac);
        return NULL;
    }
    return h.ac;
}

/* System loopback of the default render device. Tries 48 kHz stereo 16-bit
 * with WASAPI's conversion first, then the device's own mix format (converted
 * here). Each attempt must also hand out its capture service. */
static char s_audio_why[160];
static IAudioClient *system_loopback(int *is_float, int *channels, unsigned *rate, IAudioCaptureClient **cc)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    WAVEFORMATEX *mix = NULL;
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, REC_RATE, REC_RATE * 4, 4, 16, 0};
    int attempt;
    *cc = NULL;
    HRESULT hr;
    if (FAILED(hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en))) {
        snprintf(s_audio_why, sizeof s_audio_why, "device enumerator 0x%08lX", hr); return NULL;
    }
    if (FAILED(hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
        snprintf(s_audio_why, sizeof s_audio_why, "no default output device 0x%08lX", hr); IMMDeviceEnumerator_Release(en); return NULL;
    }
    for (attempt = 0; attempt < 2 && !*cc; attempt++) {
        if (ac) { IAudioClient_Release(ac); ac = NULL; }
        if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac))) break;
        if (attempt == 0) {
            if (FAILED(hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | 0x80000000 | 0x08000000,
                                               2000000, 0, &wf, NULL))) { snprintf(s_audio_why, sizeof s_audio_why, "init 48k 0x%08lX", hr); continue; }
            *is_float = 0; *channels = 2; *rate = REC_RATE;
        } else {
            if (FAILED(IAudioClient_GetMixFormat(ac, &mix))) continue;
            *channels = mix->nChannels; *rate = mix->nSamplesPerSec;
            *is_float = mix->wBitsPerSample == 32;
            if (FAILED(hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, NULL))) {
                snprintf(s_audio_why + strlen(s_audio_why), sizeof s_audio_why - strlen(s_audio_why), ", init mix 0x%08lX", hr);
                CoTaskMemFree(mix); mix = NULL; continue;
            }
            CoTaskMemFree(mix); mix = NULL;
        }
        if (FAILED(hr = IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)cc))) {
            *cc = NULL;
            snprintf(s_audio_why + strlen(s_audio_why), sizeof s_audio_why - strlen(s_audio_why), ", capture service 0x%08lX", hr);
        }
    }
    if (!*cc && ac) { IAudioClient_Release(ac); ac = NULL; }
    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    return ac;
}

static void wav_header(FILE *f, unsigned long long frames)
{
    uint32_t data = (uint32_t)(frames * 4), riff = 36 + data, n16 = 16, rate = REC_RATE, br = REC_RATE * 4;
    uint16_t a[4] = {1, 2, 4, 16};
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&n16, 4, 1, f);
    fwrite(&a[0], 2, 1, f); fwrite(&a[1], 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&a[2], 2, 1, f); fwrite(&a[3], 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

static void wav_silence(FILE *f, unsigned long long n)
{
    static const int16_t z[2 * 512];
    while (n) { unsigned long long k = n > 512 ? 512 : n; fwrite(z, 4, (size_t)k, f); n -= k; s_audio_frames += k; s_audio_silence += k; }
}

static int s_system_audio;
static DWORD WINAPI audio_thread(LPVOID arg)
{
    HANDLE evb = CreateEventA(NULL, FALSE, FALSE, NULL);
    IAudioClient *ac = NULL;
    IAudioCaptureClient *cc = NULL;
    FILE *f = NULL;
    int is_float = 0, ch = 2, process = 0;
    unsigned rate = REC_RATE;
    char path[MAX_PATH * 3];
    double resample_pos = 0;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!s_system_audio && s_have_xemu && (ac = process_loopback(s_xemu.dwProcessId, evb))) {
        if (SUCCEEDED(IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)&cc))) process = 1;
        else { IAudioClient_Release(ac); ac = NULL; }
    }
    if (!ac) ac = system_loopback(&is_float, &ch, &rate, &cc);
    if (!ac || !cc) { snprintf(s_audio_status, sizeof s_audio_status, "FAILED: Windows offered no audio capture (loopback: %s)", s_audio_why); goto out; }
    snprintf(path, sizeof path, "%s\\audio.wav", s_dir);
    if (!(f = fopen(path, "wb"))) { snprintf(s_audio_status, sizeof s_audio_status, "FAILED: cannot write audio.wav"); goto out; }
    wav_header(f, 0);
    if (FAILED(IAudioClient_Start(ac))) { snprintf(s_audio_status, sizeof s_audio_status, "FAILED: capture would not start"); goto out; }
    snprintf(s_audio_status, sizeof s_audio_status, "%s, device format %u Hz %d ch %s, written as 48 kHz stereo 16-bit",
             process ? "xemu's sound only (process loopback)" : "whole PC output (system loopback: pause other audio)",
             rate, ch, is_float ? "float" : "16-bit");
    ev("audio_start", "\"mode\":\"%s\",\"device_rate\":%u", process ? "process" : "system", rate);
    while (!s_stop) {
        UINT32 n = 0;
        if (process) WaitForSingleObject(evb, 50); else Sleep(10);
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cc, &n)) && n) {
            BYTE *data; UINT32 frames, i; DWORD flags; UINT64 qpc = 0;
            double t_first;
            if (FAILED(IAudioCaptureClient_GetBuffer(cc, &data, &frames, &flags, NULL, &qpc))) break;
            /* qpc is in 100 ns units of the performance counter's time base */
            t_first = qpc ? (double)qpc * 1e-7 - (double)s_qpc0.QuadPart / (double)s_qpf.QuadPart : now_s() - (double)frames / rate;
            if (s_audio_t0 < 0) s_audio_t0 = t_first;
            {
                long long want = (long long)(t_first * REC_RATE);
                if (want > (long long)s_audio_frames + REC_RATE / 50)          /* > 20 ms hole: nothing was playing */
                    wav_silence(f, (unsigned long long)(want - (long long)s_audio_frames));
            }
            if (rate == REC_RATE && !is_float && ch == 2) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) wav_silence(f, frames);
                else {
                    for (i = 0; i < frames * 2; i++) {
                        int v = abs(((int16_t *)data)[i]);
                        if (v > s_audio_peak) s_audio_peak = v;
                    }
                    fwrite(data, 4, frames, f);
                    s_audio_frames += frames;
                }
            } else {
                /* convert: any channel count, float or 16-bit, any rate (linear resampling) */
                double step = (double)rate / REC_RATE;
                for (; resample_pos < frames; resample_pos += step) {
                    UINT32 k = (UINT32)resample_pos;
                    int16_t o[2];
                    int c;
                    for (c = 0; c < 2; c++) {
                        int src = c < ch ? c : 0;
                        float x = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f
                                : is_float ? ((float *)data)[(size_t)k * ch + src]
                                           : ((int16_t *)data)[(size_t)k * ch + src] / 32768.0f;
                        int v = (int)lrintf(x * 32767.0f);
                        if (v > 32767) v = 32767;
                        if (v < -32768) v = -32768;
                        if (abs(v) > s_audio_peak) s_audio_peak = abs(v);
                        o[c] = (int16_t)v;
                    }
                    fwrite(o, 4, 1, f);
                    s_audio_frames++;
                }
                resample_pos -= frames;
            }
            IAudioCaptureClient_ReleaseBuffer(cc, frames);
        }
        {   /* keep the header current, so a killed recorder still leaves a playable file */
            static double last_hdr;
            if (now_s() - last_hdr > 10) { last_hdr = now_s(); wav_header(f, s_audio_frames); fflush(f); }
        }
    }
    IAudioClient_Stop(ac);
    wav_header(f, s_audio_frames);
out:
    if (f) fclose(f);
    if (cc) IAudioCaptureClient_Release(cc);
    if (ac) IAudioClient_Release(ac);
    CoUninitialize();
    return 0;
}

/* ======================================================================
 * Finding and starting xemu
 * ==================================================================== */
static int file_exists(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static int search_dir(const char *dir, int depth, char *out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\xemu.exe", dir);
    if (file_exists(pat)) { strcpy(out, pat); return 1; }
    if (depth <= 0) return 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            char sub[MAX_PATH * 2];
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            if (search_dir(sub, depth - 1, out)) { FindClose(h); return 1; }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

static int find_xemu_exe(char *out)
{
    char base[MAX_PATH], buf[MAX_PATH * 2], *sl;
    const char *env[] = {"USERPROFILE", "LOCALAPPDATA", "ProgramFiles", "ProgramFiles(x86)", NULL};
    int i;
    GetModuleFileNameA(NULL, base, sizeof base);
    if ((sl = strrchr(base, '\\'))) *sl = 0;
    if (search_dir(base, 1, out)) return 1;
    if ((sl = strrchr(base, '\\'))) { *sl = 0; if (search_dir(base, 2, out)) return 1; }
    for (i = 0; env[i]; i++) {
        const char *e = getenv(env[i]);
        if (!e) continue;
        if (i == 0) {
            const char *subs[] = {"Desktop", "OneDrive\\Desktop", "Downloads", "Documents", "OneDrive\\Documents", NULL};
            int k;
            for (k = 0; subs[k]; k++) {
                snprintf(buf, sizeof buf, "%s\\%s", e, subs[k]);
                if (search_dir(buf, 2, out)) return 1;
            }
        } else if (search_dir(e, 2, out)) return 1;
    }
    return 0;
}

/* A running xemu.exe, by process name (a window-title check also matched
 * File Explorer showing the "xemu soak" folder). 0 if none. */
static DWORD xemu_running_pid(char *exe_out, size_t n)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    DWORD pid = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe))
        do {
            if (!_stricmp(pe.szExeFile, "xemu.exe")) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    if (pid && exe_out) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        DWORD len = (DWORD)n;
        exe_out[0] = 0;
        if (h) { if (!QueryFullProcessImageNameA(h, 0, exe_out, &len)) exe_out[0] = 0; CloseHandle(h); }
    }
    return pid;
}

static DWORD xemu_pid(void)
{
    if (s_have_xemu && s_xemu.dwProcessId) return s_xemu.dwProcessId;
    return xemu_running_pid(NULL, 0);
}

static BOOL CALLBACK close_pid_windows(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == (DWORD)lp && IsWindowVisible(h)) PostMessageA(h, WM_CLOSE, 0, 0);
    return TRUE;
}

/* Ask xemu to close (its windows get WM_CLOSE); force it after 8 seconds. */
static void close_xemu(DWORD pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    EnumWindows(close_pid_windows, (LPARAM)pid);
    if (h) {
        if (WaitForSingleObject(h, 8000) != WAIT_OBJECT_0) TerminateProcess(h, 0);
        WaitForSingleObject(h, 3000);
        CloseHandle(h);
    }
    Sleep(500);
}

/* Where xemu.exe was found last time (next to the recorder). */
static void remember_xemu(const char *exe_dir, const char *xemu)
{
    char p[MAX_PATH * 2];
    FILE *f;
    snprintf(p, sizeof p, "%s\\xemu-path.txt", exe_dir);
    if ((f = fopen(p, "w"))) { fprintf(f, "%s\n", xemu); fclose(f); }
}
static int recall_xemu(const char *exe_dir, char *out, size_t n)
{
    char p[MAX_PATH * 2];
    FILE *f;
    snprintf(p, sizeof p, "%s\\xemu-path.txt", exe_dir);
    out[0] = 0;
    if ((f = fopen(p, "r"))) {
        if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
        fclose(f);
    }
    return out[0] && file_exists(out);
}


/* Actor log (v2.2): during cutscenes and 8 s after, twice a second, every actor within 30 m of the
 * player's item: slot, distance, health, brain state (StateMachine::m_State), state stack depth,
 * anim mode/type, state flags (+0x93C: bit 20 hidden, bit 21 player seen), flags2, ref id, profile
 * index; plus the player item position and hidden flag. Same fields as the PC build's actors_near
 * events (src/gamelog.c), so the original and the port can be compared scene by scene. */
/* Script flag channels (FlagChannels, 0x48FA00, 290 bytes): one event per change, same fields as the
 * port's gamelog flagch event. */
static void log_flagch(const GState *s)
{
    static uint8_t last[292]; static int have;
    uint32_t cur[73]; const uint8_t *c = (const uint8_t *)cur; int k;
    if (gread(0x0048FA00u, cur, 73) != 73) return;
    if (have)
        for (k = 0; k < 290; k++)
            if (c[k] != last[k]) ev("flagch", "\"ch\":%d,\"v\":%u,\"was\":%u,\"cut\":%u", k, c[k], last[k], s->cut);
    memcpy(last, c, 292); have = 1;
}

/* Actors near the player. During cutscenes and 8 s after: 10 times a second with direct memory (2
 * with QMP); otherwise once a second with direct memory. Fields as in v2.2 plus (direct only) the
 * pushed states (stks), watch flags (w), the POI a GotoLocation heads for (poi, same text as the
 * port's gamelog) and the profile's flag-channel reactions (fc: "channel=state", * channel set,
 * ! fired). */
static void log_actors(const GState *s, double t)
{
    static double last_t = -10.0, cut_end = -100.0;
    static int was_cut;
    uint32_t p, it, tab[128 * 6], ipos[3], hid, k;
    float px, py, pz, every, radius;
    static char buf[65536]; int bl = 0, n = 0;
    int hot;
    if (was_cut && !s->cut) cut_end = t;
    was_cut = s->cut;
    hot = s->cut || t - cut_end < 8.0;
    if (!hot && !s_dm) return;
    every = hot ? (s_dm ? 0.1f : 0.5f) : 1.0f;
    radius = hot ? 30.0f : 40.0f;
    if (t - last_t < every) return;
    last_t = t;
    p = g32(0x0048FC98u);
    if (!ok_ptr(p)) return;
    it = g32(p + 4);
    if (!ok_ptr(it) || gread(it + 0xC8, ipos, 3) != 3) return;
    hid = g32(p + 0xD8C) & 0xFF;
    px = f32(ipos[0]); py = f32(ipos[1]); pz = f32(ipos[2]);
    buf[0] = 0;
    if (s_dm) { if (gread(0x0047C3F0u, tab, 128 * 6) != 128 * 6) return; }
    else {
        uint32_t base;
        for (base = 0; base < 128; base += 42) {
            uint32_t cnt = (128 - base < 42 ? 128 - base : 42);
            if (gread(0x0047C3F0u + base * 0x18u, tab + base * 6, (int)(cnt * 6)) != (int)(cnt * 6)) return;
        }
    }
    for (k = 0; k < 128 && bl < (int)sizeof buf - 1200; k++) {
        uint32_t *e = tab + k * 6, a = e[4], st[1], r[13], hp[1], fl[3];
        float dx = f32(e[0]) - px, dy = f32(e[1]) - py, dz = f32(e[2]) - pz, d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!ok_ptr(a) || d > radius) continue;
        if (gread(a + 0x140, st, 1) != 1 || gread(a + 0x770, r, 13) != 13 || gread(a + 0x7DC, hp, 1) != 1 || gread(a + 0x93C, fl, 3) != 3) continue;
        bl += snprintf(buf + bl, sizeof buf - bl,
                       "%s{\"id\":%u,\"d\":%.1f,\"pos\":[%.2f,%.2f,%.2f],\"hp\":%.0f,\"st\":\"%08X\",\"stk\":%u,"
                       "\"am\":\"%08X\",\"at\":\"%08X\",\"amn\":\"%08X\",\"sf\":\"%08X\",\"f2\":\"%08X\",\"ref\":\"%X\",\"prof\":\"%08X\"",
                       n++ ? "," : "", k, d, f32(e[0]), f32(e[1]), f32(e[2]), f32(hp[0]), st[0], r[6],
                       r[9], r[10], r[12], fl[0], fl[2], r[0], r[2]);
        if (s_dm) {
            uint32_t w = 0, j, nst = r[6] > 10 ? 10 : r[6], fc[10], fired[2], sm4[1], prt, row;
            char tmp[400]; int tl = 0;
            gread(a + 0x7EC, &w, 1);
            bl += snprintf(buf + bl, sizeof buf - bl, ",\"w\":\"%X\",\"stks\":\"", w);
            for (j = 0; j < nst; j++) { uint32_t sf2 = 0; gread(a + 0x188 + j * 0x80 + 0x70, &sf2, 1); bl += snprintf(buf + bl, sizeof buf - bl, "%s%X", j ? "," : "", sf2); }
            bl += snprintf(buf + bl, sizeof buf - bl, "\"");
            /* profile row (AP tables at 0x4816CC, rows of 0x14C): flag-channel reactions at +0xAC */
            row = r[2] >> 16;
            if ((r[2] & 0xFFFF) != 0xFFFF && row != 0xFFFF && (prt = g32(0x004816CCu + (r[2] & 0xFFFF) * 4)) && gread(prt + row * 0x14C + 0xAC, fc, 10) == 10
                && gread(a + 0x948, fired, 2) == 2) {
                const uint8_t *fb = (const uint8_t *)fired;
                for (j = 0; j < 5; j++) if (fc[j * 2]) {
                    uint32_t chv = 0; gread(0x0048FA00u + (fc[j * 2] & ~3u), &chv, 1);
                    tl += snprintf(tmp + tl, sizeof tmp - tl, "%s%u=%X%s%s", tl ? " " : "", fc[j * 2], fc[j * 2 + 1],
                                   ((chv >> ((fc[j * 2] & 3) * 8)) & 0xFF) ? "*" : "", fb[j] ? "!" : "");
                }
                tmp[tl] = 0;
                bl += snprintf(buf + bl, sizeof buf - bl, ",\"fc\":\"%s\"", tmp);
            }
            /* GotoLocation (0x88F90): the POI it heads for (s_POIs 0x3968B4, 0x2EC each) */
            if (st[0] == 0x00088F90u && gread(a + 0xE8 + 4, sm4, 1) == 1) {
                uint32_t tabp = g32(0x003968B4u), np = g32(0x00395758u), pp[0x2EC / 4];
                if (tabp && sm4[0] > 0 && sm4[0] < np && gread(tabp + sm4[0] * 0x2EC, pp, 0x2EC / 4) == 0x2EC / 4)
                    bl += snprintf(buf + bl, sizeof buf - bl, ",\"poi\":\"%u en%u nv%u b%X pos %.1f,%.1f,%.1f vs %X cs %X fc %X fo %X ty %X\"", sm4[0],
                                   pp[0x2A0 / 4], pp[0x254 / 4], pp[0x20 / 4], f32(pp[0x220 / 4]), f32(pp[0x224 / 4]), f32(pp[0x228 / 4]),
                                   pp[0x298 / 4], pp[0x29C / 4], pp[0x28C / 4], pp[0x294 / 4], pp[0x264 / 4]);
            }
        }
        bl += snprintf(buf + bl, sizeof buf - bl, "}");
    }
    ev("actors_near", "\"cut\":%u,\"since_end\":%.1f,\"item\":[%.2f,%.2f,%.2f],\"hidden\":%u,\"n\":%d,\"list\":[%s]",
       s->cut, s->cut ? -1.0 : t - cut_end, px, py, pz, hid, n, buf);
}

/* ======================================================================
 * Main: start xemu, self-test, run the recorders, poll state
 * ==================================================================== */
int main(int argc, char **argv)
{
    WSADATA wd;
    char xemu[MAX_PATH * 2] = "", exe_dir[MAX_PATH], *sl, path[MAX_PATH * 3], selftest[256] = "not run";
    int attach = 0, i, poll_hz = 0, no_audio = 0, no_input = 0;
    char out_dir[MAX_PATH * 2] = "";
    GState s, last;
    int have_last = 0;
    double next = 0, level_t0 = 0, last_status = 0, read_ms_sum = 0, read_ms_max = 0;
    unsigned long long polls = 0;
    unsigned deaths = 0, levels = 0, bookmarks = 0, damage_n = 0;
    double dump_at = 0, level_dump_at = 0;
    double damage = 0, t_vision[8] = {0}, t_level = 0;
    FILE *frames;
    HANDLE th_video = NULL, th_input = NULL, th_audio = NULL;

    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_qpc0);
    InitializeCriticalSection(&s_evlock);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--attach")) attach = 1;
        else if (!strcmp(argv[i], "--hz") && i + 1 < argc) poll_hz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--video-fps") && i + 1 < argc) s_video_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-audio")) no_audio = 1;
        else if (!strcmp(argv[i], "--system-audio")) s_system_audio = 1;
        else if (!strcmp(argv[i], "--no-input")) no_input = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) snprintf(out_dir, sizeof out_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--")) break;
        else snprintf(xemu, sizeof xemu, "%s", argv[i]);
    }
    if (poll_hz > 60) poll_hz = 60;          /* 0 = default: 60 with direct memory, 20 through QMP (set after the self-test) */
    if (s_video_fps > 30) s_video_fps = 30;
    WSAStartup(MAKEWORD(2, 2), &wd);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();   /* real pixel sizes for the window capture */
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
    if ((sl = strrchr(exe_dir, '\\'))) *sl = 0;

    printf("xemu reference recorder for Predator: Concrete Jungle\n\n");
    if (!attach) {
        char running[MAX_PATH * 2];
        DWORD pid = xemu_running_pid(running, sizeof running);
        if (pid) {
            /* Already open: record it if it was started with the control
             * socket (a recorder run earlier), else restart it with it. */
            printf("xemu is already open; checking whether it can be recorded...\n");
            if (qmp_connect(2) == 0) {
                s_xemu.dwProcessId = pid;
                s_xemu.hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                s_have_xemu = s_xemu.hProcess != NULL;
                attach = 2;
                if (running[0]) { snprintf(xemu, sizeof xemu, "%s", running); remember_xemu(exe_dir, xemu); }
                printf("Recording the xemu that is already open.\n");
            } else {
                if (MessageBoxA(NULL, "xemu is open, but it has to be started by the recorder to be recorded.\n\n"
                                      "Click OK to close xemu and open it again for recording.\n"
                                      "(Anything unsaved in xemu right now is lost.)",
                                "xemu recorder", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
                    return 1;
                if (running[0] && !*xemu) snprintf(xemu, sizeof xemu, "%s", running);
                printf("Closing xemu...\n");
                close_xemu(pid);
            }
        }
    }
    if (!attach) {
        char cmd[MAX_PATH * 4];
        STARTUPINFOA si;
        if (!*xemu && !recall_xemu(exe_dir, xemu, sizeof xemu) && !find_xemu_exe(xemu)) {
            MessageBoxA(NULL, "Could not find xemu.exe.\n\nOpen xemu the normal way, then run XEMU-SOAK.bat again and click OK. "
                              "It remembers where xemu is after that.",
                        "xemu recorder", MB_ICONERROR);
            return 1;
        }
        remember_xemu(exe_dir, xemu);
        snprintf(cmd, sizeof cmd, "\"%s\" -qmp tcp:127.0.0.1:%d,server=on,wait=off", xemu, PORT);
        for (i = 1; i < argc; i++) if (!strcmp(argv[i], "--")) { int k; for (k = i + 1; k < argc; k++) { strcat(cmd, " "); strcat(cmd, argv[k]); } break; }
        memset(&si, 0, sizeof si); si.cb = sizeof si;
        {
            char xdir[MAX_PATH * 2];
            snprintf(xdir, sizeof xdir, "%s", xemu);
            if ((sl = strrchr(xdir, '\\'))) *sl = 0;
            if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, xdir, &si, &s_xemu)) {
                MessageBoxA(NULL, "Could not start xemu.", "xemu recorder", MB_ICONERROR);
                return 1;
            }
        }
        s_have_xemu = 1;
        printf("Started: %s\n", cmd);
    }
    if (attach != 2) printf("Connecting to xemu's control socket (port %d)...\n", PORT);
    if (attach != 2 && qmp_connect(60)) {
        MessageBoxA(NULL, "Could not connect to xemu's control socket (QMP on port 4444).\n\n"
                          "If you started xemu yourself, add  -qmp tcp:127.0.0.1:4444,server=on,wait=off  to its command line.",
                    "xemu recorder", MB_ICONERROR);
        return 1;
    }
    printf("Connected.\n");
    if (!s_have_xemu) {   /* --attach: find xemu's process for the window and audio */
        DWORD pid = xemu_running_pid(NULL, 0);
        if (pid && (s_xemu.hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) {
            s_xemu.dwProcessId = pid;
            s_have_xemu = 1;
        }
    }

    /* session folder */
    {
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);
        /* Recordings are large (video): keep them out of OneDrive. Default:
         * logs-xemu next to the recorder, or Downloads\pcj-xemu-recordings
         * when the recorder sits in a OneDrive folder. --out overrides. */
        if (out_dir[0]) snprintf(path, sizeof path, "%s", out_dir);
        else if (strstr(exe_dir, "OneDrive") && getenv("USERPROFILE"))
            snprintf(path, sizeof path, "%s\\Downloads\\pcj-xemu-recordings", getenv("USERPROFILE"));
        else snprintf(path, sizeof path, "%s\\logs-xemu", exe_dir);
        CreateDirectoryA(path, NULL);
        snprintf(s_dir, sizeof s_dir, "%s\\session-%04d%02d%02d-%02d%02d%02d", path, lt->tm_year + 1900, lt->tm_mon + 1,
                 lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
        CreateDirectoryA(s_dir, NULL);
        snprintf(path, sizeof path, "%s\\shots", s_dir); CreateDirectoryA(path, NULL);
        snprintf(path, sizeof path, "%s\\mem", s_dir); CreateDirectoryA(path, NULL);
        snprintf(path, sizeof path, "%s\\video", s_dir); if (s_video_fps > 0) CreateDirectoryA(path, NULL);
    }
    snprintf(path, sizeof path, "%s\\events.jsonl", s_dir); s_events = fopen(path, "w");
    snprintf(path, sizeof path, "%s\\frames.csv", s_dir); frames = fopen(path, "w");
    if (!frames || !s_events) { MessageBoxA(NULL, "Cannot write the log folder.", "xemu recorder", MB_ICONERROR); return 1; }
    ev("session_start", "\"source\":\"xemu\"");
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&s_wic_main);

    /* streams that do not need the game: start now, so the boot is recorded too */
    if (!no_input) th_input = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
    if (!no_audio) th_audio = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    if (s_video_fps > 0) th_video = CreateThread(NULL, 0, video_thread, NULL, 0, NULL);

    /* Self-test: the XBE header the kernel maps at 0x10000 ("XBEH") and the
     * certificate's title ID and name prove that memory reads work and that
     * the disc is this game. */
    {
        double tw = now_s();
        int said = 0;
        printf("Self-test: waiting for the game to load (start it in xemu if it is not set to auto-boot)...\n");
        for (;;) {
            uint32_t h0 = 0;
            if (gread(0x00010000u, &h0, 1) == 1 && h0 == 0x48454258u) {
                uint32_t cert = g32(0x00010118u), c[22];
                if (ok_ptr(cert) && gread(cert + 8, c, 22) == 22) {
                    int k;
                    char nm[41];
                    for (k = 0; k < 40; k++) {
                        uint16_t u = (uint16_t)(c[1 + k / 2] >> ((k & 1) * 16));
                        if (!u) break;
                        nm[k] = (u < 128) ? (char)u : '?';
                    }
                    nm[k] = 0;
                    if (c[0] == 0x56550030u) {
                        snprintf(selftest, sizeof selftest, "PASS: memory reads work, running title 56550030 \"%s\" (Predator: Concrete Jungle)", nm);
                        break;
                    }
                    if (!said) { printf("  xemu is running title %08X \"%s\", waiting for Predator: Concrete Jungle...\n", c[0], nm); said = 1; }
                }
            }
            if (s_have_xemu && WaitForSingleObject(s_xemu.hProcess, 0) == WAIT_OBJECT_0) {
                snprintf(selftest, sizeof selftest, "FAILED: xemu closed before the game loaded"); break;
            }
            if (now_s() - tw > 600) {
                snprintf(selftest, sizeof selftest, "FAILED: no Predator: Concrete Jungle XBE header in guest memory after 10 minutes"); break;
            }
            Sleep(500);
        }
        printf("Self-test: %s\n", selftest);
        ev("selftest", "\"result\":\"%s\"", strncmp(selftest, "PASS", 4) ? "fail" : "pass");
        if (!strncmp(selftest, "PASS", 4)) {
            int tries;
            for (tries = 0; tries < 5 && !dm_init(); tries++) Sleep(1000);
            printf("Direct memory: %s\n", s_dm_status);
            if (poll_hz < 1) poll_hz = s_dm ? 60 : 20;
            ev("direct_memory", "\"on\":%d,\"status\":\"%s\",\"cr3\":\"%08X\",\"ram_mb\":%u", s_dm, s_dm_status, s_cr3, s_ram_size >> 20);
        }
    }
    {
        FILE *f;
        char *ver = hmp("info version");
        snprintf(path, sizeof path, "%s\\session.txt", s_dir);
        if ((f = fopen(path, "w"))) {
            time_t t = time(NULL);
            OSVERSIONINFOA vi;
            HMODULE nt = GetModuleHandleA("ntdll.dll");
            LONG (WINAPI *rgv)(OSVERSIONINFOA *) = nt ? (void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
            memset(&vi, 0, sizeof vi); vi.dwOSVersionInfoSize = sizeof vi;
            if (rgv) rgv(&vi);
            fprintf(f, "Predator: Concrete Jungle in xemu - reference recording\nstarted: %s", ctime(&t));
            fprintf(f, "xemu: %s\nqemu version: %s", *xemu ? xemu : "(attached)", ver ? ver : "?\n");
            fprintf(f, "windows: %lu.%lu build %lu\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
            fprintf(f, "self-test: %s\n", selftest);
            fprintf(f, "direct memory: %s\n", s_dm_status);
            fprintf(f, "state polls: %d Hz (frames.csv rows are polls, not game frames)\n", poll_hz);
            fprintf(f, "video: %d fps, xemu client area scaled to 960 wide, JPEG (video/, video.csv)\n", s_video_fps);
            fprintf(f, "clock: every t is seconds since the recorder started; audio.wav sample n is t = n / 48000\n");
            fprintf(f, "hotkeys: F9 bookmark (+ full-size PNG + memory dump), F8 full-size PNG, F7 memory dump (mem/NNN_tag.bin, 64 MB each: cutscene start +2 s, cutscene end, level start +5 s)\n");
            fclose(f);
        }
    }
    if (poll_hz < 1) poll_hz = 20;
    if (strncmp(selftest, "PASS", 4)) {
        printf("\nThe self-test failed, so game state cannot be recorded. Video, audio and input keep recording until xemu closes.\n");
        MessageBoxA(NULL, selftest, "xemu recorder: self-test", MB_ICONWARNING);
    }

    fprintf(frames, "t,menu,menu_sub,state_depth,level_loaded,control,mission,cutscene,player,x,y,z,speed,health,energy,"
                    "cloak,cloak_state,vision,move_mode,move_submode,weapon_slot,weapon,hand_l,hand_r,attacking,attack_state,"
                    "combo,plasma_charge,holding,game_buttons,frame_len,frame_rate,game_speed,read_ms,game_frame\n");
    RegisterHotKey(NULL, 1, 0, VK_F9);
    RegisterHotKey(NULL, 2, 0, VK_F8);
    RegisterHotKey(NULL, 3, 0, VK_F7);
    printf("\nRecording to %s\nPlay normally. F9 = bookmark (something looks or sounds wrong; also dumps memory), F8 = screenshot, F7 = memory dump. Close xemu to finish.\n\n", s_dir);

    for (;;) {
        MSG msg;
        double t = now_s();
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == 1) { bookmarks++; ev("bookmark", "\"n\":%u", bookmarks); shot("bookmark"); memdump("bookmark"); printf("\nbookmark %u\n", bookmarks); }
                else if (msg.wParam == 3) { memdump("manual"); printf("\nmemory dump %u\n", s_ndumps); }
                else shot("manual");
            }
        }
        if (s_have_xemu && WaitForSingleObject(s_xemu.hProcess, 0) == WAIT_OBJECT_0) break;
        if (!s_have_xemu && !xemu_window() && t > 10) break;
        if (t < next) { Sleep(1); continue; }
        next += 1.0 / poll_hz;
        if (t > next + 1.0) next = t;
        if (!strncmp(selftest, "PASS", 4) && s_sock != INVALID_SOCKET) {
            read_state(&s);
            if (s_dm) {                                   /* direct: QMP only once a second (alive check, CR3) */
                static double last_alive;
                static LONG last_fail;
                if (t - last_alive >= 1.0) {
                    uint32_t old = s_cr3;
                    last_alive = t;
                    if (!read_cr3()) { ev("disconnected", ""); closesocket(s_sock); s_sock = INVALID_SOCKET; continue; }
                    if (s_cr3 != old) ev("cr3", "\"from\":\"%08X\",\"to\":\"%08X\"", old, s_cr3);
                    if (s_dm_fail - last_fail > 200) {
                        s_dm = 0; snprintf(s_dm_status, sizeof s_dm_status, "off: direct reads started failing, back to QMP");
                        ev("direct_memory", "\"on\":0,\"status\":\"%s\"", s_dm_status);
                    }
                    last_fail = s_dm_fail;
                }
            } else if (!hmp("info status")) {
                ev("disconnected", "");
                closesocket(s_sock); s_sock = INVALID_SOCKET;
                continue;
            }
            polls++;
            read_ms_sum += s.read_ms; if (s.read_ms > read_ms_max) read_ms_max = s.read_ms;
            fprintf(frames, "%.4f,%s,%u,%u,%u,%u,%u,%u,%d,%.3f,%.3f,%.3f,%.4f,%.2f,%.2f,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%d,%08X,%.5f,%.2f,%.3f,%.2f,%u\n",
                    t, fe_name(s.fe), s.fe_sub, s.gs_depth, s.level, s.control, s.mission, s.cut, s.have_player,
                    s.x, s.y, s.z, s.speed, s.health, s.energy, s.cloak, s.cloak_state, s.have_player ? vision_name(s.vision) : "",
                    s.mode, s.submode, s.slot, s.weapon, s.hand0, s.hand1, s.attacking, s.attack_state, s.combo, s.plasma,
                    s.holding, s.buttons, s.frame_len, s.frame_rate, s.game_speed, s.read_ms, s.gf);
            if (polls % (unsigned)(poll_hz * 2) == 0) fflush(frames);
            if (have_last) {
                double dt = 1.0 / poll_hz;
                if (s.level) t_level += dt;
                if (s.have_player && s.vision < 8) t_vision[s.vision] += dt;
                if (s.fe != last.fe || s.fe_sub != last.fe_sub)
                    ev("menu", "\"from\":\"%s\",\"to\":\"%s\",\"sub\":%u", fe_name(last.fe), fe_name(s.fe), s.fe_sub);
                if (s.level != last.level) {
                    if (s.level) { levels++; level_t0 = t; shot("level_start"); }
                    ev(s.level ? "level_start" : "level_end", "\"mission\":%u", s.mission);
                    if (!s.level && level_t0) ev("level_time", "\"mission\":%u,\"seconds\":%.1f", last.mission, t - level_t0);
                }
                if (s.mission != last.mission) ev("mission", "\"from\":%u,\"to\":%u", last.mission, s.mission);
                if (s.cut != last.cut) {
                    ev(s.cut ? "cutscene_start" : "cutscene_end", "");
                    if (s.cut) dump_at = t + 2.0; else memdump("cutscene_end");
                }
                if (dump_at > 0 && t >= dump_at && s.cut) { dump_at = 0; memdump("cutscene"); }
                if (s.level != last.level && s.level) level_dump_at = t + 5.0;
                if (level_dump_at > 0 && t >= level_dump_at) { level_dump_at = 0; memdump("level"); }
                if (s.control != last.control) ev(s.control ? "control_gained" : "control_lost", "");
                if (fabsf(s.game_speed - last.game_speed) > 0.01f) ev("game_speed", "\"from\":%.3f,\"to\":%.3f", last.game_speed, s.game_speed);
                if (s.have_player && last.have_player) {
                    if (s.health < last.health - 0.01f) {
                        damage_n++; damage += last.health - s.health;
                        ev("damage", "\"amount\":%.2f,\"health\":%.2f,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"move_mode\":%d,\"cloak\":%d",
                           last.health - s.health, s.health, s.x, s.y, s.z, s.mode, s.cloak);
                    } else if (s.health > last.health + 0.01f)
                        ev("heal", "\"amount\":%.2f,\"health\":%.2f", s.health - last.health, s.health);
                    if (s.health <= 0 && last.health > 0) { deaths++; shot("death"); ev("death", "\"mission\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f", s.mission, s.x, s.y, s.z); }
                    if (s.weapon != last.weapon || s.slot != last.slot) ev("weapon", "\"slot\":%d,\"weapon\":%d,\"prev\":%d", s.slot, s.weapon, last.weapon);
                    if (s.hand0 != last.hand0 || s.hand1 != last.hand1) ev("hands", "\"left\":%d,\"right\":%d", s.hand0, s.hand1);
                    if (s.vision != last.vision) ev("vision", "\"from\":\"%s\",\"to\":\"%s\"", vision_name(last.vision), vision_name(s.vision));
                    if (s.cloak != last.cloak) ev(s.cloak ? "cloak_on" : "cloak_off", "\"energy\":%.2f", s.energy);
                    if (s.mode != last.mode) ev("move_mode", "\"from\":%d,\"to\":%d,\"sub\":%d,\"speed\":%.3f", last.mode, s.mode, s.submode, s.speed);
                    if (s.combo != last.combo && s.combo >= 0) ev("combo", "\"combo\":%d", s.combo);
                    if (s.attacking != last.attacking) ev(s.attacking ? "attack_start" : "attack_end", "\"state\":%d", s.attack_state);
                    if (last.plasma > 0.05f && s.plasma <= 0.05f) ev("plasma_release", "\"charge\":%.2f", last.plasma);
                    if (s.holding != last.holding) ev(s.holding ? "grab" : "release", "");
                }
            }
            log_actors(&s, t);
            if (s.level) log_flagch(&s);
            last = s; have_last = 1;
        }
        if (t - last_status >= 2.0) {
            last_status = t;
            printf("\r[%6.0fs] state %4.1f/s (%.0f ms/read) | %-14s lvl %u | %s hp %5.1f | game %4.1f fps x%.2f | video %ld | audio peak %3ld%% | pad rows %ld   ",
                   t, polls / (t + 1e-9), polls ? read_ms_sum / polls : 0.0, have_last ? fe_name(s.fe) : "-", have_last ? s.level : 0,
                   have_last && s.have_player ? "player" : "no player", have_last ? s.health : 0.0f,
                   have_last && s.frame_len > 0 ? 1.0 / s.frame_len : 0.0, have_last ? s.game_speed : 0.0f,
                   (long)s_video_frames, (long)(s_audio_peak * 100 / 32768), (long)s_input_rows);
            fflush(stdout);
            s_audio_peak = 0;
        }
    }

    s_stop = 1;
    if (th_video) { WaitForSingleObject(th_video, 5000); CloseHandle(th_video); }
    if (th_input) { WaitForSingleObject(th_input, 5000); CloseHandle(th_input); }
    if (th_audio) { WaitForSingleObject(th_audio, 5000); CloseHandle(th_audio); }
    {
        FILE *f;
        double dur = now_s();
        ev("exit", "\"seconds\":%.1f,\"polls\":%llu", dur, polls);
        snprintf(path, sizeof path, "%s\\summary.txt", s_dir);
        if ((f = fopen(path, "w"))) {
            fprintf(f, "xemu session: %.0f s\n", dur);
            fprintf(f, "Self-test: %s\n", selftest);
            fprintf(f, "Game state: %llu polls (%.1f per second, %.1f ms average read, %.1f ms worst)\n", polls,
                    polls / (dur > 0 ? dur : 1), polls ? read_ms_sum / polls : 0.0, read_ms_max);
            fprintf(f, "Video: %ld frames (%ld failed captures), method %s\n", (long)s_video_frames, (long)s_video_fail,
                    s_video_method == 1 ? "PrintWindow" : s_video_method == 2 ? "screen copy (keep xemu visible)" : "-");
            fprintf(f, "Audio: %s; %.1f s written (%.1f s of it silence filling gaps), first captured sound at t=%.3f\n", s_audio_status,
                    (double)s_audio_frames / REC_RATE, (double)s_audio_silence / REC_RATE, s_audio_t0);
            fprintf(f, "Input: %s, %ld rows\n", s_input_status, (long)s_input_rows);
            fprintf(f, "Levels started: %u, time in level %.0f s\nDeaths: %u; damage taken %.1f in %u hits\n", levels, t_level, deaths, damage, damage_n);
            fprintf(f, "Vision modes: normal %.0f s, thermal %.0f s, neuro %.0f s, predtech %.0f s\nBookmarks: %u, screenshots: %u\n",
                    t_vision[0], t_vision[1], t_vision[2], t_vision[3], bookmarks, s_nshots);
            fprintf(f, "Memory dumps: %u (mem/)\n", s_ndumps);
            fclose(f);
        }
        fclose(frames);
        ev("closed", "");
        fclose(s_events); s_events = NULL;
        printf("\n\nDone. Recording: %s\nSelf-test: %s\nAudio: %s\nVideo: %ld frames, input: %ld rows\n", s_dir, selftest, s_audio_status,
               (long)s_video_frames, (long)s_input_rows);
    }
    return 0;
}
