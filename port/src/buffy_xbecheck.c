/**
 * Which release of the game is installed, and is this executable that one's?
 *
 * The port's code is a default.xbe's, translated -- one executable for each
 * release it supports: buffy_chaos_bleeds.exe the European (PAL) release,
 * version 2; buffy_chaos_bleeds_usa.exe the North American release, version 1
 * (built from the same source: tools/translate_release.py). Another release's
 * code is at other addresses, and with it the game stops at the first call
 * made through a pointer, before anything is drawn (GitHub issue 1: "Ready",
 * then nothing). So the XBE's certificate (title, region, version) and the
 * size of its code are checked; the other supported release's executable is
 * started in this one's place when it is beside it; anything else is said
 * plainly.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XBE_TITLE_ID 0x56550005u

typedef struct {
    uint32_t region, version, text;
    const char *name, *exe;
} Release;

static const Release k_releases[] = {
    { 0x4u, 2, 1196336u, "the European (PAL) release, version 2", "buffy_chaos_bleeds.exe" },
    { 0x3u, 1, 1193024u, "the North American release, version 1", "buffy_chaos_bleeds_usa.exe" },
};
#if defined(BUFFY_RELEASE_USA)
#define THIS_RELEASE 1
#else
#define THIS_RELEASE 0
#endif

/* Start `exe` (beside this one) with this process's command line and wait
 * for it: its exit code is this one's. 0 if it could not be started. */
static int run_instead(const char *exe, DWORD *code)
{
    char path[MAX_PATH], *slash, *cmd;
    const char *args = GetCommandLineA();
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    size_t n;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (!slash)
        return 0;
    strcpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), exe);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return 0;
    /* the arguments after this program's own name */
    if (*args == '"') {
        args = strchr(args + 1, '"');
        args = args ? args + 1 : "";
    } else {
        while (*args && *args != ' ')
            args++;
    }
    n = strlen(path) + strlen(args) + 4;
    cmd = (char *)malloc(n);
    if (!cmd)
        return 0;
    sprintf_s(cmd, n, "\"%s\"%s", path, args);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(path, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        free(cmd);
        return 0;
    }
    free(cmd);
    fprintf(stderr, "[XBE] started %s for this release\n", exe);
    fflush(stderr);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* 1 to go on; 0 when this process is to exit (the other release's
 * executable ran in its place, or the player was told; *exit_code says
 * with what). */
int buffy_check_xbe_release(const uint8_t *d, size_t n, int *exit_code)
{
    uint32_t base, cert, sect, title = 0, region = 0, version = 0, text = 0;
    int i, found = -1;
    char msg[1024];
    const Release *me = &k_releases[THIS_RELEASE];
    *exit_code = 1;
    if (n < 0x200)
        return 1;                                   /* (load_xbe has said what is wrong) */
    base = *(const uint32_t *)(d + 0x104);
    cert = *(const uint32_t *)(d + 0x118) - base;
    sect = *(const uint32_t *)(d + 0x120) - base;
    if ((size_t)cert + 0xB0 <= n) {
        title = *(const uint32_t *)(d + cert + 8);
        region = *(const uint32_t *)(d + cert + 0xA0);
        version = *(const uint32_t *)(d + cert + 0xAC);
    }
    if ((size_t)sect + 12 <= n)
        text = *(const uint32_t *)(d + sect + 8);
    for (i = 0; i < (int)(sizeof k_releases / sizeof k_releases[0]); i++)
        if (title == XBE_TITLE_ID && region == k_releases[i].region && version == k_releases[i].version
                && text == k_releases[i].text)
            found = i;
    fprintf(stderr, "[XBE] title %08X region %X version %u code %u bytes: %s\n", title, region, version, text,
            found == THIS_RELEASE ? me->name : found >= 0 ? k_releases[found].name : "not a supported release");
    if (found == THIS_RELEASE || getenv("BUFFY_ANY_XBE"))   /* (testing: BUFFY_ANY_XBE=1 tries anyway) */
        return 1;
    if (found >= 0) {
        DWORD code = 1;
        if (run_instead(k_releases[found].exe, &code)) {
            *exit_code = (int)code;
            return 0;
        }
        sprintf_s(msg, sizeof msg,
                  "These game files are from %s of Buffy the Vampire Slayer: Chaos Bleeds, which plays with "
                  "%s -- it is not beside this program.\n\nPlease reinstall the port (all of its files).",
                  k_releases[found].name, k_releases[found].exe);
    } else {
        sprintf_s(msg, sizeof msg,
                  "These game files are from a release of Buffy the Vampire Slayer: Chaos Bleeds this port "
                  "does not support (title %08X, region %X, version %u).\n\n"
                  "The port contains the code of %s and of %s, translated for Windows, so it runs with those "
                  "releases' files only.\n\nPlease install from one of those disc images.",
                  title, region, version, k_releases[0].name, k_releases[1].name);
    }
    fprintf(stderr, "[XBE] %s\n", msg);
    if (!getenv("BUFFY_NO_WINDOW"))
        MessageBoxA(NULL, msg, "Buffy the Vampire Slayer: Chaos Bleeds", MB_ICONERROR);
    return 0;
}
