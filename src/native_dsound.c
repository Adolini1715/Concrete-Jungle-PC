/*
 * Native output for the game's software mixer (CJ_DSOUND, default on outside CJ_DET).
 *
 * EngineX mixes every sound effect, voice and music stream itself (AudioMixer::Mix, the
 * xbMixerThread at 0x2B2520) into five 16-bit mono rings of 4096 samples at 44.1 kHz, one per
 * speaker (0x4D4328 + ch * 0x2000): front left, front right, back left, back right, centre.
 * XBMixer_Five_One::Init (0x2B2690) hands each ring to DirectSound as a looping buffer and the
 * mixer thread mixes ahead of buffer 0's play cursor (IDirectSoundBuffer_GetCurrentPosition).
 * On the PC that went through the emulated MCPX APU: register traffic trapped as access faults
 * (~1,700/s), voice processing, a resampler to 48 kHz.
 *
 * Here the five buffers never reach DirectSound. CreateSpeakerChannel (0x2B23E0) stores a
 * stand-in handle (HANDLE_BASE + ch) and the four DirectSound calls the mixer makes with it
 * (Play, GetCurrentPosition, Stop, Release) are answered natively; any other handle goes to
 * the original code. The rings are read at the play cursor, resampled 44.1 -> 48 kHz, downmixed
 * 5.0 to stereo and added into the APU's output periods (g_apu_output_hook, xboxrecomp patch
 * 0024), so there is still one audio device; GetCurrentPosition reports that cursor. Movies
 * (XMV, their own DirectSound stream) still play through the APU.
 *
 *   CJ_DSOUND=0         the original DirectSound / APU path for the mixer too
 *   CJ_DSOUND_WAV=file  also write what the game mixer plays (stereo 16-bit) to a WAV file
 *   CJ_AUDIO_51=1       own six-channel device (FL FR C LFE BL BR) instead of the APU output
 *
 * Off in CJ_DET (recordings): the cursor follows the host audio clock.
 * Part of the recompiler's --exclude-manual input (tools/regen_all.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "recomp_types.h"

extern void sub_002B23E0_gen(void);   /* XBMixer_Five_One::CreateSpeakerChannel(int, bool, int) */
extern void sub_00332960_gen(void);   /* IDirectSoundBuffer_Play(buf, r1, r2, flags) */
extern void sub_00332A60_gen(void);   /* IDirectSoundBuffer_GetCurrentPosition(buf, &play, &write) */
extern void sub_003329A8_gen(void);   /* IDirectSoundBuffer_Stop(buf) */
extern void sub_003304D4_gen(void);   /* IDirectSoundBuffer_Release(buf) */
extern void sub_002B2210(void);       /* xbAudioMixBin (cl, dl, 3 stack bytes), cdecl */
extern int cj_det_enabled(void);

#define RING_VA        0x004D4328u    /* 5 x 0x2000 bytes */
#define RING_BYTES     0x2000u
#define RING_SAMPLES   0x1000u
#define HANDLES_VA     0x004DE330u    /* the five buffer handles */
#define HANDLE_BASE    0x00000D50u    /* stand-in handles: never dereferenced by the game */
#define NCH            5
#define RATE           44100
#define RET_VA         0x0000C0DEu

static int s_on = -1;
static SDL_AudioDeviceID s_dev;
static int s_out_ch = 2;
static double s_frac;                        /* resampler position between samples */
static int s_wav_rate = RATE;
static volatile int s_playing;
static volatile uint32_t s_cursor;           /* samples, mod RING_SAMPLES */
static volatile uint64_t s_consumed;
static float s_vol[NCH] = {1, 1, 1, 1, 1};
static FILE *s_wav;
static uint32_t s_wav_bytes;
static unsigned s_created;

static int is_ours(uint32_t h) { return s_on > 0 && h >= HANDLE_BASE && h < HANDLE_BASE + NCH; }

static int hle_on(void)
{
    if (s_on < 0) {
        const char *e = getenv("CJ_DSOUND");
        s_on = !(e && *e == '0') && !cj_det_enabled();
        if (s_on) fprintf(stderr, "[DSOUND] native output for the game mixer (CJ_DSOUND=0: the APU path)\n");
    }
    return s_on;
}

static void wav_header(FILE *f, uint32_t data)
{
    uint8_t h[44];
    uint32_t v;
    memcpy(h, "RIFF", 4); v = 36 + data; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
    h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    v = (uint32_t)s_wav_rate; memcpy(h + 24, &v, 4); v = (uint32_t)s_wav_rate * 4; memcpy(h + 28, &v, 4);
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 44, 1, f);
    fseek(f, 0, SEEK_END);
}

static float clampf(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

/* SDL audio thread: n frames from the rings at the cursor. */
static void SDLCALL callback(void *ud, Uint8 *out_b, int len)
{
    float *out = (float *)out_b;
    int frames = len / (int)(sizeof(float) * (unsigned)s_out_ch), i;
    const int16_t *ring = (const int16_t *)XBOX_PTR(RING_VA);
    int16_t wbuf[2 * 1024];
    int wn = 0;
    uint32_t cur = s_cursor;
    (void)ud;
    for (i = 0; i < frames; i++) {
        float fl = 0, fr = 0, bl = 0, br = 0, c = 0;
        if (s_playing) {
            fl = ring[0 * RING_SAMPLES + cur] * (1.0f / 32768.0f) * s_vol[0];
            fr = ring[1 * RING_SAMPLES + cur] * (1.0f / 32768.0f) * s_vol[1];
            bl = ring[2 * RING_SAMPLES + cur] * (1.0f / 32768.0f) * s_vol[2];
            br = ring[3 * RING_SAMPLES + cur] * (1.0f / 32768.0f) * s_vol[3];
            c  = ring[4 * RING_SAMPLES + cur] * (1.0f / 32768.0f) * s_vol[4];
            cur = (cur + 1) & (RING_SAMPLES - 1);
        }
        if (s_out_ch == 6) {
            float *o = out + i * 6;
            o[0] = fl; o[1] = fr; o[2] = c; o[3] = 0.0f; o[4] = bl; o[5] = br;
        } else {
            /* ITU downmix: centre and surrounds at -3 dB */
            float l = clampf(fl + 0.7071f * (c + bl)), r = clampf(fr + 0.7071f * (c + br));
            out[i * 2] = l;
            out[i * 2 + 1] = r;
            if (s_wav) {
                wbuf[wn++] = (int16_t)(l * 32767.0f);
                wbuf[wn++] = (int16_t)(r * 32767.0f);
                if (wn == 2 * 1024) { fwrite(wbuf, 2, (size_t)wn, s_wav); s_wav_bytes += (uint32_t)wn * 2; wn = 0; }
            }
        }
    }
    if (s_wav && wn) { fwrite(wbuf, 2, (size_t)wn, s_wav); s_wav_bytes += (uint32_t)wn * 2; }
    if (s_playing) { s_cursor = cur; s_consumed += (uint64_t)frames; }
}

/* APU output thread: add the game mixer into one output period (stereo s16 at `rate`). */
static void apu_hook(int16_t *io, int frames, int rate)
{
    const int16_t *ring = (const int16_t *)XBOX_PTR(RING_VA);
    double step = (double)RATE / (double)rate;
    uint32_t cur = s_cursor;
    int16_t wbuf[2 * 512];
    int i, wn = 0;
    if (!s_playing) return;
    for (i = 0; i < frames; i++) {
        uint32_t a = cur, b = (cur + 1) & (RING_SAMPLES - 1);
        float t = (float)s_frac, ch[NCH], l, r;
        int k;
        for (k = 0; k < NCH; k++) {
            float x0 = ring[k * RING_SAMPLES + a], x1 = ring[k * RING_SAMPLES + b];
            ch[k] = (x0 + (x1 - x0) * t) * (1.0f / 32768.0f) * s_vol[k];
        }
        l = clampf(ch[0] + 0.7071f * (ch[4] + ch[2]));
        r = clampf(ch[1] + 0.7071f * (ch[4] + ch[3]));
        {
            int vl = io[i * 2] + (int)(l * 32767.0f), vr = io[i * 2 + 1] + (int)(r * 32767.0f);
            io[i * 2] = (int16_t)(vl > 32767 ? 32767 : vl < -32768 ? -32768 : vl);
            io[i * 2 + 1] = (int16_t)(vr > 32767 ? 32767 : vr < -32768 ? -32768 : vr);
        }
        if (s_wav && wn < 2 * 512) { wbuf[wn++] = (int16_t)(l * 32767.0f); wbuf[wn++] = (int16_t)(r * 32767.0f); }
        s_frac += step;
        while (s_frac >= 1.0) { s_frac -= 1.0; cur = (cur + 1) & (RING_SAMPLES - 1); s_consumed++; }
    }
    s_cursor = cur;
    if (s_wav && wn) { fwrite(wbuf, 2, (size_t)wn, s_wav); s_wav_bytes += (uint32_t)wn * 2; }
}

static void wav_close(void)
{
    if (!s_wav) return;
    if (s_dev) SDL_LockAudioDevice(s_dev);
    wav_header(s_wav, s_wav_bytes);
    fclose(s_wav);
    s_wav = NULL;
    if (s_dev) SDL_UnlockAudioDevice(s_dev);
}

extern void (*volatile g_apu_output_hook)(int16_t *stereo, int frames, int rate);
static void open_device(void)
{
    SDL_AudioSpec want, have;
    const char *e = getenv("CJ_AUDIO_51"), *w = getenv("CJ_DSOUND_WAV");
    static int done;
    if (s_dev || done) return;
    done = 1;
    if (!(e && *e == '1')) {   /* default: mix into the APU's output (one device) */
        s_wav_rate = 48000;
        if (w && *w && (s_wav = fopen(w, "wb"))) { wav_header(s_wav, 0); atexit(wav_close); }
        g_apu_output_hook = apu_hook;
        fprintf(stderr, "[DSOUND] game mixer played through the APU output (48 kHz stereo)\n");
        return;
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[DSOUND] audio init failed: %s\n", SDL_GetError());
        return;
    }
    memset(&want, 0, sizeof want);
    want.freq = RATE;
    want.format = AUDIO_F32SYS;
    want.channels = (Uint8)((e && *e == '1') ? 6 : 2);
    want.samples = 512;
    want.callback = callback;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!s_dev && want.channels == 6) {
        want.channels = 2;
        s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    }
    if (!s_dev) { fprintf(stderr, "[DSOUND] cannot open the audio device: %s\n", SDL_GetError()); return; }
    s_out_ch = have.channels;
    if (w && *w && s_out_ch == 2 && (s_wav = fopen(w, "wb"))) { wav_header(s_wav, 0); atexit(wav_close); }
    fprintf(stderr, "[DSOUND] output %d Hz, %d channels, %d-sample buffer (%.1f ms)\n", have.freq, have.channels,
            have.samples, 1000.0 * have.samples / have.freq);
    SDL_PauseAudioDevice(s_dev, 0);
}

/* XBMixer_Five_One::CreateSpeakerChannel(int channels, bool adpcm, int ch), __thiscall, ret 0xc */
void sub_002B23E0(void)
{
    uint32_t nchan = MEM32(g_esp + 4), adpcm = MEM32(g_esp + 8) & 0xff, ch = MEM32(g_esp + 12);
    if (!hle_on() || nchan != 1 || adpcm || ch >= NCH) {
        if (s_on > 0 && s_created == 0) { s_on = 0; fprintf(stderr, "[DSOUND] unexpected mixer format, using the APU path\n"); }
        sub_002B23E0_gen();
        return;
    }
    memset(XBOX_PTR(RING_VA + ch * RING_BYTES), 0, RING_BYTES);
    {   /* the mix-bin table the original leaves behind (xbAudioDSS_SetMixBins reads it) */
        uint32_t esp0 = g_esp, ecx0 = g_ecx;
        PUSH32(g_esp, ch == 4);
        PUSH32(g_esp, ch == 3);
        PUSH32(g_esp, ch == 2);
        PUSH32(g_esp, RET_VA);
        g_ecx = ch == 0;
        g_edx = ch == 1;
        sub_002B2210();
        g_esp = esp0;
        g_ecx = ecx0;
    }
    MEM32(HANDLES_VA + ch * 4) = HANDLE_BASE + ch;
    s_vol[ch] = 1.0f;
    s_created |= 1u << ch;
    open_device();
    g_esp += 4 + 12;
}

/* IDirectSoundBuffer_Play(buf, reserved1, reserved2, flags), stdcall */
void sub_00332960(void)
{
    uint32_t h = MEM32(g_esp + 4);
    if (!is_ours(h)) { sub_00332960_gen(); return; }
    if (h == HANDLE_BASE) {
        if (s_dev) SDL_LockAudioDevice(s_dev);
        s_cursor = 0;
        s_frac = 0.0;
        s_playing = 1;
        if (s_dev) SDL_UnlockAudioDevice(s_dev);
    }
    g_eax = 0;
    g_esp += 4 + 16;
}

/* IDirectSoundBuffer_GetCurrentPosition(buf, &play, &write), stdcall: bytes */
void sub_00332A60(void)
{
    uint32_t h = MEM32(g_esp + 4), pp = MEM32(g_esp + 8), pw = MEM32(g_esp + 12);
    if (!is_ours(h)) { sub_00332A60_gen(); return; }
    {
        uint32_t c = s_cursor * 2u;
        if (pp) MEM32(pp) = c;
        if (pw) MEM32(pw) = (c + 512u * 2u) & (RING_BYTES - 1);
    }
    g_eax = 0;
    g_esp += 4 + 12;
}

/* IDirectSoundBuffer_Stop(buf), stdcall */
void sub_003329A8(void)
{
    uint32_t h = MEM32(g_esp + 4);
    if (!is_ours(h)) { sub_003329A8_gen(); return; }
    if (h == HANDLE_BASE) s_playing = 0;
    g_eax = 0;
    g_esp += 4 + 4;
}

/* IDirectSoundBuffer_Release(buf), stdcall: remaining references */
void sub_003304D4(void)
{
    uint32_t h = MEM32(g_esp + 4);
    if (!is_ours(h)) { sub_003304D4_gen(); return; }
    s_created &= ~(1u << (h - HANDLE_BASE));
    g_eax = 0;
    g_esp += 4 + 4;
}
