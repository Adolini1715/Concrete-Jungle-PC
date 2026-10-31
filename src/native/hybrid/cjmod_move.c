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

/* Every frame (cj_fps_tick): keep the player's gravity at the dial's value. */
void cjmod_move_tick(void)
{
    static float g = -1.0f;
    uint8_t *mem;
    uint32_t h, item, phys;
    if (g < 0.0f) {
        g = pct("CJ_GRAVITY");
        if (g != 1.0f) fprintf(stderr, "[MOVE] player gravity %.0f%%\n", g * 100.0f);
    }
    if (g == 1.0f || cj_det_enabled()) return;
    mem = (uint8_t *)xbox_GetMemoryOffset();
    h = *(volatile uint32_t *)(mem + VA_PLAYER_INSTANCE);
    if (h < 0x10000u || h >= 0x84000000u) return;
    item = *(volatile uint32_t *)(mem + h + 4);
    if (item < 0x10000u || item >= 0x84000000u) return;
    phys = *(volatile uint32_t *)(mem + item + 0x148);
    if (phys < 0x10000u || phys >= 0x84000000u) return;
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
int cjmod_run_charging(void)
{
    if (s_rc && xbox_HostNowNs() - s_rc_ns > CHARGE_STALE_NS) s_rc = 0;
    return s_rc;
}
void cjmod_run_charge_touch(int on) { s_rc = on; s_rc_ns = xbox_HostNowNs(); }
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
    float need = power_ok ? 0.33333334f : (medium_ok ? 0.1f : 1e9f);
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

