/* Frame pacing at Swap (CJ_PACE, default on; docs/frame-pacing.md).
 *
 * How EngineX paces by itself: the main loop calls EXApp::MainUpdate over
 * and over; MainUpdate returns at once until EXDisplay::m_VerticalBlanks
 * (display+0x980, bumped by XBVerticalBlankCallback from D3D's vblank
 * interrupt) reaches 60 / max frame rate, then runs one frame with
 * rate = 60 / vblanks and Presents. On the port the vblanks come from the
 * kernel's own 60 Hz schedule, unrelated to when frames finish, so:
 *   - a frame that takes 20 ms starts the next one at the following vblank,
 *     13 ms later: 20/13 ms judder on screen;
 *   - present times wander with the work of each frame;
 *   - a frame that crosses two vblanks reads as 30 fps, and the engine's
 *     smoothed rate then runs the simulation fast for a few frames.
 *
 * The pacer, on the game thread in D3DDevice_Swap (src/native_gfx.c):
 *   before Swap: wait until the frame's deadline (last present + period);
 *                a late frame is not waited for, and the next deadline is
 *                counted from when it actually presented (never catch up);
 *   after Swap:  set m_VerticalBlanks to the elapsed vblanks since the
 *                previous present (rounded, at least 60 / cap, at most 4),
 *                so MainUpdate's gate passes at once with the true count.
 * The kernel's vblank callbacks still run (anything else waiting on them is
 * unchanged, and the gate can never starve); their increments in the middle
 * of a frame are overwritten at the next Swap.
 *
 * Not active in deterministic mode (CJ_DET, recordings), which has its own
 * synthetic vblanks. CJ_PACE=0: the original vblank gate.
 *
 * Timing per frame for the gameplay log (frames.csv: work_ms, wait_ms,
 * present_ms): work = previous Swap's return to this Swap (game update,
 * render submission, the synchronous executor's draws so far), wait = the
 * pacer's wait, present = the Swap itself (the remaining draws and the GL
 * present). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <errno.h>
#endif
#include "frame_pacer.h"

extern void *xbox_GetMemoryOffset(void);
extern int cj_det_enabled(void);

#define VA_MAX_FRAME_RATE  0x0039A030u   /* EXBaseApp::m_MaxFrameRate */
#define VA_DISPLAY_PTR     0x0049E110u   /* EXBaseDisplay::m_pDisplay */
#define DISPLAY_VBLANKS    0x980u        /* EXDisplay::m_VerticalBlanks */

static int s_on = -1;
static double s_next, s_last_present, s_swap_end, s_t_entry, s_t_swap;
static float s_work_ms, s_wait_ms, s_present_ms;
static unsigned s_late, s_frames;

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

/* Sleep most of the way, spin the last stretch: sub-millisecond accuracy
 * without burning a core for the whole wait. */
static void wait_until(double t)
{
#ifdef _WIN32
    static HANDLE timer;
    static int init;
    if (!init) {
        init = 1;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    for (;;) {
        double left = t - now_s();
        if (left <= 0) return;
        if (left > 0.0015 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((left - 0.0012) * 1e7);     /* 100 ns units, relative */
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(timer, 50);
            else
                Sleep(1);
        } else if (left > 0.0015) {
            Sleep(1);
        } else {
            YieldProcessor();
        }
    }
#else
    for (;;) {
        double left = t - now_s();
        if (left <= 0) return;
        if (left > 0.0015) {
            struct timespec ts;
            double s = left - 0.001;
            ts.tv_sec = (time_t)s;
            ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }
    }
#endif
}

static int active(void)
{
    if (s_on < 0) {
        const char *e = getenv("CJ_PACE");
        s_on = !(e && *e == '0');
        fprintf(stderr, "[PACE] frame pacing at Swap: %s\n", s_on ? "on" : "off (CJ_PACE=0: the engine's vblank gate)");
    }
    return s_on && !cj_det_enabled();
}

static uint32_t cap(void)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t mx = *(volatile uint32_t *)(mem + VA_MAX_FRAME_RATE);
    return mx >= 15 && mx <= 60 ? mx : 30;
}

void cj_pace_before_swap(void)
{
    double t = now_s();
    s_t_entry = t;
    s_work_ms = s_swap_end ? (float)((t - s_swap_end) * 1000.0) : 0.0f;
    s_wait_ms = 0.0f;
    if (!active()) return;
    if (s_next && t < s_next) {
        if (s_next - t > 0.25) s_next = t;            /* clock jump or a long stall: start over */
        wait_until(s_next);
        s_wait_ms = (float)((now_s() - t) * 1000.0);
    } else if (s_next) {
        s_late++;
    }
}

void cj_pace_after_swap(void)
{
    double t = now_s(), t_present = s_t_entry + s_wait_ms / 1000.0;
    s_present_ms = (float)((t - t_present) * 1000.0);
    s_swap_end = t;
    s_frames++;
    if (active()) {
        uint32_t c = cap(), vmin = (60u + c - 1u) / c, vb;
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        uint32_t disp = *(volatile uint32_t *)(mem + VA_DISPLAY_PTR);
        double el = s_last_present ? t_present - s_last_present : 1.0 / c;
        vb = (uint32_t)floor(el * 60.0 + 0.5);
        if (vb < vmin) vb = vmin;
        if (vb > 4) vb = 4;                            /* MainUpdate skips a frame whose rate is <= 1 */
        if (disp >= 0x10000u && disp < 0x84000000u - 0x1000u) {
            *(volatile uint32_t *)(mem + disp + DISPLAY_VBLANKS) = vb;   /* the running total stays the kernel's */
        }
        s_next = t_present + 1.0 / (double)c;         /* from the actual present: no catch-up */
        if ((s_frames % 600) == 0)
            fprintf(stderr, "[PACE] cap %u, %u late frames so far; last: work %.1f ms, wait %.1f ms, present %.1f ms\n",
                    c, s_late, s_work_ms, s_wait_ms, s_present_ms);
    }
    s_last_present = t_present;
}

float cj_pace_work_ms(void) { return s_work_ms; }
float cj_pace_wait_ms(void) { return s_wait_ms; }
float cj_pace_present_ms(void) { return s_present_ms; }
