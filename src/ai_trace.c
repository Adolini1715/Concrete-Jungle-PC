/* AI trace (debug builds, -DCJ_AI_TRACE=ON): linker wraps on the StateMachine
 * transition functions log every Gosub/Goto/ExtGosub/ExtGoto whose target is in
 * CJ_AI_TRACE (comma list of state VAs, "all" for every transition): the guest
 * caller address, the actor, its current state and the message it is handling.
 * Only calls from another generated chunk than the callee's are seen. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"

extern void __real_m_Gosub_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_000803D0(void);
extern void __real_m_Goto_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_00080450(void);
extern void __real_m_ExtGosub_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_00080490(void);
extern void __real_m_ExtGoto_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_00080530(void);
unsigned cj_gl_flips(void);

static int s_on = -1, s_all;
static char s_list[512];

static void trace(const char *kind)
{
    uint32_t ret = MEM32(g_esp), state = MEM32(g_esp + 8), sm = g_ecx;
    char key[16];
    if (s_on < 0) {
        const char *e = getenv("CJ_AI_TRACE");
        s_on = e && *e;
        if (s_on) { s_all = !strcmp(e, "all"); snprintf(s_list, sizeof s_list, ",%s,", e); }
    }
    if (!s_on || !state) return;
    snprintf(key, sizeof key, ",%X,", state);
    if (!s_all && !strstr(s_list, key)) return;
    {
        uint32_t a = MEM32(sm + 0x50);
        fprintf(stderr, "[AI] flip %u %s -> %X from %08X actor ref %X cur %X msg %X (%X %X) args %X %X\n",
                cj_gl_flips(), kind, state, ret, a ? MEM32(a + 0x770) : 0, MEM32(sm + 0x58), MEM32(sm + 0x88),
                MEM32(sm + 0x8C), MEM32(sm + 0x90), MEM32(g_esp + 12), MEM32(g_esp + 16));
    }
}

void __wrap_m_Gosub_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_000803D0(void) { trace("Gosub"); __real_m_Gosub_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_000803D0(); }
void __wrap_m_Goto_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_00080450(void) { trace("Goto"); __real_m_Goto_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_00080450(); }
void __wrap_m_ExtGosub_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_00080490(void) { trace("ExtGosub"); __real_m_ExtGosub_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_00080490(); }
void __wrap_m_ExtGoto_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_00080530(void) { trace("ExtGoto"); __real_m_ExtGoto_StateMachine__QAEXIP6ACAAV1__ZHHHH_Z_00080530(); }

/* Player teleports and screen fades (cutscene timing). */
extern void __real_m_TeleportPlayer_XItemHandler_Player__QAEXAAVEXVector__M_Z_0015F900(void);
extern void __real_m_ApplyFadeOutIn_XApp__QAEXMVRGBA_ex__CC_Z_000CCC70(void);
static int tp_on(void) { static int on = -1; if (on < 0) on = getenv("CJ_AI_TRACE") != NULL; return on; }
void __wrap_m_TeleportPlayer_XItemHandler_Player__QAEXAAVEXVector__M_Z_0015F900(void)
{
    if (tp_on()) {
        uint32_t v = MEM32(g_esp + 4);
        float x, y, z; uint32_t t;
        t = MEM32(v); memcpy(&x, &t, 4); t = MEM32(v + 4); memcpy(&y, &t, 4); t = MEM32(v + 8); memcpy(&z, &t, 4);
        uint32_t e = g_esi; float tm; t = MEM32(e + 0x5C); memcpy(&tm, &t, 4);
        fprintf(stderr, "[AI] flip %u TeleportPlayer %.2f %.2f %.2f from %08X cut %u fadedone %u env %08X item %08X fl38 %X mi3c %X ch54 %X ch58 %X t5c %.2f ty80 %X b84 %08X\n",
                cj_gl_flips(), x, y, z, MEM32(g_esp), MEM32(0x00483548u) & 0xFF, MEM32(0x004833E0u) >> 16 & 0xFF,
                e, MEM32(e + 4), MEM32(e + 0x38), MEM32(e + 0x3C), MEM32(e + 0x54), MEM32(e + 0x58), tm, MEM32(e + 0x80), MEM32(e + 0x84));
    }
    __real_m_TeleportPlayer_XItemHandler_Player__QAEXAAVEXVector__M_Z_0015F900();
}
void __wrap_m_ApplyFadeOutIn_XApp__QAEXMVRGBA_ex__CC_Z_000CCC70(void)
{
    if (tp_on()) {
        uint32_t t = MEM32(g_esp + 4); float d; memcpy(&d, &t, 4);
        fprintf(stderr, "[AI] flip %u ApplyFadeOutIn %.2f col %08X %u %u from %08X cut %u\n", cj_gl_flips(), d, MEM32(g_esp + 8),
                MEM32(g_esp + 12) & 0xFF, MEM32(g_esp + 16) & 0xFF, MEM32(g_esp), MEM32(0x00483548u) & 0xFF);
    }
    __real_m_ApplyFadeOutIn_XApp__QAEXMVRGBA_ex__CC_Z_000CCC70();
}
