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
 *                     button codes); longer ones are cut there.
 *   CJ_TEXT_DIR=<dir> the folder (default: text next to pcj.exe).
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

static void load_replace(void)
{
    char path[600], line[4200];
    FILE *f;
    snprintf(path, sizeof path, "%s/replace.txt", s_dir);
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
        add_over(id, p);
    }
    fclose(f);
    if (s_nov) fprintf(stderr, "[TEXT] %d replacement strings from %s\n", s_nov, path);
}

/* Built-in replacements from port features (src/recomp_manual.c). */
void cj_text_builtin(uint32_t id, const char *text) { add_over(id, text); }

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
    strings = g32(GTEXT + 0x10);                                      /* m_Strings.m_pData (TextEntry[]) */
    if (!strings || !s_nov) return;
    for (i = 0; i < s_nov; i++) {
        uint32_t idx = s_ov[i].id - 0x51000000u, ent = strings + idx * 8u, ps, chars;
        size_t len, alloc;
        if (!gok(ent, 8) || !M[ent + 4]) continue;                  /* not decoded yet */
        if (!(ps = g32(ent)) || !(chars = g32(ps)) || !gok(chars - 8, 16)) continue;
        alloc = g16(chars - 2);                                      /* EXStringData.nAllocLength */
        len = strlen(s_ov[i].text);
        if (len > alloc) len = alloc;
        if (!memcmp(M + chars, s_ov[i].text, len) && M[chars + len] == 0) continue;
        memcpy(M + chars, s_ov[i].text, len);
        M[chars + len] = 0;
        { uint16_t l16 = (uint16_t)len; memcpy(M + chars - 4, &l16, 2); }   /* nDataLength */
    }
}
