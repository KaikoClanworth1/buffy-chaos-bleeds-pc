/**
 * Xbox DirectSound, replaced by a native mixer on Windows DirectSound.
 *
 * On the console DirectSound drives the MCPX audio chip, whose DSP does the
 * final mixing; the DSP is not emulated, so the emulated chip produced
 * silence. The game's own sound layer (xbAudio*) talks to DirectSound through
 * a small set of C functions, and those are replaced here:
 *
 *   - every buffer is a "voice": a pointer into game memory holding Xbox
 *     ADPCM (format 0x69: 36-byte blocks of 64 samples per channel) or PCM,
 *     a loop region, a frequency, volume/headroom, mix-bin volumes (2D pan)
 *     and optional 3D position against the listener;
 *   - a mixer thread decodes the voices straight from game memory (so data
 *     the engine streams into a looping buffer is heard as it arrives),
 *     resamples, mixes to 48 kHz stereo and writes ahead into a looping
 *     Windows DirectSound buffer;
 *   - GetCurrentPosition/GetStatus report the voice cursor in bytes, which the
 *     engine's streaming code relies on.
 *
 * Streams (DirectSoundCreateStream) are only used by the XMV movie player;
 * they are refused for now, so movies play without sound.
 *
 * All functions are stdcall: [esp] is the return address, arguments follow,
 * the callee pops them.
 */
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"

#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "winmm.lib")

extern uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);

#define MIX_RATE      48000
#define MAX_VOICES    256
#define OBJ_MAGIC     0x44534231u     /* 'DSB1' */

#define DSBSTATUS_PLAYING  0x00000001
#define DSBSTATUS_LOOPING  0x00000004
#define DSBPLAY_LOOPING    0x00000001
#define DSBCAPS_CTRL3D     0x00000010

typedef struct {
    int      used, playing, looping, is3d;
    uint32_t obj;                       /* guest handle */
    uint32_t data, bytes;               /* sample data in game memory */
    uint32_t tag, channels, block, rate;
    uint32_t loop_start, loop_len;      /* bytes */
    double   pos;                       /* in frames (per-channel samples) */
    uint32_t freq;
    int32_t  volume, headroom;          /* 1/100 dB */
    float    bin_l, bin_r;              /* 2D mix-bin gains */
    float    x, y, z, mind, maxd;
    /* decoded block cache */
    int32_t  cache_block;
    int16_t  cache[2][64];
} Voice;

static CRITICAL_SECTION s_lock;
static Voice   s_voice[MAX_VOICES];
static uint32_t s_device_obj;
static float   s_lx, s_ly, s_lz;                   /* listener */
static float   s_fx = 0, s_fy = 0, s_fz = 1, s_tx = 0, s_ty = 1, s_tz = 0;
static int     s_started;

static LPDIRECTSOUND8        s_ds;
static LPDIRECTSOUNDBUFFER   s_out;
static DWORD                 s_out_bytes;

static void ret_stdcall(uint32_t result, uint32_t arg_bytes)
{
    g_eax = result;
    g_esp += 4 + arg_bytes;
}

static uint32_t arg(int i)
{
    return MEM32(g_esp + 4 + 4 * (uint32_t)i);
}

static float farg(int i)
{
    uint32_t v = arg(i);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static Voice *voice_of(uint32_t obj)
{
    uint32_t idx;
    if (!obj || MEM32(obj) != OBJ_MAGIC)
        return NULL;
    idx = MEM32(obj + 4);
    return idx < MAX_VOICES && s_voice[idx].used ? &s_voice[idx] : NULL;
}

/* ── Xbox ADPCM (IMA with 64 samples per 36-byte channel block) ─────────── */

static const int s_ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767 };
static const int s_ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int16_t ima_step(int nib, int *pred, int *idx)
{
    int step = s_ima_steps[*idx];
    int diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    *pred += (nib & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += s_ima_index[nib];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

/* Decode block `b` of an ADPCM voice into v->cache (64 frames). */
static void adpcm_block(Voice *v, int32_t b)
{
    const uint8_t *p = (const uint8_t *)XBOX_PTR(v->data + (uint32_t)b * v->block);
    uint32_t ch, nch = v->channels, i, k;
    int pred[2], idx[2];

    for (ch = 0; ch < nch; ch++) {
        pred[ch] = (int16_t)(p[ch * 4] | (p[ch * 4 + 1] << 8));
        idx[ch] = p[ch * 4 + 2];
        if (idx[ch] > 88) idx[ch] = 88;
    }
    p += 4 * nch;
    /* 8 groups of 4 bytes per channel, interleaved channel by channel;
     * each 4-byte word holds 8 samples, low nibble first. */
    for (i = 0; i < 8; i++) {
        for (ch = 0; ch < nch; ch++) {
            for (k = 0; k < 4; k++) {
                uint8_t byte = p[k];
                v->cache[ch][i * 8 + k * 2]     = ima_step(byte & 0xF, &pred[ch], &idx[ch]);
                v->cache[ch][i * 8 + k * 2 + 1] = ima_step(byte >> 4, &pred[ch], &idx[ch]);
            }
            p += 4;
        }
    }
    v->cache_block = b;
}

static uint32_t voice_frames(const Voice *v, uint32_t bytes)
{
    if (v->tag == 0x69)
        return bytes / v->block * 64;
    return bytes / (v->channels * 2);
}

static uint32_t voice_bytes(const Voice *v, uint32_t frames)
{
    if (v->tag == 0x69)
        return frames / 64 * v->block;
    return frames * v->channels * 2;
}

/* One frame of the voice as float L/R in -1..1. */
static void voice_sample(Voice *v, uint32_t frame, float *l, float *r)
{
    int16_t s0, s1;
    if (v->tag == 0x69) {
        int32_t b = (int32_t)(frame / 64);
        if (b != v->cache_block)
            adpcm_block(v, b);
        s0 = v->cache[0][frame % 64];
        s1 = v->channels > 1 ? v->cache[1][frame % 64] : s0;
    } else {
        const int16_t *p = (const int16_t *)XBOX_PTR(v->data + frame * v->channels * 2);
        s0 = p[0];
        s1 = v->channels > 1 ? p[1] : s0;
    }
    *l = s0 / 32768.0f;
    *r = s1 / 32768.0f;
}

/* ── mixer ─────────────────────────────────────────────────────────────── */

static float db100_to_gain(int32_t v)
{
    if (v <= -10000)
        return 0.0f;
    return powf(10.0f, (float)v / 2000.0f);
}

static volatile LONG s_resyncs;
static uint64_t s_mixed;

static void mix(int16_t *out, int frames)
{
    static float acc[2 * 4800];
    int i, n;

    s_mixed += (uint64_t)frames;
    memset(acc, 0, sizeof(float) * 2 * frames);
    EnterCriticalSection(&s_lock);
    for (n = 0; n < MAX_VOICES; n++) {
        Voice *v = &s_voice[n];
        uint32_t total, lstart, lend;
        double step;
        float gain, gl, gr;

        if (!v->used || !v->playing || !v->data || !v->bytes)
            continue;
        total = voice_frames(v, v->bytes);
        if (!total) { v->playing = 0; continue; }
        lstart = voice_frames(v, v->loop_start);
        lend = v->loop_len ? lstart + voice_frames(v, v->loop_len) : total;
        if (lend > total) lend = total;
        if (lstart >= lend) lstart = 0;
        step = (double)(v->freq ? v->freq : v->rate) / MIX_RATE;
        gain = db100_to_gain(v->volume - v->headroom);
        gl = gain * v->bin_l;
        gr = gain * v->bin_r;
        if (v->is3d) {
            float dx = v->x - s_lx, dy = v->y - s_ly, dz = v->z - s_lz;
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            float att = 1.0f, pan;
            /* right = up x front */
            float rx = s_ty * s_fz - s_tz * s_fy, ry = s_tz * s_fx - s_tx * s_fz, rz = s_tx * s_fy - s_ty * s_fx;
            float rl = sqrtf(rx * rx + ry * ry + rz * rz);
            if (v->mind > 0 && d > v->mind)
                att = v->mind / (d < v->maxd || v->maxd <= 0 ? d : v->maxd);
            pan = (rl > 0 && d > 0.001f) ? (dx * rx + dy * ry + dz * rz) / (rl * d) : 0.0f;
            gl = gain * att * (pan > 0 ? 1.0f - pan * 0.7f : 1.0f);
            gr = gain * att * (pan < 0 ? 1.0f + pan * 0.7f : 1.0f);
        }
        for (i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)v->pos;
            float l, r;
            if (f >= lend) {
                if (v->looping) {
                    v->pos = lstart + (v->pos - lend);
                    f = (uint32_t)v->pos;
                    if (f >= lend) f = lstart;
                } else {
                    v->playing = 0;
                    break;
                }
            }
            {
                /* Linear interpolation between this frame and the next (the
                 * loop start when this is the loop's last frame): nearest-
                 * sample resampling of 22/44.1 kHz sources to 48 kHz aliased
                 * audibly. */
                float l1, r1, fr = (float)(v->pos - (double)f);
                uint32_t f1 = f + 1;
                if (f1 >= lend)
                    f1 = v->looping ? lstart : f;
                voice_sample(v, f, &l, &r);
                if (fr > 0.0001f && f1 != f) {
                    voice_sample(v, f1, &l1, &r1);
                    l += (l1 - l) * fr;
                    r += (r1 - r) * fr;
                }
            }
            acc[i * 2] += l * gl;
            acc[i * 2 + 1] += r * gr;
            v->pos += step;
        }
    }
    LeaveCriticalSection(&s_lock);
    for (i = 0; i < frames * 2; i++) {
        /* Soft knee above 0.8 instead of a hard clip: loud moments with many
         * voices crackled when the sum was simply cut at full scale. */
        float s = acc[i], a = s < 0 ? -s : s;
        if (a > 0.8f) {
            a = 0.8f + 0.2f * tanhf((a - 0.8f) / 0.2f);
            s = s < 0 ? -a : a;
        }
        out[i] = (int16_t)(s * 32767.0f);
    }

    /* Diagnostics: BUFFY_AUDIO_STATS prints active voices and peak every 5 s;
     * BUFFY_AUDIO_WAV=<file> captures the first 60 s of the mix. */
    {
        static int inited, stats;
        static FILE *wav;
        static uint32_t wav_bytes;
        static int peak;
        static DWORD last;
        if (!inited) {
            const char *w = getenv("BUFFY_AUDIO_WAV");
            inited = 1;
            stats = getenv("BUFFY_AUDIO_STATS") != NULL;
            if (w && (wav = fopen(w, "wb")) != NULL) {
                uint32_t hdr[11] = { 0x46464952, 0, 0x45564157, 0x20746D66, 16,
                                     0x00020001, MIX_RATE, MIX_RATE * 4, 0x00100004,
                                     0x61746164, 0 };
                fwrite(hdr, 4, 11, wav);
            }
        }
        if (wav && wav_bytes < MIX_RATE * 4 * 60) {
            fwrite(out, 4, (size_t)frames, wav);
            wav_bytes += (uint32_t)frames * 4;
            if (wav_bytes >= MIX_RATE * 4 * 60) {
                uint32_t v = wav_bytes + 36;
                fseek(wav, 4, SEEK_SET); fwrite(&v, 4, 1, wav);
                fseek(wav, 40, SEEK_SET); fwrite(&wav_bytes, 4, 1, wav);
                fclose(wav);
                wav = NULL;
            }
        }
        if (stats) {
            for (i = 0; i < frames * 2; i++) {
                int a = out[i] < 0 ? -out[i] : out[i];
                if (a > peak) peak = a;
            }
            if (GetTickCount() - last > 5000) {
                int active = 0, used = 0;
                last = GetTickCount();
                for (n = 0; n < MAX_VOICES; n++) {
                    used += s_voice[n].used;
                    active += s_voice[n].used && s_voice[n].playing;
                }
                fprintf(stderr, "  [DSOUND] voices %d (%d playing), peak %d, resyncs %ld, mixed %.1f s\n", used, active, peak,
                        s_resyncs, s_mixed / (double)MIX_RATE);
                peak = 0;
            }
        }
    }
}

static DWORD WINAPI mixer_thread(LPVOID unused)
{
    /* Written relative to DirectSound's own write cursor (the first byte that
     * is safe to write), keeping ~25 ms queued beyond it. The old loop kept
     * its own write offset only: after any stall the play cursor passed it,
     * the gap read as "far ahead", stale audio looped, and from then on every
     * sound came out up to a whole buffer late. Now a stall resyncs at once. */
    DWORD write = 0;
    const DWORD chunk = MIX_RATE / 200 * 4;           /* 5 ms of 16-bit stereo */
    const DWORD lead = MIX_RATE / 40 * 4;             /* 25 ms beyond the cursor */
    static int16_t buf[4800 * 2];
    int primed = 0;
    (void)unused;

    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        DWORD play = 0, wcur = 0, to_wcur, to_write;
        if (FAILED(IDirectSoundBuffer_GetCurrentPosition(s_out, &play, &wcur))) {
            Sleep(5);
            continue;
        }
        to_wcur = (wcur + s_out_bytes - play) % s_out_bytes;
        to_write = (write + s_out_bytes - play) % s_out_bytes;
        /* Behind the write cursor, or "far ahead" -- which really means the
         * play cursor lapped us -- after a stall: restart at the cursor. */
        if (!primed || to_write < to_wcur || to_write > s_out_bytes * 3 / 4) {
            if (primed)
                s_resyncs++;
            write = wcur;
            to_write = to_wcur;
            primed = 1;
        }
        while (to_write < to_wcur + lead) {
            void *p1, *p2;
            DWORD b1, b2;
            mix(buf, (int)(chunk / 4));
            if (SUCCEEDED(IDirectSoundBuffer_Lock(s_out, write, chunk, &p1, &b1, &p2, &b2, 0))) {
                memcpy(p1, buf, b1);
                if (p2)
                    memcpy(p2, (uint8_t *)buf + b1, b2);
                IDirectSoundBuffer_Unlock(s_out, p1, b1, p2, b2);
            }
            write = (write + chunk) % s_out_bytes;
            to_write += chunk;
        }
        Sleep(2);
    }
    return 0;
}

static void audio_start(void)
{
    DSBUFFERDESC d;
    WAVEFORMATEX wf;
    HRESULT hr;

    if (s_started)
        return;
    s_started = 1;
    InitializeCriticalSection(&s_lock);
    hr = DirectSoundCreate8(NULL, &s_ds, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "  [DSOUND] no output device (0x%08lX); game runs silent\n", hr);
        return;
    }
    IDirectSound8_SetCooperativeLevel(s_ds, GetDesktopWindow(), DSSCL_PRIORITY);
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = MIX_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = MIX_RATE * 4;
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLVOLUME;
    s_out_bytes = MIX_RATE * 4 / 5;                     /* 200 ms ring */
    d.dwBufferBytes = s_out_bytes;
    d.lpwfxFormat = &wf;
    hr = IDirectSound8_CreateSoundBuffer(s_ds, &d, &s_out, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "  [DSOUND] output buffer failed (0x%08lX)\n", hr);
        return;
    }
    if (getenv("BUFFY_MUTE") && getenv("BUFFY_MUTE")[0] == '1')                         /* mixes as usual, plays silent */
        IDirectSoundBuffer_SetVolume(s_out, DSBVOLUME_MIN);
    IDirectSoundBuffer_Play(s_out, 0, 0, DSBPLAY_LOOPING);
    CloseHandle(CreateThread(NULL, 0, mixer_thread, NULL, 0, NULL));
    fprintf(stderr, "  [DSOUND] Windows DirectSound output: 48 kHz stereo\n");
}

static uint32_t new_object(uint32_t index)
{
    uint32_t obj = xbox_ContiguousAlloc(64, 16);
    if (obj) {
        MEM32(obj) = OBJ_MAGIC;
        MEM32(obj + 4) = index;
    }
    return obj;
}

/* ── device ───────────────────────────────────────────────────────────── */

/* HRESULT DirectSoundCreate(LPGUID, LPDIRECTSOUND *, LPUNKNOWN) */
void DirectSoundCreate_0014BCD4(void)
{
    uint32_t out = arg(1);
    audio_start();
    if (!s_device_obj)
        s_device_obj = new_object(0xFFFFFFFFu);
    if (out)
        MEM32(out) = s_device_obj;
    ret_stdcall(0, 12);
}

/* HRESULT DirectSoundCreateStream(LPCDSSTREAMDESC, LPDIRECTSOUNDSTREAM *):
 * refused, movies play silent (see the header). */
void DirectSoundCreateStream_0014BD1B(void)
{
    uint32_t out = arg(1);
    if (out)
        MEM32(out) = 0;
    ret_stdcall(0x80004005u, 8);
}

void DirectSoundDoWork_0014A3A6(void)             { ret_stdcall(0, 0); }
void DirectSoundUseFullHRTF_00149385(void)        { ret_stdcall(0, 0); }
void IDirectSound_Release_00149359(void)          { ret_stdcall(0, 4); }
void IDirectSound_CommitDeferredSettings_0014B783(void) { ret_stdcall(0, 4); }
void IDirectSound_SynchPlayback_0014A264(void)    { ret_stdcall(0, 4); }

/* HRESULT DownloadEffectsImage(pDS, pvImage, size, pImageLoc, ppImageDesc):
 * no DSP, so no effects; hand back an empty descriptor. */
void IDirectSound_DownloadEffectsImage_0014A23D(void)
{
    uint32_t pp = arg(4);
    if (pp) {
        uint32_t desc = xbox_ContiguousAlloc(256, 16);
        MEM32(pp) = desc;
    }
    ret_stdcall(0, 20);
}

int buffy_coop_pass2_active(void);
static volatile long s_p2calls[8];            /* (testing) calls made during player 2's pass */

void buffy_dsound_p2_report(void)
{
    fprintf(stderr, "[AUDIO] in player 2's pass: listener pos %ld orient %ld play %ld stop %ld setpos %ld\n",
            s_p2calls[0], s_p2calls[1], s_p2calls[2], s_p2calls[3], s_p2calls[4]);
}

void IDirectSound_SetPosition_0014BAC4(void)
{
    if (buffy_coop_pass2_active()) {
        s_p2calls[0]++;
        ret_stdcall(0, 20);
        return;
    }
    s_lx = farg(1); s_ly = farg(2); s_lz = farg(3);
    ret_stdcall(0, 20);
}

void IDirectSound_SetVelocity_0014BAF9(void)      { ret_stdcall(0, 20); }

void IDirectSound_SetOrientation_0014BA7A(void)
{
    if (buffy_coop_pass2_active()) {
        s_p2calls[1]++;
        ret_stdcall(0, 32);
        return;
    }
    s_fx = farg(1); s_fy = farg(2); s_fz = farg(3);
    s_tx = farg(4); s_ty = farg(5); s_tz = farg(6);
    ret_stdcall(0, 32);
}

/* HRESULT CreateSoundBuffer(pDS, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, pUnk) */
void IDirectSound_CreateSoundBuffer_0014BA56(void)
{
    uint32_t desc = arg(1), out = arg(2), wfx;
    int i;
    Voice *v = NULL;

    audio_start();
    EnterCriticalSection(&s_lock);
    for (i = 0; i < MAX_VOICES; i++)
        if (!s_voice[i].used) {
            v = &s_voice[i];
            break;
        }
    if (!v) {
        LeaveCriticalSection(&s_lock);
        if (out) MEM32(out) = 0;
        ret_stdcall(0x8007000Eu, 16);                 /* E_OUTOFMEMORY */
        return;
    }
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->cache_block = -1;
    v->bin_l = v->bin_r = 1.0f;
    v->mind = 1.0f;
    v->maxd = 1000000.0f;
    v->is3d = (MEM32(desc + 4) & DSBCAPS_CTRL3D) != 0;
    wfx = MEM32(desc + 12);
    if (wfx) {
        v->tag = MEM16(wfx);
        v->channels = MEM16(wfx + 2) ? MEM16(wfx + 2) : 1;
        v->rate = MEM32(wfx + 4);
        v->block = MEM16(wfx + 12) ? MEM16(wfx + 12) : 36 * v->channels;
    } else {
        v->tag = 1; v->channels = 1; v->rate = 44100; v->block = 2;
    }
    if (v->channels > 2) v->channels = 2;
    v->freq = v->rate;
    v->obj = new_object((uint32_t)i);
    LeaveCriticalSection(&s_lock);
    if (out)
        MEM32(out) = v->obj;
    ret_stdcall(v->obj ? 0 : 0x8007000Eu, 16);
}

/* ── buffers ──────────────────────────────────────────────────────────── */

void IDirectSoundBuffer_SetBufferData_0014B2AC(void)
{
    Voice *v = voice_of(arg(0));
    if (v) {
        EnterCriticalSection(&s_lock);
        v->data = arg(1);
        v->bytes = arg(2);
        v->pos = 0;
        v->cache_block = -1;
        v->loop_start = v->loop_len = 0;
        if (!v->data || !v->bytes)
            v->playing = 0;
        LeaveCriticalSection(&s_lock);
    }
    ret_stdcall(0, 12);
}

void IDirectSoundBuffer_Play_0014A2D0(void)
{
    Voice *v = voice_of(arg(0));
    if (buffy_coop_pass2_active())
        s_p2calls[2]++;
    if (v) {
        EnterCriticalSection(&s_lock);
        v->looping = (arg(3) & DSBPLAY_LOOPING) != 0;
        v->playing = v->data && v->bytes;
        LeaveCriticalSection(&s_lock);
    }
    ret_stdcall(0, 16);
}

void IDirectSoundBuffer_Stop_0014A2F4(void)
{
    if (buffy_coop_pass2_active())
        s_p2calls[3]++;
    Voice *v = voice_of(arg(0));
    if (v) v->playing = 0;
    ret_stdcall(0, 4);
}

void IDirectSoundBuffer_SetLoopRegion_0014A30C(void)
{
    Voice *v = voice_of(arg(0));
    if (v) {
        EnterCriticalSection(&s_lock);
        v->loop_start = arg(1);
        v->loop_len = arg(2);
        LeaveCriticalSection(&s_lock);
    }
    ret_stdcall(0, 12);
}

void IDirectSoundBuffer_SetFrequency_0014AC3E(void)
{
    Voice *v = voice_of(arg(0));
    if (v) v->freq = arg(1) ? arg(1) : v->rate;
    ret_stdcall(0, 8);
}

void IDirectSoundBuffer_SetVolume_0014A27C(void)
{
    Voice *v = voice_of(arg(0));
    if (v) v->volume = (int32_t)arg(1);
    ret_stdcall(0, 8);
}

void IDirectSoundBuffer_SetHeadroom_0014A298(void)
{
    Voice *v = voice_of(arg(0));
    if (v) v->headroom = (int32_t)arg(1);
    ret_stdcall(0, 8);
}

/* DSMIXBINS { DWORD count; DSMIXBINVOLUMEPAIR *pairs } with pairs
 * { DWORD bin; LONG volume }. Bins 0/4 feed the left speaker, 1/5 the right,
 * 2 (centre) both. */
static void apply_mixbins(Voice *v, uint32_t mb, int volumes)
{
    uint32_t n, pairs, i;
    float l = 0, r = 0;
    int any = 0;
    if (!v || !mb)
        return;
    n = MEM32(mb);
    pairs = MEM32(mb + 4);
    if (!pairs || n > 32)
        return;
    for (i = 0; i < n; i++) {
        uint32_t bin = MEM32(pairs + i * 8);
        float g = volumes ? db100_to_gain((int32_t)MEM32(pairs + i * 8 + 4)) : 1.0f;
        if (bin == 0 || bin == 4) { l = l > g ? l : g; any = 1; }
        else if (bin == 1 || bin == 5) { r = r > g ? r : g; any = 1; }
        else if (bin == 2) { l = l > g ? l : g; r = r > g ? r : g; any = 1; }
    }
    if (any) {
        v->bin_l = l;
        v->bin_r = r;
    }
}

void IDirectSoundBuffer_SetMixBins_0014AC5A(void)
{
    apply_mixbins(voice_of(arg(0)), arg(1), 0);
    ret_stdcall(0, 8);
}

void IDirectSoundBuffer_SetMixBinVolumes_0014A2B4(void)
{
    apply_mixbins(voice_of(arg(0)), arg(1), 1);
    ret_stdcall(0, 8);
}

void IDirectSoundBuffer_SetPosition_0014ACBE(void)
{
    Voice *v = voice_of(arg(0));
    if (v) { v->x = farg(1); v->y = farg(2); v->z = farg(3); v->is3d = 1; }
    ret_stdcall(0, 20);
}

void IDirectSoundBuffer_SetMinDistance_0014AC9A(void)
{
    Voice *v = voice_of(arg(0));
    if (v) v->mind = farg(1);
    ret_stdcall(0, 12);
}

void IDirectSoundBuffer_SetMaxDistance_0014AC76(void)
{
    Voice *v = voice_of(arg(0));
    if (v) v->maxd = farg(1);
    ret_stdcall(0, 12);
}

void IDirectSoundBuffer_SetVelocity_0014ACF3(void)     { ret_stdcall(0, 20); }
void IDirectSoundBuffer_SetRolloffCurve_0014AD28(void) { ret_stdcall(0, 16); }
void IDirectSoundBuffer_SetI3DL2Source_0014AD4C(void)  { ret_stdcall(0, 12); }

void IDirectSoundBuffer_GetStatus_0014A32C(void)
{
    Voice *v = voice_of(arg(0));
    uint32_t out = arg(1);
    if (out)
        MEM32(out) = v && v->playing
                   ? (DSBSTATUS_PLAYING | (v->looping ? DSBSTATUS_LOOPING : 0)) : 0;
    ret_stdcall(0, 8);
}

void IDirectSoundBuffer_GetCurrentPosition_0014A348(void)
{
    Voice *v = voice_of(arg(0));
    uint32_t play = arg(1), write = arg(2), bytes = 0;
    if (v) {
        EnterCriticalSection(&s_lock);
        bytes = voice_bytes(v, (uint32_t)v->pos);
        LeaveCriticalSection(&s_lock);
    }
    if (play) MEM32(play) = bytes;
    if (write) MEM32(write) = bytes;
    ret_stdcall(0, 12);
}

void IDirectSoundBuffer_SetCurrentPosition_0014A368(void)
{
    Voice *v = voice_of(arg(0));
    if (v) {
        EnterCriticalSection(&s_lock);
        v->pos = voice_frames(v, arg(1));
        LeaveCriticalSection(&s_lock);
    }
    ret_stdcall(0, 8);
}

/* Stream methods: only reached if a stream exists, and none are created. */
void IDirectSoundStream_FlushEx_0014A38E(void)   { ret_stdcall(0, 16); }
