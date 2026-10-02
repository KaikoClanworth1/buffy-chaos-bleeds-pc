/*
 * android_main.c -- the Android build's entry (a NativeActivity).
 *
 * The game is the Windows build's (main.c's WinMain, as buffy_game_main) on
 * a thread of its own, started once the activity has a surface to draw on;
 * this thread keeps the activity's events and the controllers' input coming.
 *
 * Files: the game folder, games/Buffy Chaos Bleeds in shared storage (the
 * launcher's GameActivity says where: BUFFY_HOME), is the Windows build's
 * game folder -- default.xbe and Buffy\ from the disc, buffy_settings.ini,
 * SaveData\, mods\. Without BUFFY_HOME (started on its own, for tests) it is
 * the app's own files folder, with the disc in game\ there.
 *
 * When the app leaves the screen (the screen off, another app in front) its
 * surface goes: the renderer lets go of it and the game waits for the next
 * one (nv2a_pb_d3d11.inc, android_surface_sync).
 *
 * stderr (where the port, the kernel and the GPU layer log) goes to logcat
 * under the tag "buffy":   adb logcat -s buffy
 */
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "buffy", __VA_ARGS__)

void android_set_data_dir(const char *dir);
int  android_input_event(const AInputEvent *e);
int  buffy_game_main(void);
void buffy_audio_pause(int paused);
void android_movie_pause(int paused);   /* android_movie.c */

/* ── stderr / stdout into logcat: a pipe read by a thread of our own ── */
static int s_log_pipe[2];

static void *log_thread(void *arg)
{
    char buf[2048];
    ssize_t n;
    size_t have = 0;
    (void)arg;
    while ((n = read(s_log_pipe[0], buf + have, sizeof buf - 1 - have)) > 0) {
        char *line = buf, *nl;
        have += (size_t)n;
        buf[have] = 0;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = 0;
            if (*line)
                __android_log_write(ANDROID_LOG_INFO, "buffy", line);
            line = nl + 1;
        }
        have = strlen(line);
        if (have >= sizeof buf - 256) {         /* a very long line: out as it is */
            __android_log_write(ANDROID_LOG_INFO, "buffy", line);
            have = 0;
        } else {
            memmove(buf, line, have);
        }
    }
    return NULL;
}

static void log_to_logcat(void)
{
    pthread_t t;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pipe(s_log_pipe) != 0)
        return;
    dup2(s_log_pipe[1], 1);
    dup2(s_log_pipe[1], 2);
    pthread_create(&t, NULL, log_thread, NULL);
    pthread_detach(t);
}

/* ── the surface ── */
static ANativeWindow *volatile s_window;
static pthread_mutex_t s_win_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_win_cond = PTHREAD_COND_INITIALIZER;
static volatile int s_lost, s_released;    /* the surface is going; the renderer let go of it */
static int s_game_started;

/* The activity's surface: the renderer's "window" (win32_window_null.h).
 * Waits for one when there is none yet. */
void *android_window(void)
{
    ANativeWindow *w;
    pthread_mutex_lock(&s_win_lock);
    while (!s_window)
        pthread_cond_wait(&s_win_cond, &s_win_lock);
    w = s_window;
    pthread_mutex_unlock(&s_win_lock);
    return w;
}

/* The renderer, each frame: is the surface going? Then it lets go of it and
 * says so. */
int android_surface_lost(void)
{
    return s_lost;
}

void android_surface_released(void)
{
    pthread_mutex_lock(&s_win_lock);
    s_released = 1;
    s_lost = 0;
    pthread_cond_broadcast(&s_win_cond);
    pthread_mutex_unlock(&s_win_lock);
}

/* ── the game thread ── */
static void *game_thread(void *arg)
{
    int code;
    (void)arg;
    code = buffy_game_main();
    fprintf(stderr, "[ANDROID] the game ended (%d)\n", code);
    _exit(code);
    return NULL;
}

static void start_game(void)
{
    pthread_attr_t at;
    pthread_t t;
    if (s_game_started++)
        return;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 32u << 20);     /* deep recompiled call chains */
    pthread_create(&t, &at, game_thread, NULL);
    pthread_attr_destroy(&at);
}

/* The settings Android needs before the game reads its own (first run).
 * buffy_env.txt in the data folder, if there is one, sets more: a KEY=VALUE
 * a line (the Windows build's environment switches, for testing). */
static void android_defaults(const char *data, int home)
{
    char game[600], probe[700], line[512];
    struct stat st;
    FILE *f;
    snprintf(probe, sizeof probe, "%s/buffy_env.txt", data);
    if ((f = fopen(probe, "r")) != NULL) {
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='), *e = line + strlen(line);
            while (e > line && (e[-1] == 10 || e[-1] == 13 || e[-1] == 32))   /* (line end, spaces) */
                *--e = 0;
            if (line[0] == '#' || !eq)
                continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            fprintf(stderr, "[ANDROID] %s=%s (buffy_env.txt)\n", line, eq + 1);
        }
        fclose(f);
    }
    snprintf(game, sizeof game, home ? "%s" : "%s/game", data);
    setenv("BUFFY_GAME_DIR", game, 0);
    setenv("BUFFY_RENDERER", "vulkan", 0);          /* (the GPU layer has no Direct3D here) */
    setenv("BUFFY_GPU_BACKEND", "vulkan", 0);
    setenv("RECOMP_LAZY_DIRS", "1", 0);
    snprintf(probe, sizeof probe, "%s/default.xbe", game);
    if (stat(probe, &st) != 0) {
        snprintf(probe, sizeof probe, "%s/DEFAULT.XBE", game);
        if (stat(probe, &st) != 0)
            fprintf(stderr, "[ANDROID] no game in %s: copy the disc's files there "
                            "(default.xbe and the Buffy folder)\n", game);
    }
}

static void on_cmd(struct android_app *app, int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        pthread_mutex_lock(&s_win_lock);
        s_window = app->window;
        pthread_cond_broadcast(&s_win_cond);
        pthread_mutex_unlock(&s_win_lock);
        fprintf(stderr, "[ANDROID] surface %dx%d\n", ANativeWindow_getWidth(app->window),
                ANativeWindow_getHeight(app->window));
        start_game();
        break;
    case APP_CMD_TERM_WINDOW:
        /* The surface is going (the app left the screen): the renderer
         * lets go of it at its next frame, and the game waits for the next
         * surface. Not waited for long: a game busy loading presents
         * nothing for a while. */
        pthread_mutex_lock(&s_win_lock);
        s_window = NULL;
        if (s_game_started) {
            struct timespec until;
            s_lost = 1;
            s_released = 0;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 2;
            while (!s_released && pthread_cond_timedwait(&s_win_cond, &s_win_lock, &until) == 0)
                ;
        }
        pthread_mutex_unlock(&s_win_lock);
        fprintf(stderr, "[ANDROID] surface gone%s\n", s_released || !s_game_started ? "" : " (the renderer was busy)");
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        /* the same surface, another size or way up (folded or unfolded,
         * turned round): the renderer makes its swap chain again at its next
         * frame -- the surface stays, so it does not wait */
        if (s_game_started && s_window) {
            fprintf(stderr, "[ANDROID] surface now %dx%d\n", ANativeWindow_getWidth(s_window),
                    ANativeWindow_getHeight(s_window));
            s_lost = 1;
        }
        break;
    case APP_CMD_PAUSE:
        buffy_audio_pause(1);
        android_movie_pause(1);
        break;
    case APP_CMD_RESUME:
        buffy_audio_pause(0);
        android_movie_pause(0);
        break;
    case APP_CMD_DESTROY:
        _exit(0);
        break;
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *e)
{
    (void)app;
    return android_input_event(e);
}

void android_main(struct android_app *app)
{
    const char *home = getenv("BUFFY_HOME");
    const char *data = home && *home ? home : app->activity->externalDataPath;
    log_to_logcat();
    fprintf(stderr, "[ANDROID] Buffy the Vampire Slayer: Chaos Bleeds -- data in %s\n", data ? data : "?");
    if (data) {
        mkdir(data, 0775);
        android_set_data_dir(data);
        android_defaults(data, home && *home);
    }
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    for (;;) {
        int events;
        struct android_poll_source *src;
        while (ALooper_pollOnce(-1, NULL, &events, (void **)&src) >= 0) {
            if (src)
                src->process(app, src);
            if (app->destroyRequested)
                _exit(0);
        }
    }
}
