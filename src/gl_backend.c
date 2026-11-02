/*
 * OpenGL 3.3 render back end for the toolkit's pushbuffer executor
 * (xboxrecomp v0.15 nv2a_backend.h + patches/v0.15/0007). Opt-in: CJ_GL=1
 * (with RECOMP_PB_EXEC=1).
 *
 * The executor still decodes the pushbuffer; vertex programs run here as
 * generated GLSL (patch 0008, gen_vs: CJ_GL_VSH=0 for the CPU interpreter,
 * CJ_GL_VSH_AB=1 draws both for comparison), other batches arrive
 * transformed and clipped by the CPU. This back end replaces the CPU rasteriser: triangles in surface
 * pixels -> GL, one FBO per colour surface, blend/depth/alpha/stencil/cull/
 * colour mask as the title set them (NV2A uses the GL enum values).
 *
 * Pixel pipeline (docs/gl-backend.md):
 *  - render targets as textures: a stage whose guest address lies inside a
 *    surface the GPU drew recently samples that surface's FBO texture
 *    (sub-rect offset from the address, swizzled or linear, no alpha for
 *    R5G6B5/X8R8G8B8) instead of decoding guest memory the GPU never wrote;
 *  - texture stages 0-3 and the register combiners: one GLSL program per
 *    combiner configuration (ICW/OCW/final words, stage program, clip-plane
 *    mode, which stages are bound), generated from the rules in the toolkit's
 *    nv2a_combiner.c and cached; constants are uniforms.
 *
 *   CJ_GL_DUMP=<prefix>  write each presented frame to <prefix>NNNNN.ppm
 *   CJ_GL_DUMP_EVERY=n   only every nth flip (default 1)
 *   CJ_GL_TEXCACHE=0     decode the texture on every batch
 *   CJ_GL_RTT=0          never sample render targets (old behaviour)
 *   CJ_GL_RC=0           no combiners: stage 0 x diffuse (old behaviour)
 *   CJ_GL_DBG=1|2|3      depth test off for non-depth-writing draws | cull off | both
 *   CJ_GL_SURF_DIAG=n    per-surface draw counts every nth flip
 *   CJ_GL_DRAWLOG=<flip> one line per draw in that flip
 *   CJ_GL_SHADER_LOG=1   print each generated combiner shader
 */
#include <epoxy/gl.h>
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#include "keymap.h"
#include "focusdiag.h"
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../xboxrecomp/src/kernel/nv2a_backend.h"
#include "../xboxrecomp/src/d3d/d3d8_swizzle.h"
#include "pcsettings.h"
#include "quicksave.h"
#include <stdarg.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

void gamelog_init(const char *, const char *, const char *);
void gamelog_frame(uint32_t, uint64_t, uint64_t, const void *);
void gamelog_sdl_event(const void *);
void gamelog_event(const char *type, const char *fmt, ...);
int gamelog_shot_pending(void);
int gamelog_burst_frame(void);
static uint32_t s_burst_until;   /* F9 burst: flips below this log their surfaces, the first 2 their draws */
void gamelog_save_shot(const uint8_t *rgba_bottom_up, int w, int h);

static double s_be_time, s_swap_time, s_upload_time;
static uint64_t s_upload_n;            /* seconds inside the back end (draw/clear/flip) since the last [FPS] line */
static SDL_Window *s_win;
static SDL_GLContext s_ctx;
static int s_failed, s_ready;
static GLuint s_vao, s_vbo, s_vbo_ext, s_tex;
static uint32_t *s_texbuf;
static const char *s_dump;
static int s_texcache = 1, s_rtt = 1, s_rc_on = 1;
static unsigned s_dump_every = 1;
/* CJ_GL_NODRAW=1: every GL call but the draws (a stand-in for an infinitely
 * fast GPU: what is left is this process's own CPU cost per frame). */
static int s_nodraw;
static int s_ab, s_side;     /* CJ_GL_VSH_AB: side 0 GPU-VS, side 1 CPU-VS */
/* CJ_GL_SCALE=2..5: internal render resolution. Every surface FBO is allocated
 * at S x its guest size and drawn with an S x viewport; everything that names
 * guest pixels (scissor, window clip, copies, the present) scales by S, while
 * texture coordinates into render targets stay normalised by the guest size,
 * so sampling a target needs no change. */
static int s_rs = 1;
/* CJ_WIDESCREEN=1: the title's own anamorphic 16:9 mode (it renders a wider
 * view into the same 640x480 surfaces and scales its HUD for it; the port sets
 * EXBaseRenderEnv::ms_WideScreen, recomp_manual.c), presented 16:9. */
int cj_widescreen(void)
{
    return pcs_get(PCS_WIDESCREEN);     /* CJ_WIDESCREEN, or Options > PC Settings (live) */
}

typedef struct {
    uint32_t va, w, h, pitch, bpp, cw, ch;
    GLuint fbo2[2], color2[2], depth2[2];  /* [1]: CPU-VS side, CJ_GL_VSH_AB only */
    uint64_t used;
    uint32_t drawn_flip;
    uint32_t nd_flip, nd_legacy, nd_rtt;   /* this flip: draws, legacy-path draws, draws sampling an RT */
    uint32_t order;                        /* order of first draw this flip */
    uint32_t last_bind;                    /* order of the latest bind this flip */
    uint32_t fcw, fch, pcw, pch;           /* biggest clip this flip; area presented */
    uint32_t zeta;                         /* zeta surface of the latest draw here */
    int xa;                                /* alpha the FBO holds everywhere (0, 1), or -1: drawn alpha */
} Rt;
#define NRT 32
static Rt s_rt[NRT];
static int s_nrt;
static uint64_t s_tick;
static Rt *s_last;          /* surface drawn to most recently */
static uint32_t s_flips;
static uint64_t s_draws, s_tris, s_tex_decodes, s_rtt_binds, s_rc_draws;
static double s_t0;

typedef struct { uint32_t off, color, w, h, levels; uint64_t hash, shash; GLuint tex; uint64_t used; int next, rm; } TexEnt;
/* Decoded-texture cache. It was 256 entries with a linear search: a scene
 * binding more distinct textures than that per frame evicted and re-decoded
 * them every frame (thousands of uploads a second, the frame-rate dips in
 * dense areas). Now 4096 entries found through a hash of the key. */
#define NTEX 4096
#define NTEX_BUCKETS 8192
static TexEnt s_tc[NTEX];
static int s_tc_head[NTEX_BUCKETS];
static int s_tc_init;
static uint64_t s_tc_evictions;
static unsigned tc_bucket(uint32_t off, uint32_t color, uint32_t w, uint32_t h, uint32_t nl)
{
    uint32_t k = off * 0x9E3779B1u ^ (color * 0x85EBCA77u) ^ (w * 0xC2B2AE3Du) ^ (h << 16) ^ nl;
    k ^= k >> 15; k *= 0x2C1B3C6Du; k ^= k >> 12;
    return k & (NTEX_BUCKETS - 1);
}
static uint64_t s_mip_levels, s_mip_textures;
static float s_aniso_max;       /* 0: no anisotropic filtering extension */
static int s_aniso_force = -1;  /* CJ_GL_ANISO: -1 title's setting, else this (1..16) */
static float s_lod_bias_add;    /* CJ_GL_LOD_BIAS: added to the title's bias */
static int s_mips_on = 1;
static long s_diag_at = -1;    /* CJ_GL_DIAG_AT=<flip>: flips +1..+4 show one combiner input each (dumped) */
static long s_dump_at = -1;    /* CJ_GL_DUMP_AT=<flip>: also dump that flip and the next five */
static int s_wbuf_on = 1;      /* CJ_GL_WBUF=0: z-buffer depth even when CONTROL0 asks for W */
static uint32_t s_wbuf_draws;  /* draws with W-buffering on, since the last [GL] tex: line */
static int s_fog_argb;          /* CJ_GL_FOG_ARGB=1: fog colour as ARGB (diagnostic) */
static int s_clipsc_on = 1;      /* CJ_GL_CLIPSCISSOR=0: ignore the surface clip origin/size */
static int s_wclip_on = 1, s_polyoff_on = 1, s_depthq_on = 1;
static int s_fogpix_on = 1;     /* CJ_GL_FOGPIX=0: clamp fog per vertex (the old way) instead of per pixel */
static int s_flk = -1;          /* flicker capture (CJ_FLICKER=1 on), see ring_capture */
static int s_s3tc;                 /* DXT uploaded compressed (see upload_dxt) */
static uint64_t s_s3tc_uploads;
static int s_texhash_full = 1;  /* CJ_GL_TEXHASH=0: sampled texture change check (the old one) */
static uint64_t s_wclip_draws, s_polyoff_draws;       /* CJ_GL_MIPS=0: level 0 only (the old behaviour) */

uint32_t cj_gl_flips(void) { return s_flips; }

/* Host input (real controller + keyboard), sampled on this thread after the
 * SDL event pump in gl_flip and read by src/cj_pad.c's XInputGetState wrapper
 * when CJ_PAD_HOST=1 (the Windows default). Xbox layout: dig = XINPUT
 * digital bits (up down left right start back lthumb rthumb), ana[8] = A B X
 * Y black white LT RT (0-255), sticks in Xbox sign (up = +). */
typedef struct { uint16_t dig; uint8_t ana[8]; int16_t lx, ly, rx, ry; } CjHostPad;
volatile CjHostPad cj_host_pad;
static SDL_GameController *s_pad;
/* Rumble from the game (src/cj_pad.c, XInputSetState): motor speeds 0..65535. The game sets 0 to stop, so the
 * duration only bounds a lost stop. CJ_RUMBLE=0 turns it off; CJ_RUMBLE=<percent> scales it. */
/* a timed rumble from the port itself (charge feedback, src/native/hybrid/cjmod_move.c); CJ_RUMBLE=0 mutes it too */
void cj_host_rumble_timed(uint16_t lo, uint16_t hi, uint32_t ms)
{
    const char *e = getenv("CJ_RUMBLE");
    if (!s_pad || (e && atoi(e) == 0)) return;
    SDL_GameControllerRumble(s_pad, lo, hi, ms);
}
void cj_host_rumble(uint16_t lo, uint16_t hi)
{
    static int pct = -1;
    if (pct < 0) { const char *e = getenv("CJ_RUMBLE"); pct = e ? atoi(e) : 100; if (pct < 0) pct = 0; if (pct > 200) pct = 200; }
    if (!s_pad || !pct) return;
    unsigned l = (unsigned)lo * (unsigned)pct / 100u, h = (unsigned)hi * (unsigned)pct / 100u;
    SDL_GameControllerRumble(s_pad, (Uint16)(l > 65535u ? 65535u : l), (Uint16)(h > 65535u ? 65535u : h), (l || h) ? 5000 : 0);
}
static int s_fullscreen;
static int s_win_x, s_win_y, s_win_w, s_win_h;   /* the windowed rectangle, kept while borderless */
/* "Fullscreen" is a plain borderless window over the whole display, not SDL's fullscreen mode: that one
 * is marked topmost and exclusive-looking, and the Snipping Tool and Alt+Tab fight it.
 * The window is also one pixel taller than the display, with the extra row just above the top edge (the
 * frame is drawn bottom-aligned, so that row is the empty one). A window that exactly matches the
 * monitor gets promoted by Windows / the GL driver to direct scanout ("independent flip"), and every
 * overlay (Snipping Tool, Alt+Tab, volume popup) then forces a switch back to composition: a visible
 * flicker, and with HDR on a brightness jump. Off by one pixel, it stays composed and nothing switches. */
static void set_borderless_fs(SDL_Window *w, int on)
{
    if (on) {
        SDL_Rect b;
        int d = SDL_GetWindowDisplayIndex(w);
        if (d < 0) d = 0;
        if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_BORDERLESS)) {
            SDL_GetWindowPosition(w, &s_win_x, &s_win_y);
            SDL_GetWindowSize(w, &s_win_w, &s_win_h);
        }
        if (SDL_GetDisplayBounds(d, &b) != 0) return;
        SDL_SetWindowResizable(w, SDL_FALSE);
        SDL_SetWindowBordered(w, SDL_FALSE);
        SDL_SetWindowPosition(w, b.x, b.y - 1);
        SDL_SetWindowSize(w, b.w, b.h + 1);
    } else {
        SDL_SetWindowBordered(w, SDL_TRUE);
        SDL_SetWindowResizable(w, SDL_TRUE);
        if (s_win_w > 0) { SDL_SetWindowSize(w, s_win_w, s_win_h); SDL_SetWindowPosition(w, s_win_x, s_win_y); }
    }
}
static double s_fps_t; static uint32_t s_fps_f;

static double now_s(void)
{
    return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

/* ---- shaders ----------------------------------------------------------- */

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos;\n"     /* x, y, z, rhw */
    "layout(location=1) in vec4 a_col;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in vec4 a_spec;\n"
    "layout(location=4) in float a_fog;\n"
    "layout(location=5) in vec4 a_tc0;\n"
    "layout(location=6) in vec4 a_tc1;\n"
    "layout(location=7) in vec4 a_tc2;\n"
    "layout(location=8) in vec4 a_tc3;\n"
    "uniform vec2 u_size; uniform float u_zscale;\n"
    "invariant gl_Position;\n"
    "out vec4 v_col; out vec2 v_uv; out vec4 v_spec; out float v_fog;\n"
    "out vec4 v_tc0, v_tc1, v_tc2, v_tc3;\n"
    "void main(){\n"
    "  float w = a_pos.w > 0.0 ? 1.0 / a_pos.w : 1.0;\n"
    "  vec3 n = vec3(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0,\n"
    "                clamp(a_pos.z / u_zscale, 0.0, 1.0) * 2.0 - 1.0);\n"
    "  gl_Position = vec4(n * w, w);\n"
    "  v_col = a_col; v_uv = a_uv; v_spec = a_spec; v_fog = a_fog;\n"
    "  v_tc0 = a_tc0; v_tc1 = a_tc1; v_tc2 = a_tc2; v_tc3 = a_tc3;\n"
    "}\n";

static const char *FS_HEAD =
    "#version 330 core\n"
    "in vec4 v_col; in vec2 v_uv; in vec4 v_spec; in float v_fog;\n"
    "in vec4 v_tc0, v_tc1, v_tc2, v_tc3;\n"
    "out vec4 o;\n"
    "uniform sampler2D u_t0, u_t1, u_t2, u_t3;\n"
    "uniform vec4 u_tx[4];\n"         /* texel -> GL coords: scale.xy, offset.zw */
    "uniform vec4 u_tn;\n"            /* per stage: coords normalised (1) or texels (0) */
    "uniform vec4 u_aone;\n"          /* per stage: format has no alpha */
    "uniform vec4 u_twh[4];\n"        /* guest texture size */
    "uniform int u_textured;\n"
    "uniform int u_alpha_on; uniform int u_alpha_func; uniform float u_alpha_ref;\n"
    "uniform vec4 u_c0[8]; uniform vec4 u_c1[8]; uniform vec4 u_fc0, u_fc1; uniform vec3 u_fogc;\n"
    "vec4 samp(sampler2D s, int i, vec2 uv) {\n"
    "  vec2 t = uv * (u_tn[i] != 0.0 ? u_twh[i].xy : vec2(1.0));\n"
    "  vec4 c = texture(s, t * u_tx[i].xy + u_tx[i].zw);\n"
    "  if (u_aone[i] != 0.0) c.a = 1.0;\n"
    "  return c;\n"
    "}\n"
    "uniform int u_wc_n; uniform int u_wc_excl; uniform vec4 u_wc[8];\n"
    "uniform float u_zscale; uniform int u_wbuf; uniform int u_diag; uniform int u_dq; uniform vec2 u_po;\n"
    "void atest(float a0) {\n"
    /* NV2A W-buffering (CONTROL0 Z_PERSPECTIVE_ENABLE): the depth is the
     * perspective-correct w (in depth-buffer units, as D3D scales it), not z.
     * 1/gl_FragCoord.w is that w (gl_Position.w = the vertex's w). Every
     * fragment writes gl_FragDepth, so z-buffered draws pass gl_FragCoord.z. */
    /* u_dq (CJ_GL_DEPTHQ, default on) does what the hardware does: the depth
     * value in depth-buffer units, plus the polygon offset (bias in LSBs,
     * factor times the depth slope; GL's own glPolygonOffset is ignored
     * once a shader writes gl_FragDepth), floored to the integer the zeta
     * surface stores. A 24-bit value d is encoded as bits(d / 2^24) + 1,
     * which lands on exactly d / (2^24 - 1); a plain division rounds. */
    "  if (u_dq != 0 && (u_wbuf != 0 || u_po != vec2(0.0))) {\n"
    "    float zv = u_wbuf != 0 ? 1.0 / gl_FragCoord.w : gl_FragCoord.z * u_zscale;\n"
    "    if (u_po != vec2(0.0)) zv += u_po.x + u_po.y * max(abs(dFdx(zv)), abs(dFdy(zv)));\n"
    "    zv = floor(clamp(zv, 0.0, u_zscale));\n"
    "    gl_FragDepth = u_zscale == 16777215.0 ? uintBitsToFloat(floatBitsToUint(zv / 16777216.0) + 1u) : zv / u_zscale;\n"
    "  } else\n"
    "    gl_FragDepth = u_wbuf != 0 ? clamp(1.0 / (gl_FragCoord.w * u_zscale), 0.0, 1.0) : gl_FragCoord.z;\n"
    "  if (u_wc_n > 0) {\n"          /* NV2A window clip (SetScissors), GL window pixels */
    "    bool inr = false;\n"
    "    for (int i = 0; i < 8; i++) {\n"
    "      if (i >= u_wc_n) break;\n"
    "      vec4 r = u_wc[i];\n"
    "      if (gl_FragCoord.x >= r.x && gl_FragCoord.x < r.z && gl_FragCoord.y >= r.y && gl_FragCoord.y < r.w) inr = true;\n"
    "    }\n"
    "    if (inr == (u_wc_excl != 0)) discard;\n"
    "  }\n"
    "  if (u_alpha_on != 0) {\n"
    "    float a = floor(clamp(a0, 0.0, 1.0) * 255.0 + 0.5), r = u_alpha_ref; bool p = true;\n"
    "    if (u_alpha_func == 0) p = false; else if (u_alpha_func == 1) p = a < r;\n"
    "    else if (u_alpha_func == 2) p = a == r; else if (u_alpha_func == 3) p = a <= r;\n"
    "    else if (u_alpha_func == 4) p = a > r; else if (u_alpha_func == 5) p = a != r;\n"
    "    else if (u_alpha_func == 6) p = a >= r;\n"
    "    if (!p) discard;\n"
    "  }\n"
    "}\n";

static const char *FS_LEGACY_BODY =
    "void main(){\n"
    "  vec4 c = v_col;\n"
    "  if (u_textured != 0) c *= samp(u_t0, 0, v_uv);\n"
    "  atest(c.a);\n"
    "  o = c;\n"
    "}\n";

typedef struct {
    uint32_t color_icw[8], alpha_icw[8], color_ocw[8], alpha_ocw[8];
    uint32_t final0, final1, control, stage_program, clip_plane_mode, valid;
} RcKey;

typedef struct {
    GLuint prog;
    GLint u_size, u_zscale, u_wbuf, u_diag, u_dq, u_po, u_t[4], u_tx, u_tn, u_aone, u_twh, u_textured;
    GLint u_alpha_on, u_alpha_func, u_alpha_ref, u_c0, u_c1, u_fc0, u_fc1, u_fogc;
    GLint u_aa, u_uvs, u_clipz, u_fogm, u_fogp;      /* GPU vertex programs */
    GLint u_wc_n, u_wc_excl, u_wc;                   /* window clip */
} Prog;

/* vsh: index into s_vsh (a translated vertex program) or -1 for the fixed
 * screen-space VS; legacy: stage 0 x diffuse instead of the combiners. */
typedef struct { RcKey key; int vsh, legacy; Prog p; int used; } ProgEnt;
#define NPROG 512
static ProgEnt s_progs[NPROG];
static int s_nprog;
static Prog s_legacy, *s_cur;

static GLuint sh(GLenum t, const char *src)
{
    GLuint s = glCreateShader(t);
    GLint ok;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[GL] shader: %s\n%s\n", log, src);
    }
    return s;
}

static GLuint s_vs;

static int link_prog(Prog *p, GLuint vs, const char *fs)
{
    GLint ok;
    GLuint f = sh(GL_FRAGMENT_SHADER, fs);
    memset(p, 0, sizeof *p);
    p->prog = glCreateProgram();
    glAttachShader(p->prog, vs);
    glAttachShader(p->prog, f);
    glLinkProgram(p->prog);
    glDeleteShader(f);
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        fprintf(stderr, "[GL] link: %s\n", log);
        return 0;
    }
#define U(n) p->n = glGetUniformLocation(p->prog, #n)
    U(u_size); U(u_zscale); U(u_wbuf); U(u_diag); U(u_dq); U(u_po); U(u_tx); U(u_tn); U(u_aone); U(u_twh); U(u_textured);
    U(u_alpha_on); U(u_alpha_func); U(u_alpha_ref); U(u_c0); U(u_c1); U(u_fc0); U(u_fc1); U(u_fogc);
    U(u_aa); U(u_uvs); U(u_clipz); U(u_fogm); U(u_fogp);
    U(u_wc_n); U(u_wc_excl); U(u_wc);
#undef U
    {
        GLuint bi = glGetUniformBlockIndex(p->prog, "VshC");
        if (bi != GL_INVALID_INDEX)
            glUniformBlockBinding(p->prog, bi, 0);
    }
    p->u_t[0] = glGetUniformLocation(p->prog, "u_t0");
    p->u_t[1] = glGetUniformLocation(p->prog, "u_t1");
    p->u_t[2] = glGetUniformLocation(p->prog, "u_t2");
    p->u_t[3] = glGetUniformLocation(p->prog, "u_t3");
    glUseProgram(p->prog);
    for (ok = 0; ok < 4; ok++)
        glUniform1i(p->u_t[ok], ok);
    return 1;
}

/* ---- register combiners -> GLSL (rules: toolkit nv2a_combiner.c) ------- */

typedef struct { char *b; size_t n, cap; } Str;

static void sp(Str *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sp(Str *s, const char *fmt, ...)
{
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(s->b + s->n, s->cap - s->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && s->n + (size_t)k < s->cap) { s->n += (size_t)k; return; }
        s->cap = s->cap * 2 + 4096;
        s->b = realloc(s->b, s->cap);
    }
}

/* The input mapping (PS_INPUTMAPPING) of `x` (a float or vec3 expression). */
static void map_in(Str *s, const char *x, uint32_t byte, int vec)
{
    const char *z = vec ? "vec3(0.0)" : "0.0";
    switch ((byte >> 5) & 7) {
    case 0: sp(s, "max(%s,%s)", x, z); break;
    case 1: sp(s, "(1.0-clamp(%s,0.0,1.0))", x); break;
    case 2: sp(s, "(2.0*max(%s,%s)-1.0)", x, z); break;
    case 3: sp(s, "(-2.0*max(%s,%s)+1.0)", x, z); break;
    case 4: sp(s, "(max(%s,%s)-0.5)", x, z); break;
    case 5: sp(s, "(-max(%s,%s)+0.5)", x, z); break;
    case 6: sp(s, "(%s)", x); break;
    default: sp(s, "(-(%s))", x); break;
    }
}

static void read_rgb(Str *s, uint32_t byte)
{
    char x[32];
    byte &= 0xFF;
    snprintf(x, sizeof x, (byte & 0x10) ? "R[%u].aaa" : "R[%u].rgb", byte & 0xF);
    map_in(s, x, byte, 1);
}

static void read_alpha(Str *s, uint32_t byte)
{
    char x[32];
    byte &= 0xFF;
    snprintf(x, sizeof x, (byte & 0x10) ? "R[%u].a" : "R[%u].b", byte & 0xF);
    map_in(s, x, byte, 0);
}

static const char *map_out(uint32_t m)
{
    switch (m) {
    case 0x08: return "(%s-0.5)";
    case 0x10: return "(%s*2.0)";
    case 0x18: return "((%s-0.5)*2.0)";
    case 0x20: return "(%s*4.0)";
    case 0x30: return "(%s*0.5)";
    default:   return "(%s)";
    }
}

static char *gen_fs(const RcKey *k)
{
    Str s = { NULL, 0, 0 };
    uint32_t n = k->control & 0xFF, flags = k->control >> 8, st, i;
    char tmp[64];
    if (n > 8) n = 8;
    sp(&s, "%s", FS_HEAD);
    sp(&s, "void main(){\n  vec4 R[16];\n  for (int i = 0; i < 16; i++) R[i] = vec4(0.0);\n");
    sp(&s, "  R[3] = vec4(u_fogc, clamp(v_fog, 0.0, 1.0));\n  R[4] = v_col;\n  R[5] = v_spec;\n");
    /* texture stages */
    for (st = 0; st < 4; st++) {
        uint32_t mode = (k->stage_program >> (st * 5)) & 0x1F;
        sp(&s, "  {\n    vec4 tc = v_tc%u;\n", st);
        switch (mode) {
        case 0:
            sp(&s, "    R[%u] = vec4(0.0, 0.0, 0.0, 1.0);\n", 8 + st);
            break;
        case 1: case 2:
            if (k->valid & (1u << st))
                sp(&s, "    float q = tc.w != 0.0 ? tc.w : 1.0;\n"
                       "    R[%u] = samp(u_t%u, %u, tc.xy / q);\n", 8 + st, st, st);
            else
                sp(&s, "    R[%u] = vec4(1.0);\n", 8 + st);
            break;
        case 3:     /* cube map: not bound as a cube; ponytail */
            sp(&s, "    R[%u] = vec4(1.0);\n", 8 + st);
            break;
        case 4:
            sp(&s, "    R[%u] = clamp(tc, 0.0, 1.0);\n", 8 + st);
            break;
        case 5:
            for (i = 0; i < 4; i++) {
                int ge = (k->clip_plane_mode >> (st * 4 + i)) & 1;
                sp(&s, "    if (tc[%u] %s 0.0) discard;\n", i, ge ? ">=" : "<");
            }
            sp(&s, "    R[%u] = vec4(0.0);\n", 8 + st);
            break;
        default:    /* bump-env / dot-product modes */
            sp(&s, "    R[%u] = vec4(0.0);\n", 8 + st);
            break;
        }
        sp(&s, "  }\n");
    }
    sp(&s, "  R[12].a = %s;\n", (k->stage_program & 0x1F) ? "R[8].a" : "1.0");
    for (st = 0; st < n; st++) {
        uint32_t icw = k->color_icw[st], ocw = k->color_ocw[st];
        uint32_t aicw = k->alpha_icw[st], aocw = k->alpha_ocw[st];
        uint32_t fl = ocw >> 12, afl = aocw >> 12;
        const char *mo = map_out(fl & 0x38), *amo = map_out(afl & 0x38);
        sp(&s, "  {\n    R[1] = u_c0[%u]; R[2] = u_c1[%u];\n",
           (flags & 0x010) ? st : 0, (flags & 0x100) ? st : 0);
        if (flags & 1)
            sp(&s, "    bool mux = R[12].a >= 0.5;\n");
        else
            sp(&s, "    bool mux = (int(R[12].a * 255.0) & 1) != 0;\n");
        sp(&s, "    vec3 A = "); read_rgb(&s, icw >> 24); sp(&s, ";\n");
        sp(&s, "    vec3 B = "); read_rgb(&s, icw >> 16); sp(&s, ";\n");
        sp(&s, "    vec3 C = "); read_rgb(&s, icw >> 8);  sp(&s, ";\n");
        sp(&s, "    vec3 D = "); read_rgb(&s, icw);       sp(&s, ";\n");
        sp(&s, "    float aA = "); read_alpha(&s, aicw >> 24); sp(&s, ";\n");
        sp(&s, "    float aB = "); read_alpha(&s, aicw >> 16); sp(&s, ";\n");
        sp(&s, "    float aC = "); read_alpha(&s, aicw >> 8);  sp(&s, ";\n");
        sp(&s, "    float aD = "); read_alpha(&s, aicw);       sp(&s, ";\n");
        sp(&s, "    vec3 pab = %s;\n", (fl & 2) ? "vec3(dot(A, B))" : "A * B");
        sp(&s, "    vec3 pcd = %s;\n", (fl & 1) ? "vec3(dot(C, D))" : "C * D");
        sp(&s, "    vec3 sm = %s;\n", (fl & 4) ? "(mux ? pcd : pab)" : "pab + pcd");
        sp(&s, "    vec3 ab = clamp("); sp(&s, mo, "pab"); sp(&s, ", -1.0, 1.0);\n");
        sp(&s, "    vec3 cd = clamp("); sp(&s, mo, "pcd"); sp(&s, ", -1.0, 1.0);\n");
        sp(&s, "    vec3 ms = clamp("); sp(&s, mo, "sm"); sp(&s, ", -1.0, 1.0);\n");
        sp(&s, "    float apab = aA * aB, apcd = aC * aD;\n");
        sp(&s, "    float asum = %s;\n", (afl & 4) ? "(mux ? apcd : apab)" : "apab + apcd");
        sp(&s, "    float aab = clamp("); sp(&s, amo, "apab"); sp(&s, ", -1.0, 1.0);\n");
        sp(&s, "    float acd = clamp("); sp(&s, amo, "apcd"); sp(&s, ", -1.0, 1.0);\n");
        sp(&s, "    float ams = clamp("); sp(&s, amo, "asum"); sp(&s, ", -1.0, 1.0);\n");
        if ((ocw >> 4) & 0xF) {
            sp(&s, "    R[%u].rgb = ab;\n", (ocw >> 4) & 0xF);
            if (fl & 0x80) sp(&s, "    R[%u].a = ab.b;\n", (ocw >> 4) & 0xF);
        }
        if (ocw & 0xF) {
            sp(&s, "    R[%u].rgb = cd;\n", ocw & 0xF);
            if (fl & 0x40) sp(&s, "    R[%u].a = cd.b;\n", ocw & 0xF);
        }
        if ((ocw >> 8) & 0xF)
            sp(&s, "    R[%u].rgb = ms;\n", (ocw >> 8) & 0xF);
        if ((aocw >> 4) & 0xF) sp(&s, "    R[%u].a = aab;\n", (aocw >> 4) & 0xF);
        if (aocw & 0xF)        sp(&s, "    R[%u].a = acd;\n", aocw & 0xF);
        if ((aocw >> 8) & 0xF) sp(&s, "    R[%u].a = ams;\n", (aocw >> 8) & 0xF);
        sp(&s, "    R[0] = vec4(0.0);\n  }\n");
    }
    if (k->final0 || k->final1) {
        uint32_t f0 = k->final0, f1 = k->final1, ff = f1 & 0xFF;
        sp(&s, "  R[1] = u_fc0; R[2] = u_fc1;\n");
        snprintf(tmp, sizeof tmp, "%s + %s", (ff & 0x40) ? "(1.0 - R[5].rgb)" : "R[5].rgb",
                 (ff & 0x20) ? "(1.0 - R[12].rgb)" : "R[12].rgb");
        if (ff & 0x80)
            sp(&s, "  R[14] = vec4(clamp(%s, 0.0, 1.0), 0.0);\n", tmp);
        else
            sp(&s, "  R[14] = vec4(%s, 0.0);\n", tmp);
        sp(&s, "  { vec3 E = "); read_rgb(&s, f1 >> 24); sp(&s, ";\n");
        sp(&s, "    vec3 F = "); read_rgb(&s, f1 >> 16); sp(&s, ";\n");
        sp(&s, "    R[15] = vec4(E * F, 0.0); }\n");
        sp(&s, "  vec3 A = "); read_rgb(&s, f0 >> 24); sp(&s, ";\n");
        sp(&s, "  vec3 B = "); read_rgb(&s, f0 >> 16); sp(&s, ";\n");
        sp(&s, "  vec3 C = "); read_rgb(&s, f0 >> 8);  sp(&s, ";\n");
        sp(&s, "  vec3 D = "); read_rgb(&s, f0);       sp(&s, ";\n");
        sp(&s, "  float G = "); read_alpha(&s, f1 >> 8); sp(&s, ";\n");
        sp(&s, "  vec4 res = vec4(D + C * (1.0 - A) + B * A, G);\n");
    } else
        sp(&s, "  vec4 res = R[12];\n");
    /* CJ_GL_DIAG_AT: 1 stage-0 texel, 2 diffuse, 3 stage-1 texel, 4 specular */
    sp(&s, "  if (u_diag == 1) res.rgb = R[8].rgb; else if (u_diag == 2) res.rgb = R[4].rgb;\n"
           "  else if (u_diag == 3) res.rgb = R[9].rgb; else if (u_diag == 4) res.rgb = R[5].rgb;\n");
    sp(&s, "  res = clamp(res, 0.0, 1.0);\n  atest(res.a);\n  o = res;\n}\n");
    return s.b;
}

static GLuint vsh_shader(int id);

static Prog *prog_for(const Nv2aBatch *b, int vsh, int legacy)
{
    RcKey k;
    int i, st, lru = 0;
    const Nv2aCombiner *rc = b->rc;
    char *fs;
    ProgEnt *e;

    memset(&k, 0, sizeof k);
    if (legacy)
        goto keyed;
    memcpy(k.color_icw, rc->color_icw, sizeof k.color_icw);
    memcpy(k.alpha_icw, rc->alpha_icw, sizeof k.alpha_icw);
    memcpy(k.color_ocw, rc->color_ocw, sizeof k.color_ocw);
    memcpy(k.alpha_ocw, rc->alpha_ocw, sizeof k.alpha_ocw);
    k.final0 = rc->final0; k.final1 = rc->final1;
    k.control = rc->control; k.stage_program = rc->stage_program;
    k.clip_plane_mode = b->clip_plane_mode;
    for (st = 0; st < 4; st++) {
        uint32_t mode = (rc->stage_program >> (st * 5)) & 0x1F;
        if (b->stages[st]) k.valid |= 1u << st;
        if (mode != 5) k.clip_plane_mode &= ~(0xFu << (st * 4));
    }
keyed:
    for (i = 0; i < s_nprog; i++)
        if (s_progs[i].vsh == vsh && s_progs[i].legacy == legacy
            && !memcmp(&s_progs[i].key, &k, sizeof k)) {
            s_progs[i].used = (int)s_tick;
            return &s_progs[i].p;
        }
    if (s_nprog < NPROG)
        e = &s_progs[s_nprog++];
    else {
        for (i = 1; i < NPROG; i++)
            if (s_progs[i].used < s_progs[lru].used) lru = i;
        e = &s_progs[lru];
        glDeleteProgram(e->p.prog);
    }
    e->key = k;
    e->vsh = vsh;
    e->legacy = legacy;
    e->used = (int)s_tick;
    if (legacy) {
        size_t n = strlen(FS_HEAD) + strlen(FS_LEGACY_BODY) + 1;
        fs = malloc(n);
        snprintf(fs, n, "%s%s", FS_HEAD, FS_LEGACY_BODY);
        link_prog(&e->p, vsh_shader(vsh), fs);
        free(fs);
        return &e->p;
    }
    fs = gen_fs(&k);
    if (getenv("CJ_GL_SHADER_LOG"))
        fprintf(stderr, "[GL] combiner program %d (ctl %08X stages %05X valid %X)\n%s\n",
                s_nprog, k.control, k.stage_program, k.valid, fs);
    else
        fprintf(stderr, "[GL] combiner program %d: ctl %08X stage prog %05X valid %X final %08X/%08X\n",
                s_nprog, k.control, k.stage_program, k.valid, k.final0, k.final1);
    link_prog(&e->p, vsh >= 0 ? vsh_shader(vsh) : s_vs, fs);
    free(fs);
    return &e->p;
}

/* ---- NV2A vertex programs -> GLSL (patch 0008) ----------------------------
 *
 * A rule-for-rule port of the toolkit's CPU interpreter (nv2a_vsh_interp.c,
 * the oracle): same field table, same op semantics (rcp of 0 is +inf, rcc
 * clamps to +-[5.42e-20, 1.88e19], min/max/slt/sge as a<b / a>=b selects,
 * arl = floor(x + 0.001)), the ILU of a paired MAC+ILU instruction writes R1,
 * R12 is oPos, a source mux of 0 reads a constant like 3, an out-of-range
 * constant index reads c[0], unwritten outputs are (0,0,0,1) for the colours
 * and texture coordinates. Then what the executor does after the program:
 * oD0/oD1 clamped and quantised to 8 bits, the fog factor from oFog.x,
 * oPos.w = +0 -> 1, the anti-aliasing scale, and the near-plane clip (w >=
 * 1e-3, screen z >= c[59].z, as gl_ClipDistance with depth clamp on, so GL
 * clips in the same affine image of clip space the CPU clipper uses).
 * c[] is a std140 UBO re-sent only when it changed. One VS per program
 * (by content); a program that writes c[] while CXT_WRITE is on (constants
 * that change between vertices) stays on the CPU. */
typedef struct {
    uint32_t n, inputs, writes_c;
    uint32_t (*w)[4];
    uint64_t hash;
    GLuint vs;
    int ok;
} VshEnt;
#define NVSH 1024
static VshEnt s_vsh[NVSH];
static int s_nvsh, s_vcur = -1, s_vsh_on = 1;
static uint64_t s_vsh_batches, s_vsh_cpu_batches, s_vsh_rejects;
static GLuint s_vao2, s_vbo2, s_ebo, s_ubo;
static float s_cshadow[192][4];
static int s_cvalid;

static uint32_t vf(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

static void vsh_src(Str *o, const uint32_t *ins, int which, uint32_t *inputs)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    if (which == 0) {
        neg = vf(ins, 1, 8, 1); swz = vf(ins, 1, 0, 8);
        mux = vf(ins, 2, 26, 2); reg = vf(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = vf(ins, 2, 25, 1); swz = vf(ins, 2, 17, 8);
        mux = vf(ins, 2, 11, 2); reg = vf(ins, 2, 13, 4);
    } else {
        neg = vf(ins, 2, 10, 1); swz = vf(ins, 2, 2, 8);
        mux = vf(ins, 3, 28, 2);
        reg = (vf(ins, 2, 0, 2) << 2) | vf(ins, 3, 30, 2);
    }
    sp(o, "%c = %s", "ABC"[which], neg ? "-" : "");
    if (mux == 1)
        sp(o, "R%u", reg <= 12 ? reg : 0);
    else if (mux == 2) {
        uint32_t v = vf(ins, 1, 9, 4);
        *inputs |= 1u << v;
        sp(o, "v%u", v);
    } else {
        uint32_t ci = vf(ins, 1, 13, 8);
        if (vf(ins, 3, 1, 1))
            sp(o, "cr(%u)", ci);
        else
            sp(o, "c[%u]", ci < 192 ? ci : 0);
    }
    sp(o, ".%c%c%c%c;\n", sw[(swz >> 6) & 3], sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
}

static void vsh_wm(Str *o, const char *dst, const char *src, uint32_t m)
{
    char c[5];
    int k = 0;
    if (m & 8) c[k++] = 'x';
    if (m & 4) c[k++] = 'y';
    if (m & 2) c[k++] = 'z';
    if (m & 1) c[k++] = 'w';
    c[k] = 0;
    sp(o, "  %s.%s = %s.%s;\n", dst, c, src, c);
}

static char *gen_vs(const uint32_t (*w)[4], uint32_t n, uint32_t *inputs, uint32_t *writes_c)
{
    Str o = {0}, body = {0};
    uint32_t i, k;
    *inputs = 0;
    *writes_c = 0;
    for (i = 0; i < n; i++) {
        const uint32_t *ins = w[i];
        uint32_t mac = vf(ins, 1, 21, 4), ilu = vf(ins, 1, 25, 3);
        uint32_t mac_mask = vf(ins, 3, 24, 4), ilu_mask = vf(ins, 3, 16, 4);
        uint32_t tdst = vf(ins, 3, 20, 4), omask = vf(ins, 3, 12, 4), oaddr = vf(ins, 3, 3, 8);
        int useB = mac == 2 || (mac >= 4 && mac <= 12), useC = mac == 3 || mac == 4 || ilu;
        char d[8];
        sp(&body, "  // %u: mac %u ilu %u\n", i, mac, ilu);
        if (mac) vsh_src(&body, ins, 0, inputs);
        if (mac && useB) vsh_src(&body, ins, 1, inputs);
        if (useC) vsh_src(&body, ins, 2, inputs);
        switch (mac) {
        case 0: break;
        case 2: sp(&body, "M = A * B;\n"); break;
        case 3: sp(&body, "M = A + C;\n"); break;
        case 4: sp(&body, "M = A * B + C;\n"); break;
        case 5: sp(&body, "M = vec4(A.x*B.x + A.y*B.y + A.z*B.z);\n"); break;
        case 6: sp(&body, "M = vec4(A.x*B.x + A.y*B.y + A.z*B.z + B.w);\n"); break;
        case 7: sp(&body, "M = vec4(A.x*B.x + A.y*B.y + A.z*B.z + A.w*B.w);\n"); break;
        case 8: sp(&body, "M = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9: sp(&body, "M = mix(B, A, lessThan(A, B));\n"); break;
        case 10: sp(&body, "M = mix(B, A, greaterThanEqual(A, B));\n"); break;
        case 11: sp(&body, "M = vec4(lessThan(A, B));\n"); break;
        case 12: sp(&body, "M = vec4(greaterThanEqual(A, B));\n"); break;
        case 13: sp(&body, "M = A; a0 = int(floor(A.x + 0.001));\n"); break;
        default: sp(&body, "M = A;\n"); break;     /* mov, and 14/15 as the interpreter */
        }
        switch (ilu) {
        case 0: break;
        case 2: sp(&body, "I = vec4(C.x == 0.0 ? INF : 1.0 / C.x);\n"); break;
        case 3: sp(&body, "I = vec4(rcc(C.x));\n"); break;
        case 4: sp(&body, "I = vec4(1.0 / sqrt(abs(C.x)));\n"); break;
        case 5: sp(&body, "I = vec4(exp2(floor(C.x)), C.x - floor(C.x), exp2(C.x), 1.0);\n"); break;
        case 6: sp(&body, "I = vlog(C.x);\n"); break;
        case 7: sp(&body, "I = vlit(C);\n"); break;
        default: sp(&body, "I = C;\n"); break;
        }
        if (mac && mac != 13 && mac_mask && tdst <= 12) {
            snprintf(d, sizeof d, "R%u", tdst);
            vsh_wm(&body, d, "M", mac_mask);
        }
        if (ilu && ilu_mask) {
            uint32_t it = mac ? 1u : tdst;
            if (it <= 12) {
                snprintf(d, sizeof d, "R%u", it);
                vsh_wm(&body, d, "I", ilu_mask);
            }
        }
        if (omask) {
            const char *src = vf(ins, 3, 2, 1) ? "I" : "M";
            if (!vf(ins, 3, 11, 1))
                *writes_c = 1;                    /* refused when CXT_WRITE is on */
            else if (oaddr == 0)
                vsh_wm(&body, "R12", src, omask);
            else if (oaddr < 13) {
                snprintf(d, sizeof d, "o%u", oaddr);
                vsh_wm(&body, d, src, omask);
            }
        }
    }
    sp(&o, "#version 330 core\n"
           "layout(std140) uniform VshC { vec4 c[192]; };\n");
    for (k = 0; k < 16; k++)
        if (*inputs & (1u << k))
            sp(&o, "layout(location=%u) in vec4 v%u;\n", k, k);
    sp(&o, "uniform vec2 u_size; uniform float u_zscale; uniform vec2 u_aa, u_uvs;\n"
           "uniform float u_clipz; uniform ivec2 u_fogm; uniform vec2 u_fogp;\n"
           "out vec4 v_col; out vec2 v_uv; out vec4 v_spec; out float v_fog;\n"
           "out vec4 v_tc0, v_tc1, v_tc2, v_tc3;\n"
           "out float gl_ClipDistance[2];\n"
           "invariant gl_Position;\n"
           "const float INF = uintBitsToFloat(0x7F800000u);\n"
           "int a0;\n"
           "vec4 cr(int i) { i += a0; return (i < 0 || i >= 192) ? c[0] : c[i]; }\n"
           "float rcc(float x) {\n"
           "  float f = x == 0.0 ? (floatBitsToUint(x) != 0u ? -INF : INF) : 1.0 / x;\n"
           "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
           "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
           "  return f;\n}\n"
           "vec4 vlog(float x) {\n"
           "  float ax = abs(x);\n"
           "  if (ax == 0.0) return vec4(-INF, 1.0, -INF, 1.0);\n"
           "  float e = floor(log2(ax));\n"
           "  return vec4(e, ax / exp2(e), log2(ax), 1.0);\n}\n"
           "vec4 vlit(vec4 s) {\n"
           "  float nl = s.x > 0.0 ? s.x : 0.0, nh = s.y > 0.0 ? s.y : 0.0;\n"
           "  float p = clamp(s.w, -127.9961, 127.9961);\n"
           "  float z = s.x > 0.0 ? (p == 0.0 ? 1.0 : pow(nh, p)) : 0.0;\n"
           "  return vec4(1.0, nl, z, 1.0);\n}\n"
           "float fogf(float d) {\n"
           "  if (u_fogm.x == 0) return 1.0;\n"
           "  float f, px = u_fogp.x, py = u_fogp.y; int m = u_fogm.y;\n"
           "  if (m == 0x800 || m == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
           "  else if (m == 0x801 || m == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
           "  else f = px + d * py - 1.0;\n"
           "  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
           "  if (isnan(f)) f = 1.0;\n"
           "  return %s;\n}\n"
           "vec4 q8(vec4 x) { return floor(clamp(x, 0.0, 1.0) * 255.0 + 0.5) / 255.0; }\n"
           "void main() {\n"
           "  vec4 R0 = vec4(0.0), R1 = R0, R2 = R0, R3 = R0, R4 = R0, R5 = R0, R6 = R0,\n"
           "       R7 = R0, R8 = R0, R9 = R0, R10 = R0, R11 = R0, R12 = R0;\n"
           "  vec4 o1 = R0, o2 = R0, o3 = vec4(0.0, 0.0, 0.0, 1.0), o4 = o3, o5 = R0, o6 = R0,\n"
           "       o7 = R0, o8 = R0, o9 = o3, o10 = o3, o11 = o3, o12 = o3;\n"
           "  vec4 A, B, C, M, I;\n"
           "  a0 = 0;\n", s_fogpix_on ? "clamp(f, -3.4e38, 3.4e38)" : "clamp(f, 0.0, 1.0)");
    sp(&o, "%s", body.b ? body.b : "");
    free(body.b);
    sp(&o, "  vec4 p = R12;\n"
           "  if (floatBitsToUint(p.w) == 0u) p.w = 1.0;\n"
           "  p.xy *= u_aa;\n"
           "  float w = p.w;\n"
           "  vec3 n = vec3(p.x / u_size.x * 2.0 - 1.0, 1.0 - p.y / u_size.y * 2.0, p.z / u_zscale * 2.0 - 1.0);\n"
           "  gl_Position = vec4(n * w, w);\n"
           "  gl_ClipDistance[0] = w - 1e-3;\n"
           "  gl_ClipDistance[1] = (p.z - u_clipz) * w;\n"
           "  v_col = q8(o3); v_spec = q8(o4); v_fog = fogf(o5.x);\n"
           "  v_tc0 = o9; v_tc1 = o10; v_tc2 = o11; v_tc3 = o12; v_uv = o9.xy * u_uvs;\n"
           "}\n");
    return o.b;
}

static GLuint vsh_shader(int id) { return s_vsh[id].vs; }

static uint64_t vsh_hash(const uint32_t (*w)[4], uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < 4; j++) { h ^= w[i][j]; h *= 0x100000001B3ull; }
    return h;
}

static int init(void);

static int gl_vsh_accept(const uint32_t (*w)[4], uint32_t n, uint32_t cxt_write, uint32_t *inputs)
{
    VshEnt *e = NULL;
    uint64_t h;
    int i;
    if (!s_vsh_on || !init()) {
        s_vsh_cpu_batches++;
        return 0;
    }
    if (s_vcur >= 0 && s_vsh[s_vcur].n == n && !memcmp(s_vsh[s_vcur].w, w, n * 16))
        e = &s_vsh[s_vcur];
    else {
        h = vsh_hash(w, n);
        for (i = 0; i < s_nvsh; i++)
            if (s_vsh[i].hash == h && s_vsh[i].n == n && !memcmp(s_vsh[i].w, w, n * 16)) {
                e = &s_vsh[i];
                break;
            }
        if (!e) {
            char *src;
            GLint ok = 0;
            if (s_nvsh == NVSH) {
                s_vsh_cpu_batches++;
                return 0;
            }
            e = &s_vsh[s_nvsh++];
            e->n = n;
            e->hash = h;
            e->w = malloc(n * 16);
            memcpy(e->w, w, n * 16);
            src = gen_vs(w, n, &e->inputs, &e->writes_c);
            e->vs = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(e->vs, 1, (const char *const *)&src, NULL);
            glCompileShader(e->vs);
            glGetShaderiv(e->vs, GL_COMPILE_STATUS, &ok);
            e->ok = ok;
            if (!ok) {
                char log[2048];
                glGetShaderInfoLog(e->vs, sizeof log, NULL, log);
                fprintf(stderr, "[GL] vsh %d: compile failed, CPU path: %s\n%s\n", s_nvsh - 1, log, src);
            } else if (getenv("CJ_GL_SHADER_LOG"))
                fprintf(stderr, "[GL] vsh %d (%u instructions, inputs %04X)\n%s\n", s_nvsh - 1, n, e->inputs, src);
            else
                fprintf(stderr, "[GL] vsh %d: %u instructions, inputs %04X%s\n", s_nvsh - 1, n, e->inputs,
                        e->writes_c ? ", writes c[]" : "");
            free(src);
        }
        s_vcur = (int)(e - s_vsh);
    }
    if (!e->ok || (e->writes_c && cxt_write)) {
        s_vsh_rejects++;
        s_vsh_cpu_batches++;
        return 0;
    }
    *inputs = e->inputs;
    s_vsh_batches++;
    return s_ab ? 2 : 1;
}

/* The vertex half of a GPU-VS draw: inputs, constants, indices, clip state. */
static void vsh_draw(const Nv2aBatch *b)
{
    const Nv2aVshBatch *v = b->vsh;
    uint32_t na = 0, k, off = 0;
    for (k = 0; k < 16; k++)
        if (v->array_mask & (1u << k)) na++;
    glUniform2f(s_cur->u_aa, v->aa_sx, v->aa_sy);
    glUniform2f(s_cur->u_uvs, v->uv_scale[0], v->uv_scale[1]);
    glUniform1f(s_cur->u_clipz, v->clip_z);
    glUniform2i(s_cur->u_fogm, (int)v->fog_enable, (int)v->fog_mode);
    glUniform2f(s_cur->u_fogp, v->fog_param[0], v->fog_param[1]);
    if (!s_cvalid || memcmp(s_cshadow, v->constants, sizeof s_cshadow)) {
        memcpy(s_cshadow, v->constants, sizeof s_cshadow);
        s_cvalid = 1;
        glBindBuffer(GL_UNIFORM_BUFFER, s_ubo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof s_cshadow, s_cshadow, GL_STREAM_DRAW);
    }
    glBindVertexArray(s_vao2);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo2);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)v->nverts * (na ? na : 1) * 16, v->verts, GL_STREAM_DRAW);
    for (k = 0; k < 16; k++) {
        if (v->array_mask & (1u << k)) {
            glEnableVertexAttribArray(k);
            glVertexAttribPointer(k, 4, GL_FLOAT, GL_FALSE, (GLsizei)(na * 16), (void *)(uintptr_t)off);
            off += 16;
        } else {
            glDisableVertexAttribArray(k);
            glVertexAttrib4fv(k, v->inputs[k]);
        }
    }
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)b->count * 4, v->indices, GL_STREAM_DRAW);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_CLIP_DISTANCE0);
    glEnable(GL_CLIP_DISTANCE1);
    if (!s_nodraw) glDrawElements(GL_TRIANGLES, (GLsizei)b->count, GL_UNSIGNED_INT, 0);
    glDisable(GL_DEPTH_CLAMP);
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_CLIP_DISTANCE1);
    glBindVertexArray(s_vao);
}

/* ---- init --------------------------------------------------------------- */

static int init(void)
{
    const char *e;
    if (s_ready || s_failed)
        return s_ready;
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[GL] SDL video: %s\n", SDL_GetError());
        s_failed = 1;
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    /* Core + forward-compatible: what macOS (Wine/Whisky -> macOS GL 4.1 core) requires. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "[GL] SDL game controller: %s (keyboard only)\n", SDL_GetError());
    {
        /* CJ_WINDOW_SCALE (default 2: 1280x960), resizable, letterboxed;
         * CJ_FULLSCREEN=1 or F11 for desktop fullscreen. */
        int sc = (e = getenv("CJ_WINDOW_SCALE")) && atoi(e) > 0 ? atoi(e) : 2;
        SDL_Rect ub;
        /* Never minimize on focus loss: the Snipping Tool (Win+Shift+S) and Alt+Tab take focus, and a
         * minimized game leaves the overlay nothing to capture. */
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
        Uint32 fl = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (SDL_GetDisplayUsableBounds(0, &ub) == 0)
            while (sc > 1 && ((cj_widescreen() ? 853 : 640) * sc > ub.w || 480 * sc > ub.h - 40)) sc--;
        if (pcs_get(PCS_FULLSCREEN)) { fl |= SDL_WINDOW_BORDERLESS; fl &= ~(Uint32)SDL_WINDOW_RESIZABLE; s_fullscreen = 1; }
        s_win = SDL_CreateWindow("Predator: Concrete Jungle", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, (cj_widescreen() ? 2560 : 1920) * sc / 3, 480 * sc, fl);
    }
    if (!s_win || !(s_ctx = SDL_GL_CreateContext(s_win))) {
        fprintf(stderr, "[GL] window/context: %s\n", SDL_GetError());
        s_failed = 1;
        return 0;
    }
    if (s_fullscreen) {   /* started in fullscreen: remember the window it came from, then cover the display */
        SDL_GetWindowPosition(s_win, &s_win_x, &s_win_y);
        SDL_GetWindowSize(s_win, &s_win_w, &s_win_h);
        set_borderless_fs(s_win, 1);
    }
    focusdiag_init(s_win);
    SDL_GL_SetSwapInterval(pcs_get(PCS_VSYNC));
    fprintf(stderr, "[GL] %s | %s | %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
            glGetString(GL_VERSION));
    gamelog_init((const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
                 (const char *)glGetString(GL_VERSION));
    s_vs = sh(GL_VERTEX_SHADER, VS);
    {
        size_t n = strlen(FS_HEAD) + strlen(FS_LEGACY_BODY) + 1;
        char *fs = malloc(n);
        snprintf(fs, n, "%s%s", FS_HEAD, FS_LEGACY_BODY);
        link_prog(&s_legacy, s_vs, fs);
        free(fs);
    }
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_vbo_ext);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex), (void *)0);
    glEnableVertexAttribArray(1);   /* 0xAARRGGBB little-endian = B,G,R,A bytes */
    glVertexAttribPointer(1, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Nv2aVertex),
                          (void *)offsetof(Nv2aVertex, diffuse));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex),
                          (void *)offsetof(Nv2aVertex, u));
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo_ext);
    glVertexAttribPointer(3, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Nv2aVertexExt),
                          (void *)offsetof(Nv2aVertexExt, specular));
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertexExt),
                          (void *)offsetof(Nv2aVertexExt, fog));
    {
        int i;
        for (i = 0; i < 4; i++)
            glVertexAttribPointer(5 + i, 4, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertexExt),
                                  (void *)(offsetof(Nv2aVertexExt, tc) + 16 * i));
    }
    glGenVertexArrays(1, &s_vao2);
    glGenBuffers(1, &s_vbo2);
    glGenBuffers(1, &s_ebo);
    glGenBuffers(1, &s_ubo);
    glBindVertexArray(s_vao2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ebo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_UNIFORM_BUFFER, s_ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof s_cshadow, NULL, GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_ubo);
    s_nodraw = (e = getenv("CJ_GL_NODRAW")) && *e == '1';
    if ((e = getenv("CJ_GL_VSH")) && *e == '0')
        s_vsh_on = 0;
    if ((e = getenv("CJ_GL_VSH_AB")) && *e == '1' && s_vsh_on)
        s_ab = 1;
    fprintf(stderr, "[GL] vertex programs: %s%s\n", s_vsh_on ? "GPU (CJ_GL_VSH=0: CPU)" : "CPU (CJ_GL_VSH=0)",
            s_ab ? ", A/B: CPU-VS twin drawn to shadow surfaces, dumped as <prefix>cNNNNN.ppm" : "");
    glGenTextures(1, &s_tex);
    s_dump = getenv("CJ_GL_DUMP");
    if ((e = getenv("CJ_GL_DUMP_EVERY")) && atoi(e) > 0)
        s_dump_every = (unsigned)atoi(e);
    if ((e = getenv("CJ_GL_TEXCACHE")) && *e == '0')
        s_texcache = 0;
    if ((e = getenv("CJ_GL_RTT")) && *e == '0')
        s_rtt = 0;
    if ((e = getenv("CJ_GL_RC")) && *e == '0')
        s_rc_on = 0;
    if ((e = getenv("CJ_GL_MIPS")) && *e == '0')
        s_mips_on = 0;
    if ((e = getenv("CJ_GL_WCLIP")) && *e == '0')
        s_wclip_on = 0;
    if ((e = getenv("CJ_GL_CLIPSCISSOR")) && *e == '0')
        s_clipsc_on = 0;
    if ((e = getenv("CJ_GL_DIAG_AT")))
        s_diag_at = atol(e);
    if ((e = getenv("CJ_GL_DUMP_AT")))
        s_dump_at = atol(e);
    if ((e = getenv("CJ_GL_WBUF")) && (*e == '0' || *e == '2'))
        s_wbuf_on = *e == '0' ? 0 : 2;
    if ((e = getenv("CJ_GL_FOG_ARGB")) && *e == '1')
        s_fog_argb = 1;
    if ((e = getenv("CJ_GL_POLYOFF")) && *e == '0')
        s_polyoff_on = 0;
    if ((e = getenv("CJ_GL_DEPTHQ")) && *e == '0')
        s_depthq_on = 0;
    if ((e = getenv("CJ_GL_TEXHASH")) && *e == '0')
        s_texhash_full = 0;
    {   /* Resolution Scale left unset: render at least at the screen's height
         * (1080p -> 3, 1440p -> 3, 4K -> 5), so the picture is sharp from the start */
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(s_win ? SDL_GetWindowDisplayIndex(s_win) : 0, &dm) == 0 && dm.h > 0) {
            int want = (dm.h + 479) / 480;
            pcs_set_default(PCS_SCALE, want < 2 ? 2 : want > 5 ? 5 : want);
        }
    }
    s_rs = pcs_get(PCS_SCALE);
    if (s_rs > 1) fprintf(stderr, "[GL] internal resolution x%d (CJ_GL_SCALE)\n", s_rs);
    if ((e = getenv("CJ_GL_FOGPIX")) && *e == '0')
        s_fogpix_on = 0;
    if ((e = getenv("CJ_GL_ANISO")) && atoi(e) > 0)
        s_aniso_force = atoi(e);
    if ((e = getenv("CJ_GL_LOD_BIAS")))
        s_lod_bias_add = (float)atof(e);
    if (epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic")
        || epoxy_has_gl_extension("GL_ARB_texture_filter_anisotropic"))
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &s_aniso_max);
    s_s3tc = epoxy_has_gl_extension("GL_EXT_texture_compression_s3tc")
             && !((e = getenv("CJ_GL_S3TC")) && *e == '0');
    fprintf(stderr, "[GL] DXT textures: %s\n", s_s3tc ? "uploaded compressed (CJ_GL_S3TC=0: decode on the CPU)"
                                                      : "decoded on the CPU (no S3TC support)");
    fprintf(stderr, "[GL] textures: mip chains %s, anisotropic filtering %s (max %.0f%s)\n",
            s_mips_on ? "on (CJ_GL_MIPS=0: level 0 only)" : "off", s_aniso_max > 0 ? "available" : "not available",
            s_aniso_max, s_aniso_force > 0 ? ", CJ_GL_ANISO forces it" : ", title's setting");
    s_t0 = now_s();
    s_ready = 1;
    return 1;
}

/* ---- render targets ----------------------------------------------------- */

/* One FBO per guest colour surface (keyed by address only). The executor
 * reports the clip rectangle as the surface size, and a title that changes its
 * viewport/clip (the front end draws 540x408 into the 640x480 back buffer)
 * must keep drawing into the same pixels, so the FBO is sized from the pitch
 * and grown (contents copied) if a taller clip shows up. */
static uint32_t surf_w(const Nv2aSurface *s)
{
    uint32_t w = (s->pitch && s->bytes_per_pixel) ? s->pitch / s->bytes_per_pixel : s->width;
    if (w < s->width || w > 4096) w = s->width;
    return w;
}


static void rt_alloc1(Rt *r, uint32_t w, uint32_t h)
{
    r->w = w; r->h = h;
    glGenFramebuffers(1, &r->fbo2[s_side]);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo2[s_side]);
    glGenTextures(1, &r->color2[s_side]);
    glBindTexture(GL_TEXTURE_2D, r->color2[s_side]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r->w * s_rs, r->h * s_rs, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->color2[s_side], 0);
    glGenRenderbuffers(1, &r->depth2[s_side]);
    glBindRenderbuffer(GL_RENDERBUFFER, r->depth2[s_side]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, r->w * s_rs, r->h * s_rs);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, r->depth2[s_side]);
    glColorMask(1, 1, 1, 1); glDepthMask(GL_TRUE); glStencilMask(0xFF);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static void rt_alloc(Rt *r, uint32_t w, uint32_t h)
{
    int sv = s_side;
    {   /* CJ_GL_TEST_BIG_FBO=1: every FBO at least 640x480 (tests surfaces whose FBO is larger than their clip) */
        static int big = -1;
        if (big < 0) big = getenv("CJ_GL_TEST_BIG_FBO") && atoi(getenv("CJ_GL_TEST_BIG_FBO"));
        if (big) { if (w < 640) w = 640; if (h < 480) h = 480; }
    }
    for (s_side = 0; s_side <= s_ab; s_side++)
        rt_alloc1(r, w, h);
    s_side = sv;
}

static void rt_free(Rt *r)
{
    int sv = s_side;
    for (s_side = 0; s_side <= s_ab; s_side++) {
        glDeleteFramebuffers(1, &r->fbo2[s_side]);
        glDeleteTextures(1, &r->color2[s_side]);
        glDeleteRenderbuffers(1, &r->depth2[s_side]);
    }
    s_side = sv;
}

static Rt *rt_for(const Nv2aSurface *s)
{
    int i, lru = 0;
    Rt *r;
    uint32_t w = surf_w(s), h = s->height;
    if (w >= 640 && h < w * 3 / 4) h = w * 3 / 4;   /* clip y origin is not passed on */
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i].va == s->color_va) {
            r = &s_rt[i];
            r->used = ++s_tick;
            r->pitch = s->pitch; r->bpp = s->bytes_per_pixel;
            r->cw = s->width; r->ch = s->height;
            if (s->width > r->fcw) r->fcw = s->width;
            if (s->height > r->fch) r->fch = s->height;
            if (w > r->w || h > r->h) {        /* grow, keep the pixels */
                Rt old = *r;
                rt_alloc(r, w > old.w ? w : old.w, h > old.h ? h : old.h);
                int sv = s_side;
                glDisable(GL_SCISSOR_TEST);
                for (s_side = 0; s_side <= s_ab; s_side++) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, old.fbo2[s_side]);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, r->fbo2[s_side]);
                    /* GL rows are bottom-up and guest row 0 is the top: keep it there */
                    glBlitFramebuffer(0, 0, old.w * s_rs, old.h * s_rs, 0, (r->h - old.h) * s_rs, old.w * s_rs, r->h * s_rs,
                                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
                }
                s_side = sv;
                rt_free(&old);
                fprintf(stderr, "[GL] surface 0x%08X grown to %ux%u\n", r->va, r->w, r->h);
            }
            return r;
        }
    if (s_nrt < NRT)
        r = &s_rt[s_nrt++];
    else {
        for (i = 1; i < NRT; i++)
            if (s_rt[i].used < s_rt[lru].used) lru = i;
        r = &s_rt[lru];
        rt_free(r);
    }
    memset(r, 0, sizeof *r);
    r->va = s->color_va; r->used = ++s_tick;
    r->pitch = s->pitch; r->bpp = s->bytes_per_pixel;
    r->cw = s->width; r->ch = s->height;
    r->fcw = s->width; r->fch = s->height;
    rt_alloc(r, w, h);
    r->xa = -1;
    fprintf(stderr, "[GL] new surface 0x%08X %ux%u (clip %ux%u pitch %u bpp %u aa %ux%u format %X)\n", r->va, r->w, r->h,
            s->width, s->height, s->pitch, s->bytes_per_pixel, s->aa_sx, s->aa_sy, s->format);
    return r;
}

static Rt *s_present;       /* last full-screen-sized surface drawn to */
static uint32_t s_shown_va, s_crtc_va;
static uint32_t s_dump_until;   /* CJ_GL_DUMP_TRIGGER window: flips below this are dumped (and draw-logged) */   /* what the window showed last flip, and PCRTC_START */

static void bind_rt(const Nv2aSurface *s)
{
    Rt *r = rt_for(s);
    { static uint32_t seq; r->last_bind = ++seq; }
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo2[s_side]);
    glViewport(0, 0, r->w * s_rs, r->h * s_rs);
    s_last = r;
    /* A frame-sized surface by its pitch and this clip, not by its FBO: a
     * 320x240 bloom target that once shared an address with a 640x480 one
     * has a 640x480 FBO, and presenting it showed the blur. The front end's
     * 540x408 clip of the back buffer still counts. */
    if (surf_w(s) >= 640 && s->height >= 400) s_present = r;
}

/* Is guest texture `t` (or part of it) a surface the GPU drew lately? Then
 * that FBO's texel (x0, y0) is the texture's (0, 0). */
#define RTT_LIVE_FLIPS 30
static Rt *rt_for_texture(const Nv2aTexture *t, uint32_t *x0, uint32_t *y0)
{
    int i;
    Rt *best = NULL;
    uint32_t off = t->offset & 0x0FFFFFFFu;
    /* A surface that starts exactly here first. Only then a texture inside
     * another surface (a sub-rectangle): and inside the rows that surface has
     * now (ch, its clip at the latest bind), not its FBO's. An FBO can be
     * taller than the surface -- display-sized ones are kept 4:3, and one
     * grows when its address is reused by a bigger surface -- and the range
     * of those extra rows covered the next surfaces in memory. PCJ's focus
     * blur keeps a 128x128 target right after a 256x256 one; once the
     * 256x256 one's FBO had grown, the 128x128 texture resolved to the
     * middle of that FBO, which was black: the weapon-list background went
     * dark, more often the longer the session ran. */
    for (i = 0; i < s_nrt; i++) {
        Rt *r = &s_rt[i];
        if (!r->bpp || s_flips - r->drawn_flip > RTT_LIVE_FLIPS)
            continue;
        if (off == (r->va & 0x0FFFFFFFu)) { *x0 = *y0 = 0; return r; }
    }
    for (i = 0; i < s_nrt; i++) {
        Rt *r = &s_rt[i];
        uint32_t va = r->va & 0x0FFFFFFFu, pitch = r->pitch ? r->pitch : r->w * r->bpp;
        uint32_t rows = r->ch ? r->ch : r->h;
        if (!r->bpp || s_flips - r->drawn_flip > RTT_LIVE_FLIPS)
            continue;
        if (off > va && off < va + pitch * rows) {
            uint32_t d = off - va, ty = d / pitch, tx0 = (d % pitch) / r->bpp;
            if (tx0 + t->width <= r->w + 1 && ty + t->height <= rows + 1 && (!best || r->last_bind > best->last_bind)) {
                best = r;
                *y0 = ty;
                *x0 = tx0;
            }
        }
    }
    return best;
}

/* Colour formats without alpha (NV097 codes): R5G6B5 and X8R8G8B8, X1R5G5B5,
 * swizzled and linear. Sampled from an RGBA FBO they must read a = 1. */
static int fmt_no_alpha(uint32_t c)
{
    switch (c) {
    case 0x03: case 0x05: case 0x07: case 0x11: case 0x1C: case 0x1E:
        return 1;
    default:
        return 0;
    }
}

static int fmt_normalised(uint32_t c)
{
    /* the executor's tex_size_from_format: swizzled or DXT -> normalised */
    return d3d8_format_is_swizzled(c) || d3d8_format_dxt_block_bytes(c);
}

/* ---- state -------------------------------------------------------------- */


/* NV2A window clip -> u_wc (GL window pixels of the FBO, half-open). The
 * rectangles are inclusive logical pixels: x [min, max], scaled by the AA
 * factor; guest row 0 is the FBO's top row. A single inclusive rectangle that
 * covers the whole surface is no clip. */
static void apply_window_clip(const Nv2aSurface *sf, const Nv2aRenderState *rs)
{
    float wc[8][4];
    int n = 0, i, excl = (int)(rs->window_clip_type & 1);
    uint32_t sx = sf->aa_sx ? sf->aa_sx : 1, sy = sf->aa_sy ? sf->aa_sy : 1;
    if (s_wclip_on)
        for (i = 0; i < 8; i++) {
            uint32_t x = rs->window_clip_x[i], y = rs->window_clip_y[i];
            float x0 = (float)((x & 0xFFF) * sx), x1 = (float)((((x >> 16) & 0xFFF) + 1) * sx);
            float y0 = (float)((y & 0xFFF) * sy), y1 = (float)((((y >> 16) & 0xFFF) + 1) * sy);
            if (!x && !y) break;                    /* unused slot */
            if (x1 <= x0 || y1 <= y0) continue;
            wc[n][0] = x0 * s_rs; wc[n][2] = x1 * s_rs;
            wc[n][1] = ((float)s_last->h - y1) * s_rs; wc[n][3] = ((float)s_last->h - y0) * s_rs;
            n++;
        }
    if (n == 1 && !excl && wc[0][0] <= 0.f && wc[0][2] >= (float)(sf->width * s_rs)
        && ((float)s_last->h * s_rs - wc[0][1]) >= (float)(sf->height * s_rs))
        n = 0;
    if (n) s_wclip_draws++;
    glUniform1i(s_cur->u_wc_n, n);
    glUniform1i(s_cur->u_wc_excl, excl);
    if (n) glUniform4fv(s_cur->u_wc, n, &wc[0][0]);
}

/* Scissor to [x0, x1) x [y0, y1) in guest surface pixels (row 0 at the top
 * of the FBO). A rectangle covering the whole FBO turns the test off. */
static void scissor_guest(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)s_last->w) x1 = (int)s_last->w;
    if (y1 > (int)s_last->h) y1 = (int)s_last->h;
    if (!s_clipsc_on || (x0 == 0 && y0 == 0 && x1 >= (int)s_last->w && y1 >= (int)s_last->h)) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0 * s_rs, ((int)s_last->h - y1) * s_rs, (x1 - x0) * s_rs, (y1 - y0) * s_rs);
}

/* The alpha a colour format stores: X8R8G8B8_Z8R8G8B8 (4) and
 * X1R5G5B5_Z1R5G5B5 (1) write 0, the _O forms (5, 2) write 1, and A8R8G8B8
 * keeps what the draw produced (-1). PCJ's back buffer is an X format; the
 * focus blur copies it to a texture with SRC_ALPHA blending, sampling it as
 * A8R8G8B8, so the copy only lands where the stored alpha says so. Here the
 * FBO kept whatever alpha the draws left behind, and the blur came out black
 * or not depending on what was drawn last. CJ_GL_XALPHA=0 turns this off. */
static int s_xalpha_on = -1;
static int xalpha(const Nv2aSurface *sf)
{
    if (s_xalpha_on < 0) s_xalpha_on = !getenv("CJ_GL_XALPHA") || atoi(getenv("CJ_GL_XALPHA")) != 0;
    if (!s_xalpha_on) return -1;
    switch (sf->format & 0xF) {
    case 1: case 4: return 0;
    case 2: case 5: return 1;
    default: return -1;
    }
}

/* Before a draw into `sf`: keep the FBO's alpha at the format's constant
 * (set it once when the surface turns into an X format, then mask alpha). */
static void xalpha_draw(const Nv2aSurface *sf, Rt *r, uint32_t m)
{
    int xa = xalpha(sf);
    if (xa < 0) { r->xa = -1; return; }
    if (r->xa != xa) {
        GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0, 0, 0, (float)xa);
        glClear(GL_COLOR_BUFFER_BIT);
        if (sc) glEnable(GL_SCISSOR_TEST);
        r->xa = xa;
    }
    glColorMask(!!(m & 0x00010000u), !!(m & 0x100u), !!(m & 0x1u), GL_FALSE);
}

static void apply_state(const Nv2aRenderState *rs)
{
    if (s_polyoff_on && rs->poly_offset_fill && (rs->poly_offset_factor != 0.f || rs->poly_offset_units != 0.f)) {
        /* NV2A's bias is in units of the 24-bit depth LSB, as GL's on a
         * 24-bit buffer; the factor multiplies the depth slope, as GL's. */
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(rs->poly_offset_factor, rs->poly_offset_units);
        s_polyoff_draws++;
    } else
        glDisable(GL_POLYGON_OFFSET_FILL);
    uint32_t m = rs->color_mask;
    glColorMask(!!(m & 0x00010000u), !!(m & 0x100u), !!(m & 0x1u), !!(m & 0x01000000u));
    if (rs->blend_enable) {
        glEnable(GL_BLEND);
        glBlendFunc(rs->blend_src, rs->blend_dst);
        glBlendEquation(rs->blend_eq ? rs->blend_eq : GL_FUNC_ADD);
        glBlendColor(((rs->blend_color >> 16) & 0xFF) / 255.f, ((rs->blend_color >> 8) & 0xFF) / 255.f,
                     (rs->blend_color & 0xFF) / 255.f, (rs->blend_color >> 24) / 255.f);
    } else
        glDisable(GL_BLEND);
    if (rs->zeta_va && (rs->depth_test_enable || rs->depth_write)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(rs->depth_test_enable ? rs->depth_func : GL_ALWAYS);
        glDepthMask(rs->depth_write ? GL_TRUE : GL_FALSE);
    } else
        glDisable(GL_DEPTH_TEST);
    if (rs->stencil_enable) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(rs->stencil_func, rs->stencil_ref, rs->stencil_func_mask);
        glStencilMask(rs->stencil_write_mask);
        glStencilOp(rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass);
    } else
        glDisable(GL_STENCIL_TEST);
    {
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("CJ_GL_DBG") ? atoi(getenv("CJ_GL_DBG")) : 0;
        if ((dbg & 1) && !rs->depth_write) glDisable(GL_DEPTH_TEST);
        if (dbg & 2) { glDisable(GL_CULL_FACE); goto culled; }
    }
    if (rs->cull_enable) {
        glEnable(GL_CULL_FACE);
        glCullFace(rs->cull_face ? rs->cull_face : GL_BACK);
        /* The FBO holds the image upright (guest row 0 at the top), so a
         * triangle's on-screen winding is the same in GL window space as on
         * the NV2A: front face passes straight through. (It used to be
         * inverted, which culled every front face: menu text, the logo and
         * the near side of every model.) */
        glFrontFace(rs->front_face == GL_CCW ? GL_CCW : GL_CW);
    } else
        glDisable(GL_CULL_FACE);
culled:
    glUniform1i(s_cur->u_alpha_on, (int)rs->alpha_test_enable);
    glUniform1i(s_cur->u_alpha_func, (int)(rs->alpha_func - 0x200));
    glUniform1f(s_cur->u_alpha_ref, (float)rs->alpha_ref);
    glUniform1f(s_cur->u_zscale, rs->depth_max > 1.0f ? rs->depth_max : 16777215.0f);
    glUniform1i(s_cur->u_dq, s_depthq_on);
    /* Polygon offset in the shader (see atest); GL's is a no-op here. */
    if (s_depthq_on && s_polyoff_on && rs->poly_offset_fill)
        glUniform2f(s_cur->u_po, rs->poly_offset_units, rs->poly_offset_factor);
    else
        glUniform2f(s_cur->u_po, 0.0f, 0.0f);
    glUniform1i(s_cur->u_diag, s_diag_at >= 0 && s_flips >= (uint32_t)s_diag_at && s_flips < (uint32_t)s_diag_at + 5
                               ? (int)(s_flips - (uint32_t)s_diag_at) : 0);
    /* CJ_GL_WBUF=2 (A/B): W-buffering on even flips only */
    glUniform1i(s_cur->u_wbuf, s_wbuf_on && (s_wbuf_on == 1 || !(s_flips & 1)) && (rs->control0 & 0x10000u) ? 1 : 0);
    if (rs->control0 & 0x10000u) s_wbuf_draws++;
}

/* Bytes per texel of an NV097 colour format (0 for a DXT format or unknown).
 * Hardware facts: sizes per SET_TEXTURE_FORMAT colour code. */
static uint32_t fmt_bytes(uint32_t c)
{
    switch (c) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F: return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x17: case 0x1A:
    case 0x1C: case 0x1D: case 0x20: case 0x24: case 0x25: case 0x27: case 0x28: case 0x29:
    case 0x2C: case 0x30: case 0x31: case 0x32: case 0x35: return 2;
    case 0x06: case 0x07: case 0x12: case 0x1E: case 0x2E: case 0x2F: case 0x3A: case 0x3B:
    case 0x3C: case 0x3F: case 0x40: case 0x41: return 4;
    default: return 0;
    }
}
static int fmt_linear(uint32_t c)
{
    return (c >= 0x10 && c <= 0x13) || c == 0x17 || (c >= 0x1B && c <= 0x20) || c == 0x24 || c == 0x25
        || (c >= 0x2E && c <= 0x31) || c == 0x35 || (c >= 0x3F && c <= 0x41);
}

/* The guest bytes a texture occupies: every stored mip level (swizzled, DXT)
 * or pitch * height (linear). 0 when unknown (the sampled check is used). */
static size_t tex_span(const Nv2aTexture *t)
{
    uint32_t c = t->color, w = t->width, h = t->height, lv, nl = t->levels ? t->levels : 1;
    size_t n = 0;
    if (fmt_linear(c))
        return (size_t)(t->pitch ? t->pitch : w * fmt_bytes(c)) * h;
    for (lv = 0; lv < nl && lv < 16; lv++) {
        if (c == 0x0C) n += (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8;
        else if (c == 0x0E || c == 0x0F) n += (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
        else if (fmt_bytes(c)) n += (size_t)w * h * fmt_bytes(c);
        else return 0;
        if (t->cube) break;               /* faces follow; level 0 face only (sampled as +X) */
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    return n;
}

/* Change detector over the texture's guest bytes (the fallback: 64 sampled
 * words plus the first 16, over w*h/2 bytes, inside every format). CJ_GL_TEXHASH=1 (default)
 * hashes every byte the texture occupies, as an emulator that tracks dirty
 * pages would see every write. The old check (CJ_GL_TEXHASH=0) sampled 80
 * words over half the span: a title rewriting part of a texture (glyphs in a
 * font page, a HUD element) between draws kept the stale copy whenever no
 * sampled word changed, which shows as half-updated or flickering text. */
static uint64_t s_texhash_bytes, s_texhash_caught;
static uint64_t sampled_sig(const Nv2aTexture *t)
{
    const uint8_t *m = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    size_t span = (size_t)t->width * t->height / 2, i;
    uint64_t h = 0xCBF29CE484222325ull ^ t->filter;
    if (span < 64) span = 64;
    span &= ~(size_t)3;
    for (i = 0; i < 64; i++) { h ^= *(const uint32_t *)(m + (span - 4) * i / 63); h *= 0x100000001B3ull; }
    for (i = 0; i < 16; i++) { h ^= *(const uint32_t *)(m + 4 * i); h *= 0x100000001B3ull; }
    return h;
}
static int gen_mips_for(const Nv2aTexture *t);
static uint64_t raw_sig(const Nv2aTexture *t)
{
    const uint8_t *m = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    size_t span = (size_t)t->width * t->height / 2, i, full;
    /* Not the FILTER word: sampler state is applied on every bind and does
     * not change the decoded texels, and keying on it re-uploaded a texture
     * each time it was drawn with a different filter or LOD bias. Only the
     * generated-mips choice (CJ_GL_GENMIPS) depends on it. */
    uint64_t h = 0xCBF29CE484222325ull ^ (uint64_t)gen_mips_for(t);
    full = s_texhash_full ? tex_span(t) : 0;
    if (full && t->offset < 0x04000000u && full > 0x04000000u - t->offset)
        full = 0x04000000u - t->offset;          /* stay inside guest RAM */
    if (full) {
        /* Four independent lanes so the multiplies overlap: this runs on
         * every bind of every texture, and with two dependent lanes it was a
         * few milliseconds of each frame in busy scenes. */
        const uint64_t *q = (const uint64_t *)m;
        size_t nq = full / 8;
        uint64_t a = h, b = 0x9E3779B97F4A7C15ull, c = 0xC2B2AE3D27D4EB4Full, d = 0x165667B19E3779F9ull;
        for (i = 0; i + 3 < nq; i += 4) {
            a = (a ^ q[i])     * 0x100000001B3ull;
            b = (b ^ q[i + 1]) * 0xC2B2AE3D27D4EB4Full;
            c = (c ^ q[i + 2]) * 0x9E3779B97F4A7C15ull;
            d = (d ^ q[i + 3]) * 0xFF51AFD7ED558CCDull;
        }
        for (; i < nq; i++) a = (a ^ q[i]) * 0x100000001B3ull;
        for (i = nq * 8; i < full; i++) b = (b ^ m[i]) * 0x100000001B3ull;
        s_texhash_bytes += full;
        return a ^ (b >> 1) ^ (c >> 3) ^ (d << 7) ^ ((uint64_t)full << 32);
    }
    if (span < 64) span = 64;
    span &= ~(size_t)3;
    for (i = 0; i < 64; i++) { h ^= *(const uint32_t *)(m + (span - 4) * i / 63); h *= 0x100000001B3ull; }
    for (i = 0; i < 16; i++) { h ^= *(const uint32_t *)(m + 4 * i); h *= 0x100000001B3ull; }
    return h;
}

static uint64_t s_tex_hits;

/* NV2A addressing: 1 wrap, 2 mirror, 3 clamp to edge, 4 border (the border
 * colour when FORMAT's BORDER_SOURCE says so, else the edge), 5 clamp (GL's
 * old GL_CLAMP: half border, half edge; edge is the close match). */
static GLint wrap_mode(uint32_t a, int border)
{
    return a == 1 ? GL_REPEAT : a == 2 ? GL_MIRRORED_REPEAT
         : (a == 4 && border) ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
}


/* Number of levels to upload: the format word's MIPMAP_LEVELS for a swizzled
 * or DXT 2D texture, else 1 (linear textures have no mips; cube maps are
 * sampled as their +X face, level 0). */
static uint32_t tex_levels(const Nv2aTexture *t)
{
    uint32_t l = t->levels ? t->levels : 1, maxl = 1, d = t->width > t->height ? t->width : t->height;
    if (!s_mips_on || t->cube || !fmt_normalised(t->color))
        return 1;
    while (d > 1) { d >>= 1; maxl++; }
    return l < maxl ? l : maxl;
}

/* Decode and upload every level of `t` into the bound GL_TEXTURE_2D. Returns
 * the number of levels uploaded (0: level 0 failed). */
/* CJ_GL_GENMIPS=1 (enhancement, off by default: the original has none):
 * Concrete Jungle's textures are almost all single-level (FORMAT
 * MIPMAP_LEVELS = 1) while the title asks for mip filtering (MIN 3..6), so
 * on the Xbox they are plain bilinear and fine patterns (floors, carpet,
 * wall trim) crawl as the camera moves. With this on, single-level
 * normalised textures that ask for a mip filter get a generated chain. */
static int s_genmips = -1;
static int gen_mips_for(const Nv2aTexture *t)
{
    uint32_t min = (t->filter >> 16) & 0xFF;
    if (s_genmips < 0) { const char *e = getenv("CJ_GL_GENMIPS"); s_genmips = e && *e == '1'; }
    return s_genmips && !t->cube && fmt_normalised(t->color) && (t->levels ? t->levels : 1) == 1
        && min >= 3 && min <= 6 && t->width >= 4 && t->height >= 4;
}
static uint64_t s_genmip_textures;

/* DXT1/3/5 straight to the GPU. The NV2A's DXT blocks are the standard S3TC
 * ones (linear, not swizzled), so there is nothing to convert: the CPU path
 * decoded every texel through the software sampler, and in dense areas where
 * the title streams textures in that was most of the frame. */
static uint32_t upload_dxt(const Nv2aTexture *t, uint32_t nl)
{
    uint32_t bb = t->color == 0x0C ? 8 : 16, lv;
    GLenum fmt = t->color == 0x0C ? 0x83F1 : t->color == 0x0E ? 0x83F2 : 0x83F3;   /* RGBA_S3TC_DXT1/3/5 */
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    for (lv = 0; lv < nl; lv++) {
        Nv2aTexture lt;
        const Nv2aTexture *src = t;
        size_t bytes;
        if (lv) {
            if (!nv2a_backend_texture_level(t, lv, &lt)) break;
            src = &lt;
        }
        bytes = (size_t)((src->width + 3) / 4) * ((src->height + 3) / 4) * bb;
        {   /* inside guest RAM: the low 64 MB or its contiguous view at 0x80000000 */
            uint32_t a = src->offset & 0x7FFFFFFFu;
            if (a >= 0x04000000u || bytes > 0x04000000u - a) break;
        }
        glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)lv, fmt, src->width, src->height, 0,
                               (GLsizei)bytes, mem + src->offset);
    }
    if (lv) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)lv - 1);
        if (lv > 1) { s_mip_textures++; s_mip_levels += lv; }
        s_s3tc_uploads++;
    }
    return lv;
}

/* 32-bit ARGB textures without the per-texel software sampler: a swizzled
 * one is unswizzled in one pass, a linear one copied by rows. The texels are
 * already the B,G,R,A bytes GL_BGRA reads. CJ_GL_FASTDECODE=0 turns it off. */
static int s_fastdec = -1;
static uint64_t s_fastdec_n;
static int fast_decode32(const Nv2aTexture *t, uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t w = t->width, h = t->height, a = t->offset & 0x7FFFFFFFu, x, y;
    int sw = t->color == 0x06 || t->color == 0x07, opaque = t->color == 0x07 || t->color == 0x1E;
    size_t bytes;
    if (s_fastdec < 0) { const char *e = getenv("CJ_GL_FASTDECODE"); s_fastdec = !(e && *e == '0'); }
    if (!s_fastdec || !(sw || t->color == 0x12 || t->color == 0x1E) || t->cube)
        return 0;
    if (sw && ((w & (w - 1)) || (h & (h - 1))))
        return 0;
    if (!sw && t->pitch < w * 4)
        return 0;
    bytes = sw ? (size_t)w * h * 4 : (size_t)t->pitch * (h - 1) + w * 4;
    if (a >= 0x04000000u || bytes > 0x04000000u - a)
        return 0;
    if (sw)
        xbox_unswizzle_rect(out, mem + t->offset, w, h, 4);
    else
        for (y = 0; y < h; y++)
            memcpy(out + (size_t)y * w, mem + t->offset + (size_t)y * t->pitch, (size_t)w * 4);
    if (opaque)
        for (x = 0; x < w * h; x++) out[x] |= 0xFF000000u;
    s_fastdec_n++;
    return 1;
}

/* Texture packs (src/texpack.c): before the normal upload, dump level 0 and/or
 * use a replacement image. Returns 1 when a replacement was uploaded (with a
 * generated mip chain), 0 to go on with the normal upload. */
extern int texpack_active(void);
extern uint64_t texpack_key(const void *data, size_t n, uint32_t fmt, uint32_t w, uint32_t h);
extern int texpack_want_dump(uint64_t key);
extern void texpack_dump(uint64_t key, uint32_t w, uint32_t h, uint32_t fmt, const uint32_t *argb);
extern const uint8_t *texpack_find(uint64_t key, int *w, int *h);
extern int texpack_skip_format(uint32_t fmt);
static int s_fastdec;
static int fast_decode32(const Nv2aTexture *t, uint32_t *out);
static uint32_t s_texlog_until, s_texlog_off[4096];
static int s_texlog_n;
static int texpack_upload(const Nv2aTexture *t)
{
    Nv2aTexture l0 = *t;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset(), *rgba;
    uint32_t a = t->offset & 0x7FFFFFFFu;
    size_t n;
    uint64_t key;
    int rw, rh;
    if (!texpack_active() || t->cube || texpack_skip_format(t->color)) return 0;
    l0.levels = 1;
    n = tex_span(&l0);
    if (!n || a >= 0x04000000u || n > 0x04000000u - a) return 0;
    key = texpack_key(mem + t->offset, n, t->color, t->width, t->height);
    if ((rgba = texpack_find(key, &rw, &rh))) {
        uint32_t d = (uint32_t)(rw > rh ? rw : rh), nl = 1;
        while (d > 1) { d >>= 1; nl++; }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)nl - 1);
        glGenerateMipmap(GL_TEXTURE_2D);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return 1;
    }
    if (texpack_want_dump(key)) {
        uint32_t *px = malloc((size_t)t->width * t->height * 4);
        if (px && (fast_decode32(&l0, px) || nv2a_backend_decode_texture(&l0, px)))
            texpack_dump(key, t->width, t->height, t->color, px);
        free(px);
    }
    return 0;
}

static uint32_t upload_levels(const Nv2aTexture *t)
{
    uint32_t nl = tex_levels(t), lv;
    if (s_s3tc && (t->color == 0x0C || t->color == 0x0E || t->color == 0x0F) && !t->cube && !gen_mips_for(t))
        return upload_dxt(t, nl);
    for (lv = 0; lv < nl; lv++) {
        Nv2aTexture lt;
        const Nv2aTexture *src = t;
        if (lv) {
            if (!nv2a_backend_texture_level(t, lv, &lt)) break;
            src = &lt;
        }
        if (!(s_texbuf = realloc(s_texbuf, (size_t)src->width * src->height * 4))
            || !(fast_decode32(src, s_texbuf) || nv2a_backend_decode_texture(src, s_texbuf)))
            break;
        if (getenv("CJ_GL_FASTDECODE_CHECK") && s_fastdec > 0) {   /* diagnostic: both decoders agree */
            static int nck, nbad;
            uint32_t *ref = malloc((size_t)src->width * src->height * 4);
            if (ref && nck < 400 && nv2a_backend_decode_texture(src, ref)) {
                size_t k, n = (size_t)src->width * src->height, diff = 0;
                Nv2aTexture probe = *src;
                for (k = 0; k < n; k++) diff += ref[k] != s_texbuf[k];
                if (fast_decode32(&probe, ref)) {
                    nck++;
                    if (diff && nbad++ < 20)
                        fprintf(stderr, "[FASTDEC] MISMATCH fmt %02X %ux%u at %08X: %zu of %zu texels\n",
                                src->color, src->width, src->height, src->offset, diff, n);
                    if (nck % 50 == 0) fprintf(stderr, "[FASTDEC] %d textures checked, %d mismatched\n", nck, nbad);
                }
            }
            free(ref);
        }
        glTexImage2D(GL_TEXTURE_2D, (GLint)lv, GL_RGBA8, src->width, src->height, 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, s_texbuf);
    }
    if (lv == 1 && gen_mips_for(t)) {
        uint32_t d = t->width > t->height ? t->width : t->height, n = 1;
        while (d > 1) { d >>= 1; n++; }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)n - 1);
        glGenerateMipmap(GL_TEXTURE_2D);
        s_genmip_textures++;
        return lv;
    }
    if (lv) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)lv - 1);
        if (lv > 1) { s_mip_textures++; s_mip_levels += lv; }
    }
    return lv;
}

/* CJ_GL_TEXSTATS=1: histogram of (format, mip levels, min filter) over every
 * bind, printed with the [GL] tex line (which textures get mipmaps). */
static int s_texstats = -1;
static uint64_t s_ts_n[64][16][8];
static void texstats_note(const Nv2aTexture *t)
{
    if (s_texstats < 0) s_texstats = getenv("CJ_GL_TEXSTATS") != NULL;
    if (!s_texstats) return;
    uint32_t l = t->levels > 15 ? 15 : t->levels, m = (t->filter >> 16) & 7;
    s_ts_n[t->color & 63][l][m]++;
}
static void texstats_print(void)
{
    if (s_texstats <= 0) return;
    for (int c = 0; c < 64; c++)
        for (int l = 0; l < 16; l++)
            for (int m = 0; m < 8; m++)
                if (s_ts_n[c][l][m])
                    fprintf(stderr, "[TEXSTATS] fmt %02X levels %d minfilter %d: %llu binds\n", c, l, m,
                            (unsigned long long)s_ts_n[c][l][m]);
}

static int s_bind_rm;   /* the texture just bound is a texture-pack replacement (has mips) */
static int decode_bind(const Nv2aTexture *t)
{
    texstats_note(t);
    size_t n = (size_t)t->width * t->height;
    TexEnt *e = NULL;
    uint64_t h;
    int i, lru = 0;
    uint32_t nl = tex_levels(t);

    if (!t->width || !t->height || n > 4096u * 4096u)
        return 0;
    {   /* PC button prompts (src/glyphs.c): our art in place of a ~X icon texture */
        extern int glyph_override(uint32_t off);
        extern const unsigned char *glyph_art_px(int i, int *w, int *h);
        static GLuint gt[64];
        int a = glyph_override(t->offset);
        if (a >= 0 && a < 64) {
            if (!gt[a]) {
                int w, h;
                const unsigned char *px = glyph_art_px(a, &w, &h);
                glGenTextures(1, &gt[a]);
                glBindTexture(GL_TEXTURE_2D, gt[a]);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glGenerateMipmap(GL_TEXTURE_2D);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            }
            glBindTexture(GL_TEXTURE_2D, gt[a]);
            s_bind_rm = 1;
            return 1;
        }
    }
    if (s_texcache) {
        unsigned b = tc_bucket(t->offset, t->color, t->width, t->height, nl);
        h = raw_sig(t);
        if (!s_tc_init) {
            for (i = 0; i < NTEX_BUCKETS; i++) s_tc_head[i] = -1;
            for (i = 0; i < NTEX; i++) s_tc[i].next = -1;
            s_tc_init = 1;
        }
        for (i = s_tc_head[b]; i >= 0; i = s_tc[i].next)
            if (s_tc[i].off == t->offset && s_tc[i].color == t->color && s_tc[i].w == t->width
                && s_tc[i].h == t->height && s_tc[i].levels == nl) { e = &s_tc[i]; break; }
        if (!e) {
            int *pp;
            for (i = 0; i < NTEX; i++) {          /* a free slot, else the least recently used */
                if (!s_tc[i].tex) { lru = i; break; }
                if (s_tc[i].used < s_tc[lru].used) lru = i;
            }
            e = &s_tc[lru];
            if (e->tex) {                          /* unlink it from its old bucket */
                s_tc_evictions++;
                for (pp = &s_tc_head[tc_bucket(e->off, e->color, e->w, e->h, e->levels)]; *pp >= 0; pp = &s_tc[*pp].next)
                    if (*pp == lru) { *pp = e->next; break; }
            } else {
                glGenTextures(1, &e->tex);
            }
            e->off = t->offset; e->color = t->color; e->w = t->width; e->h = t->height; e->levels = nl;
            e->hash = 0;
            e->next = s_tc_head[b];
            s_tc_head[b] = lru;
        }
        e->used = ++s_tick;
        if (s_texlog_until > s_flips) {   /* dev: texlog_now file; list every texture bound for a few frames */
            int j, seen = 0;
            for (j = 0; j < s_texlog_n; j++) if (s_texlog_off[j] == t->offset) { seen = 1; break; }
            if (!seen && s_texlog_n < (int)(sizeof s_texlog_off / sizeof s_texlog_off[0])) {
                Nv2aTexture l0 = *t;
                size_t n;
                l0.levels = 1;
                n = tex_span(&l0);
                s_texlog_off[s_texlog_n++] = t->offset;
                if (n && (t->offset & 0x7FFFFFFFu) < 0x04000000u)
                    fprintf(stderr, "[TEXLOG] %016llx %ux%u f%02X\n",
                            (unsigned long long)texpack_key((const uint8_t *)xbox_GetMemoryOffset() + t->offset, n, t->color, t->width, t->height),
                            t->width, t->height, t->color);
            }
        }
        glBindTexture(GL_TEXTURE_2D, e->tex);
        if (s_texhash_full) {                 /* evidence: changes the sampled check would miss */
            uint64_t sh = sampled_sig(t);
            if (e->hash && e->hash != h && e->shash == sh) s_texhash_caught++;
            e->shash = sh;
        }
        if (e->hash != h) {
            double tu0 = now_s();
            int upl;
            e->rm = texpack_upload(t);
            upl = e->rm ? 1 : (int)upload_levels(t);
            s_upload_time += now_s() - tu0; s_upload_n++;
            if (!upl) {
                static uint32_t seen[64]; static int nseen; int j;
                for (j = 0; j < nseen && seen[j] != t->color; j++) ;
                if (j == nseen && nseen < 64) {
                    seen[nseen++] = t->color;
                    fprintf(stderr, "[GL] cannot decode texture format 0x%02X (%ux%u at %08X)\n",
                            t->color, t->width, t->height, t->offset);
                }
                e->hash = 0;
                return 0;
            }
            s_tex_decodes++;
            e->hash = h;
        } else
            s_tex_hits++;
        s_bind_rm = e->rm;
    } else {
        s_bind_rm = 0;
        glBindTexture(GL_TEXTURE_2D, s_tex);
        if (!upload_levels(t))
            return 0;
        s_tex_decodes++;
    }
    return 1;
}

/* Sampler state from SET_TEXTURE_FILTER / CONTROL0 / ADDRESS / BORDER_COLOR
 * on the texture bound to the active unit. FILTER: LOD bias signed 13-bit 5.8
 * fixed point in bits 0-12, MIN in 16-23 (1 point, 2 linear, 3 point mip
 * point, 4 linear mip point, 5 point mip linear, 6 linear mip linear,
 * 7 convolution at LOD 0), MAG in 24-27 (1 point, 2 linear, 4 convolution).
 * CONTROL0: MAX_ANISOTROPY log2 in bits 4-5, MAX_LOD_CLAMP 6-17 and
 * MIN_LOD_CLAMP 18-29 (4.8). `mips`: the texture has a mip chain (an FBO or
 * a single-level texture gets the non-mip form of the min filter). */
static void apply_sampler(const Nv2aTexture *t, int rtt, int mips)
{
    uint32_t min = (t->filter >> 16) & 0xFF, mag = (t->filter >> 24) & 0xF;
    GLint gmin, gmag;
    int32_t bias = (int32_t)(t->filter & 0x1FFF);
    if (bias & 0x1000) bias -= 0x2000;
    switch (min) {
    case 1:  gmin = GL_NEAREST; break;
    case 3:  gmin = mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST; break;
    case 4:  gmin = mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR; break;
    case 5:  gmin = mips ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST; break;
    case 6:  gmin = mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR; break;
    default: gmin = GL_LINEAR; break;          /* 2, 7 (convolution), unset */
    }
    gmag = mag == 1 ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gmin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gmag);
    if (rtt) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        int border = (int)t->border_color;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(t->addr_u, border));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(t->addr_v, border));
        if (border && (t->addr_u == 4 || t->addr_v == 4)) {
            float bc[4];
            bc[0] = ((t->border >> 16) & 0xFF) / 255.f; bc[1] = ((t->border >> 8) & 0xFF) / 255.f;
            bc[2] = (t->border & 0xFF) / 255.f;         bc[3] = (t->border >> 24) / 255.f;
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc);
        }
    }
    if (mips) {
        float lo = (float)((t->control0 >> 18) & 0xFFF) / 256.f, hi = (float)((t->control0 >> 6) & 0xFFF) / 256.f;
        if (!(t->control0 & 0x3FFFFFC0u) || hi < lo) { lo = -1000.f; hi = 1000.f; }
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, lo);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, hi);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, (float)bias / 256.f + s_lod_bias_add);
    }
    if (s_aniso_max > 0.f) {
        float a = (float)(1u << ((t->control0 >> 4) & 3));
        if (s_aniso_force > 0) a = (float)s_aniso_force;
        if (!mips || gmin == GL_NEAREST || gmin == GL_NEAREST_MIPMAP_NEAREST) a = 1.f;
        if (a > s_aniso_max) a = s_aniso_max;
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, a);
    }
}

/* A draw that samples the surface it renders into (PCJ's focus blur smooths
 * its 256x256 texture in place; the motion blur and the pause background read
 * the back buffer while drawing to it). The NV2A reads the target's memory,
 * which holds what was drawn before this draw. Here the target is an FBO
 * whose pixels never reach guest memory, so the stage used to decode stale
 * guest memory instead: the blur read whatever the CPU last left there.
 * Sample a copy of the FBO taken before the draw. One copy per draw serves
 * every stage that names the target. CJ_GL_SELF_COPY=0 turns it off. */
static GLuint s_self_tex;
static uint32_t s_self_w, s_self_h;
static uint64_t s_self_draw = ~0ull, s_self_copies;
static int s_self_on = -1;
static GLuint self_copy(Rt *r)
{
    if (s_self_draw == s_draws)
        return s_self_tex;
    if (!s_self_tex) glGenTextures(1, &s_self_tex);
    glBindTexture(GL_TEXTURE_2D, s_self_tex);
    if (s_self_w != r->w || s_self_h != r->h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r->w * s_rs, r->h * s_rs, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        s_self_w = r->w; s_self_h = r->h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo2[s_side]);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, r->w * s_rs, r->h * s_rs);
    s_self_draw = s_draws;
    s_self_copies++;
    return s_self_tex;
}

/* A texture over a zeta surface: the title reading its depth buffer as
 * colour. PCJ's focus blur samples its D24S8 z-buffer as a linear A8B8G8R8
 * texture (stencil in R, depth bits 0-7, 8-15, 16-23 in G, B, A) and turns
 * the depth into the blur amount. Depth lives in the FBO here, never in guest
 * memory, so the stage decoded whatever the z-buffer's memory last held: the
 * blur was off, or, where that memory held stale frames, came and went.
 * Read the depth of the surface that last drew with that zeta back as
 * GL_UNSIGNED_INT_24_8 (the same (z24 << 8) | stencil word the NV2A stores,
 * since z / zmax is the GL depth here) and sample those bytes. Rows come back
 * bottom-up, as an FBO texture is sampled. CJ_GL_ZETA_TEX=0 turns it off. */
static GLuint s_ztex;
static uint32_t *s_zbuf;
static size_t s_zbuf_n;
static uint64_t s_zeta_draw = ~0ull, s_zeta_reads;
static int s_zeta_on = -1;
static uint32_t s_ztw, s_zth;

static Rt *rt_for_zeta(const Nv2aTexture *t, uint32_t *x0, uint32_t *y0)
{
    Rt *best = NULL;
    uint32_t off = t->offset & 0x0FFFFFFFu;
    int i;
    for (i = 0; i < s_nrt; i++) {
        Rt *r = &s_rt[i];
        uint32_t z = r->zeta & 0x0FFFFFFFu, pitch = r->w * 4, d, rows = r->ch ? r->ch : r->h;
        if (!r->zeta || s_flips - r->drawn_flip > 1 || off < z || off >= z + pitch * rows)
            continue;
        d = off - z;
        if ((d % pitch) / 4 + t->width > r->w || d / pitch + t->height > rows)
            continue;
        if (!best || r->last_bind > best->last_bind) {
            best = r;
            *y0 = d / pitch;
            *x0 = (d % pitch) / 4;
        }
    }
    return best;
}

static void zeta_bind(Rt *r, uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th)
{
    size_t n = (size_t)tw * th;
    if (s_zeta_draw == s_draws && s_ztw == tw && s_zth == th) {
        glBindTexture(GL_TEXTURE_2D, s_ztex);
        return;
    }
    if (n > s_zbuf_n) {
        free(s_zbuf);
        s_zbuf = (uint32_t *)malloc(n * 4);
        s_zbuf_n = s_zbuf ? n : 0;
        if (!s_zbuf) return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo2[s_side]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (s_rs == 1)
        glReadPixels((GLint)x0, (GLint)(r->h - y0 - th), (GLsizei)tw, (GLsizei)th,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, s_zbuf);
    else {                                  /* scaled FBO: read it whole size, keep one sample per guest pixel */
        uint32_t *big = malloc(n * 4 * (size_t)s_rs * s_rs), x, y;
        if (!big) return;
        glReadPixels((GLint)(x0 * s_rs), (GLint)((r->h - y0 - th) * s_rs), (GLsizei)(tw * s_rs), (GLsizei)(th * s_rs),
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, big);
        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++)
                s_zbuf[(size_t)y * tw + x] = big[((size_t)y * s_rs + s_rs / 2) * tw * s_rs + x * s_rs + s_rs / 2];
        free(big);
    }
    if (s_last) glBindFramebuffer(GL_READ_FRAMEBUFFER, s_last->fbo2[s_side]);
    if (!s_ztex) glGenTextures(1, &s_ztex);
    glBindTexture(GL_TEXTURE_2D, s_ztex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)tw, (GLsizei)th, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_zbuf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    s_ztw = tw; s_zth = th;
    s_zeta_draw = s_draws;
    s_zeta_reads++;
}

static void dump_frame(Rt *r, uint32_t w, uint32_t h, const char *tag);
/* CJ_GL_DUMP_SRC=<va hex> (1: any): in a CJ_GL_DUMP_TRIGGER window, also dump
 * that surface each time a draw samples it, as it is then (up to 40 a flip;
 * <prefix>s<va>-<draw>-NNNNN.ppm). */
static void dump_src(Rt *r)
{
    static int on = -1;
    static uint32_t want;
    char tag[64];
    static uint32_t flip, n;
    if (on < 0) {
        const char *e = getenv("CJ_GL_DUMP_SRC");
        want = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
        on = want != 0;
    }
    static uint32_t cap = 0, skip = 0;
    if (!on || !s_dump || s_flips >= s_dump_until) return;
    if (want != 1 && (r->va & 0x0FFFFFFFu) != (want & 0x0FFFFFFFu)) return;
    if (!cap) {   /* CJ_GL_DUMP_SRC_MAX (40) per flip; CJ_GL_DUMP_SRC_SKIPVA=<va>: never that one */
        const char *m = getenv("CJ_GL_DUMP_SRC_MAX"), *k = getenv("CJ_GL_DUMP_SRC_SKIPVA");
        cap = m ? (uint32_t)strtoul(m, NULL, 0) : 40;
        skip = k ? (uint32_t)strtoul(k, NULL, 16) : 0;
    }
    if (skip && (r->va & 0x0FFFFFFFu) == (skip & 0x0FFFFFFFu)) return;
    if (flip != s_flips) { flip = s_flips; n = 0; }
    if (++n > cap) return;
    snprintf(tag, sizeof tag, "s%08X-%llu-", r->va, (unsigned long long)s_draws);
    dump_frame(r, r->w, r->h, tag);
    {   /* and its alpha, as a PGM */
        char path[512];
        uint8_t *px = malloc((size_t)r->w * r->h * 4);
        FILE *f;
        uint32_t y, x;
        if (px) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo2[s_side]);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            snprintf(path, sizeof path, "%s%sa%05u.pgm", s_dump, tag, s_flips);
            if ((f = fopen(path, "wb"))) {
                fprintf(f, "P5\n%u %u\n255\n", r->w, r->h);
                for (y = r->h; y-- > 0;)
                    for (x = 0; x < r->w; x++) fputc(px[((size_t)y * r->w + x) * 4 + 3], f);
                fclose(f);
            }
            free(px);
        }
    }
    if (s_last) glBindFramebuffer(GL_READ_FRAMEBUFFER, s_last->fbo2[s_side]);
}

/* Bind stage `st`'s texture to unit st and set its coordinate transform:
 * GL coords = texel coords * tx.xy + tx.zw. */
static int bind_stage(int st, const Nv2aTexture *t, float tx[4], float *aone)
{
    uint32_t x0, y0;
    Rt *r = NULL;
    glActiveTexture(GL_TEXTURE0 + st);
    *aone = 0.0f;
    if (s_self_on < 0) s_self_on = !getenv("CJ_GL_SELF_COPY") || atoi(getenv("CJ_GL_SELF_COPY")) != 0;
    if (s_zeta_on < 0) s_zeta_on = !getenv("CJ_GL_ZETA_TEX") || atoi(getenv("CJ_GL_ZETA_TEX")) != 0;
    if (s_rtt && (r = rt_for_texture(t, &x0, &y0)) && (r != s_last || s_self_on)) {
        dump_src(r);
        if (r == s_last)
            self_copy(r);   /* binds the copy on this unit */
        else
            glBindTexture(GL_TEXTURE_2D, r->color2[s_side]);
        tx[0] = 1.0f / r->w;  tx[2] = (float)x0 / r->w;
        tx[1] = -1.0f / r->h; tx[3] = 1.0f - (float)y0 / r->h;
        *aone = fmt_no_alpha(t->color) ? 1.0f : 0.0f;
        s_rtt_binds++;
        s_last->nd_rtt++;
    } else if (s_zeta_on && (r = rt_for_zeta(t, &x0, &y0)) != NULL) {
        zeta_bind(r, x0, y0, t->width, t->height);
        tx[0] = 1.0f / t->width;  tx[2] = 0.0f;
        tx[1] = -1.0f / t->height; tx[3] = 1.0f;
    } else {
        if (!decode_bind(t))
            return 0;
        tx[0] = 1.0f / t->width; tx[1] = 1.0f / t->height; tx[2] = tx[3] = 0.0f;
    }
    {   /* CJ_GL_BINDLOG=<target va hex>: in a dump window, how each stage of a
         * draw into that surface was bound */
        static int on = -1;
        static uint32_t want;
        if (on < 0) { const char *e = getenv("CJ_GL_BINDLOG"); want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; on = want != 0; }
        if ((on && s_flips < s_dump_until && s_last && (s_last->va & 0x0FFFFFFFu) == (want & 0x0FFFFFFFu)) ||
            (s_burst_until && s_flips + 6 < s_burst_until))
            fprintf(stderr, "[BIND] flip %u draw %llu st %d tex %08X %ux%u f%02X -> %s %08X fbo %ux%u\n", s_flips,
                    (unsigned long long)s_draws, st, t->offset, t->width, t->height, t->color,
                    !r ? "decode" : r == s_last ? "self" : (r->zeta && (r->zeta & 0x0FFFFFFFu) <= (t->offset & 0x0FFFFFFFu)
                        && (t->offset & 0x0FFFFFFFu) < (r->zeta & 0x0FFFFFFFu) + r->w * 4 * r->h) ? "zeta" : "rtt",
                    r ? r->va : 0, r ? r->w : 0, r ? r->h : 0);
    }
    apply_sampler(t, r != NULL, !r && (tex_levels(t) > 1 || gen_mips_for(t) || s_bind_rm));
    return 1;
}

static void gl_clear1(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags, uint32_t argb, uint32_t zs)
{
    GLbitfield b = 0;
    {
        static int dl = -1;
        if (dl < 0) dl = getenv("CJ_GL_DRAWLOG") ? atoi(getenv("CJ_GL_DRAWLOG")) : 0;
        if ((dl && s_flips == (uint32_t)dl) || (s_flips + 1 < s_dump_until && getenv("CJ_GL_DRAWLOG_TRIGGER")) || (s_burst_until && s_flips + 6 < s_burst_until))
            fprintf(stderr, "[CLEAR] surf %08X clip %ux%u at %u,%u flags %X argb %08X zs %08X rect %X %X %X\n",
                    s->color_va, s->width, s->height, s->clip_x, s->clip_y, flags, argb, zs,
                    rs ? rs->clear_rect_h : 0, rs ? rs->clear_rect_v : 0, rs ? rs->clear_rect_set : 0);
    }
    bind_rt(s);
    s_last->drawn_flip = s_flips;
    if (rs && rs->zeta_va)
        s_last->zeta = rs->zeta_va;
    /* CLEAR_SURFACE covers CLEAR_RECT when the title set one, else the clip */
    if (rs && (rs->clear_rect_set & 3) == 3)
        scissor_guest((int)(rs->clear_rect_h & 0xFFFF), (int)(rs->clear_rect_v & 0xFFFF),
                      (int)(rs->clear_rect_h >> 16) + 1, (int)(rs->clear_rect_v >> 16) + 1);
    else
        scissor_guest((int)s->clip_x, (int)s->clip_y, (int)(s->clip_x + s->width), (int)(s->clip_y + s->height));
    if (flags & 0xF0) {
        int xa = xalpha(s);
        glColorMask(!!(flags & 0x10), !!(flags & 0x20), !!(flags & 0x40), xa >= 0 ? GL_TRUE : !!(flags & 0x80));
        glClearColor(((argb >> 16) & 0xFF) / 255.f, ((argb >> 8) & 0xFF) / 255.f,
                     (argb & 0xFF) / 255.f, xa >= 0 ? (float)xa : (argb >> 24) / 255.f);
        b |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 1) { glDepthMask(GL_TRUE); glClearDepth((zs >> 8) / 16777215.0); b |= GL_DEPTH_BUFFER_BIT; }
    if (flags & 2) { glStencilMask(0xFF); glClearStencil(zs & 0xFF); b |= GL_STENCIL_BUFFER_BIT; }
    if (b)
        glClear(b);
}

static void gl_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zs)
{
    if (!init())
        return;
    for (s_side = 0; s_side <= s_ab; s_side++)
        gl_clear1(s, rs, flags, argb, zs);
    s_side = 0;
}

static void unpack4(uint32_t c, float *o)
{
    o[0] = ((c >> 16) & 0xFF) / 255.f; o[1] = ((c >> 8) & 0xFF) / 255.f;
    o[2] = (c & 0xFF) / 255.f;         o[3] = (c >> 24) / 255.f;
}

static void gl_draw1(const Nv2aSurface *s, const Nv2aBatch *b)
{
    int st, i, use_rc;
    float tx[4][4], aone[4] = {0}, tn[4] = {0}, twh[4][4];
    bind_rt(s);
    s_last->drawn_flip = s_flips;
    if (b->state->zeta_va)
        s_last->zeta = b->state->zeta_va;
    {
        static int dl = -1;
        if (dl < 0) dl = getenv("CJ_GL_DRAWLOG") ? atoi(getenv("CJ_GL_DRAWLOG")) : 0;
        static int dls = -1;
        static uint32_t dls_va;
        if (dls < 0) { const char *e = getenv("CJ_GL_DRAWLOG_SURF"); dls_va = e ? (uint32_t)strtoul(e, NULL, 16) : 0; dls = dls_va != 0; }
        if ((dl && s_flips == (uint32_t)dl) || (s_flips + 1 < s_dump_until && getenv("CJ_GL_DRAWLOG_TRIGGER")) || (s_burst_until && s_flips + 6 < s_burst_until)
            || (dls && s_flips < s_dump_until && (s->color_va & 0x0FFFFFFFu) == (dls_va & 0x0FFFFFFFu))) {
            const Nv2aRenderState *r = b->state;
            if (b->rc)
                fprintf(stderr, "[RC] flip %u stages: c_icw %08X %08X a_icw %08X %08X c_ocw %08X %08X a_ocw %08X %08X f0 %08X %08X\n",
                        s_flips, b->rc->color_icw[0], b->rc->color_icw[1], b->rc->alpha_icw[0], b->rc->alpha_icw[1],
                        b->rc->color_ocw[0], b->rc->color_ocw[1], b->rc->alpha_ocw[0], b->rc->alpha_ocw[1],
                        b->rc->factor0[0], b->rc->factor1[0]);
            int k;
            if (b->vsh)
                fprintf(stderr, "[DRAW] surf %08X clip %ux%u at %u,%u n %u vsh %d (%u instr, %u rows, arrays %04X)",
                        s->color_va, s->width, s->height, s->clip_x, s->clip_y, b->count, s_vcur, b->vsh->ninstr, b->vsh->nverts,
                        b->vsh->array_mask);
            if (b->vsh)
                fprintf(stderr, " c58 %g %g %g %g c59 %g %g %g %g",
                        b->vsh->constants[58][0], b->vsh->constants[58][1], b->vsh->constants[58][2], b->vsh->constants[58][3],
                        b->vsh->constants[59][0], b->vsh->constants[59][1], b->vsh->constants[59][2], b->vsh->constants[59][3]);
            if (b->vsh)
                fprintf(stderr, " fogen %u mode %X p %g %g fc0 %08X fc1 %08X f0 %08X f1 %08X zmax %g",
                        b->vsh->fog_enable, b->vsh->fog_mode, b->vsh->fog_param[0], b->vsh->fog_param[1],
                        b->rc ? b->rc->final_c0 : 0, b->rc ? b->rc->final_c1 : 0,
                        b->rc ? b->rc->final0 : 0, b->rc ? b->rc->final1 : 0, r->depth_max);
            else {
                const Nv2aVertex *v = b->vertices;
                fprintf(stderr, "[DRAW] surf %08X clip %ux%u n %u ext %d v0 (%.1f %.1f %.4g rhw %.4g) d %08X uv %.1f %.1f",
                        s->color_va, s->width, s->height, b->count, !!b->ext,
                        v[0].x, v[0].y, v[0].z, v[0].rhw, v[0].diffuse, v[0].u, v[0].v);
            }
            fprintf(stderr, " rc %d ctl %08X sp %05X blend %u %X/%X z %u/%X/%u zeta %08X at %u/%X/%u cull %u/%X/%X stc %u f%X r%X m%X w%X op %X/%X/%X cm %08X fog %08X",
                    !!b->rc, b->rc ? b->rc->control : 0,
                    b->rc ? b->rc->stage_program : 0, r->blend_enable, r->blend_src, r->blend_dst,
                    r->depth_test_enable, r->depth_func, r->depth_write, r->zeta_va, r->alpha_test_enable, r->alpha_func,
                    r->alpha_ref, r->cull_enable, r->cull_face, r->front_face, r->stencil_enable,
                    r->stencil_func, r->stencil_ref, r->stencil_func_mask, r->stencil_write_mask,
                    r->stencil_fail, r->stencil_zfail, r->stencil_zpass, r->color_mask, b->fog_color);
            fprintf(stderr, " ctl0 %08X po %u %g/%g wc %u", r->control0, r->poly_offset_fill, r->poly_offset_factor,
                    r->poly_offset_units, r->window_clip_type);
            for (k = 0; k < 8 && (r->window_clip_x[k] || r->window_clip_y[k]); k++)
                fprintf(stderr, " [%X %X]", r->window_clip_x[k], r->window_clip_y[k]);
            for (k = 0; k < 4; k++)
                if (b->stages[k])
                    fprintf(stderr, " t%d %08X %ux%u f%02X lv%u flt%08X c0%08X a%X/%X", k, b->stages[k]->offset,
                            b->stages[k]->width, b->stages[k]->height, b->stages[k]->color, b->stages[k]->levels,
                            b->stages[k]->filter, b->stages[k]->control0, b->stages[k]->addr_u, b->stages[k]->addr_v);
            if (b->ext)
                fprintf(stderr, " tc0 %.3f %.3f %.3f %.3f", b->ext[0].tc[0][0], b->ext[0].tc[0][1],
                        b->ext[0].tc[0][2], b->ext[0].tc[0][3]);
            fprintf(stderr, "\n");
        }
    }
    use_rc = s_rc_on && (b->ext || b->vsh) && b->rc;
    {
        static uint32_t ord;
        if (!s_last->nd_flip) s_last->order = ++ord;
        s_last->nd_flip++;
        if (!use_rc) s_last->nd_legacy++;
    }
    s_cur = b->vsh ? prog_for(b, s_vcur, !use_rc) : use_rc ? prog_for(b, -1, 0) : &s_legacy;
    glUseProgram(s_cur->prog);
    glUniform2f(s_cur->u_size, (float)s_last->w, (float)s_last->h);
    apply_state(b->state);
    apply_window_clip(s, b->state);
    scissor_guest((int)s->clip_x, (int)s->clip_y, (int)(s->clip_x + s->width), (int)(s->clip_y + s->height));
    memset(tx, 0, sizeof tx);
    memset(twh, 0, sizeof twh);
    if (use_rc) {
        float c[8][4], c1[8][4], f[4];
        for (st = 0; st < 4; st++) {
            uint32_t mode = (b->rc->stage_program >> (st * 5)) & 0x1F;
            const Nv2aTexture *t = b->stages[st];
            if (!t || (mode != 1 && mode != 2))
                continue;
            if (!bind_stage(st, t, tx[st], &aone[st])) {
                /* undecodable: white, as the CPU path */
                glActiveTexture(GL_TEXTURE0 + st);
                glBindTexture(GL_TEXTURE_2D, 0);
                continue;
            }
            tn[st] = fmt_normalised(t->color) ? 1.0f : 0.0f;
            twh[st][0] = (float)t->width; twh[st][1] = (float)t->height;
        }
        for (i = 0; i < 8; i++) { unpack4(b->rc->factor0[i], c[i]); unpack4(b->rc->factor1[i], c1[i]); }
        glUniform4fv(s_cur->u_c0, 8, &c[0][0]);
        glUniform4fv(s_cur->u_c1, 8, &c1[0][0]);
        unpack4(b->rc->final_c0, f); glUniform4fv(s_cur->u_fc0, 1, f);
        unpack4(b->rc->final_c1, f); glUniform4fv(s_cur->u_fc1, 1, f);
        /* FOG_COLOR is R in bits 0-7 */
        if (s_fog_argb)     /* diagnostic: read SET_FOG_COLOR as 0xAARRGGBB */
            glUniform3f(s_cur->u_fogc, ((b->fog_color >> 16) & 0xFF) / 255.f, ((b->fog_color >> 8) & 0xFF) / 255.f,
                        (b->fog_color & 0xFF) / 255.f);
        else
        glUniform3f(s_cur->u_fogc, (b->fog_color & 0xFF) / 255.f, ((b->fog_color >> 8) & 0xFF) / 255.f,
                    ((b->fog_color >> 16) & 0xFF) / 255.f);
        s_rc_draws++;
    } else {
        int textured = 0;
        if (b->texture)
            textured = bind_stage(0, b->texture, tx[0], &aone[0]);
        glUniform1i(s_cur->u_textured, textured);
        /* Nv2aVertex uv are texels for every format */
    }
    glUniform4fv(s_cur->u_tx, 4, &tx[0][0]);
    glUniform4fv(s_cur->u_twh, 4, &twh[0][0]);
    glUniform4fv(s_cur->u_tn, 1, tn);
    glUniform4fv(s_cur->u_aone, 1, aone);

    if (b->vsh) {
        vsh_draw(b);
        s_draws++;
        s_tris += b->count / 3;
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->count * sizeof(Nv2aVertex), b->vertices,
                 GL_STREAM_DRAW);
    if (use_rc) {
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo_ext);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->count * sizeof(Nv2aVertexExt), b->ext,
                     GL_STREAM_DRAW);
        for (i = 3; i <= 8; i++) glEnableVertexAttribArray(i);
    } else {
        for (i = 3; i <= 8; i++) glDisableVertexAttribArray(i);
    }
    xalpha_draw(s, s_last, b->state->color_mask);
    if (!s_nodraw) glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
    s_draws++;
    s_tris += b->count / 3;
}

/* Flicker capture, part 2: a compact record of every draw of the last 3 flips,
 * written to the log with each detected flicker so the frame that differs
 * can be compared draw by draw with its neighbours. */
typedef struct { uint32_t surf, n, tex0, fmt0, tex1, zs, bl, at, vsh, vtx; } DrawRec;
#define DREC_MAX 6000
static DrawRec *s_drec[3];
static uint32_t s_drec_n[3], s_drec_flip[3];
static unsigned s_drec_cur;
static void drec_note(const Nv2aSurface *s, const Nv2aBatch *b)
{
    DrawRec *r;
    const Nv2aRenderState *rs = b->state;
    if (s_flk <= 0) return;
    if (!s_drec[0]) {
        int i;
        for (i = 0; i < 3; i++) if (!(s_drec[i] = calloc(DREC_MAX, sizeof(DrawRec)))) { s_flk = 0; return; }
    }
    if (s_drec_n[s_drec_cur] >= DREC_MAX) return;
    r = &s_drec[s_drec_cur][s_drec_n[s_drec_cur]++];
    r->surf = s->color_va;
    r->n = b->count;
    r->tex0 = b->stages[0] ? b->stages[0]->offset : (b->texture ? b->texture->offset : 0);
    r->fmt0 = b->stages[0] ? b->stages[0]->color : (b->texture ? b->texture->color : 0xFF);
    r->tex1 = b->stages[1] ? b->stages[1]->offset : 0;
    r->zs = rs ? (rs->depth_test_enable << 24) | ((rs->depth_func & 0xFFF) << 8) | rs->depth_write : 0;
    r->bl = rs ? (rs->blend_enable << 31) | ((rs->blend_src & 0x7FFF) << 16) | (rs->blend_dst & 0xFFFF) : 0;
    r->at = rs ? (rs->alpha_test_enable << 24) | ((rs->alpha_func & 0xFFF) << 8) | (rs->alpha_ref & 0xFF) : 0;
    r->vsh = b->vsh ? 1u + (uint32_t)(s_vcur < 0 ? 0 : s_vcur) : 0;
    r->vtx = 0;
    if (!b->vsh && b->vertices) {
        uint32_t h = 2166136261u, k;
        const uint32_t *w = (const uint32_t *)&b->vertices[0];
        for (k = 0; k < 4; k++) { h ^= w[k]; h *= 16777619u; }
        r->vtx = h;
    }
}
static void drec_dump(unsigned idx_b)
{
    /* idx = which of the 3 slots: A (t-2), B (t-1, the odd frame), C (t) */
    static const char *nm[3] = { "A", "B", "C" };
    unsigned k, i;
    for (k = 0; k < 3; k++) {
        unsigned slot = (idx_b + 2 + k) % 3;   /* A = idx_b - 1 */
        fprintf(stderr, "[FLKDRAWS] %s flip %u: %u draws\n", nm[k], s_drec_flip[slot], s_drec_n[slot]);
        for (i = 0; i < s_drec_n[slot]; i++) {
            const DrawRec *r = &s_drec[slot][i];
            fprintf(stderr, "[FLKD] %s %4u surf %08X n %u tex %08X/%02X t1 %08X z %06X bl %08X at %06X vsh %u vtx %08X\n",
                    nm[k], i, r->surf, r->n, r->tex0, r->fmt0, r->tex1, r->zs, r->bl, r->at, r->vsh, r->vtx);
        }
    }
}

/* CJ_GL_FLIPLOG=1 (diagnostic): one line per flip with the draws per target. */
static int s_fliplog = -1;
static struct { uint32_t surf, n; } s_fl[16];
static int s_fln;
static void fliplog_note(uint32_t surf)
{
    int i;
    if (s_fliplog < 0) s_fliplog = getenv("CJ_GL_FLIPLOG") != NULL;
    if (!s_fliplog) return;
    for (i = 0; i < s_fln; i++) if (s_fl[i].surf == surf) { s_fl[i].n++; return; }
    if (s_fln < 16) { s_fl[s_fln].surf = surf; s_fl[s_fln].n = 1; s_fln++; }
}
static void fliplog_flip(void)
{
    char buf[512]; int bl = 0, i;
    extern uint32_t g_nv2a_flip_front_va;
    if (s_fliplog <= 0) return;
    for (i = 0; i < s_fln; i++) bl += snprintf(buf + bl, sizeof buf - bl, " %08X:%u", s_fl[i].surf, s_fl[i].n);
    fprintf(stderr, "[FLIPLOG] flip %u front %08X |%s\n", s_flips, g_nv2a_flip_front_va, buf);
    s_fln = 0;
}
static void gl_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    int lo = 0, hi = s_ab;
    if (!init() || !b->count)
        return;
    {   /* CJ_GL_SKIP=1 (test runs on a machine without a GPU, where drawing is ~90% of the time):
         * draw only inside a CJ_GL_DUMP_TRIGGER window, so screenshots are whole and the rest runs
         * at full speed. The game itself runs unchanged. */
        static int skip = -1;
        if (skip < 0) { const char *e = getenv("CJ_GL_SKIP"); skip = e && e[0] == '1'; if (skip) fprintf(stderr, "[GL] CJ_GL_SKIP=1: drawing only around screenshots\n"); }
        if (skip && s_flips >= s_dump_until) return;
    }
    drec_note(s, b);
    fliplog_note(s->color_va);
    double t0 = now_s();
    if (b->vsh) hi = 0;                 /* A/B: GPU-VS side only */
    else if (b->vsh_twin) lo = 1;       /* its CPU-VS twin: shadow side only */
    for (s_side = lo; s_side <= hi; s_side++)
        gl_draw1(s, b);
    s_side = 0;
    s_be_time += now_s() - t0;
}

static void dump_frame(Rt *r, uint32_t w, uint32_t h, const char *tag)
{
    char path[512];
    FILE *f;
    uint8_t *px = malloc((size_t)w * h * 3 * s_rs * s_rs), *row;
    uint32_t y;
    if (!px) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo2[s_side]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, (r->h - h) * s_rs, w * s_rs, h * s_rs, GL_RGB, GL_UNSIGNED_BYTE, px);
    if (s_rs > 1 && getenv("CJ_GL_DUMP_FULL")) { w *= s_rs; h *= s_rs; }   /* diagnostic: the scaled image itself */
    else if (s_rs > 1) {                /* dumps stay at guest size: one sample per guest pixel */
        uint32_t x, yy;
        for (yy = 0; yy < h; yy++)
            for (x = 0; x < w; x++)
                memmove(px + ((size_t)yy * w + x) * 3,
                        px + (((size_t)yy * s_rs + s_rs / 2) * w * s_rs + x * s_rs + s_rs / 2) * 3, 3);
    }
    snprintf(path, sizeof path, "%s%s%05u.ppm", s_dump, tag, s_flips);
    if ((f = fopen(path, "wb"))) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (y = h; y-- > 0;) {      /* GL rows are bottom-up */
            row = px + (size_t)y * w * 3;
            fwrite(row, 1, (size_t)w * 3, f);
        }
        fclose(f);
    }
    free(px);
}

/* Keyboard map (README-PC.txt): WASD left stick, IJKL right stick, arrows
 * D-pad, Space A, E B, R X, Q Y, Z black, C white, Left Shift LT, F RT,
 * Enter Start, Esc/Backspace Back, 1 L3, 2 R3. */
/* Mouse look (CJ_MOUSE=1; CJ_MOUSE_SENS degrees per 100 pixels, default 15;
 * CJ_MOUSE_INVERT=1 flips up/down). The camera turns by right-stick deflection
 * x a per-frame rate constant, so a mouse movement becomes full deflection with
 * the rate set to exactly the angle it asks for this frame (recomp_manual.c
 * cj_cam_mouse_rates): no stick curve, no dead zone, no turn-speed cap. The
 * cursor is captured while the window has focus; F10 releases or recaptures it. */
volatile int s_shot_req;
extern int stbi_write_png(const char *filename, int w, int h, int comp, const void *data, int stride_in_bytes);
int shot_thread(void *arg)
{
    uint8_t *buf = (uint8_t *)arg, *px = buf + 2 * sizeof(int), *row;
    int w, h, y;
    char dir[1024], path[1200];
    char *base = SDL_GetBasePath();
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    memcpy(&w, buf, sizeof(int));
    memcpy(&h, buf + sizeof(int), sizeof(int));
    if ((row = malloc((size_t)w * 4))) {   /* GL rows run bottom-up */
        for (y = 0; y < h / 2; y++) {
            memcpy(row, px + (size_t)y * w * 4, (size_t)w * 4);
            memcpy(px + (size_t)y * w * 4, px + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
            memcpy(px + (size_t)(h - 1 - y) * w * 4, row, (size_t)w * 4);
        }
        free(row);
    }
    for (y = 0; y < w * h; y++) px[y * 4 + 3] = 255;
    snprintf(dir, sizeof dir, "%sscreenshots", base ? base : "");
    if (base) SDL_free(base);
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    snprintf(path, sizeof path, "%s/pcj_%04d%02d%02d_%02d%02d%02d_%u.png", dir, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, (unsigned)(SDL_GetTicks() % 1000));
    if (stbi_write_png(path, w, h, 4, px, w * 4)) fprintf(stderr, "[SHOT] %s\n", path);
    else fprintf(stderr, "[SHOT] cannot write %s\n", path);
    free(buf);
    {
        extern void qs_osd_note(const char *m);
        qs_osd_note("Screenshot saved (F12)");
    }
    return 0;
}
static int s_fem_left, s_fem_right, s_fem_wheel;          /* menu mouse: clicks and wheel since the last poll */
static int s_fem_x0, s_fem_y0, s_fem_vw = 1, s_fem_vh = 1, s_fem_cw = 640, s_fem_ch = 480;   /* last present */
static int s_mouse = -1, s_mouse_inv;
static float s_mouse_rad_px;
static int s_mouse_grab = 1;
volatile int cj_gl_focus_frozen;   /* sleeping with the window unfocused (quicksave.c stall monitor) */
static int s_regrab;   /* focus came back: re-apply the mouse mode */
static int s_ptr_hidden;   /* menus: the pad or keys were used after the mouse, so the pointer hides */
static int s_shown = -1;   /* SDL_ShowCursor state last set (-1: unknown, set it again) */
static int s_rel = -1;
extern void cj_cam_mouse_rates(float rx, float ry);
extern float cj_cam_game_mul(void);
static void mouse_look(int ax[4])
{
    int dx = 0, dy = 0;
    float mul, rx = 0.0f, ry = 0.0f;
    /* live: CJ_MOUSE* or Options > PC Settings */
    s_mouse_rad_px = (float)(pcs_get(PCS_MOUSE_SENS) / 100.0 * 3.14159265 / 180.0);
    s_mouse_inv = pcs_get(PCS_MOUSE_INVERT);
    if (s_mouse != pcs_get(PCS_MOUSE)) {
        s_mouse = pcs_get(PCS_MOUSE);
        if (!s_mouse) cj_cam_mouse_rates(0, 0);
        if (s_mouse) {
            fprintf(stderr, "[MOUSE] mouse look on (%.1f degrees per 100 px; F10 releases the cursor)\n",
                    s_mouse_rad_px * 100.0f * 180.0f / 3.14159265f);
        }
    }
    {   /* the cursor is captured for mouse look, and free (shown) while a menu takes input (src/femouse.c) */
        extern int femouse_menu_active(void);
        int menu = femouse_menu_active(), want = s_mouse && s_mouse_grab && !menu;
#define rel s_rel
        if (s_regrab) { rel = -1; s_regrab = 0; }
        if (want != rel) {
            if (rel == 1 && !want) {   /* back from look: the cursor appears mid-window */
                int ww, wh;
                SDL_GetWindowSize(s_win, &ww, &wh);
                SDL_SetRelativeMouseMode(SDL_FALSE);
                SDL_WarpMouseInWindow(s_win, ww / 2, wh / 2);
            } else
                SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
            SDL_GetRelativeMouseState(&dx, &dy);   /* drop the motion of the switch */
            dx = dy = 0;
            rel = want;
        }
        {   /* the cursor shows whenever the mouse is not turning the camera */
            /* the normal Windows pointer, in the menus only, until the pad or keys are used */
            int show = menu && !s_ptr_hidden;
            (void)want;
            if (show != s_shown) { SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); s_shown = show; }
        }
        if (menu) { cj_cam_mouse_rates(0, 0); return; }
#undef rel
    }
    if (!s_mouse) return;
    SDL_GetRelativeMouseState(&dx, &dy);
    if (!s_mouse_grab || !(SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS)) { cj_cam_mouse_rates(0, 0); return; }
    mul = cj_cam_game_mul();
    if (dx) { rx = fabsf((float)dx) * s_mouse_rad_px / mul; ax[2] = dx > 0 ? 32767 : -32768; }
    if (dy) { ry = fabsf((float)dy) * s_mouse_rad_px / mul; ax[3] = ((dy < 0) != s_mouse_inv) ? 32767 : -32768; }
    cj_cam_mouse_rates(rx, ry);
}

/* On-screen status line (quick save messages): top-centre box drawn into the back buffer just
 * before the swap, so it shows in fullscreen too. Text rendered on the CPU from the embedded
 * DejaVu Sans Mono glyphs (src/osd_font.h), uploaded to a small texture and blitted at an
 * integer scale that follows the window height. */
#include "osd_font.h"
static void osd_draw(const char *msg)
{
    static GLuint tex, fbo;
    static char last[128];
    static int tw, th;
    int dw = 640, dh = 480, sc, x0, y0;
    if (!msg || !*msg) return;
    if (!fbo) {
        glGenTextures(1, &tex);
        glGenFramebuffers(1, &fbo);
    }
    if (strncmp(last, msg, sizeof last - 1)) {
        int n = (int)strlen(msg), i, x, y, pad = 6, bar = 3;
        uint8_t *px;
        snprintf(last, sizeof last, "%s", msg);
        if (n > 100) n = 100;
        tw = n * OSD_FW + pad * 2 + bar;
        th = OSD_FH + pad * 2;
        if (!(px = malloc((size_t)tw * th * 4))) return;
        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++) {
                uint8_t *q = px + ((size_t)(th - 1 - y) * tw + x) * 4;   /* GL rows go bottom-up */
                int a = 0;
                if (x < bar) { q[0] = 200; q[1] = 30; q[2] = 20; q[3] = 255; continue; }   /* red accent */
                i = x - bar - pad >= 0 ? (x - bar - pad) / OSD_FW : -1;
                if (i >= 0 && i < n && y >= pad && y < pad + OSD_FH) {
                    unsigned char c = (unsigned char)msg[i];
                    if (c >= 32 && c < 127) a = osd_font[c - 32][(y - pad) * OSD_FW + (x - bar - pad) % OSD_FW];
                }
                q[0] = (uint8_t)(18 + (237 * a) / 255);
                q[1] = (uint8_t)(18 + (237 * a) / 255);
                q[2] = (uint8_t)(20 + (235 * a) / 255);
                q[3] = 255;
            }
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        free(px);
    }
    SDL_GL_GetDrawableSize(s_win, &dw, &dh);
    sc = dh >= 1400 ? 3 : dh >= 900 ? 2 : 1;
    x0 = (dw - tw * sc) / 2;            /* top centre: the HUD's radar and weapon panel take the corners */
    y0 = dh - 20 * sc - th * sc;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    {
        GLboolean sc_on = glIsEnabled(GL_SCISSOR_TEST);
        if (sc_on) glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, tw, th, x0, y0, x0 + tw * sc, y0 + th * sc, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (sc_on) glEnable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}
static void host_input(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    CjHostPad p;
    int ax[4] = {0, 0, 0, 0}, i;
    memset(&p, 0, sizeof p);
    {   /* keyboard and mouse bindings (src/keymap.c; rebindable on the Controller pages) */
        extern int femouse_menu_active(void);
        keymap_pad(k, femouse_menu_active(), s_fem_wheel, &p.dig, p.ana);
    }
    ax[0] = (k[SDL_SCANCODE_D] ? 32767 : 0) - (k[SDL_SCANCODE_A] ? 32767 : 0);
    ax[1] = (k[SDL_SCANCODE_W] ? 32767 : 0) - (k[SDL_SCANCODE_S] ? 32767 : 0);
    ax[2] = (k[SDL_SCANCODE_L] ? 32767 : 0) - (k[SDL_SCANCODE_J] ? 32767 : 0);
    ax[3] = (k[SDL_SCANCODE_I] ? 32767 : 0) - (k[SDL_SCANCODE_K] ? 32767 : 0);
    if (s_pad) {
        static const SDL_GameControllerButton db[8] = {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_START,
            SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK};
        /* black = right shoulder, white = left shoulder (Xbox 360-style pads) */
        static const SDL_GameControllerButton ab[6] = {SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER};
        static const SDL_GameControllerAxis sa[4] = {SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
            SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY};
        for (i = 0; i < 8; i++) if (SDL_GameControllerGetButton(s_pad, db[i])) p.dig |= (uint16_t)(1u << i);
        for (i = 0; i < 6; i++) if (SDL_GameControllerGetButton(s_pad, ab[i])) p.ana[i] = 255;
        for (i = 0; i < 2; i++) {
            int t = SDL_GameControllerGetAxis(s_pad, i ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            t = t * 255 / 32767;
            if (t > p.ana[6 + i]) p.ana[6 + i] = (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t);
        }
        for (i = 0; i < 4; i++) {
            int v = SDL_GameControllerGetAxis(s_pad, sa[i]);
            if (i & 1) v = -v - 1;                  /* SDL: down = +; Xbox: up = + */
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            if (!ax[i]) ax[i] = v;   /* raw: src/cj_pad.c applies the dead zone (cj_stick) */
        }
    }
    {   /* controller quick save: click both sticks; quick load: press both bumpers (LB + RB). While a
         * pair is held (until both are released) the game sees neither button. Back is not used: the
         * game opens its menu on Back at once. The slot is picked in Options > PC Settings. */
        static int sticks, bumpers;
        int s2 = (p.dig & 0xC0) == 0xC0, s1 = (p.dig & 0xC0) != 0;
        int b2 = p.ana[4] > 128 && p.ana[5] > 128, b1 = p.ana[4] > 128 || p.ana[5] > 128;
        if (s2 && !sticks) { sticks = 1; qs_request(1); }
        if (sticks) { if (!s1) sticks = 0; p.dig &= (uint16_t)~0xC0; }
        if (b2 && !bumpers) { bumpers = 1; qs_request(2); }
        if (bumpers) { if (!b1) bumpers = 0; p.ana[4] = p.ana[5] = 0; }
    }
    {   /* which kind of input was used last picks the button prompts (src/glyphs.c) */
        extern void glyph_input(int kb_used, int pad_used);
        int kb_used = 0, pad_used = 0, n;
        const Uint8 *ks = SDL_GetKeyboardState(&n);
        for (i = 4; i < n && i < SDL_SCANCODE_KP_EQUALS; i++)   /* the F keys and Print/Scroll/Pause are port hotkeys, not play */
            if (ks[i] && !(i >= SDL_SCANCODE_F1 && i <= SDL_SCANCODE_PAUSE)) { kb_used = 1; break; }
        int keys_used = kb_used;
        if (s_fem_left || s_fem_right || s_fem_wheel || (SDL_GetMouseState(NULL, NULL) & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)))
            kb_used = 1;   /* the mouse counts as keyboard play */
        if (s_pad) {
            for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
                if (SDL_GameControllerGetButton(s_pad, (SDL_GameControllerButton)i)) pad_used = 1;
            for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++) {
                int v = SDL_GameControllerGetAxis(s_pad, (SDL_GameControllerAxis)i);
                if (v > 16000 || v < -16000) pad_used = 1;
            }
        }
        glyph_input(kb_used, pad_used);
        if (keys_used || pad_used) s_ptr_hidden = 1;   /* the menu pointer steps aside */
    }
    mouse_look(ax);
    {   /* menu mouse (src/femouse.c): the cursor in front-end units, i.e. the presented surface's pixels */
        extern void femouse_input(int fx, int fy, int left, int right, int wheel);
        int mx, my, ww, wh, dw, dh, fx = -1, fy = -1;
        SDL_GetMouseState(&mx, &my);
        {   /* moving or clicking the mouse brings the menu pointer back */
            static int lx = -1, ly = -1;
            if (mx != lx || my != ly || s_fem_left || s_fem_right || s_fem_wheel) {
                if (lx >= 0) s_ptr_hidden = 0;
                lx = mx; ly = my;
            }
        }
        SDL_GetWindowSize(s_win, &ww, &wh);
        SDL_GL_GetDrawableSize(s_win, &dw, &dh);
        if (ww > 0 && wh > 0 && (SDL_GetWindowFlags(s_win) & SDL_WINDOW_MOUSE_FOCUS) && !SDL_GetRelativeMouseMode()) {
            float px = (float)mx * dw / ww - s_fem_x0, py = (float)my * dh / wh - s_fem_y0;
            if (px >= 0 && py >= 0 && px < s_fem_vw && py < s_fem_vh) {
                fx = (int)(px * s_fem_cw / s_fem_vw);
                fy = (int)(py * s_fem_ch / s_fem_vh);
            }
        }
        femouse_input(fx, fy, s_fem_left > 0, s_fem_right > 0, s_fem_wheel);
        s_fem_left = s_fem_right = s_fem_wheel = 0;
    }
    {   /* the Controller pages' keyboard list: Enter / A rebinds, Left / Right set Mouse Speed (src/femouse.c) */
        extern void femouse_kb_keys(const Uint8 *k, int pad_a, uint16_t *dig, uint8_t ana[8]);
        femouse_kb_keys(k, p.ana[0] > 128, &p.dig, p.ana);
    }
    p.lx = (int16_t)ax[0]; p.ly = (int16_t)ax[1]; p.rx = (int16_t)ax[2]; p.ry = (int16_t)ax[3];
    memcpy((void *)&cj_host_pad, &p, sizeof p);
}

/* Flicker capture (off by default; CJ_FLICKER=1 turns it on).
 * Ring: the presented frame of the last RING_N flips, scaled to 640x480 on the
 * GPU and read back (1.2 MB each). F9 saves the ring, so the frames before the
 * press are on disk (a one-frame flicker is gone before a key press lands).
 * Detector: per 16x16 tile, a frame that differs from the one before it while
 * the next one returns to that earlier frame (A-B-A: flicker, not motion);
 * enough such tiles in one frame saves the ring automatically (cooldown 8 s,
 * at most 12 per session). Files: shots/NNNNN_<t>s_flickerK_fNN.bmp. */
#define RING_N 45
#define RING_W 640
#define RING_H 480
static GLuint s_ring_fbo, s_ring_rbo;
static uint8_t *s_ring[RING_N];
static uint32_t s_ring_flip[RING_N];
static unsigned s_ring_pos, s_ring_count, s_flk_events;
static double s_flk_last;
extern void gamelog_save_named(const uint8_t *rgba_bottom_up, int w, int h, const char *reason);
extern void gamelog_event(const char *type, const char *fmt, ...);
static void ring_dump(const char *why, unsigned want)
{
    unsigned i, n = s_ring_count < RING_N ? s_ring_count : RING_N;
    if (n > want) n = want;
    char r[64];
    for (i = 0; i < n; i++) {
        unsigned k = (s_ring_pos + RING_N - n + i) % RING_N;
        snprintf(r, sizeof r, "%s_f%02u_flip%u", why, i, s_ring_flip[k]);
        gamelog_save_named(s_ring[k], RING_W, RING_H, r);
    }
}
static unsigned flk_tiles(const uint8_t *a, const uint8_t *b, const uint8_t *c)
{
    /* a = t-2, b = t-1, c = t (all bottom-up RGBA 640x480) */
    unsigned tx, ty, n = 0;
    for (ty = 0; ty < RING_H / 16; ty++)
        for (tx = 0; tx < RING_W / 16; tx++) {
            uint32_t dab = 0, dac = 0, y, x;
            for (y = 0; y < 16; y += 2)
                for (x = 0; x < 16; x += 2) {
                    size_t o = ((size_t)(ty * 16 + y) * RING_W + tx * 16 + x) * 4;
                    int g1 = a[o + 1], g2 = b[o + 1], g3 = c[o + 1];
                    dab += (uint32_t)abs(g1 - g2);
                    dac += (uint32_t)abs(g1 - g3);
                }
            /* 64 samples: mean change >= 14 into B, back within 3 of A */
            if (dab >= 14 * 64 && dac <= 3 * 64) n++;
        }
    return n;
}
static void ring_capture(GLuint fbo, uint32_t cw, uint32_t ch, int f9)
{
    /* Off by default: the per-frame read-back stalls the render thread (it
     * waits for the GPU to finish each frame), and in heavy scenes the render
     * thread is the bottleneck. CJ_FLICKER=1 turns the capture on. */
    if (s_flk < 0) { const char *e = getenv("CJ_FLICKER"); s_flk = e && *e && *e != '0'; }
    if (!s_flk) return;
    if (!s_ring_fbo) {
        int i;
        glGenFramebuffers(1, &s_ring_fbo);
        glGenRenderbuffers(1, &s_ring_rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, s_ring_rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, RING_W, RING_H);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_ring_fbo);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_ring_rbo);
        for (i = 0; i < RING_N; i++)
            if (!(s_ring[i] = malloc((size_t)RING_W * RING_H * 4))) { s_flk = 0; return; }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_ring_fbo);
    glBlitFramebuffer(0, 0, (GLint)cw * s_rs, (GLint)ch * s_rs, 0, 0, RING_W, RING_H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_ring_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, RING_W, RING_H, GL_RGBA, GL_UNSIGNED_BYTE, s_ring[s_ring_pos]);
    s_ring_flip[s_ring_pos] = s_flips;
    s_ring_pos = (s_ring_pos + 1) % RING_N;
    s_ring_count++;
    if (f9) { ring_dump("f9before", 30); return; }
    if (s_ring_count >= 3 && s_flk_events < 12 && now_s() - s_flk_last > 8.0) {
        unsigned c = (s_ring_pos + RING_N - 1) % RING_N, b = (c + RING_N - 1) % RING_N, a = (b + RING_N - 1) % RING_N;
        unsigned n = flk_tiles(s_ring[a], s_ring[b], s_ring[c]);
        if (n >= 3) {
            char why[32];
            s_flk_last = now_s();
            s_flk_events++;
            gamelog_event("flicker", "\"tiles\":%u,\"flip\":%u", n, s_ring_flip[b]);
            fprintf(stderr, "[FLICKER] flip %u: %u tiles changed for one frame and changed back; saving the last 6 frames\n",
                    s_ring_flip[b], n);
            snprintf(why, sizeof why, "flicker%u", s_flk_events);
            ring_dump(why, 6);
            drec_dump((s_drec_cur + 2) % 3);
        }
    }
}

extern void cj_fps_tick(void);
/* Options > PC Settings changes that need the window or GL (pcsettings.c). */
static unsigned s_pc_gen[16];
static void rt_rescale_all(int ns)
{
    int i, os = s_rs, sv = s_side;
    for (i = 0; i < s_nrt; i++) {
        Rt old = s_rt[i];
        s_rs = ns;
        rt_alloc(&s_rt[i], old.w, old.h);
        glDisable(GL_SCISSOR_TEST);
        for (s_side = 0; s_side <= s_ab; s_side++) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, old.fbo2[s_side]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_rt[i].fbo2[s_side]);
            glBlitFramebuffer(0, 0, old.w * os, old.h * os, 0, 0, old.w * ns, old.h * ns, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        s_side = sv;
        rt_free(&old);
    }
    s_rs = ns;
    s_self_w = s_self_h = 0;            /* the self-copy texture is sized by s_rs */
    s_self_draw = ~0ull;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fprintf(stderr, "[GL] internal resolution now x%d (%d surfaces rescaled)\n", ns, s_nrt);
}

static void pc_settings_apply(void)
{
    static int init;
    if (!init) {
        int i;
        init = 1;
        for (i = 0; i < 16; i++) s_pc_gen[i] = pcs_gen(i);
        return;
    }
    if (s_pc_gen[PCS_FULLSCREEN] != pcs_gen(PCS_FULLSCREEN)) {
        s_pc_gen[PCS_FULLSCREEN] = pcs_gen(PCS_FULLSCREEN);
        if (s_fullscreen != pcs_get(PCS_FULLSCREEN)) {
            s_fullscreen = pcs_get(PCS_FULLSCREEN);
            set_borderless_fs(s_win, s_fullscreen);
        }
    }
    if (s_pc_gen[PCS_VSYNC] != pcs_gen(PCS_VSYNC)) {
        s_pc_gen[PCS_VSYNC] = pcs_gen(PCS_VSYNC);
        SDL_GL_SetSwapInterval(pcs_get(PCS_VSYNC));
    }
    if (s_pc_gen[PCS_WIDESCREEN] != pcs_gen(PCS_WIDESCREEN)) {
        s_pc_gen[PCS_WIDESCREEN] = pcs_gen(PCS_WIDESCREEN);
        if (!s_fullscreen && !(SDL_GetWindowFlags(s_win) & SDL_WINDOW_MAXIMIZED)) {
            int w, h;
            SDL_GetWindowSize(s_win, &w, &h);
            SDL_SetWindowSize(s_win, pcs_get(PCS_WIDESCREEN) ? h * 16 / 9 : h * 4 / 3, h);
        }
    }
    if (s_pc_gen[PCS_SCALE] != pcs_gen(PCS_SCALE)) {
        s_pc_gen[PCS_SCALE] = pcs_gen(PCS_SCALE);
        if (pcs_get(PCS_SCALE) != s_rs) rt_rescale_all(pcs_get(PCS_SCALE));
    }
}

static void gl_flip(void)
{
    SDL_Event ev;
    double flip_t0 = now_s();
    if (!init())
        return;
    fliplog_flip();
    s_flips++;
    cj_fps_tick();
    if (s_present || s_last) {
        Rt *p = s_present ? s_present : s_last;
        /* Show the buffer the swap flips to (executor: the colour target D3D
         * set at the previous FLIP_STALL), not the last frame-sized surface
         * drawn to: the cutscenes render the next scene into an offscreen
         * 640x480 target after compositing the letterboxed frame, and the
         * window alternated between the two. CJ_GL_PRESENT=drawn keeps the
         * old choice. */
        {
            static int mode = -1;
            static unsigned misses;
            extern uint32_t g_nv2a_flip_front_va;
            if (mode < 0) { const char *e = getenv("CJ_GL_PRESENT"); mode = !(e && !strcmp(e, "drawn")); }
            s_crtc_va = g_nv2a_flip_front_va;
            if (mode && g_nv2a_flip_front_va) {
                Rt *c = NULL;
                int i;
                for (i = 0; i < s_nrt; i++)
                    if (s_rt[i].va == g_nv2a_flip_front_va) { c = &s_rt[i]; break; }
                if (c && c->w >= 320) p = c;
                else if (misses++ < 8)
                    fprintf(stderr, "[GL] present: no surface at flip target %08X, showing %08X\n",
                            g_nv2a_flip_front_va, p->va);
            }
        }
        s_shown_va = p->va;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, p->fbo2[s_side]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        /* Only the part the title drew this time: a surface grown for a
         * bigger clip (the 4x back buffer in-level) keeps its old size, and
         * a 640x480 menu drawn into it sits in the top-left quarter. */
        uint32_t cw, ch;
        int i;
        /* Each surface keeps the clip of the last flip it was drawn in: the
         * buffer shown can be one drawn a flip ago. */
        for (i = 0; i < s_nrt; i++)
            if (s_rt[i].fcw) { s_rt[i].pcw = s_rt[i].fcw; s_rt[i].pch = s_rt[i].fch; s_rt[i].fcw = s_rt[i].fch = 0; }
        cw = p->pcw && p->pcw <= p->w ? p->pcw : p->w;
        ch = p->pch && p->pch <= p->h ? p->pch : p->h;
        if (cw >= 640 && ch < cw * 3 / 4) ch = cw * 3 / 4;
        if (ch > p->h) ch = p->h;
        {
            static uint32_t f9seen;
            int f9 = 0;
            extern unsigned gamelog_bookmarks_manual(void);
            unsigned m = gamelog_bookmarks_manual();
            if (m != f9seen) { f9seen = m; f9 = 1; }
            ring_capture(p->fbo2[s_side], cw, ch, f9);
            if (s_drec[0]) {   /* next frame's draw records */
                s_drec_flip[s_drec_cur] = s_flips;
                s_drec_cur = (s_drec_cur + 1) % 3;
                s_drec_n[s_drec_cur] = 0;
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, p->fbo2[s_side]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        }
        {
            int dw = 640, dh = 480, vw, vh;
            SDL_GL_GetDrawableSize(s_win, &dw, &dh);
            {
                int an = cj_widescreen() ? 16 : 4, ad = cj_widescreen() ? 9 : 3;
                if (dw * ad >= dh * an) { vh = dh; vw = dh * an / ad; } else { vw = dw; vh = dw * ad / an; }
            }
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            /* The title's 2D composites (cutscene letterbox, some full-screen
             * passes) sit about half a pixel right and down of the surface
             * edge (the NV2A's screen-space offset), so the surface's first
             * column and row are never drawn: invisible at 1x on a TV, a
             * 2-pixel line of stale picture at 4x. Leave them out of the
             * window (CJ_GL_EDGE_CROP=0 shows them). */
            static int crop_on = -1;
            int k = 0;
            if (crop_on < 0) { const char *e = getenv("CJ_GL_EDGE_CROP"); crop_on = !(e && *e == '0'); }
            if (crop_on) k = (int)(0.53125f * (float)s_rs + 0.5f);
            s_fem_x0 = (dw - vw) / 2; s_fem_y0 = (dh - vh) / 2; s_fem_vw = vw > 0 ? vw : 1; s_fem_vh = vh > 0 ? vh : 1;
            s_fem_cw = (int)cw; s_fem_ch = (int)ch;
            glBlitFramebuffer(k, (p->h - ch) * s_rs, cw * s_rs, p->h * s_rs - k, (dw - vw) / 2, (dh - vh) / 2, (dw - vw) / 2 + vw,
                              (dh - vh) / 2 + vh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        {
            /* CJ_GL_DUMP_TRIGGER=<file>: when the file appears, dump the next
             * 12 flips and delete it (a scripted run picks its moment). */
            static const char *trig; static int tinit;
            if (!tinit) { tinit = 1; trig = getenv("CJ_GL_DUMP_TRIGGER"); }
            if (trig && s_flips % 8 == 0) { FILE *tf = fopen(trig, "r"); if (tf) { fclose(tf); remove(trig); s_dump_until = s_flips + 12; } }
            if (s_dump && s_flips < s_dump_until) { dump_frame(p, cw, ch, ""); }
        }
        if (s_dump && (s_flips % s_dump_every == 0
                       || (s_diag_at >= 0 && s_flips >= (uint32_t)s_diag_at && s_flips < (uint32_t)s_diag_at + 5)
                       || (s_dump_at >= 0 && s_flips >= (uint32_t)s_dump_at && s_flips < (uint32_t)s_dump_at + 6))) {
            dump_frame(p, cw, ch, "");
            if (s_ab) {
                s_side = 1;
                dump_frame(p, cw, ch, "c");
                s_side = 0;
            }
        }
    }
    {
        static int diag = -1;
        int i;
        if (diag < 0) diag = getenv("CJ_GL_SURF_DIAG") ? atoi(getenv("CJ_GL_SURF_DIAG")) : 0;
        if ((diag && s_flips % diag == 0) || s_flips < s_burst_until) {
            fprintf(stderr, "[GLSURF] flip %u present %08X shown %08X flip %08X:", s_flips, s_present ? s_present->va : 0,
                    s_shown_va, s_crtc_va);
            for (i = 0; i < s_nrt; i++)
                if (s_rt[i].nd_flip)
                    fprintf(stderr, " [#%u-%u %08X %ux%u clip %ux%u draws %u rtt %u]", s_rt[i].order, s_rt[i].last_bind,
                            s_rt[i].va, s_rt[i].w, s_rt[i].h, s_rt[i].pcw, s_rt[i].pch, s_rt[i].nd_flip, s_rt[i].nd_rtt);
            fprintf(stderr, "\n");
        }
        for (i = 0; i < s_nrt; i++)
            s_rt[i].nd_flip = s_rt[i].nd_legacy = s_rt[i].nd_rtt = 0;
    }
    if (gamelog_burst_frame()) {
        if (s_flips >= s_burst_until) s_burst_until = s_flips + 9;
    }
    {   /* F12: screenshot of what the window shows, saved as screenshots/pcj_<date>_<time>.png next to
         * the exe (or the working folder) by a worker thread so play does not hitch */
        extern volatile int s_shot_req;
        if (s_shot_req) {
            int dw = 640, dh = 480;
            uint8_t *px;
            s_shot_req = 0;
            SDL_GL_GetDrawableSize(s_win, &dw, &dh);
            if ((px = malloc((size_t)dw * dh * 4 + 2 * sizeof(int)))) {
                extern int shot_thread(void *);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px + 2 * sizeof(int));
                memcpy(px, &dw, sizeof(int));
                memcpy(px + sizeof(int), &dh, sizeof(int));
                SDL_DetachThread(SDL_CreateThread(shot_thread, "shot", px));
            }
        }
    }
    if (gamelog_shot_pending()) {
        int dw = 640, dh = 480;
        uint8_t *px;
        SDL_GL_GetDrawableSize(s_win, &dw, &dh);
        if ((px = malloc((size_t)dw * dh * 4))) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
            gamelog_save_shot(px, dw, dh);
            free(px);
        }
    }
    osd_draw(qs_message());
    {   /* CJ_OSD_SHOT=<flip>: save the finished back buffer (with the status line) as osd_<flip>.ppm */
        static long at = -2;
        if (at == -2) at = getenv("CJ_OSD_SHOT") ? atol(getenv("CJ_OSD_SHOT")) : -1;
        static int trig = -1;   /* CJ_OSD_SHOT_TRIGGER=1: also whenever a file named osd_now appears (removed after) */
        int now = 0;
        if (trig < 0) trig = getenv("CJ_OSD_SHOT_TRIGGER") != NULL;
        if (trig && (s_flips & 7) == 0) { FILE *t = fopen("osd_now", "rb"); if (t) { fclose(t); remove("osd_now"); now = 1; } }
        {   /* the built-in controller diagram (src/texpack.c) only while FEPause exists: re-upload the
             * 512x512 DXT3 textures (the diagram's format) when that changes */
            extern int texpack_builtin_ctrl;
            uint32_t inst;
            int on, i;
            memcpy(&inst, (uint8_t *)xbox_GetMemoryOffset() + 0x0048e050u, 4);   /* FEPause::m_pInstance */
            {   extern int glyph_kb_mode(void);
                extern int cjmod_ctrl_page_showing(void);
                on = inst != 0 ? (glyph_kb_mode() ? 2 : 1) : 0;   /* 2: keyboard, the page lists keys, no pad */
                if (!on && cjmod_ctrl_page_showing()) on = glyph_kb_mode() ? 2 : 1;   /* main menu Options > Controller */
            }
            if (on != texpack_builtin_ctrl) {
                if (getenv("CJ_MENU_MOUSE_LOG")) fprintf(stderr, "[CTRLART] %d -> %d (pause %08x)\n", texpack_builtin_ctrl, on, inst);
                texpack_builtin_ctrl = on;
                for (i = 0; i < NTEX; i++)
                    if (s_tc[i].w == 512 && s_tc[i].h == 512 && s_tc[i].color == 0x0E) s_tc[i].hash = 0;
            }
        }
        if (trig && (s_flips & 7) == 2) {   /* dev: texlog_now; log the textures bound over the next 90 flips */
            FILE *t = fopen("texlog_now", "rb");
            if (t) { fclose(t); remove("texlog_now"); s_texlog_n = 0; s_texlog_until = s_flips + 90; fprintf(stderr, "[TEXLOG] begin flip %u\n", s_flips); }
        }
        if (trig && (s_flips & 7) == 4) {   /* dev: skin_now holds a costume skin index; the title Predator changes to it */
            FILE *t = fopen("skin_now", "rb");
            if (t) {
                int sk = 0;
                uint8_t *m = (uint8_t *)xbox_GetMemoryOffset();
                uint32_t fe;
                if (fscanf(t, "%d", &sk) != 1) sk = 0;
                fclose(t);
                remove("skin_now");
                memcpy(&fe, m + 0x0048ce58, 4);   /* FEMainMenu::ms_pInstance */
                if (fe) { int16_t s = (int16_t)sk; memcpy(m + fe + 0x820, &s, 2); }   /* m_PlayerSkin */
                fprintf(stderr, "[SKIN] %d (FEMainMenu %08x)\n", sk, fe);
            }
        }
        if (now || (at >= 0 && (s_flips == (uint32_t)at || s_flips == (uint32_t)at + 30))) {
            int dw = 640, dh = 480, y;
            uint8_t *px;
            SDL_GL_GetDrawableSize(s_win, &dw, &dh);
            if ((px = malloc((size_t)dw * dh * 3))) {
                char fn[64];
                FILE *f;
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, dw, dh, GL_RGB, GL_UNSIGNED_BYTE, px);
                snprintf(fn, sizeof fn, "osd_%u.ppm", s_flips);
                if ((f = fopen(fn, "wb"))) {
                    fprintf(f, "P6\n%d %d\n255\n", dw, dh);
                    for (y = dh - 1; y >= 0; y--) fwrite(px + (size_t)y * dw * 3, 1, (size_t)dw * 3, f);
                    fclose(f);
                }
                free(px);
            }
        }
    }
    {
        double ts0 = now_s();
        SDL_GL_SwapWindow(s_win);
        s_swap_time += now_s() - ts0;
    }
    {   /* CJ_TEST_F9_AT=<flip>: press F9 at that flip (tests the burst capture) */
        static long at = -2;
        if (at == -2) at = getenv("CJ_TEST_F9_AT") ? atol(getenv("CJ_TEST_F9_AT")) : -1;
        if (at >= 0 && s_flips == (uint32_t)at) {
            SDL_Event k;
            memset(&k, 0, sizeof k);
            k.type = SDL_KEYDOWN;
            k.key.keysym.sym = SDLK_F9;
            SDL_PushEvent(&k);
        }
    }
    pc_settings_apply();
    {   /* quick save status in the window title for a couple of seconds */
        static unsigned shown_gen;
        static int showing;
        const char *m = qs_message();
        if (m && qs_message_gen() != shown_gen) {
            char t[160];
            snprintf(t, sizeof t, "Predator: Concrete Jungle - %s", m);
            SDL_SetWindowTitle(s_win, t);
            shown_gen = qs_message_gen(); showing = 1;
        } else if (!m && showing) {
            SDL_SetWindowTitle(s_win, "Predator: Concrete Jungle");
            showing = 0;
        }
    }
    {   /* Focus lost (Snipping Tool, Alt+Tab, another window): let go of the mouse, stop drawing and sleep,
         * so the last frame sits still under the overlay and nothing fights it for the cursor. Only once
         * the window has had focus, so a window manager-less test display never waits. */
        static int had_focus;
        if (SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS) { had_focus = 1; focusdiag_tick(1); }
        else if (had_focus) {
            focusdiag_note("freeze start");
            cj_gl_focus_frozen = 1;
            SDL_SetRelativeMouseMode(SDL_FALSE);
            SDL_ShowCursor(SDL_ENABLE);
            while (!(SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS)) {
                while (SDL_PollEvent(&ev)) {
                    focusdiag_event(&ev);
                    gamelog_sdl_event(&ev);
                    if (ev.type == SDL_QUIT) exit(0);
                }
                focusdiag_tick(0);
                SDL_Delay(30);
            }
            focusdiag_note("freeze end");
            cj_gl_focus_frozen = 0;
            SDL_ShowCursor(SDL_DISABLE);
            s_shown = -1;
            s_regrab = 1;
        }
    }
    while (SDL_PollEvent(&ev)) {
        focusdiag_event(&ev);
        gamelog_sdl_event(&ev);
        if (keymap_capturing() >= 0 && ev.type != SDL_QUIT && keymap_event(&ev)) continue;   /* rebinding a key */
        if (ev.type == SDL_QUIT) {
            fprintf(stderr, "[GL] window closed, exiting after %u flips\n", s_flips);
            fflush(stderr);
            exit(0);
        }
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
            (ev.key.keysym.sym == SDLK_F11 ||
             (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)))) {
            s_fullscreen = !s_fullscreen;
            set_borderless_fs(s_win, s_fullscreen);
            pcs_set(PCS_FULLSCREEN, s_fullscreen);   /* remembered in pcj.ini, shown on the page */
            s_pc_gen[PCS_FULLSCREEN] = pcs_gen(PCS_FULLSCREEN);
            gamelog_event("fullscreen", "\"on\":%d", s_fullscreen);
        }
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F3) {   /* texture packs on/off */
            extern int texpack_toggle(void);
            extern int texpack_count(void);
            extern void qs_osd_note(const char *m);
            if (texpack_count() > 0) {
                int on = texpack_toggle(), i;
                for (i = 0; i < NTEX; i++) s_tc[i].hash = 0;   /* re-upload every texture with or without its replacement */
                qs_osd_note(on ? "Texture packs ON (F3)" : "Texture packs OFF (F3)");
            } else
                qs_osd_note("No texture packs installed");
        }
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F7) {   /* frame rate 30 / 60 */
            extern void qs_osd_note(const char *m);
            int fps = pcs_get(PCS_FPS) == 60 ? 30 : 60;
            pcs_set(PCS_FPS, fps);   /* applied live (recomp_manual.c), saved to pcj.ini like the page */
            qs_osd_note(fps == 60 ? "Frame rate 60 (F7)" : "Frame rate 30 (F7)");
            gamelog_event("fps", "\"to\":%d", fps);
        }
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F12) s_shot_req = 1;
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            SDL_SetRelativeMouseMode(SDL_FALSE);   /* Alt+Tab: hand the cursor back at once */
        }
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F5) qs_request(1);   /* quick save */
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F8) qs_request(2);   /* quick load (F9 is the gamelog burst) */
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F10 && s_mouse > 0)
            s_mouse_grab = !s_mouse_grab;   /* mouse_look applies it */
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) s_fem_left++;
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT) s_fem_right++;
        if (ev.type == SDL_MOUSEWHEEL) s_fem_wheel += ev.wheel.y * (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
        if (ev.type == SDL_CONTROLLERDEVICEADDED && !s_pad) {
            s_pad = SDL_GameControllerOpen(ev.cdevice.which);
            if (s_pad) {
                fprintf(stderr, "[PAD] controller: %s\n", SDL_GameControllerName(s_pad));
                gamelog_event("controller_connected", "\"name\":\"%s\"", SDL_GameControllerName(s_pad) ? SDL_GameControllerName(s_pad) : "?");
            }
        }
        if (ev.type == SDL_CONTROLLERDEVICEREMOVED && s_pad &&
            ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad))) {
            SDL_GameControllerClose(s_pad);
            s_pad = NULL;
            fprintf(stderr, "[PAD] controller removed\n");
            gamelog_event("controller_removed", "");
        }
    }
    s_be_time += now_s() - flip_t0;
    host_input();
    gamelog_frame(s_flips, s_draws, s_tris, (const void *)&cj_host_pad);
    {
        double t = now_s();
        s_fps_f++;
        if (!s_fps_t) s_fps_t = t;
        if (t - s_fps_t >= 1.0) {
            static unsigned nlog;
            char title[96];
            double fps = s_fps_f / (t - s_fps_t);
            snprintf(title, sizeof title, "Predator: Concrete Jungle - %.1f fps", fps);
            SDL_SetWindowTitle(s_win, title);
            {
#ifdef _WIN32
                extern volatile long long cj_exc_av, cj_exc_ss, cj_exc_other;
                static long long e0, e1, e2;
                long long a = cj_exc_av, b2 = cj_exc_ss, c = cj_exc_other;
#else
                long long a = 0, b2 = 0, c = 0; static long long e0, e1, e2;
#endif
                double span = t - s_fps_t;
                static uint64_t ev_last;
                fprintf(stderr, "[FPS] %.1f fps (flip %u) | renderer %.0f%% of wall time (swap %.0f%%, texture uploads %.0f%% in %llu, %llu cache evictions) | exceptions/s: %.0f access, %.0f step, %.0f other\n",
                        fps, s_flips, 100.0 * s_be_time / span, 100.0 * s_swap_time / span, 100.0 * s_upload_time / span,
                        (unsigned long long)s_upload_n, (unsigned long long)(s_tc_evictions - ev_last), (a - e0) / span, (b2 - e1) / span, (c - e2) / span);
                s_swap_time = s_upload_time = 0; s_upload_n = 0; ev_last = s_tc_evictions;
                gamelog_event("perf", "\"fps\":%.1f,\"renderer_pct\":%.0f,\"exc_access_s\":%.0f,\"exc_step_s\":%.0f", fps,
                              100.0 * s_be_time / span, (a - e0) / span, (b2 - e1) / span);
                e0 = a; e1 = b2; e2 = c; s_be_time = 0;
                fflush(stdout); fflush(stderr);
                (void)nlog;
            }
            s_fps_t = t; s_fps_f = 0;
        }
    }
    if (s_flips % 16 == 0) {
        static double t_last; static uint64_t d_last;
        double t = now_s();
        fprintf(stderr, "[GL] flip %u: %.2f flips/s since first GL call, %.2f over the last 16 (%llu batches/flip), %llu batches %llu tris %llu tex decodes %llu cache hits %llu rtt binds %llu self copies %llu zeta reads %llu rc draws %d programs\n",
                s_flips, s_flips / (t - s_t0), t_last ? 16.0 / (t - t_last) : 0.0,
                (unsigned long long)((s_draws - d_last) / 16), (unsigned long long)s_draws,
                (unsigned long long)s_tris, (unsigned long long)s_tex_decodes, (unsigned long long)s_tex_hits,
                (unsigned long long)s_rtt_binds, (unsigned long long)s_self_copies, (unsigned long long)s_zeta_reads, (unsigned long long)s_rc_draws, s_nprog);
        texstats_print();
        if (s_genmips > 0) fprintf(stderr, "[GL] genmips: %llu textures got a generated mip chain (CJ_GL_GENMIPS)\n", (unsigned long long)s_genmip_textures);
        fprintf(stderr, "[GL] texhash: %.1f MB hashed since start, %llu stale textures the sampled check would have kept (%s)\n", s_texhash_bytes / 1048576.0, (unsigned long long)s_texhash_caught,
                s_texhash_full ? "whole textures" : "sampled, CJ_GL_TEXHASH=0");
        fprintf(stderr, "[GL] vsh: %d programs translated, %llu GPU-VS batches, %llu CPU-VS batches (%llu refused)\n",
                s_nvsh, (unsigned long long)s_vsh_batches, (unsigned long long)s_vsh_cpu_batches,
                (unsigned long long)s_vsh_rejects);
        fprintf(stderr, "[GL] tex: %llu DXT uploads compressed, %llu cache evictions; %llu mip-chained textures uploaded (%llu levels); %llu window-clipped draws, %llu polygon-offset draws, %u w-buffered draws%s\n",
                (unsigned long long)s_s3tc_uploads, (unsigned long long)s_tc_evictions,
                (unsigned long long)s_mip_textures, (unsigned long long)s_mip_levels,
                (unsigned long long)s_wclip_draws, (unsigned long long)s_polyoff_draws,
                s_wbuf_draws, s_wbuf_on ? "" : " (CJ_GL_WBUF=0)");
        t_last = t; d_last = s_draws;
    }
}

/* Visibility tests (D3D Begin/EndVisibilityTest -> NV2A zpass pixel count):
 * GL_SAMPLES_PASSED queries around the draws made while counting is on. The
 * count is read back at GET_REPORT (a stall, but tests are few per frame).
 * CJ_GL_ZPASS=0 restores the old behaviour (every test reports 0 pixels). */
static int s_zp_on = -1, s_zp_active;
static GLuint s_zp_q;
static uint64_t s_zp_acc, s_zp_reports;
static void zp_end(void)
{
    GLuint n = 0;
    if (!s_zp_active) return;
    glEndQuery(GL_SAMPLES_PASSED);
    glGetQueryObjectuiv(s_zp_q, GL_QUERY_RESULT, &n);
    s_zp_acc += n;
    s_zp_active = 0;
}
static uint32_t gl_zpass(int op, uint32_t arg)
{
    static int enabled;
    if (s_zp_on < 0) { const char *e = getenv("CJ_GL_ZPASS"); s_zp_on = !(e && *e == '0'); }
    if (!s_zp_on || !init()) return 0;
    if (!s_zp_q) glGenQueries(1, &s_zp_q);
    switch (op) {
    case 0:                                   /* clear */
        zp_end();
        s_zp_acc = 0;
        if (enabled) { glBeginQuery(GL_SAMPLES_PASSED, s_zp_q); s_zp_active = 1; }
        return 0;
    case 1:                                   /* counting on/off */
        enabled = arg != 0;
        if (enabled && !s_zp_active) { glBeginQuery(GL_SAMPLES_PASSED, s_zp_q); s_zp_active = 1; }
        else if (!enabled) zp_end();
        return 0;
    default: {                                /* report */
        uint64_t v;
        int was = s_zp_active;
        zp_end();
        v = s_zp_acc;
        if (was && enabled) { glBeginQuery(GL_SAMPLES_PASSED, s_zp_q); s_zp_active = 1; }
        s_zp_reports++;
        if (s_zp_reports <= 4 || s_zp_reports % 5000 == 0)
            fprintf(stderr, "[GL] visibility test #%llu: %llu pixels\n", (unsigned long long)s_zp_reports, (unsigned long long)v);
        return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
    }
    }
}

static const Nv2aBackend s_gl = { gl_clear, gl_draw, gl_flip, gl_vsh_accept, gl_zpass };

/* Windows: called from main() after the defaults / pcj.ini are applied
 * (a constructor would run before them). */
void cj_gl_register_env(void)
{
    const char *e = getenv("CJ_GL");
    if (e && *e == '1') {
        nv2a_backend_register(&s_gl);
        fprintf(stderr, "[GL] OpenGL back end registered (CJ_GL=1)\n");
    }
}

#ifndef _WIN32
__attribute__((constructor)) static void gl_backend_register(void) { cj_gl_register_env(); }
#endif


