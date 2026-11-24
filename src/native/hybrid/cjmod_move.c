extern unsigned int g_xbox_contig_size;   /* xboxrecomp kernel.h: the contiguous window (uint32_t), 64 MB unless RECOMP_CONTIG_MB */
/* Player movement dials (pcj.ini; docs/movement-dials.md). Percentages, 100 = the original.
 *   CJ_JUMP_HEIGHT   every player jump's target height (XItemPhysics::SetThrustToAttainHeight,
 *                    mods/XItemPhysics.cpp; the player's physics only)
 *   CJ_GRAVITY       the player's gravity (XItemPhysics::m_Gravity, retail 0.016333334). Jump
 *                    thrust is computed from gravity, so jumps reach the same height with less
 *                    hang time when it is raised: heavier, quicker jumps.
 *   CJ_RUN_ACCEL     how fast running speeds up (exponent on the per-frame 0.8 gap factor)
 *   CJ_RUN_STOP      how fast running slows down (exponent on the per-frame 0.9375 slide; 300 = 3x)
 *   CJ_TURN_RATE     how fast the run turns (InterpAngle rate 0.12)
 *   (run dials: mods/Player.cpp MoveNormal)
 *   CJ_SNAPPY_STOP=1 releasing the stick at speed stops on the spot (mods/Player.cpp CheckForStop)
 * Not applied in deterministic mode (recordings): they change the simulation. */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

extern void *xbox_GetMemoryOffset(void);
extern int cj_det_enabled(void);

#define VA_PLAYER_INSTANCE 0x0048FC98u   /* XItemHandler_Player::m_pInstance */
#define RETAIL_GRAVITY 0.016333334f

static float pct(const char *k)
{
    const char *e = getenv(k);
    float v;
    if (!e || !*e) return 1.0f;
    v = (float)atof(e);
    if (!(v >= 10.0f && v <= 1000.0f)) return 1.0f;
    return v / 100.0f;
}

float cjmod_jump_scale(void)
{
    static float s = -1.0f;
    if (cj_det_enabled()) return 1.0f;
    if (s < 0.0f) {
        s = pct("CJ_JUMP_HEIGHT");
        if (s != 1.0f) fprintf(stderr, "[MOVE] jump height %.0f%%\n", s * 100.0f);
    }
    return s;
}

void gamelog_event(const char *type, const char *fmt, ...);   /* gamelog.c */
static void charge_reset_tick(uint8_t *mem, uint32_t h);
static void nan_guard_tick(uint8_t *mem, uint32_t h);
static void boss_poise_tick(uint8_t *mem);
extern uint64_t xbox_HostNowNs(void);

/* Every frame (cj_fps_tick): keep the player's gravity at the dial's value, and clear a jump
 * charge that ended without a jump (charge_reset_tick). */
void cjmod_move_tick(void)
{
    static float g = -1.0f;
    uint8_t *mem;
    uint32_t h, item, phys;
    if (g < 0.0f) {
        g = pct("CJ_GRAVITY");
        if (g != 1.0f) fprintf(stderr, "[MOVE] player gravity %.0f%%\n", g * 100.0f);
    }
    if (cj_det_enabled()) return;
    mem = (uint8_t *)xbox_GetMemoryOffset();
    h = *(volatile uint32_t *)(mem + VA_PLAYER_INSTANCE);
    if (h < 0x10000u || h >= (0x80000000u + g_xbox_contig_size)) return;
    charge_reset_tick(mem, h);
    nan_guard_tick(mem, h);
    boss_poise_tick(mem);
    if (g == 1.0f) return;
    item = *(volatile uint32_t *)(mem + h + 4);
    if (item < 0x10000u || item >= (0x80000000u + g_xbox_contig_size)) return;
    phys = *(volatile uint32_t *)(mem + item + 0x148);
    if (phys < 0x10000u || phys >= (0x80000000u + g_xbox_contig_size)) return;
    {
        volatile float *pg = (volatile float *)(mem + phys + 0x34);
        float want = RETAIL_GRAVITY * g;
        /* only overwrite the retail value or our own (a script that sets another gravity wins) */
        if (*pg == RETAIL_GRAVITY) *pg = want;
    }
}

static float dial(const char *k, float *cache)
{
    if (*cache < 0.0f) {
        *cache = pct(k);
        if (*cache != 1.0f) fprintf(stderr, "[MOVE] %s %.0f%%\n", k, *cache * 100.0f);
    }
    return cj_det_enabled() ? 1.0f : *cache;
}
float cjmod_run_accel(void) { static float c = -1.0f; return dial("CJ_RUN_ACCEL", &c); }
float cjmod_run_stop(void) { static float c = -1.0f; return dial("CJ_RUN_STOP", &c); }
float cjmod_turn_rate(void) { static float c = -1.0f; return dial("CJ_TURN_RATE", &c); }
float cjmod_pow(float x, float y) { return (float)pow((double)x, (double)y); }
double cjmod_powd(double x, double y) { return pow(x, y); }
int cjmod_snappy_stop(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("CJ_SNAPPY_STOP");
        v = e && *e == '1';
        if (v) fprintf(stderr, "[MOVE] snappy stop: no run-stop slide\n");
    }
    return cj_det_enabled() ? 0 : v;
}

/* CJ_ANIM_TEST=<mode hash>[,<mode hash>...] (testing): while idle, play the next listed animation
 * mode every CJ_ANIM_TEST_EVERY idle frames (default 120), cycling. mods/Player.cpp DoIdle. */
unsigned int cjmod_anim_test(void)
{
    static int init, n, every = 120, at;
    static unsigned int list[32];
    static unsigned long calls;
    if (!init) {
        const char *e = getenv("CJ_ANIM_TEST"), *ev = getenv("CJ_ANIM_TEST_EVERY");
        init = 1;
        while (e && *e && n < 32) {
            char *end;
            list[n++] = (unsigned int)strtoul(e, &end, 16);
            e = (*end == ',') ? end + 1 : NULL;
        }
        if (ev) every = atoi(ev) > 0 ? atoi(ev) : 120;
        if (n) fprintf(stderr, "[ANIMTEST] %d mode(s), every %d idle frames\n", n, every);
    }
    if (!n || cj_det_enabled()) return 0;
    if (++calls % (unsigned long)every) return 0;
    {
        unsigned int h = list[at++ % n];
        fprintf(stderr, "[ANIMTEST] mode %08X\n", h);
        return h;
    }
}

/* ---- "Hunter's momentum" movement (docs/movement-design.md), CJ_MOVE_STYLE=classic turns it off.
 * Off in deterministic mode (recordings). */
extern uint64_t xbox_HostNowNs(void);
extern volatile uint64_t cj_pad_roll_ns;     /* src/cj_pad.c: last press of the roll button */
int cjmod_move_modern(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("CJ_MOVE_STYLE");
        v = !(e && (e[0] == 'c' || e[0] == 'C' || e[0] == '0'));
        fprintf(stderr, "[MOVE] style: %s\n", v ? "modern (move while charging, land roll)" : "classic");
    }
    return cj_det_enabled() ? 0 : v;
}
/* fraction of the stick's run speed while charging a jump (CJ_CHARGE_MOVE percent, default 60) */
float cjmod_charge_move(void)
{
    static float c = -1.0f;
    if (c < 0.0f) {
        const char *e = getenv("CJ_CHARGE_MOVE");
        c = e && *e ? (float)atof(e) / 100.0f : 0.6f;
        if (!(c >= 0.0f && c <= 1.0f)) c = 0.6f;
    }
    return c;
}
/* the roll button was pressed within the last `ms` milliseconds (and not used yet) */
static uint64_t s_roll_used;
int cjmod_roll_pressed_within(int ms)
{
    uint64_t t = cj_pad_roll_ns, now = xbox_HostNowNs();
    static int autoroll = -1;
    if (autoroll < 0) autoroll = getenv("CJ_ROLL_AUTO") != NULL;   /* tests: always pressed */
    if (autoroll) return 1;
    if (!t || t == s_roll_used || now - t > (uint64_t)ms * 1000000ull) return 0;
    return 1;
}
void cjmod_roll_consume(void) { s_roll_used = cj_pad_roll_ns; }
int cjmod_roll_window_ms(void)
{
    static int w = -1;
    if (w < 0) { const char *e = getenv("CJ_ROLL_WINDOW"); w = e && *e ? atoi(e) : 150; if (w < 30 || w > 1000) w = 150; }
    return w;
}
/* the roll in progress: its velocity (m_Speed x, z) and how long it may last */
static float s_rx, s_rz;
static uint64_t s_roll_end;
void cjmod_roll_start(float sx, float sz)
{
    s_rx = sx; s_rz = sz;
    s_roll_end = xbox_HostNowNs() + 1200000000ull;   /* safety: never longer than 1.2 s */
    fprintf(stderr, "[MOVE] land roll (speed %.2f)\n", sqrtf(sx * sx + sz * sz));
}
int cjmod_roll_active(void) { return s_roll_end && xbox_HostNowNs() < s_roll_end; }
void cjmod_roll_end(void) { s_roll_end = 0; }
float cjmod_roll_vx(void) { return s_rx; }
float cjmod_roll_vz(void) { return s_rz; }
/* a hard landing just happened (for a roll pressed right after touchdown) */
static uint64_t s_hard_t;
static float s_hard_h0, s_hard_vx, s_hard_vz;
void cjmod_hard_land(float health_before, float vx, float vz) { s_hard_t = xbox_HostNowNs(); s_hard_h0 = health_before; s_hard_vx = vx; s_hard_vz = vz; }
int cjmod_hard_land_recent(int ms) { return s_hard_t && xbox_HostNowNs() - s_hard_t < (uint64_t)ms * 1000000ull; }
float cjmod_hard_land_health(void) { return s_hard_h0; }
float cjmod_hard_land_vx(void) { return s_hard_vx; }
float cjmod_hard_land_vz(void) { return s_hard_vz; }
void cjmod_hard_land_clear(void) { s_hard_t = 0; }
void cjmod_trace_charge(float stick, float pad, float speed, unsigned flags, float timer)
{
    static int on = -1;
    if (on < 0) on = getenv("CJ_MOVE_TRACE") != NULL;
    if (on) fprintf(stderr, "[MOVE] charge: stick %.2f pad %.2f speed %.3f flags %08x timer %.2f\n", stick, pad, speed, flags, timer);
}
/* quick landing: hard landings with an impact slower than CJ_QUICK_LAND_MAX (per frame, default
 * 1.3, about a 50-unit drop) use the quick land-to-run; faster ones the original recovery */
float cjmod_quick_land_min(void)
{
    static float m = -1.0f;
    if (m < 0.0f) {
        const char *e = getenv("CJ_QUICK_LAND_MAX");
        m = e && *e ? (float)atof(e) : 1.3f;
        if (!(m > 0.0f && m < 100.0f)) m = 1.3f;
    }
    return -m;
}

/* ---- modern movement, the base (2026-10-27 afternoon) -------------------------------------
 * Speed curve: the original maps inertia (0..1, the stick through easing) to ground speed in
 * three straight pieces (walk to 0.45, jog to 0.9, then the run doubles in the last tenth);
 * modern uses one smooth curve with the same top speed: 0.09375 * f^CJ_SPEED_CURVE (2.2). */
static float run_curve_p(void)
{
    static float p = -1.0f;
    if (p < 0.0f) { const char *e = getenv("CJ_SPEED_CURVE"); p = e && *e ? (float)atof(e) : 2.2f; if (!(p >= 1.0f && p <= 4.0f)) p = 2.2f; }
    return p;
}
float cjmod_run_curve(float f)
{
    if (0.0001f >= f) return 0.0f;
    if (f > 1.2f) f = 1.2f;
    return 0.09375f * powf(f, run_curve_p());
}
static float orig_run(float f)
{
    if (0.0001f >= f) return 0.0f;
    if (0.45f >= f) return f * 2.2222223f * 0.01171875f;
    if (0.9f >= f) return (f - 0.45f) * 2.2222223f * 0.03515625f + 0.01171875f;
    return (f - 0.9f) * 10.0f * 0.046875f + 0.046875f;
}
/* inertia that gives ground speed g under the original curve */
float cjmod_run_inv(float g)
{
    if (g <= 0.0f) return 0.0f;
    if (g <= 0.01171875f) return g / (2.2222223f * 0.01171875f);
    if (g <= 0.046875f) return (g - 0.01171875f) / (2.2222223f * 0.03515625f) + 0.45f;
    (void)orig_run;
    return (g - 0.046875f) / 0.46875f + 0.9f;
}
/* ground speed for a stick/inertia value under the curve in use */
float cjmod_ground(float f) { return cjmod_move_modern() ? cjmod_run_curve(f) : orig_run(f); }
/* the game's InertiaToRunSpeed stays the original (its x87 result feeds the movement unrounded);
 * the modern curve is applied by remapping the run's target inertia: the inertia whose original
 * ground speed equals the smooth curve's */
float cjmod_run_remap(float mag) { return cjmod_move_modern() ? cjmod_run_inv(cjmod_run_curve(mag)) : mag; }
/* turning: InterpAngle rate multiplier (modern 1.85: 0.12 -> 0.22), and how much a turn costs */
float cjmod_turn_mult(void) { return cjmod_move_modern() ? 1.85f : 1.0f; }
float cjmod_turn_cost(void) { return cjmod_move_modern() ? 1.0f : 2.0f; }
/* air control: the stick's push per frame in a free jump (original 0.015; modern x CJ_AIR_CONTROL%,
 * default 250); the horizontal cap stays the original's (air control steers, it does not speed up) */
float cjmod_air_push(void)
{
    static float k = -1.0f;
    if (k < 0.0f) { const char *e = getenv("CJ_AIR_CONTROL"); k = e && *e ? (float)atof(e) / 100.0f : 2.5f; if (!(k >= 0.0f && k <= 10.0f)) k = 2.5f; }
    return 0.01499968f * (cjmod_move_modern() ? k : 1.0f);
}
/* run stop: modern ends the stop animation at this fraction (CJ_STOP_LENGTH percent, default 30) */
float cjmod_stop_frac(void)
{
    static float k = -1.0f;
    if (k < 0.0f) { const char *e = getenv("CJ_STOP_LENGTH"); k = e && *e ? (float)atof(e) / 100.0f : 0.3f; if (!(k > 0.0f && k <= 1.0f)) k = 0.3f; }
    return cjmod_move_modern() ? k : 2.0f;
}

/* Running charge (modern movement): holding jump while running keeps the run going (at CJ_CHARGE_MOVE
 * percent) and charges in the background; the crouch shows only on release, for CJ_CHARGE_CROUCH ms
 * (default 100), then the jump with the power of the hold. State lives one frame: a frame without
 * cjmod_run_charge_touch (the player left the run: fell, attacked, was hit) ends it. */
/* "Last touched" is host time, not a frame count: cj_det_frame only advances in deterministic mode, so in
 * normal play a charge interrupted by a jump or a ledge grab never expired and fired a jump the next time
 * the player ran (2026-10-28 session). 150 ms without a touch ends it (several frames even at 20 fps). */
#define CHARGE_STALE_NS 150000000ull
static int s_rc; static uint64_t s_rc_ns; static float s_crouch_hold;
/* Bunny hop, the real one (session-20261116-230950, frame 68871): releasing a running charge starts a
 * 0.1 s crouch before the launch; pressing jump again inside it carried the old charge on (DoCrouch's
 * "button held: keep charging"), so a short second press launched at the first press's power plus its
 * own. A released charge is committed: until the crouch ends in a jump, a new press cannot extend it. */
static int s_committed;
void cjmod_charge_commit(void) { s_committed = 1; }
int cjmod_charge_committed(void) { return s_committed; }
int cjmod_run_charging(void)
{
    if (s_rc && xbox_HostNowNs() - s_rc_ns > CHARGE_STALE_NS) s_rc = 0;
    return s_rc;
}
void cjmod_run_charge_touch(int on) { s_rc = on; s_rc_ns = xbox_HostNowNs(); }

/* Bunny-hop fix (patch 0.2.0). The jump charge lives in XItemHandler_Player::m_CrouchTimer (+0x22c)
 * and only DoCrouch's launch clears it. A charge that ends any other way (the running charge
 * expiring after a fall, a hit, an attack or a ledge grab; a standing charge cut off by a fall)
 * left its time there, and the next jump started from idle, however short the tap, launched at
 * that power: a tap gave the full charged jump. Outside the crouch (mode 2) and with no running
 * charge in progress, a charge is over, so its time and any pending release crouch are cleared.
 * CJ_CHARGE_FIX=0 turns it off (the old behaviour, for comparison). */
static void charge_reset_tick(uint8_t *mem, uint32_t h)
{
    static int on = -1;
    volatile uint32_t *mode = (volatile uint32_t *)(mem + h + 0x20c);
    volatile float *timer = (volatile float *)(mem + h + 0x22c);
    if (on < 0) { const char *e = getenv("CJ_CHARGE_FIX"); on = !(e && *e == '0'); }
    {   /* Every jump: height above its take-off point and how long the crouch before it lasted, to
         * the session log (events.jsonl "jump"; stderr with CJ_CHARGE_LOG=1). A bunny hop shows as a
         * short crouch with a full-charge height. */
        static int lg = -1, in_air, crouch_frames, last_crouch; static float y0, ymax, charge0;
        uint32_t item = *(volatile uint32_t *)(mem + h + 4);
        float y = item >= 0x10000u && item < (0x80000000u + g_xbox_contig_size) ? *(volatile float *)(mem + item + 0xcc) : 0.0f;
        if (lg < 0) { const char *e = getenv("CJ_CHARGE_LOG"); lg = e && *e == '1'; }
        if (*mode == 2) crouch_frames++;
        if (*mode == 3 && !in_air) { in_air = 1; y0 = ymax = y; charge0 = *timer; last_crouch = crouch_frames; }
        else if (*mode == 3) { if (y > ymax) ymax = y; }
        else if (in_air) {
            in_air = 0;
            gamelog_event("jump", "\"height\":%.2f,\"crouch_frames\":%d,\"charge_s\":%.2f", ymax - y0, last_crouch, charge0);
            if (lg) fprintf(stderr, "[MOVE] jump height %.2f (crouch %d frames, charge %.2f s)\n", ymax - y0, last_crouch, charge0);
        }
        if (*mode != 2 && *mode != 3) crouch_frames = 0;
    }
    {   /* CJ_MOVE_TRACE: every mode change with the jump button and stick (movement tests) */
        static int tr = -1; static uint32_t pm = ~0u, ps = ~0u; static unsigned long n;
        uint32_t sub = *(volatile uint32_t *)(mem + h + 0x210);
        if (tr < 0) tr = getenv("CJ_MOVE_TRACE") != NULL;
        n++;
        if (tr && (*mode != pm || sub != ps)) {
            fprintf(stderr, "[MOVE] t%lu mode %u.%u jump %d%d stick %.2f speed %.3f charge %.2f\n", n, *mode, sub,
                    (*(volatile uint32_t *)(mem + h + 0xf0) & 0x20) != 0, (*(volatile uint32_t *)(mem + h + 0xf4) & 0x20) != 0,
                    *(volatile float *)(mem + h + 0xdc), *(volatile float *)(mem + h + 0x2f8), *timer);
            pm = *mode; ps = sub;
        }
    }
    if (*mode != 2) s_committed = 0;
    if (!on || *mode == 2 || cjmod_run_charging()) return;
    if (*timer != 0.0f) {
        static int logged;
        if (logged++ < 20) fprintf(stderr, "[MOVE] stale jump charge cleared (%.2f s, mode %u)\n", *timer, *mode);
        gamelog_event("charge_cleared", "\"charge_s\":%.2f,\"mode\":%u", *timer, *mode);
        *timer = 0.0f;
    }
    s_crouch_hold = 0.0f;
}
/* Broken-physics recovery (2026-11-16): blown back mid-jump by mech rockets, the player's position
 * went NaN the next frame; the blown-back fall (react mode 0x17, sub 5) waits for a landing that
 * never comes, so the game ran on with the Predator gone. Every frame: remember the last state with
 * finite position, rotation and velocity; if any turns non-finite, put him back there with no
 * velocity (he falls and lands, ending the reaction) and log which values broke ("nan_recover"),
 * with the previous frame's, to find the source. CJ_NAN_GUARD=0 turns it off. */
/* x, y, z only: a w lane can hold anything */
static int fin4(const volatile float *v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }
static void nan_guard_tick(uint8_t *mem, uint32_t h)
{
    static int on = -1, have, logged;
    static float pos[4], rot[4], prev[16];
    static uint32_t pmode, psub, last_item;
    uint32_t item, phys;
    volatile float *p, *r, *dm, *u2b8 = (volatile float *)(mem + h + 0x2b8), *spd = (volatile float *)(mem + h + 0x2e8);
    uint32_t mode = *(volatile uint32_t *)(mem + h + 0x20c), sub = *(volatile uint32_t *)(mem + h + 0x210);
    int bp, br, bd, bu, bs;
    if (on < 0) { const char *e = getenv("CJ_NAN_GUARD"); on = !(e && *e == '0'); }
    if (!on) return;
    item = *(volatile uint32_t *)(mem + h + 4);
    if (item < 0x10000u || item >= (0x80000000u + g_xbox_contig_size)) { have = 0; return; }
    if (item != last_item) { last_item = item; have = 0; }   /* a new level's player: never restore an old position */
    phys = *(volatile uint32_t *)(mem + item + 0x148);
    if (phys < 0x10000u || phys >= (0x80000000u + g_xbox_contig_size)) { have = 0; return; }
    p = (volatile float *)(mem + item + 0xc8); r = (volatile float *)(mem + item + 0xd8); dm = (volatile float *)(mem + phys + 0x50);
    bp = !fin4(p); br = !fin4(r); bd = !fin4(dm); bu = !fin4(u2b8); bs = !fin4(spd);
    if (!(bp | br | bd | bu | bs)) {
        int i;
        for (i = 0; i < 4; i++) { pos[i] = p[i]; rot[i] = r[i]; prev[i] = dm[i]; prev[4 + i] = u2b8[i]; prev[8 + i] = spd[i]; }
        prev[12] = p[0]; prev[13] = p[1]; prev[14] = p[2];
        pmode = mode; psub = sub; have = 1;
        return;
    }
    if (logged++ < 20) {
        fprintf(stderr, "[MOVE] broken player physics (pos %d rot %d vel %d move %d speed %d, mode %u.%u, was %u.%u): %s\n",
                bp, br, bd, bu, bs, mode, sub, pmode, psub, have ? "restored last good position" : "no good position yet");
        gamelog_event("nan_recover", "\"bad\":\"%s%s%s%s%s\",\"mode\":%u,\"sub\":%u,\"prev_mode\":%u,\"prev_sub\":%u,"
                      "\"prev_vel\":[%.3f,%.3f,%.3f],\"prev_move\":[%.3f,%.3f],\"prev_speed\":[%.3f,%.3f],\"pos\":[%.2f,%.2f,%.2f],\"restored\":%d",
                      bp ? "pos " : "", br ? "rot " : "", bd ? "vel " : "", bu ? "move " : "", bs ? "speed" : "",
                      mode, sub, pmode, psub, prev[0], prev[1], prev[2], prev[4], prev[6], prev[8], prev[10], prev[12], prev[13], prev[14], have);
    }
    if (!have) return;
    {
        int i;
        for (i = 0; i < 3; i++) {
            p[i] = pos[i]; dm[i] = 0.0f; u2b8[i] = 0.0f; spd[i] = 0.0f;
            if (br) r[i] = rot[i];
        }
        *(volatile int8_t *)(mem + item + 0x7e) = 0;   /* m_validMtx: rebuild the matrix from the fixed values */
        { volatile float *lp = (volatile float *)(mem + h + 0x2c8); for (i = 0; i < 3; i++) lp[i] = pos[i]; }   /* m_LastPos */
    }
}

/* How long jump must be held for the full-power jump. The original's 1/3 s made a slightly long tap
 * (0.35-0.45 s, easy while running with the button held) launch the full jump (James, 2026-11-16).
 * CJ_POWER_JUMP_MS (modern movement only). Default: the original 1/3 s; 500 ms felt less responsive
 * in play (James, 2026-11-16), so it stays opt-in. Classic: always the original. */
float cjmod_power_time(void)
{
    static float t = -1.0f;
    if (!cjmod_move_modern()) return 0.33333334f;
    if (t < 0.0f) { const char *e = getenv("CJ_POWER_JUMP_MS"); t = e && *e ? (float)atof(e) / 1000.0f : 0.33333334f; if (!(t >= 0.1f && t <= 2.0f)) t = 0.33333334f; }
    return t;
}
/* the run's target inertia (original curve) while charging: CJ_CHARGE_MOVE percent of its ground speed */
float cjmod_charge_cap(float mag) { return cjmod_run_charging() ? cjmod_run_inv(orig_run(mag) * cjmod_charge_move()) : mag; }
void cjmod_crouch_hold_start(void)
{
    const char *e = getenv("CJ_CHARGE_CROUCH");
    s_crouch_hold = (e ? (float)atof(e) : 100.0f) / 1000.0f;
}
/* 1 while the release crouch is still showing (dt: this frame's length in seconds) */
int cjmod_crouch_hold(float dt)
{
    if (s_crouch_hold <= 0.0f) return 0;
    s_crouch_hold -= dt;
    return 1;
}

/* Charge feedback: a firm pulse when the hold reaches the strongest jump available here (power jump
 * after 1/3 s when allowed, else the 0.1 s medium jump), then a light hum while it is held. Host-side
 * only (no game state). CJ_CHARGE_RUMBLE=0 off, or a percent. */
extern void cj_host_rumble_timed(uint16_t lo, uint16_t hi, uint32_t ms);
static int s_fb_state;   /* 0 below the threshold, 1 pulsed */
static uint64_t s_fb_ns;
void cjmod_charge_feedback(float timer, int power_ok, int medium_ok)
{
    static int pct = -1;
    if (pct < 0) { const char *e = getenv("CJ_CHARGE_RUMBLE"); pct = e ? atoi(e) : 100; }
    if (!pct) return;
    if (xbox_HostNowNs() - s_fb_ns > CHARGE_STALE_NS) s_fb_state = 0;   /* a new hold */
    s_fb_ns = xbox_HostNowNs();
    float need = power_ok ? cjmod_power_time() : (medium_ok ? 0.1f : 1e9f);
    if (timer < need) return;
    uint32_t k = (uint32_t)pct;
    if (!s_fb_state) { s_fb_state = 1; cj_host_rumble_timed((uint16_t)(0xC000u * k / 100u), (uint16_t)(0xFFFFu * k / 100u > 0xFFFFu ? 0xFFFFu : 0xFFFFu * k / 100u), 140); }
    else cj_host_rumble_timed((uint16_t)(0x1800u * k / 100u), 0, 80);
}

/* Melee attacks (DoHit, modern movement): the fraction of the speed kept per frame, frame-rate corrected
 * (CJ_ATTACK_CARRY percent per game-rate unit, default 80: about a third of a second to stop). 0 = the original stop. */
float cjmod_attack_carry(float mul)
{
    static float base = -1.0f;
    if (base < 0.0f) { const char *e = getenv("CJ_ATTACK_CARRY"); base = (e ? (float)atof(e) : 80.0f) / 100.0f; if (base > 0.98f) base = 0.98f; }
    if (base <= 0.0f) return 0.0f;
    return powf(base, mul);   /* mul: XApp::GameFrameRateMul(), as the game's own per-frame decays use */
}


/* CJ_GRAB_LOG=1 (mods/Player.cpp DoJump): falling ledge-catch diagnostics. what 0: a ledge found
 * (a = its height above the hands, b = horizontal distance), 1: facing refused (a = angle),
 * 2: no room along the ledge, 3: shimmy angle refused. */
void cjmod_grab_log(int what, int flags, float a, float b, float mul, float probe_y)
{
    static int on = -1;
    static unsigned n;
    if (on < 0) { const char *e = getenv("CJ_GRAB_LOG"); on = e && *e == '1'; }
    if (!on || n++ > 4000) return;
    fprintf(stderr, "[GRAB] %d flags=%x a=%.3f b=%.3f mul=%.3f hands=%.3f\n", what, flags, a, b, mul, probe_y);
}

/* Boss bar on damage (mods/FEHud.cpp): the last health seen per actor slot; returns the previous
 * value and stores the new one (h < 0: no boss in that slot). */
float cjmod_boss_health(int slot, float h)
{
    static float last[128];
    float prev;
    if (slot < 0 || slot >= 128) return -1.0f;
    prev = last[slot];
    last[slot] = h;
    return prev;
}

/* Boss poise (CJ_BOSS_POISE=1; launcher PC Settings > Experimental). The original lets any hit
 * restart an enemy's hit reaction (ActorUtil.cpp AR_ReactFromHit pops STATE_HitReact and pushes a
 * new one), so a boss caught in a combo flinches until it dies. With poise on, a boss (Actor
 * m_ActorFlags.m_bBoss, +0x940 bit 15) that takes a quick run of hits (m_ConsecutiveHitCounter +0x860,
 * which the game raises for each hit landing within 1 s of the last) stops flinching for a few
 * seconds: m_StateFlags.m_bDontDoHitReaction (+0x93c bit 4), which AR_ReactFromHit checks first. It
 * still takes the damage. Bosses: the boss flag (the boss skeleton) or a boss health bar. Regular enemies are untouched (James, 2026-11-17: stun-locking should only be
 * stopped on bosses). The bit is only cleared again if this set it. Off in deterministic mode.
 *   CJ_BOSS_POISE_HITS  hits in a row before poise (default 4)
 *   CJ_BOSS_POISE_MS    how long it lasts (default 3000) */
#define VA_ACTOR_LIST 0x0047C3F0u   /* g_aActorList: 128 x 0x18, Actor * at +0x10 */
/* Actor::HasBossBar() && IsThreat(), the HUD's test for the boss health bar (FEHud::UpdateBossBar):
 * the actor's sub-group animation row (SubGroupAnims::ms_pSubGroupAnimsTable 0x48FCE0, 0x48 a row,
 * index Actor +0x810) has m_Anims[15] (+0x40) set, and its profile (index Actor +0x778: table
 * 0x4816CC + 4 * sheet, 0x14C a row) has m_Honour (+0x64) above 0 */
static int boss_bar(uint8_t *mem, uint32_t a)
{
    uint32_t tab = *(volatile uint32_t *)(mem + 0x0048FCE0u), si = *(volatile uint32_t *)(mem + a + 0x810);
    uint32_t idx = *(volatile uint32_t *)(mem + a + 0x778), sh = idx & 0xFFFF, row = idx >> 16, pr;
    if (tab < 0x10000u || si > 4096 || *(volatile int32_t *)(mem + tab + si * 0x48u + 0x40) <= 0) return 0;
    if (sh == 0xFFFF || row == 0xFFFF || sh > 64 || (pr = *(volatile uint32_t *)(mem + 0x004816CCu + sh * 4)) < 0x10000u) return 0;
    return *(volatile int32_t *)(mem + pr + row * 0x14Cu + 0x64) > 0;
}
static void boss_poise_tick(uint8_t *mem)
{
    static int on = -1, hits = 4;
    static uint64_t until_ns[128], ms = 3000;
    static uint32_t who[128];
    static uint8_t ours[128];
    uint64_t now;
    uint32_t i;
    if (on < 0) {
        const char *e = getenv("CJ_BOSS_POISE");
        on = e && *e == '1';
        if ((e = getenv("CJ_BOSS_POISE_HITS")) && atoi(e) >= 2 && atoi(e) <= 20) hits = atoi(e);
        if ((e = getenv("CJ_BOSS_POISE_MS")) && atoi(e) >= 500 && atoi(e) <= 20000) ms = (uint64_t)atoi(e);
        if (on) fprintf(stderr, "[COMBAT] boss poise: %d hits in a row, then %u ms without flinching\n", hits, (unsigned)ms);
    }
    if (!on) return;
    now = xbox_HostNowNs();
    for (i = 0; i < 128; i++) {
        uint32_t a = *(volatile uint32_t *)(mem + VA_ACTOR_LIST + i * 0x18u + 0x10);
        volatile uint32_t *sf;
        uint32_t af;
        if (a < 0x10000u || a + 0x960u >= 0x80000000u + g_xbox_contig_size) { who[i] = 0; ours[i] = 0; continue; }
        if (a != who[i]) { who[i] = a; ours[i] = 0; until_ns[i] = 0; }   /* a new actor in this slot */
        af = *(volatile uint32_t *)(mem + a + 0x940);
        if (!(af & (1u << 15)) && !boss_bar(mem, a)) continue;   /* bosses only */
        sf = (volatile uint32_t *)(mem + a + 0x93c);
        if (ours[i]) {
            if (now < until_ns[i]) { *sf |= 1u << 4; continue; }   /* keep it (a state may have cleared it) */
            *sf &= ~(1u << 4);
            ours[i] = 0;
            gamelog_event("boss_poise", "\"actor\":%u,\"on\":0,\"hp\":%.0f", i, *(volatile float *)(mem + a + 0x7DC));
            continue;
        }
        if (*sf & (1u << 4)) continue;   /* the game itself has hit reactions off right now */
        if (*(volatile int32_t *)(mem + a + 0x860) + 1 >= hits) {
            *sf |= 1u << 4;
            ours[i] = 1;
            until_ns[i] = now + ms * 1000000ull;
            gamelog_event("boss_poise", "\"actor\":%u,\"on\":1,\"hp\":%.0f", i, *(volatile float *)(mem + a + 0x7DC));
        }
    }
}

/* Jump buffer (modern movement; GitHub issue #6, 2026-11-20). A jump pressed in the air was lost:
 * the run reads only a fresh press (the frame the button goes down), and landings while moving go
 * straight into the run, so holding a press made before touchdown did nothing. Now a press made
 * during the jump or fall carries into the landing:
 *  - still held at touchdown: the landing starts the charge (the running charge when moving, the
 *    standing one otherwise), timed from touchdown; it stays queued while the button is held, up to
 *    CJ_JUMP_BUFFER_HOLD_MS (default 1000) after touchdown, so a landing animation cannot eat it;
 *  - released before touchdown but pressed within CJ_JUMP_BUFFER_MS (default 150) of it: a plain
 *    jump, queued for 300 ms after touchdown.
 * Holding the button from take-off (the higher jump) queues nothing: only a new press in the air
 * counts. Off by default: CJ_JUMP_BUFFER=1 turns it on (launcher: PC Settings > Experimental). */
static uint64_t s_air_press_ns;   /* last fresh jump press while airborne (0: none this jump) */
static int s_jb;                  /* 0 nothing queued, 1 held, 2 tap */
static uint64_t s_jb_ns;          /* touchdown time of the queued jump */
static int jb_ms(void)
{
    static int v = -2;
    if (v == -2) {
        const char *on = getenv("CJ_JUMP_BUFFER"), *e = getenv("CJ_JUMP_BUFFER_MS");
        v = e && *e ? atoi(e) : 150;
        if (v < 0 || v > 1000) v = 150;
        if (!on || atoi(on) != 1) v = 0;   /* off unless turned on (launcher: PC Settings > Experimental) */
    }
    return v;
}
static int jb_hold_ms(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("CJ_JUMP_BUFFER_HOLD_MS"); v = e && *e ? atoi(e) : 1000; if (v < 0 || v > 5000) v = 1000; }
    return v;
}
void cjmod_air_jump_note(int fresh_press)
{
    if (!cjmod_move_modern() || !jb_ms()) return;
    if (fresh_press) s_air_press_ns = xbox_HostNowNs();
}
void cjmod_jump_buffer_land(int held)
{
    uint64_t now = xbox_HostNowNs();
    s_jb = 0;
    if (cjmod_move_modern() && jb_ms() && s_air_press_ns) {
        if (held) s_jb = 1;
        else if (now - s_air_press_ns <= (uint64_t)jb_ms() * 1000000ull) s_jb = 2;
        s_jb_ns = now;
        if (s_jb) {
            static int lg = -1;
            if (lg < 0) lg = getenv("CJ_MOVE_TRACE") != NULL;
            if (lg) fprintf(stderr, "[MOVE] jump buffered at landing: %s (pressed %.0f ms before)\n", s_jb == 1 ? "held" : "tap",
                            (double)(now - s_air_press_ns) / 1e6);
        }
    }
    s_air_press_ns = 0;
}
/* a queued jump for this frame (held: the jump button is down now); consumed when returned */
int cjmod_jump_buffer_take(int held)
{
    uint64_t age;
    if (!s_jb) return 0;
    age = xbox_HostNowNs() - s_jb_ns;
    if (s_jb == 1 && !held) s_jb = 2;   /* let go before the landing animation was over: a plain jump */
    if (s_jb == 1 && age > (uint64_t)jb_hold_ms() * 1000000ull) { s_jb = 0; return 0; }
    if (s_jb == 2 && age > 300000000ull) { s_jb = 0; return 0; }
    s_jb = 0;
    return 1;
}
