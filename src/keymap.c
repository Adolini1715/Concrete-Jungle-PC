/*
 * Keyboard and mouse bindings (rebindable on the Controller pages; src/femouse.c, cjmod.c).
 *
 * Each Xbox button the game reads has one key or mouse button (KM_* below); Move (WASD) and the
 * look keys (IJKL, or the mouse with mouse look on) are fixed. The menu keys always work as well,
 * whatever is bound: the arrow keys for the d-pad, Enter for Start, Backspace for Back, and Esc for
 * Back in the menus but Start (pause) in play. Weapon select (the Xbox Back button) is Tab.
 * Bindings are saved in pcj.ini as CJ_KEY_<button>=<key name> (SDL's key names, plus
 * "Mouse Left/Middle/Right" and "Wheel Up/Down"); a missing line means the default.
 *
 * Mouse buttons only act as bindings in play: while a menu takes input they are the menu
 * mouse (src/femouse.c). The wheel is a short press per notch.
 *
 * Rebinding: keymap_capture(slot) waits for the next key or mouse button (Esc cancels); W A S D,
 * I J K L and the F keys the port uses (F3 F5 F8 F10 F11) are refused with a note. A key already
 * bound to another button swaps with it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "keymap.h"
/* src/glyphs.c: the keys that have prompt art (glyph_art.h glyph_keys) */
extern int glyph_key_count(void);
extern int glyph_key_code(int i);
extern const char *glyph_key_art(int i);
extern const char *glyph_key_label(int i);

extern void pcs_set_str(const char *key, const char *val);
extern void qs_osd_note(const char *m);

static const char *const k_key_name[KM_N] = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT", "UP", "DOWN",
                                             "LEFT", "RIGHT", "START", "BACK", "LS", "RS"};
static const int k_default[KM_N] = {
    SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_Q, SDL_SCANCODE_Z, SDL_SCANCODE_C,
    SDL_SCANCODE_LSHIFT, SDL_SCANCODE_F, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_RETURN, SDL_SCANCODE_TAB, SDL_SCANCODE_1, SDL_SCANCODE_2};
static int s_bind[KM_N];
static int s_init;
static volatile int s_capture = -1;     /* slot waiting for a key, or -1 */
static volatile unsigned s_gen = 1;     /* bumped on every change (prompt art follows) */
static int s_wheel_up_hold, s_wheel_down_hold;

static const char *mouse_name(int code)
{
    switch (code) {
    case KM_MOUSE_LEFT: return "Mouse Left";
    case KM_MOUSE_MIDDLE: return "Mouse Middle";
    case KM_MOUSE_RIGHT: return "Mouse Right";
    case KM_WHEEL_UP: return "Wheel Up";
    case KM_WHEEL_DOWN: return "Wheel Down";
    }
    return NULL;
}
static int code_from_name(const char *s)
{
    int c;
    for (c = KM_MOUSE_LEFT; c <= KM_WHEEL_DOWN; c++)
        if (!SDL_strcasecmp(s, mouse_name(c))) return c;
    c = SDL_GetScancodeFromName(s);
    return c > 0 ? c : -1;
}
static const char *code_name(int code)
{
    const char *m = mouse_name(code);
    return m ? m : SDL_GetScancodeName((SDL_Scancode)code);
}

static void init(void)
{
    int i;
    char k[32];
    if (s_init) return;
    s_init = 1;
    for (i = 0; i < KM_N; i++) {
        const char *e;
        s_bind[i] = k_default[i];
        snprintf(k, sizeof k, "CJ_KEY_%s", k_key_name[i]);
        if ((e = getenv(k)) && *e) {
            int c = code_from_name(e);
            if (i == KM_BACK && c == SDL_SCANCODE_ESCAPE) c = SDL_SCANCODE_TAB;   /* the old default: Esc pauses now */
            if (c > 0) s_bind[i] = c;
            else fprintf(stderr, "[KEYS] %s: unknown key \"%s\"\n", k, e);
        }
    }
}

int keymap_get(int slot) { init(); return (slot >= 0 && slot < KM_N) ? s_bind[slot] : 0; }
unsigned keymap_gen(void) { return s_gen; }
int keymap_capturing(void) { return s_capture; }

/* The prompt art for a binding (glyph_art.h name), or NULL. */
const char *keymap_art(int slot)
{
    int c = keymap_get(slot), i;
    for (i = 0; i < glyph_key_count(); i++) if (glyph_key_code(i) == c) return glyph_key_art(i);
    return NULL;
}
/* A short label for a binding (for text, where there is no icon). */
const char *keymap_label(int slot)
{
    int c = keymap_get(slot), i;
    for (i = 0; i < glyph_key_count(); i++) if (glyph_key_code(i) == c) return glyph_key_label(i);
    return code_name(c);
}

static void save(int slot)
{
    char k[32];
    snprintf(k, sizeof k, "CJ_KEY_%s", k_key_name[slot]);
    pcs_set_str(k, code_name(s_bind[slot]));
}

void keymap_capture(int slot)
{
    init();
    if (slot >= 0 && slot < KM_N) s_capture = slot;
}
void keymap_cancel(void) { s_capture = -1; }

/* Restore every default. */
void keymap_reset(void)
{
    int i;
    init();
    for (i = 0; i < KM_N; i++) if (s_bind[i] != k_default[i]) { s_bind[i] = k_default[i]; save(i); }
    s_gen++;
}

static int refused(int c)
{
    switch (c) {
    case SDL_SCANCODE_W: case SDL_SCANCODE_A: case SDL_SCANCODE_S: case SDL_SCANCODE_D:
    case SDL_SCANCODE_I: case SDL_SCANCODE_J: case SDL_SCANCODE_K: case SDL_SCANCODE_L:
    case SDL_SCANCODE_F3: case SDL_SCANCODE_F5: case SDL_SCANCODE_F8: case SDL_SCANCODE_F10: case SDL_SCANCODE_F11:
        return 1;
    }
    {   /* only keys with a prompt icon */
        int i;
        for (i = 0; i < glyph_key_count(); i++) if (glyph_key_code(i) == c) return 0;
    }
    return 1;
}

/* GL thread, every SDL event while a capture is pending. 1 = the event was used. */
int keymap_event(const SDL_Event *ev)
{
    int c = -1, slot = s_capture, i;
    static unsigned armed_ms;
    if (slot < 0) { armed_ms = 0; return 0; }
    if (!armed_ms) armed_ms = SDL_GetTicks();
    if (SDL_GetTicks() - armed_ms < 250u) return ev->type == SDL_KEYDOWN || ev->type == SDL_MOUSEBUTTONDOWN;  /* the press that opened it */
    if (ev->type == SDL_KEYDOWN && !ev->key.repeat) {
        if (ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE) { s_capture = -1; armed_ms = 0; return 1; }
        c = ev->key.keysym.scancode;
    } else if (ev->type == SDL_MOUSEBUTTONDOWN) {
        c = ev->button.button == SDL_BUTTON_LEFT ? KM_MOUSE_LEFT : ev->button.button == SDL_BUTTON_RIGHT ? KM_MOUSE_RIGHT
          : ev->button.button == SDL_BUTTON_MIDDLE ? KM_MOUSE_MIDDLE : -1;
    } else if (ev->type == SDL_MOUSEWHEEL && ev->wheel.y) {
        c = ev->wheel.y > 0 ? KM_WHEEL_UP : KM_WHEEL_DOWN;
    } else if (ev->type == SDL_KEYUP || ev->type == SDL_MOUSEBUTTONUP || ev->type == SDL_MOUSEMOTION) {
        return 1;
    }
    if (c < 0) return 0;
    if (refused(c)) {
        qs_osd_note(c == SDL_SCANCODE_W || c == SDL_SCANCODE_A || c == SDL_SCANCODE_S || c == SDL_SCANCODE_D ||
                    c == SDL_SCANCODE_I || c == SDL_SCANCODE_J || c == SDL_SCANCODE_K || c == SDL_SCANCODE_L
                        ? "That key moves or looks. Pick another (Esc cancels)"
                        : "That key can't be bound. Pick another (Esc cancels)");
        return 1;
    }
    for (i = 0; i < KM_N; i++)
        if (i != slot && s_bind[i] == c) { s_bind[i] = s_bind[slot]; save(i); }   /* swap */
    s_bind[slot] = c;
    save(slot);
    s_gen++;
    s_capture = -1;
    armed_ms = 0;
    fprintf(stderr, "[KEYS] %s = %s\n", k_key_name[slot], code_name(c));
    return 1;
}

/* In a level (not the front end: FEMainMenu::ms_pInstance is gone, or has no manager, while playing). */
static int in_level(void)
{
    extern void *xbox_GetMemoryOffset(void);
    const uint8_t *M = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t fe, mgr;
    if (!M) return 0;
    memcpy(&fe, M + 0x0048CE58u, 4);
    if (!fe) return 1;
    memcpy(&mgr, M + fe + 0x1C0u, 4);
    return mgr == 0;
}

/* A front-end dialog has the input (FEListener::m_pDialogListener). */
static int dialog_open(void)
{
    extern void *xbox_GetMemoryOffset(void);
    const uint8_t *M = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t d;
    if (!M) return 0;
    memcpy(&d, M + 0x0048E00Cu, 4);
    return d != 0;
}

/* GL thread: the pad buttons the bindings press. `menu`: a menu takes input (mouse buttons off).
 * wheel: notches this poll (+ up). dig/ana as in CjHostPad. */
void keymap_pad(const Uint8 *k, int menu, int wheel, uint16_t *dig, uint8_t ana[8])
{
    static const struct { int slot, dig_bit, ana_i; } map[KM_N] = {
        {KM_A, 0, 0}, {KM_B, 0, 1}, {KM_X, 0, 2}, {KM_Y, 0, 3}, {KM_BLACK, 0, 4}, {KM_WHITE, 0, 5},
        {KM_LT, 0, 6}, {KM_RT, 0, 7}, {KM_UP, 0x01, -1}, {KM_DOWN, 0x02, -1}, {KM_LEFT, 0x04, -1},
        {KM_RIGHT, 0x08, -1}, {KM_START, 0x10, -1}, {KM_BACK, 0x20, -1}, {KM_LS, 0x40, -1}, {KM_RS, 0x80, -1}};
    Uint32 mb = SDL_GetMouseState(NULL, NULL);
    int i;
    init();
    if (s_capture >= 0) return;           /* rebinding: the game sees nothing */
    if (wheel > 0) s_wheel_up_hold = 3;
    if (wheel < 0) s_wheel_down_hold = 3;
    for (i = 0; i < KM_N; i++) {
        int c = s_bind[map[i].slot], on = 0;
        if (c > 0 && c < SDL_NUM_SCANCODES) on = k[c] != 0;
        else if (!menu) {
            if (c == KM_MOUSE_LEFT) on = (mb & SDL_BUTTON_LMASK) != 0;
            else if (c == KM_MOUSE_RIGHT) on = (mb & SDL_BUTTON_RMASK) != 0;
            else if (c == KM_MOUSE_MIDDLE) on = (mb & SDL_BUTTON_MMASK) != 0;
            else if (c == KM_WHEEL_UP) on = s_wheel_up_hold > 0;
            else if (c == KM_WHEEL_DOWN) on = s_wheel_down_hold > 0;
        }
        if (!on) continue;
        if (map[i].ana_i >= 0) ana[map[i].ana_i] = 255;
        else *dig |= (uint16_t)map[i].dig_bit;
    }
    if (s_wheel_up_hold > 0) s_wheel_up_hold--;
    if (s_wheel_down_hold > 0) s_wheel_down_hold--;
    /* the menu keys, always */
    if (k[SDL_SCANCODE_UP]) *dig |= 1;
    if (k[SDL_SCANCODE_DOWN]) *dig |= 2;
    if (k[SDL_SCANCODE_LEFT]) *dig |= 4;
    if (k[SDL_SCANCODE_RIGHT]) *dig |= 8;
    if ((k[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & KMOD_ALT)) || k[SDL_SCANCODE_KP_ENTER]) *dig |= 0x10;
    /* Esc: in a level it is Start, so it opens and closes the pause menu (the pause menu counts as
     * a menu for the pointer, so `menu` can't decide this); a dialog (the quit question) takes
     * Back. In the front end it is Back. Backspace is always Back. */
    {   static int was;
        int lvl = in_level(), back = !lvl || dialog_open();
        if (k[SDL_SCANCODE_ESCAPE]) *dig |= back ? 0x20 : 0x10;
        if (k[SDL_SCANCODE_ESCAPE] && !was && getenv("CJ_MENU_MOUSE_LOG"))
            fprintf(stderr, "[KEYS] Esc: menu %d in_level %d -> %s\n", menu, lvl, back ? "Back" : "Start");
        was = k[SDL_SCANCODE_ESCAPE];
    }
    if (k[SDL_SCANCODE_BACKSPACE]) *dig |= 0x20;
}
