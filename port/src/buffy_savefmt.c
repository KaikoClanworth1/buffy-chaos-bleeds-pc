/**
 * The saves' format on the PC (see buffy_savefmt.h), and the conversion from
 * the Xbox layout that versions before 0.4 used:
 *
 *   UDATA\56550005\13F0104591D5\SaveMeta.xbx   "Name=BUFFY A" (UTF-16)
 *   UDATA\56550005\13F0104591D5\BUFFY A        the game's save (1 KB)
 *   UDATA\56550005\TitleMeta.xbx, SaveImage.xbx, TitleImage.xbx
 *
 * becomes SaveData\BUFFY A.sav: the same bytes, the same date.
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#include "buffy_savefmt.h"

#define TITLE_ID L"56550005"

void savefmt_file_name(const WCHAR *name, WCHAR *out, int n)
{
    int i, k = 0;
    for (i = 0; name[i] && k < n - 5; i++) {
        WCHAR c = name[i];
        out[k++] = (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    }
    while (k > 0 && (out[k - 1] == L' ' || out[k - 1] == L'.'))
        k--;                                            /* Windows drops these */
    if (!k)
        out[k++] = L'_';
    wcscpy_s(out + k, (size_t)(n - k), L".sav");
}

/* The save's name from SaveMeta.xbx ("Name=..."), or "". */
static void meta_name(const WCHAR *meta, WCHAR *name, int n)
{
    FILE *f;
    WCHAR buf[256] = L"", *p;
    size_t got;
    name[0] = 0;
    if (_wfopen_s(&f, meta, L"rb") || !f)
        return;
    got = fread(buf, sizeof(WCHAR), 255, f);
    fclose(f);
    buf[got] = 0;
    if ((p = wcsstr(buf, L"Name=")) != NULL) {
        wcsncpy_s(name, (size_t)n, p + 5, _TRUNCATE);
        if ((p = wcspbrk(name, L"\r\n")) != NULL)
            *p = 0;
    }
}

int savefmt_convert_xbox(const WCHAR *title_dir, const WCHAR *save_dir, int overwrite)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, bad = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\*", title_dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    CreateDirectoryW(save_dir, NULL);
    do {
        WCHAR dir[MAX_PATH], meta[MAX_PATH], name[128], data[MAX_PATH], file[160], dst[MAX_PATH];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
            continue;
        swprintf_s(dir, MAX_PATH, L"%s\\%s", title_dir, fd.cFileName);
        swprintf_s(meta, MAX_PATH, L"%s\\SaveMeta.xbx", dir);
        meta_name(meta, name, 128);
        if (!name[0])
            continue;                                   /* not a save */
        /* the data: the file named as the save, else the one that is not .xbx */
        swprintf_s(data, MAX_PATH, L"%s\\%s", dir, name);
        if (GetFileAttributesW(data) == INVALID_FILE_ATTRIBUTES) {
            WIN32_FIND_DATAW d;
            HANDLE g;
            data[0] = 0;
            swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
            g = FindFirstFileW(pat, &d);
            if (g != INVALID_HANDLE_VALUE) {
                do {
                    size_t l = wcslen(d.cFileName);
                    if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                            && !(l > 4 && !_wcsicmp(d.cFileName + l - 4, L".xbx"))) {
                        swprintf_s(data, MAX_PATH, L"%s\\%s", dir, d.cFileName);
                        break;
                    }
                } while (FindNextFileW(g, &d));
                FindClose(g);
            }
            if (!data[0])
                continue;
        }
        savefmt_file_name(name, file, 160);
        swprintf_s(dst, MAX_PATH, L"%s\\%s", save_dir, file);
        if (!overwrite && GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES)
            continue;                                   /* already converted */
        if (CopyFileW(data, dst, FALSE))                /* (keeps the date) */
            n++;
        else
            bad = 1;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return bad ? -1 : n;
}

static int is_dir(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* SaveData\ as the emulated console's folder (versions before 0.4): its
 * folders move to XboxData\; its partition images, empty scratch the game
 * never wrote anything of the player's into (the port stopped using them),
 * are deleted. */
static void move_console_files(const WCHAR *exe_dir)
{
    static const WCHAR *items[] = { L"Partition0.img", L"Partition1.img", L"Partition2.img", L"Partition3.img",
                                    L"Partition4.img", L"Partition5.img", L"Cache", L"SystemData", L"TitleData",
                                    L"UserData" };
    WCHAR sd[MAX_PATH], xd[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    int i;
    swprintf_s(sd, MAX_PATH, L"%s\\SaveData", exe_dir);
    swprintf_s(xd, MAX_PATH, L"%s\\XboxData", exe_dir);
    swprintf_s(a, MAX_PATH, L"%s\\Partition0.img", sd);
    if (GetFileAttributesW(a) == INVALID_FILE_ATTRIBUTES)
        return;                                         /* SaveData already holds saves */
    CreateDirectoryW(xd, NULL);
    for (i = 0; i < (int)(sizeof items / sizeof items[0]); i++) {
        swprintf_s(a, MAX_PATH, L"%s\\%s", sd, items[i]);
        swprintf_s(b, MAX_PATH, L"%s\\%s", xd, items[i]);
        if (GetFileAttributesW(a) == INVALID_FILE_ATTRIBUTES)
            continue;
        if (i < 6) {
            DeleteFileW(a);
            continue;
        }
        if (!MoveFileExW(a, b, 0) && is_dir(a))
            RemoveDirectoryW(a);                        /* an empty one already there */
    }
}

int savefmt_migrate(const WCHAR *game_dir, const WCHAR *exe_dir, int move_originals)
{
    WCHAR title[MAX_PATH], save_dir[MAX_PATH];
    int n, i;
    move_console_files(exe_dir);
    for (i = 0; i < 6; i++) {
        /* images an earlier build of this version left in XboxData */
        WCHAR img[MAX_PATH];
        swprintf_s(img, MAX_PATH, L"%s\\XboxData\\Partition%d.img", exe_dir, i);
        DeleteFileW(img);
    }
    swprintf_s(save_dir, MAX_PATH, L"%s\\SaveData", exe_dir);
    CreateDirectoryW(save_dir, NULL);
    swprintf_s(title, MAX_PATH, L"%s\\UDATA\\" TITLE_ID, game_dir);
    if (!is_dir(title))
        return 0;
    n = savefmt_convert_xbox(title, save_dir, 0);
    if (n < 0 || !move_originals)
        return n;
    {
        /* The originals, out of the way but kept. */
        WCHAR bk[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
        SYSTEMTIME t;
        int k;
        GetLocalTime(&t);
        swprintf_s(bk, MAX_PATH, L"%s\\SaveBackups", game_dir);
        CreateDirectoryW(bk, NULL);
        swprintf_s(bk, MAX_PATH, L"%s\\SaveBackups\\%04u-%02u-%02u %02u.%02u.%02u (Xbox saves, before conversion)",
                   game_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        CreateDirectoryW(bk, NULL);
        for (k = 0; k < 2; k++) {
            const WCHAR *sub = k ? L"TDATA" : L"UDATA";
            swprintf_s(a, MAX_PATH, L"%s\\%s", game_dir, sub);
            swprintf_s(b, MAX_PATH, L"%s\\%s", bk, sub);
            if (is_dir(a))
                MoveFileExW(a, b, 0);
        }
    }
    return n;
}
