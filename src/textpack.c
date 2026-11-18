/*
 * Text packs: dump the game's text and replace strings.
 *
 *   CJ_TEXT_DUMP=1    writes every string (2,489 of them) to text/dump.txt as
 *                     <id><TAB><text>, once the language file is loaded.
 *   text/replace.txt  lines in the same form replace those strings in game
 *                     (CJ_TEXT_REPLACE=0 ignores the file). ~X codes are the
 *                     game's button icons; keep them. A replacement may be as
 *                     long as the space the game allocated for the original
 *                     (the original's length, or longer for strings with
 *                     button codes); longer ones are cut there (not so with
 *                     CJ_LANG, below).
 *   CJ_TEXT_DIR=<dir> the folder (default: text next to pcj.exe).
 *   CJ_LANG=<name>    also reads lang/<name>/text.txt (a translation, UTF-8) first; replace.txt
 *                     still wins. The game then reads all its text from an image the port
 *                     builds (use_image); see src/langfont.c for the fonts.
 *
 * The game decodes a string the first time it is used (FEText::Get) into an
 * EXString; the port rewrites that decoded copy, so the disc file is never
 * touched. gText is the FEText instance at 0x48FD00 (layout: include/FEText.h).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

extern void *xbox_GetMemoryOffset(void);
#define GTEXT 0x0048FD00u
static uint8_t *M;
static int gok(uint32_t va, uint32_t n) { uint32_t a = va & 0x7FFFFFFFu; return va && a >= 0x10000u && a + n < 0x08000000u; }
static uint32_t g32(uint32_t va) { uint32_t v = 0; if (gok(va, 4)) memcpy(&v, M + va, 4); return v; }
static uint16_t g16(uint32_t va) { uint16_t v = 0; if (gok(va, 2)) memcpy(&v, M + va, 2); return v; }
static int16_t gs16(uint32_t va) { return (int16_t)g16(va); }

typedef struct { uint32_t id; char *text; } Over;
static Over *s_ov;
static int s_nov, s_init, s_dump, s_repl;
static char s_dir[512];
static uint32_t s_dumped_for;

/* FEText::Decompress over guest memory: Huffman nodes {short left; short right
 * or char ch[2]}, a negative left marks a leaf (-1: one char in ch[0], < -1:
 * two chars ch[1] then ch[0]); bits are taken LSB first from dwords. */
static int decode(uint32_t huff, uint32_t bits, char *out, int max)
{
    uint32_t node = huff;
    int n = 0, guard = 0;
    for (;;) {
        uint32_t w = g32(bits); bits += 4;
        int i;
        if (++guard > 4096) return n;
        for (i = 0; i < 32; i++) {
            int16_t left = gs16(node);
            node += (uint32_t)(int32_t)((w & 1) ? gs16(node + 2) : left) * 4u;
            w >>= 1;
            left = gs16(node);
            if (left >= 0) continue;
            if (left < -1) { char c = (char)M[node + 3]; if (c == (char)0xAC) c = (char)0xA0; if (n < max - 1) out[n++] = c; }
            {
                char c = (char)M[node + 2];
                if (c == (char)0xAC) c = (char)0xA0;
                if (n < max - 1) out[n++] = c;
                if (c == 0) return n;
            }
            node = huff;
        }
    }
}

static void dump_all(void)
{
    uint32_t data = g32(GTEXT + 0x1C), huff = g32(GTEXT + 0x20), dir = g32(GTEXT + 0x24);
    int ndir = (int)g32(GTEXT + 0x28), i;
    char path[600], buf[4096];
    FILE *f;
    MKDIR(s_dir);
    snprintf(path, sizeof path, "%s/dump.txt", s_dir);
    if (!(f = fopen(path, "wb"))) return;
    fprintf(f, "# Predator: Concrete Jungle text. Copy lines to replace.txt and edit the text after the tab.\n");
    for (i = 0; i < ndir && i < 4096; i++) {
        uint32_t e = dir + (uint32_t)i * 6;
        uint16_t h = g16(e);
        int16_t off = gs16(e + 2);
        int n = decode(huff, data + (uint32_t)(int32_t)off * 4u, buf, sizeof buf);
        char *p;
        buf[n > 0 ? n - 1 : 0] = 0;
        for (p = buf; *p; p++) if (*p == '\n') *p = '|'; else if (*p == '\r' || *p == '\t') *p = ' ';
        fprintf(f, "%08X\t%s\n", 0x51000000u | h, buf);
    }
    fclose(f);
    fprintf(stderr, "[TEXT] %d strings written to %s\n", ndir, path);
}

static void add_over(uint32_t id, const char *text)
{
    int i;
    for (i = 0; i < s_nov; i++) if (s_ov[i].id == id) { free(s_ov[i].text); s_ov[i].text = strdup(text); return; }
    if (!(s_ov = realloc(s_ov, (size_t)(s_nov + 1) * sizeof *s_ov))) { s_nov = 0; return; }
    s_ov[s_nov].id = id; s_ov[s_nov].text = strdup(text); s_nov++;
}

/* The game's text is 8-bit (Latin-1: the fonts' accented letters sit at their Latin-1 codes,
 * src/langfont.c). Files are UTF-8; a line that isn't valid UTF-8 is taken as Latin-1 already. */
static void utf8_to_latin1(char *s)
{
    unsigned char *p = (unsigned char *)s, *q = p;
    for (const unsigned char *t = p; *t; t++)
        if (*t >= 0x80) {
            if ((t[0] & 0xE0) == 0xC0 && (t[1] & 0xC0) == 0x80) { t++; continue; }
            if ((t[0] & 0xF0) == 0xE0 && (t[1] & 0xC0) == 0x80 && (t[2] & 0xC0) == 0x80) { t += 2; continue; }
            if ((t[0] & 0xF8) == 0xF0 && (t[1] & 0xC0) == 0x80 && (t[2] & 0xC0) == 0x80 && (t[3] & 0xC0) == 0x80) { t += 3; continue; }
            return;   /* not UTF-8 */
        }
    while (*p) {
        unsigned cp;
        if (*p < 0x80) { *q++ = *p++; continue; }
        if ((*p & 0xE0) == 0xC0) { cp = (unsigned)(p[0] & 0x1F) << 6 | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = (unsigned)(p[0] & 0x0F) << 12 | (unsigned)(p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
        else { cp = '?'; p += 4; }
        if (cp == 0x2018 || cp == 0x2019) cp = '\'';          /* typographic quotes and dashes */
        else if (cp == 0x201C || cp == 0x201D) cp = '"';
        else if (cp == 0x2013 || cp == 0x2014) cp = '-';
        else if (cp == 0x2026) { *q++ = '.'; *q++ = '.'; cp = '.'; }
        else if (cp > 0xFF) cp = '?';
        *q++ = (unsigned char)cp;
    }
    *q = 0;
}

static void load_file(const char *path)
{
    char line[4200];
    FILE *f;
    int before = s_nov;
    if (!(f = fopen(path, "rb"))) return;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t'), *e, *p;
        uint32_t id;
        if (line[0] == '#' || !tab) continue;
        *tab = 0;
        id = (uint32_t)strtoul(line, &e, 16);
        if (*e || id <= 0x51000000u || id >= 0x510009B9u) continue;
        p = tab + 1;
        for (e = p + strlen(p); e > p && (e[-1] == '\n' || e[-1] == '\r'); ) *--e = 0;
        for (e = p; *e; e++) if (*e == '|') *e = '\n';
        utf8_to_latin1(p);
        add_over(id, p);
    }
    fclose(f);
    if (s_nov > before) fprintf(stderr, "[TEXT] %d strings from %s\n", s_nov - before, path);
}

static void load_replace(void)
{
    extern const char *lang_name(void);
    char path[600];
    const char *lang = lang_name();
    if (lang) { snprintf(path, sizeof path, "lang/%s/text.txt", lang); load_file(path); }
    snprintf(path, sizeof path, "%s/replace.txt", s_dir);
    load_file(path);
}

/* Built-in replacements from port features (src/recomp_manual.c). */
void cj_text_builtin(uint32_t id, const char *text) { add_over(id, text); }

/* Puts override i into the game's decoded copy of its string (if decoded). */
static void fix_one(uint32_t strings, int i)
{
    uint32_t idx = s_ov[i].id - 0x51000000u, ent = strings + idx * 8u, ps, chars;
    size_t len, alloc;
    if (!gok(ent, 8) || !M[ent + 4]) return;                  /* not decoded yet */
    if (!(ps = g32(ent)) || !(chars = g32(ps)) || !gok(chars - 8, 16)) return;
    alloc = g16(chars - 2);                                      /* EXStringData.nAllocLength */
    len = strlen(s_ov[i].text);
    if (len > alloc) len = alloc;
    if (!memcmp(M + chars, s_ov[i].text, len) && M[chars + len] == 0) return;
    memcpy(M + chars, s_ov[i].text, len);
    M[chars + len] = 0;
    { uint16_t l16 = (uint16_t)len; memcpy(M + chars - 4, &l16, 2); }   /* nDataLength */
}

/* A translation (CJ_LANG) replaces the whole text file image. Its strings are longer than the
 * English and use letters the file's Huffman code has no symbol for, and the menus copy a string the
 * moment they decode it, so patching decoded strings afterwards is too late for some. Instead the
 * port builds a new image in the file's own format (FEText: a header of two dwords, the Huffman
 * tree, the bit stream, the 6-byte directory) holding every string, translated or English, and points
 * gText's m_pData / m_pHuff / m_pDir at it. The game then decodes the translation itself, at the size
 * it needs. When the game puts its pointers back (HandleMemCompaction), they are pointed at the new
 * image again and strings decoded in between are decoded again. */
static uint32_t s_img, s_img_data, s_img_dir;
static int s_img_ndir, s_img_failed;

typedef struct { int sym, l, r; unsigned long long f; } HNode;
static int over_index(uint32_t id) { int i; for (i = 0; i < s_nov; i++) if (s_ov[i].id == id) return i; return -1; }

static void build_image(void)
{
    extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
    uint32_t data = g32(GTEXT + 0x1C), huff = g32(GTEXT + 0x20), dir = g32(GTEXT + 0x24);
    int ndir = (int)g32(GTEXT + 0x28), i, n, nn, root;
    char **txt;
    unsigned long long freq[256] = {0};
    static HNode node[512];
    int order[512], depth_code_len[256];
    uint32_t code[256];
    int clen[256];
    uint32_t *bitpos, nwords_b = 0, nwords_a = 0, *off;
    int *in_a;
    if (ndir <= 0 || ndir > 4096 || !gok(dir, (uint32_t)ndir * 6)) { s_img_failed = 1; return; }
    txt = calloc((size_t)ndir, sizeof *txt);
    bitpos = calloc((size_t)ndir, sizeof *bitpos);
    off = calloc((size_t)ndir, sizeof *off);
    in_a = calloc((size_t)ndir, sizeof *in_a);
    if (!txt || !bitpos || !off || !in_a) { s_img_failed = 1; return; }
    for (i = 0; i < ndir; i++) {
        uint32_t e = dir + (uint32_t)i * 6;
        int k = over_index(0x51000000u | g16(e));
        if (k >= 0) txt[i] = strdup(s_ov[k].text);
        else {
            char buf[4096];
            int m = decode(huff, data + (uint32_t)(int32_t)gs16(e + 2) * 4u, buf, sizeof buf);
            buf[m > 0 ? m - 1 : 0] = 0;
            txt[i] = strdup(buf);
        }
        for (const unsigned char *c = (const unsigned char *)txt[i]; ; c++) {
            freq[*c == 0xAC ? 0xA0 : *c]++;
            if (!*c) break;
        }
    }
    /* Huffman tree over the bytes used (0 ends a string) */
    for (n = 0, i = 0; i < 256; i++) if (freq[i]) { node[n].sym = i; node[n].l = node[n].r = -1; node[n].f = freq[i]; n++; }
    if (n < 2) { node[n].sym = 1; node[n].l = node[n].r = -1; node[n].f = 1; n++; }
    nn = n;
    {
        int alive[512], na = n;
        for (i = 0; i < n; i++) alive[i] = i;
        while (na > 1) {
            int a = -1, b = -1, x;
            for (x = 0; x < na; x++) {
                if (a < 0 || node[alive[x]].f < node[alive[a]].f) { b = a; a = x; }
                else if (b < 0 || node[alive[x]].f < node[alive[b]].f) b = x;
            }
            node[nn].sym = -1; node[nn].l = alive[a]; node[nn].r = alive[b]; node[nn].f = node[alive[a]].f + node[alive[b]].f;
            if (a > b) { int t = a; a = b; b = t; }
            alive[b] = alive[--na];
            alive[a] = nn++;
        }
        root = alive[0];
    }
    /* codes, and the tree laid out breadth first: children always after their parent */
    {
        int q[512], qh = 0, qt = 0, pos[512];
        uint32_t cc[512]; int cl[512];
        q[qt++] = root; cc[root] = 0; cl[root] = 0;
        while (qh < qt) {
            int v = q[qh];
            pos[v] = qh;
            order[qh++] = v;
            if (node[v].sym < 0) {
                cc[node[v].l] = cc[v]; cl[node[v].l] = cl[v] + 1;
                cc[node[v].r] = cc[v] | (1u << cl[v]); cl[node[v].r] = cl[v] + 1;
                q[qt++] = node[v].l; q[qt++] = node[v].r;
            } else { code[node[v].sym] = cc[v]; clen[node[v].sym] = cl[v]; }
        }
        for (i = 0; i < qt; i++) { int v = order[i]; if (node[v].sym >= 0 && cl[v] > 31) { s_img_failed = 1; goto out; } }
        (void)pos; (void)depth_code_len;
        /* per string: bit length, dword offsets; strings go after the header while a short dword offset
         * reaches them, the rest before it (negative offsets) */
        {
            uint32_t tree_words = (uint32_t)qt, words[4096];
            for (i = 0; i < ndir; i++) {
                uint32_t bits = 0;
                for (const unsigned char *c = (const unsigned char *)txt[i]; ; c++) { bits += (uint32_t)clen[*c == 0xAC ? 0xA0 : *c]; if (!*c) break; }
                words[i] = (bits + 31) / 32;
            }
            for (i = 0; i < ndir; i++) {
                uint32_t o = 2 + tree_words + nwords_b;
                if (o + words[i] <= 32767u) { in_a[i] = 0; off[i] = o; nwords_b += words[i]; }
                else { in_a[i] = 1; nwords_a += words[i]; off[i] = nwords_a; }   /* counted back from the header */
            }
            if (nwords_a > 32768u) { s_img_failed = 1; goto out; }
            {
                uint32_t total = (nwords_a + 2 + tree_words + nwords_b) * 4u + (uint32_t)ndir * 6u + 16u;
                uint32_t base = xbox_HeapAlloc(total, 16), hdr, w;
                if (!base) { s_img_failed = 1; goto out; }
                memset(M + base, 0, total);
                hdr = base + nwords_a * 4u;
                s_img = base; s_img_data = hdr; s_img_ndir = ndir;
                s_img_dir = hdr + (2 + tree_words + nwords_b) * 4u;
                w = (uint32_t)ndir; memcpy(M + hdr, &w, 4);
                w = s_img_dir - hdr; memcpy(M + hdr + 4, &w, 4);
                for (i = 0; i < qt; i++) {   /* nodes: {short left, short right}, or a leaf {-1, ch, 0} */
                    int v = order[i];
                    uint32_t at = hdr + 8u + (uint32_t)i * 4u;
                    if (node[v].sym < 0) {
                        int16_t l = 0, r = 0;
                        int x;
                        for (x = i + 1; x < qt; x++) { if (order[x] == node[v].l) l = (int16_t)(x - i); if (order[x] == node[v].r) r = (int16_t)(x - i); }
                        memcpy(M + at, &l, 2); memcpy(M + at + 2, &r, 2);
                    } else {
                        int16_t l = -1;
                        memcpy(M + at, &l, 2); M[at + 2] = (uint8_t)node[v].sym; M[at + 3] = 0;
                    }
                }
                for (i = 0; i < ndir; i++) {
                    int32_t dw = in_a[i] ? -(int32_t)off[i] : (int32_t)off[i];
                    uint32_t at = hdr + (uint32_t)(dw * 4), bit = 0, e = s_img_dir + (uint32_t)i * 6u;
                    size_t len = strlen(txt[i]);
                    uint16_t h = g16(dir + (uint32_t)i * 6), l16 = (uint16_t)((len > 0x7FFF ? 0x7FFF : len) | (strchr(txt[i], '~') ? 0x8000u : 0));
                    int16_t o16 = (int16_t)dw;
                    for (const unsigned char *c = (const unsigned char *)txt[i]; ; c++) {
                        int sym = *c == 0xAC ? 0xA0 : *c, b;
                        for (b = 0; b < clen[sym]; b++, bit++)
                            if (code[sym] >> b & 1) M[at + (bit >> 5) * 4 + ((bit & 31) >> 3)] |= (uint8_t)(1u << (bit & 7));
                        if (!*c) break;
                    }
                    memcpy(M + e, &h, 2); memcpy(M + e + 2, &o16, 2); memcpy(M + e + 4, &l16, 2);
                }
                fprintf(stderr, "[TEXT] new text image: %d strings, %u KB (tree %d nodes)\n", ndir, total / 1024, qt);
                {   /* check: every string decodes back */
                    int bad = 0;
                    for (i = 0; i < ndir; i++) {
                        char buf[4096];
                        int m = decode(hdr + 8u, hdr + (uint32_t)(int32_t)gs16(s_img_dir + (uint32_t)i * 6u + 2) * 4u, buf, sizeof buf);
                        buf[m > 0 ? m - 1 : 0] = 0;
                        if (strcmp(buf, txt[i])) bad++;
                    }
                    if (bad) { fprintf(stderr, "[TEXT] new text image: %d strings decode wrong; not used\n", bad); s_img_failed = 1; s_img = 0; }
                }
            }
        }
    }
out:
    for (i = 0; i < ndir; i++) free(txt[i]);
    free(txt); free(bitpos); free(off); free(in_a);
}

static void use_image(void)
{
    extern const char *lang_name(void);
    uint32_t strings, k;
    if (!lang_name() || s_img_failed || !s_nov) return;
    if (!s_img) build_image();
    if (!s_img || g32(GTEXT + 0x1C) == s_img_data) return;
    memcpy(M + GTEXT + 0x1C, &s_img_data, 4);
    { uint32_t h = s_img_data + 8u; memcpy(M + GTEXT + 0x20, &h, 4); }
    memcpy(M + GTEXT + 0x24, &s_img_dir, 4);
    memcpy(M + GTEXT + 0x28, &s_img_ndir, 4);
    if ((strings = g32(GTEXT + 0x10)))   /* anything decoded from the old image is decoded again */
        for (k = 0; k < 0x9B9u; k++) if (gok(strings + k * 8u, 8)) M[strings + k * 8u + 4] = 0;
}

void cj_text_tick(void)
{
    uint32_t data, strings;
    int i;
    M = (uint8_t *)xbox_GetMemoryOffset();
    if (!s_init) {
        const char *e = getenv("CJ_TEXT_DIR");
        s_init = 1;
        snprintf(s_dir, sizeof s_dir, "%s", e && *e ? e : "text");
        s_dump = (e = getenv("CJ_TEXT_DUMP")) && *e == '1';
        s_repl = !((e = getenv("CJ_TEXT_REPLACE")) && *e == '0');
        if (s_repl) {
            /* PC wording for the Xbox save messages (replace.txt can still override them) */
            static const struct { uint32_t id; const char *t; } pc[] = {
                {0x510002E6u, "Saving Predator.\nPlease don't quit the game."},
                {0x51000825u, "Loading data.\nPlease don't quit the game."},
                {0x5100082Au, "No Predator saved games found.\n\nWould you like to create a Predator save?"},
                {0x510002E7u, "There isn't enough free disk space to save games.\n\nPress ~A to continue without saving.\n"},
                /* controller page labels, short enough for their column (mods/FEPause.cpp) */
                {0x510004ABu, "Cam Dist / Zoom Out"},
                {0x510004ACu, "Center Cam / Zoom In"},
            };
            unsigned k;
            for (k = 0; k < sizeof pc / sizeof pc[0]; k++) add_over(pc[k].id, pc[k].t);
            load_replace();
        }
    }
    if (!M[GTEXT + 0x14] || !(data = g32(GTEXT + 0x1C))) return;     /* language not loaded */
    if (s_dump && s_dumped_for != data) { s_dumped_for = data; dump_all(); }
    use_image();   /* a translation: the game decodes from our text image */
    strings = g32(GTEXT + 0x10);                                      /* m_Strings.m_pData (TextEntry[]) */
    if (!strings || !s_nov) return;

    if (s_img && g32(GTEXT + 0x1C) == s_img_data) return;   /* the image holds every override */
    for (i = 0; i < s_nov; i++) fix_one(strings, i);
}
