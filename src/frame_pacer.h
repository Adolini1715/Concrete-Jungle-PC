/* Frame pacing at Swap (src/frame_pacer.c). */
#ifndef CJ_FRAME_PACER_H
#define CJ_FRAME_PACER_H
void cj_pace_before_swap(void);
void cj_pace_after_swap(void);
float cj_pace_work_ms(void);
float cj_pace_wait_ms(void);
float cj_pace_present_ms(void);
#endif
