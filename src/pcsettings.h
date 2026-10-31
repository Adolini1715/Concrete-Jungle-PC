/* PC Settings (src/pcsettings.c): values shown on Options > PC Settings. */
#ifndef PCSETTINGS_H
#define PCSETTINGS_H
#ifdef __cplusplus
extern "C" {
#endif
enum { PCS_CHECK = 0, PCS_SLIDER = 1, PCS_SPIN = 2 };
enum { PCS_WIDESCREEN, PCS_FULLSCREEN, PCS_VSYNC, PCS_TEX_REPLACE, PCS_FPS, PCS_SCALE, PCS_MOUSE,
       PCS_MOUSE_SENS, PCS_MOUSE_INVERT, PCS_CAM_MODERN, PCS_CAM_FOLLOW, PCS_GLYPHS };
int pcs_get(int id);
unsigned pcs_gen(int id);          /* changes whenever the value changes */
void pcs_set(int id, int v);       /* also exports the env var and schedules the pcj.ini save */
void pcs_set_ini_path(const char *p);
void pcs_rebind(char *p);          /* quick load: the block now lives at p (guest RAM) */
void pcs_tick(void);               /* game thread, once per frame */
#ifdef __cplusplus
}
#endif
#endif
