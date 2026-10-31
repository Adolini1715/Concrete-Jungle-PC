/*
 * Button prompts for PC (the ~X icon codes in the game's text).
 *
 * The game draws each icon as a small sprite texture from the front-end file
 * (XGamePad.cpp GraphicArray, 0x398818: one texture hash per button slot).
 * Every sprite texture the game selects passes EXWnd::_SelectSprite2DTexture,
 * which src/recomp_manual.c hands to glyph_seen(): an icon texture's texel
 * address is noted with its slot. When the GL back end binds a texture at one
 * of those addresses it asks glyph_override() and draws our art instead
 * (src/glyph_art.h, drawn by tools/make_glyphs.py): keyboard keycaps after a
 * key was pressed, modern controller icons after a pad input (A/B/X/Y,
 * LB/RB for White/Black, LT/RT, Menu/View for Start/Back, LS/RS for the
 * sticks; the originals are 16x16 and blur at PC resolutions). Each slot's width in the text
 * (its m_Scale) follows the art, so "Space" or "Enter" get a wider cap.
 *
 * ~E and ~F (the menus' Select and Back) share A's and B's textures, but on a
 * keyboard they are Enter and Esc. While keycaps show, those two slots are
 * pointed at spare front-end textures no Xbox prompt draws (k_spare), so
 * they have addresses of their own.
 *
 *   CJ_GLYPHS=0       original Xbox icons throughout (Options > PC Settings >
 *                     PC Button Prompts; read at startup).
 *   CJ_GLYPHS=pad     always controller icons; CJ_GLYPHS=kb always keycaps.
 *   CJ_GLYPH_DUMP=1   writes each icon texture as it is first drawn to
 *                     textures/glyphs/<slot>_<hash>_<w>x<h>.png.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../xboxrecomp/src/kernel/nv2a_backend.h"
#include "glyph_art.h"
#include "pcsettings.h"
#include "keymap.h"

extern void *xbox_GetMemoryOffset(void);
extern int stbi_write_png(const char *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

#define GRAPHIC_ARRAY 0x00398818u   /* XButtonGraphic[56] (0x10 each): texture hash, text hash, float scale, ... */
#define NGRAPHIC 56
#define SLOT(i) (GRAPHIC_ARRAY + (uint32_t)(i) * 16u)

typedef struct { uint32_t hash0; float scale0; int art_kb, art_pad, init; } Slot;
static Slot s_slot[NGRAPHIC];
/* Icon textures found so far. A level load can reload the front-end file at new
 * addresses and reuse the old ones for other textures, so each entry keeps its
 * EXTexture and is checked against it before use (seen_ok); stale ones are reused. */
typedef struct { uint32_t off, hash, tex; } Seen;
static Seen s_seen[256];
static int s_nseen;
static int s_mode = -1;        /* -1 not read yet, 0 off, 1 follow input, 2 pad, 3 kb */
static int s_kb;               /* the last input was the keyboard or mouse */
static int s_dump;

/* Which art each slot gets (keyboard, controller); NULL keeps the original. ~E and ~F
 * share A's and B's textures on a controller, so they draw pad_a and pad_b there. */
static const struct { int idx; const char *kb, *pad; } k_map[] = {
    {1, "kb_q", "pad_y"},          /* ~Y Y */
    {2, "kb_space", "pad_a"},      /* ~A A */
    {3, "kb_r", "pad_x"},          /* ~X X */
    {4, "kb_e", "pad_b"},          /* ~B B */
    {7, "kb_shift", "pad_lt"},      /* ~L left trigger */
    {8, "kb_c", "pad_lb"},      /* ~S White (left shoulder on a modern pad) */
    {9, "kb_f", "pad_rt"},          /* ~R right trigger */
    {10, "kb_z", "pad_rb"},     /* ~T Black (right shoulder) */
    {13, "kb_arrows", "pad_dpad"},    /* ~D d-pad */
    {14, "kb_updown", "pad_dpad_ud"},    /* ~V d-pad up/down (Scroll) */
    {15, "kb_leftright", "pad_dpad_lr"}, /* ~H d-pad left/right (Change) */
    {16, "kb_up", "pad_up"},        /* ~= */
    {17, "kb_down", "pad_down"},      /* ~> */
    {18, "kb_left", "pad_left"},      /* ~? */
    {19, "kb_right", "pad_right"},     /* ~@ */
    {22, "kb_enter", NULL},     /* ~E Select (A's art) */
    {23, "kb_esc", NULL},       /* ~F Back (B's art) */
    {11, "kb_enter", "pad_menu"},   /* ~J Start (the word START on Xbox) */
    {12, "kb_esc", "pad_view"},     /* ~K Back (the word BACK) */
    {24, "kb_wasd", "pad_ls"},      /* ~M left thumbstick (words) */
    {25, "kb_mouse", "pad_rs"},     /* ~O right thumbstick (words) */
};
/* Slots given a spare front-end texture (the PlayStation 2 and unused Xbox art
 * the Xbox build carries but never draws; textures/glyphs dump). kb_only: only
 * while keycaps show (~E/~F: A's and B's own art on a controller); otherwise
 * for good, and a slot that was text (START, BACK, left thumbstick) becomes an
 * icon (its text hash cleared), which the game's text decoder then leaves as a
 * ~X code for the renderer. */
static const struct { int idx; uint32_t spare; int kb_only; } k_spare[] = {
    {22, 0x06000096u, 1},       /* PS2 cross */
    {23, 0x06000098u, 1},       /* PS2 square */
    {11, 0x060001B2u, 0},       /* Xbox START button art (unused) */
    {12, 0x060001B1u, 0},       /* Xbox BACK button art (unused) */
    {24, 0x0600009Bu, 0},       /* PS2 L1 */
    {25, 0x0600009Du, 0},       /* PS2 R1 */
};

static uint8_t *M(void) { return (uint8_t *)xbox_GetMemoryOffset(); }
static uint32_t g32(uint32_t va) { uint32_t v; memcpy(&v, M() + va, 4); return v; }   /* flat 4 GB view */
static void p32(uint32_t va, uint32_t v) { memcpy(M() + va, &v, 4); }

static int find_art(const char *name)
{
    int i;
    for (i = 0; i < GLYPH_ART_N; i++) if (!strcmp(glyph_art[i].name, name)) return i;
    return -1;
}

static void init(void)
{
    const char *e = getenv("CJ_GLYPHS");
    int i, n;
    if (e && !strcmp(e, "pad")) s_mode = 2;
    else if (e && !strcmp(e, "kb")) s_mode = 3;
    else s_mode = pcs_get(PCS_GLYPHS) ? 1 : 0;   /* Options > PC Settings, pcj.ini CJ_GLYPHS */
    s_dump = (e = getenv("CJ_GLYPH_DUMP")) && *e == '1';
    for (i = 0; i < NGRAPHIC; i++) {
        s_slot[i].art_kb = s_slot[i].art_pad = -1;
        for (n = 0; n < (int)(sizeof k_map / sizeof k_map[0]); n++)
            if (k_map[n].idx == i) {
                if (k_map[n].kb) s_slot[i].art_kb = find_art(k_map[n].kb);
                if (k_map[n].pad) s_slot[i].art_pad = find_art(k_map[n].pad);
            }
    }
}

/* The keys that have prompt art, for src/keymap.c. */
int glyph_key_count(void) { return GLYPH_KEYS_N; }
int glyph_key_code(int i) { return (i >= 0 && i < GLYPH_KEYS_N) ? glyph_keys[i].code : -1; }
const char *glyph_key_art(int i) { return (i >= 0 && i < GLYPH_KEYS_N) ? glyph_keys[i].art : NULL; }
const char *glyph_key_label(int i) { return (i >= 0 && i < GLYPH_KEYS_N) ? glyph_keys[i].label : NULL; }

/* Slots whose keyboard art follows the bindings (src/keymap.c). ~E/~F (menu Select/Back), the
 * d-pad pairs, WASD and the mouse stay as drawn. */
static const struct { int idx, km; } k_bound[] = {
    {1, KM_Y}, {2, KM_A}, {3, KM_X}, {4, KM_B}, {7, KM_LT}, {8, KM_WHITE}, {9, KM_RT}, {10, KM_BLACK},
    {11, KM_START}, {12, KM_BACK}, {16, KM_UP}, {17, KM_DOWN}, {18, KM_LEFT}, {19, KM_RIGHT}};
static void bound_art(void)
{
    static unsigned gen;
    size_t n;
    if (gen == keymap_gen()) return;
    gen = keymap_gen();
    for (n = 0; n < sizeof k_bound / sizeof k_bound[0]; n++) {
        const char *a = keymap_art(k_bound[n].km);
        int i = a ? find_art(a) : -1;
        if (i >= 0) s_slot[k_bound[n].idx].art_kb = i;
    }
}

static int kb_now(void) { return s_mode == 3 || (s_mode == 1 && s_kb); }
/* 1 while keyboard prompts show (the pause Controller page then lists the keys). */
int glyph_kb_mode(void) { if (s_mode < 0) init(); return s_mode != 0 && kb_now(); }

static void dump_glyph(uint32_t hash, uint32_t fmtw, uint32_t data)
{
    Nv2aTexture t;
    uint32_t *px;
    char path[300];
    int i;
    if (((fmtw >> 20) & 0xF) > 6 || ((fmtw >> 24) & 0xF) > 6) return;   /* icons are small */
    for (i = 0; i < NGRAPHIC && g32(SLOT(i)) != hash; i++) ;
    memset(&t, 0, sizeof t);
    t.offset = data | 0x80000000u;              /* the contiguous mapping, as the GPU sees it */
    t.color = (fmtw >> 8) & 0xFF;
    t.width = 1u << ((fmtw >> 20) & 0xF);
    t.height = 1u << ((fmtw >> 24) & 0xF);
    t.levels = 1;
    t.pitch = t.width * 4;
    if (!(px = malloc((size_t)t.width * t.height * 4))) return;
    if (nv2a_backend_decode_texture(&t, px)) {
        uint8_t *rgba = (uint8_t *)px;
        size_t k;
        for (k = 0; k < (size_t)t.width * t.height; k++) {   /* ARGB words to RGBA bytes */
            uint32_t c = px[k];
            rgba[k * 4 + 0] = (uint8_t)(c >> 16); rgba[k * 4 + 1] = (uint8_t)(c >> 8);
            rgba[k * 4 + 2] = (uint8_t)c;         rgba[k * 4 + 3] = (uint8_t)(c >> 24);
        }
        snprintf(path, sizeof path, "textures/glyphs/%02d_%08x_%ux%u_%02X.png", i < NGRAPHIC ? i : 99, hash, t.width, t.height, t.color);
        if (!stbi_write_png(path, (int)t.width, (int)t.height, 4, rgba, (int)t.width * 4))
            fprintf(stderr, "[GLYPH] cannot write %s (make the folder)\n", path);
    }
    free(px);
}

static int slot_of(uint32_t hash)
{
    int i, k;
    for (i = 0; i < NGRAPHIC; i++) if (g32(SLOT(i)) == hash) return i;
    for (k = 0; k < (int)(sizeof k_spare / sizeof k_spare[0]); k++)
        if (k_spare[k].spare && k_spare[k].spare == hash) return k_spare[k].idx;
    return -1;
}

/* The entry still describes a loaded icon: its EXTexture has the same hash and texels. */
static int seen_ok(const Seen *e)
{
    uint32_t d3d;
    if (!e->tex || g32(e->tex + 0x18) != e->hash || !(d3d = g32(e->tex + 0x4C))) return 0;
    return (g32(d3d + 4) & 0x7FFFFFFFu) == e->off;
}

/* Notes where texture `tex` keeps its texels if it is a glyph (or a spare). */
static void note(uint32_t tex, int dump_any)
{
    uint32_t hash = g32(tex + 0x18), d3d, data, off;   /* EXBaseTexture (at +0xC) m_HashCode */
    int i, slot;
    if ((hash >> 24) != 6) return;
    slot = slot_of(hash);
    if (slot < 0 && !dump_any) return;
    if (!(d3d = g32(tex + 0x4C))) return;        /* EXTexture::m_pD3DTexture */
    data = g32(d3d + 4);                         /* D3DResource::Data */
    off = data & 0x7FFFFFFFu;
    if (s_dump && dump_any) dump_glyph(hash, g32(d3d + 0xC), data);
    if (slot < 0) return;
    for (i = 0; i < s_nseen; i++)
        if (s_seen[i].tex == tex || s_seen[i].off == off) break;
    if (i == s_nseen)                            /* a free entry: one gone stale, else a new one */
        for (i = 0; i < s_nseen && seen_ok(&s_seen[i]); i++) ;
    if (i == s_nseen) {
        if (s_nseen == (int)(sizeof s_seen / sizeof s_seen[0])) return;
        s_nseen++;
    }
    s_seen[i].off = off;
    s_seen[i].hash = hash;
    s_seen[i].tex = tex;
}

/* EXTexture `tex` (guest address) is about to be selected for 2D sprites. The
 * first time a glyph shows up, its neighbours in the file's texture array
 * (EXTexture is 0x50 bytes, same vtable) are noted too, so every icon is known
 * before it is first drawn. */
void glyph_seen(uint32_t tex)
{
    static uint32_t scanned;
    uint32_t hash;
    if (s_mode < 0) init();
    if (!s_mode && !s_dump) return;
    hash = g32(tex + 0x18);
    if ((hash >> 24) != 6 || slot_of(hash) < 0) return;
    if (scanned != tex - (tex % 0x50u) && scanned != tex) {
        uint32_t vt = g32(tex), a, lo = tex > 64u * 0x50u ? tex - 64u * 0x50u : tex;
        scanned = tex;
        for (a = lo; a < tex + 64u * 0x50u; a += 0x50u)
            if (g32(a) == vt) note(a, s_dump);
    }
    note(tex, 0);
}

/* Two front-end prompts are string literals in the executable that name the
 * pad buttons themselves: "~B Back" (the menus' Back) and "~V Scroll  ~A
 * Select" (the title flow). Point them at ~F and ~E, the menus' Back and
 * Select, which show Esc and Enter on a keyboard (B and A on a pad, as before). */
static void patch_literals(void)
{
    static const struct { const char *from; int at; char to; } k[] = {
        {"~B Back", 1, 'F'},
        {"~V Scroll  ~A Select", 12, 'E'},
    };
    uint8_t *m = M();
    unsigned i;
    uint32_t a;
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        size_t n = strlen(k[i].from) + 1;
        for (a = 0x00300000u; a < 0x00400000u - n; a++)
            if (m[a] == '~' && !memcmp(m + a, k[i].from, n) && m[a - 1] == 0) {
                m[a + (uint32_t)k[i].at] = (uint8_t)k[i].to;
                /* "Navigate" for "Scroll": the literal has three bytes of padding after it, room for 23 */
                if (i == 1 && !m[a + n] && !m[a + n + 1] && !m[a + n + 2]) memcpy(m + a, "~V Navigate  ~E Select", 23);
                break;
            }
    }
}

/* The PC prompts are on (~J and ~K are icons, not the words START and BACK). */
int glyph_icons_on(void)
{
    if (s_mode < 0) init();
    return s_mode != 0;
}

/* Keyboard vs controller, from src/gl_backend.c host_input once a frame. Sets
 * each slot's width and the spare textures for the current kind of input. */
void glyph_input(int kb_used, int pad_used)
{
    int i, kb;
    if (s_mode < 0) init();
    if (pad_used) s_kb = 0;
    else if (kb_used) s_kb = 1;
    if (!s_mode) return;
    kb = kb_now();
    if (!g32(SLOT(2))) return;                   /* the game's data isn't there yet */
    {
        static int patched;
        if (!patched) { patched = 1; patch_literals(); }
    }
    bound_art();
    for (i = 0; i < NGRAPHIC; i++) {
        Slot *s = &s_slot[i];
        int a, k;
        float sc;
        uint32_t v, want;
        if (!s->init) {
            s->init = 1;
            s->hash0 = g32(SLOT(i));
            memcpy(&s->scale0, M() + (SLOT(i) + 8), 4);
        }
        want = s->hash0;
        for (k = 0; k < (int)(sizeof k_spare / sizeof k_spare[0]); k++)
            if (k_spare[k].idx == i && (kb || !k_spare[k].kb_only)) {
                want = k_spare[k].spare;
                if (!k_spare[k].kb_only && g32(SLOT(i) + 4)) p32(SLOT(i) + 4, 0);   /* text -> icon */
            }
        if (!want) continue;
        if (g32(SLOT(i)) != want) p32(SLOT(i), want);
        a = kb ? s->art_kb : s->art_pad;
        sc = a >= 0 ? s->scale0 * (float)glyph_art[a].w / (float)glyph_art[a].h : s->scale0;
        memcpy(&v, &sc, 4);
        if (g32(SLOT(i) + 8) != v) p32(SLOT(i) + 8, v);
    }
}

/* The art (index into glyph_art) to draw instead of the guest texture at `off`, or -1. */
int glyph_override(uint32_t off)
{
    int i, j, kb;
    if (s_mode <= 0 || !s_nseen) return -1;
    off &= 0x7FFFFFFFu;
    for (i = 0; i < s_nseen && s_seen[i].off != off; i++) ;
    if (i == s_nseen || !seen_ok(&s_seen[i])) return -1;
    kb = kb_now();
    for (j = 0; j < NGRAPHIC; j++) {             /* the slot that draws this texture now */
        if (!s_slot[j].init || g32(SLOT(j)) != s_seen[i].hash) continue;
        if (kb ? s_slot[j].art_kb >= 0 : s_slot[j].art_pad >= 0) return kb ? s_slot[j].art_kb : s_slot[j].art_pad;
    }
    return -1;
}

const unsigned char *glyph_art_px(int i, int *w, int *h)
{
    if (i < 0 || i >= GLYPH_ART_N) return NULL;
    *w = glyph_art[i].w;
    *h = glyph_art[i].h;
    return glyph_art[i].rgba;
}
