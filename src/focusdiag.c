#include "focusdiag.h"
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL_syswm.h>

/* Window/focus diagnostics only. No input capture: SDL already delivers key events to the game,
 * and nothing here reads or records keystrokes. We log the window's own state (style, topmost,
 * rect vs. the monitor, z-order) and the Windows messages SDL forwards, so an Alt+Tab or a
 * Win+Shift+S can be traced after the fact. CJ_FOCUS_DIAG=1 turns it on. */

static FILE *s_f;
static HWND s_hw;
static DWORD s_t0, s_last;

static void wname(HWND h, char *out, int n)
{
    char cls[64] = "", ttl[48] = "";
    if (!h) { snprintf(out, n, "none"); return; }
    GetClassNameA(h, cls, sizeof cls);
    GetWindowTextA(h, ttl, sizeof ttl);
    snprintf(out, n, "%p[%s|%s]", (void *)h, cls, ttl);
}

static void state(const char *tag, const char *extra)
{
    char fg[160], above[400] = "", tmp[120];
    RECT r = {0, 0, 0, 0};
    HMONITOR mon;
    MONITORINFO mi;
    HWND p;
    int z = 0, covers = 0, exact = 0;
    if (!s_f || !s_hw) return;
    wname(GetForegroundWindow(), fg, sizeof fg);
    GetWindowRect(s_hw, &r);
    mi.cbSize = sizeof mi;
    mon = MonitorFromWindow(s_hw, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoA(mon, &mi)) {
        covers = (r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
                  r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom);
        exact = EqualRect(&r, &mi.rcMonitor) ? 1 : 0;
    }
    for (p = GetWindow(s_hw, GW_HWNDPREV); p && z < 4; p = GetWindow(p, GW_HWNDPREV)) {
        char c[48] = "";
        RECT pr;
        if (!IsWindowVisible(p)) continue;
        GetClassNameA(p, c, sizeof c);
        GetWindowRect(p, &pr);
        snprintf(tmp, sizeof tmp, "%s(%ldx%ld%s) ", c, pr.right - pr.left, pr.bottom - pr.top,
                 (GetWindowLong(p, GWL_EXSTYLE) & WS_EX_TOPMOST) ? ",top" : "");
        if (strlen(above) + strlen(tmp) < sizeof above) strcat(above, tmp);
        z++;
    }
    fprintf(s_f, "%7lu %-10s fg=%s style=%08lx ex=%08lx%s rect=%ld,%ld-%ld,%ld covers=%d exact=%d iconic=%d vis=%d above=[%s] %s\n",
            (unsigned long)(GetTickCount() - s_t0), tag, fg,
            (unsigned long)GetWindowLong(s_hw, GWL_STYLE), (unsigned long)GetWindowLong(s_hw, GWL_EXSTYLE),
            (GetWindowLong(s_hw, GWL_EXSTYLE) & WS_EX_TOPMOST) ? "(TOPMOST)" : "",
            r.left, r.top, r.right, r.bottom, covers, exact, IsIconic(s_hw) ? 1 : 0, IsWindowVisible(s_hw) ? 1 : 0,
            above, extra ? extra : "");
    fflush(s_f);
}

void focusdiag_init(SDL_Window *w)
{
    SDL_SysWMinfo i;
    char path[MAX_PATH], *sl;
    const char *e = getenv("CJ_FOCUS_DIAG");
    if (!e || e[0] != '1') return;   /* off unless CJ_FOCUS_DIAG=1 */
    SDL_VERSION(&i.version);
    if (!SDL_GetWindowWMInfo(w, &i)) return;
    s_hw = i.info.win.window;
    GetModuleFileNameA(NULL, path, sizeof path);
    sl = strrchr(path, '\\');
    if (sl) *sl = 0;
    strncat(path, "\\focus-diag.txt", sizeof path - strlen(path) - 1);
    s_f = fopen(path, "w");
    if (!s_f) return;
    s_t0 = GetTickCount();
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    fprintf(s_f, "window/focus diagnostics (no input capture). Alt+Tab or Win+Shift+S, then quit.\n"
                 "exact=1 means the window exactly matches the monitor (the direct-scanout trigger); fullscreen should show exact=0.\n");
    state("init", "");
}

void focusdiag_note(const char *what) { state("note", what); }

void focusdiag_event(const SDL_Event *ev)
{
    char x[160];
    if (!s_f) return;
    if (ev->type == SDL_WINDOWEVENT) {
        snprintf(x, sizeof x, "SDL_WINDOWEVENT ev=%d d1=%d d2=%d", ev->window.event, ev->window.data1, ev->window.data2);
        state("sdl-win", x);
    } else if (ev->type == SDL_QUIT) {
        state("sdl-quit", "");
    } else if (ev->type == SDL_SYSWMEVENT && ev->syswm.msg->subsystem == SDL_SYSWM_WINDOWS) {
        UINT m = ev->syswm.msg->msg.win.msg;
        LPARAM lp = ev->syswm.msg->msg.win.lParam;
        const char *n = NULL;
        switch (m) {
        case WM_ACTIVATE: n = "WM_ACTIVATE"; break;
        case WM_ACTIVATEAPP: n = "WM_ACTIVATEAPP"; break;
        case WM_NCACTIVATE: n = "WM_NCACTIVATE"; break;
        case WM_WINDOWPOSCHANGING: n = "WM_WINDOWPOSCHANGING"; break;
        case WM_WINDOWPOSCHANGED: n = "WM_WINDOWPOSCHANGED"; break;
        case WM_SIZE: n = "WM_SIZE"; break;
        case WM_SHOWWINDOW: n = "WM_SHOWWINDOW"; break;
        case WM_DISPLAYCHANGE: n = "WM_DISPLAYCHANGE"; break;
        case WM_ENTERSIZEMOVE: n = "WM_ENTERSIZEMOVE"; break;
        case WM_EXITSIZEMOVE: n = "WM_EXITSIZEMOVE"; break;
        case WM_CAPTURECHANGED: n = "WM_CAPTURECHANGED"; break;
        case WM_SYSCOMMAND: n = "WM_SYSCOMMAND"; break;
        default: return;
        }
        if (m == WM_WINDOWPOSCHANGING || m == WM_WINDOWPOSCHANGED) {
            const WINDOWPOS *wpos = (const WINDOWPOS *)lp;
            snprintf(x, sizeof x, "%s flags=%08x pos=%d,%d size=%dx%d", n, (unsigned)wpos->flags, wpos->x, wpos->y,
                     wpos->cx, wpos->cy);
        } else
            snprintf(x, sizeof x, "%s", n);
        state("wm", x);
    }
}

void focusdiag_tick(int focused)
{
    DWORD now = GetTickCount();
    if (!s_f) return;
    if (now - s_last >= (focused ? 2000u : 150u)) { s_last = now; state(focused ? "tick" : "tick-bg", ""); }
}
#else
void focusdiag_init(SDL_Window *w) { (void)w; }
void focusdiag_event(const SDL_Event *ev) { (void)ev; }
void focusdiag_tick(int focused) { (void)focused; }
void focusdiag_note(const char *what) { (void)what; }
#endif
