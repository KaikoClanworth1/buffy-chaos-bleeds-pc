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
int  buffy_settings_fullscreen(void);
void buffy_settings_step_res(int dir);       /* next / previous resolution, wraps */
void buffy_settings_set_vsync(int on);
void buffy_settings_set_fullscreen(int on);
const char *buffy_settings_path(void);
int  buffy_settings_frame_interpolation(void);   /* a frame in between each two */
int  buffy_settings_widescreen_wide(void);     /* 0: widescreen keeps the 4:3 side-to-side view */
int  buffy_settings_invert_camera_x(void);         /* buffy_settings.ini */
int  buffy_settings_fps_limit(void);         /* 60 or 30: the frame cap (the game's logic is one step a frame) */
int  buffy_settings_show_fps(void);          /* the FPS counter */
int  buffy_settings_overlay(void);           /* the debug overlay */
void buffy_settings_set_fps_limit(int fps);
void buffy_settings_set_show_fps(int on);
void buffy_settings_set_overlay(int on);

#endif
