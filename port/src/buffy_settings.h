#ifndef BUFFY_SETTINGS_H
#define BUFFY_SETTINGS_H

/* PC display settings (buffy_settings.c), saved to buffy_settings.ini. */
void buffy_settings_load(void);
void buffy_settings_save(void);
void buffy_settings_frame(void);             /* once a frame, game thread */
int  buffy_settings_res_width(void);
int  buffy_settings_res_height(void);
int  buffy_settings_widescreen(void);        /* the resolution is not 4:3 */
int  buffy_settings_vsync(void);
int  buffy_settings_fullscreen(void);              /* display mode: 0 windowed, 1 borderless, 2 fullscreen */
const char *buffy_settings_display_mode_name(int mode);
void buffy_settings_step_res(int dir);       /* next / previous resolution, wraps */
void buffy_settings_set_vsync(int on);
void buffy_settings_set_fullscreen(int on);
const char *buffy_settings_path(void);
int  buffy_settings_frame_interpolation(void);   /* a frame in between each two */
int  buffy_settings_widescreen_wide(void);     /* 0: widescreen keeps the 4:3 side-to-side view */
int  buffy_settings_invert_camera_x(void);         /* buffy_settings.ini */
int  buffy_settings_language_code(void);    /* the Xbox language code the game is told (XGetLanguage) */
int  buffy_settings_language_index(void);   /* 0 English, 1 French, 2 German, 3 Spanish ([Game] Language) */
int  buffy_settings_language_running(void); /* ...the one the game started in */
void buffy_settings_step_language(int dir); /* next / previous, saved for the next start */
int  buffy_settings_fps_limit(void);         /* 30 to 360: the frame cap (the game keeps its speed: buffy_frame.c) */
int  buffy_settings_show_fps(void);          /* the FPS counter */
int  buffy_settings_overlay(void);           /* the debug overlay */
void buffy_settings_set_fps_limit(int fps);
void buffy_settings_set_show_fps(int on);
void buffy_settings_set_overlay(int on);

#endif
