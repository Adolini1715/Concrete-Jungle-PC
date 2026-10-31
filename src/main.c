/*
 * Predator: Concrete Jungle -- static recompilation host entry (POSIX).
 *
 * XBE: Title ID 0x56550030, base 0x00010000, entry 0x002CEE3E
 * (_mainCRTStartup per the retail linker map), XDK 5849.
 *
 * Boot order follows xboxrecomp's templates/new-game/src/main.c; the
 * Win32-only pieces (WinMain, VEH, dbghelp) are replaced with a POSIX
 * SIGSEGV/SIGBUS handler that offers the fault to the toolkit's MMIO models
 * first and otherwise prints the guest register file and guest call chain.
 *
 * Windows (MinGW-w64 cross build, docs/windows-build.md): the toolkit's VEH
 * path (xbox_VehMmioFault) replaces the signal handler; settings come from
 * pcj.ini next to the exe and the command line; output goes to pcj-log.txt.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "async_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <io.h>
#else
#include <signal.h>
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include <xbox/xboxrecomp.h>
#include "pcsettings.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

extern void m_mainCRTStartup_002CEE3E(void);
#define xbe_entry_point m_mainCRTStartup_002CEE3E
extern int recomp_dispatch_init(void);

#define PCJ_ENTRY 0x002CEE3Eu

static void print_guest_regs(void)
{
    int i, n;
    const uint32_t *sp;
    fprintf(stderr, "  guest eax=%08X ecx=%08X edx=%08X ebx=%08X\n", g_eax, g_ecx, g_edx, g_ebx);
    fprintf(stderr, "  guest esi=%08X edi=%08X esp=%08X\n", g_esi, g_edi, g_esp);
    if (g_xbox_mem_offset && g_esp) {
        sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        fprintf(stderr, "  guest stack return addresses:\n");
        for (i = 0, n = 0; i < 256 && n < 24; i++)
            if (sp[i] > g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%d] 0x%08X\n", i * 4, sp[i]);
                n++;
            }
    }
}

#ifdef _WIN32
extern void cj_gl_register_env(void);

/* Exception census for the [FPS] line: every exception the process takes
 * (trapped device registers, single-steps) is a kernel round trip, so a high
 * rate is a frame-rate problem on Windows. Counted by a first handler that
 * always passes the exception on. */
volatile LONG64 cj_exc_av, cj_exc_ss, cj_exc_other;
static LONG CALLBACK veh_count(PEXCEPTION_POINTERS ep)
{
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_ACCESS_VIOLATION) InterlockedIncrement64(&cj_exc_av);
    else if (c == EXCEPTION_SINGLE_STEP) InterlockedIncrement64(&cj_exc_ss);
    else InterlockedIncrement64(&cj_exc_other);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    static volatile LONG reported;
    if (code == EXCEPTION_ACCESS_VIOLATION && xbox_VehMmioFault(ep))
        return EXCEPTION_CONTINUE_EXECUTION;      /* trapped device register, stepped */
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR)
        return EXCEPTION_CONTINUE_SEARCH;         /* debug strings, thread names, C++ */
    if (InterlockedExchange(&reported, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    {
        uintptr_t fault = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
                              ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
        fprintf(stderr, "\n[CRASH] exception 0x%08lX at RIP=0x%llX, fault addr 0x%llX (guest VA 0x%08X)\n",
                (unsigned long)code, (unsigned long long)ep->ContextRecord->Rip,
                (unsigned long long)fault, (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset));
        {
            HMODULE hm = NULL;
            char mn[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)ep->ContextRecord->Rip, &hm) && hm)
                GetModuleFileNameA(hm, mn, sizeof mn);
            fprintf(stderr, "[CRASH] in %s +0x%llX, thread %lu, rsp 0x%llX\n", mn,
                    (unsigned long long)(ep->ContextRecord->Rip - (uintptr_t)hm), GetCurrentThreadId(),
                    (unsigned long long)ep->ContextRecord->Rsp);
            fflush(stderr);
        }
        print_guest_regs();
        fflush(stderr);
        fflush(stdout);
        { extern void gamelog_crash(const char *); char w[64];
          snprintf(w, sizeof w, "exception 0x%08lX", (unsigned long)code); gamelog_crash(w); }
        cj_afile_drain(1500);    /* let the background writers put the report on disk */
        TerminateProcess(GetCurrentProcess(), 3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void drain_at_exit(void) { cj_afile_drain(1000); }

static DWORD WINAPI log_flusher(LPVOID arg)
{
    (void)arg;
    for (;;) { Sleep(250); fflush(stdout); fflush(stderr); }
    return 0;
}

static char s_exe_dir[MAX_PATH];
static char s_ini_game[MAX_PATH], s_ini_save[MAX_PATH];

static void trim(char *t)
{
    size_t n = strlen(t);
    while (n && (t[n - 1] == '\r' || t[n - 1] == '\n' || t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '"'))
        t[--n] = 0;
    while (*t == ' ' || *t == '\t' || *t == '"') memmove(t, t + 1, strlen(t));
}

/* pcj.ini next to the exe: game=<dir>, save=<dir>, and any other KEY=VALUE
 * becomes an environment variable unless already set (run.bat / the shell
 * override the ini). */
static void load_ini(void)
{
    char path[MAX_PATH + 16], line[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s\\pcj.ini", s_exe_dir);
    if (!(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof line, f)) {
        char *eq, *k = line, *v;
        trim(k);
        if (!*k || *k == '#' || *k == ';' || *k == '[') continue;
        if (!(eq = strchr(k, '='))) continue;
        *eq = 0; v = eq + 1;
        trim(k); trim(v);
        if (!_stricmp(k, "game")) snprintf(s_ini_game, sizeof s_ini_game, "%s", v);
        else if (!_stricmp(k, "save")) snprintf(s_ini_save, sizeof s_ini_save, "%s", v);
        else if (!getenv(k)) _putenv_s(k, v);
    }
    fclose(f);
    fprintf(stderr, "[PC] read %s\n", path);
}

static void env_default(const char *k, const char *v)
{
    if (!getenv(k)) _putenv_s(k, v);
}

/* Settings the Linux run.sh exports, plus the PC defaults (docs/windows-build.md). */
static void pc_defaults(void)
{
    env_default("RECOMP_AC97_READY", "1");
    env_default("RECOMP_TITLE_KEVENTS", "1");
    env_default("RECOMP_VBLANK", "paced");   /* the 60 Hz schedule; "1" is the old ~40 Hz tick */
    env_default("RECOMP_PB_EXEC", "1");
    env_default("CJ_GL", "1");
    env_default("CJ_PAD_HOST", "1");
}

/* The toolkit's APU tries XAudio2 first, then waveOut. Wine's XAudio2
 * (FAudio) faults inside CreateMasteringVoice when the host has no audio
 * device, so skip it then (and on CJ_AUDIO_XA2=0); linked with
 * -Wl,--wrap=xa2_init (CMakeLists.txt). */
int __real_xa2_init(void);
int __wrap_xa2_init(void)
{
    const char *e = getenv("CJ_AUDIO_XA2");
    UINT n = waveOutGetNumDevs();
    if ((e && *e == '0') || n == 0) {
        fprintf(stderr, "[PC] XAudio2 skipped (%s), waveOut fallback\n",
                n == 0 ? "no audio output device" : "CJ_AUDIO_XA2=0");
        return 0;
    }
    return __real_xa2_init();
}

static int dir_has_xbe(const char *d)
{
    char p[MAX_PATH + 32];
    snprintf(p, sizeof p, "%s\\default.xbe", d);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

/* First run: no game folder configured or found. Ask for the folder with default.xbe (the
 * extracted disc) and remember it in pcj.ini as game=<folder>. */
static int pick_game_dir(char *out, size_t n)
{
    BROWSEINFOA bi;
    LPITEMIDLIST id;
    char dir[MAX_PATH];
    for (;;) {
        memset(&bi, 0, sizeof bi);
        bi.lpszTitle = "Choose your Predator: Concrete Jungle game folder (the extracted disc, with default.xbe in it)";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        id = SHBrowseForFolderA(&bi);
        if (!id) return 0;
        dir[0] = 0;
        SHGetPathFromIDListA(id, dir);
        CoTaskMemFree(id);
        if (dir[0] && dir_has_xbe(dir)) break;
        if (MessageBoxA(NULL, "That folder has no default.xbe.\n\nPick the folder you extracted the game disc into.",
                        "Predator: Concrete Jungle", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            return 0;
    }
    snprintf(out, n, "%s", dir);
    {   /* append game=<dir> to pcj.ini (load_ini reads it next time) */
        char ini[MAX_PATH + 16];
        FILE *f;
        snprintf(ini, sizeof ini, "%s\\pcj.ini", s_exe_dir);
        if ((f = fopen(ini, "a"))) { fprintf(f, "\ngame=%s\n", dir); fclose(f); }
    }
    return 1;
}
#else
static struct sigaction s_prev_segv, s_prev_bus;

static void crash_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t fault = (uintptr_t)si->si_addr;
    if (xbox_PosixMmioFault(ucv, fault))
        return;                       /* trapped device register, stepped */
    {
        static int depth;
        void *bt[48];
        int n, i;
        const uint32_t *sp;
        (void)sp; (void)i;
        if (depth++) _exit(128 + sig);
        fprintf(stderr, "\n[CRASH] signal %d at host addr %p (guest VA 0x%08X)\n",
                sig, si->si_addr, (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset));
        print_guest_regs();
        n = backtrace(bt, 48);
        fprintf(stderr, "  host backtrace:\n");
        backtrace_symbols_fd(bt, n, 2);
        fflush(stderr);
        _exit(128 + sig);
    }
}
#endif

static void *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *d;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc((size_t)n);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *size = (size_t)n;
    return d;
}

int main(int argc, char **argv)
{
    const char *game_dir = argc > 1 ? argv[1] : getenv("PCJ_GAME_DIR");
    const char *save_dir = argc > 2 ? argv[2] : getenv("PCJ_SAVE_DIR");
    char xbe_path[4096];
    size_t xbe_size = 0;
    void *xbe;
#ifdef _WIN32
    static char save_buf[MAX_PATH + 16];
    {
        /* Everything goes to pcj-log.txt next to the exe (PCJ_LOG=- keeps the
         * console, PCJ_LOG=<file> picks another file). */
        char *slash, logp[MAX_PATH + 32];
        const char *lg = getenv("PCJ_LOG");
        GetModuleFileNameA(NULL, s_exe_dir, sizeof s_exe_dir);
        if ((slash = strrchr(s_exe_dir, '\\'))) *slash = 0;
        if (lg && !strcmp(lg, "-") && AllocConsole()) {   /* a GUI app has no console: make one */
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        if (!lg || strcmp(lg, "-")) {
            if (lg && *lg) snprintf(logp, sizeof logp, "%s", lg);
            else snprintf(logp, sizeof logp, "%s\\pcj-log.txt", s_exe_dir);
            /* written by a background thread (src/async_io.c): a slow disk or a
             * OneDrive folder must not stall the game thread */
            if (!cj_afile_redirect_std(logp) && freopen(logp, "w", stdout))
                _dup2(_fileno(stdout), _fileno(stderr));
            atexit(drain_at_exit);   /* registered first, so it runs after every other exit hook */
        }
    }
    /* The log file is block-buffered and flushed once a second by the renderer
     * (and on exit / crash). Unbuffered, every line was its own WriteFile:
     * ~17 per frame, and in a OneDrive / antivirus-scanned folder each one can
     * take milliseconds, which is a frame-rate problem by itself. The console
     * (PCJ_LOG=-) stays unbuffered. */
    {
        const char *lg = getenv("PCJ_LOG");
        int to_file = !lg || strcmp(lg, "-");
        setvbuf(stdout, NULL, to_file ? _IOFBF : _IONBF, to_file ? 1 << 16 : 0);
        setvbuf(stderr, NULL, to_file ? _IOFBF : _IONBF, to_file ? 1 << 16 : 0);
        /* A start-up that dies or hangs before the renderer's once-a-second
         * flush left an empty log: flush from a thread of its own too. */
        if (to_file) {
            HANDLE ft = CreateThread(NULL, 0, log_flusher, NULL, 0, NULL);
            if (ft) CloseHandle(ft);
        }
    }
    /* The crash handler goes in before anything else can fault: registered
     * after the memory layout, a fault while building it killed the process
     * with nothing in the log. It is added "last", so the toolkit's AC97 /
     * watch handlers, registered later as "first", still see their traps
     * before it. */
    AddVectoredExceptionHandler(0, veh_handler);
    load_ini();
    {   /* Options > PC Settings saves its values here (pcsettings.c) */
        char ini[MAX_PATH + 16];
        snprintf(ini, sizeof ini, "%s\\pcj.ini", s_exe_dir);
        pcs_set_ini_path(ini);
    }
    pc_defaults();
    if (!game_dir && s_ini_game[0]) game_dir = s_ini_game;
    if (!game_dir && dir_has_xbe(s_exe_dir)) game_dir = s_exe_dir;
    {   /* a "game" folder next to the exe (the README's layout) */
        static char sub[MAX_PATH + 8];
        snprintf(sub, sizeof sub, "%s\\game", s_exe_dir);
        if (!game_dir && dir_has_xbe(sub)) game_dir = sub;
    }
    {
        static char picked[MAX_PATH];
        if (!game_dir && pick_game_dir(picked, sizeof picked)) game_dir = picked;
    }
    if (!save_dir && s_ini_save[0]) save_dir = s_ini_save;
    if (!save_dir) {
        snprintf(save_buf, sizeof save_buf, "%s\\save", s_exe_dir);
        CreateDirectoryA(save_buf, NULL);
        save_dir = save_buf;
    }
    timeBeginPeriod(1);   /* 1 ms Sleep granularity for the frame pacing */
    {
        OSVERSIONINFOA vi;
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        const char *(*wine_ver)(void) = ntdll ? (const char *(*)(void))(void (*)(void))GetProcAddress(ntdll, "wine_get_version") : NULL;
        memset(&vi, 0, sizeof vi);
        vi.dwOSVersionInfoSize = sizeof vi;
        fprintf(stderr, "[PC] exe dir %s, game dir %s, save dir %s, %s%s\n", s_exe_dir,
                game_dir ? game_dir : "(none)", save_dir, wine_ver ? "Wine " : "Windows",
                wine_ver ? wine_ver() : "");
    }
    if (!game_dir) {
        fprintf(stderr, "No game directory. Pass it as the first argument, or put\n"
                        "game=<folder with default.xbe> in pcj.ini next to the exe.\n");
        MessageBoxA(NULL, "No game folder given.\n\nEdit pcj.ini next to pcj.exe: game=<folder that contains default.xbe>",
                    "Predator: Concrete Jungle", MB_OK | MB_ICONERROR);
        return 2;
    }
    cj_gl_register_env();
#else
    struct sigaction sa;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (!game_dir) {
        fprintf(stderr, "usage: %s <extracted disc dir> [save dir]\n", argv[0]);
        return 2;
    }
#endif
    printf("=== Predator: Concrete Jungle - static recompilation ===\n");

    snprintf(xbe_path, sizeof xbe_path, "%s/DEFAULT.XBE", game_dir);
    xbe = load_file(xbe_path, &xbe_size);
    if (!xbe) {
        snprintf(xbe_path, sizeof xbe_path, "%s/default.xbe", game_dir);
        xbe = load_file(xbe_path, &xbe_size);
    }
    if (!xbe) {
        fprintf(stderr, "cannot read DEFAULT.XBE / default.xbe in %s\n", game_dir);
#ifdef _WIN32
        MessageBoxA(NULL, "Cannot find default.xbe in the game folder (see pcj-log.txt / pcj.ini).",
                    "Predator: Concrete Jungle", MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
    printf("XBE loaded: %llu bytes\n", (unsigned long long)xbe_size);

    { void prof_start(void); prof_start(); }   /* src/prof.c: sampling profiler, CJ_PROF=0 off */
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "[FATAL] xbox_MemoryLayoutInit failed: could not reserve/map the Xbox memory window\n"
                        "        (guest RAM + mirrors). The lines above say which mapping failed.\n");
#ifdef _WIN32
        MessageBoxA(NULL, "Could not map the Xbox memory window (xbox_MemoryLayoutInit failed).\nSee pcj-log.txt.",
                    "Predator: Concrete Jungle", MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped, offset 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* CJ_NULLHW=1: the XDK D3D library's own no-GPU mode (D3D__NullHardware,
     * BSS 0x00307D70). After its first real KickOff (which the executor still
     * services) D3D retires every pushbuffer at once (GET = PUT in
     * g_NullHardwareGetPut) and never hands work to a GPU. The fence time it
     * leaves behind is caught up in the BlockOnTime bridge (recomp_manual.c).
     * Nothing is drawn by the executor after that first kick: this mode is for
     * the native renderer and for headless runs. */
    /* CJ_NULLHW=2: the same, and the pushbuffer is executed synchronously at
     * each kickoff on the game thread (src/native_gfx.c). */
    {
        extern int cj_gfx_mode(void);
        int m = cj_gfx_mode();
        if (m) {
            *(volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + 0x00307D70u) = 1;
            fprintf(stderr, "[BOOT] CJ_NULLHW=%d: D3D__NullHardware set (%s)\n", m,
                    m == 2 ? "synchronous executor at kickoff" : "nothing is drawn");
        }
    }

#ifdef _WIN32
    AddVectoredExceptionHandler(1, veh_count);     /* first: counts, never handles */
#else
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_prev_segv);
    sigaction(SIGBUS, &sa, &s_prev_bus);
#endif

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED");
    }

    {   /* CJ_REC / CJ_REC_PLAY: session recording and exact replay (src/cj_rec.c); before the
         * deterministic mode reads its settings */
        extern void cj_rec_boot(const char *save_dir, int fps60);
        const char *f = getenv("CJ_FPS");
        cj_rec_boot(save_dir, f && atoi(f) == 60);
    }
    xbox_kernel_init();
    {   /* mods folder (docs/mods-folder.md): <exe folder>/mods, or CJ_MODS_DIR; CJ_MODS_DIR=0 off.
           Off in recordings: a replay must read the same data. */
        extern char g_xbox_overlay_dir[1024];
        extern int cj_det_enabled(void);
        const char *m = getenv("CJ_MODS_DIR");
        char d[1024];
        struct stat sb;
        if (m && *m) snprintf(d, sizeof d, "%s", m);
        else {
#ifdef _WIN32
            snprintf(d, sizeof d, "%s/mods", s_exe_dir);
#else
            char exe[900];
            ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
            char *sl;
            exe[k > 0 ? k : 0] = 0;
            if ((sl = strrchr(exe, '/'))) *sl = 0;
            snprintf(d, sizeof d, "%s/mods", exe);
#endif
        }
        if (!(m && !strcmp(m, "0")) && !cj_det_enabled() && stat(d, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            snprintf(g_xbox_overlay_dir, sizeof g_xbox_overlay_dir, "%s", d);
            fprintf(stderr, "[MODDIR] overlay folder %s: its files replace game files of the same path\n", d);
        }
    }
    xbox_path_init(game_dir, save_dir);
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable\n");
    xbox_WatchdogStart();

    printf("Entry 0x%08X, ESP 0x%08X. Starting game...\n", PCJ_ENTRY, g_esp);
    xbe_entry_point();
    printf("Game returned.\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    return 0;
}
