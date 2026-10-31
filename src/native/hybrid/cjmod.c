/* Native helpers for mods (see src/native/hybrid/cjmod.h, mods/README.md). */
#include "cjmod.h"

int cjmod_ticks30(unsigned int frame, float mul)
{
    static unsigned int last = 0xFFFFFFFFu;
    static float acc;
    static int ticks;
    if (frame != last) {
        last = frame;
        if (!(mul > 0.0f) || mul > 8.0f) mul = 2.0f;   /* unset or absurd: one tick */
        acc += mul * 0.5f;
        ticks = (int)acc;
        acc -= (float)ticks;
    }
    return ticks;
}

/* powf for mods (frame-rate correct exponential easing: x per 30 Hz frame
 * becomes x^k for a frame covering k of them). */
#include <math.h>
float cjmod_powf(float x, float y) { return powf(x, y); }

/* Menu strings for the front-end mods (mods/FEManager.cpp, mods/FEMainMenu.cpp, mods/FEPause.cpp).
 * Guest code can only use guest memory, so a mod allocates a block once and hands it here; the
 * strings are copied into it and handed back by index. */
#include <stdlib.h>
#include <string.h>
static char *s_ui;
static char *s_ui_str[16];
static unsigned s_ui_len[16];
static const char *const k_ui_text[] = {
    "Quit Game",                     /* 0 main menu row, pause row */
    "Press again to quit",           /* 1 confirm (menu rows) */
    "Press ~F again to quit the game",  /* 2 confirm (title screen) */
    "Restart Level",                 /* 3 pause */
    "Quit to Main Menu",             /* 4 pause */
    "Level Stats",                   /* 5 pause */
    "Resume",                        /* 6 pause */
    "Press START to begin    ~F Quit Game",  /* 7 title screen prompt */
    "Save Game",                     /* 8 pause row */
    "Load Game",                     /* 9 pause row */
};
char *cjmod_ui_block(void) { return s_ui; }
void cjmod_ui_attach(char *p)
{
    char *s;
    unsigned i;
    if (s_ui || !p) return;
    memset(p, 0, 0x400);
    s_ui = p;
    s = p;
    for (i = 0; i < sizeof k_ui_text / sizeof k_ui_text[0]; i++) {
        const char *t = k_ui_text[i], *e = getenv("CJ_UI_TEST");   /* debug: string 0 (the Quit Game row) */
        size_t l;
        if (i == 0 && e && *e && strlen(e) < 300) t = e;
        if (i == 7) {   /* with PC prompts START is an icon (Enter / Menu), src/glyphs.c */
            extern int glyph_icons_on(void);
            if (glyph_icons_on()) t = "Press ~J to begin    ~F Quit Game";
        }
        l = strlen(t) + 1;
        memcpy(s, t, l);
        s_ui_len[i] = (unsigned)l;
        s_ui_str[i] = s;
        s += l;
    }
}
char *cjmod_ui_str(int i) { return (s_ui && i >= 0 && i < 16 && s_ui_str[i]) ? s_ui_str[i] : s_ui; }

/* Milliseconds (host clock), for confirm timeouts. */
#include <SDL2/SDL.h>
/* Milliseconds since `t`, never negative. Timestamps here are written by one thread and read by
 * another: a stamp taken a moment after the reader's SDL_GetTicks() made the unsigned difference
 * wrap to ~49 days, so a fresh stamp read as ancient (the key list closed for a frame, a click was
 * dropped as stale). */
static unsigned ms_since(unsigned t) { unsigned now = SDL_GetTicks(); return (int)(now - t) < 0 ? 0u : now - t; }
unsigned int cjmod_ms(void) { return SDL_GetTicks(); }

/* Quit Game: the same exit as closing the window (atexit handlers save pcj.ini). */
#include <stdio.h>
void cjmod_quit(void)
{
    fprintf(stderr, "[FE] Quit Game chosen, exiting\n");
    fflush(stderr);
    exit(0);
}

/* Press-twice confirms (Quit Game): the first press arms `id` for 3 s, a second press inside that
 * window confirms. cjmod_confirm_state reports 1 while armed, and 2 once when an arm runs out
 * (so the mod can put the original text back), 0 otherwise. */
static unsigned int s_arm[8];
int cjmod_confirm(int id)
{
    unsigned int now = SDL_GetTicks();
    if (id < 0 || id >= 8) return 0;
    if (s_arm[id] && now - s_arm[id] < 3000u) { s_arm[id] = 0; return 1; }
    s_arm[id] = now ? now : 1;
    return 0;
}
int cjmod_confirm_state(int id)
{
    if (id < 0 || id >= 8 || !s_arm[id]) return 0;
    if (ms_since(s_arm[id]) < 3000u) return 1;
    s_arm[id] = 0;
    return 2;
}

/* Pause menu Save Game / Load Game (mods/FEPause.cpp). Choosing either row opens a slot screen in
 * the same panel: a title row and five slots for this chapter with their ages (src/quicksave.c; the
 * quick save is separate, F5 / F8). B / Back or the title row goes back. Choosing a slot closes the
 * pause menu (a Start press, src/cj_pad.c) and the save or load runs once the game is back in play
 * (qs_after_pause), so a save never captures the menu. Saving over a used slot asks for a second
 * press; loading an empty slot does nothing. The two row sets are swapped in the list's child
 * array (host-side copies of the guest pointers). */
extern void qs_after_pause(int what, int slot);
extern int qs_after_pause_pending(void);
extern void qs_slot_age(int slot, char *out, size_t n);
extern void cj_pad_inject(unsigned short dig, int polls);
#include <stdint.h>
extern void *xbox_GetMemoryOffset(void);
static int s_pick;                 /* 0 the normal rows, 1 choosing a slot to save, 2 to load */
static int s_pick_confirm;         /* slot armed for overwrite (0 none) */
static uint32_t s_rows[2][6];      /* guest pointers: [0] normal rows, [1] slot screen rows */
static char *s_pick_str[6];         /* guest strings for the slot screen rows */
static unsigned s_last_alive;
static char s_last_lab[6][64];

/* The pause menu calls this every frame; 1 when it has just been opened again (back to the normal rows). */
int cjmod_pause_alive(void)
{
    extern void qs_pause_alive(void);
    unsigned now = SDL_GetTicks();
    int reopened = s_last_alive && now - s_last_alive > 250u;
    s_last_alive = now;
    qs_pause_alive();
    return reopened;
}
/* Keep the list's current six child pointers as row set `set` (0 normal, 1 slot screen). */
void cjmod_pause_keep(char *list, int set)
{
    uint8_t *M = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t arr;
    memcpy(&arr, list + 0x1b8, 4);
    memcpy(s_rows[set & 1], M + arr, 24);
}
/* Empty the list's six child slots (so ObjectAdd, which deletes what it replaces, builds the
 * second row set without destroying the first). */
void cjmod_pause_clear(char *list)
{
    uint8_t *M = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t arr;
    memcpy(&arr, list + 0x1b8, 4);
    memset(M + arr, 0, 24);
}
/* Show row set `set`; `mode` 1 save / 2 load for the slot screen. */
void cjmod_pause_show(char *list, int set, int mode)
{
    uint8_t *M = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t arr;
    memcpy(&arr, list + 0x1b8, 4);
    memcpy(M + arr, s_rows[set & 1], 24);
    s_pick = set ? mode : 0;
    s_pick_confirm = 0;
    memset(s_last_lab, 0, sizeof s_last_lab);
}
int cjmod_pause_mode(void) { return s_pick; }
/* The guest string block for the slot screen rows, attached once by the mod (6 x 64 bytes). */
void cjmod_pause_pick_attach(char *p)
{
    int i;
    for (i = 0; i < 6; i++) { s_pick_str[i] = p + 64 * i; p[64 * i] = 0; }
}
/* Text of slot screen row i (0 title, 1..5 slots); NULL when unchanged since the last call. */
char *cjmod_pause_pick_label(int i)
{
    char t[64], age[32];
    if (i < 0 || i > 5 || !s_pick_str[0]) return 0;
    if (i == 0) snprintf(t, sizeof t, "%s    ~F Back", s_pick == 2 ? "Load from which slot?" : "Save to which slot?");
    else {
        if (s_pick_confirm && cjmod_confirm_state(3) == 2) s_pick_confirm = 0;
        qs_slot_age(i, age, sizeof age);
        if (s_pick_confirm == i) snprintf(t, sizeof t, "Slot %d    Overwrite? Press again", i);
        else snprintf(t, sizeof t, "Slot %d    %s", i, age);
    }
    if (!strcmp(t, s_last_lab[i])) return 0;
    snprintf(s_last_lab[i], sizeof s_last_lab[0], "%s", t);
    snprintf(s_pick_str[i], 64, "%s", t);
    return s_pick_str[i];
}
/* A or Start on slot `slot`: 1 when the menu is closing to save / load, 0 when nothing happens yet. */
int cjmod_pause_pick(int slot)
{
    char age[32];
    int load = s_pick == 2;
    if (qs_after_pause_pending() || slot < 1 || slot > 5) return 0;
    qs_slot_age(slot, age, sizeof age);
    if (load && !strcmp(age, "Empty")) return 0;
    if (!load && strcmp(age, "Empty")) {   /* saving over a used slot: press again within 3 s */
        if (s_pick_confirm != slot) { s_pick_confirm = slot; s_arm[3] = 0; cjmod_confirm(3); return 0; }
        if (!cjmod_confirm(3)) { s_pick_confirm = slot; return 0; }
    }
    s_pick_confirm = 0;
    qs_after_pause(load ? 2 : 1, slot);
    cj_pad_inject(0x10, 4);   /* Start: close the pause menu */
    return 1;
}
int cjmod_pause_qs_busy(void) { return qs_after_pause_pending(); }

/* Grow the pause Level list (an FELayoutScrollGrid, host view `list`) from 4 rows to 6, leaving
 * rows 1 and 2 empty for Quick Save / Quick Load. `mem` is 6 guest pointers the mod allocated.
 * Offsets: m_pChildren 0x1b8, m_ChildCount 0x1bc, m_Cols 0x1c0, m_Visible.y 0x1d4. */
#include <stdint.h>
extern void *xbox_GetMemoryOffset(void);
int cjmod_pause_grow(char *list, char *mem)
{
    uint8_t *M = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t old, c[4], gmem = (uint32_t)((uint8_t *)mem - M), six = 6, z = 0;
    int i;
    memcpy(&old, list + 0x1b8, 4);
    if (!old) return 0;
    for (i = 0; i < 4; i++) memcpy(&c[i], M + old + 4u * (uint32_t)i, 4);
    memcpy(mem + 0, &c[0], 4);
    memcpy(mem + 4, &z, 4);
    memcpy(mem + 8, &z, 4);
    memcpy(mem + 12, &c[1], 4);
    memcpy(mem + 16, &c[2], 4);
    memcpy(mem + 20, &c[3], 4);
    memcpy(list + 0x1b8, &gmem, 4);
    memcpy(list + 0x1bc, &six, 4);
    memcpy(list + 0x1c0, &six, 4);
    memcpy(list + 0x1d4, &six, 4);
    return 1;
}

/* Pause > Controller (mods/FEPause.cpp): a modern pad picture (built-in texture, src/ctrl_art.h,
 * tools/art/controller_pause.py) with its labels in two columns of nine rows, left column
 * right-aligned, each at the end of its leader line. Cell i: rows 0-8 left, 9-17 right, top to
 * bottom; the label hash XGamePad::GetControllerString takes, and a prefix for stick clicks. */
static const struct { unsigned int hash; const char *pre; } k_ctrl[18] = {
    {0x100000ddu, ""}, {0x100000deu, ""}, {0x100000e2u, ""}, {0x100000e7u, ""}, {0x100000dbu, ""},
    {0x100000e3u, ""}, {0x100000e5u, ""}, {0x100000e4u, ""}, {0x100000e6u, ""},
    {0x100000dfu, ""}, {0x100000e0u, ""}, {0x100000e1u, ""}, {0x100000d7u, ""}, {0x100000d9u, ""},
    {0x100000dau, ""}, {0x100000d8u, ""}, {0x100000e8u, ""}, {0x100000dcu, ""},
};
extern int glyph_kb_mode(void);
#include "../../keymap.h"
#include "../../pcsettings.h"
/* Guest block: 18 pad-page labels, then 22 keyboard-page labels, 64 bytes each. */
#define CTRL_PAD 18
#define CTRL_KB 22
static char *s_ctrl;
void cjmod_ctrl_attach(char *p) { if (!s_ctrl && p) { memset(p, 0, (CTRL_PAD + CTRL_KB) * 64); s_ctrl = p; } }
/* The main menu's Options > Controller page is showing (it draws the pause menu's pad art too). */
static volatile unsigned s_ctrl_page_ms;
static char *s_ctrl_grid;   /* the main menu page's label grid (guest object) */
char *cjmod_ctrl_grid(char *set) { if (set) s_ctrl_grid = set; return s_ctrl_grid; }
int cjmod_ctrl_kb(void) { return glyph_kb_mode(); }
void cjmod_ctrl_page_alive(void) { s_ctrl_page_ms = SDL_GetTicks() | 1u; }
int cjmod_ctrl_page_showing(void) { return s_ctrl_page_ms && ms_since(s_ctrl_page_ms) < 2000u; }
unsigned int cjmod_ctrl_hash(int i) { return (i >= 0 && i < 18) ? k_ctrl[i].hash : 0; }
/* Pad page label i (the picture's leader lines): empty while the keyboard list shows. */
char *cjmod_ctrl_text(int i, const char *action)
{
    char *d;
    if (!s_ctrl || i < 0 || i >= CTRL_PAD) return s_ui;
    d = s_ctrl + i * 64;
    if (!action || !*action || glyph_kb_mode()) d[0] = 0;
    else snprintf(d, 64, "%s%s", k_ctrl[i].pre, action);
    return d;
}

/* Keyboard page: 11 rows x 2 columns. Cells 0-17 are the 18 actions in the pad page's order
 * (column 0 then column 2); 18-21 the mouse rows: Mouse Look, Mouse Speed, Invert Mouse, Reset Keys. */
static const signed char k_cell_km[18] = {KM_LT, KM_WHITE, KM_BACK, -1, KM_LS, KM_UP, KM_LEFT, KM_DOWN, KM_RIGHT,
                                          KM_RT, KM_BLACK, KM_START, KM_Y, KM_X, KM_B, KM_A, -1, KM_RS};
static uint32_t s_cell[CTRL_KB];            /* guest objects, registered each frame */
int cjmod_kb_row(int k) { return k < 18 ? k % 9 : 9 + (k - 18) / 2; }
int cjmod_kb_col(int k) { return k < 18 ? (k < 9 ? 0 : 2) : ((k - 18) & 1) * 2; }
static signed char s_lit[CTRL_KB];         /* highlight applied to each cell: 0 off, 1 on, -1 unknown */
static volatile unsigned s_cells_ms;
void cjmod_kb_cell(int k, char *obj)
{
    extern void *xbox_GetMemoryOffset(void);
    uint32_t va;
    if (k < 0 || k >= CTRL_KB) return;
    va = obj ? (uint32_t)((uint8_t *)obj - (uint8_t *)xbox_GetMemoryOffset()) : 0;
    if (s_cell[k] != va) {
        if (getenv("CJ_MENU_MOUSE_LOG")) fprintf(stderr, "[KBLIST] cell %d object %08x -> %08x (t=%u)\n", k, s_cell[k], va, SDL_GetTicks());
        s_cell[k] = va; s_lit[k] = -1;
    }
    s_cells_ms = SDL_GetTicks() | 1u;
}
/* The keyboard list is on screen (a Controller page in keyboard mode). */
int cjmod_kb_visible(void) { return glyph_kb_mode() && s_cells_ms && ms_since(s_cells_ms) < 2500u; }
/* 0: leave cell k's colour; 1: make it plain; 2: make it the selection colour (src/femouse.c). */
int cjmod_kb_relight(int k)
{
    extern int femouse_kb_sel(void);
    int want;
    if (k < 0 || k >= CTRL_KB) return 0;
    want = femouse_kb_sel() == k;
    if (s_lit[k] == want) return 0;
    if (getenv("CJ_MENU_MOUSE_LOG")) fprintf(stderr, "[KBLIST] cell %d %s (was %d, t=%u)\n", k, want ? "lit" : "plain", s_lit[k], SDL_GetTicks());
    s_lit[k] = (signed char)want;
    return want ? 2 : 1;
}
/* The keyboard-page cell this guest object is, or -1 (src/femouse.c). */
int cjmod_kb_cell_of(uint32_t obj)
{
    int k;
    if (!obj) return -1;
    for (k = 0; k < CTRL_KB; k++) if (s_cell[k] == obj) return k;
    return -1;
}
uint32_t cjmod_kb_cell_obj(int k) { return (k >= 0 && k < CTRL_KB) ? s_cell[k] : 0; }
int cjmod_kb_focusable(int k)
{
    if (!glyph_kb_mode() || k < 0 || k >= CTRL_KB) return 0;
    return k >= 18 || k_cell_km[k] >= 0;
}
/* The binding a cell edits (KM_*), or -1. */
int cjmod_kb_cell_km(int k) { return (k >= 0 && k < 18) ? k_cell_km[k] : -1; }
char *cjmod_kb_text(int k, const char *action)
{
    char *d, key[24];
    if (!s_ctrl || k < 0 || k >= CTRL_KB) return s_ui;
    d = s_ctrl + (CTRL_PAD + k) * 64;
    d[0] = 0;
    if (!glyph_kb_mode()) return d;
    if (k >= 18) {
        switch (k) {
        case 18: snprintf(d, 64, "Mouse Look  %s", pcs_get(PCS_MOUSE) ? "On" : "Off"); break;
        case 19: snprintf(d, 64, "Mouse Speed  ~? %d ~@", pcs_get(PCS_MOUSE_SENS)); break;
        case 20: snprintf(d, 64, "Invert Mouse  %s", pcs_get(PCS_MOUSE_INVERT) ? "On" : "Off"); break;
        default: snprintf(d, 64, "Reset Keys"); break;
        }
        return d;
    }
    if (!action || !*action) return d;
    {
        int km = k_cell_km[k];
        if (km >= 0 && keymap_capturing() == km) {
            snprintf(d, 64, k < 9 ? "Press a key (Esc cancels)  " : "  Press a key (Esc cancels)");
            return d;
        }
        if (k == 3) snprintf(key, sizeof key, "~M");                 /* move: WASD */
        else if (k == 16) snprintf(key, sizeof key, "~O");           /* look: the mouse */
        else {
            /* the prompt glyph that follows this binding (src/glyphs.c k_bound), or its name */
            static const char *const g[18] = {"~L", "~S", "~K", 0, 0, "~=", "~?", "~>", "~@",
                                              "~R", "~T", "~J", "~Y", "~X", "~B", "~A", 0, 0};
            if (g[k]) snprintf(key, sizeof key, "%s", g[k]);
            else snprintf(key, sizeof key, "[%s]", keymap_label(km));
        }
    }
    if (k < 9) snprintf(d, 64, "%s  %s", action, key);
    else snprintf(d, 64, "%s  %s", key, action);
    return d;
}
