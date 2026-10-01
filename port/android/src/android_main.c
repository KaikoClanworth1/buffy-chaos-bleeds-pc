/*
 * android_main.c -- the Android build's entry (a NativeActivity).
 *
 * Milestone 0: the GPU layer's Vulkan backend on the device. The window's
 * surface gets a swap chain through gpu.h and is cleared, colour cycling,
 * every frame; tapping the screen ends it. stderr (where the port and the
 * GPU layer log) goes to logcat under the tag "buffy":
 *   adb logcat -s buffy
 */
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gpu.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "buffy", __VA_ARGS__)

/* stderr / stdout into logcat: a pipe read by a thread of our own */
static int s_log_pipe[2];

static void *log_thread(void *arg)
{
    char buf[1024];
    ssize_t n;
    (void)arg;
    while ((n = read(s_log_pipe[0], buf, sizeof buf - 1)) > 0) {
        char *line = buf, *nl;
        buf[n] = 0;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = 0;
            if (*line)
                __android_log_write(ANDROID_LOG_INFO, "buffy", line);
            line = nl + 1;
        }
        if (*line)
            __android_log_write(ANDROID_LOG_INFO, "buffy", line);
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

typedef struct {
    ANativeWindow *window;
    GpuSwapchain *sc;
    int gpu_up, quit;
} App;

static void on_cmd(struct android_app *app, int32_t cmd)
{
    App *a = (App *)app->userData;
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        a->window = app->window;
        if (!a->gpu_up) {
            a->gpu_up = gpu_init(GPU_BACKEND_VULKAN, a->window);
            fprintf(stderr, "GPU layer: %s (%s)\n", a->gpu_up ? "up" : "FAILED", gpu_adapter_name());
        }
        if (a->gpu_up && !a->sc) {
            a->sc = gpu_swapchain_create(a->window);
            fprintf(stderr, "swap chain: %s (%dx%d)\n", a->sc ? "made" : "FAILED",
                    ANativeWindow_getWidth(a->window), ANativeWindow_getHeight(a->window));
        }
        break;
    case APP_CMD_TERM_WINDOW:
        if (a->sc) {
            gpu_swapchain_release(a->sc);
            a->sc = NULL;
        }
        a->window = NULL;
        break;
    case APP_CMD_DESTROY:
        a->quit = 1;
        break;
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *e)
{
    App *a = (App *)app->userData;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION
            && (AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_UP) {
        fprintf(stderr, "tap: done\n");
        ANativeActivity_finish(app->activity);
        a->quit = 1;
        return 1;
    }
    return 0;
}

void android_main(struct android_app *app)
{
    App a;
    unsigned frames = 0;
    struct timespec t0, now;
    memset(&a, 0, sizeof a);
    log_to_logcat();
    fprintf(stderr, "Buffy: Chaos Bleeds (Android) -- milestone 0: Vulkan through the GPU layer\n");
    app->userData = &a;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!a.quit && !app->destroyRequested) {
        int events;
        struct android_poll_source *src;
        /* poll: wait only while there is nothing to draw on */
        while (ALooper_pollOnce(a.sc ? 0 : -1, NULL, &events, (void **)&src) >= 0) {
            if (src)
                src->process(app, src);
            if (app->destroyRequested)
                break;
        }
        if (a.sc) {
            GpuTexture *bb = gpu_swapchain_begin(a.sc, 1);
            if (bb) {
                float t = (float)frames / 60.0f;
                float rgba[4] = { 0.5f + 0.5f * sinf(t), 0.5f + 0.5f * sinf(t + 2.1f), 0.5f + 0.5f * sinf(t + 4.2f), 1.0f };
                gpu_clear_target(bb, rgba);
                gpu_swapchain_present(a.sc, 1);
                frames++;
            }
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= 5) {
                fprintf(stderr, "%.1f fps\n", frames / (double)(now.tv_sec - t0.tv_sec));
                frames = 0;
                t0 = now;
            }
        }
    }
    if (a.sc)
        gpu_swapchain_release(a.sc);
    fprintf(stderr, "exit\n");
}
