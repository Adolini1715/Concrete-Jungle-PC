/*
 * Disk cache of linked GL programs (glGetProgramBinary / glProgramBinary).
 *
 * The port builds a GLSL program for every NV2A register-combiner setup x vertex program
 * the game uses and compiles it the first time a draw needs it. A compile + link costs a
 * few ms on NVIDIA and far more on AMD's OpenGL driver, so a new area stutters while its
 * programs are built (a player's report: RX 9060 XT). Here every linked program is kept
 * as the driver's own binary in shadercache/pcj-shaders.bin (next to pcj.exe when it runs
 * from its folder): a later session loads it instead of compiling, which takes well under
 * a millisecond. Vertex shaders that compiled once are remembered too, so they are not
 * compiled again unless a program actually has to be linked.
 *
 * The file starts with a hash of GL_VENDOR / GL_RENDERER / GL_VERSION: a new driver or GPU
 * starts a fresh file. A binary the driver refuses (glProgramBinary link status 0) is
 * compiled from source as before. Records are only ever appended; past 128 MB the file
 * starts over.
 *
 *   CJ_GL_SHADER_CACHE=0   off (compile everything from source, write nothing)
 *   CJ_GL_SHADER_CACHE_DIR=<dir>   where the cache lives (default: shadercache)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <epoxy/gl.h>
#include "shader_cache.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#define SEP "\\"
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#define SEP "/"
#endif

#define MAGIC "PCJSHC1"
#define MAX_FILE (128u << 20)

typedef struct { uint64_t key; uint32_t fmt, len; const uint8_t *data; } Rec;

static int s_on = -1;
static uint64_t s_driver;
static Rec *s_map;            /* open addressing, key 0 = empty */
static uint32_t s_cap, s_n;
static uint64_t *s_vs;        /* known-good vertex shader source hashes */
static uint32_t s_vcap, s_vn;
static uint8_t *s_blob;       /* the file as read at start (records point into it) */
static FILE *s_out;
static char s_path[1024];
static uint32_t s_hits, s_miss, s_bad, s_stored;
static long s_size;

uint64_t shc_hash(const char *a, const char *b)
{
    uint64_t h = 0xCBF29CE484222325ull;
    const unsigned char *p;
    for (p = (const unsigned char *)a; p && *p; p++) { h ^= *p; h *= 0x100000001B3ull; }
    h ^= 0xFF; h *= 0x100000001B3ull;
    for (p = (const unsigned char *)b; p && *p; p++) { h ^= *p; h *= 0x100000001B3ull; }
    return h ? h : 1;
}

static void map_put(uint64_t key, uint32_t fmt, uint32_t len, const uint8_t *data)
{
    uint32_t i;
    if (s_n * 2 >= s_cap) {
        uint32_t oc = s_cap, j;
        Rec *om = s_map;
        s_cap = oc ? oc * 2 : 4096;
        s_map = calloc(s_cap, sizeof *s_map);
        s_n = 0;
        for (j = 0; j < oc; j++) if (om[j].key) map_put(om[j].key, om[j].fmt, om[j].len, om[j].data);
        free(om);
    }
    for (i = (uint32_t)key & (s_cap - 1); s_map[i].key && s_map[i].key != key; i = (i + 1) & (s_cap - 1)) {}
    if (!s_map[i].key) s_n++;
    s_map[i].key = key; s_map[i].fmt = fmt; s_map[i].len = len; s_map[i].data = data;
}

static Rec *map_get(uint64_t key)
{
    uint32_t i;
    if (!s_cap) return NULL;
    for (i = (uint32_t)key & (s_cap - 1); s_map[i].key; i = (i + 1) & (s_cap - 1))
        if (s_map[i].key == key) return &s_map[i];
    return NULL;
}

static void vs_put(uint64_t h)
{
    uint32_t i;
    if (s_vn * 2 >= s_vcap) {
        uint32_t oc = s_vcap, j;
        uint64_t *om = s_vs;
        s_vcap = oc ? oc * 2 : 1024;
        s_vs = calloc(s_vcap, sizeof *s_vs);
        s_vn = 0;
        for (j = 0; j < oc; j++) if (om[j]) vs_put(om[j]);
        free(om);
    }
    for (i = (uint32_t)h & (s_vcap - 1); s_vs[i] && s_vs[i] != h; i = (i + 1) & (s_vcap - 1)) {}
    if (!s_vs[i]) { s_vs[i] = h; s_vn++; }
}

int shc_vs_known(uint64_t h)
{
    uint32_t i;
    if (s_on <= 0 || !s_vcap) return 0;
    for (i = (uint32_t)h & (s_vcap - 1); s_vs[i]; i = (i + 1) & (s_vcap - 1))
        if (s_vs[i] == h) return 1;
    return 0;
}

static void put_rec(uint8_t type, uint64_t key, uint32_t fmt, uint32_t len, const void *data)
{
    if (!s_out) return;
    if (s_size + 17 + (long)len > (long)MAX_FILE) return;
    fwrite(&type, 1, 1, s_out);
    fwrite(&key, 8, 1, s_out);
    fwrite(&fmt, 4, 1, s_out);
    fwrite(&len, 4, 1, s_out);
    if (len) fwrite(data, 1, len, s_out);
    fflush(s_out);
    s_size += 17 + (long)len;
}

void shc_vs_note(uint64_t h)
{
    if (s_on <= 0 || shc_vs_known(h)) return;
    vs_put(h);
    put_rec(2, h, 0, 0, NULL);
}

static void stats_at_exit(void)
{
    char b[200];
    shc_stats(b, sizeof b);
    fprintf(stderr, "[GL] %s\n", b);
}

void shc_init(const char *vendor, const char *renderer, const char *version)
{
    const char *e = getenv("CJ_GL_SHADER_CACHE"), *dir = getenv("CJ_GL_SHADER_CACHE_DIR");
    GLint nfmt = 0;
    FILE *f;
    long n = 0;
    char head[16];
    if (s_on >= 0) return;
    s_on = !(e && *e == '0');   /* on unless turned off (launcher > PC Settings > Shader cache) */
    if (!s_on) { fprintf(stderr, "[GL] shader cache off (CJ_GL_SHADER_CACHE=0)\n"); return; }
    if (!(epoxy_gl_version() >= 41 || epoxy_has_gl_extension("GL_ARB_get_program_binary"))) { s_on = 0; }
    else { glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nfmt); if (nfmt <= 0) s_on = 0; }
    if (!s_on) { fprintf(stderr, "[GL] shader cache: the driver offers no program binaries, off\n"); return; }
    {
        char d[1024];
        snprintf(d, sizeof d, "%s|%s|%s", vendor ? vendor : "", renderer ? renderer : "", version ? version : "");
        s_driver = shc_hash(d, MAGIC);
    }
    if (!dir || !*dir) dir = "shadercache";
    MKDIR(dir);
    snprintf(s_path, sizeof s_path, "%s" SEP "pcj-shaders.bin", dir);
    if ((f = fopen(s_path, "rb"))) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n >= 16 && n <= (long)MAX_FILE && fread(head, 1, 16, f) == 16 && !memcmp(head, MAGIC, 8)
            && !memcmp(head + 8, &s_driver, 8)) {
            s_blob = malloc((size_t)n - 16);
            if (s_blob && fread(s_blob, 1, (size_t)n - 16, f) == (size_t)n - 16) {
                long p = 0, m = n - 16;
                while (p + 17 <= m) {
                    uint8_t t = s_blob[p];
                    uint64_t key; uint32_t fmt, len;
                    memcpy(&key, s_blob + p + 1, 8); memcpy(&fmt, s_blob + p + 9, 4); memcpy(&len, s_blob + p + 13, 4);
                    if ((uint64_t)p + 17 + len > (uint64_t)m) break;   /* a record cut short (crash while writing) */
                    if (t == 1 && key && len) map_put(key, fmt, len, s_blob + p + 17);
                    else if (t == 2 && key) vs_put(key);
                    p += 17 + (long)len;
                }
                n = 16 + p;   /* append after the last whole record */
            } else n = 0;
        } else n = 0;
        fclose(f);
    }
    if (n >= 16) {
        s_out = fopen(s_path, "r+b");
        if (s_out) fseek(s_out, n, SEEK_SET);
        s_size = n;
    } else {
        s_out = fopen(s_path, "wb");
        if (s_out) { memcpy(head, MAGIC, 8); memcpy(head + 8, &s_driver, 8); fwrite(head, 1, 16, s_out); fflush(s_out); }
        s_size = 16;
    }
    atexit(stats_at_exit);
    fprintf(stderr, "[GL] shader cache %s: %u programs, %u vertex shaders ready%s\n", s_path, s_n, s_vn,
            s_out ? "" : " (cannot write, read only)");
}

int shc_on(void) { return s_on > 0; }

int shc_load(uint64_t key, unsigned prog)
{
    Rec *r;
    GLint ok = 0;
    if (s_on <= 0) return 0;
    if (!(r = map_get(key)) || !r->len) { s_miss++; return 0; }
    glProgramBinary(prog, r->fmt, r->data, (GLsizei)r->len);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { s_bad++; s_miss++; r->len = 0; return 0; }   /* refused: compiled and stored afresh */
    s_hits++;
    return 1;
}

void shc_prepare(unsigned prog)
{
    if (s_on > 0) glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
}

void shc_store(uint64_t key, unsigned prog)
{
    GLint len = 0;
    GLenum fmt = 0;
    void *buf;
    Rec *r = map_get(key);
    if (s_on <= 0 || !s_out || (r && r->len)) return;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || !(buf = malloc((size_t)len))) return;
    glGetProgramBinary(prog, len, NULL, &fmt, buf);
    put_rec(1, key, fmt, (uint32_t)len, buf);
    map_put(key, fmt, (uint32_t)len, buf);   /* buf stays: the map points at it */
    s_stored++;
}

/* In play, a new program's binary is read back ~2 s after it was linked, one per frame, instead of
 * right after the link: glGetProgramBinary waits for the driver to finish that program (some drivers
 * finish compiles on their own threads) and the record is written to disk, which in the frame that
 * first draws with it was a hitch of its own. A program deleted before then is dropped (its name
 * could be reused for another program). */
typedef struct { uint64_t key; unsigned prog; uint32_t due; } Pend;
static Pend s_pend[512];
static int s_npend;
static uint32_t s_frame;

void shc_store_later(uint64_t key, unsigned prog)
{
    if (s_on <= 0 || !s_out) return;
    if (s_npend < (int)(sizeof s_pend / sizeof *s_pend)) s_pend[s_npend++] = (Pend){key, prog, s_frame + 120};
}

void shc_forget(unsigned prog)
{
    int i;
    for (i = 0; i < s_npend; i++)
        if (s_pend[i].prog == prog) { s_pend[i] = s_pend[--s_npend]; i--; }
}

void shc_tick(void)
{
    int i;
    s_frame++;
    for (i = 0; i < s_npend; i++)
        if ((int32_t)(s_frame - s_pend[i].due) >= 0) {
            Pend p = s_pend[i];
            s_pend[i] = s_pend[--s_npend];
            shc_store(p.key, p.prog);
            return;   /* one per frame */
        }
}

void shc_stats(char *out, int n)
{
    snprintf(out, (size_t)n, "shader cache: %u loaded, %u compiled (%u refused binaries), %u stored", s_hits, s_miss,
             s_bad, s_stored);
}
