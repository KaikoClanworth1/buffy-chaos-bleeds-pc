/**
 * Buffy the Vampire Slayer: Chaos Bleeds -- launcher.
 *
 * Four tabs, as the X-Men Legends launcher:
 *
 *   Play      the game folder, Play, close-on-start.
 *   Settings  display mode, resolution, vsync, skipping the intro movies --
 *             written to buffy_settings.ini in the game folder, which the
 *             game reads at start (port/src/buffy_settings.c).
 *   Mods      mods\<name>\mod.ini + files\ in the game folder; the ticked
 *             ones, in order, go to mods\.launcher\enabled.txt and the game
 *             reads their files instead of its own (port/src/buffy_mods.c).
 *   Install   the game from the disc image (ISO or XISO): its XDVDFS file
 *             system is read directly and every file copied out, the
 *             port's program files copied beside them, and the movies
 *             converted for PC playback when FFmpeg is available.
 *
 * Command line (tests):  --capture <play|settings|mods|textures|install> <file.bmp>
 *                        --install <image> <folder>   (no window; exit code)
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <urlmon.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define LAUNCHER_VERSION  L"1.1"
#define GAME_TITLE        L"Buffy the Vampire Slayer: Chaos Bleeds"
#define GAME_EXE          L"buffy_chaos_bleeds.exe"
#define TITLE_ID          0x56550005u   /* from the disc's default.xbe certificate */
#define FFMPEG_URL        L"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"

/* ── state ─────────────────────────────────────────────────────────────── */

enum { TAB_PLAY, TAB_SETTINGS, TAB_MODS, TAB_TEXTURES, TAB_INSTALL, TAB_COUNT };

enum {
    ID_TAB = 100,
    /* play */
    ID_GAMEDIR, ID_GAMEDIR_CHANGE, ID_PLAY, ID_CLOSE_ON_PLAY, ID_PLAY_STATUS, ID_VERSION,
    /* settings */
    ID_WINDOWED, ID_FULLSCREEN, ID_RESOLUTION, ID_VSYNC, ID_SKIP_INTRO, ID_WS_SAFE, ID_INVERT_X, ID_DEFAULTS, ID_SAVE,
    ID_SETTINGS_STATUS,
    /* mods */
    ID_MODLIST, ID_MOD_DESC, ID_MOD_UP, ID_MOD_DOWN, ID_MOD_OPEN, ID_MOD_REFRESH, ID_MOD_APPLY,
    ID_MOD_STATUS,
    /* textures */
    ID_TEX_LOAD, ID_TEX_PREFETCH, ID_TEX_DUMP, ID_TEX_OPEN_LOAD, ID_TEX_OPEN_DUMP, ID_TEX_REFRESH, ID_TEX_STATUS,
    /* install */
    ID_IMAGE, ID_IMAGE_BROWSE, ID_TARGET, ID_TARGET_BROWSE, ID_MOVIES, ID_FFMPEG_STATUS,
    ID_FFMPEG_GET, ID_INSTALL, ID_PROGRESS, ID_INSTALL_STATUS,
};

#define WM_APP_PROGRESS (WM_APP + 1)     /* wParam permille, lParam heap WCHAR* or 0 */
#define WM_APP_DONE     (WM_APP + 2)     /* wParam 1 ok / 0 failed, lParam heap WCHAR* */
#define WM_APP_GAMEEND  (WM_APP + 3)     /* wParam exit code */

static HINSTANCE s_inst;
static HWND      s_wnd, s_tab;
static HFONT     s_font, s_bold, s_big;
static int       s_dpi = 96;
static HICON     s_icon;
static HWND      s_ctl[TAB_COUNT][32];
static int       s_nctl[TAB_COUNT];
static int       s_cur_tab;
static WCHAR     s_launcher_dir[MAX_PATH], s_launcher_ini[MAX_PATH];
static WCHAR     s_game_dir[MAX_PATH];
static volatile LONG s_busy, s_cancel;
static HANDLE    s_game_proc;
static int       s_settings_dirty;

static const struct { int w, h; const WCHAR *label; } k_res[] = {
    { 1920, 1080, L"1920 \x00D7 1080  (1080p, widescreen)" },
    { 2560, 1440, L"2560 \x00D7 1440  (1440p, widescreen)" },
    { 3840, 2160, L"3840 \x00D7 2160  (4K, widescreen)" },
    { 1280,  720, L"1280 \x00D7 720  (720p, widescreen)" },
    {  640,  480, L"640 \x00D7 480  (4:3, original)" },
    { 1280,  960, L"1280 \x00D7 960  (4:3)" },
    { 1920, 1440, L"1920 \x00D7 1440  (4:3)" },
    { 2560, 1920, L"2560 \x00D7 1920  (4:3)" },
};
#define N_RES ((int)(sizeof k_res / sizeof k_res[0]))

static int S(int v) { return MulDiv(v, s_dpi, 96); }

/* ── small helpers ─────────────────────────────────────────────────────── */

static int file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void join(WCHAR *out, const WCHAR *a, const WCHAR *b)
{
    swprintf_s(out, MAX_PATH, L"%s\\%s", a, b);
}

static int mkdirs(const WCHAR *path)
{
    WCHAR tmp[MAX_PATH];
    WCHAR *p;
    wcscpy_s(tmp, MAX_PATH, path);
    for (p = tmp + 3; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            WCHAR c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static WCHAR *wdup(const WCHAR *s)
{
    size_t n = wcslen(s) + 1;
    WCHAR *d = (WCHAR *)malloc(n * sizeof(WCHAR));
    if (d)
        wcscpy_s(d, n, s);
    return d;
}

static void post_progress(int permille, const WCHAR *fmt, ...)
{
    WCHAR buf[512];
    va_list ap;
    if (!fmt) {
        PostMessageW(s_wnd, WM_APP_PROGRESS, (WPARAM)permille, 0);
        return;
    }
    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);
    PostMessageW(s_wnd, WM_APP_PROGRESS, (WPARAM)permille, (LPARAM)wdup(buf));
}

static HWND ctl(int id)
{
    return GetDlgItem(s_wnd, id);
}

static void set_text(int id, const WCHAR *t)
{
    SetWindowTextW(ctl(id), t);
}

static int is_game_folder(const WCHAR *dir)
{
    WCHAR a[MAX_PATH], b[MAX_PATH];
    join(a, dir, L"default.xbe");
    join(b, dir, L"Buffy\\Binary\\_bin_xb\\BuildData");
    return dir[0] && file_exists(a) && dir_exists(b);
}

static int has_program(const WCHAR *dir)
{
    WCHAR a[MAX_PATH];
    join(a, dir, GAME_EXE);
    return file_exists(a);
}

static void settings_path(WCHAR *out)
{
    join(out, s_game_dir, L"buffy_settings.ini");
}

/* ── launcher.ini ──────────────────────────────────────────────────────── */

static void load_launcher_ini(void)
{
    GetPrivateProfileStringW(L"Launcher", L"GameFolder", L"", s_game_dir, MAX_PATH, s_launcher_ini);
    if (!s_game_dir[0] || !is_game_folder(s_game_dir))
        if (is_game_folder(s_launcher_dir))
            wcscpy_s(s_game_dir, MAX_PATH, s_launcher_dir);
}

static void save_launcher_ini(void)
{
    WritePrivateProfileStringW(L"Launcher", L"GameFolder", s_game_dir, s_launcher_ini);
    WritePrivateProfileStringW(L"Launcher", L"CloseOnPlay",
        IsDlgButtonChecked(s_wnd, ID_CLOSE_ON_PLAY) == BST_CHECKED ? L"1" : L"0", s_launcher_ini);
}

/* ── icon: the game's own save-game image (128x128 DXT1 in an XPR0) ───── */

static void dxt1_block(const uint8_t *b, uint32_t out[16])
{
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    uint32_t pal[4], bits = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    int r0 = (c0 >> 11) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
    int r1 = (c1 >> 11) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
    int i;
    pal[0] = 0xFF000000u | r0 << 16 | g0 << 8 | b0;
    pal[1] = 0xFF000000u | r1 << 16 | g1 << 8 | b1;
    if (c0 > c1) {
        pal[2] = 0xFF000000u | ((2 * r0 + r1) / 3) << 16 | ((2 * g0 + g1) / 3) << 8 | (2 * b0 + b1) / 3;
        pal[3] = 0xFF000000u | ((r0 + 2 * r1) / 3) << 16 | ((g0 + 2 * g1) / 3) << 8 | (b0 + 2 * b1) / 3;
    } else {
        pal[2] = 0xFF000000u | ((r0 + r1) / 2) << 16 | ((g0 + g1) / 2) << 8 | (b0 + b1) / 2;
        pal[3] = 0;
    }
    for (i = 0; i < 16; i++)
        out[i] = pal[(bits >> (i * 2)) & 3];
}

static HICON load_game_icon(const WCHAR *game_dir)
{
    WCHAR p[MAX_PATH];
    HANDLE f;
    uint8_t buf[0x2800];
    DWORD got = 0;
    uint32_t *px;
    BITMAPV5HEADER bh;
    HBITMAP color, mask;
    ICONINFO ii;
    HICON ic = NULL;
    HDC dc;
    void *bits = NULL;
    int bx, by, i;

    join(p, game_dir, L"Buffy\\Binary\\_bin_xb\\buffytitle.xbx");
    f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    ReadFile(f, buf, sizeof buf, &got, NULL);
    CloseHandle(f);
    if (got < 0x2800 || memcmp(buf, "XPR0", 4) || ((buf[0x19]) != 0x0C))
        return NULL;
    memset(&bh, 0, sizeof bh);
    bh.bV5Size = sizeof bh;
    bh.bV5Width = 128;
    bh.bV5Height = -128;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    dc = GetDC(NULL);
    color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color)
        return NULL;
    px = (uint32_t *)bits;
    for (by = 0; by < 32; by++)
        for (bx = 0; bx < 32; bx++) {
            uint32_t blk[16];
            dxt1_block(buf + 0x800 + (by * 32 + bx) * 8, blk);
            for (i = 0; i < 16; i++)
                px[(by * 4 + i / 4) * 128 + bx * 4 + i % 4] = blk[i];
        }
    mask = CreateBitmap(128, 128, 1, 1, NULL);
    ii.fIcon = TRUE; ii.xHotspot = ii.yHotspot = 0; ii.hbmMask = mask; ii.hbmColor = color;
    ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

/* ── the disc image: XDVDFS ────────────────────────────────────────────── */

typedef struct {
    WCHAR    path[MAX_PATH];     /* relative, backslashes */
    uint32_t sector, size;
} DiscFile;

typedef struct {
    HANDLE    f;
    uint64_t  base;              /* partition offset in the image */
    DiscFile *files;
    int       count, cap;
    uint64_t  total;
} Disc;

static int disc_read(Disc *d, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)(d->base + off);
    return SetFilePointerEx(d->f, li, NULL, FILE_BEGIN) && ReadFile(d->f, buf, n, &got, NULL) && got == n;
}

static int disc_add(Disc *d, const WCHAR *path, uint32_t sector, uint32_t size)
{
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 256;
        DiscFile *n = (DiscFile *)realloc(d->files, sizeof(DiscFile) * (size_t)cap);
        if (!n)
            return 0;
        d->files = n;
        d->cap = cap;
    }
    wcscpy_s(d->files[d->count].path, MAX_PATH, path);
    d->files[d->count].sector = sector;
    d->files[d->count].size = size;
    d->count++;
    d->total += size;
    return 1;
}

static int disc_dir(Disc *d, uint32_t sector, uint32_t size, const WCHAR *prefix, int depth);

/* One node of a directory's binary tree (offset in dwords), in order. */
static int disc_node(Disc *d, const uint8_t *dir, uint32_t size, uint32_t off, const WCHAR *prefix,
                     int depth, int guard)
{
    uint16_t left, right;
    uint32_t sec, sz;
    uint8_t attr, nlen;
    WCHAR name[256], path[MAX_PATH];
    int i;

    if (guard > 4096 || (size_t)off * 4 + 14 > size)
        return 1;
    left = (uint16_t)(dir[off * 4] | dir[off * 4 + 1] << 8);
    right = (uint16_t)(dir[off * 4 + 2] | dir[off * 4 + 3] << 8);
    if (left == 0xFFFF && right == 0xFFFF)
        return 1;                                   /* padding */
    memcpy(&sec, dir + off * 4 + 4, 4);
    memcpy(&sz, dir + off * 4 + 8, 4);
    attr = dir[off * 4 + 12];
    nlen = dir[off * 4 + 13];
    if ((size_t)off * 4 + 14 + nlen > size)
        return 0;
    for (i = 0; i < nlen; i++)
        name[i] = (WCHAR)dir[off * 4 + 14 + i];
    name[nlen] = 0;
    if (!nlen || wcschr(name, L'\\') || wcschr(name, L'/') || !wcscmp(name, L"..") || !wcscmp(name, L"."))
        return 0;                                   /* unsafe or broken */
    if (left && !disc_node(d, dir, size, left, prefix, depth, guard + 1))
        return 0;
    swprintf_s(path, MAX_PATH, prefix[0] ? L"%s\\%s" : L"%s%s", prefix, name);
    if (attr & 0x10) {
        if (sz && !disc_dir(d, sec, sz, path, depth + 1))
            return 0;
    } else if (!disc_add(d, path, sec, sz)) {
        return 0;
    }
    if (right && !disc_node(d, dir, size, right, prefix, depth, guard + 1))
        return 0;
    return 1;
}

static int disc_dir(Disc *d, uint32_t sector, uint32_t size, const WCHAR *prefix, int depth)
{
    uint8_t *buf;
    int ok;
    if (depth > 32 || size > (16u << 20))
        return 0;
    buf = (uint8_t *)malloc(size);
    if (!buf)
        return 0;
    ok = disc_read(d, (uint64_t)sector * 2048, buf, size) && disc_node(d, buf, size, 0, prefix, depth, 0);
    free(buf);
    return ok;
}

static void disc_close(Disc *d)
{
    if (d->f && d->f != INVALID_HANDLE_VALUE)
        CloseHandle(d->f);
    free(d->files);
    memset(d, 0, sizeof *d);
}

/* Open an image and list its files; on failure *err explains why. */
static int disc_open(Disc *d, const WCHAR *image, WCHAR *err, size_t errn)
{
    static const uint64_t bases[] = { 0, 0x18300000ull, 0xFD90000ull, 0x2080000ull };
    uint8_t vd[2048];
    int i;
    memset(d, 0, sizeof *d);
    d->f = CreateFileW(image, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (d->f == INVALID_HANDLE_VALUE) {
        swprintf_s(err, errn, L"The disc image could not be opened (error %lu).", GetLastError());
        return 0;
    }
    for (i = 0; i < 4; i++) {
        d->base = bases[i];
        if (disc_read(d, 32 * 2048, vd, sizeof vd) && !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)
                && !memcmp(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20)) {
            uint32_t root, rsize;
            memcpy(&root, vd + 20, 4);
            memcpy(&rsize, vd + 24, 4);
            if (!disc_dir(d, root, rsize, L"", 0)) {
                swprintf_s(err, errn, L"The disc image is damaged: its file list could not be read.");
                return 0;
            }
            return 1;
        }
    }
    swprintf_s(err, errn, L"This is not an Xbox disc image. (A PlayStation 2 or GameCube image of the game "
                          L"will not work; the port needs the Xbox version.)");
    return 0;
}

static DiscFile *disc_find(Disc *d, const WCHAR *path)
{
    int i;
    for (i = 0; i < d->count; i++)
        if (!_wcsicmp(d->files[i].path, path))
            return &d->files[i];
    return NULL;
}

/* The title ID in default.xbe's certificate. */
static int disc_title_id(Disc *d, uint32_t *id)
{
    DiscFile *x = disc_find(d, L"default.xbe");
    uint8_t hdr[0x1000];
    uint32_t base, cert;
    if (!x || x->size < sizeof hdr || !disc_read(d, (uint64_t)x->sector * 2048, hdr, sizeof hdr)
            || memcmp(hdr, "XBEH", 4))
        return 0;
    memcpy(&base, hdr + 0x104, 4);
    memcpy(&cert, hdr + 0x118, 4);
    cert -= base;
    if (cert + 12 > x->size)
        return 0;
    if (cert + 12 <= sizeof hdr) {
        memcpy(id, hdr + cert + 8, 4);
        return 1;
    }
    return disc_read(d, (uint64_t)x->sector * 2048 + cert + 8, id, 4);
}

/* ── FFmpeg, for the movies ────────────────────────────────────────────── */

static int find_ffmpeg(WCHAR *out)
{
    WCHAR p[MAX_PATH];
    join(p, s_launcher_dir, L"tools\\ffmpeg.exe");
    if (file_exists(p)) { wcscpy_s(out, MAX_PATH, p); return 1; }
    if (s_game_dir[0]) {
        join(p, s_game_dir, L"tools\\ffmpeg.exe");
        if (file_exists(p)) { wcscpy_s(out, MAX_PATH, p); return 1; }
    }
    if (SearchPathW(NULL, L"ffmpeg.exe", NULL, MAX_PATH, out, NULL))
        return 1;
    if (file_exists(L"C:\\ffmpeg\\bin\\ffmpeg.exe")) {
        wcscpy_s(out, MAX_PATH, L"C:\\ffmpeg\\bin\\ffmpeg.exe");
        return 1;
    }
    return 0;
}

static void update_ffmpeg_status(void)
{
    WCHAR p[MAX_PATH], t[MAX_PATH + 64];
    if (find_ffmpeg(p)) {
        swprintf_s(t, MAX_PATH + 64, L"FFmpeg: %s", p);
        ShowWindow(ctl(ID_FFMPEG_GET), SW_HIDE);
    } else {
        wcscpy_s(t, MAX_PATH + 64, L"FFmpeg was not found. Without it the game skips its movies.");
        if (s_cur_tab == TAB_INSTALL)
            ShowWindow(ctl(ID_FFMPEG_GET), SW_SHOW);
    }
    set_text(ID_FFMPEG_STATUS, t);
}

/* Run a program hidden and wait; its exit code, or -1. */
static int run_hidden(const WCHAR *exe, WCHAR *cmdline, const WCHAR *cwd)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(exe, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi))
        return -1;
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT)
        if (s_cancel) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static DWORD WINAPI ffmpeg_download_thread(LPVOID unused)
{
    WCHAR tools[MAX_PATH], zip[MAX_PATH], tmp[MAX_PATH], cmd[MAX_PATH * 3], tar[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    HRESULT hr;
    (void)unused;
    join(tools, s_launcher_dir, L"tools");
    mkdirs(tools);
    join(zip, tools, L"ffmpeg-download.zip");
    join(tmp, tools, L"ffmpeg-unpack");
    post_progress(0, L"Downloading FFmpeg (about 150 MB)...");
    hr = URLDownloadToFileW(NULL, FFMPEG_URL, zip, 0, NULL);
    if (FAILED(hr)) {
        PostMessageW(s_wnd, WM_APP_DONE, 0, (LPARAM)wdup(L"FFmpeg could not be downloaded. Check the internet connection, "
                                                          L"or put ffmpeg.exe in the launcher's tools folder."));
        return 0;
    }
    post_progress(500, L"Unpacking FFmpeg...");
    mkdirs(tmp);
    GetSystemDirectoryW(tar, MAX_PATH);
    wcscat_s(tar, MAX_PATH, L"\\tar.exe");
    swprintf_s(cmd, MAX_PATH * 3, L"\"%s\" -xf \"%s\" -C \"%s\"", tar, zip, tmp);
    if (run_hidden(tar, cmd, tmp) != 0) {
        PostMessageW(s_wnd, WM_APP_DONE, 0, (LPARAM)wdup(L"The FFmpeg download could not be unpacked."));
        return 0;
    }
    /* ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe */
    swprintf_s(cmd, MAX_PATH * 3, L"%s\\*", tmp);
    h = FindFirstFileW(cmd, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            WCHAR src[MAX_PATH], dst[MAX_PATH];
            if (fd.cFileName[0] == L'.')
                continue;
            swprintf_s(src, MAX_PATH, L"%s\\%s\\bin\\ffmpeg.exe", tmp, fd.cFileName);
            join(dst, tools, L"ffmpeg.exe");
            if (file_exists(src))
                MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    DeleteFileW(zip);
    {
        SHFILEOPSTRUCTW op;
        WCHAR from[MAX_PATH + 2];
        memset(from, 0, sizeof from);
        wcscpy_s(from, MAX_PATH, tmp);
        memset(&op, 0, sizeof op);
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    }
    join(tmp, tools, L"ffmpeg.exe");
    PostMessageW(s_wnd, WM_APP_DONE, file_exists(tmp) ? 1 : 0,
                 (LPARAM)wdup(file_exists(tmp) ? L"FFmpeg is ready." : L"The download did not contain ffmpeg.exe."));
    return 0;
}

/* ── install ───────────────────────────────────────────────────────────── */

typedef struct {
    WCHAR image[MAX_PATH], target[MAX_PATH];
    int   movies;
} InstallJob;

static InstallJob s_job;
static int s_install_ok;
static WCHAR s_install_msg[1024];

static int copy_out(Disc *d, DiscFile *df, const WCHAR *target, uint8_t *buf, uint64_t *done, uint64_t total)
{
    WCHAR dst[MAX_PATH], dir[MAX_PATH], *slash;
    HANDLE out;
    uint32_t left = df->size;
    uint64_t off = (uint64_t)df->sector * 2048;
    join(dst, target, df->path);
    wcscpy_s(dir, MAX_PATH, dst);
    slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        swprintf_s(s_install_msg, 1024, L"Could not create %s (error %lu).", dst, GetLastError());
        return 0;
    }
    while (left) {
        DWORD n = left > (4u << 20) ? (4u << 20) : left, wr = 0;
        if (s_cancel) {
            CloseHandle(out);
            DeleteFileW(dst);
            wcscpy_s(s_install_msg, 1024, L"Installation cancelled.");
            return 0;
        }
        if (!disc_read(d, off, buf, n)) {
            CloseHandle(out);
            swprintf_s(s_install_msg, 1024, L"The disc image could not be read at %s. It may be incomplete.", df->path);
            return 0;
        }
        if (!WriteFile(out, buf, n, &wr, NULL) || wr != n) {
            CloseHandle(out);
            swprintf_s(s_install_msg, 1024, L"Could not write %s (error %lu). Is the disk full?", dst, GetLastError());
            return 0;
        }
        off += n;
        left -= n;
        *done += n;
        post_progress((int)(*done * 900 / (total ? total : 1)), NULL);
    }
    CloseHandle(out);
    return 1;
}

/* The port's own files, from the launcher's folder to the install. */
static int copy_program(const WCHAR *target)
{
    /* The Visual C++ runtime ships beside the exe (app-local), for PCs
     * without the redistributable installed. */
    static const WCHAR *files[] = { GAME_EXE, L"buffy_chaos_bleeds.pdb", L"Read Me.txt",
                                    L"vcruntime140.dll", L"vcruntime140_1.dll" };
    WCHAR src[MAX_PATH], dst[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int i;
    if (!_wcsicmp(target, s_launcher_dir))
        return 1;
    for (i = 0; i < (int)_countof(files); i++) {
        join(src, s_launcher_dir, files[i]);
        join(dst, target, files[i]);
        if (file_exists(src) && !CopyFileW(src, dst, FALSE) && i == 0) {
            swprintf_s(s_install_msg, 1024, L"Could not copy %s (error %lu).", files[i], GetLastError());
            return 0;
        }
    }
    /* Mods that come with the release (mods\<name>\ beside the launcher):
     * copied in once, unticked; a mod already in the game folder is kept. */
    join(pat, s_launcher_dir, L"mods\\*");
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            WCHAR from[MAX_PATH + 2], to[MAX_PATH + 2];
            SHFILEOPSTRUCTW op;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
                continue;
            memset(from, 0, sizeof from);
            memset(to, 0, sizeof to);
            swprintf_s(from, MAX_PATH, L"%s\\mods\\%s", s_launcher_dir, fd.cFileName);
            swprintf_s(to, MAX_PATH, L"%s\\mods\\%s", target, fd.cFileName);
            if (dir_exists(to))
                continue;
            join(dst, target, L"mods");
            mkdirs(dst);
            memset(&op, 0, sizeof op);
            op.wFunc = FO_COPY;
            op.pFrom = from;
            op.pTo = to;
            op.fFlags = FOF_NO_UI;
            SHFileOperationW(&op);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    /* Compiled shaders: the first sight of each effect then has no hitch. */
    join(pat, s_launcher_dir, L"ShaderCache\\*.cso");
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        join(dst, target, L"ShaderCache");
        mkdirs(dst);
        do {
            WCHAR a[MAX_PATH], b[MAX_PATH];
            swprintf_s(a, MAX_PATH, L"%s\\ShaderCache\\%s", s_launcher_dir, fd.cFileName);
            swprintf_s(b, MAX_PATH, L"%s\\ShaderCache\\%s", target, fd.cFileName);
            CopyFileW(a, b, TRUE);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return 1;
}

static void write_mods_readme(const WCHAR *target)
{
    static const char readme[] =
        "Buffy the Vampire Slayer: Chaos Bleeds mods\r\n"
        "==========================================\r\n\r\n"
        "Put each mod in its own folder here, then tick it on the launcher's Mods tab\r\n"
        "and press Apply mods (or Play).\r\n\r\n"
        "  mods\\MyMod\\mod.ini\r\n"
        "  mods\\MyMod\\files\\...   laid out like the game folder\r\n\r\n"
        "A file under files\\ is used instead of the game's file with the same path,\r\n"
        "for example files\\Buffy\\Binary\\_bin_xb\\BuildData\\Filelist.005 or\r\n"
        "files\\Movies\\intro1.mp4. The game's own files are never changed: untick a\r\n"
        "mod and it is gone. When two ticked mods have the same file, the one lower\r\n"
        "in the list wins.\r\n\r\n"
        "mod.ini:\r\n\r\n"
        "  [Mod]\r\n"
        "  Name = My mod\r\n"
        "  Author = you\r\n"
        "  Version = 1.0\r\n"
        "  Description = What it changes.\r\n\r\n"
        "Some changes are not in any file but are built into the PC version; a\r\n"
        "mod switches them on in mod.ini (a mod can be just this, with no files\\):\r\n\r\n"
        "  [Patches]\r\n"
        "  UnlockAllMultiplayerCharacters = 1   ; every Character Select portrait\r\n"
        "  UnlockAllMultiplayerArenas = 1       ; every Arena Select arena\r\n"
        "  UnlockAllWillowSpells = 1            ; Willow starts with all 14 spells\r\n"
        "  AlwaysPlayAsWillow = 1               ; every story level as Willow\r\n"
        "  StoryCoop = 1                        ; a second player in the story: Start\r\n"
        "                                       ; on controller 2 to join and pick\r\n"
        "  StoryCoopScreens = 2                 ; 1 one screen, 2 two windows,\r\n"
        "                                       ; 3 split screen (also on the\r\n"
        "                                       ; pause menu's Co-op page)\r\n"
        "  Player1ChangeCharacter = 1           ; Change Character on player 1's\r\n"
        "                                       ; pause menu\r\n";
    WCHAR dir[MAX_PATH], p[MAX_PATH];
    HANDLE f;
    DWORD wr;
    join(dir, target, L"mods");
    mkdirs(dir);
    join(p, dir, L"README.txt");
    if (file_exists(p))
        return;
    f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, readme, (DWORD)sizeof readme - 1, &wr, NULL);
        CloseHandle(f);
    }
}

static int convert_movies(const WCHAR *target)
{
    WCHAR ff[MAX_PATH], pat[MAX_PATH], outdir[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, i = 0, failed = 0;
    if (!find_ffmpeg(ff))
        return 1;
    swprintf_s(pat, MAX_PATH, L"%s\\Buffy\\Binary\\_bin_xb\\_Movies\\*.xmv", target);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do n++; while (FindNextFileW(h, &fd));
    FindClose(h);
    join(outdir, target, L"Movies");
    mkdirs(outdir);
    h = FindFirstFileW(pat, &fd);
    do {
        WCHAR name[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], cmd[MAX_PATH * 4], *dot;
        if (s_cancel)
            break;
        wcscpy_s(name, MAX_PATH, fd.cFileName);
        _wcslwr_s(name, MAX_PATH);
        dot = wcsrchr(name, L'.');
        if (dot)
            *dot = 0;
        swprintf_s(src, MAX_PATH, L"%s\\Buffy\\Binary\\_bin_xb\\_Movies\\%s", target, fd.cFileName);
        swprintf_s(dst, MAX_PATH, L"%s\\%s.mp4", outdir, name);
        i++;
        post_progress(900 + i * 100 / (n + 1), L"Converting the movies for PC playback (%d of %d)...", i, n);
        if (file_exists(dst))
            continue;
        swprintf_s(cmd, MAX_PATH * 4,
                   L"\"%s\" -hide_banner -loglevel error -y -i \"%s\" -c:v libx264 -preset medium -crf 18 "
                   L"-pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart \"%s\"", ff, src, dst);
        if (run_hidden(ff, cmd, target) != 0) {
            DeleteFileW(dst);
            failed++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (failed)
        swprintf_s(s_install_msg, 1024, L"%d movie(s) could not be converted; the game skips those.", failed);
    return 1;
}

static int install_run(InstallJob *job)
{
    Disc d;
    uint32_t tid = 0;
    uint64_t done = 0;
    ULARGE_INTEGER freeb;
    uint8_t *buf;
    int i, ok = 1;
    WCHAR err[512];

    s_install_msg[0] = 0;
    post_progress(0, L"Reading the disc image...");
    if (!disc_open(&d, job->image, err, 512)) {
        wcscpy_s(s_install_msg, 1024, err);
        disc_close(&d);
        return 0;
    }
    if (!disc_title_id(&d, &tid)) {
        wcscpy_s(s_install_msg, 1024, L"The disc image has no readable default.xbe; it is incomplete or not a game disc.");
        disc_close(&d);
        return 0;
    }
    if (tid != TITLE_ID) {
        swprintf_s(s_install_msg, 1024, L"This disc is not Buffy the Vampire Slayer: Chaos Bleeds (its title ID is %08X; "
                                        L"the game is %08X).", tid, TITLE_ID);
        disc_close(&d);
        return 0;
    }
    if (!mkdirs(job->target)) {
        swprintf_s(s_install_msg, 1024, L"Could not create %s.", job->target);
        disc_close(&d);
        return 0;
    }
    if (GetDiskFreeSpaceExW(job->target, &freeb, NULL, NULL)
            && freeb.QuadPart < d.total + (1ull << 30)) {
        swprintf_s(s_install_msg, 1024, L"Not enough free space: the game needs about %.1f GB, and the drive has %.1f GB.",
                   (d.total + (1ull << 30)) / 1073741824.0, freeb.QuadPart / 1073741824.0);
        disc_close(&d);
        return 0;
    }
    buf = (uint8_t *)malloc(4u << 20);
    if (!buf) {
        disc_close(&d);
        wcscpy_s(s_install_msg, 1024, L"Out of memory.");
        return 0;
    }
    for (i = 0; i < d.count && ok; i++) {
        post_progress((int)(done * 900 / (d.total ? d.total : 1)),
                      L"Copying the game from the disc image (%.1f of %.1f GB)...",
                      done / 1073741824.0, d.total / 1073741824.0);
        ok = copy_out(&d, &d.files[i], job->target, buf, &done, d.total);
    }
    free(buf);
    disc_close(&d);
    if (!ok)
        return 0;
    post_progress(900, L"Setting up the PC version...");
    if (!copy_program(job->target))
        return 0;
    write_mods_readme(job->target);
    if (job->movies && !convert_movies(job->target))
        return 0;
    if (s_cancel) {
        wcscpy_s(s_install_msg, 1024, L"Installation cancelled.");
        return 0;
    }
    if (!has_program(job->target)) {
        wcscpy_s(s_install_msg, 1024, L"The game files are installed, but the launcher's folder has no " GAME_EXE
                                      L" to copy beside them.");
        return 0;
    }
    return 1;
}

static DWORD WINAPI install_thread(LPVOID unused)
{
    int ok;
    (void)unused;
    ok = install_run(&s_job);
    PostMessageW(s_wnd, WM_APP_DONE, ok ? 1 : 0, (LPARAM)wdup(s_install_msg));
    return 0;
}

/* ── settings tab ──────────────────────────────────────────────────────── */

static void settings_load(void)
{
    WCHAR p[MAX_PATH];
    int w, h, i, sel = 0;
    settings_path(p);
    w = (int)GetPrivateProfileIntW(L"Display", L"Width", 1920, p);
    h = (int)GetPrivateProfileIntW(L"Display", L"Height", 1080, p);
    for (i = 0; i < N_RES; i++)
        if (k_res[i].w == w && k_res[i].h == h)
            sel = i;
    SendMessageW(ctl(ID_RESOLUTION), CB_SETCURSEL, (WPARAM)sel, 0);
    CheckRadioButton(s_wnd, ID_WINDOWED, ID_FULLSCREEN,
                     GetPrivateProfileIntW(L"Display", L"Fullscreen", 1, p) ? ID_FULLSCREEN : ID_WINDOWED);
    CheckDlgButton(s_wnd, ID_VSYNC, GetPrivateProfileIntW(L"Display", L"VSync", 1, p) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_SKIP_INTRO,
                   GetPrivateProfileIntW(L"Game", L"SkipIntroMovies", 0, p) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_WS_SAFE,
                   GetPrivateProfileIntW(L"Display", L"WidescreenWide", 0, p) ? BST_UNCHECKED : BST_CHECKED);
    CheckDlgButton(s_wnd, ID_INVERT_X,
                   GetPrivateProfileIntW(L"Controls", L"InvertCameraX", 0, p) ? BST_CHECKED : BST_UNCHECKED);
    s_settings_dirty = 0;
    set_text(ID_SETTINGS_STATUS, L"");
}

static int settings_save(void)
{
    WCHAR p[MAX_PATH], v[16];
    int sel = (int)SendMessageW(ctl(ID_RESOLUTION), CB_GETCURSEL, 0, 0);
    if (!is_game_folder(s_game_dir)) {
        set_text(ID_SETTINGS_STATUS, L"Install the game first; settings are kept in its folder.");
        return 0;
    }
    if (sel < 0 || sel >= N_RES)
        sel = 0;
    settings_path(p);
    swprintf_s(v, 16, L"%d", k_res[sel].w);
    WritePrivateProfileStringW(L"Display", L"Width", v, p);
    swprintf_s(v, 16, L"%d", k_res[sel].h);
    WritePrivateProfileStringW(L"Display", L"Height", v, p);
    WritePrivateProfileStringW(L"Display", L"VSync", IsDlgButtonChecked(s_wnd, ID_VSYNC) ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"Display", L"Fullscreen", IsDlgButtonChecked(s_wnd, ID_FULLSCREEN) ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"Game", L"SkipIntroMovies", IsDlgButtonChecked(s_wnd, ID_SKIP_INTRO) ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"Display", L"WidescreenWide", IsDlgButtonChecked(s_wnd, ID_WS_SAFE) ? L"0" : L"1", p);
    WritePrivateProfileStringW(L"Controls", L"InvertCameraX", IsDlgButtonChecked(s_wnd, ID_INVERT_X) ? L"1" : L"0", p);
    s_settings_dirty = 0;
    set_text(ID_SETTINGS_STATUS, L"Settings saved.");
    return 1;
}

static void settings_defaults(void)
{
    SendMessageW(ctl(ID_RESOLUTION), CB_SETCURSEL, 0, 0);
    CheckRadioButton(s_wnd, ID_WINDOWED, ID_FULLSCREEN, ID_FULLSCREEN);
    CheckDlgButton(s_wnd, ID_VSYNC, BST_CHECKED);
    CheckDlgButton(s_wnd, ID_SKIP_INTRO, BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_WS_SAFE, BST_CHECKED);
    CheckDlgButton(s_wnd, ID_INVERT_X, BST_UNCHECKED);
    s_settings_dirty = 1;
    set_text(ID_SETTINGS_STATUS, L"Defaults restored. Press Save or Play to keep them.");
}

/* ── mods tab ──────────────────────────────────────────────────────────── */

typedef struct {
    WCHAR folder[128], name[128], author[128], version[32], desc[512];
} Mod;

static Mod *s_mods;
static int  s_nmods;

static void mods_dir(WCHAR *out)
{
    join(out, s_game_dir, L"mods");
}

/* Every mod folder, ordered as enabled.txt lists the ticked ones first. */
static void mods_scan(void)
{
    WCHAR dir[MAX_PATH], pat[MAX_PATH], list[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    LVITEMW it;
    HWND lv = ctl(ID_MODLIST);
    char line[256];
    FILE *f;
    int i, n = 0, order = 0;
    static WCHAR enabled[64][128];
    int nen = 0;

    free(s_mods);
    s_mods = NULL;
    s_nmods = 0;
    ListView_DeleteAllItems(lv);
    if (!is_game_folder(s_game_dir)) {
        set_text(ID_MOD_STATUS, L"Install the game first, or choose an existing game folder on the Play tab.");
        return;
    }
    mods_dir(dir);
    swprintf_s(list, MAX_PATH, L"%s\\.launcher\\enabled.txt", dir);
    if (!_wfopen_s(&f, list, L"r") && f) {
        while (fgets(line, sizeof line, f) && nen < 64) {
            size_t k = strlen(line);
            while (k && (line[k - 1] == '\n' || line[k - 1] == '\r'))
                line[--k] = 0;
            if (k)
                MultiByteToWideChar(CP_UTF8, 0, line, -1, enabled[nen++], 128);
        }
        fclose(f);
    }
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
                Mod *m;
                WCHAR ini[MAX_PATH];
                Mod *grown = (Mod *)realloc(s_mods, sizeof(Mod) * (size_t)(n + 1));
                if (!grown)
                    break;
                s_mods = grown;
                m = &s_mods[n++];
                memset(m, 0, sizeof *m);
                wcscpy_s(m->folder, 128, fd.cFileName);
                swprintf_s(ini, MAX_PATH, L"%s\\%s\\mod.ini", dir, fd.cFileName);
                GetPrivateProfileStringW(L"Mod", L"Name", fd.cFileName, m->name, 128, ini);
                GetPrivateProfileStringW(L"Mod", L"Author", L"", m->author, 128, ini);
                GetPrivateProfileStringW(L"Mod", L"Version", L"", m->version, 32, ini);
                GetPrivateProfileStringW(L"Mod", L"Description", L"", m->desc, 512, ini);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    s_nmods = n;
    /* Ticked mods first, in their saved order, then the rest by name. */
    for (i = 0; i < nen; i++) {
        int k;
        for (k = order; k < n; k++)
            if (!_wcsicmp(s_mods[k].folder, enabled[i])) {
                Mod t = s_mods[order];
                s_mods[order] = s_mods[k];
                s_mods[k] = t;
                order++;
                break;
            }
    }
    for (i = 0; i < n; i++) {
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = s_mods[i].name;
        ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, i, 1, s_mods[i].version);
        ListView_SetItemText(lv, i, 2, s_mods[i].author);
        ListView_SetCheckState(lv, i, i < order);
    }
    set_text(ID_MOD_DESC, L"");
    set_text(ID_MOD_STATUS, n ? L"Tick the mods to use. They are used from the next time the game starts."
                              : L"No mods yet. Put each mod in its own folder under mods\\ in the game folder; "
                                L"mods\\README.txt describes the layout.");
}

/* Game files two ticked mods both replace, for a warning. */
static void mods_conflicts(WCHAR *out, size_t cap)
{
    HWND lv = ctl(ID_MODLIST);
    int a, b, found = 0;
    out[0] = 0;
    for (a = 0; a < s_nmods && !found; a++) {
        WCHAR pat[MAX_PATH];
        if (!ListView_GetCheckState(lv, a))
            continue;
        for (b = a + 1; b < s_nmods && !found; b++) {
            WIN32_FIND_DATAW fd;
            HANDLE h;
            WCHAR root[MAX_PATH], stack[16][MAX_PATH];
            int sp = 0;
            if (!ListView_GetCheckState(lv, b))
                continue;
            swprintf_s(root, MAX_PATH, L"%s\\mods\\%s\\files", s_game_dir, s_mods[a].folder);
            stack[sp++][0] = 0;
            while (sp && !found) {
                WCHAR rel[MAX_PATH];
                wcscpy_s(rel, MAX_PATH, stack[--sp]);
                swprintf_s(pat, MAX_PATH, rel[0] ? L"%s\\%s\\*" : L"%s%s\\*", root, rel);
                h = FindFirstFileW(pat, &fd);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                do {
                    WCHAR r2[MAX_PATH], other[MAX_PATH];
                    if (fd.cFileName[0] == L'.')
                        continue;
                    swprintf_s(r2, MAX_PATH, rel[0] ? L"%s\\%s" : L"%s%s", rel, fd.cFileName);
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (sp < 16)
                            wcscpy_s(stack[sp++], MAX_PATH, r2);
                    } else {
                        swprintf_s(other, MAX_PATH, L"%s\\mods\\%s\\files\\%s", s_game_dir, s_mods[b].folder, r2);
                        if (file_exists(other)) {
                            swprintf_s(out, cap, L"%s and %s both replace %s; %s is lower in the list and is used.",
                                       s_mods[a].name, s_mods[b].name, r2, s_mods[b].name);
                            found = 1;
                            break;
                        }
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
    }
}

static int mods_apply(void)
{
    WCHAR dir[MAX_PATH], list[MAX_PATH], msg[1024];
    HWND lv = ctl(ID_MODLIST);
    FILE *f;
    int i, n = 0;
    if (!is_game_folder(s_game_dir))
        return 0;
    if (s_game_proc && WaitForSingleObject(s_game_proc, 0) == WAIT_TIMEOUT) {
        set_text(ID_MOD_STATUS, L"Close the game before changing mods.");
        return 0;
    }
    mods_dir(dir);
    wcscat_s(dir, MAX_PATH, L"\\.launcher");
    mkdirs(dir);
    join(list, dir, L"enabled.txt");
    if (_wfopen_s(&f, list, L"w") || !f) {
        set_text(ID_MOD_STATUS, L"The mod list could not be saved.");
        return 0;
    }
    for (i = 0; i < s_nmods; i++)
        if (ListView_GetCheckState(lv, i)) {
            char u[512];
            WideCharToMultiByte(CP_UTF8, 0, s_mods[i].folder, -1, u, sizeof u, NULL, NULL);
            fprintf(f, "%s\n", u);
            n++;
        }
    fclose(f);
    mods_conflicts(msg, 1024);
    if (!msg[0])
        swprintf_s(msg, 1024, n ? L"%d mod(s) will be used when the game starts." : L"No mods will be used.", n);
    set_text(ID_MOD_STATUS, msg);
    return 1;
}

static void mods_move(int dir)
{
    HWND lv = ctl(ID_MODLIST);
    int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED), j = i + dir, ci, cj;
    Mod t;
    if (i < 0 || j < 0 || j >= s_nmods)
        return;
    ci = ListView_GetCheckState(lv, i);
    cj = ListView_GetCheckState(lv, j);
    t = s_mods[i]; s_mods[i] = s_mods[j]; s_mods[j] = t;
    ListView_SetItemText(lv, i, 0, s_mods[i].name);
    ListView_SetItemText(lv, i, 1, s_mods[i].version);
    ListView_SetItemText(lv, i, 2, s_mods[i].author);
    ListView_SetItemText(lv, j, 0, s_mods[j].name);
    ListView_SetItemText(lv, j, 1, s_mods[j].version);
    ListView_SetItemText(lv, j, 2, s_mods[j].author);
    ListView_SetCheckState(lv, i, cj);
    ListView_SetCheckState(lv, j, ci);
    ListView_SetItemState(lv, j, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    set_text(ID_MOD_STATUS, L"Order changed. Press Apply mods or Play to keep it.");
}

/* ── play tab ──────────────────────────────────────────────────────────── */

static void refresh_play(void)
{
    WCHAR t[MAX_PATH + 128];
    set_text(ID_GAMEDIR, s_game_dir[0] ? s_game_dir : L"(not installed)");
    if (!is_game_folder(s_game_dir))
        wcscpy_s(t, MAX_PATH + 128, L"The game is not installed. Install it from your disc image on the Install tab, "
                                    L"or choose an existing game folder.");
    else if (!has_program(s_game_dir))
        swprintf_s(t, MAX_PATH + 128, L"That folder has the game's files but not " GAME_EXE L".");
    else if (s_game_proc && WaitForSingleObject(s_game_proc, 0) == WAIT_TIMEOUT)
        wcscpy_s(t, MAX_PATH + 128, L"The game is running.");
    else
        wcscpy_s(t, MAX_PATH + 128, L"Ready.");
    set_text(ID_PLAY_STATUS, t);
    EnableWindow(ctl(ID_PLAY), is_game_folder(s_game_dir) && has_program(s_game_dir));
    if (s_icon == NULL && is_game_folder(s_game_dir)) {
        s_icon = load_game_icon(s_game_dir);
        if (s_icon) {
            SendMessageW(s_wnd, WM_SETICON, ICON_BIG, (LPARAM)s_icon);
            SendMessageW(s_wnd, WM_SETICON, ICON_SMALL, (LPARAM)s_icon);
            InvalidateRect(s_wnd, NULL, TRUE);
        }
    }
}

static DWORD WINAPI game_watch(LPVOID p)
{
    HANDLE h = (HANDLE)p;
    DWORD code = 0;
    WaitForSingleObject(h, INFINITE);
    GetExitCodeProcess(h, &code);
    PostMessageW(s_wnd, WM_APP_GAMEEND, code, 0);
    return 0;
}

static void play(void)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 4];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (s_game_proc && WaitForSingleObject(s_game_proc, 0) == WAIT_TIMEOUT) {
        set_text(ID_PLAY_STATUS, L"The game is already running.");
        return;
    }
    if (s_settings_dirty)
        settings_save();
    if (s_nmods)
        mods_apply();
    join(exe, s_game_dir, GAME_EXE);
    swprintf_s(cmd, MAX_PATH + 4, L"\"%s\"", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, s_game_dir, &si, &pi)) {
        WCHAR m[MAX_PATH + 64];
        swprintf_s(m, MAX_PATH + 64, L"Windows could not start the game (error %lu).", GetLastError());
        set_text(ID_PLAY_STATUS, m);
        return;
    }
    CloseHandle(pi.hThread);
    if (s_game_proc)
        CloseHandle(s_game_proc);
    s_game_proc = pi.hProcess;
    save_launcher_ini();
    if (IsDlgButtonChecked(s_wnd, ID_CLOSE_ON_PLAY) == BST_CHECKED) {
        DestroyWindow(s_wnd);
        return;
    }
    {
        HANDLE dup;
        DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &dup, SYNCHRONIZE | PROCESS_QUERY_INFORMATION,
                        FALSE, 0);
        CloseHandle(CreateThread(NULL, 0, game_watch, dup, 0, NULL));
    }
    set_text(ID_PLAY_STATUS, L"The game is running.");
}

/* ── folder / file pickers ─────────────────────────────────────────────── */

static int pick_folder(const WCHAR *title, WCHAR *out)
{
    IFileOpenDialog *d;
    IShellItem *it;
    PWSTR p = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    d->lpVtbl->SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->lpVtbl->SetTitle(d, title);
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcscpy_s(out, MAX_PATH, p);
            CoTaskMemFree(p);
            ok = 1;
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
    return ok;
}

static int pick_image(WCHAR *out)
{
    OPENFILENAMEW of;
    WCHAR buf[MAX_PATH] = L"";
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = s_wnd;
    of.lpstrFilter = L"Xbox disc images (*.iso, *.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"Choose your Buffy the Vampire Slayer: Chaos Bleeds disc image";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of))
        return 0;
    wcscpy_s(out, MAX_PATH, buf);
    return 1;
}

/* ── textures ──────────────────────────────────────────────────────────── */

/* Texture packs, as in Dolphin and PCSX2: the game dumps its textures to
 * textures_replacement\dump and replaces them with the images in
 * textures_replacement\load ([Textures] in buffy_settings.ini). */

static void tex_dir(WCHAR *out, const WCHAR *sub)
{
    WCHAR root[MAX_PATH];
    join(root, s_game_dir, L"textures_replacement");
    if (sub)
        join(out, root, sub);
    else
        wcscpy_s(out, MAX_PATH, root);
}

static void write_tex_readme(void)
{
    static const char readme[] =
        "Buffy the Vampire Slayer: Chaos Bleeds texture packs\r\n"
        "====================================================\r\n\r\n"
        "Switch these on in the launcher's Textures tab.\r\n\r\n"
        "dump\\  With \"Dump textures\" on, every texture the game shows is saved here\r\n"
        "       once, as a PNG named  buffy_<width>x<height>_<code>_<format>.png\r\n\r\n"
        "load\\  With \"Load custom textures\" on, an image here replaces the game's\r\n"
        "       texture that has the same 16-character code in its name.\r\n"
        "       - PNG (or DDS), any size: 2x or 4x the original looks sharper.\r\n"
        "       - Keep the code in the name; anything else may change\r\n"
        "         (buffy_256x256_049c2d6e895bf871_06_HD.png is fine).\r\n"
        "       - Subfolders are fine, so a pack can be one folder.\r\n"
        "       - Keep the transparent parts transparent.\r\n\r\n"
        "Making a pack: tick Dump, play the parts you want to change, copy the\r\n"
        "PNGs you want from dump\\ into load\\, edit them, then untick Dump and\r\n"
        "tick Load. Sharing a pack: zip your folder from load\\; others unzip it\r\n"
        "into their load\\ folder.\r\n";
    WCHAR p[MAX_PATH];
    HANDLE h;
    DWORD n;
    tex_dir(p, L"README.txt");
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, readme, (DWORD)(sizeof readme - 1), &n, NULL);
        CloseHandle(h);
    }
}

static void tex_make_dirs(void)
{
    WCHAR d[MAX_PATH];
    tex_dir(d, L"load");
    mkdirs(d);
    tex_dir(d, L"dump");
    mkdirs(d);
    write_tex_readme();
}

/* Images in a folder and its subfolders. */
static int tex_count(const WCHAR *dir, int depth)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    join(pat, dir, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        const WCHAR *ext = wcsrchr(fd.cFileName, L'.');
        if (fd.cFileName[0] == L'.')
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8) {
                join(p, dir, fd.cFileName);
                n += tex_count(p, depth + 1);
            }
        } else if (ext && (!_wcsicmp(ext, L".png") || !_wcsicmp(ext, L".dds"))) {
            n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void tex_refresh(void)
{
    WCHAR p[MAX_PATH], d[MAX_PATH], t[256];
    int ok = is_game_folder(s_game_dir), i;
    int ids[] = { ID_TEX_LOAD, ID_TEX_PREFETCH, ID_TEX_DUMP, ID_TEX_OPEN_LOAD, ID_TEX_OPEN_DUMP, ID_TEX_REFRESH };
    for (i = 0; i < (int)(sizeof ids / sizeof ids[0]); i++)
        EnableWindow(ctl(ids[i]), ok);
    if (!ok) {
        set_text(ID_TEX_STATUS, L"Install the game first (Install tab).");
        return;
    }
    settings_path(p);
    CheckDlgButton(s_wnd, ID_TEX_LOAD, GetPrivateProfileIntW(L"Textures", L"Load", 0, p) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_TEX_PREFETCH,
                   GetPrivateProfileIntW(L"Textures", L"Prefetch", 0, p) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_TEX_DUMP, GetPrivateProfileIntW(L"Textures", L"Dump", 0, p) ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(ctl(ID_TEX_PREFETCH), IsDlgButtonChecked(s_wnd, ID_TEX_LOAD) == BST_CHECKED);
    tex_make_dirs();
    tex_dir(d, L"load");
    i = tex_count(d, 0);
    tex_dir(d, L"dump");
    swprintf_s(t, 256, L"Custom textures in load: %d.   Dumped textures in dump: %d.", i, tex_count(d, 0));
    set_text(ID_TEX_STATUS, t);
}

static void tex_save(void)
{
    WCHAR p[MAX_PATH];
    if (!is_game_folder(s_game_dir))
        return;
    settings_path(p);
    WritePrivateProfileStringW(L"Textures", L"Load", IsDlgButtonChecked(s_wnd, ID_TEX_LOAD) ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"Textures", L"Prefetch", IsDlgButtonChecked(s_wnd, ID_TEX_PREFETCH) ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"Textures", L"Dump", IsDlgButtonChecked(s_wnd, ID_TEX_DUMP) ? L"1" : L"0", p);
    EnableWindow(ctl(ID_TEX_PREFETCH), IsDlgButtonChecked(s_wnd, ID_TEX_LOAD) == BST_CHECKED);
}

static void tex_open(const WCHAR *sub)
{
    WCHAR d[MAX_PATH];
    if (!is_game_folder(s_game_dir))
        return;
    tex_make_dirs();
    tex_dir(d, sub);
    ShellExecuteW(s_wnd, L"open", d, NULL, NULL, SW_SHOWNORMAL);
}

/* ── window ────────────────────────────────────────────────────────────── */

#define HEADER_H 64      /* dark title band */
#define DY       64      /* page content sits this much lower than laid out */

static HWND add(int tab, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, S(x), S(y + DY), S(w), S(h), s_wnd,
                             (HMENU)(INT_PTR)id, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    if (tab >= 0 && s_nctl[tab] < 32)
        s_ctl[tab][s_nctl[tab]++] = c;
    return c;
}

static void show_tab(int t)
{
    int i, k;
    s_cur_tab = t;
    for (k = 0; k < TAB_COUNT; k++)
        for (i = 0; i < s_nctl[k]; i++)
            ShowWindow(s_ctl[k][i], k == t ? SW_SHOW : SW_HIDE);
    if (t == TAB_INSTALL)
        update_ffmpeg_status();
    if (t == TAB_PLAY)
        refresh_play();
    if (t == TAB_MODS)
        mods_scan();
    if (t == TAB_TEXTURES)
        tex_refresh();
    TabCtrl_SetCurSel(s_tab, t);
}

#define X0 28
static void build_ui(void)
{
    TCITEMW ti;
    HWND h, lv;
    LVCOLUMNW col;
    WCHAR v[64];
    int i;
    static const WCHAR *names[TAB_COUNT] = { L"Play", L"Settings", L"Mods", L"Textures", L"Install" };

    /* Just the strip of tabs; the pages below are plain window. */
    s_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
                            S(12), S(HEADER_H + 10), S(596), S(28), s_wnd, (HMENU)ID_TAB, s_inst, NULL);
    SendMessageW(s_tab, WM_SETFONT, (WPARAM)s_font, TRUE);
    for (i = 0; i < TAB_COUNT; i++) {
        ti.mask = TCIF_TEXT;
        ti.pszText = (WCHAR *)names[i];
        TabCtrl_InsertItem(s_tab, i, &ti);
    }

    /* Play */
    add(TAB_PLAY, L"Static", L"The PC port of the 2003 Xbox game, running the original game code.", SS_LEFT,
        X0, 60, 560, 24, 0);
    add(TAB_PLAY, L"Button", L"Game folder", BS_GROUPBOX, X0, 104, 560, 70, 0);
    add(TAB_PLAY, L"Edit", L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER, X0 + 14, 130, 430, 24, ID_GAMEDIR);
    add(TAB_PLAY, L"Button", L"Change...", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 129, 92, 26, ID_GAMEDIR_CHANGE);
    add(TAB_PLAY, L"Static", L"", SS_LEFT, X0, 190, 560, 40, ID_PLAY_STATUS);
    h = add(TAB_PLAY, L"Button", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, X0, 246, 200, 52, ID_PLAY);
    SendMessageW(h, WM_SETFONT, (WPARAM)s_big, TRUE);
    add(TAB_PLAY, L"Button", L"Close the launcher when the game starts", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0, 312, 400, 24, ID_CLOSE_ON_PLAY);
    add(TAB_PLAY, L"Static", L"While playing: Alt+Enter or F11 switches fullscreen. The game's own Options page "
                             L"also has Resolution and VSync, and the main menu has Exit.",
        SS_LEFT, X0, 356, 560, 40, 0);
    swprintf_s(v, 64, L"Launcher version %s", LAUNCHER_VERSION);
    add(TAB_PLAY, L"Static", v, SS_LEFT, X0, 440, 300, 20, ID_VERSION);

    /* Settings */
    add(TAB_SETTINGS, L"Button", L"Display", BS_GROUPBOX, X0, 50, 560, 206, 0);
    add(TAB_SETTINGS, L"Button", L"Windowed", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, X0 + 16, 76, 120, 24, ID_WINDOWED);
    add(TAB_SETTINGS, L"Button", L"Fullscreen", BS_AUTORADIOBUTTON, X0 + 150, 76, 120, 24, ID_FULLSCREEN);
    add(TAB_SETTINGS, L"Static", L"Borderless, covering the whole monitor.", SS_LEFT, X0 + 276, 80, 270, 20, 0);
    add(TAB_SETTINGS, L"Static", L"Resolution", SS_LEFT, X0 + 16, 116, 120, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 150, 112, 300, 300, ID_RESOLUTION);
    for (i = 0; i < N_RES; i++)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)k_res[i].label);
    add(TAB_SETTINGS, L"Static", L"The size the game is drawn at. Widescreen sizes play in the game's own 16:9 mode; "
                                 L"menus and movies stay 4:3.", SS_LEFT, X0 + 150, 144, 400, 36, 0);
    add(TAB_SETTINGS, L"Button", L"VSync (no tearing; waits for the monitor's refresh)", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0 + 16, 184, 420, 24, ID_VSYNC);
    add(TAB_SETTINGS, L"Button", L"Widescreen: keep the original side-to-side view (no pop-in or clipping at the edges)",
        BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, X0 + 16, 212, 530, 36, ID_WS_SAFE);
    add(TAB_SETTINGS, L"Button", L"Game and controls", BS_GROUPBOX, X0, 264, 560, 92, 0);
    add(TAB_SETTINGS, L"Button", L"Skip the intro movies at start-up", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0 + 16, 288, 420, 24, ID_SKIP_INTRO);
    add(TAB_SETTINGS, L"Button", L"Invert camera left / right (right stick)", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0 + 16, 318, 420, 24, ID_INVERT_X);
    add(TAB_SETTINGS, L"Static", L"Volume, subtitles, vibration and the game's own camera inversion are kept in its save "
                                 L"and are changed from Options in the game.", SS_LEFT, X0, 364, 560, 34, 0);
    add(TAB_SETTINGS, L"Button", L"Restore defaults", BS_PUSHBUTTON | WS_TABSTOP, X0, 404, 140, 30, ID_DEFAULTS);
    add(TAB_SETTINGS, L"Button", L"Save", BS_PUSHBUTTON | WS_TABSTOP, X0 + 452, 404, 108, 30, ID_SAVE);
    add(TAB_SETTINGS, L"Static", L"", SS_LEFT, X0, 444, 560, 20, ID_SETTINGS_STATUS);

    /* Mods */
    lv = add(TAB_MODS, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
             X0, 50, 460, 250, ID_MODLIST);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(lv, L"Explorer", NULL);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = L"Name"; col.cx = S(260); ListView_InsertColumn(lv, 0, &col);
    col.pszText = L"Version"; col.cx = S(70); ListView_InsertColumn(lv, 1, &col);
    col.pszText = L"Author"; col.cx = S(120); ListView_InsertColumn(lv, 2, &col);
    add(TAB_MODS, L"Button", L"Move up", BS_PUSHBUTTON | WS_TABSTOP, X0 + 470, 50, 90, 28, ID_MOD_UP);
    add(TAB_MODS, L"Button", L"Move down", BS_PUSHBUTTON | WS_TABSTOP, X0 + 470, 84, 90, 28, ID_MOD_DOWN);
    add(TAB_MODS, L"Static", L"", SS_LEFT, X0, 308, 560, 54, ID_MOD_DESC);
    add(TAB_MODS, L"Static", L"", SS_LEFT, X0, 368, 560, 40, ID_MOD_STATUS);
    add(TAB_MODS, L"Button", L"Open mods folder", BS_PUSHBUTTON | WS_TABSTOP, X0, 420, 140, 30, ID_MOD_OPEN);
    add(TAB_MODS, L"Button", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, X0 + 150, 420, 100, 30, ID_MOD_REFRESH);
    add(TAB_MODS, L"Button", L"Apply mods", BS_PUSHBUTTON | WS_TABSTOP, X0 + 452, 420, 108, 30, ID_MOD_APPLY);

    /* Textures */
    add(TAB_TEXTURES, L"Static", L"Swap the game's textures for your own, like Dolphin and PCSX2 texture packs. "
                                 L"They live in the game folder's textures_replacement folder.",
        SS_LEFT, X0, 50, 560, 36, 0);
    add(TAB_TEXTURES, L"Button", L"Custom textures", BS_GROUPBOX, X0, 92, 560, 96, 0);
    add(TAB_TEXTURES, L"Button", L"Load custom textures from textures_replacement\\load", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0 + 16, 116, 520, 24, ID_TEX_LOAD);
    add(TAB_TEXTURES, L"Button", L"Load them all when the game starts (no stutter the first time each one is "
                                 L"seen; uses more memory)", BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP,
        X0 + 36, 144, 500, 36, ID_TEX_PREFETCH);
    add(TAB_TEXTURES, L"Button", L"Dumping", BS_GROUPBOX, X0, 196, 560, 76, 0);
    add(TAB_TEXTURES, L"Button", L"Dump textures while playing to textures_replacement\\dump",
        BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 16, 220, 520, 24, ID_TEX_DUMP);
    add(TAB_TEXTURES, L"Static", L"Each texture is saved once, as a PNG. Leave it off when not making a pack.",
        SS_LEFT, X0 + 36, 246, 500, 20, 0);
    add(TAB_TEXTURES, L"Static", L"Making a pack:  1. Tick Dump and play the parts you want to change.  "
                                 L"2. Copy those PNGs from dump into load and edit them - any size, but keep the "
                                 L"16-character code in the name.  3. Untick Dump, tick Load, and play.",
        SS_LEFT, X0, 282, 560, 54, 0);
    add(TAB_TEXTURES, L"Static", L"", SS_LEFT, X0, 346, 560, 40, ID_TEX_STATUS);
    add(TAB_TEXTURES, L"Button", L"Open load folder", BS_PUSHBUTTON | WS_TABSTOP, X0, 420, 140, 30, ID_TEX_OPEN_LOAD);
    add(TAB_TEXTURES, L"Button", L"Open dump folder", BS_PUSHBUTTON | WS_TABSTOP, X0 + 150, 420, 140, 30, ID_TEX_OPEN_DUMP);
    add(TAB_TEXTURES, L"Button", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, X0 + 460, 420, 100, 30, ID_TEX_REFRESH);

    /* Install */
    add(TAB_INSTALL, L"Static", L"1.  Your Buffy the Vampire Slayer: Chaos Bleeds disc image (Xbox ISO or XISO)",
        SS_LEFT, X0, 54, 560, 20, 0);
    add(TAB_INSTALL, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 20, 78, 424, 24, ID_IMAGE);
    add(TAB_INSTALL, L"Button", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 77, 92, 26, ID_IMAGE_BROWSE);
    add(TAB_INSTALL, L"Static", L"The disc image is only read, never changed.", SS_LEFT, X0 + 20, 106, 520, 20, 0);
    add(TAB_INSTALL, L"Static", L"2.  Install to", SS_LEFT, X0, 138, 560, 20, 0);
    add(TAB_INSTALL, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 20, 162, 424, 24, ID_TARGET);
    add(TAB_INSTALL, L"Button", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 161, 92, 26, ID_TARGET_BROWSE);
    add(TAB_INSTALL, L"Static", L"3.  Movies", SS_LEFT, X0, 202, 560, 20, 0);
    add(TAB_INSTALL, L"Button", L"Convert the game's movies for PC playback (uses FFmpeg)", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0 + 20, 224, 440, 24, ID_MOVIES);
    add(TAB_INSTALL, L"Static", L"", SS_LEFT | SS_PATHELLIPSIS, X0 + 20, 252, 420, 20, ID_FFMPEG_STATUS);
    add(TAB_INSTALL, L"Button", L"Download FFmpeg", BS_PUSHBUTTON | WS_TABSTOP, X0 + 446, 247, 100, 28, ID_FFMPEG_GET);
    add(TAB_INSTALL, L"Button", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, X0, 300, 140, 36, ID_INSTALL);
    add(TAB_INSTALL, PROGRESS_CLASSW, L"", PBS_SMOOTH, X0, 350, 560, 20, ID_PROGRESS);
    SendMessageW(ctl(ID_PROGRESS), PBM_SETRANGE32, 0, 1000);
    add(TAB_INSTALL, L"Static", L"", SS_LEFT, X0, 378, 560, 60, ID_INSTALL_STATUS);

    CheckDlgButton(s_wnd, ID_MOVIES, BST_CHECKED);
    CheckDlgButton(s_wnd, ID_CLOSE_ON_PLAY,
                   GetPrivateProfileIntW(L"Launcher", L"CloseOnPlay", 0, s_launcher_ini) ? BST_CHECKED : BST_UNCHECKED);
    {
        WCHAR img[MAX_PATH];
        GetPrivateProfileStringW(L"Launcher", L"LastImage", L"", img, MAX_PATH, s_launcher_ini);
        set_text(ID_IMAGE, img);
    }
    set_text(ID_TARGET, s_game_dir[0] ? s_game_dir : s_launcher_dir);
    settings_load();
}

static void set_busy(int busy)
{
    int ids[] = { ID_IMAGE, ID_IMAGE_BROWSE, ID_TARGET, ID_TARGET_BROWSE, ID_MOVIES, ID_FFMPEG_GET, ID_PLAY };
    int i;
    s_busy = busy;
    for (i = 0; i < (int)(sizeof ids / sizeof ids[0]); i++)
        EnableWindow(ctl(ids[i]), !busy);
    set_text(ID_INSTALL, busy ? L"Cancel" : L"Install");
}

static void start_install(void)
{
    WCHAR t[MAX_PATH + 200];
    if (s_busy) {
        if (MessageBoxW(s_wnd, L"Stop the installation?", GAME_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            InterlockedExchange(&s_cancel, 1);
            set_text(ID_INSTALL_STATUS, L"Cancelling...");
        }
        return;
    }
    GetWindowTextW(ctl(ID_IMAGE), s_job.image, MAX_PATH);
    GetWindowTextW(ctl(ID_TARGET), s_job.target, MAX_PATH);
    s_job.movies = IsDlgButtonChecked(s_wnd, ID_MOVIES) == BST_CHECKED;
    if (!s_job.image[0] || !file_exists(s_job.image)) {
        set_text(ID_INSTALL_STATUS, L"Choose your Buffy the Vampire Slayer: Chaos Bleeds disc image (ISO or XISO) first.");
        return;
    }
    if (!s_job.target[0]) {
        set_text(ID_INSTALL_STATUS, L"Choose the folder to install the game to.");
        return;
    }
    if (s_game_proc && WaitForSingleObject(s_game_proc, 0) == WAIT_TIMEOUT) {
        set_text(ID_INSTALL_STATUS, L"Close the game before reinstalling it.");
        return;
    }
    if (is_game_folder(s_job.target)) {
        if (MessageBoxW(s_wnd, L"The game is already installed in this folder. Install over it?\n\n"
                               L"The game files are replaced from the disc image. Saved games, settings and "
                               L"mods are kept.", GAME_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    WritePrivateProfileStringW(L"Launcher", L"LastImage", s_job.image, s_launcher_ini);
    s_cancel = 0;
    set_busy(1);
    SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 0, 0);
    swprintf_s(t, MAX_PATH + 200, L"Installing to %s...", s_job.target);
    set_text(ID_INSTALL_STATUS, t);
    CloseHandle(CreateThread(NULL, 0, install_thread, NULL, 0, NULL));
}

static int s_downloading;

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == ID_TAB && n->code == TCN_SELCHANGE)
            show_tab(TabCtrl_GetCurSel(s_tab));
        if (n->idFrom == ID_MODLIST && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nl = (NMLISTVIEW *)lp;
            if ((nl->uNewState & LVIS_SELECTED) && nl->iItem >= 0 && nl->iItem < s_nmods) {
                WCHAR t[700];
                Mod *md = &s_mods[nl->iItem];
                swprintf_s(t, 700, L"%s%s%s", md->desc[0] ? md->desc : L"(No description.)",
                           md->folder[0] ? L"\n\nFolder: mods\\" : L"", md->folder);
                set_text(ID_MOD_DESC, t);
            }
            if ((nl->uChanged & LVIF_STATE) && ((nl->uNewState ^ nl->uOldState) & LVIS_STATEIMAGEMASK))
                set_text(ID_MOD_STATUS, L"Mod changes are used when you press Apply mods or Play.");
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_PLAY:
            play();
            break;
        case ID_GAMEDIR_CHANGE: {
            WCHAR d[MAX_PATH];
            if (pick_folder(L"Choose the folder that holds the installed game", d)) {
                if (!is_game_folder(d)) {
                    MessageBoxW(w, L"That folder does not have the game in it (default.xbe and the Buffy folder). "
                                   L"To set the game up from your disc image, use the Install tab.",
                                GAME_TITLE, MB_OK | MB_ICONINFORMATION);
                } else {
                    wcscpy_s(s_game_dir, MAX_PATH, d);
                    save_launcher_ini();
                    settings_load();
                    refresh_play();
                }
            }
            break;
        }
        case ID_RESOLUTION:
            if (HIWORD(wp) == CBN_SELCHANGE)
                s_settings_dirty = 1, set_text(ID_SETTINGS_STATUS, L"");
            break;
        case ID_WINDOWED: case ID_FULLSCREEN: case ID_VSYNC: case ID_SKIP_INTRO: case ID_WS_SAFE: case ID_INVERT_X:
            s_settings_dirty = 1;
            set_text(ID_SETTINGS_STATUS, L"");
            break;
        case ID_DEFAULTS:
            settings_defaults();
            break;
        case ID_SAVE:
            settings_save();
            break;
        case ID_TEX_LOAD: case ID_TEX_PREFETCH: case ID_TEX_DUMP:
            tex_save();
            break;
        case ID_TEX_OPEN_LOAD: tex_open(L"load"); break;
        case ID_TEX_OPEN_DUMP: tex_open(L"dump"); break;
        case ID_TEX_REFRESH: tex_refresh(); break;
        case ID_MOD_UP: mods_move(-1); break;
        case ID_MOD_DOWN: mods_move(1); break;
        case ID_MOD_REFRESH: mods_scan(); break;
        case ID_MOD_APPLY: mods_apply(); break;
        case ID_MOD_OPEN: {
            WCHAR d[MAX_PATH];
            if (!is_game_folder(s_game_dir))
                break;
            mods_dir(d);
            mkdirs(d);
            write_mods_readme(s_game_dir);
            ShellExecuteW(w, L"open", d, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case ID_IMAGE_BROWSE: {
            WCHAR p[MAX_PATH];
            if (pick_image(p)) {
                Disc d;
                WCHAR err[512];
                uint32_t id = 0;
                set_text(ID_IMAGE, p);
                if (!disc_open(&d, p, err, 512))
                    set_text(ID_INSTALL_STATUS, err);
                else if (!disc_title_id(&d, &id) || id != TITLE_ID)
                    set_text(ID_INSTALL_STATUS, L"This disc image is not Buffy the Vampire Slayer: Chaos Bleeds.");
                else {
                    WCHAR t[128];
                    swprintf_s(t, 128, L"Buffy the Vampire Slayer: Chaos Bleeds disc image found (%.1f GB).",
                               d.total / 1073741824.0);
                    set_text(ID_INSTALL_STATUS, t);
                }
                disc_close(&d);
            }
            break;
        }
        case ID_TARGET_BROWSE: {
            WCHAR p[MAX_PATH];
            if (pick_folder(L"Choose where to install the game", p))
                set_text(ID_TARGET, p);
            break;
        }
        case ID_FFMPEG_GET:
            if (s_busy)
                break;
            if (MessageBoxW(w, L"Download FFmpeg (about 150 MB) from github.com/BtbN/FFmpeg-Builds into the "
                               L"launcher's tools folder? It is only used to convert the game's movies.",
                            GAME_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                s_downloading = 1;
                set_busy(1);
                CloseHandle(CreateThread(NULL, 0, ffmpeg_download_thread, NULL, 0, NULL));
            }
            break;
        case ID_INSTALL:
            start_install();
            break;
        }
        break;
    case WM_APP_PROGRESS:
        SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, wp, 0);
        if (lp) {
            set_text(ID_INSTALL_STATUS, (WCHAR *)lp);
            free((void *)lp);
        }
        break;
    case WM_APP_DONE: {
        WCHAR *msg = (WCHAR *)lp;
        set_busy(0);
        if (s_downloading) {
            s_downloading = 0;
            set_text(ID_INSTALL_STATUS, msg ? msg : L"");
            SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 0, 0);
            update_ffmpeg_status();
        } else if (wp) {
            WCHAR t[1400];
            SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 1000, 0);
            wcscpy_s(s_game_dir, MAX_PATH, s_job.target);
            save_launcher_ini();
            settings_load();
            {
                WCHAR sp[MAX_PATH];
                settings_path(sp);
                if (!file_exists(sp))
                    settings_save();            /* the launcher's defaults */
            }
            swprintf_s(t, 1400, L"Installed in %s.%s%s", s_game_dir, msg && msg[0] ? L" " : L"", msg ? msg : L"");
            set_text(ID_INSTALL_STATUS, t);
            refresh_play();
            if (MessageBoxW(w, L"Buffy the Vampire Slayer: Chaos Bleeds is installed.\n\nPlay now?", GAME_TITLE,
                            MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                show_tab(TAB_PLAY);
                play();
            }
        } else {
            WCHAR t[1400];
            swprintf_s(t, 1400, L"The game was not installed. %s", msg ? msg : L"");
            set_text(ID_INSTALL_STATUS, t);
            SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 0, 0);
        }
        free(msg);
        break;
    }
    case WM_APP_GAMEEND:
        if (wp != 0 && wp != 1) {
            WCHAR t[300];
            swprintf_s(t, 300, L"The game stopped with code 0x%08lX. Details are in buffy_log.txt in the game folder.",
                       (unsigned long)wp);
            set_text(ID_PLAY_STATUS, t);
        } else {
            refresh_play();
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_PAINT: {
        /* The title band. */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r, tr;
        HBRUSH band = CreateSolidBrush(RGB(28, 18, 36));
        GetClientRect(w, &r);
        r.bottom = S(HEADER_H);
        FillRect(dc, &r, band);
        DeleteObject(band);
        if (s_icon)
            DrawIconEx(dc, S(16), S(12), s_icon, S(40), S(40), 0, NULL, DI_NORMAL);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 232, 214));
        SelectObject(dc, s_big);
        tr = r;
        tr.left = S(s_icon ? 66 : 20);
        tr.top = S(8);
        tr.bottom = S(36);
        DrawTextW(dc, GAME_TITLE, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SetTextColor(dc, RGB(176, 160, 186));
        SelectObject(dc, s_font);
        tr.top = S(34);
        tr.bottom = S(56);
        DrawTextW(dc, L"PC launcher", -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (s_busy && MessageBoxW(w, L"Stop the installation and close?", GAME_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        InterlockedExchange(&s_cancel, 1);
        if (s_settings_dirty && is_game_folder(s_game_dir)) {
            int r = MessageBoxW(w, L"Save the changed settings?", GAME_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL)
                return 0;
            if (r == IDYES)
                settings_save();
        }
        save_launcher_ini();
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* --capture: the window as a 24-bit BMP (for checking the layout). */
static void capture(const WCHAR *file)
{
    RECT r;
    HDC wdc, mdc;
    HBITMAP bmp;
    BITMAPINFOHEADER bi;
    BITMAPFILEHEADER fh;
    uint8_t *bits;
    int w, h, stride;
    FILE *f;
    GetWindowRect(s_wnd, &r);
    w = r.right - r.left;
    h = r.bottom - r.top;
    wdc = GetDC(s_wnd);
    mdc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, w, h);
    SelectObject(mdc, bmp);
    PrintWindow(s_wnd, mdc, 2 /* PW_RENDERFULLCONTENT */);
    stride = (w * 3 + 3) & ~3;
    bits = (uint8_t *)malloc((size_t)stride * h);
    memset(&bi, 0, sizeof bi);
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24;
    GetDIBits(mdc, bmp, 0, (UINT)h, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    memset(&fh, 0, sizeof fh);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi;
    fh.bfSize = fh.bfOffBits + (DWORD)(stride * h);
    if (!_wfopen_s(&f, file, L"wb") && f) {
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi, sizeof bi, 1, f);
        fwrite(bits, (size_t)stride * h, 1, f);
        fclose(f);
    }
    free(bits);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(s_wnd, wdc);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    NONCLIENTMETRICSW ncm;
    MSG msg;
    RECT r;
    int argc = 0, capture_tab = -1;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc), *slash, *capture_file = NULL;
    (void)prev; (void)cmd;

    s_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetModuleFileNameW(NULL, s_launcher_dir, MAX_PATH);
    slash = wcsrchr(s_launcher_dir, L'\\');
    if (slash)
        *slash = 0;
    join(s_launcher_ini, s_launcher_dir, L"launcher.ini");
    load_launcher_ini();

    /* --install <image> <folder>: install without a window (tests). */
    if (argc >= 4 && !wcscmp(argv[1], L"--install")) {
        wcscpy_s(s_job.image, MAX_PATH, argv[2]);
        wcscpy_s(s_job.target, MAX_PATH, argv[3]);
        s_job.movies = argc < 5 || wcscmp(argv[4], L"--no-movies");
        {
            int ok = install_run(&s_job);
            fwprintf(stderr, L"%s: %s\n", ok ? L"installed" : L"failed", s_install_msg);
            return ok ? 0 : 1;
        }
    }
    if (argc >= 4 && !wcscmp(argv[1], L"--capture")) {
        static const WCHAR *names[] = { L"play", L"settings", L"mods", L"textures", L"install" };
        int i;
        for (i = 0; i < TAB_COUNT; i++)
            if (!_wcsicmp(argv[2], names[i]))
                capture_tab = i;
        capture_file = argv[3];
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    s_dpi = (int)GetDpiForSystem();
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, (UINT)s_dpi);
    s_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    s_bold = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -MulDiv(15, s_dpi, 72);
    s_big = CreateFontIndirectW(&ncm.lfMessageFont);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"BuffyLauncher";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);
    r.left = 0; r.top = 0; r.right = S(620); r.bottom = S(490 + DY);
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, (UINT)s_dpi);
    s_wnd = CreateWindowExW(0, wc.lpszClassName, GAME_TITLE L" - Launcher",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    build_ui();
    refresh_play();                 /* also loads the game's icon for the header */
    show_tab(capture_tab >= 0 ? capture_tab : (is_game_folder(s_game_dir) ? TAB_PLAY : TAB_INSTALL));
    ShowWindow(s_wnd, show);
    UpdateWindow(s_wnd);
    if (capture_file) {
        MSG pm;
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 400)
            while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&pm);
                DispatchMessageW(&pm);
            }
        capture(capture_file);
        return 0;
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(s_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
