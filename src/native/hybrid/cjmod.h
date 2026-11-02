/* Native helpers callable from mods (mods/*.cpp, `cjmod_` prefix). */
#ifndef CJMOD_H
#define CJMOD_H
#ifdef __cplusplus
extern "C" {
#endif
/* 30 Hz ticks elapsed this game frame: accumulates mul / 2 (the gameplay
 * multiplier is 60 / frame rate) once per new `frame` value and returns the
 * whole ticks, so frame-counted original code keeps 30 fps timing at any rate.
 * At a steady 30 fps it returns 1 every frame. */
int cjmod_ticks30(unsigned int frame, float mul);
/* powf (frame-rate correct exponential easing). */
float cjmod_powf(float x, float y);
/* PC Settings page (src/pcsettings.c, mods/FEMainMenu.cpp). Pointers are
 * host views of a guest-heap block the mod allocates and attaches once. */
char *cjmod_pc_block(void);
void cjmod_pc_attach(char *p);
int cjmod_pc_page(int set);        /* set < 0: query */
int cjmod_pc_built(int set);      /* variant the cached Audio-state page holds */
int cjmod_pc_count(void);
int cjmod_pc_kind(int i);          /* 0 checkbox, 1 slider, 2 spinner */
int cjmod_pc_lo(int i);
int cjmod_pc_hi(int i);
int cjmod_pc_delta(int i);
char *cjmod_pc_label(int i);
char *cjmod_pc_help(int i);
char *cjmod_pc_str(int i);         /* 0 "PC Settings", 1 "Game", 2 "Options", 3 "" */
int *cjmod_pc_val(int i);
float *cjmod_pc_widths(void);
/* Modern third-person camera (src/native/hybrid/cjmod_cam.c). */
int cjmod_cam_modern(void);
int cjmod_cam_step(float game_yaw, float game_pitch, float tgt_yaw, float tgt_pitch, float rx, float ry, float lx, float ly,
                   float rot, float speed, int pmode, int cmode, int inv_x, int inv_y, float mul);
float cjmod_cam_out(int i);
float cjmod_cam_pivot_k(int axis);
float cjmod_cam_dist(float cur, float blocked);
/* Movement dials (src/native/hybrid/cjmod_move.c, docs/movement-dials.md): 1.0 = original. */
float cjmod_jump_scale(void);
float cjmod_run_accel(void);
float cjmod_run_stop(void);
float cjmod_turn_rate(void);
void cjmod_grab_log(int what, int flags, float a, float b, float mul, float probe_y);   /* CJ_GRAB_LOG=1 */
int cjmod_snappy_stop(void);
unsigned int cjmod_anim_test(void);
/* Modern movement (docs/movement-design.md). */
int cjmod_move_modern(void);
float cjmod_charge_move(void);
int cjmod_roll_pressed_within(int ms);
void cjmod_roll_consume(void);
int cjmod_roll_window_ms(void);
void cjmod_roll_start(float sx, float sz);
int cjmod_roll_active(void);
void cjmod_roll_end(void);
float cjmod_roll_vx(void);
float cjmod_roll_vz(void);
void cjmod_hard_land(float health_before, float vx, float vz);
int cjmod_hard_land_recent(int ms);
float cjmod_hard_land_health(void);
float cjmod_hard_land_vx(void);
float cjmod_hard_land_vz(void);
void cjmod_hard_land_clear(void);
void cjmod_trace_charge(float stick, float pad, float speed, unsigned flags, float timer);
float cjmod_quick_land_min(void);
float cjmod_run_curve(float f);
float cjmod_run_inv(float g);
float cjmod_ground(float f);
float cjmod_run_remap(float mag);
float cjmod_turn_mult(void);
float cjmod_turn_cost(void);
float cjmod_air_push(void);
float cjmod_stop_frac(void);
int cjmod_run_charging(void);
void cjmod_run_charge_touch(int on);
float cjmod_charge_cap(float mag);
void cjmod_crouch_hold_start(void);
int cjmod_crouch_hold(float dt);
void cjmod_charge_feedback(float timer, int power_ok, int medium_ok);
float cjmod_attack_carry(float mul);
/* Front-end mods: guest string block, host clock, quit. */
char *cjmod_ui_block(void);
void cjmod_ui_attach(char *p);
char *cjmod_ui_str(int i);
unsigned int cjmod_ms(void);
void cjmod_quit(void);
int cjmod_confirm(int id);
int cjmod_confirm_state(int id);
/* Pause menu Save Game / Load Game slot screen (mods/FEPause.cpp). */
int cjmod_pause_alive(void);
void cjmod_pause_keep(char *list, int set);
void cjmod_pause_show(char *list, int set, int mode);
void cjmod_pause_clear(char *list);
int cjmod_pause_mode(void);
void cjmod_pause_pick_attach(char *p);
char *cjmod_pause_pick_label(int i);
int cjmod_pause_pick(int slot);
int cjmod_pause_qs_busy(void);
int cjmod_pause_grow(char *list, char *mem);
void cjmod_ctrl_attach(char *p);
unsigned int cjmod_ctrl_hash(int i);
char *cjmod_ctrl_text(int i, const char *action);
char *cjmod_ctrl_grid(char *set);
void cjmod_ctrl_page_alive(void);
int cjmod_ctrl_kb(void);
int cjmod_kb_row(int k);
int cjmod_kb_col(int k);
void cjmod_kb_cell(int k, char *obj);
int cjmod_kb_focusable(int k);
int cjmod_kb_relight(int k);
char *cjmod_kb_text(int k, const char *action);
int cjmod_ctrl_page_showing(void);
#ifdef __cplusplus
}
#endif
#endif
