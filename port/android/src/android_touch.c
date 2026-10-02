/* On-screen touch controls (Android): a pad drawn over the game and read as
 * pad 0, together with any controller (android_input.c).
 *
 *   left:   a move stick that starts where the thumb lands (bottom left) and a
 *           d-pad above it; LT in the top-left corner
 *   right:  A / B / X / Y in the Xbox's colours, Black and White above them;
 *           RT in the top-right corner; anywhere else on the right half
 *           is the camera (the right stick, from where the thumb lands)
 *   top:    Back and Start, and EDIT under them
 *
 * Each finger keeps the control it landed on until it lifts. The controls
 * hide when a controller is used and come back at the next touch.
 *
 * EDIT (or the launcher's Controls tab: BUFFY_TOUCH_EDIT=1) rearranges them:
 * drag a control to move it, tap it and press - / + to size it, RESET for
 * the original layout, DONE to keep it. The game gets no touches meanwhile.
 * Each group's place and size is kept in buffy_settings.ini, [Android]
 * TouchLayout = name:dx:dy:size,... (dx, dy in hundredths of the screen's
 * height from where the group starts; size in percent).
 *
 * Drawn as one layer: a half-size picture of the controls (premultiplied
 * BGRA, made on the CPU, again only when something changes) blended over
 * the back buffer before each present (nv2a_overlay.inc calls
 * android_touch_draw). Sizes are in units of a hundredth of the screen's
 * height. */
#include <windows.h>
#include <xinput.h>
#include <android/input.h>
#include <math.h>
#include <pthread.h>
#include "gpu.h"
#include "android_canvas.h"

enum {
    C_NONE = -1,
    C_A, C_B, C_X, C_Y, C_BLACK, C_WHITE, C_LT, C_RT, C_BACK, C_START,
    C_COUNT,
    C_DPAD = 100, C_LSTICK, C_RSTICK, C_EDIT,
    C_E_MINUS = 200, C_E_PLUS, C_E_RESET, C_E_DONE, C_E_GROUP    /* edit mode: its buttons, a group (+ index) */
};

/* the groups that move together */
enum { G_FACE, G_BW, G_DPAD, G_STICK, G_LT, G_RT, G_BACK, G_START, G_COUNT };
static const char *const k_group_name[G_COUNT] = { "face", "bw", "dpad", "stick", "lt", "rt", "back", "start" };

typedef struct { float x, y, r; } Circle;
typedef struct { float x0, y0, x1, y1; } Box;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int   s_w, s_h;                    /* the screen, pixels */
static float s_u;                         /* one unit: a hundredth of the height (times the size setting) */
static Circle s_face[4], s_bw[2], s_dpad, s_lstick_home;
static Box    s_lt, s_rt, s_back, s_start, s_edit;
static int    s_visible = 1;
static int    s_down[C_COUNT];            /* buttons held, by control */
static int    s_dpad_bits;                /* XINPUT d-pad bits */
static float  s_lx, s_ly, s_rx, s_ry;     /* sticks, -1..1 (y up) */
static Circle s_lstick, s_rstick;         /* where each stick was started (r 0: not held) */
static float  s_lknob[2], s_rknob[2];
static unsigned s_version = 1;            /* bumped at each change: the picture is redrawn */

typedef struct { int32_t id; int control; float x0, y0, dx0, dy0; } Finger;
static Finger s_finger[10];

/* the layout: each group's move (units) and size */
static float s_gdx[G_COUNT], s_gdy[G_COUNT], s_gsize[G_COUNT] = { 1, 1, 1, 1, 1, 1, 1, 1 };
static int   s_editing, s_selected = -1;
static Box   s_ebtn[4];                   /* edit mode's - + RESET DONE */

/* The launcher's Controls tab ([Android] in buffy_settings.ini), read once. */
static int   s_enabled = 1, s_vibrate = 1;
static float s_scale = 1.0f, s_opacity = 1.0f, s_cam = 1.0f;
void android_vibrate(int ms);
const char *buffy_settings_path(void);

static void load_layout(const char *v)
{
    char name[16];
    float dx, dy, size;
    int n, g;
    while (v && *v) {
        if (sscanf(v, "%15[a-z]:%f:%f:%f%n", name, &dx, &dy, &size, &n) == 4) {
            for (g = 0; g < G_COUNT; g++)
                if (!strcmp(name, k_group_name[g])) {
                    s_gdx[g] = dx;
                    s_gdy[g] = dy;
                    s_gsize[g] = size < 40 ? 0.4f : size > 250 ? 2.5f : size / 100.0f;
                }
            v += n;
        }
        v = strchr(v, ',');
        if (v)
            v++;
    }
}

static void save_layout(void)
{
    char v[512];
    int g, o = 0;
    const char *ini = buffy_settings_path();
    for (g = 0; g < G_COUNT; g++)
        o += snprintf(v + o, sizeof v - (size_t)o, "%s%s:%.1f:%.1f:%d", g ? "," : "", k_group_name[g], s_gdx[g], s_gdy[g],
                      (int)lrintf(s_gsize[g] * 100.0f));
    if (ini && *ini)
        WritePrivateProfileStringA("Android", "TouchLayout", v, ini);
    fprintf(stderr, "  [TOUCH] layout saved: %s\n", v);
}

static void settings(void)
{
    static int done;
    char layout_v[512];
    const char *ini = buffy_settings_path();
    if (done || !ini || !*ini)
        return;
    done = 1;
    s_enabled = GetPrivateProfileIntA("Android", "TouchControls", 1, ini) != 0;
    s_vibrate = GetPrivateProfileIntA("Android", "TouchVibrate", 1, ini) != 0;
    s_scale = (float)GetPrivateProfileIntA("Android", "TouchScale", 100, ini) / 100.0f;
    s_opacity = (float)GetPrivateProfileIntA("Android", "TouchOpacity", 100, ini) / 100.0f;
    s_cam = (float)GetPrivateProfileIntA("Android", "TouchCameraSpeed", 100, ini) / 100.0f;
    GetPrivateProfileStringA("Android", "TouchLayout", "", layout_v, sizeof layout_v, ini);
    load_layout(layout_v);
    if (s_scale < 0.5f || s_scale > 2.0f) s_scale = 1.0f;
    if (s_opacity < 0.2f || s_opacity > 2.0f) s_opacity = 1.0f;
    if (s_cam < 0.3f || s_cam > 3.0f) s_cam = 1.0f;
    if (getenv("BUFFY_TOUCH_EDIT") && getenv("BUFFY_TOUCH_EDIT")[0] == '1')
        s_enabled = s_editing = 1;                   /* (the launcher's "Edit the layout") */
    if (!s_enabled)
        s_visible = 0;
    fprintf(stderr, "  [TOUCH] %s, size %d%%, opacity %d%%, camera %d%%%s\n", s_enabled ? "on" : "off",
            (int)(s_scale * 100), (int)(s_opacity * 100), (int)(s_cam * 100), s_editing ? ", editing" : "");
}

/* ── layout ── */

static void move_circle(Circle *c, float cx, float cy, int g)
{
    float u = s_u, k = s_gsize[g];
    c->x = cx + (c->x - cx) * k + s_gdx[g] * u;
    c->y = cy + (c->y - cy) * k + s_gdy[g] * u;
    c->r *= k;
}

static void move_box(Box *b, int g)
{
    float cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2, u = s_u, k = s_gsize[g];
    float hw = (b->x1 - b->x0) / 2 * k, hh = (b->y1 - b->y0) / 2 * k;
    cx += s_gdx[g] * u;
    cy += s_gdy[g] * u;
    *b = (Box){ cx - hw, cy - hh, cx + hw, cy + hh };
}

static void layout_now(void)
{
    float u = s_u, W = (float)s_w, H = (float)s_h;
    int i;
    static const float face_dx[4] = { 0, 1, -1, 0 }, face_dy[4] = { 1, 0, 0, -1 };   /* A B X Y */
    float fx = W - 22 * u, fy = H - 32 * u;
    for (i = 0; i < 4; i++) {
        s_face[i] = (Circle){ fx + face_dx[i] * 12 * u, fy + face_dy[i] * 12 * u, 7 * u };
        move_circle(&s_face[i], fx, fy, G_FACE);
    }
    s_bw[0] = (Circle){ W - 37 * u, H - 56 * u, 4.5f * u };     /* Black */
    s_bw[1] = (Circle){ W - 47 * u, H - 50 * u, 4.5f * u };     /* White */
    for (i = 0; i < 2; i++)
        move_circle(&s_bw[i], W - 42 * u, H - 53 * u, G_BW);
    s_dpad = (Circle){ 22 * u, H - 60 * u, 14 * u };
    move_circle(&s_dpad, s_dpad.x, s_dpad.y, G_DPAD);
    s_lstick_home = (Circle){ 26 * u, H - 26 * u, 15 * u };
    move_circle(&s_lstick_home, s_lstick_home.x, s_lstick_home.y, G_STICK);
    s_lt = (Box){ 5 * u, 4 * u, 27 * u, 14 * u };
    move_box(&s_lt, G_LT);
    s_rt = (Box){ W - 27 * u, 4 * u, W - 5 * u, 14 * u };
    move_box(&s_rt, G_RT);
    s_back = (Box){ W / 2 - 22 * u, 3 * u, W / 2 - 4 * u, 10 * u };
    move_box(&s_back, G_BACK);
    s_start = (Box){ W / 2 + 4 * u, 3 * u, W / 2 + 22 * u, 10 * u };
    move_box(&s_start, G_START);
    s_edit = (Box){ W / 2 - 6 * u, 12 * u, W / 2 + 6 * u, 17 * u };
    /* edit mode's buttons, top middle */
    s_ebtn[0] = (Box){ W / 2 - 34 * u, 20 * u, W / 2 - 24 * u, 29 * u };   /* - */
    s_ebtn[1] = (Box){ W / 2 - 22 * u, 20 * u, W / 2 - 12 * u, 29 * u };   /* + */
    s_ebtn[2] = (Box){ W / 2 - 9 * u, 20 * u, W / 2 + 11 * u, 29 * u };    /* RESET */
    s_ebtn[3] = (Box){ W / 2 + 14 * u, 20 * u, W / 2 + 34 * u, 29 * u };   /* DONE */
    s_version++;
}

static void layout(int w, int h)
{
    if (w == s_w && h == s_h)
        return;
    s_w = w;
    s_h = h;
    s_u = (float)h / 100.0f * s_scale;
    layout_now();
}

static Box circle_box(const Circle *c) { return (Box){ c->x - c->r, c->y - c->r, c->x + c->r, c->y + c->r }; }

static Box join(Box a, Box b)
{
    return (Box){ fminf(a.x0, b.x0), fminf(a.y0, b.y0), fmaxf(a.x1, b.x1), fmaxf(a.y1, b.y1) };
}

/* a group's outline (edit mode) */
static Box group_box(int g)
{
    Box b;
    int i;
    switch (g) {
    case G_FACE:
        b = circle_box(&s_face[0]);
        for (i = 1; i < 4; i++) b = join(b, circle_box(&s_face[i]));
        return b;
    case G_BW:    return join(circle_box(&s_bw[0]), circle_box(&s_bw[1]));
    case G_DPAD:  return circle_box(&s_dpad);
    case G_STICK: return circle_box(&s_lstick_home);
    case G_LT:    return s_lt;
    case G_RT:    return s_rt;
    case G_BACK:  return s_back;
    default:      return s_start;
    }
}

static int in_circle(const Circle *c, float x, float y, float slack)
{
    float dx = x - c->x, dy = y - c->y, r = c->r * slack;
    return dx * dx + dy * dy <= r * r;
}

static int in_box(const Box *b, float x, float y, float slack)
{
    float m = (slack - 1.0f) * s_u * 3;
    return x >= b->x0 - m && x <= b->x1 + m && y >= b->y0 - m && y <= b->y1 + m;
}

/* the control a finger landing at x, y takes */
static int hit(float x, float y)
{
    int i;
    if (s_editing) {
        for (i = 0; i < 4; i++)
            if (in_box(&s_ebtn[i], x, y, 1.1f))
                return C_E_MINUS + i;
        for (i = G_COUNT - 1; i >= 0; i--) {
            Box b = group_box(i);
            if (in_box(&b, x, y, 1.3f))
                return C_E_GROUP + i;
        }
        return C_NONE;
    }
    for (i = 0; i < 4; i++)
        if (in_circle(&s_face[i], x, y, 1.25f))
            return C_A + i;
    for (i = 0; i < 2; i++)
        if (in_circle(&s_bw[i], x, y, 1.3f))
            return C_BLACK + i;
    if (in_box(&s_lt, x, y, 1.2f))
        return C_LT;
    if (in_box(&s_rt, x, y, 1.2f))
        return C_RT;
    if (in_box(&s_back, x, y, 1.3f))
        return C_BACK;
    if (in_box(&s_start, x, y, 1.3f))
        return C_START;
    if (in_box(&s_edit, x, y, 1.1f))
        return C_EDIT;
    if (in_circle(&s_dpad, x, y, 1.15f))
        return C_DPAD;
    if (in_circle(&s_lstick_home, x, y, 1.4f) || (x < s_w * 0.45f && y > s_h * 0.45f))
        return C_LSTICK;
    if (x > s_w * 0.5f && y > s_h * 0.25f)
        return C_RSTICK;
    return C_NONE;
}

static void stick_move(Circle *c, float x, float y, float *sx, float *sy, float *knob)
{
    float dx = (x - c->x) / c->r, dy = (y - c->y) / c->r, len = sqrtf(dx * dx + dy * dy);
    if (len > 1.0f) {
        dx /= len;
        dy /= len;
    }
    *sx = dx;
    *sy = -dy;
    knob[0] = c->x + dx * c->r;
    knob[1] = c->y + dy * c->r;
}

static void dpad_at(float x, float y)
{
    float dx = x - s_dpad.x, dy = y - s_dpad.y;
    int bits = 0;
    if (fabsf(dx) > s_dpad.r * 0.25f || fabsf(dy) > s_dpad.r * 0.25f) {
        /* eight ways: a direction counts within 67.5 degrees of it */
        float a = atan2f(-dy, dx) * 57.29578f;
        if (a > -67.5f && a < 67.5f) bits |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (a > 22.5f && a < 157.5f) bits |= XINPUT_GAMEPAD_DPAD_UP;
        if (a > 112.5f || a < -112.5f) bits |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (a < -22.5f && a > -157.5f) bits |= XINPUT_GAMEPAD_DPAD_DOWN;
    }
    if (bits != s_dpad_bits) {
        s_dpad_bits = bits;
        s_version++;
    }
}

static void all_up(void);

/* edit mode: a button pressed */
static void edit_button(int c)
{
    switch (c) {
    case C_E_MINUS:
    case C_E_PLUS:
        if (s_selected >= 0) {
            float k = s_gsize[s_selected] + (c == C_E_PLUS ? 0.1f : -0.1f);
            s_gsize[s_selected] = k < 0.4f ? 0.4f : k > 2.5f ? 2.5f : k;
        }
        break;
    case C_E_RESET:
        memset(s_gdx, 0, sizeof s_gdx);
        memset(s_gdy, 0, sizeof s_gdy);
        for (c = 0; c < G_COUNT; c++)
            s_gsize[c] = 1.0f;
        s_selected = -1;
        break;
    case C_E_DONE:
        save_layout();
        s_editing = 0;
        s_selected = -1;
        all_up();
        break;
    }
    layout_now();
}

static void finger_down(int32_t id, float x, float y)
{
    int i, c = hit(x, y);
    for (i = 0; i < 10 && s_finger[i].control != C_NONE && s_finger[i].id != id; i++)
        ;
    if (i == 10)
        return;
    s_finger[i].id = id;
    s_finger[i].control = c;
    s_finger[i].x0 = x;
    s_finger[i].y0 = y;
    if (c >= C_E_GROUP) {
        s_selected = c - C_E_GROUP;
        s_finger[i].dx0 = s_gdx[s_selected];
        s_finger[i].dy0 = s_gdy[s_selected];
    } else if (c >= C_E_MINUS) {
        edit_button(c);
        if (s_vibrate)
            android_vibrate(12);
    } else if (c == C_EDIT) {
        s_editing = 1;
        s_selected = -1;
        memset(s_down, 0, sizeof s_down);
        s_dpad_bits = 0;
        s_lx = s_ly = s_rx = s_ry = 0;
        s_lstick.r = s_rstick.r = 0;
    } else if (c >= 0 && c < C_COUNT) {
        s_down[c]++;
        if (s_vibrate)
            android_vibrate(12);
    } else if (c == C_DPAD) {
        dpad_at(x, y);
    } else if (c == C_LSTICK) {
        s_lstick.x = x; s_lstick.y = y; s_lstick.r = 13 * s_u * s_gsize[G_STICK];
        stick_move(&s_lstick, x, y, &s_lx, &s_ly, s_lknob);
    } else if (c == C_RSTICK) {
        s_rstick.x = x; s_rstick.y = y; s_rstick.r = 10 * s_u / s_cam;
        stick_move(&s_rstick, x, y, &s_rx, &s_ry, s_rknob);
    }
    s_version++;
}

static void finger_move(int32_t id, float x, float y)
{
    int i;
    for (i = 0; i < 10; i++) {
        int c = s_finger[i].control;
        if (c == C_NONE || s_finger[i].id != id)
            continue;
        if (c >= C_E_GROUP) {
            int g = c - C_E_GROUP;
            s_gdx[g] = s_finger[i].dx0 + (x - s_finger[i].x0) / s_u;
            s_gdy[g] = s_finger[i].dy0 + (y - s_finger[i].y0) / s_u;
            layout_now();
        } else if (c == C_DPAD) {
            dpad_at(x, y);
        } else if (c == C_LSTICK) {
            stick_move(&s_lstick, x, y, &s_lx, &s_ly, s_lknob);
            s_version++;
        } else if (c == C_RSTICK) {
            stick_move(&s_rstick, x, y, &s_rx, &s_ry, s_rknob);
            s_version++;
        }
    }
}

static void finger_up(int32_t id)
{
    int i;
    for (i = 0; i < 10; i++) {
        int c = s_finger[i].control;
        if (c == C_NONE || s_finger[i].id != id)
            continue;
        if (c >= 0 && c < C_COUNT && s_down[c] > 0)
            s_down[c]--;
        else if (c == C_DPAD)
            s_dpad_bits = 0;
        else if (c == C_LSTICK)
            s_lx = s_ly = 0, s_lstick.r = 0;
        else if (c == C_RSTICK)
            s_rx = s_ry = 0, s_rstick.r = 0;
        s_finger[i].control = C_NONE;
        s_version++;
    }
}

static void all_up(void)
{
    int i;
    for (i = 0; i < 10; i++)
        s_finger[i].control = C_NONE;
    memset(s_down, 0, sizeof s_down);
    s_dpad_bits = 0;
    s_lx = s_ly = s_rx = s_ry = 0;
    s_lstick.r = s_rstick.r = 0;
    s_version++;
}

/* A touch-screen event (android_input.c); nonzero when it was one. */
int android_touch_event(const AInputEvent *e)
{
    int32_t act, idx, i, n;
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION || !(AInputEvent_getSource(e) & AINPUT_SOURCE_TOUCHSCREEN))
        return 0;
    pthread_mutex_lock(&s_lock);
    if (!s_w || !s_enabled) {
        pthread_mutex_unlock(&s_lock);
        return s_enabled;                           /* (not drawn yet: no layout; or switched off) */
    }
    if (!s_visible) {
        s_visible = 1;
        s_version++;
    }
    act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    idx = (AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    switch (act) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        finger_down(AMotionEvent_getPointerId(e, (size_t)idx), AMotionEvent_getX(e, (size_t)idx), AMotionEvent_getY(e, (size_t)idx));
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        n = (int32_t)AMotionEvent_getPointerCount(e);
        for (i = 0; i < n; i++)
            finger_move(AMotionEvent_getPointerId(e, (size_t)i), AMotionEvent_getX(e, (size_t)i), AMotionEvent_getY(e, (size_t)i));
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        finger_up(AMotionEvent_getPointerId(e, (size_t)idx));
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        all_up();
        break;
    }
    pthread_mutex_unlock(&s_lock);
    return 1;
}

/* A controller was used: the touch pad hides (and lets go). */
void android_touch_hide(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_visible && !s_editing) {
        all_up();
        s_visible = 0;
        s_version++;
    }
    pthread_mutex_unlock(&s_lock);
}

/* The touch pad's state, into pad 0's (android_input.c). */
void android_touch_merge(XINPUT_GAMEPAD *g)
{
    static const WORD bits[C_COUNT] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        0, 0, 0, 0, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
    };
    int i;
    pthread_mutex_lock(&s_lock);
    if (s_visible && !s_editing) {
        for (i = 0; i < C_COUNT; i++)
            if (s_down[i] && bits[i])
                g->wButtons |= bits[i];
        /* the Xbox's Black and White: the PC pad's left and right shoulders (buffy_input.c) */
        if (s_down[C_BLACK]) g->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (s_down[C_WHITE]) g->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        g->wButtons |= (WORD)s_dpad_bits;
        if (s_down[C_LT]) g->bLeftTrigger = 255;
        if (s_down[C_RT]) g->bRightTrigger = 255;
        if (s_lx || s_ly) {
            g->sThumbLX = (SHORT)lrintf(s_lx * 32767.0f);
            g->sThumbLY = (SHORT)lrintf(s_ly * 32767.0f);
        }
        if (s_rx || s_ry) {
            g->sThumbRX = (SHORT)lrintf(s_rx * 32767.0f);
            g->sThumbRY = (SHORT)lrintf(s_ry * 32767.0f);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

/* ── the picture ── */

#define WHITE  0xFFFFFFu
#define ACCENT 0xB3122Bu

static void pill(const Box *b, const char *label, float u, int down, float h)
{
    CvBox c = { b->x0, b->y0, b->x1, b->y1 };
    cv_rbox(&c, fminf(3.5f * u, (b->y1 - b->y0) / 2), 0, WHITE, down ? 0.45f : 0.12f);
    cv_rbox(&c, fminf(3.5f * u, (b->y1 - b->y0) / 2), 0.3f * u, WHITE, 0.45f);
    cv_text(label, (b->x0 + b->x1) / 2, (b->y0 + b->y1) / 2, h, WHITE, 0.9f);
}

static void paint(void)
{
    static const uint32_t face_rgb[4] = { 0x3EB34Au, 0xE03C31u, 0x2E7BD6u, 0xF1C40Fu };
    static const char *face_txt[4] = { "A", "B", "X", "Y" };
    float u = s_u;
    int i;
    cv_clear(s_editing ? 0x60000000u : 0);            /* edit mode: the game dimmed */
    if (!s_visible)
        return;
    for (i = 0; i < 4; i++) {
        int d = s_down[C_A + i] > 0;
        float t = s_face[i].r / (7 * u);
        cv_disc(s_face[i].x, s_face[i].y, s_face[i].r, 0, face_rgb[i], d ? 0.85f : 0.40f);
        cv_disc(s_face[i].x, s_face[i].y, s_face[i].r, 0.35f * u, WHITE, d ? 0.9f : 0.45f);
        cv_text(face_txt[i], s_face[i].x, s_face[i].y, 4.2f * u * t, WHITE, d ? 1.0f : 0.8f);
    }
    cv_disc(s_bw[0].x, s_bw[0].y, s_bw[0].r, 0, 0x101010u, s_down[C_BLACK] ? 0.9f : 0.55f);
    cv_disc(s_bw[0].x, s_bw[0].y, s_bw[0].r, 0.3f * u, WHITE, s_down[C_BLACK] ? 0.9f : 0.45f);
    cv_disc(s_bw[1].x, s_bw[1].y, s_bw[1].r, 0, WHITE, s_down[C_WHITE] ? 0.9f : 0.40f);
    pill(&s_lt, "LT", u, s_down[C_LT] > 0, 3.2f * u * s_gsize[G_LT]);
    pill(&s_rt, "RT", u, s_down[C_RT] > 0, 3.2f * u * s_gsize[G_RT]);
    pill(&s_back, "BACK", u, s_down[C_BACK] > 0, 2.8f * u * s_gsize[G_BACK]);
    pill(&s_start, "START", u, s_down[C_START] > 0, 2.8f * u * s_gsize[G_START]);
    if (!s_editing)
        pill(&s_edit, "EDIT", u * 0.6f, 0, 2.0f * u);
    /* d-pad */
    {
        float r = s_dpad.r, a = 4 * u * (r / (14 * u));
        cv_disc(s_dpad.x, s_dpad.y, r, 0, WHITE, 0.10f);
        cv_disc(s_dpad.x, s_dpad.y, r, 0.35f * u, WHITE, 0.40f);
        cv_arrow(s_dpad.x, s_dpad.y - 0.62f * r, a, 0, WHITE, (s_dpad_bits & XINPUT_GAMEPAD_DPAD_UP) ? 0.95f : 0.5f);
        cv_arrow(s_dpad.x, s_dpad.y + 0.62f * r, a, 1, WHITE, (s_dpad_bits & XINPUT_GAMEPAD_DPAD_DOWN) ? 0.95f : 0.5f);
        cv_arrow(s_dpad.x - 0.62f * r, s_dpad.y, a, 2, WHITE, (s_dpad_bits & XINPUT_GAMEPAD_DPAD_LEFT) ? 0.95f : 0.5f);
        cv_arrow(s_dpad.x + 0.62f * r, s_dpad.y, a, 3, WHITE, (s_dpad_bits & XINPUT_GAMEPAD_DPAD_RIGHT) ? 0.95f : 0.5f);
    }
    /* the move stick: where it was started, or its resting place */
    if (s_lstick.r > 0) {
        cv_disc(s_lstick.x, s_lstick.y, s_lstick.r, 0.35f * u, WHITE, 0.45f);
        cv_disc(s_lknob[0], s_lknob[1], 6.5f * u * s_gsize[G_STICK], 0, WHITE, 0.45f);
    } else {
        cv_disc(s_lstick_home.x, s_lstick_home.y, s_lstick_home.r, 0.35f * u, WHITE, 0.30f);
        cv_disc(s_lstick_home.x, s_lstick_home.y, 6.5f * u * s_gsize[G_STICK], 0, WHITE, 0.25f);
    }
    if (s_rstick.r > 0) {
        cv_disc(s_rstick.x, s_rstick.y, s_rstick.r, 0.3f * u, WHITE, 0.30f);
        cv_disc(s_rknob[0], s_rknob[1], 5 * u, 0, WHITE, 0.35f);
    }
    if (s_editing) {
        static const char *labels[4] = { "-", "+", "RESET", "DONE" };
        for (i = 0; i < G_COUNT; i++) {
            Box b = group_box(i);
            CvBox c = { b.x0 - u, b.y0 - u, b.x1 + u, b.y1 + u };
            cv_rbox(&c, 2 * u, 0.4f * u, i == s_selected ? ACCENT : WHITE, i == s_selected ? 1.0f : 0.5f);
        }
        for (i = 0; i < 4; i++) {
            CvBox c = { s_ebtn[i].x0, s_ebtn[i].y0, s_ebtn[i].x1, s_ebtn[i].y1 };
            int dim = i < 2 && s_selected < 0;
            cv_rbox(&c, 2.5f * u, 0, i == 3 ? ACCENT : 0x2A1F33u, dim ? 0.4f : 0.95f);
            cv_text(labels[i], (c.x0 + c.x1) / 2, (c.y0 + c.y1) / 2, 4 * u, WHITE, dim ? 0.4f : 1.0f);
        }
        cv_text(s_selected < 0 ? "DRAG A CONTROL TO MOVE IT, TAP ONE TO SIZE IT"
                               : "DRAG TO MOVE, - / + TO SIZE", (float)s_w / 2, 34 * u, 3 * u, WHITE, 0.9f);
    }
}

/* ── drawing it ── */

static CvGpu s_gpu;
static unsigned s_drawn_version;
static int s_painted_visible;

/* Over the back buffer, before present (nv2a_overlay.inc). */
void android_touch_draw(GpuSwapchain *sc)
{
    GpuTexture *bb;
    GpuTextureDesc bd;
    int show;
    if (!sc || !(bb = gpu_swapchain_begin(sc, 0)))
        return;
    gpu_texture_desc(bb, &bd);
    pthread_mutex_lock(&s_lock);
    settings();
    layout((int)bd.width, (int)bd.height);
    if (s_drawn_version != s_version || !s_gpu.tex || cv_w != (int)bd.width / 2) {
        if (cv_alloc((int)bd.width / 2, (int)bd.height / 2, 0.5f)) {
            paint();
            if (s_opacity != 1.0f && !s_editing) {
                int i, n = cv_w * cv_h;
                for (i = 0; i < n; i++) {
                    uint32_t p = cv_px[i], out = 0;
                    int k;
                    if (!p)
                        continue;
                    for (k = 0; k < 32; k += 8) {
                        float c = (float)((p >> k) & 255) * s_opacity;
                        out |= (uint32_t)(c > 255.0f ? 255.0f : c) << k;
                    }
                    cv_px[i] = out;
                }
            }
            cv_upload(&s_gpu);
        }
        s_drawn_version = s_version;
        s_painted_visible = s_visible;
    }
    show = s_painted_visible;
    pthread_mutex_unlock(&s_lock);
    if (show)
        cv_draw(&s_gpu, bb, bd.width, bd.height);
}
