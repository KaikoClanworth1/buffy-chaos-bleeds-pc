/**
 * PC display settings: resolution, vsync, display mode (windowed, borderless
 * or fullscreen).
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
#include "buffy_platform.h"

void nv2a_gpu_set_display(int width, int height, int vsync, int fullscreen);
void nv2a_gpu_set_display_callback(void (*cb)(int fullscreen));
void nv2a_gpu_set_frame_widescreen(int wide);
void nv2a_gpu_set_texture_pack(const wchar_t *root, int dump, int load, int prefetch);
void nv2a_gpu_set_overlay(int mode);
void nv2a_gpu_set_frame_limit(int fps);   /* the overlay's target (nv2a_overlay.inc) */   /* 0 off, 1 FPS, 2 debug panel (nv2a_pb_d3d11.inc) */
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
static char s_language[16] = "English";   /* [Game] Language */
static int  s_ws_wide, s_invert_x;      /* [Display] WidescreenWide, [Controls] InvertCameraX */
static int  s_interp;                   /* [Display] FrameInterpolation */
static int  s_fps_limit = 60, s_show_fps, s_overlay;   /* [Display] FpsLimit, ShowFps, DebugOverlay */

/* The frame rate the game runs at: 30 to 360. The game's speed does not
 * depend on it -- the engine scales its per-frame steps by it (buffy_frame.c,
 * buffy_frame_rate_apply). BUFFY_FPS_LIMIT overrides the setting (testing). */
static int buffy_fps_clamp(int fps)
{
    const char *e = getenv("BUFFY_FPS_LIMIT");
    if (e && atoi(e) > 0)
        fps = atoi(e);
    return fps < 30 ? 30 : fps > 360 ? 360 : fps;
}
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
    sprintf_s(v, sizeof v, "%d", s_fullscreen);
    WritePrivateProfileStringA("Display", "Fullscreen", v, s_path);
    WritePrivateProfileStringA("Display", "RenderScale", NULL, s_path);   /* old key */
    sprintf_s(v, sizeof v, "%d", s_fps_limit);
    WritePrivateProfileStringA("Display", "FpsLimit", v, s_path);
    WritePrivateProfileStringA("Display", "ShowFps", s_show_fps ? "1" : "0", s_path);
    WritePrivateProfileStringA("Display", "DebugOverlay", s_overlay ? "1" : "0", s_path);
}

static void apply_overlay(void)
{
    nv2a_gpu_set_frame_limit(s_fps_limit);
    nv2a_gpu_set_overlay(s_overlay ? 2 : s_show_fps ? 1 : 0);
}

static void apply(void)
{
    nv2a_gpu_set_display(k_res[s_res].w, k_res[s_res].h, s_vsync, s_fullscreen);
    apply_overlay();
}

/* The renderer's Alt+Enter / F11 toggle (the mode it switched to). */
static void on_display_key(int mode)
{
    s_fullscreen = mode;
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
        /* 0 windowed, 1 borderless (what 1 has always meant), 2 fullscreen */
        s_fullscreen = (int)GetPrivateProfileIntA("Display", "Fullscreen", 0, s_path);
        if (s_fullscreen < 0 || s_fullscreen > 2)
            s_fullscreen = 1;
        if (i < 0 && !GetPrivateProfileIntA("Display", "Width", 0, s_path) && buffy_on_steam_deck()) {
            /* first run on a Steam Deck: its own screen, 720p */
            s_res = find_res(1280, 720);
            s_fullscreen = 1;
        }
        if (i < 0)
            buffy_settings_save();                 /* first run or old file: write it */
        s_ws_wide = GetPrivateProfileIntA("Display", "WidescreenWide", 1, s_path) != 0;
        s_interp = GetPrivateProfileIntA("Display", "FrameInterpolation", 0, s_path) != 0;
        s_invert_x = GetPrivateProfileIntA("Controls", "InvertCameraX", 0, s_path) != 0;
        s_fps_limit = buffy_fps_clamp((int)GetPrivateProfileIntA("Display", "FpsLimit", 60, s_path));
        s_show_fps = GetPrivateProfileIntA("Display", "ShowFps", 0, s_path) != 0;
        s_overlay = GetPrivateProfileIntA("Display", "DebugOverlay", 0, s_path) != 0;
        /* [Game], set from the launcher's Settings tab. */
        if (GetPrivateProfileIntA("Game", "SkipIntroMovies", 0, s_path) && !getenv("BUFFY_SKIP_INTRO"))
            _putenv("BUFFY_SKIP_INTRO=1");
        GetPrivateProfileStringA("Game", "Language", "English", s_language, sizeof s_language, s_path);
        buffy_settings_language_running();     /* the language this start is in */
    }
    {
        /* [Textures], from the launcher's Textures tab: texture packs in
         * textures_replacement\ beside the exe (dump\ and load\). */
        WCHAR root[MAX_PATH], *ws;
        GetModuleFileNameW(NULL, root, MAX_PATH);
        ws = wcsrchr(root, L'\\');
        if (ws)
            wcscpy_s(ws + 1, MAX_PATH - (ws + 1 - root), L"textures_replacement");
        nv2a_gpu_set_texture_pack(root,
                                  s_path[0] && GetPrivateProfileIntA("Textures", "Dump", 0, s_path),
                                  s_path[0] && GetPrivateProfileIntA("Textures", "Load", 0, s_path),
                                  s_path[0] && GetPrivateProfileIntA("Textures", "Prefetch", 0, s_path));
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
            buffy_settings_display_mode_name(s_fullscreen));
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

const char *buffy_settings_display_mode_name(int mode)
{
    return mode == 2 ? "fullscreen" : mode == 1 ? "borderless" : "windowed";
}

void buffy_settings_set_fullscreen(int mode)
{
    s_fullscreen = mode < 0 || mode > 2 ? 0 : mode;
    apply();
    buffy_settings_save();
}

int  buffy_settings_fps_limit(void) { return s_fps_limit; }
int  buffy_settings_show_fps(void)  { return s_show_fps; }
int  buffy_settings_overlay(void)   { return s_overlay; }

void buffy_settings_set_fps_limit(int fps)
{
    s_fps_limit = buffy_fps_clamp(fps);
    apply_overlay();
    buffy_settings_save();
}

void buffy_settings_set_show_fps(int on)
{
    s_show_fps = on != 0;
    apply_overlay();
    buffy_settings_save();
}

void buffy_settings_set_overlay(int on)
{
    s_overlay = on != 0;
    apply_overlay();
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
    {
        /* (testing) BUFFY_TEST_FPS_AT=secs: the FPS counter turned on then */
        static DWORD t0;
        static int done;
        const char *at = getenv("BUFFY_TEST_FPS_AT");
        if (!t0)
            t0 = GetTickCount();
        if (at && !done && GetTickCount() - t0 > (DWORD)(atof(at) * 1000)) {
            done = 1;
            fprintf(stderr, "[SETTINGS] (test) FPS counter on\n");
            buffy_settings_set_show_fps(1);
        }
    }
}

/* The settings file (for other parts' own sections, e.g. [Coop]). */
const char *buffy_settings_path(void)
{
    return s_path;
}

/* Widescreen view: 1 (default) the game's own widescreen, wider at the sides
 * (checked in the Magic Box and the cemetery: nothing culled at the edges);
 * 0 the original side-to-side view zoomed, top and bottom trimmed. */
/* Frame interpolation (a frame between each two of the game's): on. */
int buffy_settings_frame_interpolation(void)
{
    return s_interp || getenv("BUFFY_INTERP");
}

int buffy_settings_widescreen_wide(void)
{
    return s_ws_wide;
}

/* The right stick's left / right reversed (all controllers). */
int buffy_settings_invert_camera_x(void)
{
    return s_invert_x;
}

/* [Game] Language: the language the game's text is in -- the console's own
 * dashboard setting, which XGetLanguage answers (buffy_crt.c). English,
 * French, German, Spanish or Italian (the European release's); a number is
 * taken as an Xbox language code. BUFFY_LANGUAGE overrides (testing). */
static const struct { const char *name; int code; } k_languages[] = {
    { "English", 1 }, { "Japanese", 2 }, { "German", 3 }, { "French", 4 }, { "Spanish", 5 },
    { "Italian", 6 }, { "Korean", 7 }, { "Chinese", 8 }, { "Portuguese", 9 },
};

int buffy_settings_language_code(void)
{
    const char *v = getenv("BUFFY_LANGUAGE");
    size_t i;
#if defined(BUFFY_RELEASE_USA)
    return 1;                                  /* the North American release: English only */
#endif
    if (!v || !*v)
        v = s_language;
    if (atoi(v) >= 1 && atoi(v) <= 9)
        return atoi(v);
    for (i = 0; i < sizeof k_languages / sizeof k_languages[0]; i++)
        if (!_stricmp(v, k_languages[i].name))
            return k_languages[i].code;
    return 1;
}

/* The PC menu's Language line: the European release's four (Italian is not
 * on its disc). The game reads the language as it starts, so a change is
 * saved for the next start. */
static const char *const k_menu_languages[] = { "English", "French", "German", "Spanish" };

int buffy_settings_language_index(void)
{
    int i;
    for (i = 0; i < 4; i++)
        if (!_stricmp(s_language, k_menu_languages[i]))
            return i;
    return 0;
}

int buffy_settings_language_running(void)
{
    static int at_start = -1;
    if (at_start < 0)
        at_start = buffy_settings_language_index();
    return at_start;
}

void buffy_settings_step_language(int dir)
{
    int i = (buffy_settings_language_index() + (dir < 0 ? 3 : 1)) % 4;
    buffy_settings_language_running();          /* (what the game started in, kept) */
    strcpy_s(s_language, sizeof s_language, k_menu_languages[i]);
    if (s_path[0])
        WritePrivateProfileStringA("Game", "Language", s_language, s_path);
}
