/* gamelog.c: gameplay session recorder for the PC build.
 *
 * Every run writes a folder logs/session-YYYYMMDD-HHMMSS next to pcj.exe
 * (CJ_GAMELOG_DIR overrides the parent folder, CJ_GAMELOG=0 turns it off):
 *
 *   session.txt   machine, GPU, settings, controller, build
 *   frames.csv    one row per presented frame: timing, renderer load, game
 *                 state, player state (position, health, energy, cloak,
 *                 vision mode, weapon, movement mode, combat), raw input
 *   events.jsonl  one JSON object per event: state changes, level/mission/
 *                 cutscene start and end, damage, death, weapon / vision /
 *                 cloak changes, combos, hitches, controller plug/unplug,
 *                 fullscreen, focus, bookmarks (F9), screenshots, exit, crash
 *   shots/        screenshots: every CJ_GAMELOG_SHOT_EVERY seconds (60),
 *                 on key events (level start, death, bookmark), and F12
 *   summary.txt   written on exit: duration, frame-rate statistics, hitches,
 *                 time per level and vision mode, deaths, damage, weapons
 *   pcj-log.txt   copy of the full engine log, on exit
 *
 * Player fields come from XItemHandler_Player (pcj-decomp include/
 * XItemHandler_Player.h offsets; instance pointer 0x0048FC98), game state from
 * the same XApp / FEMainMenu fields cj_pad.c's [STATE] monitor reads.
 * Everything is read-only: the logger never writes guest memory. */
#include "async_io.h"
#include "frame_pacer.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define SEP "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define SEP "/"
#endif
#include <SDL2/SDL.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

typedef struct { uint16_t dig; uint8_t ana[8]; int16_t lx, ly, rx, ry; } GlPad;

static int s_on = -1;
static char s_dir[1024];
const char *gamelog_dir(void) { return s_dir[0] ? s_dir : NULL; }
static FILE *s_frames, *s_events;
static double s_t0, s_last_t, s_last_flush, s_last_shot, s_last_auto_shot;
static int s_shot_every = 60;
static char s_shot_reason[64];
static unsigned s_nshots, s_nbookmarks, s_nhitch;
static uint64_t s_nframes;

/* statistics */
#define HB 1001                          /* frame-time histogram, 1 ms buckets, last = >= 1 s */
static uint64_t s_hist[HB];
static double s_sum_ms, s_max_ms;
static double s_time_level[64], s_time_vision[8], s_time_weapon[64], s_time_cloak, s_time_control;
static unsigned s_deaths, s_damage_events, s_levels_started, s_cutscenes, s_combos;
static double s_damage_taken, s_healed;

/* ---------- guest memory (read-only, range-checked) ---------- */
static int gok(uint32_t va, uint32_t n)
{
    uint32_t a = va & 0x7FFFFFFFu;
    return va && a >= 0x10000u && a + n < 0x08000000u;
}
static uint32_t g32(uint32_t va) { return gok(va, 4) ? *(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + va) : 0; }
static uint8_t g8(uint32_t va) { return gok(va, 1) ? *((const uint8_t *)xbox_GetMemoryOffset() + va) : 0; }
static float gf(uint32_t va)
{
    float f = 0; uint32_t u = g32(va);
    memcpy(&f, &u, 4);
    return isfinite(f) ? f : 0.0f;
}

typedef struct {
    uint32_t fe, fe_sub, gs_depth, level, control, mission, cut;
    int have_player;
    float x, y, z, speed, health, energy, plasma;
    int cloak, cloak_state, vision, mode, submode, slot, weapon, hand0, hand1, attacking, attack_state, combo, holding;
    uint32_t game_buttons;
    uint32_t anim_mode;     /* the player item's current anim mode hash (XItem +0x160) */
} GState;

static void read_state(GState *s)
{
    uint32_t fe = g32(0x0048CE58u), app = g32(0x004931E8u), p = g32(0x0048FC98u);
    memset(s, 0, sizeof *s);
    if (fe && g32(fe + 0x1C0)) { uint32_t m = g32(fe + 0x1C0); s->fe = 1 + g32(m + 0x404); s->fe_sub = g32(m + 0x408); }
    if (app) {
        s->gs_depth = g32(app + 0xC98);
        s->level = g8(app + 0xCBC); s->control = g8(app + 0xD00); s->mission = g32(app + 0xD04); s->cut = g8(app + 0xE30);
    }
    if (p && gok(p, 0xD90) && s->level) {
        float dx = gf(p + 0x2D8), dy = gf(p + 0x2DC), dz = gf(p + 0x2E0);
        s->have_player = 1;
        s->x = gf(p + 0x2C8); s->y = gf(p + 0x2CC); s->z = gf(p + 0x2D0);
        s->speed = sqrtf(dx * dx + dy * dy + dz * dz);
        s->health = gf(p + 0xC5C); s->energy = gf(p + 0xC60);
        s->cloak = (int8_t)g8(p + 0xC68); s->cloak_state = (int)g32(p + 0xC6C); s->vision = g8(p + 0xC70);
        s->mode = (int)g32(p + 0x20C); s->submode = (int)g32(p + 0x210);
        s->slot = (int)g32(p + 0x2AC);
        s->weapon = (s->slot >= 0 && s->slot < 4) ? (int)g32(p + 0x298 + 4 * (uint32_t)s->slot) : -1;
        s->hand0 = (int)g32(p + 0x290); s->hand1 = (int)g32(p + 0x294);
        s->attacking = (int8_t)g8(p + 0x1D4); s->attack_state = (int)g32(p + 0x1DC); s->combo = (int)g32(p + 0x1E4);
        s->plasma = gf(p + 0x1F0); s->holding = g8(p + 0x12C);
        s->game_buttons = g32(p + 0xF4);
        { uint32_t it = g32(p + 4); s->anim_mode = it ? g32(it + 0x160) : 0; }
    }
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

/* ---------- output ---------- */
static double now(void) { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

void gamelog_event(const char *type, const char *fmt, ...)
{
    va_list ap;
    if (s_on <= 0 || !s_events) return;
    /* gf: the game's own frame counter (g_Stats.m_FrameCount, 0x49DB48, EXApp::MainUpdate), the same
     * field xemu_soak logs, so port and Xbox logs line up event for event */
    fprintf(s_events, "{\"t\":%.3f,\"frame\":%llu,\"gf\":%u,\"type\":\"%s\"", now() - s_t0, (unsigned long long)s_nframes,
            g32(0x0049DB48u), type);
    if (fmt && *fmt) {
        fputc(',', s_events);
        va_start(ap, fmt); vfprintf(s_events, fmt, ap); va_end(ap);
    }
    fputs("}\n", s_events);
}

static void request_shot(const char *reason)
{
    if (s_on <= 0) return;
    if (!s_shot_reason[0]) snprintf(s_shot_reason, sizeof s_shot_reason, "%s", reason);
}

/* F9 also captures the 8 flips after it (consecutive frames show a flicker;
 * one picture cannot), and gl_backend logs those flips' surfaces and draws.
 * Called once per flip before gamelog_shot_pending; non-zero during a burst. */
static unsigned s_burst, s_burst_n;
static uint64_t s_auto_at;
static uint32_t s_auto_fx;
int gamelog_burst_frame(void)
{
    char r[48];
    if (s_on <= 0 || !s_burst) return 0;
    snprintf(r, sizeof r, "bookmark%u_burst%u", s_nbookmarks, 9 - s_burst);
    s_burst--;
    s_shot_reason[0] = 0;
    request_shot(r);
    s_burst_n++;
    return 1;
}

/* Manual F9 presses so far (gl_backend saves its frame ring on each new one). */
static unsigned s_nmanual;
unsigned gamelog_bookmarks_manual(void) { return s_on > 0 ? s_nmanual : 0; }

/* Save one bottom-up RGBA frame under shots/ with the given reason (flicker ring). */
void gamelog_save_shot(const uint8_t *rgba_bottom_up, int w, int h);
void gamelog_save_named(const uint8_t *rgba_bottom_up, int w, int h, const char *reason)
{
    if (s_on <= 0) return;
    char keep[sizeof s_shot_reason];
    memcpy(keep, s_shot_reason, sizeof keep);
    snprintf(s_shot_reason, sizeof s_shot_reason, "%s", reason);
    gamelog_save_shot(rgba_bottom_up, w, h);
    memcpy(s_shot_reason, keep, sizeof keep);
}

/* gl_backend asks before SwapWindow; non-zero: read the back buffer and call gamelog_save_shot */
int gamelog_shot_pending(void) { return s_on > 0 && s_shot_reason[0]; }

void gamelog_save_shot(const uint8_t *rgba_bottom_up, int w, int h)
{
    char path[1200];
    int step = 1, ow, oh, x, y;
    SDL_Surface *sf;
    if (s_on <= 0) return;
    while (w / step > 960) step++;
    ow = w / step; oh = h / step;
    sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 24, SDL_PIXELFORMAT_RGB24);
    if (sf) {
        for (y = 0; y < oh; y++) {
            uint8_t *d = (uint8_t *)sf->pixels + (size_t)y * sf->pitch;
            const uint8_t *s = rgba_bottom_up + (size_t)(h - 1 - y * step) * w * 4;
            for (x = 0; x < ow; x++) { d[x * 3] = s[x * step * 4]; d[x * 3 + 1] = s[x * step * 4 + 1]; d[x * 3 + 2] = s[x * step * 4 + 2]; }
        }
        snprintf(path, sizeof path, "%s" SEP "shots" SEP "%05u_%07.1fs_%s.bmp", s_dir, s_nshots, now() - s_t0, s_shot_reason);
        if (cj_afile_save_bmp(sf, path) == 0) {   /* saved and freed by a background thread */
            s_nshots++;
            gamelog_event("screenshot", "\"reason\":\"%s\",\"file\":\"shots/%05u_%07.1fs_%s.bmp\"", s_shot_reason,
                          s_nshots - 1, now() - s_t0, s_shot_reason);
        }
    }
    s_shot_reason[0] = 0;
    s_last_shot = now();
}

static void write_session(const char *gl_vendor, const char *gl_renderer, const char *gl_version)
{
    char path[1100];
    FILE *f;
    snprintf(path, sizeof path, "%s" SEP "session.txt", s_dir);
    if (!(f = fopen(path, "w"))) return;
    {
        time_t t = time(NULL);
        fprintf(f, "Predator: Concrete Jungle PC build - gameplay session\nstarted: %s", ctime(&t));
    }
    fprintf(f, "build: %s %s\n", __DATE__, __TIME__);
    fprintf(f, "platform: %s, %d logical CPUs, %d MB RAM, SDL %d.%d.%d\n", SDL_GetPlatform(), SDL_GetCPUCount(),
            SDL_GetSystemRAM(), SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
#ifdef _WIN32
    {
        char cpu[64] = "";
        HKEY k;
        DWORD n = sizeof cpu;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == 0) {
            RegQueryValueExA(k, "ProcessorNameString", NULL, NULL, (BYTE *)cpu, &n);
            RegCloseKey(k);
        }
        fprintf(f, "cpu: %s\n", cpu);
        {
            char prod[128] = "", build[32] = "", disp[32] = "";
            DWORD n1 = sizeof prod, n2 = sizeof build, n3 = sizeof disp;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &k) == 0) {
                RegQueryValueExA(k, "ProductName", NULL, NULL, (BYTE *)prod, &n1);
                RegQueryValueExA(k, "CurrentBuild", NULL, NULL, (BYTE *)build, &n2);
                RegQueryValueExA(k, "DisplayVersion", NULL, NULL, (BYTE *)disp, &n3);
                RegCloseKey(k);
            }
            fprintf(f, "windows: %s %s (build %s)\n", prod, disp, build);
        }
        {
            char wine[64] = "";
            HMODULE nt = GetModuleHandleA("ntdll.dll");
            typedef const char *(CDECL *wv_t)(void);
            wv_t wv = nt ? (wv_t)(void *)GetProcAddress(nt, "wine_get_version") : NULL;
            if (wv) snprintf(wine, sizeof wine, "%s", wv());
            if (*wine) fprintf(f, "wine: %s (macOS/Whisky or Linux)\n", wine);
        }
    }
#endif
    fprintf(f, "gpu: %s | %s | OpenGL %s\n", gl_vendor ? gl_vendor : "?", gl_renderer ? gl_renderer : "?",
            gl_version ? gl_version : "?");
    {
        int i, n = SDL_GetNumVideoDisplays();
        for (i = 0; i < n; i++) {
            SDL_DisplayMode dm;
            if (SDL_GetDesktopDisplayMode(i, &dm) == 0)
                fprintf(f, "display %d: %dx%d @ %d Hz\n", i, dm.w, dm.h, dm.refresh_rate);
        }
    }
    {
        static const char *keys[] = {"CJ_GL", "CJ_GL_VSH", "CJ_WINDOW_SCALE", "CJ_FULLSCREEN", "CJ_VSYNC", "CJ_MOVIE_AUDIO",
                                     "CJ_AUDIO_XA2", "CJ_NATIVE_SWAP", "CJ_GL_WBUF", "CJ_GL_ANISO", "CJ_GL_LOD_BIAS",
                                     "CJ_GAMELOG_SHOT_EVERY", NULL};
        int i;
        fprintf(f, "settings:");
        for (i = 0; keys[i]; i++) if (getenv(keys[i])) fprintf(f, " %s=%s", keys[i], getenv(keys[i]));
        fprintf(f, "\n");
    }
    fprintf(f, "\nfiles: frames.csv (per frame), events.jsonl (events), shots/ (screenshots), summary.txt, pcj-log.txt\n"
               "hotkeys: F9 = bookmark this moment (+ the next 8 frames; with CJ_FLICKER=1 also the 45 before it), F12 = screenshot\n"
               "flicker (CJ_FLICKER=1 only): frames that change for one frame and change back are saved by themselves (shots/*flicker*)\n");
    fclose(f);
}

static void gamelog_close(void);

void gamelog_init(const char *gl_vendor, const char *gl_renderer, const char *gl_version)
{
    char parent[1024], path[1100];
    const char *e = getenv("CJ_GAMELOG");
    if (s_on >= 0) return;
    s_on = !(e && *e == '0');
    if (!s_on) return;
    if ((e = getenv("CJ_GAMELOG_SHOT_EVERY"))) s_shot_every = atoi(e);
    if ((e = getenv("CJ_GAMELOG_DIR")) && *e) snprintf(parent, sizeof parent, "%s", e);
    else {
#ifdef _WIN32
        char *sl;
        GetModuleFileNameA(NULL, parent, sizeof parent - 8);
        if ((sl = strrchr(parent, '\\'))) *sl = 0;
        strcat(parent, "\\logs");
#else
        snprintf(parent, sizeof parent, "logs");
#endif
    }
    MKDIR(parent);
    {
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);
        snprintf(s_dir, sizeof s_dir, "%s" SEP "session-%04d%02d%02d-%02d%02d%02d", parent, lt->tm_year + 1900,
                 lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
    }
    MKDIR(s_dir);
    snprintf(path, sizeof path, "%s" SEP "shots", s_dir);
    MKDIR(path);
    snprintf(path, sizeof path, "%s" SEP "frames.csv", s_dir);
    s_frames = cj_afile_open(path, "w");
    snprintf(path, sizeof path, "%s" SEP "events.jsonl", s_dir);
    s_events = cj_afile_open(path, "w");
    if (!s_frames || !s_events) {
        fprintf(stderr, "[LOG] cannot create the session log in %s; gameplay logging off\n", s_dir);
        s_on = 0;
        return;
    }
    fprintf(s_frames, "t,frame,flip,frame_ms,draws,tris,menu,menu_sub,state_depth,level_loaded,control,mission,cutscene,"
                      "player,x,y,z,speed,health,energy,cloak,cloak_state,vision,move_mode,move_submode,weapon_slot,weapon,"
                      "hand_l,hand_r,attacking,attack_state,combo,plasma_charge,holding,game_buttons,"
                      "pad_dpad_etc,pad_a,pad_b,pad_x,pad_y,pad_black,pad_white,pad_lt,pad_rt,pad_lx,pad_ly,pad_rx,pad_ry,cam_dist,cam_target_dist,cam_angle,cam_elev,work_ms,wait_ms,present_ms\n");
    s_t0 = s_last_t = s_last_flush = s_last_shot = s_last_auto_shot = now();
    write_session(gl_vendor, gl_renderer, gl_version);
    gamelog_event("session_start", "");
    fprintf(stderr, "[LOG] gameplay log: %s\n", s_dir);
    atexit(gamelog_close);
}

/* called once per presented frame, after input has been sampled */

/* Window monitor: the engine's window tree (EXBaseDisplay::m_pDisplay at 0x49e110, top-level list at
 * +0x1c, children at wnd+0x10, list items at wnd+4 {prev, next}). Logs each window's vtable, flags
 * (+0x70), effects (+0x74: 4 focus blur, 8 motion blur, 0x100 expansion blur, 1 glow, 2 noise,
 * 0x20 refraction, 0x40/0x80 colour passes) and the per-frame bloom bits (+0x298) when they change,
 * so a flicker can be tied to the post effect that is running. */
typedef struct { uint32_t w, vt, fl, fx, efx; } WndSig;
static int wnd_walk(uint32_t item, int depth, WndSig *out, int n, int max)
{
    int guard = 0;
    while (item && n < max && guard++ < 64) {
        uint32_t w = item - 4;
        if (!gok(w, 0x29c)) break;
        out[n].w = w; out[n].vt = g32(w); out[n].fl = g32(w + 0x70); out[n].fx = g32(w + 0x74);
        out[n].efx = g32(w + 0x298);
        n++;
        if (depth < 4) n = wnd_walk(g32(w + 0x10), depth + 1, out, n, max);
        item = g32(item + 4);
    }
    return n;
}
static void wnd_monitor(void)
{
    static WndSig last[48];
    static int nlast = -1;
    WndSig cur[48];
    uint32_t disp = g32(0x49e110);
    int n = disp ? wnd_walk(g32(disp + 0x1c), 0, cur, 0, 48) : 0;
    if (n == nlast && !memcmp(cur, last, n * sizeof(WndSig))) return;
    char buf[2048]; int o = 0;
    for (int i = 0; i < n && o < (int)sizeof(buf) - 80; i++)
        o += snprintf(buf + o, sizeof(buf) - o, "%s{\"w\":\"%08X\",\"vt\":\"%08X\",\"flags\":\"%08X\",\"fx\":\"%08X\",\"efx\":%u}",
                      i ? "," : "", cur[i].w, cur[i].vt, cur[i].fl, cur[i].fx, cur[i].efx);
    gamelog_event("windows", "\"n\":%d,\"list\":[%s]", n, buf);
    /* A post effect turned on (weapon list and pause menu: focus blur 4): capture a burst of
     * frames once it has settled, so a flicker there is on record without anyone pressing F9. */
    {
        static uint32_t lastfx;
        static unsigned nauto;
        uint32_t fx = 0;
        for (int i = 0; i < n; i++) fx |= cur[i].fx;
        if (fx && fx != lastfx && nauto < 6 && getenv("CJ_AUTO_BURST")) { s_auto_at = s_nframes + 45; s_auto_fx = fx; nauto++; }
        lastfx = fx;
    }
    fprintf(stderr, "[WND] n=%d %s\n", n, buf);
    memcpy(last, cur, n * sizeof(WndSig)); nlast = n;
}

/* Player animation modes: frames spent in each (summary.txt; names: tools/edb/anim_names.py), and
 * CJ_ANIM_LOG=1 prints each change to the log (also without the game log, e.g. in replays). */
#define AM_N 512
static uint32_t s_am_hash[AM_N];
static uint64_t s_am_frames[AM_N];
static unsigned s_am_enters[AM_N];
static int s_am_n;
static uint32_t s_am_last;
static void anim_count(uint32_t h)
{
    int i;
    if (!h) return;
    for (i = 0; i < s_am_n && s_am_hash[i] != h; i++) ;
    if (i == s_am_n) { if (s_am_n == AM_N) return; s_am_hash[s_am_n++] = h; }
    s_am_frames[i]++;
    if (h != s_am_last) s_am_enters[i]++;
    s_am_last = h;
}
static void anim_summary(FILE *f)
{
    int i, j;
    if (!s_am_n) return;
    fprintf(f, "\nPlayer animation modes (hash: frames, times entered), %d distinct:\n", s_am_n);
    for (i = 0; i < s_am_n; i++) {          /* most frames first */
        int best = i;
        for (j = i + 1; j < s_am_n; j++) if (s_am_frames[j] > s_am_frames[best]) best = j;
        if (best != i) {
            uint32_t h = s_am_hash[i]; uint64_t fr = s_am_frames[i]; unsigned en = s_am_enters[i];
            s_am_hash[i] = s_am_hash[best]; s_am_frames[i] = s_am_frames[best]; s_am_enters[i] = s_am_enters[best];
            s_am_hash[best] = h; s_am_frames[best] = fr; s_am_enters[best] = en;
        }
        fprintf(f, "  %08x: %llu, %u\n", s_am_hash[i], (unsigned long long)s_am_frames[i], s_am_enters[i]);
    }
}
static void anim_log_stderr(uint32_t flip)
{
    static int on = -1;
    static uint32_t last;
    uint32_t p, it, h;
    if (on < 0) { const char *e = getenv("CJ_ANIM_LOG"); on = e && *e == '1'; }
    if (!on) return;
    p = g32(0x0048FC98u);
    it = p ? g32(p + 4) : 0;
    h = it ? g32(it + 0x160) : 0;
    if (h != last) {
        fprintf(stderr, "[ANIM] flip %u mode %08x -> %08x (move mode %d sub %d)\n", flip, last, h, p ? (int)g32(p + 0x20C) : -1, p ? (int)g32(p + 0x210) : -1);
        last = h;
    }
}

void gamelog_frame(uint32_t flip, uint64_t draws_total, uint64_t tris_total, const void *pad_v)
{
    static GState last;
    static int have_last;
    static uint64_t d_last, t_last;
    static double level_start_t;
    const GlPad *pad = (const GlPad *)pad_v;
    GState s;
    double t, dt_ms;
    anim_log_stderr(flip);
    if (s_on <= 0) return;
    t = now();
    dt_ms = (t - s_last_t) * 1000.0;
    s_last_t = t;
    s_nframes++;
    read_state(&s);
    {   /* FlagChannels (0x48FA00, 290 bytes): script/trigger channel changes */
        static uint8_t fc[290]; static int fc_init;
        int k;
        if (s.level && gok(0x0048FA00u, 290)) {
            const uint8_t *cur = (const uint8_t *)xbox_GetMemoryOffset() + 0x0048FA00u;
            if (!fc_init) { memcpy(fc, cur, sizeof fc); fc_init = 1; }
            for (k = 0; k < 290; k++)
                if (cur[k] != fc[k]) { gamelog_event("flagch", "\"ch\":%d,\"v\":%u,\"was\":%u,\"cut\":%u", k, cur[k], fc[k], s.cut); fc[k] = cur[k]; }
        } else fc_init = 0;
    }
    wnd_monitor();
    if (s_auto_at && s_nframes >= s_auto_at) {
        s_auto_at = 0;
        s_nbookmarks++;
        gamelog_event("bookmark", "\"n\":%u,\"auto\":\"effects %X\"", s_nbookmarks, s_auto_fx);
        request_shot("bookmark");
        s_burst = 8;
        fprintf(stderr, "[LOG] auto bookmark %u for window effects %X (with the next 8 frames)\n", s_nbookmarks, s_auto_fx);
    }

    float cam[4];
    /* frame-time statistics (skip the first frame) */
    if (s_nframes > 1) {
        int b = dt_ms >= HB - 1 ? HB - 1 : (int)dt_ms;
        s_hist[b]++;
        s_sum_ms += dt_ms;
        if (dt_ms > s_max_ms) s_max_ms = dt_ms;
        static double ema = 33.0;           /* typical frame time; a hitch is a spike above it */
        int spike = dt_ms > 100.0 && dt_ms > 3.0 * ema;
        ema = ema * 0.95 + (dt_ms < 1000.0 ? dt_ms : 1000.0) * 0.05;
        if (spike && s_nframes > 30) {
            s_nhitch++;
            gamelog_event("hitch", "\"ms\":%.1f,\"level_loaded\":%u,\"cutscene\":%u", dt_ms, s.level, s.cut);
        }
        if (s.level && s.mission < 64) s_time_level[s.mission] += dt_ms / 1000.0;
        if (s.have_player) {
            if (s.vision >= 0 && s.vision < 8) s_time_vision[s.vision] += dt_ms / 1000.0;
            if (s.weapon >= 0 && s.weapon < 64) s_time_weapon[s.weapon] += dt_ms / 1000.0;
            if (s.cloak) s_time_cloak += dt_ms / 1000.0;
            if (s.control) s_time_control += dt_ms / 1000.0;
        }
    }

    {   /* camera (gpCameraItem[0]->m_pItemHandler): distance, target distance, angle, elevation */
        uint32_t ci = g32(0x0048E8A0u), ch = ci ? g32(ci + 0x144) : 0;
        if (ch && gok(ch, 0x100)) { cam[0] = gf(ch + 0x18); cam[1] = gf(ch + 0x24); cam[2] = gf(ch + 0x0C); cam[3] = gf(ch + 0x10); }
        else cam[0] = cam[1] = cam[2] = cam[3] = 0.0f;
    }
    fprintf(s_frames, "%.4f,%llu,%u,%.2f,%llu,%llu,%s,%u,%u,%u,%u,%u,%u,%d,%.3f,%.3f,%.3f,%.4f,%.2f,%.2f,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%d,%08X,"
                      "%04X,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%.2f,%.2f,%.3f,%.3f,%.2f,%.2f,%.2f\n",
            t - s_t0, (unsigned long long)s_nframes, flip, dt_ms, (unsigned long long)(draws_total - d_last),
            (unsigned long long)(tris_total - t_last), fe_name(s.fe), s.fe_sub, s.gs_depth, s.level, s.control, s.mission,
            s.cut, s.have_player, s.x, s.y, s.z, s.speed, s.health, s.energy, s.cloak, s.cloak_state,
            s.have_player ? vision_name(s.vision) : "", s.mode, s.submode, s.slot, s.weapon, s.hand0, s.hand1,
            s.attacking, s.attack_state, s.combo, s.plasma, s.holding, s.game_buttons,
            pad ? pad->dig : 0, pad ? pad->ana[0] : 0, pad ? pad->ana[1] : 0, pad ? pad->ana[2] : 0,
            pad ? pad->ana[3] : 0, pad ? pad->ana[4] : 0, pad ? pad->ana[5] : 0, pad ? pad->ana[6] : 0,
            pad ? pad->ana[7] : 0, pad ? pad->lx : 0, pad ? pad->ly : 0, pad ? pad->rx : 0, pad ? pad->ry : 0,
            cam[0], cam[1], cam[2], cam[3], cj_pace_work_ms(), cj_pace_wait_ms(), cj_pace_present_ms());
    d_last = draws_total; t_last = tris_total;

    /* events from state changes */
    if (have_last) {
        if (s.fe != last.fe || s.fe_sub != last.fe_sub)
            gamelog_event("menu", "\"from\":\"%s\",\"to\":\"%s\",\"sub\":%u", fe_name(last.fe), fe_name(s.fe), s.fe_sub);
        if (s.level != last.level) {
            if (s.level) { s_levels_started++; level_start_t = t; request_shot("level_start"); }
            gamelog_event(s.level ? "level_start" : "level_end", "\"mission\":%u%s", s.mission,
                          s.level ? "" : "");
            if (!s.level && level_start_t) gamelog_event("level_time", "\"mission\":%u,\"seconds\":%.1f", last.mission, t - level_start_t);
        }
        if (s.mission != last.mission) gamelog_event("mission", "\"from\":%u,\"to\":%u", last.mission, s.mission);
        if (s.cut != last.cut) { if (s.cut) s_cutscenes++; gamelog_event(s.cut ? "cutscene_start" : "cutscene_end", ""); }
        /* POIs near the player once per cutscene, 1.5 s in (after any cutscene teleport): index, position,
         * type, vendor/customer force states, flag/force channels, binds enabled, vendor count. For the
         * hallway "gangsters should panic" question (vendor state FS_PanicStand vs NONE). */
        {
            static double cut_start_t = 0.0; static unsigned dumped = 0;
            if (s.cut && !last.cut) cut_start_t = t;
            if (s.cut && dumped != s_cutscenes && t - cut_start_t >= 1.5) {
                uint32_t tab = g32(0x003968B4u), np = g32(0x00395758u), i;
                char buf[6000]; int bl = 0, n = 0;
                dumped = s_cutscenes; buf[0] = 0;
                for (i = 1; tab && i < np && i < 500 && bl < (int)sizeof buf - 200; i++) {
                    uint32_t pp = tab + i * 0x2EC;
                    float px = gf(pp + 0x220), py = gf(pp + 0x224), pz = gf(pp + 0x228);
                    float dx = px - s.x, dz = pz - s.z;
                    if (dx * dx + dz * dz > 30.0f * 30.0f || fabsf(py - s.y) > 6.0f) continue;
                    bl += snprintf(buf + bl, sizeof buf - bl, "%s[%u,%.1f,%.1f,%.1f,\"%X\",\"%X\",\"%X\",%u,%u,%u,%u,\"%X\"]", n++ ? "," : "",
                                   i, px, py, pz, g32(pp + 0x264), g32(pp + 0x298), g32(pp + 0x29C), g32(pp + 0x28C), g32(pp + 0x294),
                                   g32(pp + 0x2A0), g32(pp + 0x254), g32(pp + 0x20));
                }
                gamelog_event("pois_near", "\"n\":%d,\"total\":%u,\"cols\":\"i,x,y,z,type,vendor,customer,flagch,forcech,en,nv,bind0\",\"list\":[%s]", n, np, buf);
            }
        }
        /* Actors near the player through a cutscene and for 8 s after it, twice a
         * second: who is close, their health and AI state (StateMachine::m_State,
         * a guest code address), and whether the player has control. For the
         * "enemies get a head start on the cutscene" question. */
        {
            static double cut_end_t = -100.0, last_dump = 0.0;
            if (last.cut && !s.cut) cut_end_t = t;
            if (s.have_player && (s.cut || t - cut_end_t < 8.0) && t - last_dump >= 0.5) {
                char buf[4096]; int bl = 0, n = 0, total = 0;
                buf[0] = 0;
                uint32_t p = g32(0x0048FC98u);
                last_dump = t;
                for (uint32_t i = 0; i < 128 && bl < (int)sizeof buf - 300; i++) {
                    uint32_t ab = 0x0047C3F0u + i * 0x18u, a = g32(ab + 0x10);
                    if (!a || !gok(a, 0x800)) continue;
                    total++;
                    float ax = gf(ab), ay = gf(ab + 4), az = gf(ab + 8);
                    float dx = ax - s.x, dy = ay - s.y, dz = az - s.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (d > 25.0f) continue;
                    {   /* brain: current state, its locals 0-2, last message, pushed states (Actor::m_Stack) */
                        uint32_t sm = a + 0xE8, k, nst = g32(a + 0x788);
                        char stk[96]; int sl = 0;
                        char poi[160] = "", prof[200] = "";
                        {   /* profile row (AP_GetPtrFromIndex: tables at 0x4816CC, 0x14C each): hash, start, idle, 0x18, loud noise */
                            uint32_t idx = g32(a + 0x778), sh = idx & 0xFFFF, row = idx >> 16, pr;
                            if (sh != 0xFFFF && row != 0xFFFF && (pr = g32(0x004816CCu + sh * 4)) != 0) {
                                pr += row * 0x14C;
                                int pl = snprintf(prof, sizeof prof, "%X st %X idle %X ua %X ln %X fc", g32(pr), g32(pr + 0x10), g32(pr + 0x14), g32(pr + 0x18), g32(pr + 0x1C));
                                for (k = 0; k < 5 && pl < (int)sizeof prof - 24; k++)   /* flag-channel reactions (UpdateFlagChannel): channel=state fired? */
                                    if (g32(pr + 0xAC + k * 8))
                                        pl += snprintf(prof + pl, sizeof prof - pl, " %u=%X%s%s", g32(pr + 0xAC + k * 8), g32(pr + 0xB0 + k * 8),
                                                       (g8(0x0048FA00u + g32(pr + 0xAC + k * 8)) ? "*" : ""), (g8(a + 0x948 + k) ? "!" : ""));
                            }
                        }
                        stk[0] = 0;
                        {   /* the POI a GotoLocation heads for (s_POIs 0x3968B4, 0x2EC each): binds enabled, vendors, vendor binds */
                            uint32_t pn = g32(sm + 4), tab = g32(0x003968B4u);
                            if (g32(sm + 0x58) == 0x00088F90u && tab && pn > 0 && pn < g32(0x00395758u)) {
                                uint32_t pp = tab + pn * 0x2EC;
                                snprintf(poi, sizeof poi, "en%u nv%u b%X pos %.1f,%.1f,%.1f vs %X cs %X fc %X fo %X ty %X", g32(pp + 0x2A0), g32(pp + 0x254),
                                         g32(pp + 0x20), gf(pp + 0x220), gf(pp + 0x224), gf(pp + 0x228),
                                         g32(pp + 0x298), g32(pp + 0x29C), g32(pp + 0x28C), g32(pp + 0x294), g32(pp + 0x264));
                            }
                        }
                        for (k = 0; k < nst && k < 10 && sl < (int)sizeof stk - 10; k++)
                            sl += snprintf(stk + sl, sizeof stk - sl, "%s%X", k ? "," : "", g32(a + 0x188 + k * 0x80 + 0x70));
                        bl += snprintf(buf + bl, sizeof buf - bl, "%s{\"id\":%u,\"d\":%.1f,\"hp\":%.0f,\"st\":\"%08X\",\"w\":%u,"
                                       "\"loc\":[\"%X\",\"%X\",\"%X\",\"%X\",\"%X\"],\"msg\":\"%X\",\"stk\":\"%s\",\"sf\":\"%08X\","
                                       "\"ref\":\"%X\",\"bp\":%d,\"poi\":\"%s\",\"f2\":\"%X\",\"sit\":%.1f,\"r7ec\":\"%X\",\"prof\":\"%s\"}",
                                       n++ ? "," : "", i, d, gf(a + 0x7DC), g32(sm + 0x58), g8(ab + 0x14),
                                       g32(sm), g32(sm + 4), g32(sm + 8), g32(sm + 12), g32(sm + 16), g32(sm + 0x88), stk, g32(a + 0x93C),
                                       g32(a + 0x770), (int)g32(a + 0x7D8), poi, g32(a + 0x944), gf(a + 0x814), g32(a + 0x7EC), prof);
                    }
                }
                {   /* the real player during a cutscene: hidden flag (Hide), render flag of its item */
                    uint32_t it = p ? g32(p + 4) : 0;
                    gamelog_event("actors_near", "\"cut\":%u,\"since_end\":%.1f,\"player_ctl\":%d,\"hidden\":%d,\"render\":%d,\"pv\":%d,\"item\":[%.2f,%.2f,%.2f],\"head\":[%.2f,%.2f,%.2f],\"feet\":[%.2f,%.2f,%.2f],\"actors\":%d,\"n\":%d,\"list\":[%s]",
                                  s.cut, s.cut ? -1.0 : t - cut_end_t, p ? (int8_t)g8(p + 0x30C) : -1,
                                  p ? (int8_t)g8(p + 0xD8C) : -1, it ? (g8(it + 0x10) & 1) : -1, g8(0x00394B4Du), gf(g32(0x0048E890u) + 0xC8), gf(g32(0x0048E890u) + 0xCC), gf(g32(0x0048E890u) + 0xD0),
                                  gf(0x00481D5Cu), gf(0x00481D60u), gf(0x00481D64u), gf(0x00481D6Cu), gf(0x00481D70u), gf(0x00481D74u),
                                  total, n, buf);
                }
            }
        }
        if (s.control != last.control) gamelog_event(s.control ? "control_gained" : "control_lost", "");
        if (s.have_player && last.have_player) {
            if (s.health < last.health - 0.01f) {
                s_damage_events++; s_damage_taken += last.health - s.health;
                gamelog_event("damage", "\"amount\":%.2f,\"health\":%.2f,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"move_mode\":%d,\"cloak\":%d",
                              last.health - s.health, s.health, s.x, s.y, s.z, s.mode, s.cloak);
            } else if (s.health > last.health + 0.01f) {
                s_healed += s.health - last.health;
                gamelog_event("heal", "\"amount\":%.2f,\"health\":%.2f", s.health - last.health, s.health);
            }
            if (s.health <= 0.0f && last.health > 0.0f) {
                s_deaths++;
                request_shot("death");
                gamelog_event("death", "\"mission\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"move_mode\":%d", s.mission, s.x, s.y, s.z, s.mode);
            }
            if (s.weapon != last.weapon || s.slot != last.slot)
                gamelog_event("weapon", "\"slot\":%d,\"weapon\":%d,\"prev\":%d", s.slot, s.weapon, last.weapon);
            if (s.hand0 != last.hand0 || s.hand1 != last.hand1)
                gamelog_event("hands", "\"left\":%d,\"right\":%d", s.hand0, s.hand1);
            if (s.vision != last.vision)
                gamelog_event("vision", "\"from\":\"%s\",\"to\":\"%s\"", vision_name(last.vision), vision_name(s.vision));
            if (s.cloak != last.cloak) gamelog_event(s.cloak ? "cloak_on" : "cloak_off", "\"energy\":%.2f", s.energy);
            if (s.anim_mode != last.anim_mode) {
                gamelog_event("anim", "\"from\":\"%08x\",\"to\":\"%08x\",\"move_mode\":%d,\"sub\":%d,\"speed\":%.3f",
                              last.anim_mode, s.anim_mode, s.mode, s.submode, s.speed);
            }
            anim_count(s.anim_mode);
            if (s.mode != last.mode)
                gamelog_event("move_mode", "\"from\":%d,\"to\":%d,\"sub\":%d,\"speed\":%.3f", last.mode, s.mode, s.submode, s.speed);
            if (s.combo != last.combo && s.combo >= 0) { s_combos++; gamelog_event("combo", "\"combo\":%d", s.combo); }
            if (s.attacking != last.attacking) gamelog_event(s.attacking ? "attack_start" : "attack_end", "\"state\":%d", s.attack_state);
            if (last.plasma > 0.05f && s.plasma <= 0.05f) gamelog_event("plasma_release", "\"charge\":%.2f", last.plasma);
            if (s.holding != last.holding) gamelog_event(s.holding ? "grab" : "release", "");
            if (s.energy <= 0.01f && last.energy > 0.01f) gamelog_event("energy_empty", "");
        }
    }
    last = s; have_last = 1;

    /* periodic screenshot while playing */
    if (s_shot_every > 0 && t - s_last_auto_shot >= s_shot_every) {
        s_last_auto_shot = t;
        request_shot(s.level ? "periodic" : "periodic_menu");
    }
    if (t - s_last_flush > 2.0) { fflush(s_frames); fflush(s_events); s_last_flush = t; }
}

/* SDL key presses the logger owns (F9 bookmark, F12 screenshot); window events */
void gamelog_sdl_event(const void *ev_v)
{
    const SDL_Event *ev = (const SDL_Event *)ev_v;
    if (s_on <= 0) return;
    if (ev->type == SDL_KEYDOWN && !ev->key.repeat) {
        if (ev->key.keysym.sym == SDLK_F9) {
            s_nbookmarks++;
            s_nmanual++;
            gamelog_event("bookmark", "\"n\":%u", s_nbookmarks);
            request_shot("bookmark");
            s_burst = 8;
            fprintf(stderr, "[LOG] bookmark %u (with the next 8 frames)\n", s_nbookmarks);
        } else if (ev->key.keysym.sym == SDLK_F12) {
            request_shot("manual");
        }
    }
    if (ev->type == SDL_WINDOWEVENT) {
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST) gamelog_event("focus_lost", "");
        else if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) gamelog_event("focus_gained", "");
        else if (ev->window.event == SDL_WINDOWEVENT_MINIMIZED) gamelog_event("minimized", "");
        else if (ev->window.event == SDL_WINDOWEVENT_RESTORED) gamelog_event("restored", "");
        else if (ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            gamelog_event("window_size", "\"w\":%d,\"h\":%d", ev->window.data1, ev->window.data2);
    }
}

static double pct_ms(double p)
{
    uint64_t n = 0, want, c = 0;
    int i;
    for (i = 0; i < HB; i++) n += s_hist[i];
    if (!n) return 0;
    want = (uint64_t)(n * p);
    for (i = 0; i < HB; i++) { c += s_hist[i]; if (c >= want) return i + 0.5; }
    return HB;
}

static void copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b;
    char buf[65536];
    size_t k;
    if (!a) return;
    if ((b = fopen(to, "wb"))) { while ((k = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, k, b); fclose(b); }
    fclose(a);
}

static void gamelog_close(void)
{
    char path[1100];
    FILE *f;
    double dur;
    int i;
    if (s_on <= 0) return;
    dur = now() - s_t0;
    gamelog_event("exit", "\"seconds\":%.1f,\"frames\":%llu", dur, (unsigned long long)s_nframes);
    snprintf(path, sizeof path, "%s" SEP "summary.txt", s_dir);
    if ((f = fopen(path, "w"))) {
        double avg = s_nframes > 1 ? s_sum_ms / (double)(s_nframes - 1) : 0;
        fprintf(f, "Session length: %.0f min %.0f s, %llu frames\n", floor(dur / 60), fmod(dur, 60), (unsigned long long)s_nframes);
        fprintf(f, "Frame rate: average %.1f fps (%.1f ms); median %.1f ms; 95th percentile %.1f ms (%.1f fps); 99th %.1f ms (1%% low %.1f fps); worst %.0f ms\n",
                avg > 0 ? 1000 / avg : 0, avg, pct_ms(0.5), pct_ms(0.95), 1000 / pct_ms(0.95), pct_ms(0.99),
                1000 / pct_ms(0.99), s_max_ms);
        fprintf(f, "Hitches (frames over 100 ms and 3x the recent average): %u\n", s_nhitch);
        fprintf(f, "Levels started: %u; cutscenes: %u\n", s_levels_started, s_cutscenes);
        for (i = 0; i < 64; i++) if (s_time_level[i] > 0.5) fprintf(f, "  mission %d: %.0f s in level\n", i, s_time_level[i]);
        fprintf(f, "Player control: %.0f s; cloaked: %.0f s\n", s_time_control, s_time_cloak);
        fprintf(f, "Vision modes:");
        for (i = 0; i < 4; i++) fprintf(f, " %s %.0f s;", vision_name(i), s_time_vision[i]);
        fprintf(f, "\nWeapons (seconds equipped, by weapon id):");
        for (i = 0; i < 64; i++) if (s_time_weapon[i] > 0.5) fprintf(f, " %d:%.0f", i, s_time_weapon[i]);
        fprintf(f, "\nDeaths: %u; damage taken: %.1f in %u hits; healed: %.1f; combos: %u\n", s_deaths, s_damage_taken,
                s_damage_events, s_healed, s_combos);
        fprintf(f, "Bookmarks (F9): %u; screenshots: %u\n", s_nbookmarks, s_nshots);
        anim_summary(f);
        fprintf(f, "\nFrame-time histogram (ms: frames):\n");
        for (i = 0; i < HB; i++) if (s_hist[i]) fprintf(f, "  %s%d: %llu\n", i == HB - 1 ? ">=" : "", i, (unsigned long long)s_hist[i]);
        fclose(f);
    }
    if (s_frames) fclose(s_frames);
    if (s_events) fclose(s_events);
    s_frames = s_events = NULL;
    fflush(stderr);
#ifdef _WIN32
    {
        char exe[1024], *sl;
        GetModuleFileNameA(NULL, exe, sizeof exe - 16);
        if ((sl = strrchr(exe, '\\'))) *sl = 0;
        strcat(exe, "\\pcj-log.txt");
        snprintf(path, sizeof path, "%s" SEP "pcj-log.txt", s_dir);
        copy_file(exe, path);
    }
#else
    (void)copy_file;
#endif
    s_on = 0;
}

/* crash handler: record the crash and close the files (best effort) */
void gamelog_crash(const char *what)
{
    if (s_on <= 0) return;
    gamelog_event("crash", "\"what\":\"%s\"", what);
    gamelog_close();
}
