/*
 * Game data preload: the disc's archives read once, in the background, so
 * Windows keeps them in its file cache.
 *
 * The game streams from Buffy\Binary\_bin_xb\BuildData\Filelist.0NN (about
 * 3.2 GB in all) as a level plays -- sounds, music, the level's sections --
 * with the reads on its own thread. From a hard disk a cold read took 150 to
 * 250 ms: a hitch in play. A thread at background priority (low I/O and
 * memory priority, so the game's own reads always go first) reads each
 * archive through from start to end; after that the game's reads come from
 * memory.
 *
 * [Game] PreloadData in buffy_settings.ini: 1 (the default) on, 0 off. It is
 * skipped on a PC with less than 8 GB of memory, where the cache would only
 * push other things out.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

const char *buffy_settings_path(void);

static DWORD WINAPI preload_thread(LPVOID arg)
{
    char dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    char *buf;
    unsigned long long total = 0;
    DWORD t0 = GetTickCount();
    int files = 0;
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    {
        const char *env = getenv("BUFFY_GAME_DIR");
        if (env && *env)
            strcpy_s(dir, MAX_PATH, env);
        else {
            char *slash;
            GetModuleFileNameA(NULL, dir, MAX_PATH);
            if ((slash = strrchr(dir, '\\')) != NULL)
                *slash = 0;
        }
    }
    strcat_s(dir, MAX_PATH, "\\Buffy\\Binary\\_bin_xb\\BuildData");
    sprintf_s(pat, MAX_PATH, "%s\\Filelist.0*", dir);
    if (!(buf = (char *)malloc(4 << 20)))
        return 0;
    f = FindFirstFileA(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            char path[MAX_PATH];
            HANDLE h;
            DWORD got;
            sprintf_s(path, MAX_PATH, "%s\\%s", dir, fd.cFileName);
            h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            while (ReadFile(h, buf, 4 << 20, &got, NULL) && got)
                total += got;
            CloseHandle(h);
            files++;
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    free(buf);
    fprintf(stderr, "[PRELOAD] %d archives, %.0f MB in the file cache, %.1f s\n", files, total / 1048576.0,
            (GetTickCount() - t0) / 1000.0);
    return 0;
}

/* Once, when the game is running (the first frame). */
void buffy_preload_start(void)
{
    static int started;
    const char *ini;
    MEMORYSTATUSEX ms;
    if (started)
        return;
    started = 1;
    ini = buffy_settings_path();
    if (ini && ini[0] && !GetPrivateProfileIntA("Game", "PreloadData", 1, ini))
        return;
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys < 8ull << 30) {
        fprintf(stderr, "[PRELOAD] skipped: %.1f GB of memory\n", ms.ullTotalPhys / 1073741824.0);
        return;
    }
    CloseHandle(CreateThread(NULL, 0, preload_thread, NULL, 0, NULL));
}
