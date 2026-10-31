/* Modern third-person camera (mods/Camera.cpp calls these from SetCamThirdPerson).
 *
 * What the original does, measured from a PC session (frames.csv): the right
 * stick moves a target angle; the real angle chases it with a lag of ~10
 * frames, and the camera position chases its own target again (m_Warp), so a
 * released stick keeps turning 15-40 degrees for half a second. On top, the
 * auto-rotation adds to the same target while running and often swings the
 * camera back the other way. Distance snaps in to the centre ray and eases out
 * at 1/30 per frame, pumping near pillars.
 *
 * This controller owns yaw and pitch in normal third person (camera mode 0,
 * player not wall-walking):
 *   - the stick sets an angular speed (response curve, top speed) with a very
 *     short ramp: no coasting once released;
 *   - gentle follow: after the stick has been idle for a while and the player
 *     runs steadily, yaw eases behind the player; any stick input stops it;
 *   - time-based smoothing everywhere (identical at 30, 60 or 144 fps);
 *   - distance: pulled in quickly when blocked, held, then eased out slowly.
 * The game's own changes to the angle that are not small per-frame steps
 * (scripted camera turns, respawn, quick load) are taken over as they come.
 * Directions follow the game's invert settings (Options > Controller); with
 * them off, right looks right and up looks up.
 *
 * CJ_CAM_MODERN=0 (or PC Settings) restores the original camera. Angles: yaw a,
 * the camera looks along (sin a, cos a); elevation e from straight up (pi/2
 * level, larger = camera higher). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "cjmod.h"
#include "../../pcsettings.h"

extern float cj_cam_mouse_rad(int axis);   /* recomp_manual.c: mouse look this frame, radians (size only;
                                              the direction is the stick value gl_backend.c sets) */

#define PI_F 3.14159265f

static int s_on;                 /* the controller ran last frame */
static float s_yaw, s_pitch;     /* our angles (what we wrote last frame) */
static float s_yv, s_pv;         /* angular speeds, rad/s */
static float s_idle = 10.0f;     /* seconds since the last look input */
static float s_follow;           /* follow weight 0..1 */
static float s_steady;           /* seconds the player has run in a steady direction */
static float s_prev_rot;
static float s_hold;             /* distance: seconds to wait before easing out */
static float s_dt = 1.0f / 60.0f;
static unsigned s_frames;

static float wrap_pi(float a)
{
    while (a > PI_F) a -= 2.0f * PI_F;
    while (a < -PI_F) a += 2.0f * PI_F;
    return a;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float ease(float tau) { return tau <= 0.0f ? 1.0f : 1.0f - expf(-s_dt / tau); }

static int cfg(const char *k, int def)
{
    const char *e = getenv(k);
    return e && *e ? atoi(e) : def;
}

/* Tunables (CJ_CAM_* environment, read once; degrees/s, ms). */
static float t_yaw_max, t_pitch_max, t_curve, t_ramp_up, t_ramp_down;
static float t_follow_delay, t_follow_rate, t_follow_max, t_pitch_home;
static float t_piv_h, t_piv_v, t_dist_in, t_dist_out, t_dist_hold;
static int t_trace = -1;
static void tunables(void)
{
    if (t_trace >= 0) return;
    t_yaw_max      = cfg("CJ_CAMX_YAW", 200) * PI_F / 180.0f;    /* full stick */
    t_pitch_max    = cfg("CJ_CAMX_PITCH", 160) * PI_F / 180.0f;
    t_curve        = cfg("CJ_CAMX_CURVE", 160) / 100.0f;          /* response exponent */
    t_ramp_up      = cfg("CJ_CAMX_RAMP", 60) / 1000.0f;           /* speed-up time constant */
    t_ramp_down    = cfg("CJ_CAMX_STOP", 25) / 1000.0f;           /* stop time constant */
    t_follow_delay = cfg("CJ_CAMX_FOLLOW_DELAY", 1200) / 1000.0f; /* stick idle + steady running */
    t_follow_rate  = cfg("CJ_CAMX_FOLLOW_RATE", 120) / 100.0f;    /* 1/s of the remaining angle */
    t_follow_max   = cfg("CJ_CAMX_FOLLOW_MAX", 70) * PI_F / 180.0f;
    t_pitch_home   = cfg("CJ_CAMX_PITCH_HOME", 2000) / 1000.0f;   /* elevation follow returns to */
    t_piv_h        = cfg("CJ_CAMX_PIVOT_H", 50) / 1000.0f;
    t_piv_v        = cfg("CJ_CAMX_PIVOT_V", 140) / 1000.0f;
    t_dist_in      = cfg("CJ_CAMX_DIST_IN", 60) / 1000.0f;
    t_dist_out     = cfg("CJ_CAMX_DIST_OUT", 450) / 1000.0f;
    t_dist_hold    = cfg("CJ_CAMX_DIST_HOLD", 300) / 1000.0f;
    t_trace        = cfg("CJ_CAMX_TRACE", 0);
    fprintf(stderr, "[CAM] modern camera: yaw %.0f deg/s, pitch %.0f deg/s, curve %.2f, follow after %.1f s\n",
            t_yaw_max * 180.0f / PI_F, t_pitch_max * 180.0f / PI_F, t_curve, t_follow_delay);
}

int cjmod_cam_modern(void)
{
    static int env = -2;
    if (env == -2) env = getenv("CJ_CAM_MODERN") ? cfg("CJ_CAM_MODERN", 1) : -1;
    if (env >= 0) return env;
    return pcs_get(PCS_CAM_MODERN);
}

static float response(float s)
{
    float m = fabsf(s);
    if (m > 1.0f) m = 1.0f;
    m = powf(m, t_curve);
    return s < 0.0f ? -m : m;
}

/* One frame. Returns 1 when the controller drives the camera this frame
 * (results in cjmod_cam_out). game_*: the camera's angles and tgt_* its target
 * angles as DoUpdate left them; rx, ry right stick and lx, ly left stick (-1..1, as the game read
 * them); rot the player's heading; inv_x, inv_y the camera's invert flags. */
int cjmod_cam_step(float game_yaw, float game_pitch, float tgt_yaw, float tgt_pitch, float rx, float ry, float lx, float ly,
                   float rot, float speed, int pmode, int cmode, int inv_x, int inv_y, float mul)
{
    float wx, wy, mx, my, k;
    int look, active;

    tunables();
    if (!(mul > 0.05f && mul < 8.0f)) mul = 1.0f;
    s_dt = mul / 60.0f;                       /* the engine's frame length (nominal 60 Hz) */
    active = cjmod_cam_modern() && cmode == 0 && pmode != 0x18;
    if (!active) {
        s_on = 0;
        return 0;
    }
    if (!s_on) {                              /* take over where the game's camera is */
        s_yaw = game_yaw; s_pitch = game_pitch;
        s_yv = s_pv = 0.0f; s_follow = 0.0f; s_idle = 10.0f;
    } else {
        /* The game's per-frame stick and follow steps are small; anything
         * larger is a scripted turn, a respawn or a load: go with it. */
        if (fabsf(wrap_pi(tgt_yaw - s_yaw)) > 0.6f) { s_yaw = tgt_yaw; s_yv = 0.0f; }
        if (fabsf(tgt_pitch - s_pitch) > 0.45f) { s_pitch = tgt_pitch; s_pv = 0.0f; }
    }
    s_on = 1;
    s_frames++;

    /* Look input, with the game's own sign convention and invert flags (the
     * game adds stick x invert x rate to the target angle): with the flags off,
     * right looks right and up looks up. */
    mx = cj_cam_mouse_rad(0);
    my = cj_cam_mouse_rad(1);
    if (mx != 0.0f) mx *= (rx >= 0.0f ? 1.0f : -1.0f) * (inv_x ? -1.0f : 1.0f);
    if (my != 0.0f) my *= (ry >= 0.0f ? 1.0f : -1.0f) * (inv_y ? -1.0f : 1.0f);
    /* The stick is one direction, not two axes: curve how far it is pushed
     * and keep the angle it is pushed at, then speed up or stop both axes
     * together. (Per axis, a diagonal bent toward the stronger axis and a
     * small vertical component was crushed while turning: the axes fought.) */
    {
        float sx = (mx != 0.0f) ? 0.0f : rx, sy = (my != 0.0f) ? 0.0f : ry;
        float mag = sqrtf(sx * sx + sy * sy), want, have, c;
        if (mag > 1.0f) { sx /= mag; sy /= mag; mag = 1.0f; }
        c = mag > 0.0f ? response(mag) / mag : 0.0f;
        wx = sx * c * t_yaw_max * (inv_x ? -1.0f : 1.0f);
        wy = sy * c * t_pitch_max * (inv_y ? -1.0f : 1.0f);
        want = wx * wx / (t_yaw_max * t_yaw_max) + wy * wy / (t_pitch_max * t_pitch_max);
        have = s_yv * s_yv / (t_yaw_max * t_yaw_max) + s_pv * s_pv / (t_pitch_max * t_pitch_max);
        k = ease(want > have ? t_ramp_up : t_ramp_down);
        s_yv += (wx - s_yv) * k;
        s_pv += (wy - s_pv) * k;
    }
    s_yaw = wrap_pi(s_yaw + s_yv * s_dt + mx);
    s_pitch += s_pv * s_dt + my;

    look = fabsf(rx) > 0.08f || fabsf(ry) > 0.08f || mx != 0.0f || my != 0.0f;
    s_idle = look ? 0.0f : s_idle + s_dt;

    /* Gentle follow: running steadily (heading changes slowly) with the left
     * stick, look input idle. Never pulls toward a heading facing the camera. */
    {
        float turn = fabsf(wrap_pi(rot - s_prev_rot)) / (s_dt > 0 ? s_dt : 1.0f);
        int moving = (pmode == 0 || pmode == 1) && speed > 0.5f && (lx * lx + ly * ly) > 0.25f;
        float d = wrap_pi(rot - s_yaw);
        s_prev_rot = rot;
        s_steady = (moving && turn < 2.5f) ? s_steady + s_dt : 0.0f;
        if (pcs_get(PCS_CAM_FOLLOW) && s_idle > 0.3f && s_steady > t_follow_delay && fabsf(d) < 2.4f)
            s_follow = clampf(s_follow + s_dt / 1.0f, 0.0f, 1.0f);
        else
            s_follow = clampf(s_follow - s_dt / 0.12f, 0.0f, 1.0f);
        if (s_follow > 0.0f) {
            float v = clampf(d * t_follow_rate, -t_follow_max, t_follow_max) * s_follow;
            s_yaw = wrap_pi(s_yaw + v * s_dt);
            s_pitch += (t_pitch_home - s_pitch) * ease(1.5f) * s_follow;
        }
    }
    if (s_pitch <= 0.9f && s_pv < 0.0f) s_pv = 0.0f;     /* at a pitch limit: no stored speed into it */
    if (s_pitch >= 2.85f && s_pv > 0.0f) s_pv = 0.0f;
    s_pitch = clampf(s_pitch, 0.9f, 2.85f);
    if (t_trace && (s_frames % 2) == 0)
        fprintf(stderr, "[CAMX] f=%u dt=%.4f rx=%.2f ry=%.2f yaw=%.3f pitch=%.3f yv=%.2f pv=%.2f idle=%.2f follow=%.2f tgt=%.3f/%.3f rot=%.3f spd=%.2f pm=%d\n",
                s_frames, s_dt, rx, ry, s_yaw, s_pitch, s_yv, s_pv, s_idle, s_follow, tgt_yaw, tgt_pitch, rot, speed, pmode);
    return 1;
}

float cjmod_cam_out(int i) { return i == 0 ? s_yaw : s_pitch; }

/* Look-at smoothing factor for this frame: axis 0 horizontal, 1 vertical. */
float cjmod_cam_pivot_k(int axis) { return ease(axis ? t_piv_v : t_piv_h); }

/* Camera distance: cur the current distance, blocked the nearest whisker hit
 * (or the wanted distance when nothing is in the way). */
float cjmod_cam_dist(float cur, float blocked)
{
    if (blocked < cur - 0.01f) {
        s_hold = t_dist_hold;
        cur += (blocked - cur) * ease(t_dist_in);
    } else if (s_hold > 0.0f) {
        s_hold -= s_dt;
    } else {
        cur += (blocked - cur) * ease(t_dist_out);
    }
    return cur;
}
