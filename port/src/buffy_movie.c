/**
 * Movies: the XDK's XMV decoder API, answered with Windows Media Foundation.
 *
 * The engine plays a movie through xbMovieOpen / xbMovieBlit / xbMovieClose:
 * it creates an XMV decoder for the file, allocates two YUY2 textures, and
 * every frame asks XMVDecoder_GetNextFrame to fill the back one, drawing
 * whichever is current as an ordinary textured quad. Everything above the
 * decoder -- textures, drawing, timing, the skip button -- is left to the
 * game. Only the decoder is replaced:
 *
 *   - the .xmv files (WMV2 video, Xbox ADPCM audio) are converted once, at
 *     deploy time, to Movies\<name>.mp4 (H.264 + AAC) beside the exe
 *     (tools/deploy.sh, with ffmpeg)
 *   - video: a Media Foundation source reader decodes NV12, converted here to
 *     the YUY2 layout the game's texture expects, written straight into it
 *   - audio: a second reader decodes PCM into a DirectSound stream buffer; the
 *     movie clock starts when that buffer starts, so picture and sound agree
 *   - the decoder reports no Xbox audio streams, so the engine never touches
 *     an IDirectSoundStream (whose DirectSound side is not emulated)
 *
 * Missing .mp4 or BUFFY_SKIP_MOVIES=1: creation fails, and the engine treats
 * the movie as finished at once, as it did when movies were skipped.
 *
 * All six are stdcall.
 */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <dsound.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "recomp/gen/recomp_types.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "ole32.lib")

#define MOVIE_HANDLE   0x0DEC0DE0u      /* the one decoder we hand out */
#define XMV_NOFRAME    0
#define XMV_NEWFRAME   1
#define XMV_ENDOFFILE  2

/* Engine globals (xbMovieOpen / xbMovieCreateTexture). */
#define G_MOVIE_MEM    0x0030AE7Cu      /* [2] frame memory (alloc + 0x80 slack) */
#define G_MOVIE_SURF   0x0030AE8Cu      /* [2] surface level of each texture */
#define G_MOVIE_VOLUME 0x0030AE78u      /* 0..100 */

typedef struct {
    IMFSourceReader *vr, *ar;
    UINT32 w, h;
    IMFSample *pending;              /* next video frame, not yet due */
    LONGLONG pending_ts;             /* its time, 100 ns */
    int vid_eof, started;
    LARGE_INTEGER t0, freq;
    /* audio */
    IDirectSound8 *ds;
    IDirectSoundBuffer *buf;
    DWORD buf_bytes, write_pos, block;
    HANDLE thread;
    volatile LONG stop;
    int volume;
    uint64_t audio_bytes;            /* PCM written, for the close log */
    DWORD audio_rate_bytes;
} Movie;

static Movie *s_mv;

static void ret_stdcall(uint32_t result, uint32_t arg_bytes)
{
    g_eax = result;
    g_esp += 4 + arg_bytes;
}

static void movie_dir(char *out, size_t cap)
{
    const char *env = getenv("BUFFY_MOVIE_DIR");
    char *slash;
    if (env && *env) {
        strncpy_s(out, cap, env, _TRUNCATE);
        return;
    }
    GetModuleFileNameA(NULL, out, (DWORD)cap);
    slash = strrchr(out, '\\');
    if (slash)
        strcpy_s(slash + 1, cap - (size_t)(slash + 1 - out), "Movies");
}

static LONG vol_to_db(int v)
{
    if (getenv("BUFFY_MUTE") && getenv("BUFFY_MUTE")[0] == '1')
        return DSBVOLUME_MIN;
    if (v <= 0) return DSBVOLUME_MIN;
    if (v >= 100) return 0;
    return (LONG)(2000.0 * log10(v / 100.0));
}

/* ── audio ─────────────────────────────────────────────────────────────── */

/* Decode and write PCM until `until` bytes are queued ahead of the play
 * cursor (or the stream ends). Returns 0 at end of stream. */
static int audio_fill(Movie *m, DWORD ahead_bytes)
{
    for (;;) {
        DWORD play = 0, wr = 0, queued;
        DWORD flags = 0, idx;
        LONGLONG ts;
        IMFSample *s = NULL;
        IMFMediaBuffer *mb = NULL;
        BYTE *p;
        DWORD len;

        IDirectSoundBuffer_GetCurrentPosition(m->buf, &play, &wr);
        queued = (m->write_pos + m->buf_bytes - play) % m->buf_bytes;
        if (m->started && queued >= ahead_bytes)
            return 1;
        if (!m->started && m->write_pos >= ahead_bytes)
            return 1;
        if (FAILED(IMFSourceReader_ReadSample(m->ar, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                              0, &idx, &flags, &ts, &s)))
            return 0;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (s) IMFSample_Release(s);
            return 0;
        }
        if (!s)
            continue;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &mb))) {
            if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, NULL, &len))) {
                while (len) {
                    void *a1, *a2;
                    DWORD n1, n2, chunk = len;
                    if (chunk > m->buf_bytes / 2)
                        chunk = m->buf_bytes / 2;
                    if (SUCCEEDED(IDirectSoundBuffer_Lock(m->buf, m->write_pos, chunk,
                                                          &a1, &n1, &a2, &n2, 0))) {
                        memcpy(a1, p, n1);
                        if (a2) memcpy(a2, p + n1, n2);
                        IDirectSoundBuffer_Unlock(m->buf, a1, n1, a2, n2);
                    }
                    m->write_pos = (m->write_pos + chunk) % m->buf_bytes;
                    m->audio_bytes += chunk;
                    p += chunk;
                    len -= chunk;
                }
                IMFMediaBuffer_Unlock(mb);
            }
            IMFMediaBuffer_Release(mb);
        }
        IMFSample_Release(s);
    }
}

static DWORD WINAPI audio_thread(LPVOID param)
{
    Movie *m = (Movie *)param;
    int ended = 0;
    DWORD end_pos = 0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (!m->stop) {
        int v = (int)MEM32(G_MOVIE_VOLUME);
        if (v != m->volume) {
            m->volume = v;
            IDirectSoundBuffer_SetVolume(m->buf, vol_to_db(v));
        }
        if (!ended) {
            if (!audio_fill(m, m->buf_bytes / 2)) {
                ended = 1;
                end_pos = m->write_pos;
            }
        } else {
            /* Past the end: keep silence ahead of the cursor. */
            DWORD play = 0, wr = 0;
            void *a1, *a2;
            DWORD n1, n2;
            IDirectSoundBuffer_GetCurrentPosition(m->buf, &play, &wr);
            if (SUCCEEDED(IDirectSoundBuffer_Lock(m->buf, end_pos, m->buf_bytes / 4, &a1, &n1, &a2, &n2, 0))) {
                memset(a1, 0, n1);
                if (a2) memset(a2, 0, n2);
                IDirectSoundBuffer_Unlock(m->buf, a1, n1, a2, n2);
            }
            end_pos = (end_pos + m->buf_bytes / 4) % m->buf_bytes;
            (void)play;
        }
        Sleep(20);
    }
    CoUninitialize();
    return 0;
}

static int audio_open(Movie *m, const WCHAR *path)
{
    IMFMediaType *t = NULL, *cur = NULL;
    WAVEFORMATEX wf;
    DSBUFFERDESC d;
    UINT32 rate = 0, ch = 0;

    if (FAILED(MFCreateSourceReaderFromURL(path, NULL, &m->ar)))
        return 0;
    IMFSourceReader_SetStreamSelection(m->ar, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(m->ar, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(IMFSourceReader_SetCurrentMediaType(m->ar, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, t))) {
        IMFMediaType_Release(t);
        return 0;
    }
    IMFMediaType_Release(t);
    IMFSourceReader_GetCurrentMediaType(m->ar, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur);
    IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
    IMFMediaType_Release(cur);
    if (!rate || !ch)
        return 0;

    if (FAILED(DirectSoundCreate8(NULL, &m->ds, NULL)))
        return 0;
    IDirectSound8_SetCooperativeLevel(m->ds, GetDesktopWindow(), DSSCL_PRIORITY);
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)ch;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(ch * 2);
    wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
    m->block = wf.nBlockAlign;
    m->audio_rate_bytes = wf.nAvgBytesPerSec;
    m->buf_bytes = wf.nAvgBytesPerSec;                   /* one second ring */
    m->buf_bytes -= m->buf_bytes % m->block;
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLVOLUME;
    d.dwBufferBytes = m->buf_bytes;
    d.lpwfxFormat = &wf;
    if (FAILED(IDirectSound8_CreateSoundBuffer(m->ds, &d, &m->buf, NULL)))
        return 0;
    m->volume = (int)MEM32(G_MOVIE_VOLUME);
    IDirectSoundBuffer_SetVolume(m->buf, vol_to_db(m->volume));
    return 1;
}

/* ── video ─────────────────────────────────────────────────────────────── */

static int video_open(Movie *m, const WCHAR *path)
{
    IMFMediaType *t = NULL, *cur = NULL;
    UINT64 size = 0;

    if (FAILED(MFCreateSourceReaderFromURL(path, NULL, &m->vr)))
        return 0;
    IMFSourceReader_SetStreamSelection(m->vr, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(m->vr, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    if (FAILED(IMFSourceReader_SetCurrentMediaType(m->vr, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, t))) {
        IMFMediaType_Release(t);
        return 0;
    }
    IMFMediaType_Release(t);
    IMFSourceReader_GetCurrentMediaType(m->vr, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &size);
    IMFMediaType_Release(cur);
    m->w = (UINT32)(size >> 32);
    m->h = (UINT32)size;
    return m->w && m->h;
}

/* Next decoded frame into m->pending; 0 at end of stream. */
static int video_read(Movie *m)
{
    for (;;) {
        DWORD flags = 0, idx;
        LONGLONG ts = 0;
        IMFSample *s = NULL;
        if (FAILED(IMFSourceReader_ReadSample(m->vr, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                              0, &idx, &flags, &ts, &s)))
            return 0;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (s) IMFSample_Release(s);
            return 0;
        }
        if (s) {
            m->pending = s;
            m->pending_ts = ts;
            return 1;
        }
    }
}

/* NV12 (decoder output) -> YUY2 (Y0 U Y1 V, the texture's format) at dst. */
static void video_write(Movie *m, IMFSample *s, uint8_t *dst, uint32_t pitch)
{
    IMFMediaBuffer *mb = NULL;
    IMF2DBuffer *b2 = NULL;
    BYTE *p = NULL;
    LONG stride = 0;
    DWORD len = 0;
    int locked2d = 0;
    uint32_t x, y;

    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &mb)))
        return;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(mb, &IID_IMF2DBuffer, (void **)&b2))
            && SUCCEEDED(IMF2DBuffer_Lock2D(b2, &p, &stride))) {
        locked2d = 1;
    } else if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, NULL, &len))) {
        stride = (LONG)m->w;
    } else {
        p = NULL;
    }
    if (p && stride > 0) {
        /* The chroma plane follows the luma rows, which may be padded to a
         * multiple of 16. */
        uint32_t rows = m->h;
        const uint8_t *uvbase;
        if (!locked2d && len >= (DWORD)stride * ((m->h + 15) & ~15u) * 3 / 2)
            rows = (m->h + 15) & ~15u;
        else if (locked2d)
            rows = m->h;
        uvbase = p + (size_t)stride * rows;
        for (y = 0; y < m->h; y++) {
            const uint8_t *yr = p + (size_t)stride * y;
            const uint8_t *uv = uvbase + (size_t)stride * (y >> 1);
            uint8_t *o = dst + (size_t)pitch * y;
            for (x = 0; x + 1 < m->w; x += 2) {
                o[0] = yr[x];
                o[1] = uv[x];
                o[2] = yr[x + 1];
                o[3] = uv[x + 1];
                o += 4;
            }
        }
    }
    if (locked2d) IMF2DBuffer_Unlock2D(b2);
    else if (p) IMFMediaBuffer_Unlock(mb);
    if (b2) IMF2DBuffer_Release(b2);
    IMFMediaBuffer_Release(mb);
}

static void movie_free(Movie *m)
{
    if (!m)
        return;
    if (m->audio_rate_bytes)
        fprintf(stderr, "  [MOVIE] closed: %.1f s of sound streamed\n",
                (double)m->audio_bytes / m->audio_rate_bytes);
    if (m->thread) {
        InterlockedExchange(&m->stop, 1);
        WaitForSingleObject(m->thread, 2000);
        CloseHandle(m->thread);
    }
    if (m->buf) { IDirectSoundBuffer_Stop(m->buf); IDirectSoundBuffer_Release(m->buf); }
    if (m->ds) IDirectSound8_Release(m->ds);
    if (m->pending) IMFSample_Release(m->pending);
    if (m->vr) IMFSourceReader_Release(m->vr);
    if (m->ar) IMFSourceReader_Release(m->ar);
    free(m);
}

/* ── XMV API ───────────────────────────────────────────────────────────── */

/* HRESULT XMVDecoder_CreateDecoderForFile(DWORD flags, LPCSTR file, XMVDecoder **pp) */
void XMVDecoder_CreateDecoderForFile_00166B8D(void)
{
    static int mf_up;
    uint32_t file = MEM32(g_esp + 8), pp = MEM32(g_esp + 12);
    char name[MAX_PATH], dir[MAX_PATH], full[MAX_PATH];
    WCHAR wpath[MAX_PATH];
    const char *src = file ? (const char *)XBOX_PTR(file) : "";
    const char *base = src;
    char *dot;
    size_t i;
    Movie *m;

    if (pp)
        MEM32(pp) = 0;
    for (i = 0; src[i]; i++)
        if (src[i] == '\\' || src[i] == '/' || src[i] == ':')
            base = src + i + 1;
    strncpy_s(name, sizeof name, base, _TRUNCATE);
    for (i = 0; name[i]; i++)
        name[i] = (char)tolower((unsigned char)name[i]);
    dot = strrchr(name, '.');
    if (dot) *dot = 0;
    movie_dir(dir, sizeof dir);
    sprintf_s(full, sizeof full, "%s\\%s.mp4", dir, name);
    {
        /* A ticked mod may replace a movie (mods\<mod>\files\Movies\<name>.mp4). */
        int buffy_mods_find(const char *rel, char *out, size_t cap);
        char rel[96], alt[MAX_PATH];
        sprintf_s(rel, sizeof rel, "Movies\\%s.mp4", name);
        if (buffy_mods_find(rel, alt, sizeof alt))
            strcpy_s(full, sizeof full, alt);
    }
    /* Settings: skip the three intro movies at start-up (not the story ones). */
    if (getenv("BUFFY_SKIP_INTRO") && getenv("BUFFY_SKIP_INTRO")[0] == '1'
            && !strncmp(name, "intro", 5)) {
        fprintf(stderr, "  [MOVIE] %s: skipped (intro)\n", src);
        ret_stdcall(0x80004005u, 12);
        return;
    }

    if ((getenv("BUFFY_SKIP_MOVIES") && getenv("BUFFY_SKIP_MOVIES")[0] == '1')
            || GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "  [MOVIE] %s: skipped (%s)\n", src,
                GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES ? "no converted movie" : "BUFFY_SKIP_MOVIES");
        ret_stdcall(0x80004005u, 12);
        return;
    }
    if (!mf_up) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        mf_up = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)) ? 1 : -1;
    }
    if (s_mv) {
        movie_free(s_mv);
        s_mv = NULL;
    }
    m = (Movie *)calloc(1, sizeof *m);
    MultiByteToWideChar(CP_ACP, 0, full, -1, wpath, MAX_PATH);
    if (mf_up < 0 || !m || !video_open(m, wpath)) {
        fprintf(stderr, "  [MOVIE] %s: cannot decode %s\n", src, full);
        movie_free(m);
        ret_stdcall(0x80004005u, 12);
        return;
    }
    if (!audio_open(m, wpath)) {
        /* Silent movie rather than none. */
        if (m->buf) { IDirectSoundBuffer_Release(m->buf); m->buf = NULL; }
        if (m->ds) { IDirectSound8_Release(m->ds); m->ds = NULL; }
        if (m->ar) { IMFSourceReader_Release(m->ar); m->ar = NULL; }
    }
    QueryPerformanceFrequency(&m->freq);
    s_mv = m;
    fprintf(stderr, "  [MOVIE] playing %s (%ux%u, %s)\n", full, m->w, m->h, m->buf ? "with sound" : "silent");
    if (pp)
        MEM32(pp) = MOVIE_HANDLE;
    ret_stdcall(0, 12);
}

/* void XMVDecoder_GetVideoDescriptor(XMVDecoder *, XMVVIDEO_DESC *) */
void XMVDecoder_GetVideoDescriptor_001671B5(void)
{
    uint32_t desc = MEM32(g_esp + 8);
    if (desc) {
        MEM32(desc + 0) = s_mv ? s_mv->w : 0;
        MEM32(desc + 4) = s_mv ? s_mv->h : 0;
        MEM32(desc + 8) = 30;
        MEM32(desc + 12) = 0;               /* audio is ours, not an Xbox stream */
    }
    ret_stdcall(0, 8);
}

/* HRESULT XMVDecoder_EnableAudioStream(dec, index, flags, mixbins, pp) */
void XMVDecoder_EnableAudioStream_00167222(void)
{
    ret_stdcall(0x80004005u, 20);
}

/* HRESULT XMVDecoder_GetAudioStream(dec, index, IDirectSoundStream **pp) */
void XMVDecoder_GetAudioStream_001673A1(void)
{
    uint32_t pp = MEM32(g_esp + 12);
    if (pp)
        MEM32(pp) = 0;
    ret_stdcall(0x80004005u, 12);
}

/* Guest address of a movie surface's pixels: the engine's own frame memory
 * for that surface (xbMovieCreateTexture aligns it up to 128 bytes). */
static uint32_t surface_memory(uint32_t surface)
{
    int i;
    for (i = 0; i < 2; i++)
        if (MEM32(G_MOVIE_SURF + 4 * i) == surface && MEM32(G_MOVIE_MEM + 4 * i))
            return (MEM32(G_MOVIE_MEM + 4 * i) + 0x7Fu) & ~0x7Fu;
    return 0;
}

/* HRESULT XMVDecoder_GetNextFrame(dec, IDirect3DSurface8 *, DWORD *result, REFERENCE_TIME *) */
void XMVDecoder_GetNextFrame_001673C0(void)
{
    uint32_t surface = MEM32(g_esp + 8), result = MEM32(g_esp + 12);
    Movie *m = s_mv;
    uint32_t r = XMV_NOFRAME, mem;
    LARGE_INTEGER now;
    LONGLONG clock;

    if (!m) {
        if (result) MEM32(result) = XMV_ENDOFFILE;
        ret_stdcall(0, 16);
        return;
    }
    if (!m->started) {
        /* Queue some sound, then start sound and picture together. */
        if (m->buf) {
            audio_fill(m, m->buf_bytes / 4);
            IDirectSoundBuffer_Play(m->buf, 0, 0, DSBPLAY_LOOPING);
        }
        QueryPerformanceCounter(&m->t0);
        m->started = 1;
        if (m->buf)
            m->thread = CreateThread(NULL, 0, audio_thread, m, 0, NULL);
    }
    QueryPerformanceCounter(&now);
    clock = (now.QuadPart - m->t0.QuadPart) * 10000000 / m->freq.QuadPart;

    if (!m->pending && !m->vid_eof && !video_read(m))
        m->vid_eof = 1;
    if (m->pending && m->pending_ts <= clock) {
        /* Drop frames that are already late, keep the latest due one. */
        for (;;) {
            IMFSample *due = m->pending;
            m->pending = NULL;
            if (!video_read(m)) {
                m->vid_eof = 1;
                m->pending = NULL;
            }
            if (m->pending && m->pending_ts <= clock) {
                IMFSample_Release(due);
                continue;
            }
            mem = surface_memory(surface);
            if (mem)
                video_write(m, due, (uint8_t *)XBOX_PTR(mem), m->w * 2);
            IMFSample_Release(due);
            r = XMV_NEWFRAME;
            break;
        }
    } else if (!m->pending && m->vid_eof) {
        r = XMV_ENDOFFILE;
    }
    if (result)
        MEM32(result) = r;
    ret_stdcall(0, 16);
}

/* void XMVDecoder_CloseDecoder(XMVDecoder *) */
void XMVDecoder_CloseDecoder_001670D1(void)
{
    if (s_mv) {
        movie_free(s_mv);
        s_mv = NULL;
    }
    ret_stdcall(0, 4);
}

/* A movie is open (the display presents it 4:3 even in widescreen). */
int buffy_movie_active(void)
{
    return s_mv != NULL;
}
