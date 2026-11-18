/*
 * Languages: translated text and the accented letters it needs.
 *
 *   CJ_LANG=<name>    (pcj.ini; the launcher's Language setting writes 1 for pt-br) loads lang/<name>/:
 *     text.txt        <id><TAB><text> lines, UTF-8, like text/replace.txt (src/textpack.c reads
 *                     it; text/replace.txt still wins over it)
 *     fonts/fonts.txt and fonts/<atlas key>.png: the game's fonts with accented letters
 *                     (tools/art/lang_fonts.py)
 *
 * The game's four fonts hold ASCII (and (R), (C)) only. Each font's atlas is swapped for one twice
 * as tall: the top half is the Sharp UI atlas, the same glyphs where they always were, the bottom
 * half the accented letters. When a font is first used (src/recomp_manual.c fe_font_kern) every
 * old glyph's v is halved and the empty entries of its glyph table (character codes 0xC0-0xFF,
 * ...) are pointed at new glyph records in memory the port owns, whose UVs fall in the bottom half
 * and whose size is the base letter's. The game then lays out and draws them like any other letter.
 * A font reloaded at a new address comes back without them and is extended again.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern void *xbox_GetMemoryOffset(void);
extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
extern unsigned char *cj_png_load(const char *path, int *w, int *h);

#define MAXCH 64
typedef struct { uint8_t c, base; float u0, v0, u1, v1; uint16_t w, h; } LChar;
typedef struct {
    uint64_t key;
    float a_u0;
    int W, H, ay, n;   /* ay: the 'a' cell's top row in the original atlas */
    LChar ch[MAXCH];
    uint32_t block;           /* guest memory: n glyph records (0x18 each) */
    uint8_t *rgba;            /* the tall atlas */
    int rw, rh, tried;
} LFont;
static LFont s_font[8];
static int s_nfont, s_init;
static char s_lang[64];

const char *lang_name(void)
{
    if (!s_init) {
        const char *e = getenv("CJ_LANG");
        s_init = 1;
        if (e && !strcmp(e, "1")) e = "pt-br";   /* the launcher's Language setting: 0 English, 1 Portuguese */
        if (e && *e && strcmp(e, "en") && strlen(e) < sizeof s_lang && !strpbrk(e, "/\\.:")) {
            FILE *f;
            char path[200];
            snprintf(s_lang, sizeof s_lang, "%s", e);
            snprintf(path, sizeof path, "lang/%s/fonts/fonts.txt", s_lang);
            if ((f = fopen(path, "r"))) {
                char line[256];
                LFont *cur = NULL;
                while (fgets(line, sizeof line, f)) {
                    unsigned long long k;
                    float a;
                    int W, H, c, b, w, h;
                    float u0, v0, u1, v1;
                    int ay;
                    if (sscanf(line, "font %llx %f %d %d %d", &k, &a, &W, &H, &ay) == 5) {
                        cur = s_nfont < 8 ? &s_font[s_nfont++] : NULL;
                        if (cur) { memset(cur, 0, sizeof *cur); cur->key = k; cur->a_u0 = a; cur->W = W; cur->H = H; cur->ay = ay; }
                    } else if (cur && cur->n < MAXCH &&
                               sscanf(line, "%d %d %f %f %f %f %d %d", &c, &b, &u0, &v0, &u1, &v1, &w, &h) == 8 &&
                               c >= 0x80 && c < 0x100 && b > 0x20 && b < 0x7F) {
                        LChar *l = &cur->ch[cur->n++];
                        l->c = (uint8_t)c; l->base = (uint8_t)b; l->u0 = u0; l->v0 = v0; l->u1 = u1; l->v1 = v1;
                        l->w = (uint16_t)w; l->h = (uint16_t)h;
                    }
                }
                fclose(f);
            }
            fprintf(stderr, "[LANG] %s: %d fonts with accented letters\n", s_lang, s_nfont);
        }
    }
    return s_lang[0] ? s_lang : NULL;
}

int langfont_active(void) { return lang_name() && s_nfont > 0; }

/* The tall atlas for texture `key`, or NULL. */
const uint8_t *langfont_atlas(uint64_t key, int *w, int *h)
{
    int i;
    if (!langfont_active()) return NULL;
    for (i = 0; i < s_nfont; i++) {
        LFont *f = &s_font[i];
        if (f->key != key) continue;
        if (!f->tried) {
            char path[200];
            f->tried = 1;
            snprintf(path, sizeof path, "lang/%s/fonts/%016llx.png", s_lang, (unsigned long long)key);
            f->rgba = cj_png_load(path, &f->rw, &f->rh);
            if (!f->rgba) fprintf(stderr, "[LANG] cannot read %s\n", path);
        }
        if (!f->rgba) return NULL;
        *w = f->rw; *h = f->rh;
        return f->rgba;
    }
    return NULL;
}

static uint32_t rd32(uint8_t *M, uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static void wr32(uint8_t *M, uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }

/* 1 if guest address `e` is one of our glyph records (src/recomp_manual.c font_table_ok). */
int langfont_owns(uint32_t e)
{
    int i;
    for (i = 0; i < s_nfont; i++)
        if (s_font[i].block && e >= s_font[i].block && e < s_font[i].block + (uint32_t)s_font[i].n * 0x18u) return 1;
    return 0;
}


/* The game copies a loaded font to new memory now and then. Its glyph table holds relative
 * offsets, so a copy of an extended font has its new letters pointing at nothing. Before the font
 * code reads a table, any of our letters' slots that doesn't land on one of our records is cleared
 * (the fonts have nothing of their own there), and langfont_apply puts them back. */
void langfont_repair(uint32_t page)
{
    uint8_t *M;
    int i, k;
    if (!langfont_active()) return;
    M = (uint8_t *)xbox_GetMemoryOffset();
    for (i = 0; i < s_nfont; i++)
        for (k = 0; k < s_font[i].n; k++) {
            uint32_t a = page + s_font[i].ch[k].c * 4u, o = rd32(M, a);
            if (o && !langfont_owns(a + o)) wr32(M, a, 0);
        }
}

/* Called with a font's glyph page and its original 'a' u0 once the font's UVs are corrected. */
void langfont_apply(uint32_t page, float a_u0)
{
    uint8_t *M;
    LFont *f = NULL;
    int i, c;
    if (!langfont_active()) return;
    for (i = 0; i < s_nfont; i++) if (fabsf(s_font[i].a_u0 - a_u0) < 0.0003f) f = &s_font[i];
    if (!f || !f->n) return;
    M = (uint8_t *)xbox_GetMemoryOffset();
    if (!f->block && !(f->block = xbox_HeapAlloc((uint32_t)f->n * 0x18u, 16))) return;
    {   /* already extended: the first new letter points at our record */
        uint32_t a = page + f->ch[0].c * 4u, o = rd32(M, a);
        if (o && a + o == f->block) goto kern;
    }
    {   /* a copy of an extended font already has its old glyphs in the top half: its 'a' starts at
         * row ay of the tall atlas (twice the height), not of the original */
        uint32_t a = page + 'a' * 4u, o = rd32(M, a);
        float v0;
        if (!o) return;
        memcpy(&v0, M + a + o + 4, 4);
        if (fabsf(v0 * (float)(2 * f->H) - (float)f->ay) < 0.5f) goto add;
        if (fabsf(v0 * (float)f->H - (float)f->ay) > 0.5f) return;   /* neither: leave the font alone */
    }
    for (c = 0; c < 256; c++) {   /* the atlas is twice as tall: old glyphs move to its top half */
        uint32_t a = page + (uint32_t)c * 4u, o = rd32(M, a);
        float v;
        if (!o || langfont_owns(a + o)) continue;
        memcpy(&v, M + a + o + 4, 4); v *= 0.5f; memcpy(M + a + o + 4, &v, 4);
        memcpy(&v, M + a + o + 12, 4); v *= 0.5f; memcpy(M + a + o + 12, &v, 4);
    }
add:
    for (i = 0; i < f->n; i++) {
        LChar *l = &f->ch[i];
        uint32_t e = f->block + (uint32_t)i * 0x18u, a = page + l->c * 4u;
        uint16_t z = 0;
        memcpy(M + e, &l->u0, 4); memcpy(M + e + 4, &l->v0, 4);
        memcpy(M + e + 8, &l->u1, 4); memcpy(M + e + 12, &l->v1, 4);
        memcpy(M + e + 0x10, &l->w, 2); memcpy(M + e + 0x12, &l->h, 2);
        memcpy(M + e + 0x14, &z, 2); memcpy(M + e + 0x16, &z, 2);
        if (!rd32(M, a)) wr32(M, a, e - a);   /* a slot the font fills itself is left alone */
    }
    {
        static int told;
        if (told++ < 8) fprintf(stderr, "[LANG] font %016llx at %08X: %d accented letters added\n",
                                (unsigned long long)f->key, page, f->n);
    }
kern:
    for (i = 0; i < f->n; i++) {   /* each new letter takes its base letter's spacing */
        LChar *l = &f->ch[i];
        uint32_t ba = page + l->base * 4u, bo = rd32(M, ba), e = f->block + (uint32_t)i * 0x18u;
        if (bo) memcpy(M + e + 0x14, M + ba + bo + 0x14, 2);
    }
}
