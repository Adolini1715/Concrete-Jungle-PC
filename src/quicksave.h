/* Quick save / quick load (src/quicksave.c). */
#ifndef QUICKSAVE_H
#define QUICKSAVE_H
#ifdef __cplusplus
extern "C" {
#endif
void qs_request(int what);          /* the quick save: 1 save, 2 load, at the next frame's safe point */
void qs_request_slot(int what, int slot);   /* 1 save / 2 load to slot 0 (quick save) or 1..5 (this chapter) */
int qs_slot(void);                  /* the slot the last save or load used */
const char *qs_message(void);       /* status line to show, or NULL */
unsigned qs_message_gen(void);      /* changes with each new message */
#ifdef __cplusplus
}
#endif
#endif
