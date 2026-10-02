/* Controllers on Android, as XInput pads (include/xinput.h).
 *
 * Game controllers (Bluetooth or USB) arrive as key and motion events on the
 * activity's input queue (android_main.c hands each to android_input_event).
 * The first controller seen is pad 0, the next pad 1, and so on; each keeps
 * an XINPUT_STATE the game's XInputGetState reads (buffy_input.c). */
#include <windows.h>
#include <xinput.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <math.h>
#include <pthread.h>

#define PADS 4

typedef struct {
    int32_t device;          /* Android input device id; 0 = free */
    XINPUT_STATE st;
} Pad;

static Pad s_pad[PADS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static Pad *pad_for(int32_t device)
{
    int i;
    for (i = 0; i < PADS; i++)
        if (s_pad[i].device == device)
            return &s_pad[i];
    for (i = 0; i < PADS; i++)
        if (!s_pad[i].device) {
            s_pad[i].device = device;
            memset(&s_pad[i].st, 0, sizeof s_pad[i].st);
            fprintf(stderr, "  [INPUT] controller (device %d) is pad %d\n", (int)device, i);
            return &s_pad[i];
        }
    return NULL;
}

static WORD key_bit(int32_t key)
{
    switch (key) {
    case AKEYCODE_BUTTON_A:      return XINPUT_GAMEPAD_A;
    case AKEYCODE_BUTTON_B:      return XINPUT_GAMEPAD_B;
    case AKEYCODE_BUTTON_X:      return XINPUT_GAMEPAD_X;
    case AKEYCODE_BUTTON_Y:      return XINPUT_GAMEPAD_Y;
    case AKEYCODE_BUTTON_L1:     return XINPUT_GAMEPAD_LEFT_SHOULDER;
    case AKEYCODE_BUTTON_R1:     return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    case AKEYCODE_BUTTON_THUMBL: return XINPUT_GAMEPAD_LEFT_THUMB;
    case AKEYCODE_BUTTON_THUMBR: return XINPUT_GAMEPAD_RIGHT_THUMB;
    case AKEYCODE_BUTTON_START:  return XINPUT_GAMEPAD_START;
    case AKEYCODE_BUTTON_SELECT: return XINPUT_GAMEPAD_BACK;
    case AKEYCODE_BACK:          return XINPUT_GAMEPAD_BACK;
    case AKEYCODE_DPAD_UP:       return XINPUT_GAMEPAD_DPAD_UP;
    case AKEYCODE_DPAD_DOWN:     return XINPUT_GAMEPAD_DPAD_DOWN;
    case AKEYCODE_DPAD_LEFT:     return XINPUT_GAMEPAD_DPAD_LEFT;
    case AKEYCODE_DPAD_RIGHT:    return XINPUT_GAMEPAD_DPAD_RIGHT;
    default:                     return 0;
    }
}

static SHORT stick(float v, int invert)
{
    if (invert)
        v = -v;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (SHORT)lrintf(v * 32767.0f);
}

static BYTE trigger(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (BYTE)lrintf(v * 255.0f);
}

int  android_touch_event(const AInputEvent *e);
void android_touch_hide(void);
void android_touch_merge(XINPUT_GAMEPAD *g);

/* Android's Back (the gesture, the button): the game's Start, so it opens
 * the pause menu rather than closing the app. Held while Back is held. */
static volatile int s_back_down;

/* One input event; nonzero when it was used (a touch, a controller's, Back). */
int android_input_event(const AInputEvent *e)
{
    int32_t src = AInputEvent_getSource(e), type = AInputEvent_getType(e);
    int handled = 0;
    Pad *p;
    if (android_touch_event(e))
        return 1;
    if (type == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(e) == AKEYCODE_BACK
            && !(src & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK))) {
        s_back_down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
        return 1;
    }
    if (!(src & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK | AINPUT_SOURCE_DPAD)))
        return 0;
    android_touch_hide();                           /* a controller in use: the touch pad goes */
    pthread_mutex_lock(&s_lock);
    p = pad_for(AInputEvent_getDeviceId(e));
    if (p && type == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(e), act = AKeyEvent_getAction(e);
        WORD bit = key_bit(key);
        if (key == AKEYCODE_BUTTON_L2 || key == AKEYCODE_BUTTON_R2) {
            /* digital triggers */
            BYTE *t = key == AKEYCODE_BUTTON_L2 ? &p->st.Gamepad.bLeftTrigger : &p->st.Gamepad.bRightTrigger;
            *t = act == AKEY_EVENT_ACTION_DOWN ? 255 : 0;
            handled = 1;
        } else if (bit) {
            if (act == AKEY_EVENT_ACTION_DOWN)
                p->st.Gamepad.wButtons |= bit;
            else if (act == AKEY_EVENT_ACTION_UP)
                p->st.Gamepad.wButtons &= (WORD)~bit;
            handled = 1;
        }
        if (handled)
            p->st.dwPacketNumber++;
    } else if (p && type == AINPUT_EVENT_TYPE_MOTION) {
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        float lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0);
        float rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        WORD hat = 0;
        if (lt <= 0.0f) lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0);
        if (rt <= 0.0f) rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
        p->st.Gamepad.sThumbLX = stick(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0), 0);
        p->st.Gamepad.sThumbLY = stick(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0), 1);
        p->st.Gamepad.sThumbRX = stick(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0), 0);
        p->st.Gamepad.sThumbRY = stick(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0), 1);
        p->st.Gamepad.bLeftTrigger = trigger(lt);
        p->st.Gamepad.bRightTrigger = trigger(rt);
        /* a hat d-pad (many controllers report the d-pad as axes) */
        if (hx < -0.5f) hat |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (hx > 0.5f)  hat |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (hy < -0.5f) hat |= XINPUT_GAMEPAD_DPAD_UP;
        if (hy > 0.5f)  hat |= XINPUT_GAMEPAD_DPAD_DOWN;
        p->st.Gamepad.wButtons = (WORD)((p->st.Gamepad.wButtons & ~0x000Fu) | hat);
        p->st.dwPacketNumber++;
        handled = 1;
    }
    pthread_mutex_unlock(&s_lock);
    return handled;
}

/* A controller went away (android_main.c, on its device being removed). */
void android_input_device_removed(int32_t device)
{
    int i;
    pthread_mutex_lock(&s_lock);
    for (i = 0; i < PADS; i++)
        if (s_pad[i].device == device)
            s_pad[i].device = 0;
    pthread_mutex_unlock(&s_lock);
}

/* Pad 0 is always there: the touch pad, and the first controller with it. */
DWORD android_XInputGetState(DWORD user, XINPUT_STATE *s)
{
    DWORD r = ERROR_DEVICE_NOT_CONNECTED;
    if (user >= PADS)
        return r;
    pthread_mutex_lock(&s_lock);
    if (s_pad[user].device) {
        *s = s_pad[user].st;
        r = ERROR_SUCCESS;
    } else if (user == 0) {
        memset(s, 0, sizeof *s);
        r = ERROR_SUCCESS;
    }
    pthread_mutex_unlock(&s_lock);
    if (user == 0) {
        static DWORD packet;
        android_touch_merge(&s->Gamepad);
        if (s_back_down)
            s->Gamepad.wButtons |= XINPUT_GAMEPAD_START;
        s->dwPacketNumber = ++packet;
    }
    return r;
}

DWORD android_XInputSetState(DWORD user, XINPUT_VIBRATION *v)
{
    (void)v;
    return user < PADS && s_pad[user].device ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
