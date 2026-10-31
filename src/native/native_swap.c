/*
 * native_swap.c -- checkpoint 1: replace ONE recompiled function with native
 * C++ built from the pcj-decomp source, reversibly (see docs/native-swap.md).
 *
 * Target: 0x001077C0  FEObjectScript::AddMarker(FEScriptMarker &)   (MATCH)
 *   __thiscall, ecx = this, [esp+4] = &marker, returns this in eax, ret 4.
 *
 * Mechanism (xboxrecomp >= v0.14, PR #168/#175; this file is passed to
 * tools.recomp as --exclude-manual):
 *   - `extern void sub_001077C0_gen(void);` makes the recompiled body
 *     sub_001077C0_gen; direct calls, tail jumps and dispatch reach the
 *     sub_001077C0 defined below.
 *   - `sub_001077C0_enter` is the entry hook inside the generated body. It
 *     only runs when the generated body runs, so in native mode its count
 *     must stay 0 (proof that no path bypasses the swap).
 *
 * CJ_NATIVE_SWAP=1 selects the native adapter; read once, default off.
 * CJ_SWAP_LOG=N   logs the first N calls individually (default 48).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "recomp_types.h"

extern void sub_001077C0_gen(void);
/* C++ (src/native/fe_script_native.cpp): FEObjectScript::AddMarker on guest memory. */
extern uint32_t cj_native_FEObjectScript_AddMarker(uint32_t this_va, uint32_t marker_va);

static int      s_mode = -1;       /* 0 = generated, 1 = native */
static uint32_t s_log_n = 48;
static uint64_t s_calls, s_enter_calls, s_esp_bad, s_ret_bad, s_appended;
static uint64_t s_hash = 1469598103934665603ull;   /* running FNV-1a over all calls */

static uint64_t fnv(uint64_t h, uint32_t v)
{
    for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFF; h *= 1099511628211ull; }
    return h;
}

/* Hash of everything the function reads or may write: this->m_pMarkers,
 * this->m_nMarkers, the 24-byte marker argument and the slot at
 * m_pMarkers[m_nMarkers] (pre-call index). */
static uint64_t state_hash(uint32_t self, uint32_t arg, int32_t slot_idx)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t pm = MEM32(self + 0x24);
    h = fnv(h, pm);
    h = fnv(h, MEM32(self + 0x28));
    for (int i = 0; i < 6; i++) h = fnv(h, MEM32(arg + 4 * i));
    if (pm && slot_idx >= 0 && slot_idx < 16)
        for (int i = 0; i < 6; i++) h = fnv(h, MEM32(pm + 24 * slot_idx + 4 * i));
    return h;
}

static void init_mode(void)
{
    const char *e = getenv("CJ_NATIVE_SWAP");
    const char *l = getenv("CJ_SWAP_LOG");
    s_mode = (e && e[0] == '1') ? 1 : 0;
    if (l) s_log_n = (uint32_t)strtoul(l, NULL, 0);
    fprintf(stderr, "[SWAP] sub_001077C0 FEObjectScript::AddMarker -> %s\n",
            s_mode ? "NATIVE (C++ from pcj-decomp FEScript.cpp)" : "generated");
}

void sub_001077C0_enter(void)
{
    s_enter_calls++;
}

static void native_adapter(void)
{
    uint32_t self = g_ecx;
    uint32_t marker = MEM32(g_esp + 4);          /* [esp+4]: FEScriptMarker & */
    uint32_t pm = MEM32(self + 0x24);
    int32_t  n  = (int32_t)MEM32(self + 0x28);
    g_eax = cj_native_FEObjectScript_AddMarker(self, marker);
    /* Scratch registers as the original leaves them (MSVC callers do not
     * rely on them, but matching them keeps the A/B register state exact):
     * NULL array -> ecx=this, edx=0; full -> ecx=n, edx=pm;
     * append -> ecx=marker.m_Event, edx=&pm[n]. */
    if (!pm)          { g_ecx = self; g_edx = 0; }
    else if (n >= 16) { g_ecx = (uint32_t)n; g_edx = pm; }
    else              { g_ecx = MEM32(marker + 0x14); g_edx = pm + 24u * (uint32_t)n; }
    g_esp += 8;                                   /* ret 4: return addr + 1 arg */
}

void sub_001077C0(void)
{
    if (s_mode < 0) init_mode();
    uint64_t n = ++s_calls;
    uint32_t self = g_ecx, esp0 = g_esp;
    uint32_t arg = MEM32(esp0 + 4);
    int32_t idx = (int32_t)MEM32(self + 0x28);
    uint32_t pm = MEM32(self + 0x24);
    uint64_t pre = state_hash(self, arg, idx);
    uint32_t ebx0 = g_ebx, esi0 = g_esi, edi0 = g_edi;

    if (s_mode) native_adapter(); else sub_001077C0_gen();

    if (g_esp != esp0 + 8) s_esp_bad++;
    if (g_eax != self) s_ret_bad++;
    if (g_ebx != ebx0 || g_esi != esi0 || g_edi != edi0)
        fprintf(stderr, "[SWAP] callee-saved register changed at call %llu\n",
                (unsigned long long)n);
    int32_t idx1 = (int32_t)MEM32(self + 0x28);
    if (idx1 == idx + 1) s_appended++;
    uint64_t post = state_hash(self, arg, idx);
    s_hash = fnv(fnv(fnv(fnv(fnv(s_hash, (uint32_t)pre), (uint32_t)post), g_eax), g_ecx), g_edx);

    if (n <= s_log_n)
        fprintf(stderr, "[SWAP] #%llu this=%08X arg=%08X pm=%08X n=%d->%d eax=%08X esp+%d pre=%016llx post=%016llx\n",
                (unsigned long long)n, self, arg, pm, idx, idx1, g_eax,
                (int)(g_esp - esp0), (unsigned long long)pre, (unsigned long long)post);
    if ((n & (n - 1)) == 0 || n % 1000 == 0)
        fprintf(stderr, "[SWAP] ckpt calls=%llu enter(gen body)=%llu appended=%llu esp_bad=%llu ret_bad=%llu run_hash=%016llx\n",
                (unsigned long long)n, (unsigned long long)s_enter_calls,
                (unsigned long long)s_appended, (unsigned long long)s_esp_bad,
                (unsigned long long)s_ret_bad, (unsigned long long)s_hash);
}
