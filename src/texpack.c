/*
 * Texture packs: dump the textures the game uploads, and replace them with
 * edited (or higher resolution) images.
 *
 *   CJ_TEX_DUMP=1     write every distinct texture the game uses (level 0) to
 *                     textures/dump/<key>_<w>x<h>_<fmt>.png, once each.
 *   textures/         texture packs: any PNG whose name starts with a dumped key,
 *                     in any subfolder (a pack's own folder, replace/, ...) or
 *                     inside a .zip dropped there, is used instead of that
 *                     texture (any size; mipmaps are generated). dump/ and
 *                     glyphs/ are skipped. Packs load in name order and a later
 *                     one wins where two replace the same texture, so a pack
 *                     named zz_... overrides the rest. CJ_TEX_REPLACE=0 ignores
 *                     them all.
 *   CJ_TEX_DIR=<dir>  the textures folder (default: textures in the working
 *                     directory, i.e. next to pcj.exe).
 *
 * The key is a 64-bit hash of the texture's format, size and level-0 bytes as
 * the game stores them, so it does not depend on where in memory the texture
 * sits: the same image gets the same name every run. Render targets (glow,
 * heat vision, shadows, the radar layers) are drawn by the GPU each frame and
 * are not textures here.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif
/* Packs come from the internet: bound what a crafted file can make us allocate or decode. */
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

static int s_init, s_dump, s_repl;
static char s_dir[512];

/* A replacement: a loose PNG (zoff < 0) or a member of a zip (zoff = local header offset). */
typedef struct { uint64_t key; char *path; long zoff; int order; uint8_t *rgba; int w, h, tried; } Repl;
static Repl *s_rp;
static int s_nrp;
static uint64_t *s_dumped;          /* open-addressed set of dumped keys */
static size_t s_dumped_cap, s_dumped_n;
static unsigned s_ndump, s_nrepl_used;

static int dumped_insert(uint64_t k)  /* 1 if newly inserted */
{
    size_t i;
    if (!k) k = 1;
    if (s_dumped_n * 2 >= s_dumped_cap) {
        size_t nc = s_dumped_cap ? s_dumped_cap * 2 : 4096, j;
        uint64_t *n = calloc(nc, 8), *o = s_dumped;
        if (!n) return 0;
        for (j = 0; j < s_dumped_cap; j++)
            if (o[j]) { size_t p = (size_t)(o[j] * 0x9E3779B97F4A7C15ull >> 20) & (nc - 1); while (n[p]) p = (p + 1) & (nc - 1); n[p] = o[j]; }
        free(o); s_dumped = n; s_dumped_cap = nc;
    }
    i = (size_t)(k * 0x9E3779B97F4A7C15ull >> 20) & (s_dumped_cap - 1);
    while (s_dumped[i]) { if (s_dumped[i] == k) return 0; i = (i + 1) & (s_dumped_cap - 1); }
    s_dumped[i] = k; s_dumped_n++;
    return 1;
}

static int repl_cmp(const void *a, const void *b)
{
    const Repl *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->order - y->order;
}

static int s_order;
static int s_npacks;

/* `name` (a file name, no folders) starts with a 16-digit hex key and ends in .png. */
static int key_of(const char *name, uint64_t *k)
{
    size_t l = strlen(name);
    char hex[17], *end;
    if (l < 20 || (strcmp(name + l - 4, ".png") && strcmp(name + l - 4, ".PNG"))) return 0;
    memcpy(hex, name, 16); hex[16] = 0;
    *k = strtoull(hex, &end, 16);
    return !*end;
}

static void add_repl(uint64_t k, const char *path, long zoff)
{
    Repl *n = realloc(s_rp, (size_t)(s_nrp + 1) * sizeof *s_rp);
    if (!n) return;
    s_rp = n;
    memset(&s_rp[s_nrp], 0, sizeof *s_rp);
    s_rp[s_nrp].key = k;
    s_rp[s_nrp].path = strdup(path);
    s_rp[s_nrp].zoff = zoff;
    s_rp[s_nrp].order = s_order++;
    s_nrp++;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

#define ZIP_MAX_CD (16u << 20)        /* central directory bytes */
#define ZIP_MAX_MEMBER (128u << 20)   /* one member, packed or unpacked */

/* The central directory of a zip (malloc'd, *size bytes, *count entries), checked against the
 * file: NULL when the file is not a zip or any field points outside it. */
static uint8_t *zip_cd(FILE *f, uint32_t *size, uint32_t *count)
{
    uint8_t tail[65557], *cd;
    long fsize, back, i;
    uint32_t cdoff, cdsize;
    if (fseek(f, 0, SEEK_END)) return NULL;
    fsize = ftell(f);
    if (fsize < 22) return NULL;
    back = fsize < (long)sizeof tail ? fsize : (long)sizeof tail;
    if (fseek(f, fsize - back, SEEK_SET) || fread(tail, 1, (size_t)back, f) != (size_t)back) return NULL;
    for (i = back - 22; i >= 0; i--) if (rd32(tail + i) == 0x06054b50u) break;   /* end of central directory */
    if (i < 0) return NULL;
    *count = rd16(tail + i + 10); cdsize = rd32(tail + i + 12); cdoff = rd32(tail + i + 16);
    if (!cdsize || cdsize > ZIP_MAX_CD || (uint64_t)cdoff + cdsize > (uint64_t)fsize) return NULL;
    if (!(cd = malloc(cdsize))) return NULL;
    if (fseek(f, (long)cdoff, SEEK_SET) || fread(cd, 1, cdsize, f) != cdsize) { free(cd); return NULL; }
    *size = cdsize;
    return cd;
}

/* Entry at cd+i if it fits wholly inside the directory: its total length, else 0. */
static uint32_t zip_entry(const uint8_t *cd, uint32_t size, uint32_t i)
{
    uint32_t len;
    if ((uint64_t)i + 46 > size || rd32(cd + i) != 0x02014b50u) return 0;
    len = 46u + rd16(cd + i + 28) + rd16(cd + i + 30) + rd16(cd + i + 32);
    return (uint64_t)i + len <= size ? len : 0;
}

/* A texture pack as one .zip: every member PNG named <key>... (in any folder of the zip). */
static void scan_zip(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *cd = NULL;
    uint32_t cdsize = 0, n = 0, i, j, len;
    int found = 0;
    if (!f) return;
    if (!(cd = zip_cd(f, &cdsize, &n))) goto out;
    for (i = 0, j = 0; j < n && (len = zip_entry(cd, cdsize, i)); j++, i += len) {
        uint16_t nl = rd16(cd + i + 28);
        char name[300];
        const char *base;
        uint64_t k;
        if (nl < sizeof name) {
            memcpy(name, cd + i + 46, nl); name[nl] = 0;
            base = strrchr(name, '/'); base = base ? base + 1 : name;
            if (key_of(base, &k)) { add_repl(k, path, (long)rd32(cd + i + 42)); found++; }
        }
    }
    if (found) { s_npacks++; fprintf(stderr, "[TEXPACK] pack %s: %d textures\n", path, found); }
out:
    free(cd);
    fclose(f);
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The textures folder: loose PNGs and .zip packs, subfolders too, in name order. */
static void scan_replace(const char *dir, int depth)   /* depth: folders below textures/ (capped: link loops) */
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char **names = NULL;
    int nn = 0, i, loose = 0;
    if (!d) return;
    while ((de = readdir(d))) {
        char **m;
        if (de->d_name[0] == '.') continue;
        if (!depth && (!strcmp(de->d_name, "dump") || !strcmp(de->d_name, "glyphs"))) continue;
        if (!(m = realloc(names, (size_t)(nn + 1) * sizeof *names))) break;
        names = m;
        names[nn++] = strdup(de->d_name);
    }
    closedir(d);
    if (nn) qsort(names, (size_t)nn, sizeof *names, name_cmp);
    for (i = 0; i < nn; i++) {
        const char *n = names[i];
        size_t l = strlen(n);
        char full[1024];
        struct stat st;
        uint64_t k;
        snprintf(full, sizeof full, "%s/%s", dir, n);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) { if (depth < 8) scan_replace(full, depth + 1); }
        else if (l > 4 && (!strcmp(n + l - 4, ".zip") || !strcmp(n + l - 4, ".ZIP"))) scan_zip(full);
        else if (key_of(n, &k)) { add_repl(k, full, -1); loose++; }
        free(names[i]);
    }
    free(names);
    if (loose) s_npacks++;
}

/* A zip member's bytes, uncompressed (stored or deflate), with every size checked against the
 * file and the limits above; NULL for anything malformed. */
static uint8_t *zip_member(const char *path, long off, int *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[30], *c = NULL, *out = NULL;
    uint32_t csize = 0, usize = 0;
    uint16_t method;
    long fsize;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (fsize = ftell(f)) < 30 || off < 0 || off > fsize - 30) goto out;
    if (fseek(f, off, SEEK_SET) || fread(h, 1, 30, f) != 30 || rd32(h) != 0x04034b50u) goto out;
    method = rd16(h + 8);
    {   /* the sizes from the central directory (always present; the local header may defer them) */
        uint8_t *cd;
        uint32_t cdsize = 0, n = 0, i, j, len;
        int hit = 0;
        if (!(cd = zip_cd(f, &cdsize, &n))) goto out;
        for (i = 0, j = 0; j < n && (len = zip_entry(cd, cdsize, i)); j++, i += len)
            if ((long)rd32(cd + i + 42) == off) { csize = rd32(cd + i + 20); usize = rd32(cd + i + 24); hit = 1; break; }
        free(cd);
        if (!hit) goto out;
    }
    if (csize > ZIP_MAX_MEMBER || usize > ZIP_MAX_MEMBER || !usize) goto out;
    {
        long data = off + 30 + rd16(h + 26) + rd16(h + 28);
        if (data > fsize || (uint64_t)csize > (uint64_t)(fsize - data)) goto out;
        if (fseek(f, data, SEEK_SET) || !(c = malloc(csize ? csize : 1)) || fread(c, 1, csize, f) != csize) goto out;
    }
    if (method == 0) {
        if (csize != usize) goto out;
        out = c; c = NULL; *len = (int)csize;
    } else if (method == 8) {
        int got;
        if (!(out = malloc(usize))) goto out;
        got = stbi_zlib_decode_noheader_buffer((char *)out, (int)usize, (const char *)c, (int)csize);
        if (got != (int)usize) { free(out); out = NULL; goto out; }   /* short, corrupt or larger than declared */
        *len = got;
    }
out:
    free(c);
    if (f) fclose(f);
    return out;
}

static void texpack_init(void)
{
    const char *e;
    char sub[600];
    s_init = 1;
    e = getenv("CJ_TEX_DIR");
    snprintf(s_dir, sizeof s_dir, "%s", e && *e ? e : "textures");
    s_dump = (e = getenv("CJ_TEX_DUMP")) && *e == '1';
    s_repl = !((e = getenv("CJ_TEX_REPLACE")) && *e == '0');
    if (s_dump) {
        MKDIR(s_dir);
        snprintf(sub, sizeof sub, "%s/dump", s_dir);
        MKDIR(sub);
        fprintf(stderr, "[TEXPACK] dumping textures to %s\n", sub);
    }
    if (s_repl) {
        int i, o = 0;
        scan_replace(s_dir, 0);
        if (s_nrp) qsort(s_rp, (size_t)s_nrp, sizeof *s_rp, repl_cmp);
        for (i = 0; i < s_nrp; i++) {      /* one per key: the last pack in name order wins */
            if (i + 1 < s_nrp && s_rp[i + 1].key == s_rp[i].key) { free(s_rp[i].path); continue; }
            s_rp[o++] = s_rp[i];
        }
        s_nrp = o;
        if (s_nrp) fprintf(stderr, "[TEXPACK] %d replacement textures from %d pack(s) in %s\n", s_nrp, s_npacks, s_dir);
        else s_repl = 0;
    }
    (void)sub;
}

/* Video frames (YUV, fmt 0x24/0x25) change every frame: never packed. */
int texpack_skip_format(uint32_t fmt) { return fmt == 0x24 || fmt == 0x25; }

int texpack_active(void)
{
    if (!s_init) texpack_init();
    return 1;   /* built-in art (builtin_find) needs the keys even with packs off */
}

uint64_t texpack_key(const void *data, size_t n, uint32_t fmt, uint32_t w, uint32_t h)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t k = 0xCBF29CE484222325ull;
    size_t i;
    k = (k ^ fmt) * 0x100000001B3ull;
    k = (k ^ w) * 0x100000001B3ull;
    k = (k ^ h) * 0x100000001B3ull;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t v; memcpy(&v, p + i, 8);
        k = (k ^ v) * 0x100000001B3ull;
        k ^= k >> 29;
    }
    for (; i < n; i++) k = (k ^ p[i]) * 0x100000001B3ull;
    return k ? k : 1;
}

/* 1 if `key` should be dumped now (dumping on, not dumped before); marks it. */
int texpack_want_dump(uint64_t key)
{
    return s_dump && dumped_insert(key);
}

/* argb: the decoder's texels (0xAARRGGBB words). */
void texpack_dump(uint64_t key, uint32_t w, uint32_t h, uint32_t fmt, const uint32_t *argb)
{
    char path[700];
    uint8_t *rgba;
    size_t i, n = (size_t)w * h;
    if (!s_dump) return;
    if (!(rgba = malloc(n * 4))) return;
    for (i = 0; i < n; i++) {
        uint32_t c = argb[i];
        rgba[i * 4 + 0] = (uint8_t)(c >> 16); rgba[i * 4 + 1] = (uint8_t)(c >> 8);
        rgba[i * 4 + 2] = (uint8_t)c;         rgba[i * 4 + 3] = (uint8_t)(c >> 24);
    }
    snprintf(path, sizeof path, "%s/dump/%016llx_%ux%u_%02X.png", s_dir, (unsigned long long)key, w, h, fmt);
    if (stbi_write_png(path, (int)w, (int)h, 4, rgba, (int)w * 4)) s_ndump++;
    free(rgba);
    if (s_ndump && s_ndump % 100 == 0) fprintf(stderr, "[TEXPACK] %u textures dumped\n", s_ndump);
}

/* The replacement for `key`, loaded on first use: RGBA bytes, top row first. */
/* F3 in game: texture packs on/off for this session (src/gl_backend.c re-uploads every texture). */
static int s_off;
int texpack_toggle(void) { s_off = !s_off; return !s_off; }
int texpack_count(void) { if (!s_init) texpack_init(); return s_nrp; }

/* Built-in art (always on, below any pack): the pause menu's modern controller diagram in place
 * of the Xbox controller texture (hud.edb 0x86000076); mods/FEPause.cpp lays its labels over it. */
#include "ctrl_art.h"
int texpack_builtin_ctrl;   /* set by src/gl_backend.c while the pause menu exists (the main menu's
                               controller page still lays the original labels over this texture) */
static const uint8_t *builtin_find(uint64_t key, int *w, int *h)
{
    static uint8_t *px;
    static int pw, ph, tried;
    if (key != 0xba1de2225579e28cull || !texpack_builtin_ctrl) return NULL;
    if (texpack_builtin_ctrl == 2) {   /* keyboard: the page lists the keys, so no pad picture */
        static const uint8_t clear[4 * 4 * 4];
        *w = 4; *h = 4;
        return clear;
    }
    if (!tried) {
        int c;
        tried = 1;
        px = stbi_load_from_memory(ctrl_art_png, (int)sizeof ctrl_art_png, &pw, &ph, &c, 4);
    }
    if (!px) return NULL;
    *w = pw; *h = ph;
    return px;
}

static const uint8_t *pack_find(uint64_t key, int *w, int *h);
const uint8_t *texpack_find(uint64_t key, int *w, int *h)
{
    const uint8_t *p = (s_repl && !s_off) ? pack_find(key, w, h) : NULL;
    return p ? p : builtin_find(key, w, h);
}

static const uint8_t *pack_find(uint64_t key, int *w, int *h)
{
    int lo = 0, hi = s_nrp - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        Repl *r = &s_rp[mid];
        if (r->key == key) {
            if (!r->tried) {
                int c;
                r->tried = 1;
                if (r->zoff >= 0) {
                    int len = 0;
                    uint8_t *png = zip_member(r->path, r->zoff, &len);
                    r->rgba = png ? stbi_load_from_memory(png, len, &r->w, &r->h, &c, 4) : NULL;
                    free(png);
                } else
                    r->rgba = stbi_load(r->path, &r->w, &r->h, &c, 4);
                if (!r->rgba) fprintf(stderr, "[TEXPACK] cannot read %s: %s\n", r->path, stbi_failure_reason());
                else { s_nrepl_used++; fprintf(stderr, "[TEXPACK] using %s (%dx%d)\n", r->path, r->w, r->h); }
            }
            if (!r->rgba) return NULL;
            *w = r->w; *h = r->h;
            return r->rgba;
        }
        if (r->key < key) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}
