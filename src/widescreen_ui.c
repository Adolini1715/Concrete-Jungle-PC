/* 4:3 menus, text and HUD in widescreen.
 *
 * CJ_WIDESCREEN uses the game's anamorphic 16:9 mode: the 3D view gets wider,
 * but every 2D sprite (menus, text glyphs, HUD) is still laid out for a 640 wide
 * 4:3 screen and so comes out stretched by 4/3. All of it goes through EXWnd's
 * virtual sprite calls (always called through the vtable, so recomp_lookup_manual
 * can stand in front of them): _DrawSprite2D for plain, rotated and four-corner
 * sprites, and _SetSprite2DClipRect. While widescreen is on these squeeze x
 * around the screen centre by 3/4, so 2D keeps its 4:3 shape inside the 16:9
 * frame. Sprites spanning the whole width (fades, full-screen backgrounds) are
 * left alone. CJ_WS_UI=0 turns this off.
 *
 * EXWnd::m_PrimRect.x (window origin on screen) is at +0x38 (XBWndSprite.obj). */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "recomp_types.h"

int cj_widescreen(void);

#define SCR_W 640
/* The in-game HUD and radar windows already lay out for widescreen in the
 * original (FEHud/FERadar read EXBaseRenderEnv::WideScreen), so they are left
 * alone. */
static int hud_wnd(uint32_t wnd)
{
    uint32_t vt = MEM32(wnd);
    return vt == 0x00387CC0u || vt == 0x00389BC8u;   /* FEHud, FERadar vtables */
}
static int ws_active(void)
{
    static int env = -1;
    if (env < 0) { const char *e = getenv("CJ_WS_UI"); env = e && *e == '1';   /* opt-in: the original stretched layout is the default */ }
    return env && cj_widescreen();
}
/* Where a window's 2D content is pinned when it is squeezed to 4:3 proportions:
 * a window in the left or right third of the screen keeps its distance to that
 * edge (HUD corners stay in the corners), anything else (menus, full-screen
 * windows) is squeezed around the centre. Decided per window (EXWnd::m_PrimRect
 * at +0x38: x, y, w, h) so all the sprites of one element move together. */
static int s_anchor;   /* -1 left, 0 centre, 1 right: set by set_anchor for the current call */
static void set_anchor(uint32_t wnd)
{
    int x = (int32_t)MEM32(wnd + 0x38), w = (int32_t)MEM32(wnd + 0x40), c = x + w / 2;
    static int log = -1;
    if (log < 0) log = getenv("CJ_WS_LOG") != NULL;
    if (w >= SCR_W * 2 / 3 || w <= 0) s_anchor = 0;
    else if (c < SCR_W / 3) s_anchor = -1;
    else if (c > SCR_W * 2 / 3) s_anchor = 1;
    else s_anchor = 0;
    if (log) {
        static uint32_t seen[256]; static int ns;
        int i;
        for (i = 0; i < ns && seen[i] != wnd; i++) ;
        if (i == ns && ns < 256) {
            seen[ns++] = wnd;
            fprintf(stderr, "[WSUI] wnd %08X vt %08X rect %d,%d %dx%d anchor %d\n", wnd, MEM32(wnd), x,
                    (int32_t)MEM32(wnd + 0x3C), w, (int32_t)MEM32(wnd + 0x44), s_anchor);
        }
    }
}
/* Squeeze factor and pivot for centre-anchored windows (menus): CJ_WS_UI_SCALE
 * (default 0.75 = exact 4:3 shape) and CJ_WS_UI_PIVOT (screen x in 640 units
 * that stays put, default 320 = centre; 0 keeps the original left margin). */
static float s_scale = -1.0f; static int s_pivot;
static void ws_params(void)
{
    const char *e;
    if (s_scale >= 0.0f) return;
    e = getenv("CJ_WS_UI_SCALE"); s_scale = e ? (float)atof(e) : 0.75f;
    if (s_scale < 0.5f || s_scale > 1.0f) s_scale = 0.75f;
    e = getenv("CJ_WS_UI_PIVOT"); s_pivot = e ? atoi(e) : SCR_W / 2;
}
static int sq(int X)
{
    int o;
    float v;
    ws_params();
    o = s_anchor < 0 ? 0 : s_anchor > 0 ? SCR_W : s_pivot;
    v = (float)o + (float)(X - o) * s_scale;
    return (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
}
static int sqw(int w) { int n; ws_params(); n = (int)((float)w * s_scale + 0.5f); return n > 0 ? n : (w ? 1 : 0); }

/* Element mode (default): each front-end item keeps its place on screen and is
 * narrowed around its own centre, the way the game's own 3D front-end art (the
 * logo) behaves in widescreen. Squeezing everything around the screen centre
 * (CJ_WS_UI_MODE=centre) pulls text away from the logo and frames next to it.
 *
 * Sprites are queued as commands in the window's sprite blocks (EXBaseWnd
 * m_Sprite2DBlocks at +0xC4: count +0xCC, data +0xD0; block: u32 used, data at
 * +0x10) and drawn later. FEBaseWnd::RenderSprites sets a clip rect before each
 * cached item and once more at the end, so the commands added between two clip
 * rect calls are one item: they are recorded as they are queued and rewritten
 * around the group's centre when the next clip rect arrives. A group is only
 * touched in the frame it was queued in. */
static int s_mode = -1;      /* 1 element, 0 centre */
#define GMAX 1024
static uint32_t g_addr[GMAX]; static uint8_t g_type[GMAX]; static int g_n;
static uint32_t g_wnd, g_frame;

static uint32_t game_frame(void) { uint32_t gw = MEM32(0x0048E858u); return gw ? MEM32(gw + 0x384) : 0; }

static int cmd_minmax(uint32_t a, int t, int *lo, int *hi)
{
    int i, x;
    if (t == 6) {
        for (i = 0; i < 4; i++) { x = (int16_t)MEM16(a + 4 + i * 16 + 4); if (x < *lo) *lo = x; if (x > *hi) *hi = x; }
        return 1;
    }
    x = (int16_t)MEM16(a + 8);
    if (x < *lo) *lo = x;
    if (x + MEM16(a + 12) > *hi) *hi = x + MEM16(a + 12);
    return 1;
}

static void group_flush(void)
{
    int i, lo = 1 << 30, hi = -(1 << 30), j;
    float c;
    if (!g_n) return;
    if (game_frame() != g_frame) { g_n = 0; return; }
    for (i = 0; i < g_n; i++) cmd_minmax(g_addr[i], g_type[i], &lo, &hi);
    if (lo <= 0 && hi >= SCR_W) { g_n = 0; return; }                /* full width: leave it */
    ws_params();
    c = 0.5f * (float)(lo + hi);
    for (i = 0; i < g_n; i++) {
        uint32_t a = g_addr[i];
        if (g_type[i] == 6) {
            for (j = 0; j < 4; j++) {
                float v = c + ((float)(int16_t)MEM16(a + 4 + j * 16 + 4) - c) * s_scale;
                MEM16(a + 4 + j * 16 + 4) = (uint16_t)(int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
            }
        } else {
            int x = (int16_t)MEM16(a + 8), w = MEM16(a + 12);
            float v;
            if (g_type[i] == 5) {        /* rotated: x, y is the centre */
                v = c + ((float)x - c) * s_scale;
            } else {
                float mid = (float)x + 0.5f * (float)w;
                v = c + (mid - c) * s_scale - 0.5f * (float)sqw(w);
            }
            MEM16(a + 8) = (uint16_t)(int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
            MEM16(a + 12) = (uint16_t)sqw(w);
        }
    }
    g_n = 0;
}

/* call the original and record where its command landed */
static void queue_and_record(uint32_t va, int type)
{
    uint32_t wnd = g_ecx, n = MEM32(wnd + 0xCC), arr = MEM32(wnd + 0xD0), i, used[64], blk[64];
    uint32_t fr = game_frame();
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) { blk[i] = MEM32(arr + i * 4); used[i] = blk[i] ? MEM32(blk[i]) : 0; }
    if (wnd != g_wnd || fr != g_frame) { group_flush(); g_n = 0; g_wnd = wnd; g_frame = fr; }
    recomp_lookup(va)();
    {
        uint32_t n2 = MEM32(wnd + 0xCC), arr2 = MEM32(wnd + 0xD0), a = 0;
        if (n2 > 64) n2 = 64;
        for (i = 0; i < n2 && !a; i++) {
            uint32_t b = MEM32(arr2 + i * 4);
            if (!b) continue;
            if (i < n && arr2 == arr && blk[i] == b) { if (MEM32(b) > used[i]) a = b + 0x10 + used[i]; }
            else if (MEM32(b) > 0) a = b + 0x10 + MEM32(b) - (type == 6 ? 0x44u : type == 5 ? 0x28u : 0x20u);
        }
        if (a && (int)MEM32(a) == type && g_n < GMAX) { g_addr[g_n] = a; g_type[g_n] = (uint8_t)type; g_n++; }
    }
}

static int element_mode(void)
{
    if (s_mode < 0) { const char *e = getenv("CJ_WS_UI_MODE"); s_mode = !(e && *e == 'c'); }
    return s_mode;
}

/* EXSprite2D / EXRotSprite2D: short x, y at +4, u16 w, h at +8 */
static void sprite_common(uint32_t va, int type)
{
    uint32_t s = MEM32(g_esp + 4), px = (uint32_t)(int16_t)MEM16(g_ecx + 0x38);
    int16_t x = (int16_t)MEM16(s + 4);
    uint16_t w = MEM16(s + 8);
    int X = (int)(int16_t)px + x;
    if (!ws_active() || hud_wnd(g_ecx) || (X <= 0 && X + w >= SCR_W)) { recomp_lookup(va)(); return; }
    if (element_mode()) { queue_and_record(va, type); return; }
    set_anchor(g_ecx);
    {
        MEM16(s + 4) = (uint16_t)(int16_t)(sq(X) - (int)(int16_t)px);
        MEM16(s + 8) = (uint16_t)sqw(w);
        recomp_lookup(va)();
        MEM16(s + 4) = (uint16_t)x;
        MEM16(s + 8) = w;
    }
}
static void ws_sprite(void) { sprite_common(0x0024E0C0u, 4); }
static void ws_rotsprite(void) { sprite_common(0x0024E170u, 5); }

/* EXFlexiSprite2D: 4 verts of 0x10 bytes, short x at +4 */
static void ws_flexisprite(void)
{
    uint32_t s = MEM32(g_esp + 4);
    int px = (int16_t)MEM16(g_ecx + 0x38), i, lo = 1 << 30, hi = -(1 << 30);
    int16_t old[4];
    for (i = 0; i < 4; i++) {
        int X = px + (int16_t)MEM16(s + i * 16 + 4);
        old[i] = (int16_t)MEM16(s + i * 16 + 4);
        if (X < lo) lo = X;
        if (X > hi) hi = X;
    }
    if (!ws_active() || hud_wnd(g_ecx) || (lo <= 0 && hi >= SCR_W)) { recomp_lookup(0x0024DEA0u)(); return; }
    if (element_mode()) { queue_and_record(0x0024DEA0u, 6); return; }
    set_anchor(g_ecx);
    for (i = 0; i < 4; i++) MEM16(s + i * 16 + 4) = (uint16_t)(int16_t)(sq(px + old[i]) - px);
    recomp_lookup(0x0024DEA0u)();
    for (i = 0; i < 4; i++) MEM16(s + i * 16 + 4) = (uint16_t)old[i];
}

/* _SetSprite2DClipRect(short x, short y, u16 w, u16 h): by value on the stack */
static void ws_cliprect(void)
{
    if (ws_active() && !hud_wnd(g_ecx)) {
        if (element_mode()) {
            if (g_ecx == g_wnd) group_flush();
        } else {
            int px = (int16_t)MEM16(g_ecx + 0x38);
            int X = px + (int16_t)MEM16(g_esp + 4), w = MEM16(g_esp + 12);
            if (!(X <= 0 && X + w >= SCR_W)) {
                int x0, x1;
                set_anchor(g_ecx);
                x0 = sq(X); x1 = sq(X + w);
                MEM16(g_esp + 4) = (uint16_t)(int16_t)(x0 - px);
                MEM16(g_esp + 12) = (uint16_t)(x1 - x0);
            }
        }
    }
    recomp_lookup(0x0024DE20u)();
}

recomp_func_t ws_lookup(uint32_t va)
{
    if (va < 0x0024DE20u || va > 0x0024E170u) return 0;
    switch (va) {
    case 0x0024E0C0u: return ws_sprite;
    case 0x0024E170u: return ws_rotsprite;
    case 0x0024DEA0u: return ws_flexisprite;
    case 0x0024DE20u: return ws_cliprect;
    }
    return 0;
}

/* FEObjectScript::SetScale (both overloads) multiplies x by 1.33 in widescreen
 * when the script's m_State has 0x1000000 (every FE script starts with it): the
 * Xbox design stretched front-end and HUD art across a 16:9 TV. With the 4:3
 * option on, the flag is masked for the call so the art keeps its shape.
 * Linker wraps (callers are in other generated chunks: FEHud, menus). */
extern void __real_m_SetScale_FEObjectScript__QAEPAV1_M_Z_001084E0(void);
extern void __real_m_SetScale_FEObjectScript__QAEPAV1_VEXVector___Z_001079C0(void);
static void setscale(void (*real)(void))
{
    uint32_t self = g_ecx, st;
    uint32_t ret = MEM32(g_esp);
    if (!ws_active() || !self || (ret >= 0x000D0000u && ret < 0x000D9000u)) { real(); return; }   /* FEHud callers */
    st = MEM32(self + 0x48);                              /* FEObjectScript::m_State */
    MEM32(self + 0x48) = st & ~0x01000000u;
    real();
    MEM32(self + 0x48) = (MEM32(self + 0x48) & ~0x01000000u) | (st & 0x01000000u);
}
void __wrap_m_SetScale_FEObjectScript__QAEPAV1_M_Z_001084E0(void) { setscale(__real_m_SetScale_FEObjectScript__QAEPAV1_M_Z_001084E0); }
void __wrap_m_SetScale_FEObjectScript__QAEPAV1_VEXVector___Z_001079C0(void) { setscale(__real_m_SetScale_FEObjectScript__QAEPAV1_VEXVector___Z_001079C0); }

/* FEObjectScript::SetSize(int w, int h, signed char) scales a script to a pixel
 * size measured through the front-end camera, so in widescreen the art fills w
 * framebuffer pixels and is then stretched 4/3 on the 16:9 screen. With the 4:3
 * option on, w is asked for at 3/4 (screen-wide art, w >= 600, stays wide). */
extern void __real_m_SetSize_FEObjectScript__QAEPAV1_HHC_Z_00108540(void);
void __wrap_m_SetSize_FEObjectScript__QAEPAV1_HHC_Z_00108540(void)
{
    int32_t w = (int32_t)MEM32(g_esp + 4);
    if (ws_active() && w > 0 && w < 600) {
        MEM32(g_esp + 4) = (uint32_t)((w * 3 + 2) / 4);
        __real_m_SetSize_FEObjectScript__QAEPAV1_HHC_Z_00108540();
        return;   /* thiscall: the callee popped its arguments, nothing to restore */
    }
    __real_m_SetSize_FEObjectScript__QAEPAV1_HHC_Z_00108540();
}
