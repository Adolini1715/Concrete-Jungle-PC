/* PC Settings page (Options > PC Settings, mods/FEMainMenu.cpp).
 *
 * The page is built by the game's own front-end code (checkboxes, sliders and
 * spinners bound to guest memory), so it looks and navigates like the other
 * Options pages. This file owns the settings: their table, the guest block the
 * widgets are bound to, the strings the page shows, applying changes live and
 * saving them to pcj.ini.
 *
 * Guest block layout (allocated on the guest heap by the mod, attached once):
 *   +0x000 int vals[PCS_MAX]    widget-bound values
 *   +0x080 float widths[2]      grid column proportions
 *   +0x100 strings              labels, help texts, misc (NUL-terminated)
 *
 * Threads: the game thread calls pcs_tick() (from cj_fps_tick) which syncs the
 * guest block with s_val[]; the GL side reads s_val[] through pcs_get() and
 * applies window/scale changes itself (gl_backend.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcsettings.h"

#define PCS_MAX 32
#define BLK_STR 0x100
#define BLK_SIZE 0x1000

typedef struct {
    const char *key, *label, *help;
    int kind, lo, hi, delta, def;
} PcSet;

/* Order = row order on the page; the PCS_* ids in pcsettings.h index this. The
 * page shows 9 rows and scrolls; a spinner or slider draws itself even when
 * scrolled out of view, so those rows sit at 4-8, which every scroll position shows. */
static const PcSet s_set[] = {
    {"CJ_WIDESCREEN", "Widescreen", "The game's own 16:9 mode with a 16:9 window.", PCS_CHECK, 0, 1, 1, 0},
    {"CJ_FULLSCREEN", "Fullscreen", "Desktop fullscreen. F11 or Alt+Enter also toggle it.", PCS_CHECK, 0, 1, 1, 0},
    {"CJ_VSYNC", "V-Sync", "Sync to the monitor. Off has the lowest input lag.", PCS_CHECK, 0, 1, 1, 0},
    {"CJ_TEX_REPLACE", "Texture Packs", "Use replacement textures from the textures folder. Applies after a restart.",
     PCS_CHECK, 0, 1, 1, 1},
    {"CJ_FPS", "Frame Rate", "60 runs the game at 60 fps. 30 is the original.", PCS_SPIN, 30, 60, 30, 30},
    {"CJ_GL_SCALE", "Resolution Scale", "1 = 640x480 (original), 2 = 1280x960, 3 = 1920x1440, 4 = 2560x1920, 5 = 3200x2400 (4K).",
     PCS_SPIN, 1, 5, 1, 1},
    {"CJ_MOUSE", "Mouse Look", "Turn the camera with the mouse. F10 frees the cursor.", PCS_CHECK, 0, 1, 1, 0},
    {"CJ_MOUSE_SENS", "Mouse Speed", "How far the camera turns per mouse movement.", PCS_SLIDER, 2, 50, 1, 15},
    {"CJ_MOUSE_INVERT", "Invert Mouse", "Mouse up looks down.", PCS_CHECK, 0, 1, 1, 0},
    {"CJ_CAM_MODERN", "Modern Camera", "The camera stops when you let go of the stick. Off: the original camera.",
     PCS_CHECK, 0, 1, 1, 1},
    {"CJ_CAM_FOLLOW", "Camera Follow", "With Modern Camera, swing slowly behind you while you run.", PCS_CHECK, 0, 1, 1, 1},
    {"CJ_GLYPHS", "PC Button Prompts", "Keyboard keys or modern controller icons, whichever you used last. "
     "Off: the original Xbox icons. Applies after a restart.", PCS_CHECK, 0, 1, 1, 1},
};
#define NSET ((int)(sizeof s_set / sizeof s_set[0]))

static const char *s_misc[] = {"PC Settings", "Game", "Options", "", "Off"};   /* 4: main menu Controller page */
#define NMISC ((int)(sizeof s_misc / sizeof s_misc[0]))

static volatile int s_val[PCS_MAX];     /* current values (any thread reads) */
static volatile unsigned s_gen[PCS_MAX];/* bumped on every change */
static int s_last[PCS_MAX];             /* guest block values last seen */
static char *s_blk;                     /* host view of the guest block */
static char *s_label[PCS_MAX], *s_help[PCS_MAX], *s_mstr[NMISC];
static int s_page;
static char s_ini[1024];
static int s_dirty;
static int s_changed[PCS_MAX];      /* keys to write at the next save */
static unsigned s_dirty_tick, s_tick;

static void put_env(const char *k, int v)
{
    char b[32];
    snprintf(b, sizeof b, "%d", v);
#ifdef _WIN32
    _putenv_s(k, b);
#else
    setenv(k, b, 1);
#endif
}

static int clampv(int i, int v)
{
    if (v < s_set[i].lo) v = s_set[i].lo;
    if (v > s_set[i].hi) v = s_set[i].hi;
    if (s_set[i].kind == PCS_SPIN && s_set[i].delta > 1)       /* 30/60 */
        v = s_set[i].lo + (v - s_set[i].lo + s_set[i].delta / 2) / s_set[i].delta * s_set[i].delta;
    return v;
}

static void init_vals(void)
{
    static int done;
    int i;
    if (done) return;
    done = 1;
    if (!s_ini[0] && getenv("CJ_PC_INI")) pcs_set_ini_path(getenv("CJ_PC_INI"));
    for (i = 0; i < NSET; i++) {
        const char *e = getenv(s_set[i].key);
        int v = s_set[i].def;
        if (e && *e) {
            if (s_set[i].kind == PCS_CHECK) v = *e != '0';
            else if (i == PCS_FPS) v = atoi(e) >= 45 ? 60 : 30;
            else v = atoi(e);
        }
        s_val[i] = clampv(i, v);
    }
}

int pcs_get(int id)
{
    init_vals();
    return (id >= 0 && id < NSET) ? s_val[id] : 0;
}

unsigned pcs_gen(int id) { return (id >= 0 && id < NSET) ? s_gen[id] : 0; }

static void save_ini(void);
static void flush_at_exit(void)
{
    if (s_dirty) { s_dirty = 0; save_ini(); }
}

void pcs_set(int id, int v)
{
    static int reg;
    if (!reg) { reg = 1; atexit(flush_at_exit); }
    init_vals();
    if (id < 0 || id >= NSET) return;
    v = clampv(id, v);
    if (s_val[id] == v) return;
    s_val[id] = v;
    s_gen[id]++;
    put_env(s_set[id].key, v);
    s_dirty = 1;
    s_changed[id] = 1;
    s_dirty_tick = s_tick;
    fprintf(stderr, "[PCSET] %s = %d\n", s_set[id].key, v);
}

/* Text settings saved the same way (the key bindings, src/keymap.c). */
#define XMAX 32
static char s_xkey[XMAX][32], s_xval[XMAX][48];
static int s_nx;
void pcs_set_str(const char *key, const char *val)
{
    static int reg;
    int i;
    if (!reg) { reg = 1; atexit(flush_at_exit); }
    for (i = 0; i < s_nx && strcmp(s_xkey[i], key); i++) ;
    if (i == s_nx) {
        if (s_nx == XMAX) return;
        snprintf(s_xkey[s_nx++], sizeof s_xkey[0], "%s", key);
    }
    snprintf(s_xval[i], sizeof s_xval[0], "%s", val);
    s_dirty = 1;
    s_dirty_tick = s_tick;
    fprintf(stderr, "[PCSET] %s = %s\n", key, val);
}

void pcs_set_ini_path(const char *p) { snprintf(s_ini, sizeof s_ini, "%s", p ? p : ""); }

/* pcj.ini: replace the first `KEY=` line (active or commented with # or ;)
 * for each setting, append the rest. Other lines are kept as they are. */
static void save_ini(void)
{
    char *lines[512], buf[1024], tmp[1100];
    int n = 0, i, j, found[PCS_MAX] = {0}, xfound[XMAX] = {0};
    const char *eol = "\n";
    FILE *f;
    if (!s_ini[0]) return;
    if ((f = fopen(s_ini, "rb"))) {     /* binary: keep the file's own line endings */
        while (n < 512 && fgets(buf, sizeof buf, f)) lines[n++] = strdup(buf);
        fclose(f);
    }
    if (n && strstr(lines[0], "\r\n")) eol = "\r\n";
    for (i = 0; i < n; i++) {
        char *p = lines[i];
        while (*p == ' ' || *p == '\t' || *p == '#' || *p == ';') p++;
        for (j = 0; j < s_nx; j++) {
            size_t kl = strlen(s_xkey[j]);
            const char *q = p + kl;
            if (xfound[j] || strncmp(p, s_xkey[j], kl)) continue;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '=') continue;
            {
                size_t ll = strlen(lines[i]);
                int crlf = ll >= 2 && lines[i][ll - 2] == '\r';
                snprintf(buf, sizeof buf, "%s=%s%s", s_xkey[j], s_xval[j], crlf ? "\r\n" : "\n");
            }
            free(lines[i]);
            lines[i] = strdup(buf);
            xfound[j] = 1;
            p = NULL;
            break;
        }
        if (!p) continue;
        for (j = 0; j < NSET; j++) {
            size_t kl = strlen(s_set[j].key);
            if (!s_changed[j] || found[j] || strncmp(p, s_set[j].key, kl)) continue;
            {
                const char *q = p + kl;
                while (*q == ' ' || *q == '\t') q++;
                if (*q != '=') continue;
            }
            {
                size_t ll = strlen(lines[i]);
                int crlf = ll >= 2 && lines[i][ll - 2] == '\r';
                snprintf(buf, sizeof buf, "%s=%d%s", s_set[j].key, s_val[j], crlf ? "\r\n" : "\n");
            }
            free(lines[i]);
            lines[i] = strdup(buf);
            found[j] = 1;
            break;
        }
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", s_ini);
    if (!(f = fopen(tmp, "wb"))) { fprintf(stderr, "[PCSET] cannot write %s\n", tmp); goto out; }
    for (i = 0; i < n; i++) {
        fputs(lines[i], f);
        if (i == n - 1 && lines[i][0] && lines[i][strlen(lines[i]) - 1] != '\n') fputs(eol, f);
    }
    {
        int hdr = 0;
        for (i = 0; i < n; i++)
            if (strstr(lines[i], "# PC Settings (written by")) hdr = 1;     /* header already there */
        for (j = 0; j < NSET; j++)
            if (s_changed[j] && !found[j]) {
                if (!hdr) { fprintf(f, "# PC Settings (written by the game's Options > PC Settings page)%s", eol); hdr = 1; }
                fprintf(f, "%s=%d%s", s_set[j].key, s_val[j], eol);
            }
        for (j = 0; j < s_nx; j++)
            if (!xfound[j]) {
                if (!hdr) { fprintf(f, "# PC Settings (written by the game's Options > PC Settings page)%s", eol); hdr = 1; }
                fprintf(f, "%s=%s%s", s_xkey[j], s_xval[j], eol);
            }
    }
    fclose(f);
#ifdef _WIN32
    remove(s_ini);
#endif
    if (rename(tmp, s_ini)) fprintf(stderr, "[PCSET] cannot replace %s\n", s_ini);
    else fprintf(stderr, "[PCSET] saved %s\n", s_ini);
out:
    for (i = 0; i < n; i++) free(lines[i]);
}

/* ---- mod interface (cjmod.h) -------------------------------------------- */

char *cjmod_pc_block(void) { return s_blk; }
void cjmod_pc_attach(char *p);

/* After a quick load: RAM now holds the block where the saved session had it (p, may differ from
 * this session's), and the menu pages bound to it there. Point at that copy and write this
 * session's values and strings into it, so the settings in effect stay as they are (a load does
 * not change PC settings) and the old address, which belongs to the loaded game now, is left alone. */
void pcs_rebind(char *p)
{
    s_blk = 0;                  /* NULL: the saved session had no block; the menu makes one when needed */
    if (p) cjmod_pc_attach(p);
}

void cjmod_pc_attach(char *p)
{
    int i;
    char *s;
    init_vals();
    if (s_blk || !p) return;
    memset(p, 0, BLK_SIZE);
    s_blk = p;
    ((float *)(p + 0x80))[0] = 0.62f;
    ((float *)(p + 0x80))[1] = 0.38f;
    s = p + BLK_STR;
#define ADDSTR(dst, src) do { size_t l_ = strlen(src) + 1; if (s + l_ > p + BLK_SIZE) { dst = p + BLK_SIZE - 1; break; } \
                              memcpy(s, src, l_); dst = s; s += l_; } while (0)
    for (i = 0; i < NMISC; i++) ADDSTR(s_mstr[i], s_misc[i]);
    for (i = 0; i < NSET; i++) { ADDSTR(s_label[i], s_set[i].label); ADDSTR(s_help[i], s_set[i].help); }
#undef ADDSTR
    for (i = 0; i < NSET; i++) ((int *)p)[i] = s_last[i] = s_val[i];
    fprintf(stderr, "[PCSET] page attached (%d settings, ini %s)\n", NSET, s_ini[0] ? s_ini : "none");
}

int cjmod_pc_page(int set)
{
    if (set >= 0) s_page = set != 0;
    return s_page;
}

/* which variant the Audio state's cached page holds (-1: not built yet) */
int cjmod_pc_built(int set)
{
    static int built = -1;
    if (set >= 0) built = set != 0;
    return built;
}

int cjmod_pc_count(void) { return NSET; }
int cjmod_pc_kind(int i) { return (i >= 0 && i < NSET) ? s_set[i].kind : 0; }
int cjmod_pc_lo(int i) { return (i >= 0 && i < NSET) ? s_set[i].lo : 0; }
int cjmod_pc_hi(int i) { return (i >= 0 && i < NSET) ? s_set[i].hi : 0; }
int cjmod_pc_delta(int i) { return (i >= 0 && i < NSET) ? s_set[i].delta : 1; }
char *cjmod_pc_label(int i) { return (s_blk && i >= 0 && i < NSET) ? s_label[i] : s_blk ? s_mstr[3] : 0; }
char *cjmod_pc_help(int i) { return (s_blk && i >= 0 && i < NSET) ? s_help[i] : s_blk ? s_mstr[3] : 0; }
char *cjmod_pc_str(int i) { return (s_blk && i >= 0 && i < NMISC) ? s_mstr[i] : s_blk ? s_mstr[3] : 0; }
int *cjmod_pc_val(int i) { return (s_blk && i >= 0 && i < NSET) ? (int *)s_blk + i : 0; }
float *cjmod_pc_widths(void) { return s_blk ? (float *)(s_blk + 0x80) : 0; }

/* Game thread, once per frame. */
/* CJ_PC_TEST="flip:KEY=val,flip:KEY=val": change settings at those flips, as
 * the page would (tests the live paths without driving the menu). */
static void test_script(void)
{
    static const char *e = (const char *)1;
    extern unsigned int cj_gl_flips(void);
    unsigned f;
    const char *p;
    if (e == (const char *)1) e = getenv("CJ_PC_TEST");
    if (!e) return;
    f = cj_gl_flips();
    for (p = e; p && *p;) {
        unsigned at = (unsigned)strtoul(p, (char **)&p, 10);
        char key[64]; int n = 0, i;
        if (*p++ != ':') break;
        while (*p && *p != '=' && n < 63) key[n++] = *p++;
        key[n] = 0;
        if (*p++ != '=') break;
        {
            int v = (int)strtol(p, (char **)&p, 10);
            if (at == f)
                for (i = 0; i < NSET; i++)
                    if (!strcmp(s_set[i].key, key)) { fprintf(stderr, "[PCSET] test at flip %u\n", f); pcs_set(i, v); }
        }
        if (*p == ',') p++;
    }
}

void pcs_tick(void)
{
    int i;
    init_vals();
    s_tick++;
    test_script();
    if (s_blk) {
        int *g = (int *)s_blk;
        for (i = 0; i < NSET; i++) {
            int v = g[i];
            if (s_set[i].kind == PCS_CHECK) v = (signed char)(v & 0xFF) != 0;  /* DT_S8 binding */
            if (v != s_last[i]) {          /* the page changed it */
                s_last[i] = v;
                pcs_set(i, v);
            }
            if (g[i] != s_val[i] || s_last[i] != s_val[i]) {   /* changed elsewhere (F11) or clamped */
                g[i] = s_last[i] = s_val[i];
            }
        }
    }
    /* save a moment after the last change (a slider held down changes every frame) */
    if (s_dirty && s_tick - s_dirty_tick > 45) {
        s_dirty = 0;
        save_ini();
    }
}
