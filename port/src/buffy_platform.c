/**
 * What the game runs on: Windows, or Wine / Proton on Linux (the Steam Deck).
 * Logged at start-up and in bug reports, so reports say where they came from.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffy_platform.h"

static HMODULE ntdll(void)
{
    return GetModuleHandleW(L"ntdll.dll");
}

/* Wine or Proton (ntdll exports wine_get_version). */
int buffy_is_wine(void)
{
    static int wine = -1;
    if (wine < 0)
        wine = ntdll() && GetProcAddress(ntdll(), "wine_get_version") != NULL;
    return wine;
}

/* A Steam Deck (Steam sets SteamDeck=1 for what it starts there). */
int buffy_on_steam_deck(void)
{
    const char *v = getenv("SteamDeck");
    return v && !strcmp(v, "1");
}

/* "Windows 10.0.26200", "Wine 9.0 on Linux 6.5.0 (Proton) (Steam Deck)", ... */
const char *buffy_platform_text(void)
{
    static char s[160];
    if (s[0])
        return s;
    if (buffy_is_wine()) {
        typedef const char *(__cdecl *WineVersion)(void);
        typedef void (__cdecl *HostVersion)(const char **sysname, const char **release);
        WineVersion version = (WineVersion)GetProcAddress(ntdll(), "wine_get_version");
        HostVersion host = (HostVersion)GetProcAddress(ntdll(), "wine_get_host_version");
        const char *sysname = NULL, *release = NULL;
        if (host)
            host(&sysname, &release);
        sprintf_s(s, sizeof s, "Wine %s%s%s%s%s%s", version ? version() : "?", sysname ? " on " : "",
                  sysname ? sysname : "", release ? " " : "", release ? release : "",
                  getenv("STEAM_COMPAT_DATA_PATH") ? " (Proton)" : "");
    } else {
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
        RtlGetVersionFn get = (RtlGetVersionFn)GetProcAddress(ntdll(), "RtlGetVersion");
        OSVERSIONINFOW v;
        memset(&v, 0, sizeof v);
        v.dwOSVersionInfoSize = sizeof v;
        if (get && get(&v) == 0)
            sprintf_s(s, sizeof s, "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        else
            strcpy_s(s, sizeof s, "Windows");
    }
    if (buffy_on_steam_deck())
        strcat_s(s, sizeof s, " (Steam Deck)");
    return s;
}
