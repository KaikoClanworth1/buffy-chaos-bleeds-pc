/* Movies on Android: the XDK's XMV decoder API (buffy_movie.c's, on the PC
 * with Media Foundation) answered with the NDK's media codecs.
 *
 * The movies are the PC build's: Movies/<name>.mp4 (H.264 + AAC), converted
 * from the disc's .xmv files by the PC launcher (FFmpeg) and copied into the
 * game folder -- a phone has no decoder for the Xbox's WMV2. A mod may
 * replace one (mods/<mod>/files/Movies/<name>.mp4), as on the PC.
 *
 *   video: AMediaExtractor + AMediaCodec (the phone's hardware H.264
 *          decoder) on a thread of its own, a few frames ahead, each turned
 *          into the YUY2 the game's movie texture holds (NV12 or I420 in)
 *   sound: the AAC track decoded to 16-bit PCM and written to an AAudio
 *          stream of its own by a second thread
 *   clock: the sound played so far (AAudio's frames read), so picture and
 *          sound agree and both stop while the app is in the background;
 *          a silent movie (or BUFFY_MUTE) runs on the monotonic clock
 *
 * The game's side -- the movie textures, drawing them, timing, the skip
 * button -- is the game's own, as on the PC: GetNextFrame writes the frame
 * that is due into the texture's memory. No .mp4, or BUFFY_SKIP_MOVIES=1:
 * creating the decoder fails and the engine treats the movie as over. */
#include <windows.h>
#include <aaudio/AAudio.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "recomp/gen/recomp_types.h"

#define MOVIE_HANDLE   0x0DEC0DE0u      /* the one decoder we hand out */
#define XMV_NOFRAME    0
#define XMV_NEWFRAME   1
#define XMV_ENDOFFILE  2

/* Engine globals (xbMovieOpen / xbMovieCreateTexture), as buffy_movie.c's. */
#define G_MOVIE_MEM    0x0030AE7Cu      /* [2] frame memory (alloc + 0x80 slack) */
#define G_MOVIE_SURF   0x0030AE8Cu      /* [2] surface level of each texture */
#define G_MOVIE_VOLUME 0x0030AE78u      /* 0..100 */

#define QUEUE 4                         /* decoded frames held ahead */

/* MediaCodec's colour formats (MediaCodecInfo.CodecCapabilities) */
#define COLOR_YUV420_PLANAR      19     /* I420 */
#define COLOR_YUV420_SEMIPLANAR  21     /* NV12 */
#define COLOR_YUV420_FLEXIBLE    0x7F420888

typedef struct {
    uint8_t *yuy2;                      /* w * h * 2 */
    int64_t us;                         /* presentation time */
} Frame;

typedef struct {
    int fd;
    uint32_t w, h;
    /* video */
    AMediaExtractor *vx;
    AMediaCodec *vc;
    int32_t color, stride, slice;       /* the decoder's output layout */
    pthread_t vthread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    Frame q[QUEUE];
    int qhead, qcount;                  /* decoded, not yet shown */
    int vid_eof;                        /* the decoder has no more */
    /* sound */
    AMediaExtractor *ax;
    AMediaCodec *ac;
    AAudioStream *as;
    int32_t rate, channels;
    pthread_t athread;
    int audio_ok;
    int volume;
    /* clock */
    int started;
    struct timespec t0;
    atomic_int stop;
} Movie;

static Movie *s_mv;

static void ret_stdcall(uint32_t result, uint32_t arg_bytes)
{
    g_eax = result;
    g_esp += 4 + arg_bytes;
}

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void slashes(char *p)
{
    for (; *p; p++)
        if (*p == '\\')
            *p = '/';
}

/* ── video ─────────────────────────────────────────────────────────────── */

/* One decoded picture (MediaCodec's layout) -> YUY2 (Y0 U Y1 V). */
static void to_yuy2(Movie *m, const uint8_t *p, size_t len, uint8_t *dst)
{
    uint32_t x, y, w = m->w, h = m->h;
    size_t stride = m->stride > 0 ? (size_t)m->stride : w;
    size_t slice = m->slice > 0 ? (size_t)m->slice : h;
    const uint8_t *yp = p, *up, *vp;
    size_t cstride;
    int semi = m->color != COLOR_YUV420_PLANAR;
    if (semi) {
        up = p + stride * slice;
        vp = up + 1;
        cstride = stride;
    } else {
        up = p + stride * slice;
        vp = up + (stride / 2) * (slice / 2);
        cstride = stride / 2;
    }
    if ((size_t)(up - p) + cstride * (h / 2) > len)
        return;                          /* (not the layout it said: left as it was) */
    for (y = 0; y < h; y++) {
        const uint8_t *yr = yp + stride * y;
        const uint8_t *ur = up + cstride * (y >> 1), *vr = vp + cstride * (y >> 1);
        uint8_t *o = dst + (size_t)w * 2 * y;
        for (x = 0; x + 1 < w; x += 2) {
            o[0] = yr[x];
            o[1] = semi ? ur[x] : ur[x >> 1];
            o[2] = yr[x + 1];
            o[3] = semi ? vr[x] : vr[x >> 1];
            o += 4;
        }
    }
}

static void read_layout(Movie *m)
{
    AMediaFormat *f = AMediaCodec_getOutputFormat(m->vc);
    int32_t v;
    if (!f)
        return;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v)) m->color = v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v > 0) m->stride = v;
    if (AMediaFormat_getInt32(f, "slice-height", &v) && v > 0) m->slice = v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v) && v > 0 && !m->stride) m->stride = v;
    AMediaFormat_delete(f);
    fprintf(stderr, "  [MOVIE] decoder output: colour %d, stride %d, rows %d\n", m->color, m->stride, m->slice);
}

/* Feeds one sample to a decoder; 0 when the track has ended (sent EOS). */
static int feed(AMediaExtractor *x, AMediaCodec *c, int *eos_sent)
{
    ssize_t i;
    if (*eos_sent)
        return 0;
    i = AMediaCodec_dequeueInputBuffer(c, 2000);
    if (i < 0)
        return 1;
    {
        size_t cap;
        uint8_t *b = AMediaCodec_getInputBuffer(c, (size_t)i, &cap);
        ssize_t n = b ? AMediaExtractor_readSampleData(x, b, cap) : -1;
        if (n < 0) {
            AMediaCodec_queueInputBuffer(c, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            *eos_sent = 1;
            return 0;
        }
        AMediaCodec_queueInputBuffer(c, (size_t)i, 0, (size_t)n, AMediaExtractor_getSampleTime(x), 0);
        AMediaExtractor_advance(x);
    }
    return 1;
}

static void *video_thread(void *arg)
{
    Movie *m = (Movie *)arg;
    int eos_sent = 0;
    while (!atomic_load(&m->stop)) {
        AMediaCodecBufferInfo info;
        ssize_t o;
        pthread_mutex_lock(&m->lock);
        while (m->qcount == QUEUE && !atomic_load(&m->stop))
            pthread_cond_wait(&m->cond, &m->lock);
        pthread_mutex_unlock(&m->lock);
        if (atomic_load(&m->stop))
            break;
        feed(m->vx, m->vc, &eos_sent);
        o = AMediaCodec_dequeueOutputBuffer(m->vc, &info, 5000);
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            read_layout(m);
            continue;
        }
        if (o < 0)
            continue;
        if (info.size > 0) {
            size_t cap;
            uint8_t *b = AMediaCodec_getOutputBuffer(m->vc, (size_t)o, &cap);
            pthread_mutex_lock(&m->lock);
            {
                Frame *f = &m->q[(m->qhead + m->qcount) % QUEUE];
                pthread_mutex_unlock(&m->lock);
                if (b)
                    to_yuy2(m, b + info.offset, (size_t)info.size, f->yuy2);
                f->us = info.presentationTimeUs;
                pthread_mutex_lock(&m->lock);
                m->qcount++;
            }
            pthread_mutex_unlock(&m->lock);
        }
        AMediaCodec_releaseOutputBuffer(m->vc, (size_t)o, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
            break;
    }
    pthread_mutex_lock(&m->lock);
    m->vid_eof = 1;
    pthread_mutex_unlock(&m->lock);
    return NULL;
}

static ssize_t pick_track(AMediaExtractor *x, const char *prefix, AMediaFormat **out)
{
    size_t i, n = AMediaExtractor_getTrackCount(x);
    for (i = 0; i < n; i++) {
        AMediaFormat *f = AMediaExtractor_getTrackFormat(x, i);
        const char *mime = NULL;
        if (f && AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && mime
                && !strncmp(mime, prefix, strlen(prefix))) {
            *out = f;
            return (ssize_t)i;
        }
        if (f)
            AMediaFormat_delete(f);
    }
    return -1;
}

static int video_open(Movie *m)
{
    AMediaFormat *f = NULL;
    const char *mime = NULL;
    int32_t w = 0, h = 0, i;
    ssize_t t;
    off_t size = lseek(m->fd, 0, SEEK_END);
    m->vx = AMediaExtractor_new();
    if (!m->vx || AMediaExtractor_setDataSourceFd(m->vx, m->fd, 0, size) != AMEDIA_OK)
        return 0;
    if ((t = pick_track(m->vx, "video/", &f)) < 0)
        return 0;
    AMediaExtractor_selectTrack(m->vx, (size_t)t);
    AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
    if (w <= 0 || h <= 0 || !mime || !(m->vc = AMediaCodec_createDecoderByType(mime))) {
        AMediaFormat_delete(f);
        return 0;
    }
    m->w = (uint32_t)w;
    m->h = (uint32_t)h;
    m->color = COLOR_YUV420_SEMIPLANAR;
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_YUV420_FLEXIBLE);
    if (AMediaCodec_configure(m->vc, f, NULL, NULL, 0) != AMEDIA_OK || AMediaCodec_start(m->vc) != AMEDIA_OK) {
        AMediaFormat_delete(f);
        return 0;
    }
    AMediaFormat_delete(f);
    for (i = 0; i < QUEUE; i++)
        if (!(m->q[i].yuy2 = (uint8_t *)calloc((size_t)w * h, 2)))
            return 0;
    read_layout(m);
    return 1;
}

/* ── sound ─────────────────────────────────────────────────────────────── */

static void *audio_thread(void *arg)
{
    Movie *m = (Movie *)arg;
    int eos_sent = 0;
    int mute = getenv("BUFFY_MUTE") && getenv("BUFFY_MUTE")[0] == '1';
    while (!atomic_load(&m->stop)) {
        AMediaCodecBufferInfo info;
        ssize_t o;
        feed(m->ax, m->ac, &eos_sent);
        o = AMediaCodec_dequeueOutputBuffer(m->ac, &info, 5000);
        if (o < 0)
            continue;
        if (info.size > 0) {
            size_t cap;
            int16_t *pcm = (int16_t *)AMediaCodec_getOutputBuffer(m->ac, (size_t)o, &cap);
            if (pcm) {
                int32_t frames = info.size / (2 * m->channels), done = 0;
                int v = (int)MEM32(G_MOVIE_VOLUME), k;
                int16_t *s = (int16_t *)((uint8_t *)pcm + info.offset);
                if (mute || v <= 0)
                    memset(s, 0, (size_t)info.size);
                else if (v < 100)
                    for (k = 0; k < frames * m->channels; k++)
                        s[k] = (int16_t)(s[k] * v / 100);
                while (done < frames && !atomic_load(&m->stop)) {
                    /* (blocks while the stream's buffer is full: the pace) */
                    aaudio_result_t r = AAudioStream_write(m->as, s + done * m->channels, frames - done, 100000000LL);
                    if (r < 0)
                        break;
                    done += r;
                }
            }
        }
        AMediaCodec_releaseOutputBuffer(m->ac, (size_t)o, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
            break;
    }
    return NULL;
}

static int audio_open(Movie *m)
{
    AMediaFormat *f = NULL;
    const char *mime = NULL;
    AAudioStreamBuilder *b;
    ssize_t t;
    off_t size = lseek(m->fd, 0, SEEK_END);
    m->ax = AMediaExtractor_new();
    if (!m->ax || AMediaExtractor_setDataSourceFd(m->ax, m->fd, 0, size) != AMEDIA_OK)
        return 0;
    if ((t = pick_track(m->ax, "audio/", &f)) < 0)
        return 0;
    AMediaExtractor_selectTrack(m->ax, (size_t)t);
    AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &m->rate);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &m->channels);
    if (m->rate <= 0 || m->channels <= 0 || m->channels > 2 || !mime
            || !(m->ac = AMediaCodec_createDecoderByType(mime))
            || AMediaCodec_configure(m->ac, f, NULL, NULL, 0) != AMEDIA_OK
            || AMediaCodec_start(m->ac) != AMEDIA_OK) {
        AMediaFormat_delete(f);
        return 0;
    }
    AMediaFormat_delete(f);
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK)
        return 0;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(b, m->rate);
    AAudioStreamBuilder_setChannelCount(b, m->channels);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MOVIE);
    if (AAudioStreamBuilder_openStream(b, &m->as) != AAUDIO_OK)
        m->as = NULL;
    AAudioStreamBuilder_delete(b);
    if (!m->as)
        return 0;
    /* (the decoder may resample: the stream's own rate is the clock's) */
    m->rate = AAudioStream_getSampleRate(m->as);
    return 1;
}

/* Microseconds of movie played: the sound's, or the wall clock's. */
static int64_t movie_clock(Movie *m)
{
    if (m->audio_ok)
        return AAudioStream_getFramesRead(m->as) * 1000000 / m->rate;
    return now_us() - ((int64_t)m->t0.tv_sec * 1000000 + m->t0.tv_nsec / 1000);
}

static void movie_free(Movie *m)
{
    int i;
    if (!m)
        return;
    atomic_store(&m->stop, 1);
    if (m->vthread) {
        pthread_mutex_lock(&m->lock);
        pthread_cond_broadcast(&m->cond);
        pthread_mutex_unlock(&m->lock);
        pthread_join(m->vthread, NULL);
    }
    if (m->athread)
        pthread_join(m->athread, NULL);
    if (m->as) { AAudioStream_requestStop(m->as); AAudioStream_close(m->as); }
    if (m->vc) { AMediaCodec_stop(m->vc); AMediaCodec_delete(m->vc); }
    if (m->ac) { AMediaCodec_stop(m->ac); AMediaCodec_delete(m->ac); }
    if (m->vx) AMediaExtractor_delete(m->vx);
    if (m->ax) AMediaExtractor_delete(m->ax);
    for (i = 0; i < QUEUE; i++)
        free(m->q[i].yuy2);
    if (m->fd >= 0) close(m->fd);
    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->cond);
    free(m);
}

/* The app went to the background (or came back): the movie's sound, and
 * with it the movie's clock, waits too. */
void android_movie_pause(int paused)
{
    Movie *m = s_mv;
    if (!m || !m->as || !m->started)
        return;
    if (paused)
        AAudioStream_requestPause(m->as);
    else
        AAudioStream_requestStart(m->as);
}

/* ── XMV API (buffy_movie.c's six) ─────────────────────────────────────── */

/* HRESULT XMVDecoder_CreateDecoderForFile(DWORD flags, LPCSTR file, XMVDecoder **pp) */
void XMVDecoder_CreateDecoderForFile_00166B8D(void)
{
    uint32_t file = MEM32(g_esp + 8), pp = MEM32(g_esp + 12);
    const char *src = file ? (const char *)XBOX_PTR(file) : "";
    const char *base = src, *env = getenv("BUFFY_MOVIE_DIR");
    char name[128], dir[1024], full[1100], *dot, *slash;
    struct stat st;
    size_t i;
    Movie *m;

    if (pp)
        MEM32(pp) = 0;
    for (i = 0; src[i]; i++)
        if (src[i] == '\\' || src[i] == '/' || src[i] == ':')
            base = src + i + 1;
    snprintf(name, sizeof name, "%s", base);
    for (i = 0; name[i]; i++)
        name[i] = (char)tolower((unsigned char)name[i]);
    if ((dot = strrchr(name, '.')) != NULL)
        *dot = 0;
    if (env && *env) {
        snprintf(dir, sizeof dir, "%s", env);
    } else {
        GetModuleFileNameA(NULL, dir, sizeof dir);
        slashes(dir);
        if ((slash = strrchr(dir, '/')) != NULL)
            snprintf(slash + 1, sizeof dir - (size_t)(slash + 1 - dir), "Movies");
    }
    snprintf(full, sizeof full, "%s/%s.mp4", dir, name);
    {
        /* A ticked mod may replace a movie (mods/<mod>/files/Movies/<name>.mp4). */
        int buffy_mods_find(const char *rel, char *out, size_t cap);
        char rel[160], alt[1100];
        snprintf(rel, sizeof rel, "Movies\\%s.mp4", name);
        if (buffy_mods_find(rel, alt, sizeof alt)) {
            slashes(alt);
            snprintf(full, sizeof full, "%s", alt);
        }
    }
    slashes(full);
    if (getenv("BUFFY_SKIP_INTRO") && getenv("BUFFY_SKIP_INTRO")[0] == '1' && !strncmp(name, "intro", 5)) {
        fprintf(stderr, "  [MOVIE] %s: skipped (intro)\n", src);
        ret_stdcall(0x80004005u, 12);
        return;
    }
    if ((getenv("BUFFY_SKIP_MOVIES") && getenv("BUFFY_SKIP_MOVIES")[0] == '1') || stat(full, &st) != 0) {
        fprintf(stderr, "  [MOVIE] %s: skipped (%s)\n", src,
                stat(full, &st) != 0 ? "no converted movie: copy a PC install's Movies folder" : "BUFFY_SKIP_MOVIES");
        ret_stdcall(0x80004005u, 12);
        return;
    }
    if (s_mv) {
        movie_free(s_mv);
        s_mv = NULL;
    }
    m = (Movie *)calloc(1, sizeof *m);
    if (m) {
        m->fd = open(full, O_RDONLY);
        pthread_mutex_init(&m->lock, NULL);
        pthread_cond_init(&m->cond, NULL);
    }
    if (!m || m->fd < 0 || !video_open(m)) {
        fprintf(stderr, "  [MOVIE] %s: cannot decode %s\n", src, full);
        movie_free(m);
        ret_stdcall(0x80004005u, 12);
        return;
    }
    m->audio_ok = audio_open(m);
    if (!m->audio_ok) {
        /* a silent movie rather than none */
        if (m->as) { AAudioStream_close(m->as); m->as = NULL; }
        if (m->ac) { AMediaCodec_stop(m->ac); AMediaCodec_delete(m->ac); m->ac = NULL; }
    }
    pthread_create(&m->vthread, NULL, video_thread, m);
    s_mv = m;
    fprintf(stderr, "  [MOVIE] playing %s (%ux%u, %s)\n", full, m->w, m->h,
            m->audio_ok ? "with sound" : "silent");
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
        MEM32(desc + 12) = 0;               /* sound is ours, not an Xbox stream */
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

/* Guest address of a movie surface's pixels (xbMovieCreateTexture's memory,
 * aligned up to 128 bytes). */
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
    uint32_t r = XMV_NOFRAME;
    int64_t clock;
    Frame *due = NULL;

    if (!m) {
        if (result) MEM32(result) = XMV_ENDOFFILE;
        ret_stdcall(0, 16);
        return;
    }
    if (!m->started) {
        /* the first picture decoded, then sound and picture start together */
        int64_t give_up = now_us() + 2000000;
        for (;;) {
            int ready;
            pthread_mutex_lock(&m->lock);
            ready = m->qcount > 0 || m->vid_eof;
            pthread_mutex_unlock(&m->lock);
            if (ready || now_us() > give_up)
                break;
            usleep(2000);
        }
        if (m->audio_ok) {
            AAudioStream_requestStart(m->as);
            pthread_create(&m->athread, NULL, audio_thread, m);
        }
        clock_gettime(CLOCK_MONOTONIC, &m->t0);
        m->started = 1;
    }
    clock = movie_clock(m);
    pthread_mutex_lock(&m->lock);
    /* the latest frame that is due; late ones before it are dropped */
    while (m->qcount > 1 && m->q[(m->qhead + 1) % QUEUE].us <= clock) {
        m->qhead = (m->qhead + 1) % QUEUE;
        m->qcount--;
    }
    if (m->qcount > 0 && m->q[m->qhead].us <= clock)
        due = &m->q[m->qhead];
    if (due) {
        uint32_t mem = surface_memory(surface);
        if (mem)
            memcpy((uint8_t *)XBOX_PTR(mem), due->yuy2, (size_t)m->w * m->h * 2);
        m->qhead = (m->qhead + 1) % QUEUE;
        m->qcount--;
        pthread_cond_signal(&m->cond);
        r = XMV_NEWFRAME;
    } else if (m->qcount == 0 && m->vid_eof) {
        r = XMV_ENDOFFILE;
    }
    pthread_mutex_unlock(&m->lock);
    if (result)
        MEM32(result) = r;
    ret_stdcall(0, 16);
}

/* void XMVDecoder_CloseDecoder(XMVDecoder *) */
void XMVDecoder_CloseDecoder_001670D1(void)
{
    if (s_mv) {
        fprintf(stderr, "  [MOVIE] closed at %.1f s\n", s_mv->started ? movie_clock(s_mv) / 1e6 : 0.0);
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
