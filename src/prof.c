/*
 * Built-in sampling profiler (CJ_PROF, default on; CJ_PROF=0 turns it off).
 *
 * A sampler thread interrupts every other thread of the process CJ_PROF_HZ
 * times a second (default 200) and records the instruction pointer:
 *   Windows: SuspendThread + GetThreadContext + ResumeThread. Nothing that
 *            can take a lock runs while a thread is suspended.
 *   Linux:   tgkill(SIGPROF) to each task in /proc/self/task; the handler
 *            reads the PC from the ucontext.
 * Samples go into a lock-free table keyed by (thread, module, offset). Every
 * 30 s and at exit the table is written to prof.txt in the session log folder
 * (or next to the log): per thread, the share of samples by module, and the
 * hottest offsets per module. Offsets in pcj.exe are image-relative (RVA);
 * tools/prof_report.py turns them into function names with the unstripped
 * build. A thread whose samples sit in ntdll/kernelbase/win32u (or, on Linux,
 * libc futex/poll) is waiting, not working.
 */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#else
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <ucontext.h>
#endif

const char *gamelog_dir(void);

#define TAB_BITS 16
#define TAB_SIZE (1u << TAB_BITS)
#define MAX_THREADS 64
#define MAX_MODS 128

typedef struct { volatile uint64_t key; volatile uint32_t count; } Slot;   /* key: thread<<56 | mod<<48 | off */
static Slot s_tab[TAB_SIZE];
static volatile uint32_t s_dropped, s_total;

typedef struct { uintptr_t base, end; char name[64]; } Mod;
/* s_reg: every module seen, append-only, so a recorded index always names the
 * same module. s_sorted: two lists of registry indices sorted by base;
 * refresh_mods fills the idle one and flips s_cur, so a sample taken during a
 * refresh sees one whole list or the other. */
static Mod s_reg[MAX_MODS];
static volatile int s_nreg;
static uint8_t s_sorted[2][MAX_MODS];
static int s_nsorted[2];
static volatile int s_cur;
static int cmp_idx(const void *a, const void *b)
{
    uintptr_t x = s_reg[*(const uint8_t *)a].base, y = s_reg[*(const uint8_t *)b].base;
    return x < y ? -1 : x > y;
}
static void publish_mods(const Mod *m, int n)
{
    int nx = s_cur ^ 1, k = 0;
    for (int i = 0; i < n; i++) {
        int r;
        for (r = 0; r < s_nreg; r++)
            if (s_reg[r].base == m[i].base && s_reg[r].end == m[i].end && !strcmp(s_reg[r].name, m[i].name)) break;
        if (r == s_nreg) {
            if (s_nreg >= MAX_MODS - 1) continue;
            s_reg[r] = m[i];
            __atomic_store_n(&s_nreg, r + 1, __ATOMIC_RELEASE);
        }
        s_sorted[nx][k++] = (uint8_t)r;
    }
    qsort(s_sorted[nx], k, 1, cmp_idx);
    s_nsorted[nx] = k;
    __atomic_store_n(&s_cur, nx, __ATOMIC_RELEASE);
}
#define s_mods s_reg
#define s_nmods s_nreg

typedef struct { uint32_t tid; uintptr_t start; volatile uint32_t samples; } Thr;
static Thr s_thr[MAX_THREADS];
static volatile int s_nthr;
static int s_hz = 200;
static volatile int s_stop;
static time_t s_t0;
static int s_chains;   /* CJ_PROF_CHAINS=1: also walk the stacks (DIAG.bat sets it) */

static int mod_of(uintptr_t pc, uintptr_t *off)
{
    int c = __atomic_load_n(&s_cur, __ATOMIC_ACQUIRE);
    const uint8_t *idx = s_sorted[c];
    int lo = 0, hi = s_nsorted[c] - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        const Mod *md = &s_reg[idx[m]];
        if (pc < md->base) hi = m - 1;
        else if (pc >= md->end) lo = m + 1;
        else { *off = pc - md->base; return idx[m]; }
    }
    *off = pc;
    return 0x7F;                                     /* unknown: JIT / driver heap / anonymous */
}

static int thr_index(uint32_t tid)
{
    for (int i = 0; i < s_nthr; i++) if (s_thr[i].tid == tid) return i;
    return -1;
}

static void record(int ti, uintptr_t pc)
{
    uintptr_t off;
    int m = mod_of(pc, &off);
    uint64_t key = ((uint64_t)ti << 56) | ((uint64_t)m << 48) | (off & 0xFFFFFFFFFFFFull);
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - TAB_BITS));
    __atomic_fetch_add(&s_total, 1, __ATOMIC_RELAXED);
    if (ti >= 0 && ti < MAX_THREADS) __atomic_fetch_add(&s_thr[ti].samples, 1, __ATOMIC_RELAXED);
    for (uint32_t n = 0; n < 64; n++, h = (h + 1) & (TAB_SIZE - 1)) {
        uint64_t k = s_tab[h].key;
        if (k == key + 1) { __atomic_fetch_add(&s_tab[h].count, 1, __ATOMIC_RELAXED); return; }
        if (k == 0) {
            uint64_t z = 0;
            if (__atomic_compare_exchange_n(&s_tab[h].key, &z, key + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)
                || z == key + 1) {
                __atomic_fetch_add(&s_tab[h].count, 1, __ATOMIC_RELAXED);
                return;
            }
        }
    }
    __atomic_fetch_add(&s_dropped, 1, __ATOMIC_RELAXED);
}


/* Call chains (Windows): the first CHAIN_N frames of each sample that are in
 * the main module, innermost first. A thread blocked in ntdll shows here as
 * the host function that waited and the guest function that called it. */
#define CHAIN_N 4
#define CTAB_SIZE 8192
typedef struct { volatile uint32_t key_hi; volatile uint32_t count; uint32_t thr; uint32_t off[CHAIN_N]; } Chain;
static Chain s_ctab[CTAB_SIZE];
static volatile uint32_t s_cdropped;

__attribute__((unused)) static void record_chain(int ti, const uint32_t *off, int n)
{
    uint64_t h64 = 0xCBF29CE484222325ull ^ (uint64_t)ti;
    uint32_t o[CHAIN_N] = {0};
    for (int i = 0; i < n && i < CHAIN_N; i++) { o[i] = off[i]; h64 = (h64 ^ off[i]) * 0x100000001B3ull; }
    uint32_t tag = (uint32_t)(h64 >> 32) | 1u, h = (uint32_t)h64 & (CTAB_SIZE - 1);
    for (uint32_t k = 0; k < 64; k++, h = (h + 1) & (CTAB_SIZE - 1)) {
        Chain *c = &s_ctab[h];
        if (c->key_hi == tag && c->thr == (uint32_t)ti && !memcmp(c->off, o, sizeof o)) { c->count++; return; }
        if (c->key_hi == 0) {                 /* only the sampler thread writes this table */
            c->thr = (uint32_t)ti;
            memcpy(c->off, o, sizeof o);
            c->count = 1;
            c->key_hi = tag;
            return;
        }
    }
    s_cdropped++;
}

/* ---------------------------------------------------------------- modules */
#ifdef _WIN32
static void refresh_mods(void)
{
    HMODULE h[MAX_MODS];
    DWORD need = 0;
    Mod tmp[MAX_MODS];
    int n = 0;
    if (!EnumProcessModules(GetCurrentProcess(), h, sizeof h, &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && n < MAX_MODS; i++) {
        MODULEINFO mi;
        char path[MAX_PATH];
        if (!GetModuleInformation(GetCurrentProcess(), h[i], &mi, sizeof mi)) continue;
        if (!GetModuleFileNameA(h[i], path, sizeof path)) strcpy(path, "?");
        const char *b = strrchr(path, '\\');
        tmp[n].base = (uintptr_t)mi.lpBaseOfDll;
        tmp[n].end = tmp[n].base + mi.SizeOfImage;
        snprintf(tmp[n].name, sizeof tmp[n].name, "%s", b ? b + 1 : path);
        n++;
    }
    publish_mods(tmp, n);
}
#else
static Mod s_tmp[MAX_MODS];
static int s_ntmp;
static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size; (void)data;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD) continue;
        uintptr_t a = info->dlpi_addr + p->p_vaddr;
        if (a < lo) lo = a;
        if (a + p->p_memsz > hi) hi = a + p->p_memsz;
    }
    if (s_ntmp >= MAX_MODS || hi <= lo) return 0;
    const char *nm = info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "pcj";
    const char *b = strrchr(nm, '/');
    s_tmp[s_ntmp].base = info->dlpi_name && *info->dlpi_name ? lo : 0;   /* main exe: non-PIE, absolute */
    s_tmp[s_ntmp].end = hi;
    snprintf(s_tmp[s_ntmp].name, sizeof s_tmp[s_ntmp].name, "%s", b ? b + 1 : nm);
    s_ntmp++;
    return 0;
}
static void refresh_mods(void)
{
    s_ntmp = 0;
    dl_iterate_phdr(phdr_cb, NULL);
    publish_mods(s_tmp, s_ntmp);
}
#endif

/* ---------------------------------------------------------------- report */
typedef struct { uint64_t key; uint32_t count; } Ent;
static int cmp_ent(const void *a, const void *b)
{
    const Ent *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static char s_path[1024];

static void write_report(void)
{
    static Ent ents[TAB_SIZE];
    int n = 0;
    for (uint32_t i = 0; i < TAB_SIZE; i++)
        if (s_tab[i].key) { ents[n].key = s_tab[i].key - 1; ents[n].count = s_tab[i].count; n++; }
    qsort(ents, n, sizeof(Ent), cmp_ent);
    if (!s_path[0]) {
        const char *d = gamelog_dir();
        snprintf(s_path, sizeof s_path, "%s%sprof.txt", d ? d : ".", d ? "/" : "");
    }
    FILE *f = fopen(s_path, "w");
    if (!f) return;
    fprintf(f, "# pcj sampling profile: %d Hz, %u samples over %ld s, %u dropped\n", s_hz, s_total,
            (long)(time(NULL) - s_t0), s_dropped);
    fprintf(f, "# modules\n");
    for (int m = 0; m < s_nmods; m++)
        fprintf(f, "mod %d %s %llx %llx\n", m, s_mods[m].name, (unsigned long long)s_mods[m].base,
                (unsigned long long)s_mods[m].end);
    for (int t = 0; t < s_nthr; t++) {
        uint32_t ts = s_thr[t].samples;
        if (!ts) continue;
        uintptr_t so = 0;
        int sm = mod_of(s_thr[t].start, &so);
        fprintf(f, "\n## thread %d (tid %u, start %s+0x%llx): %u samples = %.1f s of CPU-or-wait\n", t,
                s_thr[t].tid, sm == 0x7F ? "?" : s_mods[sm].name, (unsigned long long)so, ts, (double)ts / s_hz);
        uint32_t bym[MAX_MODS + 1] = {0};
        for (int i = 0; i < n; i++)
            if ((int)(ents[i].key >> 56) == t) {
                int m = (ents[i].key >> 48) & 0x7F;
                bym[m == 0x7F ? MAX_MODS : m] += ents[i].count;
            }
        for (int m = 0; m <= MAX_MODS; m++)
            if (bym[m] * 200 >= ts)
                fprintf(f, "  %5.1f%%  %s\n", 100.0 * bym[m] / ts, m == MAX_MODS ? "(unknown)" : s_mods[m].name);
        int shown = 0;
        for (int i = 0; i < n && shown < 25; i++) {
            if ((int)(ents[i].key >> 56) != t) continue;
            int m = (ents[i].key >> 48) & 0x7F;
            fprintf(f, "    %5.1f%%  %s+0x%llx\n", 100.0 * ents[i].count / ts,
                    m == 0x7F ? "?" : s_mods[m].name, (unsigned long long)(ents[i].key & 0xFFFFFFFFFFFFull));
            shown++;
        }
    }
    fprintf(f, "\n# chains: thread count off0 off1 off2 off3 (offsets in module 0, innermost first; %u dropped)\n",
            s_cdropped);
    for (uint32_t i = 0; i < CTAB_SIZE; i++)
        if (s_ctab[i].key_hi && s_ctab[i].count >= 2)
            fprintf(f, "chain %u %u %x %x %x %x\n", s_ctab[i].thr, s_ctab[i].count, s_ctab[i].off[0],
                    s_ctab[i].off[1], s_ctab[i].off[2], s_ctab[i].off[3]);
    fprintf(f, "\n# raw: thread module offset count\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "raw %d %d %llx %u\n", (int)(ents[i].key >> 56), (int)((ents[i].key >> 48) & 0x7F),
                (unsigned long long)(ents[i].key & 0xFFFFFFFFFFFFull), ents[i].count);
    fclose(f);
}

/* ---------------------------------------------------------------- sampler */
#ifdef _WIN32
typedef LONG (WINAPI *NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
static HANDLE s_handles[MAX_THREADS];

static void refresh_threads(DWORD self)
{
    static NtQIT q;
    if (!q) q = (NtQIT)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { .dwSize = sizeof te };
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || thr_index(te.th32ThreadID) >= 0) continue;
        if (s_nthr >= MAX_THREADS) break;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!h) continue;
        uintptr_t start = 0;
        if (q) q(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL);
        s_handles[s_nthr] = h;
        s_thr[s_nthr].tid = te.th32ThreadID;
        s_thr[s_nthr].start = start;
        s_nthr++;
    }
    CloseHandle(snap);
}

/* .pdata of each registered module, found once from its PE headers. Looked up
 * here rather than with RtlLookupFunctionEntry, which can take a loader lock a
 * suspended thread may hold. */
typedef struct { const RUNTIME_FUNCTION *fn; uint32_t n; int done; } Pdata;
static Pdata s_pdata[MAX_MODS];

static const RUNTIME_FUNCTION *find_fn(int m, uintptr_t pc)
{
    Pdata *p = &s_pdata[m];
    if (!p->done) {
        const uint8_t *base = (const uint8_t *)s_reg[m].base;
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        p->done = 1;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
                const IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if (d->VirtualAddress && d->Size) {
                    p->fn = (const RUNTIME_FUNCTION *)(base + d->VirtualAddress);
                    p->n = d->Size / sizeof(RUNTIME_FUNCTION);
                }
            }
        }
    }
    if (!p->fn) return NULL;
    uint32_t rva = (uint32_t)(pc - s_reg[m].base), lo = 0, hi = p->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rva < p->fn[mid].BeginAddress) hi = mid;
        else if (rva >= p->fn[mid].EndAddress) lo = mid + 1;
        else return &p->fn[mid];
    }
    return NULL;
}

/* Walk the suspended thread's stack from its context; collect up to CHAIN_N
 * frames in module 0 (the exe). Stops at the first frame outside a module. */
static int walk(CONTEXT *c, uint32_t *out)
{
    int n = 0;
    for (int depth = 0; depth < 48 && n < CHAIN_N; depth++) {
        uintptr_t pc = (uintptr_t)c->Rip, off;
        int m = mod_of(pc, &off);
        if (m == 0x7F || pc < 0x10000) break;
        if (m == 0) out[n++] = (uint32_t)off;
        const RUNTIME_FUNCTION *fn = find_fn(m, pc);
        if (fn) {
            PVOID hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, (DWORD64)s_reg[m].base, (DWORD64)pc, (PRUNTIME_FUNCTION)fn, c, &hd,
                             &est, NULL);
        } else {                                  /* leaf: return address at [rsp] */
            if (IsBadReadPtr((void *)c->Rsp, 8)) break;
            c->Rip = *(DWORD64 *)c->Rsp;
            c->Rsp += 8;
        }
        if (!c->Rip) break;
    }
    return n;
}

static DWORD WINAPI sampler(LPVOID arg)
{
    (void)arg;
    DWORD self = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    DWORD last_scan = 0, last_dump = GetTickCount();
    LARGE_INTEGER fq, now, next;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&next);
    while (!s_stop) {
        DWORD tick = GetTickCount();
        if (tick - last_scan >= 1000) { refresh_mods(); refresh_threads(self); last_scan = tick; }
        for (int i = 0; i < s_nthr; i++) {
            if (!s_handles[i]) continue;
            if (SuspendThread(s_handles[i]) == (DWORD)-1) { CloseHandle(s_handles[i]); s_handles[i] = NULL; continue; }
            CONTEXT c;
            uint32_t chain[CHAIN_N];
            int nc = 0;
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            BOOL ok = GetThreadContext(s_handles[i], &c);
            uintptr_t pc = (uintptr_t)c.Rip;
            if (ok && s_chains) nc = walk(&c, chain);   /* reads only the stopped thread's stack */
            ResumeThread(s_handles[i]);
            if (ok) { record(i, pc); if (nc) record_chain(i, chain, nc); }
        }
        if (tick - last_dump >= 30000) { write_report(); last_dump = tick; }
        next.QuadPart += fq.QuadPart / s_hz;
        QueryPerformanceCounter(&now);
        if (next.QuadPart > now.QuadPart) Sleep((DWORD)((next.QuadPart - now.QuadPart) * 1000 / fq.QuadPart));
        else next = now;
    }
    return 0;
}
#else
static void on_prof(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
    ucontext_t *uc = ucv;
    int ti = thr_index((uint32_t)syscall(SYS_gettid));
#if defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    uintptr_t pc = 0;
#endif
    if (ti >= 0) record(ti, pc);
}

static void refresh_threads(pid_t self)
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name);
        if (tid <= 0 || tid == self || thr_index((uint32_t)tid) >= 0 || s_nthr >= MAX_THREADS) continue;
        s_thr[s_nthr].tid = (uint32_t)tid;
        s_thr[s_nthr].start = 0;
        s_nthr++;
    }
    closedir(d);
}

static void *sampler(void *arg)
{
    (void)arg;
    pid_t self = (pid_t)syscall(SYS_gettid), pid = getpid();
    struct timespec ts = { 0, 1000000000L / s_hz };
    time_t last_scan = 0, last_dump = time(NULL);
    while (!s_stop) {
        time_t now = time(NULL);
        if (now != last_scan) { refresh_mods(); refresh_threads(self); last_scan = now; }
        for (int i = 0; i < s_nthr; i++)
            if (s_thr[i].tid) syscall(SYS_tgkill, pid, (pid_t)s_thr[i].tid, SIGPROF);
        if (now - last_dump >= 30) { write_report(); last_dump = now; }
        nanosleep(&ts, NULL);
    }
    return NULL;
}
#endif

static void prof_exit(void)
{
    s_stop = 1;
    write_report();
}

void prof_start(void)
{
    const char *e = getenv("CJ_PROF");
    if (e && *e == '0') return;
    if (getenv("CJ_PROF_HZ")) s_hz = atoi(getenv("CJ_PROF_HZ"));
    if (getenv("CJ_PROF_CHAINS") && *getenv("CJ_PROF_CHAINS") == '1') s_chains = 1;
    if (s_hz < 10) s_hz = 10;
    if (s_hz > 2000) s_hz = 2000;
    s_t0 = time(NULL);
    refresh_mods();
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (h) CloseHandle(h);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, sampler, NULL);
    pthread_detach(t);
#endif
    atexit(prof_exit);
    fprintf(stderr, "[PROF] sampling all threads at %d Hz (CJ_PROF=0 turns it off)\n", s_hz);
}
