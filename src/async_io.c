/* Background file writing: nothing the port writes during play (pcj-log.txt,
 * frames.csv, events.jsonl, screenshots) may stall the game thread.
 *
 * Why: PC Build sits in the user's OneDrive folder, where a WriteFile can
 * take milliseconds (sync client, antivirus). The game thread used to do
 * those writes itself: every 64 KB of log, the once-a-second flush, the
 * gamelog flush every 2 s, and screenshot BMPs. In a 60 fps session those
 * showed up as 19-50 ms frames.
 *
 * Streams (Windows): cj_afile_open() returns an ordinary FILE * whose fd is
 * the write end of an anonymous pipe with a large buffer. A writer thread
 * per stream copies the pipe to the real file. fprintf/fflush on the game
 * thread then only copy memory into the pipe. If the pipe is ever full
 * (the disk stalled for longer than the buffer covers) the writer blocks,
 * as before; nothing is dropped.
 * cj_afile_redirect_std() does the same for stdout/stderr (pcj-log.txt).
 *
 * Whole files (screenshots): cj_afile_save_bmp() hands an SDL surface to a
 * worker thread that saves and frees it.
 *
 * cj_afile_drain(ms): flush the C streams and wait until the writers have
 * caught up (exit, crash report).
 *
 * Elsewhere (Linux test builds) streams are plain fopen and screenshots are
 * saved on the calling thread.
 *
 * CJ_ASYNC_IO=0 turns all of it off (direct writes, the old behaviour). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "async_io.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>

#define PIPE_BYTES (4u << 20)
#define MAX_STREAMS 8

typedef struct {
    HANDLE rd, file;
    volatile LONG busy;          /* a read has returned and its write is in progress */
} Stream;

static Stream s_st[MAX_STREAMS];
static volatile LONG s_nst;
static int s_on = -1;

static int on(void)
{
    if (s_on < 0) {
        const char *e = getenv("CJ_ASYNC_IO");
        s_on = !(e && *e == '0');
    }
    return s_on;
}

static DWORD WINAPI stream_thread(LPVOID arg)
{
    Stream *s = (Stream *)arg;
    static char bufs[MAX_STREAMS][1 << 16];
    char *buf = bufs[s - s_st];
    DWORD n, w;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        if (!ReadFile(s->rd, buf, sizeof bufs[0], &n, NULL) || n == 0)
            break;                                   /* write end closed */
        InterlockedExchange(&s->busy, 1);
        WriteFile(s->file, buf, n, &w, NULL);
        InterlockedExchange(&s->busy, 0);
    }
    CloseHandle(s->file);
    return 0;
}

/* A pipe whose reader thread writes to `path`; returns the write end as a CRT fd. */
static int open_stream_fd(const char *path, int text)
{
    HANDLE f, rd, wr, th;
    LONG i;
    int fd;
    if (s_nst >= MAX_STREAMS) return -1;
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    if (!CreatePipe(&rd, &wr, NULL, PIPE_BYTES)) { CloseHandle(f); return -1; }
    fd = _open_osfhandle((intptr_t)wr, (text ? _O_TEXT : _O_BINARY) | _O_WRONLY);
    if (fd < 0) { CloseHandle(rd); CloseHandle(wr); CloseHandle(f); return -1; }
    i = InterlockedIncrement(&s_nst) - 1;
    s_st[i].rd = rd;
    s_st[i].file = f;
    th = CreateThread(NULL, 0, stream_thread, &s_st[i], 0, NULL);
    if (!th) { _close(fd); CloseHandle(f); return -1; }
    CloseHandle(th);
    return fd;
}

FILE *cj_afile_open(const char *path, const char *mode)
{
    int text = !strchr(mode, 'b'), fd;
    FILE *f;
    if (!on() || mode[0] != 'w') return fopen(path, mode);
    if ((fd = open_stream_fd(path, text)) < 0) return fopen(path, mode);
    if (!(f = _fdopen(fd, text ? "w" : "wb"))) { _close(fd); return fopen(path, mode); }
    return f;
}

int cj_afile_redirect_std(const char *path)
{
    int fd;
    if (!on()) return 0;
    if ((fd = open_stream_fd(path, 1)) < 0) return 0;
    /* stdout/stderr of a GUI-subsystem exe may have no valid fd yet: give them one first */
    if (_fileno(stdout) < 0 && !freopen("NUL", "w", stdout)) return 0;
    if (_fileno(stderr) < 0 && !freopen("NUL", "w", stderr)) return 0;
    if (_dup2(fd, _fileno(stdout)) || _dup2(fd, _fileno(stderr))) return 0;
    _close(fd);
    return 1;
}

/* ---- whole files: screenshots ---- */
#define SHOTQ 32
typedef struct { SDL_Surface *sf; char path[1200]; } Shot;
static Shot s_q[SHOTQ];
static volatile LONG s_qhead, s_qtail;       /* producer: game thread only */
static HANDLE s_qsem;
static volatile LONG s_qbusy;

static DWORD WINAPI shot_thread(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Shot *s;
        WaitForSingleObject(s_qsem, INFINITE);
        InterlockedExchange(&s_qbusy, 1);
        s = &s_q[s_qtail % SHOTQ];
        SDL_SaveBMP(s->sf, s->path);
        SDL_FreeSurface(s->sf);
        InterlockedIncrement(&s_qtail);
        InterlockedExchange(&s_qbusy, 0);
    }
    return 0;
}

int cj_afile_save_bmp(SDL_Surface *sf, const char *path)
{
    static int started;
    if (!on()) { int r = SDL_SaveBMP(sf, path); SDL_FreeSurface(sf); return r; }
    if (!started) {
        HANDLE th;
        s_qsem = CreateSemaphoreA(NULL, 0, SHOTQ, NULL);
        th = s_qsem ? CreateThread(NULL, 0, shot_thread, NULL, 0, NULL) : NULL;
        if (!th) { s_on = 0; return cj_afile_save_bmp(sf, path); }
        CloseHandle(th);
        started = 1;
    }
    if (s_qhead - s_qtail >= SHOTQ) { SDL_FreeSurface(sf); return -1; }   /* queue full: skip this one */
    s_q[s_qhead % SHOTQ].sf = sf;
    snprintf(s_q[s_qhead % SHOTQ].path, sizeof s_q[0].path, "%s", path);
    InterlockedIncrement(&s_qhead);
    ReleaseSemaphore(s_qsem, 1, NULL);
    return 0;
}

void cj_afile_drain(int ms)
{
    DWORD t0 = GetTickCount();
    LONG i;
    fflush(NULL);
    while ((int)(GetTickCount() - t0) < ms) {
        int pending = s_qhead != s_qtail || s_qbusy;
        for (i = 0; i < s_nst && !pending; i++) {
            DWORD avail = 0;
            if (s_st[i].busy || (PeekNamedPipe(s_st[i].rd, NULL, 0, NULL, &avail, NULL) && avail))
                pending = 1;
        }
        if (!pending) return;
        Sleep(5);
    }
}

#else   /* not Windows: direct writes */

FILE *cj_afile_open(const char *path, const char *mode) { return fopen(path, mode); }
int cj_afile_redirect_std(const char *path) { (void)path; return 0; }
int cj_afile_save_bmp(SDL_Surface *sf, const char *path) { int r = SDL_SaveBMP(sf, path); SDL_FreeSurface(sf); return r; }
void cj_afile_drain(int ms) { (void)ms; fflush(NULL); }

#endif
