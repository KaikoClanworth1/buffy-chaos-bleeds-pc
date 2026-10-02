/* The Windows build's modules that have no Android counterpart yet, as the
 * game sees them when they are switched off:
 *
 *   buffy_movie.c     movies (Media Foundation): every movie is skipped, as
 *                     with BUFFY_SKIP_MOVIES -- the engine moves on
 *   buffy_export.c    the launcher's model export (WIC): off
 *   buffy_debug.c     the bug report: off (logcat has the log)
 *   buffy_profile.c   the sampling profiler (dbghelp): off
 *   buffy_xbecheck.c  the other release's exe hand-off: one release per APK
 *   buffy_platform.c  Wine / Steam Deck detection: neither */
#include <windows.h>
#include <sys/system_properties.h>
#include "recomp/gen/recomp_types.h"

static void ret_stdcall(uint32_t result, uint32_t arg_bytes)
{
    g_eax = result;
    g_esp += 4 + arg_bytes;
}

/* ── movies ── */
void XMVDecoder_CreateDecoderForFile_00166B8D(void)
{
    uint32_t file = MEM32(g_esp + 8), pp = MEM32(g_esp + 12);
    if (pp)
        MEM32(pp) = 0;
    fprintf(stderr, "  [MOVIE] %s: skipped (no movie player on Android yet)\n",
            file ? (const char *)XBOX_PTR(file) : "?");
    ret_stdcall(0x80004005u, 12);
}
void XMVDecoder_GetVideoDescriptor_001671B5(void)  { ret_stdcall(0, 8); }
void XMVDecoder_EnableAudioStream_00167222(void)   { ret_stdcall(0x80004005u, 20); }
void XMVDecoder_GetAudioStream_001673A1(void)      { ret_stdcall(0x80004005u, 12); }
void XMVDecoder_GetNextFrame_001673C0(void)        { ret_stdcall(0, 16); }
void XMVDecoder_CloseDecoder_001670D1(void)        { ret_stdcall(0, 4); }
int  buffy_movie_active(void)                      { return 0; }

/* ── the rest ── */
void buffy_export_frame(void) {}
void buffy_debug_start(void) {}
void buffy_profile_frame(void) {}
int  buffy_is_wine(void) { return 0; }
int  buffy_on_steam_deck(void) { return 0; }

int buffy_check_xbe_release(const uint8_t *d, size_t n, int *exit_code)
{
    (void)d;
    (void)n;
    *exit_code = 0;
    return 1;
}

const char *buffy_platform_text(void)
{
    static char text[256];
    char model[PROP_VALUE_MAX] = "", release[PROP_VALUE_MAX] = "", sdk[PROP_VALUE_MAX] = "", soc[PROP_VALUE_MAX] = "";
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.build.version.release", release);
    __system_property_get("ro.build.version.sdk", sdk);
    __system_property_get("ro.soc.model", soc);
    snprintf(text, sizeof text, "Android %s (API %s), %s, %s", release, sdk, model, soc);
    return text;
}
