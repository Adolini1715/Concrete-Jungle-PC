/*
 * Mods folder, files inside the disc archives (docs/mods-folder.md).
 *
 * Most of the game's data (.edb packages, audio banks) lives in BuildData/Filelist.000-005, a
 * member often in several of them. A file in the mods folder at a member's path
 * (mods/Predator/Binary/_bin_xb/pred.edb) replaces that member: every XBFileManager::FileRead
 * (0x285180) from an archive is patched with the mod file's bytes where it overlaps a copy of
 * the member (located through the game's in-memory file lists). The member's space on the disc
 * is fixed, so the mod file may not be longer than the member (shorter reads as zeros after). Loose disc files (audio banks, movies) are replaced by the path
 * overlay in the kernel (g_xbox_overlay_dir), at any size.
 * Part of the recompiler's --exclude-manual input (tools/regen_all.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include "recomp_types.h"

extern void sub_00285180_gen(void);   /* XBFileManager::FileRead(AsyncFileData *, u8 *dest, u32 size), thiscall */
extern char g_xbox_overlay_dir[1024];
#ifdef _WIN32
#include <windows.h>
extern BOOL xbox_translate_path(const char *xbox_path, wchar_t *host, DWORD n);
#else
typedef int BOOL;
extern BOOL xbox_translate_path(const char *xbox_path, char *host, unsigned n);
#endif

#define AFD_NAME   0x2Cu
#define AFD_SEEK   0x1ACu
#define AFD_NUMBER 0x1D4u

typedef struct {
    int archive;
    uint32_t off, len;      /* in the archive */
    uint32_t mod_len;
    char path[1024];        /* host path of the mod file */
} Member;

static Member *s_m;
static int s_n, s_init;

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static int host_path(const char *xbox, char *out, size_t n)
{
#ifdef _WIN32
    wchar_t w[1024];
    if (!xbox_translate_path(xbox, w, 1024)) return 0;
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)n, NULL, NULL);
    return 1;
#else
    return xbox_translate_path(xbox, out, (unsigned)n) ? 1 : 0;
#endif
}

/* The game's own file lists (EXFileSys::m_pFileSys, 0x49E880): every member with a file in the
 * mods folder, at each of its up to four disc copies. Rebuilt when the lists change. */
#define FILESYS_VA 0x0049E880u
static uint32_t s_sig = ~0u;

static int disc_archive(const char *nm)
{
    char low[512], *p;
    snprintf(low, sizeof low, "%s", nm);
    lower(low);
    p = strstr(low, "filelist.");
    return p && isdigit((unsigned char)p[9]) ? atoi(p + 9) : -1;
}

static void load_index(void)
{
    uint32_t fs = MEM32(FILESYS_VA), nl, i, sig = 0;
    s_init = 1;
    if (!g_xbox_overlay_dir[0] || !fs) return;
    nl = MEM32(fs + 8);
    for (i = 0; i < nl && i < 64; i++) {
        uint32_t fl = MEM32(MEM32(fs + 0xc) + i * 4);
        sig = sig * 31u + fl + MEM32(fl + 0x1c + 8);
    }
    if (sig == s_sig) return;
    s_sig = sig;
    s_n = 0;
    for (i = 0; i < nl && i < 64; i++) {
        uint32_t fl = MEM32(MEM32(fs + 0xc) + i * 4);
        uint32_t nf = MEM32(fl + 0x1c + 8), fd = MEM32(fl + 0x1c + 0xc);
        uint32_t nd = MEM32(fl + 0x0c + 8), dd = MEM32(fl + 0x0c + 0xc), k;
        for (k = 0; k < nf; k++) {
            uint32_t e = fd + k * 0x3c, j;
            char name[512], mp[1100];
            struct stat sb;
            const char *pn = (const char *)XBOX_PTR(MEM32(e));
            if (!MEM32(e)) continue;
            snprintf(name, sizeof name, "%s", pn);
            if (!host_path(name, mp, sizeof mp) || strncmp(mp, g_xbox_overlay_dir, strlen(g_xbox_overlay_dir))) continue;
            if (stat(mp, &sb) != 0) continue;
            if ((uint32_t)sb.st_size > MEM32(e + 4)) {
                fprintf(stderr, "[MODDIR] %s is %lu bytes, longer than the game's %u for it: not used\n", mp,
                        (unsigned long)sb.st_size, MEM32(e + 4));
                continue;
            }
            for (j = 0; j < 4; j++) {
                uint32_t off = MEM32(e + 8 + j * 8), ni = MEM32(e + 12 + j * 8);
                int arc;
                if (ni >= nd) continue;
                arc = disc_archive((const char *)XBOX_PTR(MEM32(dd + ni * 4)));
                if (arc < 0) continue;
                {   /* copies often repeat the same location */
                    int q, dup = 0;
                    for (q = 0; q < s_n; q++) dup |= s_m[q].archive == arc && s_m[q].off == off;
                    if (dup) continue;
                }
                s_m = realloc(s_m, sizeof *s_m * (size_t)(s_n + 1));
                s_m[s_n].archive = arc;
                s_m[s_n].off = off;
                s_m[s_n].len = MEM32(e + 4);
                s_m[s_n].mod_len = (uint32_t)sb.st_size;
                snprintf(s_m[s_n].path, sizeof s_m[s_n].path, "%s", mp);
                fprintf(stderr, "[MODDIR] %s replaces %s (copy %u: archive %d at 0x%x, %lu of %u bytes)\n", mp, name, j,
                        arc, off, (unsigned long)sb.st_size, MEM32(e + 4));
                s_n++;
            }
        }
    }
}

static int archive_of(uint32_t pData)
{
    char nm[0x178 + 1], *p;
    memcpy(nm, XBOX_PTR(pData + AFD_NAME), 0x178);
    nm[0x178] = 0;
    lower(nm);
    p = strstr(nm, "filelist.");
    if (!p) return -1;
    p += 9;
    if (isdigit((unsigned char)p[0])) return atoi(p);
    return -1;
}

/* XBFileManager::FileRead(AsyncFileData *pData, u8 *pDest, u32 size), __thiscall, ret 0xc */
void sub_00285180(void)
{
    uint32_t pData = MEM32(g_esp + 4), dest = MEM32(g_esp + 8), size = MEM32(g_esp + 12);   /* ecx: the manager */
    int arc, i;
    uint32_t seek;
    sub_00285180_gen();
    load_index();
    {
        static int trace = -1;
        if (trace < 0) { const char *e = getenv("CJ_MODDIR_CHECK"); trace = e && *e == '2'; }
        if (trace && pData)
            fprintf(stderr, "[MODDIR] FileRead %.80s seek 0x%x size 0x%x (%d members)\n", (const char *)XBOX_PTR(pData + AFD_NAME),
                    MEM32(pData + AFD_SEEK), size, s_n);
    }
    if (!s_n || !pData || !dest) return;
    arc = archive_of(pData);
    if (arc < 0) return;
    seek = MEM32(pData + AFD_SEEK);
    for (i = 0; i < s_n; i++) {
        Member *m = &s_m[i];
        uint32_t a, b;
        if (m->archive != arc) continue;
        a = seek > m->off ? seek : m->off;
        b = seek + size < m->off + m->len ? seek + size : m->off + m->len;
        if (a >= b) continue;
        {
            uint8_t *d = (uint8_t *)XBOX_PTR(dest + (a - seek));
            uint32_t rel = a - m->off, n = b - a, have = 0;
            static int check = -1;
            uint8_t *orig = NULL;
            FILE *f = fopen(m->path, "rb");
            if (check < 0) { const char *e = getenv("CJ_MODDIR_CHECK"); check = e && *e == '1'; }
            if (check && (orig = malloc(n))) memcpy(orig, d, n);
            if (f) {
                if (rel < m->mod_len && fseek(f, (long)rel, SEEK_SET) == 0)
                    have = (uint32_t)fread(d, 1, n < m->mod_len - rel ? n : m->mod_len - rel, f);
                fclose(f);
            }
            if (have < n) memset(d + have, 0, n - have);
            if (orig) {   /* tests: how much of the read the mod changed */
                uint32_t k, diff = 0;
                for (k = 0; k < n; k++) diff += orig[k] != d[k];
                fprintf(stderr, "[MODDIR] read archive %d 0x%x+0x%x: %u bytes from %s, %u differ\n", arc, seek, size, n, m->path, diff);
                free(orig);
            }
        }
    }
}
