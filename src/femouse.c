/*
 * Mouse in the menus (CJ_MENU_MOUSE=0 turns it off).
 *
 * While a front-end list is taking input, the cursor is shown (mouse look's relative mode is
 * released, src/gl_backend.c) and:
 *   - pointing at an item focuses it, as the d-pad would (only when the mouse moves, so the
 *     keyboard and pad keep working with the cursor parked anywhere);
 *   - left click on an item presses A on it; on a slider or a number spinner, the left or right
 *     half of the control steps it down or up;
 *   - right click is B (back), the wheel steps up and down the list.
 *
 * The game side runs from the FEListener::Update hook (src/recomp_manual.c): each listener that is
 * taking input this frame asks FEListener::GetChildren (event 0xF: every focusable, visible
 * object under its owner) for its items, finds the one under the cursor from the objects' own
 * rects, and calls FEListener::SetFocus. The GL thread supplies the cursor in front-end units
 * (the 640x480 surface the menus draw to) and the click counts; presses go in through the pad
 * (cj_pad_inject).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "recomp_types.h"

extern void cj_pad_inject(unsigned short dig, int polls);
extern void cj_pad_inject_ana(unsigned char ana_mask, int polls);

#define VA_GETCHILDREN   0x000F75B0u   /* static FEListener::GetChildren(ev, a, b, list, &n, flags, nearestLast), cdecl */
#define VA_SETFOCUS      0x000F6730u   /* FEListener::SetFocus(FEObject *, s8), thiscall */
#define VA_INPUTDISABLED 0x0048E008u   /* FEListener::ms_bInputDisabled */
#define VA_DIALOGLIST    0x0048E00Cu   /* FEListener::m_pDialogListener */
#define VT_SLIDER        0x00388B40u
#define VT_FEPAUSE       0x00389A18u
#define VT_FEHUD         0x00387CC0u   /* FEHud: +0xB90 weapon selector open (FEHud::UpdateWeaponSelect) */
#define VT_SPIN          0x00388A40u
#define RET_VA           0x0000C0DFu

/* GL thread -> game thread */
static volatile int s_on = -1;
static volatile int s_mx = -1, s_my = -1;            /* cursor in front-end units, -1 outside */
static volatile unsigned s_move_seq, s_click_seq;    /* bumped on motion / left click */
static volatile int s_click_x, s_click_y;
static volatile unsigned s_click_ms;
/* game thread -> GL thread */
static volatile unsigned s_click_taken;              /* last click a list handled (or ignored on purpose) */
static volatile unsigned s_menu_ms;                  /* last frame a list took input */
static volatile unsigned s_items_ms;                 /* last frame such a list had items */

/* Milliseconds since `t`, never negative. Timestamps here are written by one thread and read by
 * another: a stamp taken a moment after the reader's SDL_GetTicks() made the unsigned difference
 * wrap to ~49 days, so a fresh stamp read as ancient (the key list closed for a frame, a click was
 * dropped as stale). */
static unsigned ms_since(unsigned t) { unsigned now = SDL_GetTicks(); return (int)(now - t) < 0 ? 0u : now - t; }
int femouse_enabled(void)
{
    if (s_on < 0) { const char *e = getenv("CJ_MENU_MOUSE"); s_on = !(e && e[0] == '0'); }
    return s_on;
}
/* The menus are taking input (the cursor should show). */
int femouse_menu_active(void)
{
    return femouse_enabled() && ms_since(s_menu_ms) < 600u;
}


/* Clickable button prompts ("~E Select", "~F Back" in the footers, the title's "Press ~J"): the
 * front-end text hook (src/recomp_manual.c) reports every prompt drawn with its box; a left click
 * inside one presses that button. Boxes not drawn for 250 ms are gone. */
typedef struct { int x, y, w, h, btn; unsigned ms; uint32_t win; volatile unsigned vis_ms; } FePrompt;
static FePrompt s_pr[16];
static volatile unsigned s_prompt_click;    /* the click a prompt took (the lists ignore it) */
static int prompt_button(char c)
{
    switch (c) {
    case 'A': case 'E': return 0x100;   /* A (analog bit 0) */
    case 'B': case 'F': return 0x101;   /* B */
    case 'X': return 0x102;             /* X (the hub's Quit) */
    case 'Y': return 0x103;             /* Y */
    case 'J': return 0x10;              /* Start */
    case 'K': return 0x20;              /* Back */
    }
    return -1;
}
/* Game thread, from XWnd::FontPrint: text `t` drawn in the box (x, y, w, h), front-end units. The
 * first button code in the text is the box's button (the title's two-prompt line starts). */
void femouse_prompt(uint32_t win, int x, int y, int w, int h, const char *t)
{
    const char *c = t ? strchr(t, '~') : NULL;
    int b, i, slot = -1;
    unsigned now = SDL_GetTicks() | 1u;
    if (!femouse_enabled() || !c || w <= 0 || h <= 0) return;
    if ((b = prompt_button(c[1])) < 0) return;
    for (i = 0; i < 16; i++) if (s_pr[i].ms && s_pr[i].x == x && s_pr[i].y == y && s_pr[i].w == w && s_pr[i].h == h) { slot = i; break; }
    if (slot < 0) for (i = 0; i < 16; i++) if (!s_pr[i].ms || (ms_since(s_pr[i].ms) > 250u && !(s_pr[i].vis_ms && ms_since(s_pr[i].vis_ms) < 500u))) { slot = i; break; }
    if (slot < 0) return;
    if (getenv("CJ_FE_PROMPT_LOG") && !(s_pr[slot].ms && s_pr[slot].x == x && s_pr[slot].y == y && s_pr[slot].w == w))
        fprintf(stderr, "[PROMPT] new box %d,%d %dx%d button %x (t=%u) \"%.40s\"\n", x, y, w, h, b, now, t);
    s_pr[slot].x = x; s_pr[slot].y = y; s_pr[slot].w = w; s_pr[slot].h = h; s_pr[slot].btn = b;
    if (s_pr[slot].win != win) s_pr[slot].vis_ms = 0;
    s_pr[slot].win = win;
    s_pr[slot].ms = now;
}
static int prompt_at(int fx, int fy)
{
    int i, best = -1;
    long ba = 0;
    for (i = 0; i < 16; i++) {
        const FePrompt *p = &s_pr[i];
        long a;
        /* drawn just now, or its window still on screen (the menus print a footer once and keep it) */
        if (!p->ms || (ms_since(p->ms) > 250u && !(p->vis_ms && ms_since(p->vis_ms) < 500u))) continue;
        if (fx < p->x || fy < p->y || fx >= p->x + p->w || fy >= p->y + p->h) continue;
        if (p->h > 40 && femouse_menu_active()) continue;   /* a text block: only where no list takes clicks (the title) */
        a = (long)p->w * p->h;
        if (best < 0 || a < ba) { best = i; ba = a; }
    }
    return best < 0 ? -1 : s_pr[best].btn;
}
/* GL thread, every input poll: cursor position in front-end units (or -1), button edges, wheel. */
void femouse_input(int fx, int fy, int left_down_edge, int right_down_edge, int wheel)
{
    if (!femouse_enabled()) return;
    if (fx != s_mx || fy != s_my) { s_mx = fx; s_my = fy; s_move_seq++; }
    {   /* one line in pcj-log.txt the first time the menus see the cursor (for problem reports) */
        static int said;
        if (!said && fx >= 0 && femouse_menu_active()) { said = 1; fprintf(stderr, "[FEMOUSE] menu cursor at %d,%d (640x480 units)\n", fx, fy); }
    }
    if (left_down_edge && fx >= 0) { s_click_x = fx; s_click_y = fy; s_click_ms = SDL_GetTicks(); s_click_seq++; }
    if (left_down_edge && fx >= 0) {   /* a click on a button prompt presses that button */
        int b = prompt_at(fx, fy);
        if (b >= 0) {
            if (b & 0x100) cj_pad_inject_ana((unsigned char)(1u << (b & 0xFF)), 3);
            else cj_pad_inject((unsigned short)b, 3);
            s_prompt_click = s_click_seq;
            if (getenv("CJ_MENU_MOUSE_LOG")) fprintf(stderr, "[FEMOUSE] prompt click %d,%d -> button %x\n", fx, fy, b);
        }
    }
    if (ms_since(s_menu_ms) > 1500u) return;   /* no list taking input lately */
    if (right_down_edge) cj_pad_inject_ana(1u << 1, 3);         /* B */
    if (wheel > 0) cj_pad_inject(1u << 0, 2);                   /* up */
    else if (wheel < 0) cj_pad_inject(1u << 1, 2);              /* down */
}

/* Keyboard-page cells (src/native/hybrid/cjmod.c): rebinding and the mouse rows. */
extern int cjmod_kb_cell_of(uint32_t obj);
extern int cjmod_kb_focusable(int k);
extern int cjmod_kb_cell_km(int k);
extern void keymap_capture(int slot);
extern void keymap_cancel(void);
extern void keymap_reset(void);
extern int keymap_capturing(void);
extern int pcs_get(int id);
extern void pcs_set(int id, int v);
extern void qs_osd_note(const char *m);
#include "pcsettings.h"
/* The keyboard list keeps its own selection (the pages' listeners only reach their top rows):
 * Down from the bottom of the page's own rows enters the list, the arrows move in it, Up from its
 * top row leaves it, Enter / Space / A activates a cell, Esc leaves; the mouse hovers and clicks. */
extern int cjmod_kb_row(int k);
extern int cjmod_kb_col(int k);
extern int cjmod_kb_visible(void);
static volatile int s_sel = -1;                     /* selected cell, or -1 */
static volatile int s_at_bottom;                    /* the page's focus has nothing below it */
static volatile int s_hover_on;
static int s_ws_target = -1, s_ws_arrow;   /* weapon selector: row a click asked for, then Right (1) / Left (-1) */
static unsigned s_ws_ms;
static int s_wsel;   /* the listener being updated is the weapon selector's (FEHud +0xB90) */                     /* hover holds the list selection (mouse last used) */
int femouse_kb_sel(void) { return (s_sel >= 0 && cjmod_kb_visible()) ? s_sel : -1; }
static void kb_activate(int k, int dir)
{
    int km = cjmod_kb_cell_km(k);
    if (km >= 0) { keymap_capture(km); return; }
    switch (k) {
    case 18: pcs_set(PCS_MOUSE, !pcs_get(PCS_MOUSE)); break;
    case 19: pcs_set(PCS_MOUSE_SENS, pcs_get(PCS_MOUSE_SENS) + (dir < 0 ? -1 : 1)); break;
    case 20: pcs_set(PCS_MOUSE_INVERT, !pcs_get(PCS_MOUSE_INVERT)); break;
    case 21: keymap_reset(); qs_osd_note("Keys reset to defaults"); break;
    }
}
/* The next focusable cell from `k` in direction (dr rows, dc columns), or -1. */
static int kb_step(int k, int dr, int dc)
{
    int r = cjmod_kb_row(k), c = cjmod_kb_col(k), best = -1, bd = 1000, j;
    for (j = 0; j < 22; j++) {
        int rj = cjmod_kb_row(j), cj = cjmod_kb_col(j), d;
        if (j == k || !cjmod_kb_focusable(j)) continue;
        if (dr && (cj != c || (rj - r) * dr <= 0)) continue;
        if (dc && ((cj - c) * dc <= 0)) continue;
        d = dr ? abs(rj - r) : abs(rj - r) * 4;
        if (d < bd) { bd = d; best = j; }
    }
    return best;
}
static int kb_first(void)
{
    int j;
    for (j = 0; j < 22; j++) if (cjmod_kb_focusable(j)) return j;
    return -1;
}

/* GL thread, after the bindings fill the pad. */
void femouse_kb_keys(const Uint8 *k, int pad_a, uint16_t *dig, uint8_t ana[8])
{
    static int held, prev_dig, logged = -2;
    if (logged != s_sel && getenv("CJ_MENU_MOUSE_LOG")) { fprintf(stderr, "[FEMOUSE] sel %d -> %d (t=%u)\n", logged, s_sel, SDL_GetTicks()); logged = s_sel; }
    int act = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER] || k[SDL_SCANCODE_SPACE] || pad_a;
    int edge = *dig & ~prev_dig, sel = s_sel;
    prev_dig = *dig;
    if (keymap_capturing() >= 0) {
        if (!femouse_menu_active() || !cjmod_kb_visible()) keymap_cancel();   /* the menu closed */
        held = 1;
        return;
    }
    {   /* the list closed: gone from the screen, or no menu taking input for a while (a frame or two
         * without a listener update must not drop the selection: it made the row flicker) */
        static unsigned quiet_since;
        int gone = !cjmod_kb_visible(), quiet = !femouse_menu_active();
        if (!quiet) quiet_since = 0;
        else if (!quiet_since) quiet_since = SDL_GetTicks() | 1u;
        if (gone || (quiet && SDL_GetTicks() - quiet_since > 1500u)) {
            if (s_sel >= 0 && getenv("CJ_MENU_MOUSE_LOG")) fprintf(stderr, "[FEMOUSE] list closed (menu %d visible %d)\n", !quiet, !gone);
            s_sel = -1; held = act; return;
        }
        if (quiet) { held = act; return; }
    }
    if (getenv("CJ_MENU_MOUSE_LOG") && (edge & 0x3F)) fprintf(stderr, "[FEMOUSE] keys %02x sel %d bottom %d\n", edge & 0x3F, sel, s_at_bottom);
    if (sel < 0) {
        if ((edge & 2) && s_at_bottom) {
            s_sel = kb_first();
            *dig &= (uint16_t)~2;
            if (getenv("CJ_MENU_MOUSE_LOG")) fprintf(stderr, "[FEMOUSE] into the list: cell %d\n", s_sel);
        }
        held = act;
        return;
    }
    if (!cjmod_kb_focusable(sel)) {
        if (getenv("CJ_MENU_MOUSE_LOG")) fprintf(stderr, "[FEMOUSE] cell %d not focusable\n", sel);
        s_sel = -1; return;
    }
    if (edge & 0x2F) s_hover_on = 0;   /* the arrows or Back move the selection now, not the resting pointer */
    /* in the list: the game sees none of the navigation keys */
    if (edge & 1) { int n = kb_step(sel, -1, 0); s_sel = n >= 0 ? n : -1; }
    else if (edge & 2) { int n = kb_step(sel, 1, 0); if (n >= 0) s_sel = n; }
    else if (edge & 4) { if (sel == 19) kb_activate(19, -1); else { int n = kb_step(sel, 0, -1); if (n >= 0) s_sel = n; } }
    else if (edge & 8) { if (sel == 19) kb_activate(19, 1); else { int n = kb_step(sel, 0, 1); if (n >= 0) s_sel = n; } }
    if (edge & 0x20) s_sel = -1;                    /* Esc / Back: out of the list, the menu stays */
    if (act && !held && !(edge & 0x20)) kb_activate(sel, 1);
    held = act;
    *dig &= (uint16_t)~0x3F;
    ana[0] = ana[1] = 0;
}

static uint8_t *M(void) { extern void *xbox_GetMemoryOffset(void); return (uint8_t *)xbox_GetMemoryOffset(); }
/* Guest reads and writes for object fields: a pointer outside RAM (a list freed as its menu
 * closes, read in the same frame) gives 0 instead of faulting. Going back to the main menu from
 * the pause menu crashed here, reading an item at 0xFE000000. */
static int ok_va(uint32_t va) { return va >= 0x10000u && va <= 0x84000000u - 4u; }
static uint32_t rd32(uint32_t va) { uint32_t v; if (!ok_va(va)) return 0; memcpy(&v, M() + va, 4); return v; }
static int32_t rds(uint32_t va) { return (int32_t)rd32(va); }
static uint8_t rd8(uint32_t va) { return ok_va(va) ? ((uint8_t *)M())[va] : 0; }
static void wr32(uint32_t va, uint32_t v) { if (ok_va(va)) memcpy(M() + va, &v, 4); }

/* Absolute front-end rect of an object: its window's rect origin + base rect origin + local rect. */
static void obj_rect(uint32_t o, int r[4])
{
    uint32_t w;
    int wx = 0, wy = 0;
    if (!ok_va(o) || !ok_va(o + 0x28u)) { r[0] = r[1] = r[2] = r[3] = 0; return; }
    w = rd32(o + 0x10u);
    if (w) { wx = rds(w + 0x18u); wy = rds(w + 0x1Cu); }
    r[0] = wx + rds(o + 0x24u) + rds(o + 0x14u);
    r[1] = wy + rds(o + 0x28u) + rds(o + 0x18u);
    r[2] = rds(o + 0x1Cu);
    r[3] = rds(o + 0x20u);
}


/* The Hunting Gear and trophy screens (FEMainMenu::UpdateStateViewTrophies, 0x000ECFB0) draw their
 * exhibits as 3D items on a grid and keep the selection in function statics; the page's only menu
 * object sits off screen. Mouse support on Hunting Gear: a click picks the weapon slot nearest the
 * pointer (screen positions from weapon_screen below) and the screen's own move runs (s_Target,
 * SetSubstate(2)), so the lasers travel to it as they do for the d-pad; a click on the slot the
 * lasers are on presses A. */
#define VA_MAINMENU      0x0048CE58u   /* FEMainMenu::ms_pInstance */
#define VA_TROPHY_OBJS   0x0048CD70u   /* XItemHandler_FETrophy::ms_Objects[58] */
#define VA_TROPHY_N      0x0048CE70u   /* ::ms_nObjects */
#define VA_TR_TARGET     0x0048DE84u   /* UpdateStateViewTrophies: s_Target */
#define VA_TR_CURRENT    0x0048DE8Cu   /*                          s_Current */
#define VA_SETSUBSTATE   0x000E1B80u   /* FEMainMenu::SetSubstate(unsigned int), thiscall */
static float rdf(uint32_t va) { float f; memcpy(&f, M() + va, 4); return f; }
/* 1 if the trophy/gear screen took the pointer.
 * The exhibits' screen positions: the camera on this screen is fixed and the rack is flat (every
 * weapon sits at world x 23.6), so screen x follows world z and screen y world y. The mapping was
 * fitted on a 2560x1440 capture of the Hunting Gear screen (the sword, the spears de and dd, the
 * combistick outline df, checked against every traced outline, and the four rows)
 * and agrees with the game's own projection in y; the engine's matrices are the 2D front end's
 * whenever this code runs, so they can't be used. Trophies (another camera) keep the keys. */
static int weapon_screen(uint32_t item, int *sx, int *sy)
{
    float y = rdf(item + 0xCCu), z = rdf(item + 0xD0u);
    if (y != y || z != z) return 0;
    *sx = (int)(326.4f - 76.4f * z);
    *sy = (int)(-110.5f * y - 5124.7f);
    return 1;
}
/* Each slot's clickable area: the box around its outline or weapon on the rack, in front-end units,
 * traced from the user's markup of the outlines on a capture, plus the visible weapons. A click
 * inside boxes picks the one whose centre is nearest; outside all, the nearest slot point within
 * 45 units. Right of the sword the slots are de and dd (the spears) and df (the combistick outline). */
static const struct { uint16_t hash, x0, x1, y0, y1; } k_slot_box[] = {
    /* top row: the blade outlines (the user's markup) and the sword */
    {0xd8, 172, 196,  66, 275}, {0xd7, 204, 232,  58, 292}, {0xd9, 239, 275,  61, 281},
    {0xda, 279, 318,  59, 204}, {0xdb, 318, 358,  64, 209}, {0xdc, 360, 399,  65, 206},
    {0xde, 402, 422,  54, 366}, {0xdd, 430, 454,  56, 316}, {0xdf, 458, 480,  66, 318},   /* spear, spear, combistick */
    /* middle: shuriken, the spiral outline, the smart disc, the dark disc outline */
    {0xc9, 135, 166, 243, 282}, {0xd4, 272, 315, 209, 280}, {0xd5, 302, 346, 214, 266},
    {0xd6, 350, 390, 214, 266},
    /* lower: dagger, outline, round blade, the T outline, outline, gauntlet, outline */
    {0xca, 137, 165, 302, 336}, {0xcb, 172, 200, 288, 332}, {0xcc, 202, 236, 291, 339},
    {0xcd, 238, 277, 288, 346}, {0xd2, 283, 311, 270, 313}, {0xd1, 316, 354, 268, 320},
    {0xd3, 363, 390, 270, 316},
    /* bottom: the two throwing blades and the outline on the right */
    {0xce, 286, 316, 318, 368}, {0xcf, 318, 348, 321, 368}, {0xd0, 362, 395, 326, 364},
};
/* The slot whose box holds (x, y), nearest box centre first; 0 if none. */
static uint32_t slot_box_at(int x, int y)
{
    uint32_t best = 0;
    int bd = 0x7fffffff;
    for (size_t i = 0; i < sizeof k_slot_box / sizeof k_slot_box[0]; i++) {
        int cx, cy, d;
        if (x < k_slot_box[i].x0 || x >= k_slot_box[i].x1 || y < k_slot_box[i].y0 || y >= k_slot_box[i].y1) continue;
        cx = (k_slot_box[i].x0 + k_slot_box[i].x1) / 2 - x;
        cy = (k_slot_box[i].y0 + k_slot_box[i].y1) / 2 - y;
        d = cx * cx + (cy * cy) / 4;
        if (d < bd) { bd = d; best = k_slot_box[i].hash; }
    }
    return best;
}
/* The trophy room (same screen, other camera): the exhibits hang on a flat wall (world z 23.8), so
 * screen x follows world x and screen y world y; fitted on a 2560x1440 capture (the skull grid's
 * corners), it lands every skull, mask and clan symbol within a few units. */
static int trophy_screen(uint32_t item, int *sx, int *sy)
{
    float x = rdf(item + 0xC8u), y = rdf(item + 0xCCu);
    if (x != x || y != y) return 0;
    *sx = (int)(150.0f + 81.6f * (x + 2.218f));
    *sy = (int)(86.7f - 106.5f * (y + 47.276f));
    return 1;
}
static int trophy_mouse(uint32_t list, uint32_t n, int px, int py, int moved, int click)
{
    uint32_t mm = rd32(VA_MAINMENU), mgr, cur, best = 0;
    int r[4], bd = 0x7fffffff;
    (void)moved;
    if (n != 1 || !mm) return 0;
    obj_rect(rd32(list), r);
    if (r[0] > -500 || r[1] > -500) return 0;                 /* not the exhibit page */
    mgr = rd32(mm + 0x1C0u);
    if (!mgr) return 0;
    {   /* calibration log (CJ_MENU_MOUSE_LOG): every exhibit's grid cell and world position, every 5 s */
        static unsigned last;
        if (getenv("CJ_MENU_MOUSE_LOG") && ms_since(last) > 5000u) {
            uint32_t cnt = rd32(VA_TROPHY_N);
            last = SDL_GetTicks();
            if (cnt > 58u) cnt = 58u;
            fprintf(stderr, "[TROPHYPOS] view %s current %x\n", rd32(mm + 0x200u) == 9u ? "weapons" : "trophies", rd32(VA_TR_CURRENT));
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t h = rd32(VA_TROPHY_OBJS + i * 4u), it;
                if (!h || !(it = rd32(h + 4u))) continue;
                fprintf(stderr, "[TROPHYPOS] %x grid %d,%d world %.3f %.3f %.3f\n", rd32(h + 8u), rds(h + 0x10u), rds(h + 0x14u),
                        rdf(it + 0xC8u), rdf(it + 0xCCu), rdf(it + 0xD0u));
            }
        }
    }
    int weapons = rd32(mm + 0x200u) == 9u;
    if (!click) return 1;                                      /* the lasers move on a click, not on hover */
    cur = rd32(VA_TR_CURRENT);
    uint32_t cnt = rd32(VA_TROPHY_N);
    if (cnt > 58u) cnt = 58u;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t h = rd32(VA_TROPHY_OBJS + i * 4u), item, hash;
        int sx, sy, dx, dy, d;
        if (!h || !(item = rd32(h + 4u))) continue;
        hash = rd32(h + 8u);
        if (weapons) {
            if (hash < 0xC9u || hash > 0xDFu) continue;        /* weapons only (not the lasers) */
            if (!weapon_screen(item, &sx, &sy)) continue;
        } else {
            if (!((hash >= 0x1u && hash <= 0xFu) || (hash >= 0x65u && hash <= 0x76u))) continue;   /* trophies */
            if (!trophy_screen(item, &sx, &sy)) continue;
            if (rds(h + 0x14u) == 3 && hash <= 0xFu) sy += 14;  /* the masks hang lower than their point */
        }
        dx = px - sx; dy = py - sy;
        d = dx * dx + (dy * dy) / 4;                           /* tall weapons: forgive more up and down */
        if (d < bd) { bd = d; best = hash; }
    }
    if (weapons) {   /* inside a slot's outline: that slot (the boxes, not the points, decide) */
        uint32_t b = slot_box_at(px, py);
        if (b) { best = b; bd = 0; }
    }
    if (getenv("CJ_MENU_MOUSE_LOG")) fprintf(stderr, "[TROPHY] click %d,%d -> %x (current %x, substate %u)\n", px, py, best, cur, rd32(mgr + 0x408u));
    if (!best || bd > 45 * 45) return 1;                       /* not near a slot */
    if (rd32(mgr + 0x408u) != 1u) return 1;                    /* moving or showing one: wait */
    if (best != cur) {
        uint32_t e0 = g_eax, c0 = g_ecx, d0 = g_edx, sp = g_esp;
        wr32(VA_TR_TARGET, best);
        PUSH32(g_esp, 2u); PUSH32(g_esp, RET_VA);
        g_ecx = mm;
        recomp_lookup(VA_SETSUBSTATE)();
        g_esp = sp; g_eax = e0; g_ecx = c0; g_edx = d0;
    } else {
        cj_pad_inject_ana(1u << 0, 3);                         /* A: the slot the lasers are on */
    }
    return 1;
}
/* Game thread: which prompts' windows are still on screen (EXBaseDisplay::IsWndVisible walks the
 * display's window list by pointer, so a window that has gone is simply not found). */
#define VA_DISPLAY       0x0049E110u   /* EXBaseDisplay::m_pDisplay */
#define VA_ISWNDVISIBLE  0x002168A0u   /* EXBaseDisplay::IsWndVisible(EXWnd *) const, thiscall */
static void prompts_check(void)
{
    uint32_t disp = rd32(VA_DISPLAY);
    unsigned now = SDL_GetTicks() | 1u;
    if (!disp) return;
    for (int i = 0; i < 16; i++) {
        FePrompt *p = &s_pr[i];
        if (!p->ms || !p->win) continue;
        uint32_t e0 = g_eax, c0 = g_ecx, d0 = g_edx, sp = g_esp;
        PUSH32(g_esp, p->win); PUSH32(g_esp, RET_VA);
        g_ecx = disp;
        recomp_lookup(VA_ISWNDVISIBLE)();
        int vis = (g_eax & 0xFFu) != 0;
        g_esp = sp; g_eax = e0; g_ecx = c0; g_edx = d0;
        p->vis_ms = vis ? now : 0;
        if (!vis) p->ms = 0;   /* its window closed: gone */
    }
}
/* Game thread: FEListener `L` is about to run its Update. */
/* Several lists can update in one frame (the pause menu: its tab bar, then the page's items), so
 * each keeps its own record of the pointer moves it has seen, and a click that misses one list
 * is left for the others for a moment. */
#define FEM_LISTS 8
static uint32_t s_list_L[FEM_LISTS];
static unsigned s_list_seen[FEM_LISTS];
static unsigned *seen_move_for(uint32_t L)
{
    static int next;
    int i;
    for (i = 0; i < FEM_LISTS; i++) if (s_list_L[i] == L) return &s_list_seen[i];
    i = next++ % FEM_LISTS;
    s_list_L[i] = L; s_list_seen[i] = s_move_seq;
    return &s_list_seen[i];
}
static unsigned s_miss_ck, s_miss_ms;

void femouse_listener(uint32_t L)
{
    unsigned *seen_mp;
#define seen_move (*seen_mp)
    if (!femouse_enabled() || !L) return;
    seen_mp = seen_move_for(L);
    {   static unsigned checked;
        if (ms_since(checked) > 100u) { checked = SDL_GetTicks(); prompts_check(); }
    }
    if (getenv("CJ_MENU_MOUSE_LOG")) {
        static unsigned last, calls;
        calls++;
        if (SDL_GetTicks() - last > 2000u) {
            last = SDL_GetTicks();
            fprintf(stderr, "[FEMOUSE] %u listener updates; L=%08x dis=%d/%d dlg=%08x owner=%08x st=%08x cb=%08x focus=%08x cursor=%d,%d\n",
                    calls, L, rd8(VA_INPUTDISABLED), rd8(L + 0x60u), rd32(VA_DIALOGLIST), rd32(L + 0x1Cu),
                    rd32(L + 0x1Cu) ? rd32(rd32(L + 0x1Cu) + 0x70u) : 0, rd32(L + 0x64u), rd32(L + 0x18u), s_mx, s_my);
        }
    }
    if ((int8_t)rd8(VA_INPUTDISABLED) || (int8_t)rd8(L + 0x60u)) return;
    uint32_t dlg = rd32(VA_DIALOGLIST);
    if (dlg && dlg != L) return;
    uint32_t owner = rd32(L + 0x1Cu), focus = rd32(L + 0x18u);
    if (!owner || !focus) return;   /* (Update runs only for a visible object taking input) */
    {   /* Plain play: in a level (FEMainMenu gone) with only the base game state on XApp's stack. Lists
         * update there too (the hint popups), but no menu is open: pause (state 0xC), the weapon selector
         * (0x14) and dialogs (0x10) all push a state. */
        uint32_t fe = rd32(0x0048CE58u), w = rd32(owner + 0x10u), vt = w ? rd32(w) : 0;
        int in_level = !fe || !rd32(fe + 0x1C0u);
        int menu_wnd = vt == VT_FEPAUSE || (vt == VT_FEHUD && rd8(w + 0xB90u)) || rd32(VA_DIALOGLIST) != 0;
        if (in_level && !menu_wnd) return;
    }
    {   /* the HUD's own lists update in play: not a menu, unless the weapon selector is open */
        uint32_t wnd = rd32(owner + 0x10u);
        if (getenv("CJ_MENU_MOUSE_LOG")) {
            static unsigned t; static uint32_t lw;
            if (wnd != lw || ms_since(t) > 3000u) { t = SDL_GetTicks(); lw = wnd; fprintf(stderr, "[FEMOUSE] listener %08x window %08x vt %08x selector %d\n", L, wnd, wnd ? rd32(wnd) : 0, wnd ? rd8(wnd + 0xB90u) : -1); }
        }
        if (wnd && rd32(wnd) == VT_FEHUD && !rd8(wnd + 0xB90u)) return;
        s_wsel = wnd && rd32(wnd) == VT_FEHUD && rd8(wnd + 0xB90u);
    }
    if (cjmod_kb_visible()) {   /* can the page's focus move down? (GetChildren event 0xC, as Update) */
        uint32_t e0 = g_eax, c0 = g_ecx, d0 = g_edx, sp = g_esp;
        uint32_t lst = (sp - 0x1400u) & ~15u, cnt = lst + 0x400u;
        wr32(cnt, 0);
        g_esp = lst - 0x40u;
        PUSH32(g_esp, 1u); PUSH32(g_esp, 0x48u); PUSH32(g_esp, cnt);
        PUSH32(g_esp, lst); PUSH32(g_esp, focus); PUSH32(g_esp, owner); PUSH32(g_esp, 0xCu);
        PUSH32(g_esp, RET_VA);
        recomp_lookup(VA_GETCHILDREN)();
        g_esp = sp; g_eax = e0; g_ecx = c0; g_edx = d0;
        s_at_bottom = rd32(cnt) == 0;
        if (getenv("CJ_MENU_MOUSE_LOG")) { static int was = -1; if (was != s_at_bottom) { was = s_at_bottom; fprintf(stderr, "[FEMOUSE] at bottom %d (focus %08x)\n", was, focus); } }
    }
    unsigned now = SDL_GetTicks(), mv = s_move_seq, ck = s_click_seq;
    if (now - s_menu_ms > 1500u) { seen_move = mv; s_click_taken = ck; }   /* a menu just opened: the parked cursor picks nothing */
    s_menu_ms = now;

    int moved = mv != seen_move, click = ck != s_click_taken;
    if (click && ck == s_prompt_click) { s_click_taken = ck; click = 0; }   /* a prompt took it */
    if (click && ms_since(s_click_ms) > 1500u) { s_click_taken = ck; click = 0; }   /* stale (made outside a menu) */
    if (click && s_miss_ck == ck && ms_since(s_miss_ms) > 200u) { s_click_taken = ck; click = 0; }   /* no list took it */
    if (s_hover_on && !click && s_mx >= 0 && cjmod_kb_visible() && keymap_capturing() < 0) {
        /* the pointer resting on a keyboard-list cell keeps it selected */
        extern uint32_t cjmod_kb_cell_obj(int k);
        int k;
        for (k = 0; k < 22; k++) {
            uint32_t o = cjmod_kb_cell_obj(k);
            int r[4];
            if (!o || !cjmod_kb_focusable(k)) continue;
            obj_rect(o, r);
            if (s_mx >= r[0] && s_my >= r[1] && s_mx < r[0] + r[2] && s_my < r[1] + r[3]) { s_sel = k; break; }
        }
    }
    if (!moved && !click && !(s_wsel && s_ws_target >= 0)) return;
    int px = click ? s_click_x : s_mx, py = click ? s_click_y : s_my;
    if (px < 0) { seen_move = mv; return; }
    if (moved) s_hover_on = 1;   /* the mouse is in use: hover selects until the keys or pad move the selection */
    if (moved && cjmod_kb_visible() && keymap_capturing() < 0) {   /* pointing at a keyboard-list cell selects it */
        extern uint32_t cjmod_kb_cell_obj(int k);
        int k;
        for (k = 0; k < 22; k++) {
            uint32_t o = cjmod_kb_cell_obj(k);
            int r[4];
            if (!o || !cjmod_kb_focusable(k)) continue;
            obj_rect(o, r);
            if (px >= r[0] && py >= r[1] && px < r[0] + r[2] && py < r[1] + r[3]) { s_sel = k; break; }
        }
    }

    /* the list's items: GetChildren(0xF, owner, focus, list, &n, 0x48, 1), scratch below the guest stack */
    uint32_t eax0 = g_eax, ecx0 = g_ecx, edx0 = g_edx, esp0 = g_esp;
    uint32_t list = (esp0 - 0x1400u) & ~15u, np = list + 0x400u;
    wr32(np, 0);
    g_esp = list - 0x40u;
    PUSH32(g_esp, 1u); PUSH32(g_esp, 0x48u); PUSH32(g_esp, np);
    PUSH32(g_esp, list); PUSH32(g_esp, focus); PUSH32(g_esp, owner); PUSH32(g_esp, 0xFu);
    PUSH32(g_esp, RET_VA);
    recomp_lookup(VA_GETCHILDREN)();
    g_esp = esp0;
    uint32_t gc_ret = g_eax;
    uint32_t n = rd32(np);
    if (getenv("CJ_MENU_MOUSE_LOG"))
        fprintf(stderr, "[FEMOUSE] GetChildren ret=%u n=%u owner=%08x vt=%08x st=%08x focus=%08x vt=%08x st=%08x parent=%08x\n",
                gc_ret, n, owner, rd32(owner), rd32(owner + 0x70u), focus, rd32(focus), rd32(focus + 0x70u), rd32(focus + 8u));
    if (n > 0x100u) n = 0;
    if (trophy_mouse(list, n, px, py, moved, click)) {
        g_esp = esp0; g_eax = eax0; g_ecx = ecx0; g_edx = edx0;
        seen_move = mv;
        if (click) s_click_taken = ck;
        return;
    }
    if (n) s_items_ms = SDL_GetTicks();
    if (s_wsel) {
        /* Weapon selector (FEHud::UpdateWeaponSelect). Only the game's own input moves its selection:
         * focusing a row from here (FEListener::SetFocus), on the selector or on the HUD's other lists
         * that update while it is open, corrupted the HUD and crashed the game. So hover does nothing,
         * the HUD's other lists ignore the mouse, and a click presses Up/Down until the clicked row is
         * selected, then Right on the category icon or right arrow, Left on the left arrow. */
        /* the selector's list: the on-screen items (a row's slide animation keeps an off-screen
         * copy), 2-12 of them stacked in one column */
        uint32_t vis[12];
        int nv = 0, rows, fi = -1, ti = -1, r0[4] = {0, 0, 0, 0}, ly = -100000;
        for (uint32_t i = 0; i < n && nv < 12; i++) {
            int ri[4];
            obj_rect(rd32(list + i * 4u), ri);
            if (ri[0] < 0 || ri[0] + ri[2] > 640 || ri[2] < 40) continue;
            vis[nv++] = rd32(list + i * 4u);
        }
        rows = nv >= 2;
        if (nv) obj_rect(vis[0], r0);
        for (int i = 0; i < nv && rows; i++) {
            int ri[4];
            obj_rect(vis[i], ri);
            if (ri[0] < r0[0] - 40 || ri[0] > r0[0] + 40 || ri[1] <= ly) rows = 0;
            ly = ri[1];
            if (vis[i] == focus) fi = i;
        }
        if (click && getenv("CJ_MENU_MOUSE_LOG")) {
            fprintf(stderr, "[FEMOUSE] weapon selector list (rows %d, focus %d) click %d,%d:", rows, fi, px, py);
            for (int i = 0; i < nv; i++) { int ri[4]; obj_rect(vis[i], ri); fprintf(stderr, " [%d,%d %dx%d]", ri[0], ri[1], ri[2], ri[3]); }
            fprintf(stderr, "\n");
        }
        if (rows && click) {
            for (int i = 0; i < nv; i++) {   /* the row clicked, or the row beside the click (30 units past its end) */
                int ri[4];
                obj_rect(vis[i], ri);
                if (py >= ri[1] && py < ri[1] + ri[3] && px >= ri[0] && px < ri[0] + ri[2] + 30) {
                    int rel = px - ri[0], w = ri[2];
                    ti = i;
                    s_ws_arrow = rel >= w - 48 ? 1 : rel >= w - 74 ? -1 : 0;
                    if (getenv("CJ_MENU_MOUSE_LOG")) fprintf(stderr, "[FEMOUSE] weapon selector click row %d (now %d) rel %d of %d\n", ti, fi, rel, w);
                }
            }
            if (ti >= 0) { s_ws_target = ti; s_ws_ms = 0; }
            s_click_taken = ck;
        }
        if (rows && s_ws_target >= 0 && ms_since(s_ws_ms) > 120u) {
            s_ws_ms = SDL_GetTicks();
            if (fi < 0 || s_ws_target >= nv) s_ws_target = -1;
            else if (fi < s_ws_target) cj_pad_inject(1u << 1, 2);          /* down */
            else if (fi > s_ws_target) cj_pad_inject(1u << 0, 2);          /* up */
            else {
                if (s_ws_arrow > 0) cj_pad_inject(1u << 3, 2);             /* right: next weapon */
                else if (s_ws_arrow < 0) cj_pad_inject(1u << 2, 2);        /* left: previous */
                s_ws_target = -1; s_ws_arrow = 0;
            }
        }
        g_esp = esp0; g_eax = eax0; g_ecx = ecx0; g_edx = edx0;
        seen_move = mv;
        return;
    }
    uint32_t hit = 0;
    long best = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = rd32(list + i * 4u);
        int r[4];
        if (!o) continue;
        obj_rect(o, r);
        if (r[2] <= 0 || r[3] <= 0) continue;
        if (px < r[0] || py < r[1] || px >= r[0] + r[2] || py >= r[1] + r[3]) continue;
        long area = (long)r[2] * r[3];
        if (!hit || area < best) { hit = o; best = area; }
    }
    int tabs = 0;   /* a tab bar (the pause menu's): items side by side in one row */
    if (n >= 3) {
        int r0[4], same = 1;
        obj_rect(rd32(list), r0);
        for (uint32_t i = 1; i < n && same; i++) { int ri[4]; obj_rect(rd32(list + i * 4u), ri); if (ri[1] < r0[1] - 4 || ri[1] > r0[1] + 4) same = 0; }
        tabs = same;
    }
    if (!hit && !tabs) {   /* (a tab's box is its whole target: the page below belongs to the page's list) */
        /* An item's focusable object can be much smaller than what is drawn for it: a save slot's
         * is only its title line, above the chapter name or "Empty"; a menu word can be wider than
         * its box. Off every item, the pointer belongs to the item whose box it is in: the nearest
         * one starting above it in the same column (a little slack sideways), within 90 units. */
        int bt = -100000, bdx = 100000;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t o = rd32(list + i * 4u);
            int r[4], dx;
            if (!o) continue;
            obj_rect(o, r);
            if (r[2] <= 0 || r[3] <= 0) continue;
            if (r[1] > py || py - r[1] > 90) continue;
            if (py >= r[1] + r[3] && r[3] >= 32) continue;   /* a full-size item: nothing below it is its */
            if (px < r[0] - 32 || px >= r[0] + r[2] + 32) continue;
            dx = px < r[0] ? r[0] - px : px >= r[0] + r[2] ? px - (r[0] + r[2]) + 1 : 0;
            if (r[1] > bt || (r[1] == bt && dx < bdx)) { bt = r[1]; bdx = dx; hit = o; }
        }
    }
    if (getenv("CJ_MENU_MOUSE_LOG")) {
        fprintf(stderr, "[FEMOUSE] L=%08x n=%u cursor=%d,%d hit=%08x%s\n", L, n, px, py, hit, click ? " click" : "");
        for (uint32_t i = 0; i < n && i < 24; i++) {
            int r[4]; obj_rect(rd32(list + i * 4u), r);
            fprintf(stderr, "[FEMOUSE]   %08x vt=%08x %d,%d %dx%d\n", rd32(list + i * 4u), rd32(rd32(list + i * 4u)), r[0], r[1], r[2], r[3]);
        }
    }
    if (hit && hit != focus && (!tabs || click)) {   /* tabs switch on a click only, not on hover */
        PUSH32(g_esp, 0u); PUSH32(g_esp, hit); PUSH32(g_esp, RET_VA);
        g_ecx = L;
        recomp_lookup(VA_SETFOCUS)();
        g_esp = esp0;
    }
    g_eax = eax0; g_ecx = ecx0; g_edx = edx0;
    seen_move = mv;
    if (click && keymap_capturing() < 0) {   /* a keyboard-page cell (focusable or not in this list) */
        extern uint32_t cjmod_kb_cell_obj(int k);
        int k;
        for (k = 0; k < 22; k++) {
            uint32_t o = cjmod_kb_cell_obj(k);
            int r[4];
            if (!o || !cjmod_kb_focusable(k)) continue;
            obj_rect(o, r);
            if (px < r[0] || py < r[1] || px >= r[0] + r[2] || py >= r[1] + r[3]) continue;
            s_sel = k;
            kb_activate(k, px >= r[0] + r[2] / 2 ? 1 : -1);
            s_click_taken = ck;
            return;
        }
    }
    if (click && hit && tabs) {
        s_click_taken = ck;     /* the focus change above switched the tab */
    } else if (click && hit) {
        uint32_t vt = rd32(hit);
        if (vt == VT_SLIDER || vt == VT_SPIN) {
            int r[4]; obj_rect(hit, r);
            cj_pad_inject(px >= r[0] + r[2] / 2 ? (1u << 3) : (1u << 2), 2);   /* right / left */
        } else {
            cj_pad_inject_ana(1u << 0, 3);                                    /* A */
        }
        s_click_taken = ck;
    } else if (click && n) {
        /* beside this list's items: another list updating this frame may own it */
        if (s_miss_ck != ck) { s_miss_ck = ck; s_miss_ms = SDL_GetTicks(); }
    }
}

/* FEListener::Update (0x000F7D00, thiscall, no args): every list's per-frame input, called directly
 * by FEObject::HandleInput, so it is replaced here (tools/regen_all.sh lists this file) rather
 * than through recomp_lookup_manual. */
extern void sub_000F7D00_gen(void);
void sub_000F7D00(void)
{
    femouse_listener(g_ecx);
    sub_000F7D00_gen();
}
#undef seen_move
