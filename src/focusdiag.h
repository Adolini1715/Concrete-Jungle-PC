/* Focus / Alt+Tab / Snipping Tool diagnostics (Windows). Writes focus-diag.txt next to the exe.
 * Off unless CJ_FOCUS_DIAG=1. */
#ifndef FOCUSDIAG_H
#define FOCUSDIAG_H
#include <SDL2/SDL.h>
void focusdiag_init(SDL_Window *w);
void focusdiag_event(const SDL_Event *ev);
void focusdiag_tick(int focused);
void focusdiag_note(const char *what);
#endif
