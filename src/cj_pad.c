/*
 * Scripted pad at the XAPI level (CJ_PAD_SCRIPT / CJ_PAD_LIVE).
 *
 * The toolkit's RECOMP_PAD_SCRIPT drives an emulated OHCI controller, but its
 * MMIO trap has no x86-64 POSIX decoder, so on this host USB stays off and the
 * game never sees a pad. Instead the five XAPI entry points that
 * EXGamePad::Update (0x0026F310, the only caller, in recomp_0044.c) uses are
 * replaced with ld --wrap (CMakeLists.txt): XGetDevices reports a gamepad in
 * port 0, XInputOpen hands out a fake handle, XInputGetState returns the
 * scripted state, XInputSetState (rumble) goes to the host controller, XInputClose is a
 * no-op for the fake handle. With neither variable set every wrapper calls the
 * generated body unchanged.
 *
 *   CJ_PAD_SCRIPT=<t>:<btn>[+<btn>..][:<hold>],...   or  =@file
 *     t and hold count XInputGetState polls of port 0 (one per game frame,
 *     EXGamePad::Update runs once per MainUpdate); CJ_PAD_UNIT=ms makes them
 *     wall-clock milliseconds since the first poll. Default hold 4 polls / 150 ms.
 *     Buttons: up down left right start back lthumb rthumb a b x y black white
 *     lt rt, lleft lright lup ldown rleft rright rup rdown.
 *   CJ_PAD_REPEAT=<btn>,<period>,<hold>  pulse a button forever (after the script).
 *   CJ_PAD_LIVE=<file>  lines <btn>[+<btn>..][:<hold>] appended at run time are
 *     pressed at the next poll.
 *   CJ_PAD_REPLAY=<input.csv>  replay a controller recording from the xemu
 *     recorder (tools/xemu_soak): XInput buttons, triggers and sticks, pad 0,
 *     mapped as xemu maps them (LB = White, RB = Black). Time zero is the
 *     first front-end state the game reports (the first "menu" event in both
 *     logs); CJ_PAD_REPLAY_ANCHOR=<seconds> is that moment in the recording
 *     (tools/xemu_input_to_replay.py prints it). Rows before it are skipped.
 *   CJ_PAD_REPLAY_SYNC=<seconds>[@x,y,z]  instead start when the player first has control in
 *     a level, at that recording time (a "control_gained" event), and advance by the
 *     game's frame length; logs [RP] player position lines to compare with frames.csv
 *     (tools/replay_compare.py).
 *   CJ_PAD_HOST=1  also read the real controller + keyboard (src/gl_backend.c
 *     host_input, needs CJ_GL=1); the Windows build sets it by default. Host
 *     input and the script are OR-ed (host sticks win when deflected).
 *
 * Host sticks (cj_stick): the game's own stick processing (EXGamePad::StorePadState
 * 0x26ED60) scales each axis by sqrt(2), and in gameplay (the player selects dead
 * zone mode 1 every frame, XItemHandler_Player::GetPadData) zeroes each axis on its
 * own below 13107 (28% of the raw range) before rescaling the rest. Small
 * deflections do nothing and anything within about 16 degrees of a cardinal
 * direction snaps onto it, for movement and camera alike. The port instead applies
 * a radial dead zone to the host stick and hands the game the raw value that its
 * own mapping turns back into exactly that output (per mode, read from
 * EXBaseGamePad::m_pGamePad +0x284), so every direction and small deflection gets
 * through.
 *   CJ_STICK_DEADZONE=<0..0.9>  radial dead zone, fraction of full tilt (default 0.12)
 *   CJ_STICK_CURVE=<exp>        response exponent after the dead zone (default 1 = linear)
 *   CJ_STICK_CLASSIC=1          original behaviour: raw sticks, the game's dead zones only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "recomp_types.h"

extern void __real_m_XGetDevices_0037F68C(void);
extern void __real_m_XInputOpen_0037FBED(void);
extern void __real_m_XInputClose_0037FC43(void);
extern void __real_m_XInputGetState_0037FE27(void);
extern void __real_m_XInputSetState_0037FE9A(void);

#define FAKE_HANDLE 0x0000FAD0u
#define MAXSTEP 512

typedef struct { unsigned long at, hold; uint16_t dig; uint8_t ana, stick; } Step;
static Step s_step[MAXSTEP];
static int s_n, s_on = -1, s_ms;
static unsigned long s_polls, s_t0;
static uint32_t s_packet = 1, s_last;
static FILE *s_live;
volatile uint64_t cj_pad_roll_ns;
typedef struct { uint16_t dig; uint8_t ana[8]; int16_t lx, ly, rx, ry; } CjHostPad;
extern volatile CjHostPad cj_host_pad;   /* src/gl_backend.c */
static int s_host;
static float s_dz = 0.12f, s_curve = 1.0f; static int s_classic;
typedef struct { double t; uint16_t xb; uint8_t lt, rt; int16_t lx, ly, rx, ry; } ReplayRow;
static ReplayRow *s_rp; static int s_rpn, s_rpi; static double s_rp_anchor = -1, s_rp_start = -1;
static int replay_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256]; int cap = 0;
    const char *a = getenv("CJ_PAD_REPLAY_ANCHOR");
    if (!f) { fprintf(stderr, "[PAD] replay: cannot open %s\n", path); return 0; }
    while (fgets(line, sizeof line, f)) {
        ReplayRow r; unsigned pad, conn, b, lt, rt; int lx, ly, rx, ry;
        if (sscanf(line, "%lf,%u,%u,%x,%u,%u,%d,%d,%d,%d", &r.t, &pad, &conn, &b, &lt, &rt, &lx, &ly, &rx, &ry) != 10 || pad != 0) continue;
        r.xb = (uint16_t)b; r.lt = (uint8_t)lt; r.rt = (uint8_t)rt; r.lx = (int16_t)lx; r.ly = (int16_t)ly; r.rx = (int16_t)rx; r.ry = (int16_t)ry;
        if (s_rpn == cap) { cap = cap ? cap * 2 : 1024; s_rp = realloc(s_rp, sizeof *s_rp * cap); }
        s_rp[s_rpn++] = r;
    }
    fclose(f);
    s_rp_anchor = a ? atof(a) : 0;
    fprintf(stderr, "[PAD] replay: %d rows from %s, anchor %.3f s (starts at the first front-end state)\n", s_rpn, path, s_rp_anchor);
    return s_rpn > 0;
}
/* XInput state at replay time t (seconds in the recording's clock) */
static int replay_state(double t, uint16_t *dig, uint8_t av[8], int16_t *lx, int16_t *ly, int16_t *rx, int16_t *ry)
{
    const ReplayRow *r;
    while (s_rpi + 1 < s_rpn && s_rp[s_rpi + 1].t <= t) s_rpi++;
    if (!s_rpn || s_rp[s_rpi].t > t) return 0;
    r = &s_rp[s_rpi];
    *dig = r->xb & 0xFF;                          /* d-pad, start, back, thumbs: same bits as the Xbox pad */
    av[0] = (r->xb & 0x1000) ? 0xFF : 0; av[1] = (r->xb & 0x2000) ? 0xFF : 0;   /* A B */
    av[2] = (r->xb & 0x4000) ? 0xFF : 0; av[3] = (r->xb & 0x8000) ? 0xFF : 0;   /* X Y */
    av[4] = (r->xb & 0x0200) ? 0xFF : 0; av[5] = (r->xb & 0x0100) ? 0xFF : 0;   /* Black = RB, White = LB */
    av[6] = r->lt; av[7] = r->rt;
    *lx = r->lx; *ly = r->ly; *rx = r->rx; *ry = r->ry;
    return 1;
}
static uint8_t s_rep_ana, s_rep_stick; static uint16_t s_rep_dig;
static unsigned long s_rep_period, s_rep_hold, s_rep_after;
uint32_t cj_pad_polls(void) { return (uint32_t)s_polls; }

static unsigned long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000);
}
static unsigned long tnow(void) { return s_ms ? now_ms() - s_t0 : s_polls; }

static volatile int s_inject_polls;
static volatile uint16_t s_inject_dig;
/* Hold digital buttons `dig` for the next `polls` pad reads (pause menu Quick Save closes the menu). */
/* Presses made by the port last at least `polls` polls and 40 ms per poll: at 60 fps three polls
 * were 50 ms, short enough for the menus to miss a mouse click now and then. */
extern uint64_t xbox_HostNowNs(void);
static volatile uint64_t s_inject_until, s_inject_ana_until;
extern int cj_det_enabled(void);
/* deterministic runs (recordings, replays) keep the frame count only */
static uint64_t inject_ns(int polls) { return cj_det_enabled() ? 0 : xbox_HostNowNs() + (uint64_t)polls * 40000000ull; }
void cj_pad_inject(unsigned short dig, int polls)
{
    s_inject_dig = dig; s_inject_polls = polls;
    s_inject_until = inject_ns(polls);
}
/* The same for the analog face buttons (bit i = A, B, X, Y, Black, White, LT, RT): menu mouse clicks. */
static volatile int s_inject_ana_polls;
static volatile uint8_t s_inject_ana;
void cj_pad_inject_ana(unsigned char mask, int polls)
{
    s_inject_ana = mask; s_inject_ana_polls = polls;
    s_inject_ana_until = inject_ns(polls);
}

static int btn(const char *b, size_t n, uint16_t *dig, uint8_t *ana, uint8_t *stick)
{
    static const char *dn[] = {"up","down","left","right","start","back","lthumb","rthumb"};
    static const char *an[] = {"a","b","x","y","black","white","lt","rt"};
    static const char *sn[] = {"lleft","lright","lup","ldown","rleft","rright","rup","rdown"};
    for (int i = 0; i < 8; i++) {
        if (strlen(dn[i]) == n && !strncasecmp(dn[i], b, n)) { *dig |= (uint16_t)(1u << i); return 1; }
        if (strlen(an[i]) == n && !strncasecmp(an[i], b, n)) { *ana |= (uint8_t)(1u << i); return 1; }
        if (strlen(sn[i]) == n && !strncasecmp(sn[i], b, n)) { *stick |= (uint8_t)(1u << i); return 1; }
    }
    fprintf(stderr, "[PAD] unknown button \"%.*s\"\n", (int)n, b);
    return 0;
}

/* <btns>[:<hold>] starting at p; returns the end. */
static const char *parse_btns(const char *p, Step *st)
{
    for (;;) {
        size_t n = strcspn(p, "+:,\r\n ");
        btn(p, n, &st->dig, &st->ana, &st->stick);
        p += n;
        if (*p != '+') break;
        p++;
    }
    if (*p == ':') st->hold = strtoul(p + 1, (char **)&p, 0);
    return p;
}

static void parse(const char *p)
{
    while (*p && s_n < MAXSTEP) {
        Step st; char *e;
        while (*p && strchr(", \r\n\t", *p)) p++;
        if (!*p) break;
        memset(&st, 0, sizeof st);
        st.hold = s_ms ? 150 : 4;
        st.at = strtoul(p, &e, 0);
        if (e == p || *e != ':') { fprintf(stderr, "[PAD] bad entry at \"%.20s\"\n", p); break; }
        p = parse_btns(e + 1, &st);
        s_step[s_n++] = st;
    }
}

static void init(void)
{
    const char *sc = getenv("CJ_PAD_SCRIPT"), *lv = getenv("CJ_PAD_LIVE"), *rp = getenv("CJ_PAD_REPEAT");
    const char *u = getenv("CJ_PAD_UNIT"), *h = getenv("CJ_PAD_HOST");
    s_on = 0;
    if (h && *h == '1') { s_host = 1; s_on = 1; }
    {
        const char *e;
        if ((e = getenv("CJ_STICK_DEADZONE")) && *e) s_dz = (float)atof(e);
        if ((e = getenv("CJ_STICK_CURVE")) && *e) s_curve = (float)atof(e);
        if ((e = getenv("CJ_STICK_CLASSIC")) && *e == '1') s_classic = 1;
        if (s_dz < 0) s_dz = 0;
        if (s_dz > 0.9f) s_dz = 0.9f;
        if (s_curve <= 0) s_curve = 1.0f;
    }
    s_ms = u && !strcmp(u, "ms");
    if (sc && *sc) {
        if (sc[0] == '@') {
            static char buf[32768]; FILE *f = fopen(sc + 1, "rb"); size_t n = 0;
            if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
            buf[n] = 0; parse(buf);
        } else parse(sc);
        s_on = 1;
    }
    { const char *rp2 = getenv("CJ_PAD_REPLAY"); if (rp2 && *rp2 && replay_load(rp2)) s_on = 1; }
    { extern int cj_rec_mode(void); if (cj_rec_mode()) s_on = 1; }   /* src/cj_rec.c */
    if (lv && *lv) { s_live = fopen(lv, "a+b"); if (s_live) { fseek(s_live, 0, SEEK_END); s_on = 1; } }
    if (rp && *rp) {
        Step st; memset(&st, 0, sizeof st);
        const char *p = rp; size_t n = strcspn(p, ",");
        btn(p, n, &st.dig, &st.ana, &st.stick); p += n;
        s_rep_dig = st.dig; s_rep_ana = st.ana; s_rep_stick = st.stick;
        s_rep_period = (*p == ',') ? strtoul(p + 1, (char **)&p, 0) : 60;
        s_rep_hold = (*p == ',') ? strtoul(p + 1, (char **)&p, 0) : 4;
        if (!s_rep_period) s_rep_period = 60;
        for (int i = 0; i < s_n; i++)
            if (s_step[i].at + s_step[i].hold > s_rep_after) s_rep_after = s_step[i].at + s_step[i].hold;
        s_on = 1;
    }
    if (s_on)
        fprintf(stderr, "[PAD] XAPI pad: %d script step(s), unit %s, live %s, repeat %s, host %s\n",
                s_n, s_ms ? "ms" : "polls", s_live ? lv : "-", rp ? rp : "-",
                s_host ? "controller+keyboard" : "off");
}

static void live_read(void)
{
    char line[256];
    static long pos = -1;
    if (!s_live) return;
    /* re-seek every poll: msvcrt keeps its EOF buffer otherwise and never sees
     * lines appended later (live presses never reached the game under Wine) */
    if (pos < 0) pos = ftell(s_live);
    clearerr(s_live);
    fseek(s_live, pos, SEEK_SET);
    while (fgets(line, sizeof line, s_live)) {
        pos = ftell(s_live);
        Step st; memset(&st, 0, sizeof st);
        if (line[0] == '\n' || line[0] == '#') continue;
        if (!strncmp(line, "stop", 4)) { s_rep_period = 0; fprintf(stderr, "[PAD] live: repeat off\n"); continue; }
        st.hold = s_ms ? 150 : 4;
        parse_btns(line, &st);
        st.at = tnow();
        if (s_n < MAXSTEP) s_step[s_n++] = st;
        fprintf(stderr, "[PAD] live: %s", line);
    }
}

static void state_now(uint16_t *dig, uint8_t *ana, uint8_t *stick)
{
    unsigned long t = tnow();
    *dig = 0; *ana = 0; *stick = 0;
    for (int i = 0; i < s_n; i++) {
        Step *s = &s_step[i];
        if (t >= s->at && t < s->at + s->hold) {
            *dig |= s->dig; *ana |= s->ana; *stick |= s->stick;
            if (t == s->at || s_ms) {
                static int last = -1;
                if (last != i) { last = i; fprintf(stderr, "[PAD] t=%lu press step %d (dig %02x ana %02x stick %02x)\n", t, i, s->dig, s->ana, s->stick); }
            }
        }
    }
    if (s_rep_period && t >= s_rep_after && (t - s_rep_after) % s_rep_period < s_rep_hold) {
        *dig |= s_rep_dig; *ana |= s_rep_ana; *stick |= s_rep_stick;
        if ((t - s_rep_after) % s_rep_period == 0) fprintf(stderr, "[PAD] t=%lu repeat press\n", t);
    }
}

/* Game-state monitor (logged on change, every poll): FEMainMenu state/substate
 * (FEMainMenu::Instance 0x0048CE58, State = [[fe+0x1C0]+0x404]), XApp (XGetApp
 * 0x004931E8): game-state stack depth +0xC98 and top object vtable, level loaded
 * +0xCBC, player control +0xD00, mission +0xD04, cutscene +0xE30. */
#ifdef _WIN32
extern uint32_t cj_gl_flips(void);       /* PE has no weak undefined symbols; gl_backend.c is always linked */
#else
extern uint32_t cj_gl_flips(void) __attribute__((weak));
#endif
static const char *fe_name(uint32_t st)
{
    static const char *n[] = {"None","Main","New_Game","Load_Game","Loaded","Training","Story_Level_Select",
        "Ritual_Challenge_Select","View_Records","Hunting_Gear","Costume_Select","View_Trophies","Save_Game",
        "Options","Options_Controller","Options_Camera","Options_Audio","Options_Game","Autosave","EditName",
        "Credits","Story_Level_Stats","Ritual_Challenge_Times","Done"};
    return st < 24 ? n[st] : "?";
}
static void state_monitor(void)
{
    static uint32_t last[8]; static int first = 1;
    uint32_t v[8] = {0}, fe = MEM32(0x0048CE58u), app = MEM32(0x004931E8u);
    if (fe && MEM32(fe + 0x1C0)) { uint32_t m = MEM32(fe + 0x1C0); v[0] = 1 + MEM32(m + 0x404); v[1] = MEM32(m + 0x408); }
    if (app) {
        v[2] = MEM32(app + 0xC98);
        if (v[2] && MEM32(app + 0xC9C)) { uint32_t g = MEM32(MEM32(app + 0xC9C) + 4); v[3] = g ? MEM32(g) : 0; }
        v[4] = MEM8(app + 0xCBC); v[5] = MEM8(app + 0xD00); v[6] = MEM32(app + 0xD04); v[7] = MEM8(app + 0xE30);
    }
    if (first || memcmp(v, last, sizeof v)) {
        first = 0; memcpy(last, v, sizeof v);
        fprintf(stderr, "[STATE] poll=%lu flip=%u fe=%s/%u gs_depth=%u gs_vt=%08x level_loaded=%u control=%u mission=%u cutscene=%u\n",
                s_polls, cj_gl_flips ? cj_gl_flips() : 0, v[0] ? fe_name(v[0] - 1) : "-", v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
}

static inline uint32_t ARG(int i) { return MEM32(g_esp + 4 + 4 * i); }

/* Raw value the game turns into output o (-1..1) on one axis in dead zone mode 1:
 * out = (raw * sqrt2 - 13107) / 19661 above the threshold (strictly greater). */
static int16_t game_axis_mode1(float o)
{
    float a = fabsf(o), r;
    if (a < 1e-4f) return 0;
    if (a > 1.0f) a = 1.0f;
    r = (a * 19661.0f + 13107.0f) / 1.41421354f + 1.0f;
    if (r > 32767.0f) r = 32767.0f;
    return (int16_t)(o < 0 ? -(int)r : (int)r);
}
/* Host stick (raw, up = +) -> what the game should be given. */
static void cj_stick(int16_t *px, int16_t *py, int stick)
{
    float x = *px / 32767.0f, y = *py / 32767.0f, r = sqrtf(x * x + y * y), m;
    uint32_t gp = MEM32(0x004931ECu);           /* EXBaseGamePad::m_pGamePad */
    int mode = gp ? MEM8(gp + 0x284 + stick) : 1; /* pad 0: [0] left, [1] right */
    {
        static int last[2] = {-1, -1};
        if (mode != last[stick]) {
            last[stick] = mode;
            fprintf(stderr, "[PAD] %s stick: game dead zone mode %d%s\n", stick ? "right" : "left", mode,
                    s_classic ? " (classic: passed through)" : "");
        }
    }
    if (s_classic) return;
    if (r <= s_dz) { *px = *py = 0; return; }
    m = (r > 1.0f ? 1.0f : r);
    m = (m - s_dz) / (1.0f - s_dz);
    if (s_curve != 1.0f) m = powf(m, s_curve);
    x = x / r * m; y = y / r * m;               /* wanted game output, radial */
    if (mode == 1) {                            /* per-axis dead zone (gameplay) */
        *px = game_axis_mode1(x);
        *py = game_axis_mode1(y);
    } else if (mode == 2) {                     /* raw: out = raw * sqrt2 / 32768 */
        *px = (int16_t)(x * 23170.0f);
        *py = (int16_t)(y * 23170.0f);
    } else {                                    /* mode 0: radial dead zone 13107 after sqrt2 */
        float rr = (m * 19661.0f + 13107.0f) / 1.41421354f + 1.0f;
        if (rr > 32767.0f) rr = 32767.0f;
        *px = (int16_t)(x / m * rr);
        *py = (int16_t)(y / m * rr);
    }
}

/* DWORD XGetDevices(PXPP_DEVICE_TYPE) */
void __wrap_m_XGetDevices_0037F68C(void)
{
    if (s_on < 0) init();
    if (!s_on) { __real_m_XGetDevices_0037F68C(); return; }
    g_eax = 1;              /* port 0 */
    g_esp += 8;
}

/* HANDLE XInputOpen(type, port, slot, pollparams) */
void __wrap_m_XInputOpen_0037FBED(void)
{
    if (s_on < 0) init();
    if (!s_on) { __real_m_XInputOpen_0037FBED(); return; }
    fprintf(stderr, "[PAD] XInputOpen port %u -> fake handle\n", ARG(1));
    g_eax = FAKE_HANDLE + ARG(1);
    g_esp += 20;
}

void __wrap_m_XInputClose_0037FC43(void)
{
    if (s_on <= 0) { __real_m_XInputClose_0037FC43(); return; }
    g_esp += 8;
}

/* DWORD XInputSetState(handle, PXINPUT_FEEDBACK): complete at once */
void __wrap_m_XInputSetState_0037FE9A(void)
{
    if (s_on <= 0) { __real_m_XInputSetState_0037FE9A(); return; }
    uint32_t fb = ARG(1);
    if (fb) {
        /* XINPUT_FEEDBACK: the game (EXGamePad::SetPadRumble 0x26ECE0) fills the motor speeds at +66 (left,
         * low-frequency) and +68 (right, high-frequency); pass them to the host controller (CJ_RUMBLE=0: off) */
        extern void cj_host_rumble(uint16_t lo, uint16_t hi);
        cj_host_rumble(MEM16(fb + 66), MEM16(fb + 68));
        MEM32(fb) = 0;  /* Header.dwStatus = ERROR_SUCCESS */
    }
    g_eax = 0;
    g_esp += 12;
}

/* DWORD XInputGetState(handle, PXINPUT_STATE) */
void __wrap_m_XInputGetState_0037FE27(void)
{
    if (s_on <= 0) { __real_m_XInputGetState_0037FE27(); return; }
    uint32_t h = ARG(0), out = ARG(1);
    uint16_t dig = 0; uint8_t ana = 0, stick = 0;
    if (h == FAKE_HANDLE) {
        if (!s_polls) s_t0 = now_ms();
        live_read();
        state_now(&dig, &ana, &stick);
        state_monitor();
        s_polls++;
    }
    uint8_t av[8];
    for (int i = 0; i < 8; i++) av[i] = (ana >> i & 1) ? 0xFF : 0;
    int16_t lx = (stick & 1) ? -32768 : (stick & 2) ? 32767 : 0;
    int16_t ly = (stick & 8) ? -32768 : (stick & 4) ? 32767 : 0;
    int16_t rx = (stick & 0x10) ? -32768 : (stick & 0x20) ? 32767 : 0;
    int16_t ry = (stick & 0x80) ? -32768 : (stick & 0x40) ? 32767 : 0;
    if (s_rpn && h == FAKE_HANDLE) {
        /* time zero: the first front-end state (state_monitor saw a menu) */
        uint32_t fe = MEM32(0x0048CE58u);
        double rt = 0;
        static double s_sync = -2, s_gt;
        static float s_sp[3]; static int s_has_sp;
        if (s_sync == -2) {
            const char *e = getenv("CJ_PAD_REPLAY_SYNC"), *at;
            s_sync = e && *e ? atof(e) : -1;
            if (e && (at = strchr(e, '@')) && sscanf(at + 1, "%f,%f,%f", &s_sp[0], &s_sp[1], &s_sp[2]) == 3) s_has_sp = 1;
        }
        if (s_sync >= 0) {
            /* game clock: start when the player first gains control in a level, at recording time
             * CJ_PAD_REPLAY_SYNC (its "control_gained" event), then advance by the game's own frame
             * length (0x396CEC+12, what the recorder logs as frame_len), so a slow host replays at
             * game speed, not wall-clock speed */
            uint32_t app = MEM32(0x004931E8u), pl = MEM32(0x0048FC98u);
            int near = 1;
            if (s_has_sp && pl) {   /* <seconds>@x,y,z: also wait until the player is at the recording's start */
                float p[3]; uint32_t v; int k;
                for (k = 0; k < 3; k++) { v = MEM32(pl + 0x2C8 + 4 * k); memcpy(&p[k], &v, 4); }
                near = fabsf(p[0] - s_sp[0]) + fabsf(p[1] - s_sp[1]) + fabsf(p[2] - s_sp[2]) < 1.5f;
            }
            if (s_rp_start < 0 && app && MEM32(app + 0xCBC) && MEM32(app + 0xD00) && pl && near
                    && !(fe && MEM32(fe + 0x1C0))) {
                s_rp_start = 0; s_gt = 0;
                fprintf(stderr, "[PAD] replay: synced to %.3f s at poll %lu\n", s_sync, s_polls);
            } else if (s_rp_start >= 0) {
                float fl; uint32_t u = MEM32(0x00396CECu + 12); memcpy(&fl, &u, 4);
                if (fl > 0 && fl < 0.5f) s_gt += fl;
                if (pl && (s_polls % 15) == 0) {
                    float x, y, z, hp; uint32_t v;
                    v = MEM32(pl + 0x2C8); memcpy(&x, &v, 4); v = MEM32(pl + 0x2CC); memcpy(&y, &v, 4);
                    v = MEM32(pl + 0x2D0); memcpy(&z, &v, 4); v = MEM32(pl + 0xC5C); memcpy(&hp, &v, 4);
                    fprintf(stderr, "[RP] t=%.3f x=%.3f y=%.3f z=%.3f hp=%.1f\n", s_sync + s_gt, x, y, z, hp);
                }
            }
            rt = s_sync + s_gt;
        } else {
            if (s_rp_start < 0 && fe && MEM32(fe + 0x1C0)) {
                s_rp_start = now_ms() / 1000.0;
                fprintf(stderr, "[PAD] replay: started at poll %lu\n", s_polls);
            }
            rt = s_rp_anchor + now_ms() / 1000.0 - s_rp_start;
        }
        if (s_rp_start >= 0) {
            uint16_t rd = 0; uint8_t ra[8] = {0}; int16_t a, b, c, d;
            if (replay_state(rt, &rd, ra, &a, &b, &c, &d)) {
                dig |= rd;
                for (int i = 0; i < 8; i++) if (ra[i] > av[i]) av[i] = ra[i];
                if (a) lx = a;
                if (b) ly = b;
                if (c) rx = c;
                if (d) ry = d;
            }
        }
    }
    if (s_host && h == FAKE_HANDLE) {
        CjHostPad hp;
        memcpy(&hp, (const void *)&cj_host_pad, sizeof hp);
        cj_stick(&hp.lx, &hp.ly, 0);
        cj_stick(&hp.rx, &hp.ry, 1);
        dig |= hp.dig;
        for (int i = 0; i < 8; i++) if (hp.ana[i] > av[i]) av[i] = hp.ana[i];
        if (hp.lx) lx = hp.lx;
        if (hp.ly) ly = hp.ly;
        if (hp.rx) rx = hp.rx;
        if (hp.ry) ry = hp.ry;
        {   /* first 40 host input changes, for test reports */
            static CjHostPad last; static int nlog;
            if (nlog < 40 && memcmp(&hp, &last, sizeof hp)) {
                nlog++; last = hp;
                fprintf(stderr, "[PAD] host input: dig %04x ana %02x%02x%02x%02x%02x%02x%02x%02x sticks %d,%d %d,%d\n",
                        hp.dig, hp.ana[0], hp.ana[1], hp.ana[2], hp.ana[3], hp.ana[4], hp.ana[5], hp.ana[6], hp.ana[7],
                        hp.lx, hp.ly, hp.rx, hp.ry);
            }
        }
    }
    if (h == FAKE_HANDLE && (s_inject_polls > 0 || (s_inject_dig && xbox_HostNowNs() < s_inject_until))) {   /* a press made by the port */
        dig |= s_inject_dig;
        if (s_inject_polls > 0) s_inject_polls--;
        if (s_inject_polls <= 0 && xbox_HostNowNs() >= s_inject_until) s_inject_dig = 0;
    }
    if (h == FAKE_HANDLE && (s_inject_ana_polls > 0 || (s_inject_ana && xbox_HostNowNs() < s_inject_ana_until))) {
        for (int i = 0; i < 8; i++) if (s_inject_ana & (1u << i)) av[i] = 255;
        if (s_inject_ana_polls > 0) s_inject_ana_polls--;
        if (s_inject_ana_polls <= 0 && xbox_HostNowNs() >= s_inject_ana_until) s_inject_ana = 0;
    }
    if (h == FAKE_HANDLE) {   /* session recording / exact replay (src/cj_rec.c) */
        extern void cj_rec_pad(uint16_t *, uint8_t *, int16_t *, int16_t *, int16_t *, int16_t *);
        cj_rec_pad(&dig, av, &lx, &ly, &rx, &ry);
    }
    {   /* the roll button (right stick click, Left Ctrl on keyboard): time of the last press,
           for the movement mods (src/native/hybrid/cjmod_move.c) */
        extern uint64_t xbox_HostNowNs(void);
        static uint16_t prev;
        if ((dig & 0x80) && !(prev & 0x80)) cj_pad_roll_ns = xbox_HostNowNs();
        prev = dig;
    }
    {   /* new packet number whenever anything changed */
        uint32_t key = dig ^ (uint32_t)lx * 31u ^ (uint32_t)ly * 977u ^ (uint32_t)rx * 7919u ^ (uint32_t)ry * 104729u;
        for (int i = 0; i < 8; i++) key = key * 131u + av[i];
        if (key != s_last) { s_last = key; s_packet++; }
    }
    MEM32(out) = s_packet;
    MEM16(out + 4) = dig;
    for (int i = 0; i < 8; i++) MEM8(out + 6 + i) = av[i];
    MEM16(out + 14) = (uint16_t)lx; MEM16(out + 16) = (uint16_t)ly;
    MEM16(out + 18) = (uint16_t)rx; MEM16(out + 20) = (uint16_t)ry;
    g_eax = 0;
    g_esp += 12;
}
