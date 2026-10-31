/* Background file writing (src/async_io.c). */
#ifndef CJ_ASYNC_IO_H
#define CJ_ASYNC_IO_H
#include <stdio.h>

struct SDL_Surface;

FILE *cj_afile_open(const char *path, const char *mode);   /* "w"/"wb": written by a background thread */
int cj_afile_redirect_std(const char *path);                /* stdout+stderr -> path via a background thread; 1 on success */
int cj_afile_save_bmp(struct SDL_Surface *sf, const char *path);   /* takes ownership of sf */
void cj_afile_drain(int ms);                                /* flush and wait (at most ms) for the writers */

#endif
