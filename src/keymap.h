/* Keyboard and mouse bindings for the Xbox buttons (src/keymap.c). */
#ifndef KEYMAP_H
#define KEYMAP_H
#include <stdint.h>
enum { KM_A, KM_B, KM_X, KM_Y, KM_BLACK, KM_WHITE, KM_LT, KM_RT, KM_UP, KM_DOWN, KM_LEFT, KM_RIGHT,
       KM_START, KM_BACK, KM_LS, KM_RS, KM_N };
enum { KM_MOUSE_LEFT = 1001, KM_MOUSE_MIDDLE, KM_MOUSE_RIGHT, KM_WHEEL_UP, KM_WHEEL_DOWN };
int keymap_get(int slot);
unsigned keymap_gen(void);
int keymap_capturing(void);
const char *keymap_art(int slot);
const char *keymap_label(int slot);
void keymap_capture(int slot);
void keymap_cancel(void);
void keymap_reset(void);
#ifdef SDL_h_
int keymap_event(const SDL_Event *ev);
void keymap_pad(const Uint8 *k, int menu, int wheel, uint16_t *dig, uint8_t ana[8]);
#endif
#endif
