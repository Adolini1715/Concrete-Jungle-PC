/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>   /* ptrdiff_t */
#include <math.h>     /* NAN */
#include "pcsettings.h"
#include <stdlib.h>
#include "recomp_types.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern __thread uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t cj_det_lookup(uint32_t xbox_va);   /* src/determinism.c */

/* CRT fmod special cases. _CIfmod (0x2DA5AE) classifies both x87 operands
 * with fxam and tail-jumps through the table at 0x475280 (_trandisp2,
 * `jmp [ebx]` at 0x2DCF4D). Two of its targets start mid-function, so the
 * lifter never made them entry points and the indirect jump was skipped:
 * both operands stayed on the x87 stack and fmod returned one of its inputs.
 *   0x2DCF60  fstp st0; fstp st0; fldz; ret        fmod(+-0, y) = 0
 *   0x2DD002  fstp st0; fstp st0; fld QNaN; (errno) fmod(x, 0) etc. = NaN
 * fmod(0, len) is the common one (animation / scroll time at exactly 0), so
 * the game sampled at `len` for that frame instead of 0. */
extern __thread uint32_t g_esp;
extern __thread int g_fp_top;
extern __thread double g_fp_stack[8];
static void crt_fmod_zero_002DCF60(void)
{
    g_fp_top = (g_fp_top + 2u) & 7u;            /* fstp st0 x2 */
    g_fp_top = (g_fp_top + 7u) & 7u;            /* fldz */
    g_fp_stack[g_fp_top] = 0.0;
    g_esp += 4;                                 /* ret */
}
static void crt_fmod_nan_002DD002(void)
{
    g_fp_top = (g_fp_top + 2u) & 7u;
    g_fp_top = (g_fp_top + 7u) & 7u;
    g_fp_stack[g_fp_top] = NAN;
    g_esp += 4;
}

/* Larger menu text: XWnd::FontPrint(const char *, const EXRect &, float scale, align, b) at
 * 0x0010FF80, which every queued front-end text goes through (always via the vtable), gets its
 * scale multiplied for the game's menu font (font hash at this+0x2A0 = HT_Font_Small 0x07000004;
 * the button-prompt font 0x0700000C keeps its size, its icons are fixed-size sprites). Lines are
 * laid out at that size, so alignment and wrapping follow.
 * The HUD window (vtable 0x00387CC0) keeps the original size. CJ_FE_TEXT (default 1.3, 1 = off). */
static float fe_text_scale(void)
{
    static float s = -1.0f;
    if (s < 0.0f) {
        const char *e = getenv("CJ_FE_TEXT");
        s = e ? (float)atof(e) : 1.0f;
        if (!(s >= 0.5f && s <= 3.0f)) s = 1.0f;
    }
    return s;
}
/* Windows whose layout is fixed-size and packed (the HUD, and the pause menu's option pages,
 * whose labels and values share 0x1d0 units) keep the original text size. */
static int fe_text_window(uint32_t self)
{
    uint32_t vt = MEM32(self);
    return vt != 0x00387CC0u /* FEHud */ && vt != 0x00389A18u /* FEPause */;
}
/* CJ_FONT_DUMP=1: log each font's ASCII glyph metrics once (dev aid). */
static void fe_font_dump(uint32_t self)
{
    static uint32_t seen[16]; static int n, on = -1;
    if (on < 0) on = getenv("CJ_FONT_DUMP") != NULL;
    uint32_t f = MEM32(self + 0xA8u);
    if (!on || !f) return;
    for (int i = 0; i < n; i++) if (seen[i] == f) return;
    if (n < 16) seen[n++] = f;
    uint32_t pg = f + 4u, po = MEM32(pg);
    if (!po) return;
    uint32_t page = pg + po;
    fprintf(stderr, "[FONT] font=%08x hash=%08x\n", f, MEM32(self + 0x2A0u));
    for (int c = 0x20; c < 0x7f; c++) {
        uint32_t ca = page + (uint32_t)c * 4u, co = MEM32(ca);
        if (!co) continue;
        uint32_t ch = ca + co;
        float u0, v0, u1, v1;
        uint32_t r;
        r = MEM32(ch); memcpy(&u0, &r, 4); r = MEM32(ch + 4); memcpy(&v0, &r, 4);
        r = MEM32(ch + 8); memcpy(&u1, &r, 4); r = MEM32(ch + 12); memcpy(&v1, &r, 4);
        uint32_t wh = MEM32(ch + 0x10u), k = MEM32(ch + 0x14u) & 0xffffu;
        fprintf(stderr, "[FONT] '%c' w=%u h=%u kern=%u uv=%.4f,%.4f-%.4f,%.4f\n", c,
                wh & 0xffffu, wh >> 16, k, u0, v0, u1, v1);
    }
}
/* Front-end font fixes (src/font_kern.h, tools/art/font_kern.py).
 *
 * 1. Clipped letters. The game's glyph UVs are pixel / (size - 1), not pixel / size: every cell
 *    slides right of its glyph by up to a texel across the atlas, so the right-hand glyphs lose most
 *    of their first ink column and gain a blank one ('L' starts drawing at column 199.78, its ink at
 *    199). Measured on all three fonts: with u * (size - 1) every cell lands on a whole column, every
 *    glyph's ink starts on its first column, and the column before every cell is blank. So when a
 *    font is first seen its UVs are rescaled to pixel / size, and each cell gains that blank column on
 *    its left (u0 one texel back, m_Width + 1), so the letter's left edge fades out under filtering
 *    like its right edge instead of ending in a hard cut. Fonts are reloaded at new addresses; a font
 *    is recognised as fixed when its 'a' spans whole texels of a power-of-two atlas.
 * 2. Even, roomier spacing. Text is laid out on whole 640x480 units with each advance truncated after
 *    scaling, so the stock metrics gave uneven gaps ("Ne w"). For the scale the text is measured and
 *    drawn at, each glyph's m_Kern is chosen so the gap from its ink to the next letter's ink comes
 *    out as close to k_gap source pixels as whole units allow. Measuring and drawing both read it.
 * CJ_FONT_KERN=0 keeps the stock metrics (both fixes off). */
extern void *xbox_GetMemoryOffset(void);
#include "font_kern.h"
static const float k_gap = 2.0f;   /* source pixels between letters' ink */
static float rdf(uint8_t *M, uint32_t a) { float f; memcpy(&f, M + a, 4); return f; }
static void wrf(uint8_t *M, uint32_t a, float f) { memcpy(M + a, &f, 4); }
static int font_table_ok(uint8_t *M, uint32_t page)
{
    /* Every glyph entry must be a small forward offset to an entry inside the font's own block
     * whose texture coordinates are sane, or nothing is written. Writing through one bad offset
     * once scaled a code pointer to 0.0 in the HUD's vtable (FEHud +0x110), so opening the weapon
     * selector called a null method in a loop until the game crashed. */
    for (int c = 0; c < 256; c++) {
        uint32_t a = page + (uint32_t)c * 4u, o = MEM32(a);
        if (!o) continue;
        if (o < 4u || a + o < page + 0x400u || o > 0x20000u || page < 0x80000000u) {
            static unsigned n_bad;
            if (n_bad++ < 4) fprintf(stderr, "[FONT] font table at %08X: entry %d offset %08X out of range; left as is\n", page, c, o);
            return 0;
        }
        uint32_t e = a + o;
        float eu0 = rdf(M, e), eu1 = rdf(M, e + 8u), ev0 = rdf(M, e + 4u), ev1 = rdf(M, e + 12u);
        if (!(eu0 >= 0.0f && eu1 <= 1.001f && eu0 <= eu1 && ev0 >= 0.0f && ev1 <= 1.001f && ev0 <= ev1)) {
            static unsigned n_bad;
            if (n_bad++ < 4) fprintf(stderr, "[FONT] font table at %08X: entry %d is not a glyph; left as is\n", page, c);
            return 0;
        }
    }
    return 1;
}
/* Rescale the font's UVs and pad its cells, once. Returns the font's original 'a' u0 (to find its
 * ink table), or -1. */
static float fe_font_fix(uint32_t page)
{
    uint8_t *M = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t ca = page + 'a' * 4u, co = MEM32(ca);
    if (!co) return -1.0f;
    uint32_t g = ca + co;
    float u0 = rdf(M, g), u1 = rdf(M, g + 8u), v0 = rdf(M, g + 4u), v1 = rdf(M, g + 12u);
    uint16_t w, h;
    memcpy(&w, M + g + 0x10u, 2);
    memcpy(&h, M + g + 0x12u, 2);
    if (!w || !h || !(u1 > u0) || !(v1 > v0)) return -1.0f;
    if (!font_table_ok(M, page)) return -1.0f;   /* also guards fe_font_kern's writes, which need this to pass */
    float du = (float)w / (u1 - u0), dv = (float)h / (v1 - v0);   /* texels per unit of u, v */
    float ru = roundf(du), rv = roundf(dv);
    if (fabsf(du - ru) > 0.2f || fabsf(dv - rv) > 0.2f) return -1.0f;
    int pu = ((int)ru & ((int)ru - 1)) == 0, pv = ((int)rv & ((int)rv - 1)) == 0;   /* power of two: fixed */
    if (pu && pv) return (u0 * ru + 1.0f) / (ru - 1.0f);   /* already fixed: the original 'a' u0 */
    if (pu || pv) return -1.0f;                             /* neither layout: leave it */
    {   /* only the off-by-one layout: size - 1 with a power-of-two size */
        int W1 = (int)ru + 1, H1 = (int)rv + 1;
        if ((W1 & (W1 - 1)) || (H1 & (H1 - 1))) return -1.0f;
    }
    float W = ru + 1.0f, H = rv + 1.0f;   /* the atlas: pixel / (W - 1) was used */
    float orig_a = u0;
    for (int c = 0; c < 256; c++) {
        uint32_t a = page + (uint32_t)c * 4u, o = MEM32(a);
        if (!o) continue;
        uint32_t e = a + o;
        uint16_t ew;
        float x0 = rdf(M, e) * ru, x1 = rdf(M, e + 8u) * ru;
        memcpy(&ew, M + e + 0x10u, 2);
        int pad = ew && x0 >= 1.0f;
        wrf(M, e, (x0 - (pad ? 1.0f : 0.0f)) / W);
        wrf(M, e + 8u, x1 / W);
        wrf(M, e + 4u, rdf(M, e + 4u) * rv / H);
        wrf(M, e + 12u, rdf(M, e + 12u) * rv / H);
        if (pad) { ew++; memcpy(M + e + 0x10u, &ew, 2); }
    }
    return orig_a;
}
static void fe_font_kern_(uint32_t self, float s);
/* Tripwire for the Tab crash: FEHud's vtable slot +0x110 (XWnd::CalculateAlignment, 0x0010CF30)
 * read 0 on a player's machine. If the font code ever changes it, say so and put it back. */
static void fe_font_kern(uint32_t self, float s)
{
    uint32_t before = MEM32(0x00387DD0u);
    fe_font_kern_(self, s);
    if (MEM32(0x00387DD0u) != before) {
        static unsigned n;
        if (n++ < 4) fprintf(stderr, "[FONT] the font code changed FEHud's vtable (+0x110 %08X -> %08X; window %08X font %08X); restored\n",
                             before, MEM32(0x00387DD0u), self, MEM32(self + 0xA8u));
        MEM32(0x00387DD0u) = before;
    }
}
static void fe_font_kern_(uint32_t self, float s)
{
    static struct { uint32_t f; float s; const FontInk *fk; } seen[32];
    static int n, on = -1;
    if (on < 0) { const char *e = getenv("CJ_FONT_KERN"); on = !(e && e[0] == '0'); }
    uint32_t f = MEM32(self + 0xA8u);
    if (!on || !f || !(s > 0.2f && s < 8.0f)) return;
    uint32_t pg = f + 4u, po = MEM32(pg);
    if (!po) return;
    uint32_t page = pg + po;
    /* (re)check on every call: a font freed and reloaded at the same address comes back unfixed */
    float au0 = fe_font_fix(page);
    if (au0 < 0.0f) return;
    int slot = -1;
    for (int i = 0; i < n; i++) if (seen[i].f == f) { slot = i; break; }
    const FontInk *fk = NULL;
    for (size_t i = 0; i < sizeof k_font_ink / sizeof k_font_ink[0]; i++)
        if (fabsf(k_font_ink[i].a_u0 - au0) < 0.0003f) fk = &k_font_ink[i];
    if (slot < 0) { slot = n < 32 ? n++ : 0; seen[slot].f = f; seen[slot].s = -1.0f; }
    if (seen[slot].fk != fk) { seen[slot].fk = fk; seen[slot].s = -1.0f; }
    if (!fk || seen[slot].s == s) return;
    seen[slot].s = s;
    const int italic = MEM32(self + 0x2A0u) == 0x0700000Cu;
    uint8_t *M = (uint8_t *)xbox_GetMemoryOffset();
    for (int c = 0x21; c < 0x7f; c++) {
        uint32_t a = page + (uint32_t)c * 4u, o = MEM32(a);
        unsigned i1 = fk->ink[c - 0x21][1];
        if (!o || !i1) continue;
        uint16_t w;
        memcpy(&w, M + a + o + 0x10u, 2);
        if (!w) continue;
        /* the next letter's ink starts one source pixel into its padded cell */
        int dw = (int)((float)w * s);    /* drawn width, as RenderFont truncates it */
        float right = (float)i1 / 16.0f * (float)dw / (float)w;
        float want = (k_gap - 1.0f) * s;
        int best = 0;
        float be = 1e9f;
        for (int k = 0; k < w; k++) {
            float e = fabsf((float)(int)((float)(w - k) * s) - right - want);
            if (e < be - 0.001f) { be = e; best = k; }
        }
        if ((c >= '0' && c <= '9') || italic) best = 1;   /* digits and the italic title font keep the game's
                                         own advance (the padded cell less its blank column): the HUD's counters
                                         and the pause menu's tab bar are laid out for it */
        uint16_t k16 = (uint16_t)best;
        memcpy(M + a + o + 0x14u, &k16, 2);
    }
}
/* Height of the window's current font (its 'A'), 0 when unknown. */
static int fe_font_height(uint32_t self)
{
    uint32_t f = MEM32(self + 0xA8u);
    if (!f) return 0;
    uint32_t pg = f + 4u, po = MEM32(pg);
    if (!po) return 0;
    uint32_t ca = pg + po + 'A' * 4u, co = MEM32(ca);
    if (!co) return 0;
    return (int)(MEM32(ca + co + 0x10u) >> 16);
}
static int text_box_fit(uint32_t self, uint32_t tp, uint32_t rp, float scale_arg, int32_t *pox, int32_t *pow);
/* A one-line button prompt ("~V Navigate") wider than its box: the PC keycaps and words are
 * longer than the Xbox icons and words the footers were laid out for, and the widget clips the
 * print to its own box, so growing the box does not help (the centred text only moves over).
 * Shrink the text to fit instead, by at most a quarter. Button icons draw 1.5 lines high. */
static float prompt_fit_scale(uint32_t self, uint32_t tp, uint32_t rp, float scale)
{
    uint8_t *M = (uint8_t *)xbox_GetMemoryOffset();
    const char *t;
    float total = 0.0f, chars = 0.0f, gmax = 0.0f;
    uint32_t f = MEM32(self + 0xA8u), page = 0;
    int fh = fe_font_height(self), i;
    int32_t ow, oh;
    if (!tp || !rp || tp >= 0x84000000u || rp >= 0x84000000u || !(scale > 0.0f && scale < 16.0f)) return scale;
    t = (const char *)(M + tp);
    if (!strchr(t, '~') || strchr(t, '\n') || strstr(t, "\\n")) return scale;
    if (fh <= 0) fh = 32;
    ow = (int32_t)MEM32(rp + 8u); oh = (int32_t)MEM32(rp + 12u);
    if (ow <= 0 || oh <= 0 || oh >= 2 * (int32_t)((float)fh * scale)) return scale;
    if (f) { uint32_t po = MEM32(f + 4u); if (po) page = f + 4u + po; }
    for (i = 0; i < 56; i++) { float g = rdf(M, 0x00398818u + (uint32_t)i * 16u + 8u); if (g > gmax && g < 8.0f) gmax = g; }
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if (c == '~' && t[1]) {
            int idx = (unsigned char)t[1] - '#';
            float sc = gmax;
            if (idx >= 0 && idx < 66) {
                uint32_t b = 0x00398B98u + (uint32_t)idx * 12u, g = MEM32(b + 4u);
                if (!MEM32(b + 8u) && g >= 0x00398818u && g < 0x00398818u + 56u * 16u) sc = rdf(M, g + 8u);
            }
            total += sc;
            t++;
            continue;
        }
        if (page && MEM32(page + c * 4u)) {
            uint32_t e = page + c * 4u + MEM32(page + c * 4u);
            uint16_t w, k;
            memcpy(&w, M + e + 0x10u, 2); memcpy(&k, M + e + 0x14u, 2);
            chars += (float)(w - k);
        }
    }
    {
        float whole = (chars + total * (float)fh * 1.5f) * scale + 4.0f, k;
        if (whole <= (float)ow || whole > (float)ow * 3.0f) return scale;
        k = ((float)ow - 2.0f) / whole;
        if (k < 0.75f) k = 0.75f;
        return scale * k;
    }
}

static void fe_fontprint_rect_0010FF80(void)
{
    uint32_t self = g_ecx, font = MEM32(self + 0x2A0u);
    fe_font_dump(self);
    {   /* clickable button prompts (src/femouse.c) */
        extern void femouse_prompt(uint32_t win, int x, int y, int w, int h, const char *t);
        uint32_t tp = MEM32(g_esp + 4u), r = MEM32(g_esp + 8u);
        if (tp && r) {
            const char *t = (const char *)((uint8_t *)xbox_GetMemoryOffset() + tp);
            if (strchr(t, '~'))
                femouse_prompt(self, (int)MEM32(self + 0x18u) + (int)MEM32(r), (int)MEM32(self + 0x1Cu) + (int)MEM32(r + 4u),
                               (int)MEM32(r + 8u), (int)MEM32(r + 12u), t);
        }
    }
    if (getenv("CJ_FE_PROMPT_LOG")) {   /* dev: text with button icons, its rect and window origin */
        uint32_t tp = MEM32(g_esp + 4u), r = MEM32(g_esp + 8u);
        const char *t = tp ? (const char *)((uint8_t *)xbox_GetMemoryOffset() + tp) : "";
        if (tp && strchr(t, '~') && r) {
            static unsigned last; static char seen[64][96]; static int ns;
            int dup = 0;
            for (int i = 0; i < ns; i++) if (!strncmp(seen[i], t, 95)) dup = 1;
            if (!dup && ns < 64) { strncpy(seen[ns++], t, 95);
                fprintf(stderr, "[PROMPT] win=%08x vt=%08x org=%d,%d rect=%d,%d %dx%d align=%u \"%.80s\"\n", self, MEM32(self),
                        (int)MEM32(self + 0x18u), (int)MEM32(self + 0x1Cu), (int)MEM32(r), (int)MEM32(r + 4u), (int)MEM32(r + 8u),
                        (int)MEM32(r + 12u), MEM32(g_esp + 0x10u), t); }
            (void)last;
        }
    }
    float k = fe_text_scale();
    if (k != 1.0f && font == 0x07000004u && fe_text_window(self)) {
        float sc;
        uint32_t a = g_esp + 0xCu, raw = MEM32(a);
        memcpy(&sc, &raw, 4);
        /* A one-line box the larger text would overflow (list cells, the save slot details) is
         * clipped to its rect, cutting the letters off below: there the text grows only as far
         * as the box allows. */
        uint32_t r = MEM32(g_esp + 8u);
        int fh = fe_font_height(self);
        if (r && fh > 0) {
            int rh = (int)MEM32(r + 0xCu);
            float fit = (float)rh / ((float)fh * sc);
            if (rh > 0 && rh < fh * sc * 2.0f && fit < k) k = fit < 1.0f ? 1.0f : fit;
        }
        sc *= k;
        memcpy(&raw, &sc, 4);
        MEM32(a) = raw;
    }
    {   /* a button prompt too wide for its box: smaller rather than cut off */
        float sc0 = rdf((uint8_t *)xbox_GetMemoryOffset(), g_esp + 0xCu);
        float sc1 = prompt_fit_scale(self, MEM32(g_esp + 4u), MEM32(g_esp + 8u), sc0);
        if (sc1 != sc0) { uint32_t raw; memcpy(&raw, &sc1, 4); MEM32(g_esp + 0xCu) = raw; }
    }
    {
        float fs;
        uint32_t rw = MEM32(g_esp + 0xCu);
        memcpy(&fs, &rw, 4);
        fe_font_kern(self, fs);
    }
    {   /* the draw clips to the box, so a box the text must grow into grows for the whole print */
        uint32_t tp = MEM32(g_esp + 4u), rp = MEM32(g_esp + 8u);
        int32_t ox = 0, ow = 0;
        uint32_t align = MEM32(g_esp + 0x10u), x0 = rp ? MEM32(rp) : 0, w0 = rp ? MEM32(rp + 8u) : 0;
        int widened = text_box_fit(self, tp, rp, rdf((uint8_t *)xbox_GetMemoryOffset(), g_esp + 0xCu), &ox, &ow);
        /* Right-aligned text (CalculateAlignment 6-8) ends exactly on the box's right edge, and the
         * window clips there: the last glyph's right side and its drop shadow were cut off (the
         * chapter statistics' "0/1"). Keep a few units clear of the edge. */
        if (rp && align >= 6u && align <= 8u) {
            int fh = fe_font_height(self);
            float sc = rdf((uint8_t *)xbox_GetMemoryOffset(), g_esp + 0xCu);
            int pad = (int)((float)(fh > 0 ? fh : 16) * (sc > 0.0f && sc < 16.0f ? sc : 1.0f) * 0.15f) + 2;
            MEM32(rp) = MEM32(rp) - (uint32_t)pad;
        }
        recomp_lookup(0x0010FF80u)();
        if (rp && (widened || (align >= 6u && align <= 8u))) { MEM32(rp) = x0; MEM32(rp + 8u) = w0; }
    }
}

/* The layout must measure text at the size FontPrint draws it, or wrapped text runs past its
 * box (dialog bodies over their buttons, labels cut short): XWnd::FontTextSize, the plain
 * (0x0010C970, ret ptr, text, scale) and rect (0x00110180, ret ptr, text, rect, scale) forms,
 * both virtual, get the same factor for the same windows. */
static void fe_scale_arg(uint32_t off)
{
    uint32_t self = g_ecx, font = MEM32(self + 0x2A0u);
    float k = fe_text_scale();
    if (k != 1.0f && font == 0x07000004u && fe_text_window(self)) {
        float sc;
        uint32_t a = g_esp + off, raw = MEM32(a);
        memcpy(&sc, &raw, 4);
        sc *= k;
        memcpy(&raw, &sc, 4);
        MEM32(a) = raw;
    }
    {
        float fs;
        uint32_t rw = MEM32(g_esp + off);
        memcpy(&fs, &rw, 4);
        fe_font_kern(self, fs);
    }
}
/* XWnd::RenderFont(text, const XLineInfo &line, float scale) at 0x0010CB50 sizes a ~X button
 * icon as line.m_Height * scale, but the line height was measured at that scale already, so any
 * scale above 1 (our larger menu text) drew the icons scale^2 big, overflowing the line and cut
 * off below it. The line height is only used for icons there, so it is passed unscaled for the
 * call and put back after. */
static void fe_renderfont_0010CB50(void)
{
    uint32_t line = MEM32(g_esp + 8u), raw = MEM32(g_esp + 0xCu);
    float sc;
    memcpy(&sc, &raw, 4);
    fe_font_kern(g_ecx, sc);
    uint16_t h0 = 0;
    int fix = line && sc > 1.001f && sc < 8.0f;
    if (fix) {
        memcpy(&h0, (uint8_t *)xbox_GetMemoryOffset() + line + 0xA, 2);
        uint16_t h1 = (uint16_t)((float)h0 / sc + 0.5f);
        memcpy((uint8_t *)xbox_GetMemoryOffset() + line + 0xA, &h1, 2);
    }
    recomp_lookup(0x0010CB50u)();
    if (fix) memcpy((uint8_t *)xbox_GetMemoryOffset() + line + 0xA, &h0, 2);
}
static void fe_textsize_0010C970(void) { fe_scale_arg(0xCu); recomp_lookup(0x0010C970u)(); }
static void fe_textsize_rect_00110180(void) { fe_scale_arg(0x10u); recomp_lookup(0x00110180u)(); }

/* PC button prompts (src/glyphs.c): EXWnd::_SelectSprite2DTexture(EXTexture *, s8, s8) at
 * 0x0024DDD0 (always via the vtable) sees every sprite texture, the ~X button icons among them;
 * glyph_seen notes where an icon's texels live so the GL back end can draw PC art there. */
static void fe_sel_tex_0024DDD0(void)
{
    extern void glyph_seen(uint32_t tex);
    uint32_t tex = MEM32(g_esp + 4u);
    if (tex) glyph_seen(tex);
    recomp_lookup(0x0024DDD0u)();
}

/* EXDisplay::BeginDraw (0x00225220, thiscall, only reached through the vtable). Whenever
 * EXBaseApp::MaxFrameRate differs from the rate the presentation interval was last set for
 * (m_FrameRate, display+0x988), the original calls D3DDevice::Reset for a new vsync interval.
 * That Reset never frees the old back buffers (the engine still holds references to them), so
 * each one leaks 2.4 MB of the 64 MB contiguous pool; it also clears the device's
 * first-kickoff flag. On the Xbox it ran a few times a session. Here every Frame Rate change on
 * PC Settings, and every level start at 60 fps, triggered one: after three or four the pool
 * ran out, the Reset failed with the back buffers already released, and the game went on
 * drawing frames that were never shown (Windows: "not responding").
 * The port paces frames itself and does not use the vsync interval, so after the first setup
 * the rate is only recorded, never Reset for. */
static void exdisplay_begindraw_00225220(void)
{
    uint32_t self = g_ecx;
    if (self >= 0x10000u && self < 0x84000000u) {
        uint32_t interval = MEM32(self + 0x58u + 0x30u);      /* m_PresentParams.FullScreen_PresentationInterval */
        uint32_t *rate = (uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + self + 0x988u);
        uint32_t want = MEM32(0x0039A030u);                   /* EXBaseApp::m_MaxFrameRate */
        if (*rate && interval != 0x80000000u && *rate != want && want >= 15u && want <= 60u) {
            static unsigned n;
            if (n++ < 4) fprintf(stderr, "[FPS] frame cap %u -> %u: kept the display's present interval (no D3D Reset)\n", *rate, want);
            *rate = want;
        }
    }
    recomp_lookup(0x00225220u)();
}

/* XItemHandler_Camera::Initialise (0x000C1DC0, thiscall, virtual). The game starts with
 * vertical look inverted, in third and first person (m_bInvertY 0x109, m_bInvertFPY 0x10B
 * set to 1), as console games of its time did; on a PC with a mouse that reads as backwards.
 * New settings start with up looking up. Saved settings still apply afterwards (GameData
 * copies them onto the camera), and Options > Controller can turn inversion back on. */
static void camera_initialise_000C1DC0(void)
{
    uint32_t self = g_ecx;
    recomp_lookup(0x000C1DC0u)();
    if (self >= 0x10000u && self < 0x84000000u) {
        uint8_t *cam = (uint8_t *)xbox_GetMemoryOffset() + self;
        cam[0x109] = 0;   /* m_bInvertY */
        cam[0x10B] = 0;   /* m_bInvertFPY */
    }
}

/* XWnd::GetStringProperties (0x0010E2F0, thiscall: text, const EXRect &, XStringInfo *, align, scale;
 * ret 0x14; only reached through the vtable). Its word wrap never ends when one button icon or
 * one letter is wider than the box: the line it breaks off is empty, it starts again at the same
 * place, and every pass appends an XLineInfo, until the array has run through memory (the HUD's
 * vtable among it) and the game crashes. The Xbox icons all fit; the PC keycaps follow the key's
 * width ("Tab" is 1.6 squares), and a box in the weapon selector fits 1.5. So the box is widened
 * about its centre, for the layout only, to the widest icon or letter in the text. */
/* Fit the box at rp to the text at tp (see below); returns 1 when it widened it (*ox, *ow: the
 * original x and width, to put back). */
static int text_box_fit(uint32_t self, uint32_t tp, uint32_t rp, float scale_arg, int32_t *pox, int32_t *pow)
{
    uint8_t *M = (uint8_t *)xbox_GetMemoryOffset();
    int32_t ox = 0, ow = 0, widened = 0;
    if (tp && rp && tp < 0x84000000u && rp < 0x84000000u) {
        float scale = scale_arg, maxsc = 0.0f, maxch = 0.0f, gmax = 0.0f, total = 0.0f, chars = 0.0f;
        int newline = strchr((const char *)(M + tp), '\n') || strstr((const char *)(M + tp), "\\n");
        uint32_t f = MEM32(self + 0xA8u), page = 0;
        int fh = fe_font_height(self), i;
        if (f) { uint32_t po = MEM32(f + 4u); if (po) page = f + 4u + po; }
        if (!(scale > 0.0f && scale < 16.0f)) scale = 1.0f;
        for (i = 0; i < 56; i++) { float g = rdf(M, 0x00398818u + (uint32_t)i * 16u + 8u); if (g > gmax && g < 8.0f) gmax = g; }
        for (const char *t = (const char *)(M + tp); *t; t++) {
            unsigned char c = (unsigned char)*t;
            if (c == '~' && t[1]) {
                int idx = (unsigned char)t[1] - '#';
                float sc = gmax;   /* an action's button is re-resolved inside the layout: allow the widest */
                if (idx >= 0 && idx < 66) {
                    uint32_t b = 0x00398B98u + (uint32_t)idx * 12u, g = MEM32(b + 4u);
                    if (!MEM32(b + 8u) && g >= 0x00398818u && g < 0x00398818u + 56u * 16u) sc = rdf(M, g + 8u);
                }
                if (sc > maxsc) maxsc = sc;
                total += sc;            /* button widths in font heights; scaled below */
                t++;
                continue;
            }
            if (page && MEM32(page + c * 4u)) {
                uint32_t e = page + c * 4u + MEM32(page + c * 4u);
                uint16_t w, k;
                memcpy(&w, M + e + 0x10u, 2); memcpy(&k, M + e + 0x14u, 2);
                if ((float)(w - k) * scale > maxch) maxch = (float)(w - k) * scale;
                chars += (float)(w - k) * scale;
            }
        }
        if (fh <= 0) fh = 32;
        {
            int32_t need = (int32_t)((float)fh * 1.5f * maxsc * scale) + 2;
            if ((int32_t)maxch + 2 > need) need = (int32_t)maxch + 2;
            ox = (int32_t)MEM32(rp); ow = (int32_t)MEM32(rp + 8u);
            if ((maxsc > 0.0f || maxch > 0.0f) && ow < need) {
                static unsigned n;
                if (n++ < 8) fprintf(stderr, "[TEXT] box %d wide for \"%.40s\": widened to %d for the layout (the wrap would never end)\n",
                                     ow, (const char *)(M + tp), need);
                MEM32(rp) = (uint32_t)(ox - (need - ow) / 2);
                MEM32(rp + 8u) = (uint32_t)need;
                widened = 1;
            } else if (!newline && maxsc > 0.0f) {
                /* A one-line prompt box (room for one line only) too narrow for its whole text: the
                 * PC words and keycaps are longer than the Xbox ones ("~V Navigate" in a box made for
                 * "~V Scroll"), and the wrapped rest would be cut off. Grow it to the left: the
                 * footers line up on their right edge. */
                int32_t oh = (int32_t)MEM32(rp + 12u), line = (int32_t)((float)fh * scale);
                int32_t whole = (int32_t)(chars + total * (float)fh * scale) + 4;
                if (oh > 0 && oh < 2 * line && whole > ow && whole < ow * 3) {
                    static unsigned n2;
                    /* The footer sizes the box to the measured text, and the layout keeps one line
                     * only if the text is narrower than the box, so it is the last few units that
                     * wrap the word away. Grow to the right, a little: the text keeps its place
                     * (the footer clips anything moved left of its box). */
                    int32_t grow = whole - ow < 16 ? whole - ow : 16;
                    if (n2++ < 8) fprintf(stderr, "[TEXT] prompt box %d wide for \"%.40s\": +%d to the right\n", ow, (const char *)(M + tp), grow);
                    MEM32(rp + 8u) = (uint32_t)(ow + grow);
                    widened = 1;
                }
            }
        }
    }
    *pox = ox; *pow = ow;
    return widened;
}

static void xwnd_string_props_0010E2F0(void)
{
    uint32_t self = g_ecx, tp = MEM32(g_esp + 4u), rp = MEM32(g_esp + 8u);
    uint8_t *M = (uint8_t *)xbox_GetMemoryOffset();
    int32_t ox = 0, ow = 0;
    int widened = text_box_fit(self, tp, rp, rdf(M, g_esp + 0x14u), &ox, &ow);
    if (getenv("CJ_FE_PROMPT_LOG") && tp && rp && strstr((const char *)(M + tp), "Navigate")) {
        static unsigned n;
        if (n++ < 6) fprintf(stderr, "[TEXT] layout \"%s\" in box %d,%d %dx%d align %u\n", (const char *)(M + tp),
                             (int)MEM32(rp), (int)MEM32(rp + 4u), (int)MEM32(rp + 8u), (int)MEM32(rp + 12u), MEM32(g_esp + 0x10u));
    }
    recomp_lookup(0x0010E2F0u)();
    if (widened) { MEM32(rp) = (uint32_t)ox; MEM32(rp + 8u) = (uint32_t)ow; }
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x0010E2F0u) return xwnd_string_props_0010E2F0;
    if (xbox_va == 0x00225220u) return exdisplay_begindraw_00225220;
    if (xbox_va == 0x000C1DC0u) return camera_initialise_000C1DC0;
    if (xbox_va == 0x0010FF80u) return fe_fontprint_rect_0010FF80;
    if (xbox_va == 0x0024DDD0u) return fe_sel_tex_0024DDD0;
    if (xbox_va == 0x0010C970u) return fe_textsize_0010C970;
    if (xbox_va == 0x0010CB50u) return fe_renderfont_0010CB50;
    if (xbox_va == 0x00110180u) return fe_textsize_rect_00110180;
    recomp_func_t det = cj_det_lookup(xbox_va);
    if (det) return det;
    {   /* quick save / load safe point (src/quicksave.c) */
        extern recomp_func_t qs_lookup(uint32_t va);
        recomp_func_t q = qs_lookup(xbox_va);
        if (q) return q;
    }
    if (xbox_va == 0x002DCF60) return crt_fmod_zero_002DCF60;
    if (xbox_va == 0x002DD002) return crt_fmod_nan_002DD002;
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);
    {   /* who made the call: the return address on top of the guest stack, the registers and the
         * stack under it (the null pointer's owner is usually ecx, a thiscall's object) */
        int k;
        fprintf(stderr, "  caller: return to 0x%08X; ecx=%08X esi=%08X edi=%08X ebx=%08X ebp=%08X esp=%08X; [ecx]=%08X\n  stack:",
                MEM32(g_esp), g_ecx, g_esi, g_edi, g_ebx, g_ebp, g_esp,
                (g_ecx >= 0x10000u && g_ecx < 0x84000000u) ? MEM32(g_ecx) : 0u);
        for (k = 1; k < 16; k++) fprintf(stderr, " %08X", MEM32(g_esp + 4u * k));
        fprintf(stderr, "\n");
    }

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    if (hits[i] <= 10) {   /* who: the return address on the guest stack, the object, the stack */
        int k;
        fprintf(stderr, "  caller: return to 0x%08X; ecx=%08X [ecx]=%08X esi=%08X edi=%08X ebx=%08X ebp=%08X esp=%08X\n  stack:",
                MEM32(g_esp), g_ecx, (g_ecx >= 0x10000u && g_ecx < 0x84000000u) ? MEM32(g_ecx) : 0u,
                g_esi, g_edi, g_ebx, g_ebp, g_esp);
        for (k = 1; k < 24; k++) fprintf(stderr, " %08X", MEM32(g_esp + 4u * k));
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ======================================================================
 * Predator: Concrete Jungle graphics bridges (addresses from the retail
 * linker map Predator.map). Their generated bodies are suppressed with
 * --manual-functions game/manual_functions.json.
 * ====================================================================== */
#include <time.h>
extern __thread uint32_t g_esp;

/* D3D::BlockOnTime(DWORD time, BOOL flush) -- __stdcall, ret 8.
 *
 * D3D calls this before the CPU reuses memory the GPU may still read
 * (dynamic vertex buffers, textures, push-buffer space). The retail body
 * (0x2E7E50) starts: dev = g_pDevice (0x308218); the time is pending while
 * (dev->time - time) < (dev->time - *dev->pGpuTime) (dev+0x2C, dev+0x30);
 * when time == dev->time nothing has been submitted for it yet, so it kicks
 * off with SetFence(0) (0x2E7D90) first. *pGpuTime is the semaphore word the
 * executor writes when it runs BACK_END_WRITE_SEMAPHORE_RELEASE, so it moves
 * only as commands really execute (the executor runs on its own thread).
 *
 * This used to return at once. That let the title overwrite vertices and
 * textures still referenced by unexecuted commands, so a later pass drew
 * new data for an old draw: unstable geometry and half-updated text.
 * Now: wait for the GPU like the original, yielding, with a timeout so a
 * stalled executor can never freeze the game. CJ_BLOCKONTIME=0 restores the
 * immediate return; CJ_BLOCKONTIME_MS sets the timeout (default 250). */
extern __thread uint32_t g_esp;
extern void m_SetFence_D3D__YGKK_Z_002E7D90(void);
extern void *xbox_GetMemoryOffset(void);
#ifdef _WIN32
#include <windows.h>
#define BOT_YIELD() SwitchToThread()
#else
#include <sched.h>
#define BOT_YIELD() sched_yield()
#endif
static double bot_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
unsigned long g_bot_calls, g_bot_waits, g_bot_kicks, g_bot_timeouts;
double g_bot_wait_ms;
/* The executor writes BACK_END_WRITE_SEMAPHORE_RELEASE values only once the
 * port names the word (nv2a_backend.h); for XDK D3D that is *(device+0x30),
 * the m_pGpuTime D3D reads. Without it the word never moves and every real
 * wait on GPU progress would hang. Named as soon as the device exists (the
 * vblank bridge runs every frame from the first). */
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
static void bot_name_semaphore(void)
{
    static uint32_t named;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t dev = *(volatile uint32_t *)(mem + 0x308218), gp;
    if (!dev) return;
    gp = *(volatile uint32_t *)(mem + dev + 0x30);
    if (!gp || gp == named) return;
    named = gp;
    nv2a_pb_set_semaphore_target(gp);
    fprintf(stderr, "[BRIDGE] GPU time semaphore at %08X (device %08X)\n", gp, dev);
}
void m_BlockOnTime_D3D__YGXKK_Z_002E7E50(void)
{
    static int mode = -1;
    static double limit_ms = 250.0;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t esp0 = g_esp;
    uint32_t want = *(volatile uint32_t *)(mem + esp0 + 4);
    uint32_t dev, cur, gp;
    if (mode < 0) {
        const char *e = getenv("CJ_BLOCKONTIME");
        mode = (e && *e == '0') ? 0 : 1;
        if ((e = getenv("CJ_BLOCKONTIME_MS")) && atof(e) > 0) limit_ms = atof(e);
        fprintf(stderr, "[BRIDGE] D3D::BlockOnTime -> %s\n",
                mode ? "waits for the GPU (CJ_BLOCKONTIME=0: immediate)" : "immediate (CJ_BLOCKONTIME=0)");
    }
    if (++g_bot_calls % 500 == 0)
        fprintf(stderr, "[BRIDGE] BlockOnTime: %lu calls, %lu waited on the GPU (%.1f ms total), %lu kicks, %lu timeouts\n",
                g_bot_calls, g_bot_waits, g_bot_wait_ms, g_bot_kicks, g_bot_timeouts);
    if (!mode) { g_esp += 4 + 8; return; }
    if (*(volatile uint32_t *)(mem + 0x00307D70u)) {      /* CJ_NULLHW (main.c) */
        /* No GPU: its time is D3D's own time. D3D's null KickOff leaves it two
         * fences behind (*m_pGpuTime = m_Time - 2); a wait on those would never
         * end. Also mark the device as past its first kickoff (flag 0x2000 in
         * device+8), so later kickoffs never wait for an idle GPU. */
        extern void cj_gfx_flush(void);
        cj_gfx_flush();                                   /* CJ_NULLHW=2: run what is pending */
        dev = *(volatile uint32_t *)(mem + 0x308218);
        if (dev) {
            gp = *(volatile uint32_t *)(mem + dev + 0x30);
            *(volatile uint32_t *)(mem + dev + 8) |= 0x2000u;
            if (gp) *(volatile uint32_t *)(mem + gp) = *(volatile uint32_t *)(mem + dev + 0x2C);
        }
        g_esp += 4 + 8;
        return;
    }
    bot_name_semaphore();
    dev = *(volatile uint32_t *)(mem + 0x308218);
    if (!dev) { g_esp += 4 + 8; return; }
    cur = *(volatile uint32_t *)(mem + dev + 0x2C);
    gp  = *(volatile uint32_t *)(mem + dev + 0x30);
    if (!gp || (uint32_t)(cur - want) >= (uint32_t)(cur - *(volatile uint32_t *)(mem + gp))) {
        g_esp += 4 + 8;                       /* not pending */
        return;
    }
    if (want == cur) {                        /* not submitted yet: kick off */
        uint32_t sp = esp0;
        sp -= 4; *(volatile uint32_t *)(mem + sp) = 0;            /* SetFence(0) */
        sp -= 4; *(volatile uint32_t *)(mem + sp) = 0x002E7E7Fu;  /* return address */
        g_esp = sp;
        m_SetFence_D3D__YGKK_Z_002E7D90();
        g_esp = esp0;
        g_bot_kicks++;
        cur = *(volatile uint32_t *)(mem + dev + 0x2C);
    }
    {
        double t0 = bot_now_ms(), t;
        g_bot_waits++;
        while ((uint32_t)(cur - want) < (uint32_t)(cur - *(volatile uint32_t *)(mem + gp))) {
            t = bot_now_ms();
            if (t - t0 > limit_ms) {
                if (g_bot_timeouts++ < 8)
                    fprintf(stderr, "[BRIDGE] BlockOnTime: GPU at %u, waited %.0f ms for %u; giving up\n",
                            *(volatile uint32_t *)(mem + gp), t - t0, want);
                break;
            }
            BOT_YIELD();
        }
        g_bot_wait_ms += bot_now_ms() - t0;
    }
    g_esp = esp0 + 4 + 8;
}

/* D3DDevice_BlockUntilVerticalBlank(void) -- __stdcall, ret 0.
 * Paced from the host: sleep to the next 60 Hz boundary. */
void m_D3DDevice_BlockUntilVerticalBlank_002EC4F0(void)
{
    static struct timespec next;
    struct timespec now;
    static unsigned long n;
    const long period = 16666667L;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (next.tv_sec == 0 || now.tv_sec > next.tv_sec + 1) next = now;
    next.tv_nsec += period;
    if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    {
        static int on = -1;
        if (on < 0) { const char *e = getenv("CJ_BLOCKONTIME"); on = !(e && *e == '0'); }
        if (on) bot_name_semaphore();
    }
    if ((n++ % 600) == 0)
        fprintf(stderr, "[BRIDGE] vblank #%lu; BlockOnTime calls %lu, waits %lu (%.1f ms total), kicks %lu, timeouts %lu\n",
                n, g_bot_calls, g_bot_waits, g_bot_wait_ms, g_bot_kicks, g_bot_timeouts);
    g_esp += 4;
}

/* CJ_FPS: frame-rate cap. EngineX runs gameplay at a nominal 60 Hz scaled by
 * EXBaseApp::m_GameFrameRateMul (= 60 / measured rate), and EXApp::MainUpdate
 * waits for refresh / m_MaxFrameRate vblanks per frame, measuring the real
 * rate from the vblank count. XApp::InitGame caps it with
 * SetMaxFrameRate(VideoFrameRate() / 2) = 30 at every level start, so the
 * Xbox game runs at 30 with a multiplier of 2. Raising m_MaxFrameRate lets
 * the engine run at up to that rate with the multiplier tracking it; if the
 * PC cannot keep up the engine measures the lower rate and scales gameplay
 * to it, as it did on the Xbox (m_MinFrameRate = 15).
 * Called once per presented frame (gl_flip). CJ_FPS=60 (max: the 60 Hz
 * vblank the port delivers); unset or 30 keeps the original cap. */
#define CJ_VA_MAX_FRAME_RATE 0x0039A030u   /* EXBaseApp::m_MaxFrameRate */
#define CJ_VA_FRAME_RATE     0x0039A038u   /* EXBaseApp::m_FrameRate (float, measured) */
#define CJ_VA_GAME_RATE_MUL  0x0039A044u   /* EXBaseApp::m_GameFrameRateMul */
/* CJ_CAMERA_STYLE: the game's own camera style byte, GameData::m_Values+0x0b
 * (GameData+0x193; instance pointer at 0x48E3C0). XItemHandler_Camera::DoUpdate
 * runs the third-person auto-rotation (camera dragged by sideways stick after a
 * moment of running straight) only when it is not 1; XItemHandler_Player::
 * GetPadData decouples movement from the camera when it is 2.
 *   0 = the default auto camera, 1 = manual (no auto-rotation), 2 = "track".
 * Unset: the game's value is left alone. Applied every frame. */
#define CJ_VA_GAMEDATA_INSTANCE 0x0048E3C0u
static void cj_camera_style_tick(uint8_t *mem)
{
    static int want = -2;
    uint32_t gd;
    if (want == -2) {
        const char *e = getenv("CJ_CAMERA_STYLE");
        want = (e && *e >= '0' && *e <= '2') ? *e - '0' : -1;
        if (want >= 0) fprintf(stderr, "[CAM] camera style forced to %d (%s)\n", want,
                               want == 1 ? "manual: no auto-rotation" : want == 2 ? "track" : "auto");
    }
    if (want < 0) return;
    gd = *(volatile uint32_t *)(mem + CJ_VA_GAMEDATA_INSTANCE);
    if (gd >= 0x10000u && gd < 0x84000000u && *(volatile uint8_t *)(mem + gd + 0x193) != (uint8_t)want)
        *(volatile uint8_t *)(mem + gd + 0x193) = (uint8_t)want;
}

/* Camera settings (moddable port), applied every frame:
 *   CJ_CAM_DISTANCE=<m>     third-person distance (game default 7.0; global
 *                           CameraDistance, which the camera eases toward)
 *   CJ_CAM_SENS_X=<x>       right-stick turn speed multiplier (1 = original)
 *   CJ_CAM_SENS_Y=<x>       right-stick up/down speed multiplier
 *   CJ_CAM_FP_SENS=<x>      first-person look multiplier (FP_Sensitivity 2.0)
 * The two right-stick rates are constants Camera::DoUpdate reads from .rdata
 * (0x387B90 = 0.039263 rad per frame at full deflection x GameFrameRateMul,
 * 0x387B8C = 0.032719); each has exactly one reader. Mouse look
 * (gl_backend.c, CJ_MOUSE=1) drives the same constants, so it goes through
 * cj_cam_turn_rate(). */
#define CJ_VA_CAMERA_DISTANCE 0x00396D20u
#define CJ_VA_FP_SENSITIVITY  0x00396D24u
#define CJ_VA_CAM_RATE_X      0x00387B90u
#define CJ_VA_CAM_RATE_Y      0x00387B8Cu
static float s_cam_rate0[2], s_cam_sens[2] = {1.0f, 1.0f};
static float s_cam_dist = -1.0f, s_cam_fp = -1.0f;
static volatile float s_cam_mouse[2];      /* mouse override of the rates this frame (0: none) */
void cj_cam_mouse_rates(float rx, float ry) { s_cam_mouse[0] = rx; s_cam_mouse[1] = ry; }
float cj_cam_game_mul(void)
{
    float m = *(volatile float *)((uint8_t *)xbox_GetMemoryOffset() + 0x00396D00u);   /* XApp::m_GameFrameRateMul */
    return (m > 0.1f && m < 8.0f) ? m : 2.0f;
}
float cj_cam_mouse_rad(int axis) { return s_cam_mouse[axis & 1] * cj_cam_game_mul(); }
float cj_cam_base_rate(int axis) { return s_cam_rate0[axis] > 0.0f ? s_cam_rate0[axis] : (axis ? 0.032719f : 0.039263f); }
static void cj_camera_settings_tick(uint8_t *mem)
{
    static int init;
    int i;
    if (!init) {
        const char *e;
        init = 1;
        memcpy(&s_cam_rate0[0], mem + CJ_VA_CAM_RATE_X, 4);
        memcpy(&s_cam_rate0[1], mem + CJ_VA_CAM_RATE_Y, 4);
        if ((e = getenv("CJ_CAM_SENS_X")) && atof(e) > 0) s_cam_sens[0] = (float)atof(e);
        if ((e = getenv("CJ_CAM_SENS_Y")) && atof(e) > 0) s_cam_sens[1] = (float)atof(e);
        if ((e = getenv("CJ_CAM_DISTANCE")) && atof(e) > 0.5) s_cam_dist = (float)atof(e);
        if ((e = getenv("CJ_CAM_FP_SENS")) && atof(e) > 0) s_cam_fp = 2.0f * (float)atof(e);
        if (s_cam_sens[0] != 1.0f || s_cam_sens[1] != 1.0f || s_cam_dist > 0 || s_cam_fp > 0)
            fprintf(stderr, "[CAM] turn x%.2f, up/down x%.2f, distance %s, first-person look %s\n",
                    s_cam_sens[0], s_cam_sens[1], s_cam_dist > 0 ? getenv("CJ_CAM_DISTANCE") : "game",
                    s_cam_fp > 0 ? getenv("CJ_CAM_FP_SENS") : "game");
    }
    for (i = 0; i < 2; i++) {
        float r = s_cam_mouse[i] > 0.0f ? s_cam_mouse[i] : s_cam_rate0[i] * s_cam_sens[i];
        memcpy(mem + (i ? CJ_VA_CAM_RATE_Y : CJ_VA_CAM_RATE_X), &r, 4);
    }
    if (s_cam_dist > 0) memcpy(mem + CJ_VA_CAMERA_DISTANCE, &s_cam_dist, 4);
    if (s_cam_fp > 0) memcpy(mem + CJ_VA_FP_SENSITIVITY, &s_cam_fp, 4);
}

/* Modern control scheme (CJ_MODERN_CONTROLS, default on): the game's control
 * styles are action maps (XGamePad PadRemap0..4: 15 {button bits, action}
 * pairs); every button prompt, the Options controller diagram and the input
 * all read them. The modern scheme is style 1, the game's default (a new game
 * and every save that never changed the style use it), as "1 - Modern":
 *   A jump, X light attack / context (also stealth kills and grabs: hold LT
 *   to lock on, X when the icon shows; grab = X+Y with the blades away),
 *   Y heavy attack, B back flip (action 0x10, the diagram's "Special Move",
 *   relabelled; prompt ~7),
 *   RT ranged attack, LT lock on, RB gadget, LB toggle cloak,
 *   D-pad up vision mode on/off, down 1st person, left cycle vision,
 *   right wristblades, Back weapon select, Start pause, R3 centre camera.
 * The original style 1 ("Predator") moves to slot 5 as "5 - Predator"
 * (replacing "Reaper"); 2 Hunter, 3 Rogue, 4 Warrior are unchanged.
 * (Xbox pad bits from EXGamePad::StorePadState 0x26ED60: A 0x2000, B 0x8000,
 * X 0x4000, Y 0x1000, Black/RB 0x800000, White/LB 0x200000, LT 0x100000,
 * RT 0x400000, Start 0x10000, Back 0x20000, R3 0x80000, D-pad 1/2/4/8.)
 * SetActionMap copies a table into the pad, so a pad already on style 1 or 5
 * gets the new table copied in as well. CJ_MODERN_CONTROLS=0: the original
 * five styles. */
#define CJ_VA_PADREMAP4 0x00398EB0u
#define CJ_VA_PADREMAP0 0x00399090u
#define CJ_VA_GAMEPAD   0x004931ECu   /* EXBaseGamePad::m_pGamePad */
static void cj_modern_controls_tick(uint8_t *mem)
{
    static int on = -1;
    static uint32_t orig0[15][2];
    static const uint32_t map[15][2] = {
        {0x2000, 0x20}, {0x4000, 0x08}, {0x1000, 0x02}, {0x8000, 0x10},
        {0x400000, 0x01}, {0x100000, 0x80}, {0x800000, 0x04}, {0x200000, 0x800},
        {0x10000, 0x100}, {0x20000, 0x4000}, {0x80000, 0x200},
        {0x1, 0x2000}, {0x2, 0x40}, {0x4, 0x1000}, {0x8, 0x400},
    };
    uint32_t pad;
    if (on < 0) {
        const char *e = getenv("CJ_MODERN_CONTROLS");
        extern void cj_text_builtin(uint32_t id, const char *text);
        on = !(e && *e == '0');
        memcpy(orig0, mem + CJ_VA_PADREMAP0, sizeof orig0);    /* still the retail table */
        /* "Scroll" reads "Navigate" on PC, and every one shows the arrow keys (~V; the four-way
         * "~D" keycap read as a drag icon). */
        cj_text_builtin(0x510001F4u, "~L/~R Move  ~V Navigate  ~E Select");
        cj_text_builtin(0x510001F6u, "~L/~R Move  ~V Navigate");
        cj_text_builtin(0x51000202u, "~L/~R Move  ~V Navigate  ~H Change");
        /* the checkbox footer: "Toggle" after the wide Enter keycap runs one letter past its box
         * (the footer centres each prompt in a box sized for the Xbox icon); "Select" fits */
        cj_text_builtin(0x510004D7u, "~E Select");
        cj_text_builtin(0x510002BBu, "~L/~R Move  ~V Navigate  ~E View");
        cj_text_builtin(0x51000473u, "~V Navigate");
        cj_text_builtin(0x51000517u, "~6 Navigate");
        cj_text_builtin(0x51000835u, "~V Navigate");
        cj_text_builtin(0x510008D4u, "~V Navigate  ~F Back");
        if (on) {
            cj_text_builtin(0x510001FEu, "1 - Modern");
            cj_text_builtin(0x510003E9u, "5 - Predator");
            cj_text_builtin(0x510004A8u, "Flip / Ledge Grab");   /* action 0x10, the diagram's "Special Move": flip onto a ledge, off a wall, a hang or a gecko surface (prompt ~7) */
            fprintf(stderr, "[PAD] control style 1 is the modern scheme (default); the original style 1 is now 5 (CJ_MODERN_CONTROLS=0: original styles)\n");
        }
    }
    if (!on) return;
    if (memcmp(mem + CJ_VA_PADREMAP0, map, sizeof map)) memcpy(mem + CJ_VA_PADREMAP0, map, sizeof map);
    if (memcmp(mem + CJ_VA_PADREMAP4, orig0, sizeof orig0)) memcpy(mem + CJ_VA_PADREMAP4, orig0, sizeof orig0);
    pad = *(volatile uint32_t *)(mem + CJ_VA_GAMEPAD);
    if (pad >= 0x10000u && pad < 0x84000000u - 0x1000u) {
        int32_t cfg = *(volatile int32_t *)(mem + pad + 0xCC4);
        const void *want = cfg == 0 ? (const void *)map : cfg == 4 ? (const void *)orig0 : NULL;
        int p, changed = 0;
        for (p = 0; want && p < 4; p++) {
            uint8_t *am = mem + pad + 0x314 + p * 256;
            if (mem[pad + 0xB14 + p] == 15 && memcmp(am, want, sizeof map)) { memcpy(am, want, sizeof map); changed = 1; }
        }
        if (changed) {
            static unsigned n_logs;
            mem[pad + 0xCC8] = 1;                      /* m_bActionsChanged: prompts refresh */
            if (n_logs++ < 4) fprintf(stderr, "[PAD] control style %d: pad action map updated (%s)\n", cfg + 1, cfg ? "original Predator" : "modern");
        }
    }
}

void cj_fps_tick(void)
{
    static int want = -1;
    static unsigned gen;
    static unsigned long n;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    volatile uint32_t *mx = (volatile uint32_t *)(mem + CJ_VA_MAX_FRAME_RATE);
    pcs_tick();     /* Options > PC Settings (pcsettings.c) */
    if (want < 0 || gen != pcs_gen(PCS_FPS)) {
        int was = want;
        gen = pcs_gen(PCS_FPS);
        want = pcs_get(PCS_FPS) == 60 ? 60 : 0;
        fprintf(stderr, "[FPS] frame-rate cap: %s\n", want ? "60 (CJ_FPS)" : "original (30)");
        if (was == 60 && !want && *mx == 60) *mx = 30;      /* switched back on the PC Settings page */
    }
    cj_camera_style_tick(mem);
    cj_camera_settings_tick(mem);
    { extern void cjmod_move_tick(void); cjmod_move_tick(); }   /* CJ_GRAVITY (src/native/hybrid/cjmod_move.c) */
    cj_modern_controls_tick(mem);
    { extern void cj_text_tick(void); cj_text_tick(); }
    if (want && *mx != (uint32_t)want && *mx >= 15 && *mx <= 60)
        *mx = (uint32_t)want;
    {   /* CJ_WIDESCREEN=1 / PC Settings: the title's anamorphic 16:9 mode */
        static unsigned wgen = ~0u;
        int ws = pcs_get(PCS_WIDESCREEN);
        volatile uint8_t *f = (volatile uint8_t *)(mem + 0x004932B0u);
        if (ws && *f != 1) *f = 1;
        if (!ws && wgen != ~0u && wgen != pcs_gen(PCS_WIDESCREEN)) *f = 0;   /* turned off on the page */
        wgen = pcs_gen(PCS_WIDESCREEN);
    }
    { static unsigned vn; if (++vn == 400) fprintf(stderr, "[VIDEO] title widescreen flag (EXBaseRenderEnv::ms_WideScreen) = %u\n", *(volatile uint8_t *)(mem + 0x004932B0u)); }
    if (want && (++n % 300) == 0)
        fprintf(stderr, "[FPS] engine: cap %u, measured %.1f, gameplay multiplier %.3f\n", *mx,
                *(volatile float *)(mem + CJ_VA_FRAME_RATE), *(volatile float *)(mem + CJ_VA_GAME_RATE_MUL));
}
