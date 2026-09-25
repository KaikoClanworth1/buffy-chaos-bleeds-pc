/**
 * PC display settings: resolution, vsync, fullscreen.
 *
 * Kept in buffy_settings.ini beside the exe, applied to the D3D11 backend
 * (nv2a_gpu_set_display) at start-up and whenever the Options page or the
 * Alt+Enter / F11 key changes one. BUFFY_RES=<w>x<h> overrides the
 * resolution for a run without saving it.
 *
 * Widescreen. The game has a 16:9 mode for widescreen TVs: with the Xbox's
 * widescreen video flag it renders gameplay anamorphic -- a wider camera and a
 * rearranged HUD squeezed into 640x480, for the TV to stretch -- while menus,
 * loading screens and movies stay 4:3. A 16:9 resolution turns that flag on
 * (XApp +0x34 at 0x26D764, which the game copies into
 * EXBaseRenderEnv::ms_WideScreen at 0x27EC21 when a level starts), renders
 * at the 16:9 size, and each flip is shown 16:9 or 4:3 by what the game says
 * it is drawing.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp/gen/recomp_types.h"
#include "buffy_settings.h"

void nv2a_gpu_set_display(int width, int height, int vsync, int fullscreen);
void nv2a_gpu_set_display_callback(void (*cb)(int fullscreen));
void nv2a_gpu_set_frame_widescreen(int wide);
int  buffy_movie_active(void);

#define G_APP_WIDESCREEN   0x26D764u       /* XApp +0x34: TV is widescreen */
#define G_MS_WIDESCREEN    0x27EC21u       /* EXBaseRenderEnv::ms_WideScreen (byte) */
#define G_SCENE_HASH       0x1B7FACu       /* current scene ... */
#define G_FRONTEND_HASH    0x1B7F68u       /* ... equal to this in the menus */

static const struct { int w, h; } k_res[] = {
    { 640, 480 }, { 1280, 960 }, { 1920, 1440 }, { 2560, 1920 },
    { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
};
#define N_RES ((int)(sizeof k_res / sizeof k_res[0]))
#define DEFAULT_RES 5                       /* 1920 x 1080 */

static int  s_res = DEFAULT_RES, s_vsync = 1, s_fullscreen;
static int  s_ws_wide, s_invert_x;      /* [Display] WidescreenWide, [Controls] InvertCameraX */
static char s_path[MAX_PATH];

void buffy_settings_save(void)
{
    char v[16];
    if (!s_path[0])
        return;
    sprintf_s(v, sizeof v, "%d", k_res[s_res].w);
    WritePrivateProfileStringA("Display", "Width", v, s_path);
    sprintf_s(v, sizeof v, "%d", k_res[s_res].h);
    WritePrivateProfileStringA("Display", "Height", v, s_path);
    WritePrivateProfileStringA("Display", "VSync", s_vsync ? "1" : "0", s_path);
    WritePrivateProfileStringA("Display", "Fullscreen", s_fullscreen ? "1" : "0", s_path);
    WritePrivateProfileStringA("Display", "RenderScale", NULL, s_path);   /* old key */
}

static void apply(void)
{
    nv2a_gpu_set_display(k_res[s_res].w, k_res[s_res].h, s_vsync, s_fullscreen);
}

/* The renderer's Alt+Enter / F11 toggle. */
static void on_display_key(int fullscreen)
{
    s_fullscreen = fullscreen;
    buffy_settings_save();
}

static int find_res(int w, int h)
{
    int i;
    for (i = 0; i < N_RES; i++)
        if (k_res[i].w == w && k_res[i].h == h)
            return i;
    return -1;
}

void buffy_settings_load(void)
{
    char *slash;
    const char *env;
    int i;
    GetModuleFileNameA(NULL, s_path, MAX_PATH);
    slash = strrchr(s_path, '\\');
    if (slash)
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - s_path), "buffy_settings.ini");
    else
        s_path[0] = 0;
    if (s_path[0]) {
        i = find_res((int)GetPrivateProfileIntA("Display", "Width", 0, s_path),
                     (int)GetPrivateProfileIntA("Display", "Height", 0, s_path));
        s_res = i >= 0 ? i : DEFAULT_RES;
        s_vsync = GetPrivateProfileIntA("Display", "VSync", 1, s_path) != 0;
        s_fullscreen = GetPrivateProfileIntA("Display", "Fullscreen", 0, s_path) != 0;
        if (i < 0)
            buffy_settings_save();                 /* first run or old file: write it */
        s_ws_wide = GetPrivateProfileIntA("Display", "WidescreenWide", 0, s_path) != 0;
        s_invert_x = GetPrivateProfileIntA("Controls", "InvertCameraX", 0, s_path) != 0;
        /* [Game], set from the launcher's Settings tab. */
        if (GetPrivateProfileIntA("Game", "SkipIntroMovies", 0, s_path) && !getenv("BUFFY_SKIP_INTRO"))
            _putenv("BUFFY_SKIP_INTRO=1");
    }
    env = getenv("BUFFY_RES");
    if (env) {
        int w = atoi(env), h = strchr(env, 'x') ? atoi(strchr(env, 'x') + 1) : 0;
        if ((i = find_res(w, h)) >= 0)
            s_res = i;
    }
    if (getenv("BUFFY_NO_WINDOW"))
        s_fullscreen = 0;
    nv2a_gpu_set_display_callback(on_display_key);
    apply();
    fprintf(stderr, "[SETTINGS] %dx%d%s, vsync %s, %s\n", k_res[s_res].w, k_res[s_res].h,
            buffy_settings_widescreen() ? " widescreen" : "", s_vsync ? "on" : "off",
            s_fullscreen ? "fullscreen" : "windowed");
}

int  buffy_settings_res_width(void)  { return k_res[s_res].w; }
int  buffy_settings_res_height(void) { return k_res[s_res].h; }
int  buffy_settings_widescreen(void) { return k_res[s_res].w * 3 != k_res[s_res].h * 4; }
int  buffy_settings_vsync(void)      { return s_vsync; }
int  buffy_settings_fullscreen(void) { return s_fullscreen; }

void buffy_settings_step_res(int dir)
{
    s_res = (s_res + (dir < 0 ? N_RES - 1 : 1)) % N_RES;
    apply();
    buffy_settings_save();
}

void buffy_settings_set_vsync(int on)
{
    s_vsync = on != 0;
    apply();
    buffy_settings_save();
}

void buffy_settings_set_fullscreen(int on)
{
    s_fullscreen = on != 0;
    apply();
    buffy_settings_save();
}

/* Once a frame, on the game thread (buffy_frame.c): keep the game's
 * widescreen flags in line with the resolution, and tell the display whether
 * this frame is 16:9. */
void buffy_settings_frame(void)
{
    static int last = -1;
    int buffy_coop_split_active(void);
    int ws = buffy_settings_widescreen() && !buffy_coop_split_active();   /* split halves: 4:3 HUD */
    MEM32(G_APP_WIDESCREEN) = (uint32_t)ws;
    if (ws != last) {
        /* Changed from the menu: the game only re-reads the flag when a
         * level starts, so switch a running level now. The menus (and the
         * boot screens, before the first frame's value) stay 4:3. */
        int first = last < 0;
        last = ws;
        if (!first && MEM32(G_SCENE_HASH) != MEM32(G_FRONTEND_HASH))
            MEM8(G_MS_WIDESCREEN) = (uint8_t)ws;
    }
    nv2a_gpu_set_frame_widescreen(MEM8(G_MS_WIDESCREEN) && !buffy_movie_active());
}

/* The settings file (for other parts' own sections, e.g. [Coop]). */
const char *buffy_settings_path(void)
{
    return s_path;
}

/* Widescreen view: 0 (default) the original side-to-side view, top and
 * bottom trimmed; 1 the game's own widescreen, wider at the sides (which its
 * 4:3 culling and camera collision were not made for). */
int buffy_settings_widescreen_wide(void)
{
    return s_ws_wide;
}

/* The right stick's left / right reversed (all controllers). */
int buffy_settings_invert_camera_x(void)
{
    return s_invert_x;
}
