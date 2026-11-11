/*
 * CJ_VIEWER=1 / CJ_VIEWER=2: start Eurocom's own model viewer instead of the game.
 *
 * The retail game still contains EngineX's geometry viewers (EXGeoViewerWnd / EXGeoViewerWnd2,
 * as XObjViewWnd / XObjViewWnd2). XApp::InitGame (0xB0060) opens one instead of the game when
 * the app's argument list holds "viewer" or "viewer2" (pcj-decomp src/game/App.cpp); the
 * developers' debug menu added that argument and restarted (GameWnd.cpp ADD_ARG_AND_RESTART).
 * Here the argument goes in the same way, through the game's own EXString(const char *) and
 * EXStringArray::Add, just before InitGame runs, with the literals the game itself compares
 * against. Reached through recomp_lookup_manual (InitGame is virtual).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "recomp_types.h"

extern void m_InitGame_XApp__UAECXZ_000B0060(void);
extern void m_0EXString__QAE_PBD_Z_001ABD40(void);                     /* EXString::EXString(const char *) */
extern void m_Add_EXStringArray__QAEHABVEXString___Z_001ACD10(void);  /* EXStringArray::Add(const EXString &) */

#define APP_PTR_VA   0x004931E8u   /* EXBaseApp::m_pTheApp */
#define APP_ARGS_OFF 0x0Cu         /* EXBaseApp::m_Args (EXStringArray: max, grow, size, data) */
#define LIT_VIEWER   0x003877BCu   /* "viewer"  */
#define LIT_VIEWER2  0x003877B4u   /* "viewer2" */
#define RET_VA       0x0000C0DEu

static void initgame_viewer(void)
{
    const char *e = getenv("CJ_VIEWER");
    uint32_t app = MEM32(APP_PTR_VA);
    if (app && e && (*e == '1' || *e == '2')) {
        uint32_t esp0 = g_esp, ecx0 = g_ecx, str;
        g_esp -= 32;
        str = g_esp + 16;                 /* the EXString object (one pointer) */
        MEM32(str) = 0;
        PUSH32(g_esp, *e == '2' ? LIT_VIEWER2 : LIT_VIEWER);
        PUSH32(g_esp, RET_VA);
        g_ecx = str;
        m_0EXString__QAE_PBD_Z_001ABD40();
        g_esp = esp0 - 32;
        PUSH32(g_esp, str);
        PUSH32(g_esp, RET_VA);
        g_ecx = app + APP_ARGS_OFF;
        m_Add_EXStringArray__QAEHABVEXString___Z_001ACD10();
        g_esp = esp0;
        g_ecx = ecx0;
        fprintf(stderr, "[VIEWER] argument \"%s\" added (args now %u): starting the model viewer\n",
                *e == '2' ? "viewer2" : "viewer", MEM32(app + APP_ARGS_OFF + 8));
    }
    m_InitGame_XApp__UAECXZ_000B0060();
}

recomp_func_t viewer_lookup(uint32_t va)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("CJ_VIEWER"); on = e && (*e == '1' || *e == '2'); }
    return (on && va == 0x000B0060u) ? (recomp_func_t)initgame_viewer : (recomp_func_t)0;
}
