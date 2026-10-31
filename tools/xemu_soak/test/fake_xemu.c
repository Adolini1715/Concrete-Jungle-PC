/* fake_xemu: a stand-in for xemu.exe to test xemu_soak under Wine.
 * - QMP server on 127.0.0.1:4444 (from "-qmp tcp:127.0.0.1:4444,...") that
 *   answers human-monitor-command "x /Nwx VA" from a guest memory snapshot
 *   (guestmem.bin next to the exe; VA & 0x0FFFFFFF), "info version", "info status"
 * - a window titled "xemu | fake" that animates (frame counter bars)
 * - a 440 Hz tone through waveOut, on for 2 s and off for 1 s
 * Exits after the number of seconds in FAKE_XEMU_SECS (default 40). */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint8_t *s_mem;
static size_t s_memn;
static volatile LONG s_frame;

static void sendj(SOCKET c, const char *s) { send(c, s, (int)strlen(s), 0); }

static DWORD WINAPI qmp_thread(LPVOID a)
{
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0), c;
    struct sockaddr_in sa;
    char buf[65536], out[300000];
    int n = 0, one = 1;
    (void)a;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(4444); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(l, (struct sockaddr *)&sa, sizeof sa) || listen(l, 1)) return 0;
    c = accept(l, NULL, NULL);
    sendj(c, "{\"QMP\": {\"version\": {\"qemu\": {\"micro\": 0, \"minor\": 2, \"major\": 8}, \"package\": \"fake\"}, \"capabilities\": []}}\r\n");
    for (;;) {
        int r = recv(c, buf + n, sizeof buf - 1 - n, 0);
        char *nl;
        if (r <= 0) break;
        n += r; buf[n] = 0;
        while ((nl = strchr(buf, '\n'))) {
            *nl = 0;
            if (strstr(buf, "qmp_capabilities")) sendj(c, "{\"return\": {}}\r\n");
            else {
                char *cl = strstr(buf, "command-line\":\"");
                unsigned cnt, va;
                if (cl && sscanf(cl + 15, "x /%uwx 0x%x", &cnt, &va) == 2) {
                    char *o = out;
                    unsigned i;
                    o += sprintf(o, "{\"return\": \"");
                    for (i = 0; i < cnt; i++) {
                        uint32_t addr = (va + 4 * i) & 0x0FFFFFFF, v = 0;
                        if (addr + 4 <= s_memn) memcpy(&v, s_mem + addr, 4);
                        if (i % 4 == 0) o += sprintf(o, "%s%08x:", i ? "\\r\\n" : "", va + 4 * i);
                        o += sprintf(o, " 0x%08x", v);
                    }
                    o += sprintf(o, "\\r\\n\"}\r\n");
                    sendj(c, out);
                } else if (cl && strstr(cl, "info version")) sendj(c, "{\"return\": \"8.2.0 (fake xemu)\\r\\n\"}\r\n");
                else if (cl && strstr(cl, "info status")) sendj(c, "{\"return\": \"VM status: running\\r\\n\"}\r\n");
                else sendj(c, "{\"return\": \"\"}\r\n");
            }
            memmove(buf, nl + 1, strlen(nl + 1) + 1);
            n = (int)strlen(buf);
        }
    }
    return 0;
}

static DWORD WINAPI audio_thread(LPVOID a)
{
    HWAVEOUT wo;
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
    static int16_t pcm[2][4800 * 2];
    WAVEHDR h[2];
    int k = 0;
    unsigned long long t = 0;
    (void)a;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL)) return 0;
    memset(h, 0, sizeof h);
    for (;;) {
        WAVEHDR *hd = &h[k];
        int i;
        while (hd->dwFlags & WHDR_PREPARED && !(hd->dwFlags & WHDR_DONE)) Sleep(5);
        if (hd->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, hd, sizeof *hd);
        for (i = 0; i < 4800; i++, t++) {
            int on = (t / 48000) % 3 != 2;
            int16_t v = on ? (int16_t)(8000 * sin(2 * 3.14159265 * 440 * t / 48000.0)) : 0;
            pcm[k][2 * i] = pcm[k][2 * i + 1] = v;
        }
        hd->lpData = (LPSTR)pcm[k]; hd->dwBufferLength = sizeof pcm[k]; hd->dwFlags = 0;
        waveOutPrepareHeader(wo, hd, sizeof *hd);
        waveOutWrite(wo, hd, sizeof *hd);
        k ^= 1;
    }
    return 0;
}

static LRESULT CALLBACK wp(HWND w, UINT m, WPARAM a, LPARAM b)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        int f = s_frame, i;
        GetClientRect(w, &r);
        for (i = 0; i < 16; i++) {
            HBRUSH br = CreateSolidBrush(RGB((i * 16 + f * 5) & 255, (f * 3) & 255, 255 - i * 12));
            RECT q = {r.right * i / 16, 0, r.right * (i + 1) / 16, r.bottom};
            FillRect(dc, &q, br);
            DeleteObject(br);
        }
        {
            char t[64];
            snprintf(t, sizeof t, "fake xemu frame %d", f);
            TextOutA(dc, 20, 20, t, (int)strlen(t));
        }
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(w, m, a, b);
}

int main(int argc, char **argv)
{
    WSADATA wd;
    char path[MAX_PATH], *sl;
    FILE *f;
    WNDCLASSA wc = {0};
    HWND w;
    DWORD t0 = GetTickCount(), secs = getenv("FAKE_XEMU_SECS") ? (DWORD)atoi(getenv("FAKE_XEMU_SECS")) : 40;
    (void)argc; (void)argv;
    WSAStartup(MAKEWORD(2, 2), &wd);
    GetModuleFileNameA(NULL, path, sizeof path);
    if ((sl = strrchr(path, '\\'))) strcpy(sl + 1, "guestmem.bin");
    if ((f = fopen(path, "rb"))) {
        fseek(f, 0, SEEK_END); s_memn = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        s_mem = malloc(s_memn); fread(s_mem, 1, s_memn, f); fclose(f);
    }
    CreateThread(NULL, 0, qmp_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    wc.lpfnWndProc = wp; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "fakexemu";
    RegisterClassA(&wc);
    w = CreateWindowA("fakexemu", "xemu | fake", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    while (GetTickCount() - t0 < secs * 1000) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        InterlockedIncrement(&s_frame);
        InvalidateRect(w, NULL, FALSE);
        Sleep(16);
    }
    return 0;
}
