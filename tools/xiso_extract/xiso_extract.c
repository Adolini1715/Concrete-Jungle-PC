/* xiso_extract: unpack an Xbox disc image (XDVDFS; xiso or full redump ISO)
 * into a folder. Usage: xiso_extract <image.iso> <dest folder>. Port of
 * tools/xdvdfs.py so Windows users need no Python. */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define fseek64 _fseeki64
#define MKDIR(p) _mkdir(p)
#define SEP '\\'
#else
#include <sys/stat.h>
#define fseek64 fseeko
#define MKDIR(p) mkdir(p, 0755)
#define SEP '/'
#endif
static FILE *img; static int64_t base; static unsigned long long nfiles, nbytes;
static void mkdirs(char *p) { char *s; for (s = p + 1; *s; s++) if (*s == '/' || *s == '\\') { char c = *s; *s = 0; MKDIR(p); *s = c; } MKDIR(p); }
static int rd(int64_t off, void *b, size_t n) { return fseek64(img, off, SEEK_SET) == 0 && fread(b, 1, n, img) == n; }
static void walk(uint32_t sec, uint32_t size, const char *dir, int depth)
{
    uint8_t *d; char *seen; int *stack, sp = 0; uint32_t n4 = size / 4 + 1;
    if (!size || depth > 64) return;
    d = malloc(size); seen = calloc(n4, 1); stack = malloc(sizeof(int) * (n4 + 1));
    if (!d || !seen || !stack || !rd(base + (int64_t)sec * 2048, d, size)) { fprintf(stderr, "read error in %s\n", dir); goto out; }
    stack[sp++] = 0;
    while (sp) {
        int off = stack[--sp]; uint16_t l, r; uint32_t s, sz; uint8_t attr, nl; char name[256], path[4096];
        if ((uint32_t)off * 4 + 14 > size || seen[off]) continue;
        seen[off] = 1;
        if (!memcmp(d + off * 4, "\xff\xff\xff\xff", 4)) continue;
        memcpy(&l, d + off * 4, 2); memcpy(&r, d + off * 4 + 2, 2); memcpy(&s, d + off * 4 + 4, 4);
        memcpy(&sz, d + off * 4 + 8, 4); attr = d[off * 4 + 12]; nl = d[off * 4 + 13];
        if ((uint32_t)off * 4 + 14 + nl > size) continue;
        memcpy(name, d + off * 4 + 14, nl); name[nl] = 0;
        snprintf(path, sizeof path, "%s%c%s", dir, SEP, name);
        if (attr & 0x10) { MKDIR(path); walk(s, sz, path, depth + 1); }
        else {
            FILE *o = fopen(path, "wb"); uint32_t left = sz; static uint8_t buf[1 << 20];
            if (!o) { fprintf(stderr, "cannot write %s\n", path); }
            else {
                if (fseek64(img, base + (int64_t)s * 2048, SEEK_SET)) left = 0;
                while (left) { size_t k = left < sizeof buf ? left : sizeof buf; if (fread(buf, 1, k, img) != k) break; fwrite(buf, 1, k, o); left -= (uint32_t)k; }
                fclose(o); nfiles++; nbytes += sz;
                if (nfiles % 200 == 0) printf("  %llu files, %llu MB\n", nfiles, nbytes >> 20);
            }
        }
        if (l && sp < (int)n4) stack[sp++] = l;
        if (r && sp < (int)n4) stack[sp++] = r;
    }
out: free(d); free(seen); free(stack);
}
int main(int argc, char **argv)
{
    static const int64_t bases[] = { 0, 0x18300000LL, 0x30600000LL, 0x2080000LL, 0xFD90000LL };
    char magic[20], dest[4096]; uint32_t rs, rz; int i;
    if (argc < 3) { fprintf(stderr, "usage: xiso_extract <image.iso> <dest folder>\n"); return 2; }
    if (!(img = fopen(argv[1], "rb"))) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    for (i = 0; i < 5; i++) if (rd(bases[i] + 32 * 2048, magic, 20) && !memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20)) break;
    if (i == 5) { fprintf(stderr, "no Xbox filesystem found in %s\n", argv[1]); return 1; }
    base = bases[i];
    rd(base + 32 * 2048 + 20, &rs, 4); rd(base + 32 * 2048 + 24, &rz, 4);
    snprintf(dest, sizeof dest, "%s", argv[2]); mkdirs(dest);
    printf("Extracting %s -> %s\n", argv[1], dest);
    walk(rs, rz, dest, 0);
    printf("Done: %llu files, %llu MB\n", nfiles, nbytes >> 20);
    return nfiles ? 0 : 1;
}
