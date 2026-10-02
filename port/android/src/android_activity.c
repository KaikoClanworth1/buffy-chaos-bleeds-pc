/* The activity's entry (the manifest's android.app.func_name): the native
 * app glue's ANativeActivity_onCreate, and on the activity's own (UI)
 * thread what has to be done there through Java:
 *
 *   - landscape only (either way up): Activity.setRequestedOrientation --
 *     besides the manifest's, which Android 16 may ignore on large screens
 *   - the whole screen: the status and navigation bars hidden (a swipe from
 *     the edge shows them for a moment), and the camera cut-out drawn into
 *
 * The bars are hidden again whenever the window gets focus back. */
#include <android/native_activity.h>
#include <jni.h>
#include <stdio.h>

void ANativeActivity_onCreate(ANativeActivity *activity, void *saved, size_t saved_size);   /* (the glue) */

static void (*s_glue_focus)(ANativeActivity *a, int focused);
static ANativeActivity *s_activity;

#define SCREEN_ORIENTATION_SENSOR_LANDSCAPE 6
#define LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS 3
#define BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE 2

static void clear(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

static void full_screen(ANativeActivity *a)
{
    JNIEnv *env = a->env;                       /* (callbacks run on the UI thread, with its env) */
    jobject act = a->clazz, win, ctl, lp;
    jclass c_act, c_win, c_ctl, c_types, c_lp;
    jmethodID m;
    jfieldID f;
    jint bars;

    c_act = (*env)->GetObjectClass(env, act);
    m = (*env)->GetMethodID(env, c_act, "setRequestedOrientation", "(I)V");
    if (m)
        (*env)->CallVoidMethod(env, act, m, SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    clear(env);

    m = (*env)->GetMethodID(env, c_act, "getWindow", "()Landroid/view/Window;");
    win = m ? (*env)->CallObjectMethod(env, act, m) : NULL;
    clear(env);
    if (!win)
        return;
    c_win = (*env)->GetObjectClass(env, win);

    /* into the cut-out */
    m = (*env)->GetMethodID(env, c_win, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
    lp = m ? (*env)->CallObjectMethod(env, win, m) : NULL;
    clear(env);
    if (lp) {
        c_lp = (*env)->GetObjectClass(env, lp);
        f = (*env)->GetFieldID(env, c_lp, "layoutInDisplayCutoutMode", "I");
        clear(env);
        if (f) {
            (*env)->SetIntField(env, lp, f, LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS);
            m = (*env)->GetMethodID(env, c_win, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
            if (m)
                (*env)->CallVoidMethod(env, win, m, lp);
            clear(env);
        }
    }

    /* the bars away */
    m = (*env)->GetMethodID(env, c_win, "setDecorFitsSystemWindows", "(Z)V");
    if (m)
        (*env)->CallVoidMethod(env, win, m, JNI_FALSE);
    clear(env);
    m = (*env)->GetMethodID(env, c_win, "getInsetsController", "()Landroid/view/WindowInsetsController;");
    ctl = m ? (*env)->CallObjectMethod(env, win, m) : NULL;
    clear(env);
    if (!ctl)
        return;
    c_types = (*env)->FindClass(env, "android/view/WindowInsets$Type");
    m = c_types ? (*env)->GetStaticMethodID(env, c_types, "systemBars", "()I") : NULL;
    bars = m ? (*env)->CallStaticIntMethod(env, c_types, m) : 0;
    clear(env);
    c_ctl = (*env)->GetObjectClass(env, ctl);
    m = (*env)->GetMethodID(env, c_ctl, "setSystemBarsBehavior", "(I)V");
    if (m)
        (*env)->CallVoidMethod(env, ctl, m, BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    clear(env);
    m = (*env)->GetMethodID(env, c_ctl, "hide", "(I)V");
    if (m && bars)
        (*env)->CallVoidMethod(env, ctl, m, bars);
    clear(env);
}

static void on_focus(ANativeActivity *a, int focused)
{
    if (focused)
        full_screen(a);
    if (s_glue_focus)
        s_glue_focus(a, focused);
}

/* A short buzz (the touch controls' presses), from any thread. */
void android_vibrate(int ms)
{
    static jobject vib;                         /* (a global reference, made once) */
    static jmethodID once;
    static jclass c_effect;
    static jmethodID make;
    JNIEnv *env = NULL;
    JavaVM *vm = s_activity ? s_activity->vm : NULL;
    if (!vm || ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK
                && (*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK))
        return;
    if (!vib) {
        jobject act = s_activity->clazz, v;
        jclass c_act = (*env)->GetObjectClass(env, act), c_vib;
        jmethodID m = (*env)->GetMethodID(env, c_act, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jstring name = (*env)->NewStringUTF(env, "vibrator");
        v = m ? (*env)->CallObjectMethod(env, act, m, name) : NULL;
        clear(env);
        if (!v)
            return;
        vib = (*env)->NewGlobalRef(env, v);
        c_vib = (*env)->GetObjectClass(env, v);
        c_effect = (jclass)(*env)->NewGlobalRef(env, (*env)->FindClass(env, "android/os/VibrationEffect"));
        make = (*env)->GetStaticMethodID(env, c_effect, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
        once = (*env)->GetMethodID(env, c_vib, "vibrate", "(Landroid/os/VibrationEffect;)V");
        clear(env);
    }
    if (vib && make && once) {
        jobject eff = (*env)->CallStaticObjectMethod(env, c_effect, make, (jlong)ms, (jint)-1);
        if (eff) {
            (*env)->CallVoidMethod(env, vib, once, eff);
            (*env)->DeleteLocalRef(env, eff);
        }
        clear(env);
    }
}

void buffy_activity_create(ANativeActivity *activity, void *saved, size_t saved_size)
{
    s_activity = activity;
    ANativeActivity_onCreate(activity, saved, saved_size);
    s_glue_focus = activity->callbacks->onWindowFocusChanged;
    activity->callbacks->onWindowFocusChanged = on_focus;
    full_screen(activity);
}
