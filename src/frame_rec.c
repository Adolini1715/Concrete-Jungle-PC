/* frame_rec.c: record a few frames of back-end work and replay them without the game.
 *
 * Record: CJ_FRAME_REC=<dir> [CJ_FRAME_REC_AT=<flip>] [CJ_FRAME_REC_N=3] (no AT: when <dir>/go
 * appears). At flip AT the guest's
 * contiguous memory window (guest 0x80000000: textures, surfaces) goes to <dir>/ram.bin, then every clear, draw and flip
 * the GL back end receives for the next N flips goes to <dir>/frames.bin, after the native
 * takeover substituted its own data, so the file holds exactly what gl_backend.c drew.
 *
 * Replay: CJ_REPLAY=<dir> (with CJ_GL=1 and the usual CJ_GL_DUMP for frames). main.c calls
 * cj_frame_replay() once the memory window exists and before the game starts: it restores
 * ram.bin, feeds the records to the back end and exits. A replay depends only on the back end's
 * input, so a recording made by one build replays in any later build: renderer changes can be
 * checked against a captured scene (or one sent in from a player's PC) in seconds, without a boot.
 *
 * GL objects that earlier frames left behind (render-to-texture results, the texture cache) are
 * not in the file: record 2-3 frames and judge the last.
 *
 * File format (little endian): "PCJFR1\n\0", then records: a uint32 tag, then the payload.
 *   'C' surface, state, flags, argb, zs
 *   'D' surface, batch with every pointee written inline (presence word before each optional one)
 *   'F' g_nv2a_flip_front_va
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include "../xboxrecomp/src/kernel/nv2a_backend.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern unsigned int g_xbox_contig_size;
extern uint32_t g_nv2a_flip_front_va;
const Nv2aBackend *cj_gl_backend(void);   /* gl_backend.c */

#define MAGIC "PCJFR1\n"
enum { T_CLEAR = 'C', T_DRAW = 'D', T_FLIP = 'F', T_END = 'E' };

static FILE *s_f;
static int s_state;               /* 0 unknown, 1 armed, 2 recording, 3 done or off */
static uint32_t s_at, s_n, s_left, s_flip;
static char s_dir[512];

static void w(const void *p, size_t n) { if (s_f && n) fwrite(p, 1, n, s_f); }
static void w32(uint32_t v) { w(&v, 4); }
static void wopt(const void *p, size_t n) { w32(p != NULL); if (p) w(p, n); }

/* CJ_TEST_CAPTURE=1 (launcher: "Test captures"): record 3 flips by itself a few minutes into play,
 * into the session's log folder (logs/session-.../rec_N), for a tester who will not remember hotkeys.
 * The player sends the folder; tools/replay.sh replays it here. About 70 MB per capture. */
static const unsigned s_auto_secs[] = { 150, 420, 900 };
static unsigned s_auto_i;
static time_t s_auto_t0;
static int s_auto = -1;
const char *gamelog_dir(void);
static void auto_capture(void)
{
    char d[600];
    const char *g;
    if (s_auto < 0) { const char *e = getenv("CJ_TEST_CAPTURE"); s_auto = e && *e == '1' && !getenv("CJ_FRAME_REC"); s_auto_t0 = time(NULL); }
    if (!s_auto || s_state == 2 || s_auto_i >= sizeof s_auto_secs / sizeof s_auto_secs[0]) return;
    if ((unsigned)(time(NULL) - s_auto_t0) < s_auto_secs[s_auto_i] || !(g = gamelog_dir())) return;
    snprintf(d, sizeof d, "%s/rec_%u", g, s_auto_i + 1);
#ifdef _WIN32
    _mkdir(d);
#else
    mkdir(d, 0755);
#endif
    snprintf(s_dir, sizeof s_dir, "%s", d);
    s_n = 3; s_at = 0; s_state = 1;
    s_auto_i++;
}

static int arm(void)
{
    if (s_state == 0) {
        const char *d = getenv("CJ_FRAME_REC"), *a = getenv("CJ_FRAME_REC_AT"), *n = getenv("CJ_FRAME_REC_N");
        s_state = 3;
        if (d && *d) {   /* no CJ_FRAME_REC_AT: start when <dir>/go appears (route.py --press-at N rec) */
            snprintf(s_dir, sizeof s_dir, "%s", d);
            s_at = a ? (uint32_t)strtoul(a, NULL, 0) : 0xFFFFFFFFu;
            s_n = n ? (uint32_t)strtoul(n, NULL, 0) : 3;
            if (!s_n) s_n = 1;
            s_state = 1;
        }
    }
    return s_state == 2;
}

void cj_frec_flip_begin(void)
{
    arm();
    s_flip++;
    auto_capture();
    if (s_state == 1 && s_at == 0xFFFFFFFFu && s_flip % 4 == 0) {
        char g[600]; FILE *gf;
        snprintf(g, sizeof g, "%s/go", s_dir);
        if ((gf = fopen(g, "r"))) { fclose(gf); remove(g); s_at = s_flip; }
    }
    if (s_state == 1 && s_flip >= s_at) {
        char p[600];
        FILE *r;
        snprintf(p, sizeof p, "%s/ram.bin", s_dir);
        if ((r = fopen(p, "wb"))) {
            fwrite((const uint8_t *)xbox_GetMemoryOffset() + 0x80000000u, 1, g_xbox_contig_size, r);   /* the contiguous window at 0x80000000: textures, surfaces */
            fclose(r);
        }
        snprintf(p, sizeof p, "%s/frames.bin", s_dir);
        s_f = fopen(p, "wb");
        if (!s_f || !r) { fprintf(stderr, "[FREC] can't write in %s\n", s_dir); s_state = 3; return; }
        w(MAGIC "\0", 8);
        s_state = 2; s_left = s_n;
        fprintf(stderr, "[FREC] recording %u flips from flip %u into %s\n", s_n, s_flip, s_dir);
    }
}

void cj_frec_flip(void)
{
    if (!arm()) return;
    w32(T_FLIP); w32(g_nv2a_flip_front_va);
    if (--s_left == 0) {
        w32(T_END); fclose(s_f); s_f = NULL; s_state = 3;
        fprintf(stderr, "[FREC] done (%s)\n", s_dir);
    }
}

void cj_frec_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags, uint32_t argb, uint32_t zs)
{
    if (!arm()) return;
    w32(T_CLEAR); w(s, sizeof *s); w(rs, sizeof *rs); w32(flags); w32(argb); w32(zs);
}

static uint32_t popc(uint32_t v) { uint32_t n = 0; for (; v; v &= v - 1) n++; return n; }

void cj_frec_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    int i;
    if (!arm()) return;
    w32(T_DRAW); w(s, sizeof *s);
    w32(b->count);
    w32(b->vertices && !b->vsh); if (b->vertices && !b->vsh) w(b->vertices, sizeof *b->vertices * b->count);
    wopt(b->texture, sizeof *b->texture);
    wopt(b->state, sizeof *b->state);
    w32(b->ext && !b->vsh); if (b->ext && !b->vsh) w(b->ext, sizeof *b->ext * b->count);
    for (i = 0; i < 4; i++) wopt(b->stages[i], sizeof *b->stages[i]);
    wopt(b->rc, sizeof *b->rc);
    w32(b->fog_color); w32(b->clip_plane_mode); w32(b->vsh_twin);
    w32(b->vsh != NULL);
    if (b->vsh) {
        const Nv2aVshBatch *v = b->vsh;
        w32(v->ninstr); w(v->program, 16 * v->ninstr);
        w(v->constants, 16 * 192);
        w32(v->array_mask); w32(v->nverts);
        if (v->raw) {   /* raw guest vertex data: recorded as the float4 rows the executor would have made */
            extern void cj_raw_rows(const Nv2aVshBatch *v, float *out);
            size_t nf = 4 * (size_t)popc(v->array_mask) * v->nverts;
            float *rows = malloc(nf * sizeof *rows);
            if (rows) { cj_raw_rows(v, rows); w(rows, nf * sizeof *rows); free(rows); }
        } else
            w(v->verts, 16 * (size_t)popc(v->array_mask) * v->nverts);
        w(v->inputs, sizeof v->inputs);
        w(v->indices, 4 * (size_t)b->count);
        w(&v->aa_sx, 4); w(&v->aa_sy, 4); w(v->uv_scale, 8); w(&v->clip_z, 4);
        w32(v->fog_enable); w32(v->fog_mode); w(v->fog_param, 8);
    }
}

/* ---- replay ---- */
static FILE *s_r;
static int r(void *p, size_t n) { return !n || fread(p, 1, n, s_r) == n; }
static uint32_t r32(void) { uint32_t v = 0; r(&v, 4); return v; }
static void *ralloc(size_t n) { void *p = malloc(n ? n : 1); if (!r(p, n)) { free(p); return NULL; } return p; }
static void *ropt(size_t n) { return r32() ? ralloc(n) : NULL; }

int cj_frame_replay(void)
{
    const char *d = getenv("CJ_REPLAY");
    const Nv2aBackend *be;
    char p[600], m[8];
    FILE *ram;
    unsigned draws = 0, clears = 0, flips = 0;
    if (!d || !*d) return 0;
    be = cj_gl_backend();
    snprintf(p, sizeof p, "%s/ram.bin", d);
    if (!(ram = fopen(p, "rb"))) { fprintf(stderr, "[REPLAY] no %s\n", p); return 1; }
    fread((uint8_t *)xbox_GetMemoryOffset() + 0x80000000u, 1, g_xbox_contig_size, ram);
    fclose(ram);
    snprintf(p, sizeof p, "%s/frames.bin", d);
    if (!(s_r = fopen(p, "rb")) || !r(m, 8) || memcmp(m, MAGIC, 7)) { fprintf(stderr, "[REPLAY] bad %s\n", p); return 1; }
    for (;;) {
        uint32_t tag = r32();
        if (feof(s_r) || tag == T_END) break;
        if (tag == T_CLEAR) {
            Nv2aSurface s; Nv2aRenderState rs; uint32_t f, c, z;
            r(&s, sizeof s); r(&rs, sizeof rs); f = r32(); c = r32(); z = r32();
            be->clear(&s, &rs, f, c, z); clears++;
            if (getenv("CJ_REPLAY_LOG"))
                fprintf(stderr, "[REPLAY] clear surf %08X %ux%u flags %X argb %08X zs %08X\n", s.color_va, s.width, s.height, f, c, z);
        } else if (tag == T_FLIP) {
            g_nv2a_flip_front_va = r32();
            be->flip(); flips++;
            if (getenv("CJ_REPLAY_LOG")) fprintf(stderr, "[REPLAY] flip front %08X\n", g_nv2a_flip_front_va);
        } else if (tag == T_DRAW) {
            Nv2aSurface s; Nv2aBatch b; Nv2aVshBatch v; int i;
            void *tofree[16]; int nf = 0;
            memset(&b, 0, sizeof b); memset(&v, 0, sizeof v);
            r(&s, sizeof s);
            b.count = r32();
            b.vertices = ropt(sizeof(Nv2aVertex) * b.count);
            b.texture = ropt(sizeof(Nv2aTexture));
            b.state = ropt(sizeof(Nv2aRenderState));
            b.ext = ropt(sizeof(Nv2aVertexExt) * b.count);
            for (i = 0; i < 4; i++) b.stages[i] = ropt(sizeof(Nv2aTexture));
            b.rc = ropt(sizeof(Nv2aCombiner));
            b.fog_color = r32(); b.clip_plane_mode = r32(); b.vsh_twin = r32();
            tofree[nf++] = (void *)b.vertices; tofree[nf++] = (void *)b.texture; tofree[nf++] = (void *)b.state;
            tofree[nf++] = (void *)b.ext; for (i = 0; i < 4; i++) tofree[nf++] = (void *)b.stages[i];
            tofree[nf++] = (void *)b.rc;
            if (r32()) {
                v.ninstr = r32(); v.program = ralloc(16 * (size_t)v.ninstr);
                v.constants = ralloc(16 * 192);
                v.array_mask = r32(); v.nverts = r32();
                v.verts = ralloc(16 * (size_t)popc(v.array_mask) * v.nverts);
                r(v.inputs, sizeof v.inputs);
                v.indices = ralloc(4 * (size_t)b.count);
                r(&v.aa_sx, 4); r(&v.aa_sy, 4); r(v.uv_scale, 8); r(&v.clip_z, 4);
                v.fog_enable = r32(); v.fog_mode = r32(); r(v.fog_param, 8);
                b.vsh = &v;
                tofree[nf++] = (void *)v.program; tofree[nf++] = (void *)v.constants;
                tofree[nf++] = (void *)v.verts; tofree[nf++] = (void *)v.indices;
            }
            if (feof(s_r)) break;
            if (b.vsh && be->vsh_accept) {   /* the back end picks its program here, as live */
                uint32_t in[16];
                be->vsh_accept(v.program, v.ninstr, 0, in);
            }
            be->draw(&s, &b); draws++;
            if (getenv("CJ_REPLAY_LOG"))
                fprintf(stderr, "[REPLAY] draw %u surf %08X %ux%u count %u vsh %d tex %08X zeta %08X ztest %u blend %u cmask %08X\n",
                        draws, s.color_va, s.width, s.height, b.count, b.vsh != NULL, b.texture ? b.texture->offset : 0,
                        b.state ? b.state->zeta_va : 0, b.state ? b.state->depth_test_enable : 0,
                        b.state ? b.state->blend_enable : 0, b.state ? b.state->color_mask : 0);
            for (i = 0; i < nf; i++) free(tofree[i]);
        } else {
            fprintf(stderr, "[REPLAY] bad record tag %u\n", tag);
            break;
        }
    }
    fclose(s_r);
    fprintf(stderr, "[REPLAY] %s: %u clears, %u draws, %u flips\n", d, clears, draws, flips);
    { void cj_gl_filter_stats(void); cj_gl_filter_stats(); }
    return 1;
}
