/**
 * Saves as plain PC files: SaveData\<name>.sav beside the exe, one file a save
 * (buffy_savefmt.h), instead of the Xbox hard disk's UDATA\56550005\<id>\
 * folders with their SaveMeta / image files.
 *
 * The game saves through four XAPI calls, each made from one place in its
 * EXMemCard (0x000FFF90 finds a save by name, 0x001006B0 saves):
 *
 *   XCreateSaveGame("U:\", L"BUFFY A", OPEN_ALWAYS, 0, path, MAX_PATH)
 *       then CreateFileA(path + "BUFFY A"), data padded to 512 + signature
 *   XFindFirstSaveGame("U:\", &find) / XFindNextSaveGame / XFindClose
 *       find.szSaveGameName is compared with the name, and the file opened
 *       from find.szSaveGameDirectory + name
 *
 * They are answered here: a save's directory is "U:\<name>\", and the path
 * layer (xbox_path_set_user_mapper) sends "U:\<name>\<name>" to
 * SaveData\<name>.sav. The data is the game's own, byte for byte.
 *
 * Saves from versions before 0.4 are converted at start-up (buffy_savefmt.c).
 * BUFFY_SAVE_DIR=<folder> keeps a test run's saves apart.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"
#include "buffy_savefmt.h"

void xbox_path_set_user_mapper(BOOL (*mapper)(const char *rest, WCHAR *out, DWORD n));
void xbox_path_set_hdd_dir(const char *dir);

#define FIND_HANDLE   0x5AFE0000u         /* + slot: a save search's handle */
#define MAX_SAVES     64

static WCHAR s_dir[MAX_PATH];             /* SaveData */

static void ret_stdcall(uint32_t result, uint32_t arg_bytes)
{
    g_eax = result;
    g_esp += 4 + arg_bytes;
}

static int log_on(void)
{
    return getenv("BUFFY_SAVES_LOG") != NULL;
}

/* A guest char string, to at most n - 1 chars. */
static void guest_str(uint32_t p, char *out, int n)
{
    int i = 0;
    while (p && i < n - 1 && MEM8(p + (uint32_t)i)) {
        out[i] = (char)MEM8(p + (uint32_t)i);
        i++;
    }
    out[i] = 0;
}

static void save_path(const WCHAR *name, WCHAR *out)
{
    WCHAR file[160];
    savefmt_file_name(name, file, 160);
    swprintf_s(out, MAX_PATH, L"%s\\%s", s_dir, file);
}

/* ── the path layer: U:\<name>\<file> -> SaveData\<name>.sav ─────────── */

static BOOL user_mapper(const char *rest, WCHAR *out, DWORD n)
{
    char name[160];
    const char *sep = strchr(rest, '\\');
    WCHAR wname[160], wfile[160], file[200];
    size_t k;
    if (!sep || !sep[1] || strchr(sep + 1, '\\'))
        return FALSE;                                    /* U:\ itself, or a folder */
    k = (size_t)(sep - rest);
    if (k >= sizeof name)
        return FALSE;
    memcpy(name, rest, k);
    name[k] = 0;
    MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 160);
    MultiByteToWideChar(CP_ACP, 0, sep + 1, -1, wfile, 160);
    if (!_wcsicmp(wname, wfile)) {
        save_path(wname, out);
    } else {
        /* another file of the save (the game writes none): <name>.<file>.dat */
        WCHAR base[160];
        savefmt_file_name(wname, base, 160);
        base[wcslen(base) - 4] = 0;
        savefmt_file_name(wfile, file, 200);
        file[wcslen(file) - 4] = 0;
        swprintf_s(out, n, L"%s\\%s.%s.dat", s_dir, base, file);
    }
    return TRUE;
}

/* ── start-up ─────────────────────────────────────────────────────────── */

/* Before xbox_path_init: the old layout converted, SaveData\ for the saves,
 * the hard disk's folders in XboxData\ (nothing Xbox-shaped left in the game
 * folder). */
void buffy_saves_init(const char *exe_dir, const char *game_dir)
{
    WCHAR we[MAX_PATH], wg[MAX_PATH];
    char hdd[MAX_PATH];
    const char *env = getenv("BUFFY_SAVE_DIR");
    int n;
    MultiByteToWideChar(CP_ACP, 0, exe_dir, -1, we, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, game_dir, -1, wg, MAX_PATH);
    for (n = 0; n < 6; n++) {
        /* no disk images any more (xbox_path_set_hdd_dir): an older build's go */
        WCHAR img[MAX_PATH];
        swprintf_s(img, MAX_PATH, L"%s\\XboxData\\Partition%d.img", we, n);
        DeleteFileW(img);
    }
    if (env && *env) {
        MultiByteToWideChar(CP_ACP, 0, env, -1, s_dir, MAX_PATH);
        CreateDirectoryW(s_dir, NULL);
    } else {
        /* the originals move to SaveBackups only in a real install (the exe
         * in the game folder), never from a test build's run on it */
        n = savefmt_migrate(wg, we, !_wcsicmp(we, wg));
        if (n)
            fprintf(stderr, "[SAVES] %d save(s) converted from the Xbox format%s\n", n < 0 ? 0 : n,
                    n < 0 ? " -- FAILED, the old saves were left as they were" : "");
        swprintf_s(s_dir, MAX_PATH, L"%s\\SaveData", we);
    }
    sprintf_s(hdd, MAX_PATH, "%s\\XboxData\\HardDisk", exe_dir);
    xbox_path_set_hdd_dir(hdd);
    xbox_path_set_user_mapper(user_mapper);
    fprintf(stderr, "[SAVES] saves in %S\n", s_dir);
}

/* ── the four XAPI calls ─────────────────────────────────────────────── */

typedef struct {
    int   used, n, next;
    WCHAR names[MAX_SAVES][128];
} Find;

/* The game's lookup by name (0x000FFF90) returns on a match without
 * XFindClose -- on the console a leaked handle. A search takes the next slot
 * round the ring, so a leaked one is simply reused. */
#define N_FIND 8
static Find s_find[N_FIND];
static int  s_find_next;

/* XGAME_FIND_DATA: WIN32_FIND_DATAA (0x140), szSaveGameDirectory[260] at
 * 0x140, WCHAR szSaveGameName[128] at 0x244. */
#define FD_SIZE   0x344u
#define FD_DIR    0x140u
#define FD_NAME   0x244u

static void fill_find(uint32_t fd, const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    char narrow[128], dir[MAX_PATH];
    uint32_t i;
    for (i = 0; i < FD_SIZE; i += 4)
        MEM32(fd + i) = 0;
    save_path(name, p);
    MEM32(fd) = FILE_ATTRIBUTE_DIRECTORY;
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &a)) {
        MEM32(fd + 4) = a.ftCreationTime.dwLowDateTime;   MEM32(fd + 8) = a.ftCreationTime.dwHighDateTime;
        MEM32(fd + 12) = a.ftLastAccessTime.dwLowDateTime; MEM32(fd + 16) = a.ftLastAccessTime.dwHighDateTime;
        MEM32(fd + 20) = a.ftLastWriteTime.dwLowDateTime;  MEM32(fd + 24) = a.ftLastWriteTime.dwHighDateTime;
    }
    WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof narrow, NULL, NULL);
    for (i = 0; narrow[i] && i < 259; i++)
        MEM8(fd + 44 + i) = (uint8_t)narrow[i];             /* cFileName */
    sprintf_s(dir, MAX_PATH, "U:\\%s\\", narrow);
    for (i = 0; dir[i] && i < 259; i++)
        MEM8(fd + FD_DIR + i) = (uint8_t)dir[i];
    for (i = 0; name[i] && i < 127; i++)
        MEM16(fd + FD_NAME + i * 2) = (uint16_t)name[i];
}

/* HANDLE XFindFirstSaveGame(LPCSTR root, PXGAME_FIND_DATA) -- 0x00129746 */
void XFindFirstSaveGame_00129746(void)
{
    char root[8];
    uint32_t fd = MEM32(g_esp + 8);
    WIN32_FIND_DATAW d;
    WCHAR pat[MAX_PATH];
    HANDLE h;
    Find *f;
    int slot, i, j;
    guest_str(MEM32(g_esp + 4), root, sizeof root);
    MEM16(fd + FD_NAME) = 0;
    slot = s_find_next;
    s_find_next = (s_find_next + 1) % N_FIND;
    f = &s_find[slot];
    if (_strnicmp(root, "U:", 2)) {
        ret_stdcall(0xFFFFFFFFu, 8);                     /* memory units: none */
        return;
    }
    memset(f, 0, sizeof *f);
    swprintf_s(pat, MAX_PATH, L"%s\\*.sav", s_dir);
    h = FindFirstFileW(pat, &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t l = wcslen(d.cFileName);
            if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || l <= 4 || l - 4 >= 128 || f->n >= MAX_SAVES)
                continue;
            wcsncpy_s(f->names[f->n], 128, d.cFileName, l - 4);
            f->n++;
        } while (FindNextFileW(h, &d));
        FindClose(h);
    }
    /* by name, as the console lists them */
    for (i = 1; i < f->n; i++)
        for (j = i; j > 0 && _wcsicmp(f->names[j - 1], f->names[j]) > 0; j--) {
            WCHAR t[128];
            wcscpy_s(t, 128, f->names[j]);
            wcscpy_s(f->names[j], 128, f->names[j - 1]);
            wcscpy_s(f->names[j - 1], 128, t);
        }
    if (log_on())
        fprintf(stderr, "[SAVES] find: %d save(s)\n", f->n);
    if (!f->n) {
        ret_stdcall(0xFFFFFFFFu, 8);
        return;
    }
    f->used = 1;
    f->next = 1;
    fill_find(fd, f->names[0]);
    ret_stdcall(FIND_HANDLE + (uint32_t)slot, 8);
}

/* BOOL XFindNextSaveGame(HANDLE, PXGAME_FIND_DATA) -- 0x00129850 */
void XFindNextSaveGame_00129850(void)
{
    uint32_t h = MEM32(g_esp + 4) - FIND_HANDLE, fd = MEM32(g_esp + 8);
    Find *f = h < N_FIND && s_find[h].used ? &s_find[h] : NULL;
    if (!f || f->next >= f->n) {
        ret_stdcall(0, 8);
        return;
    }
    fill_find(fd, f->names[f->next++]);
    ret_stdcall(1, 8);
}

/* BOOL XFindClose(HANDLE) -- 0x00129897 (only the save search uses it) */
void XFindClose_00129897(void)
{
    uint32_t h = MEM32(g_esp + 4) - FIND_HANDLE;
    if (h < N_FIND)
        s_find[h].used = 0;
    ret_stdcall(1, 4);
}

/* BOOL XMountUtilityDrive(BOOL fFormatClean) -- 0x00128E01, from XAPI's
 * start-up. On the console it picks one of the hard disk's three cache
 * partitions, checks (or formats) its file system and links Z: to it; here Z:
 * is already the folder XboxData\Cache (the path layer), so there is nothing
 * to pick, check or format: it is mounted. */
void XMountUtilityDrive_00128E01(void)
{
    ret_stdcall(1, 4);
}

/* DWORD XCreateSaveGame(LPCSTR root, LPCWSTR name, DWORD disposition,
 * DWORD flags, LPSTR path, UINT size) -- 0x001294E3 */
void XCreateSaveGame_001294E3(void)
{
    char root[8], dir[MAX_PATH], narrow[128];
    uint32_t wname = MEM32(g_esp + 8), disp = MEM32(g_esp + 12), out = MEM32(g_esp + 20);
    uint32_t size = MEM32(g_esp + 24), i;
    WCHAR name[128], safe[160], p[MAX_PATH];
    int exists;
    guest_str(MEM32(g_esp + 4), root, sizeof root);
    for (i = 0; i < 127 && MEM16(wname + i * 2); i++)
        name[i] = (WCHAR)MEM16(wname + i * 2);
    name[i] = 0;
    if (_strnicmp(root, "U:", 2) || !name[0]) {
        ret_stdcall(ERROR_PATH_NOT_FOUND, 24);
        return;
    }
    /* the directory carries the file's name as SaveData will spell it */
    savefmt_file_name(name, safe, 160);
    safe[wcslen(safe) - 4] = 0;
    save_path(name, p);
    exists = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    if (disp == CREATE_NEW && exists) {
        ret_stdcall(ERROR_ALREADY_EXISTS, 24);
        return;
    }
    if (disp == OPEN_EXISTING && !exists) {
        ret_stdcall(ERROR_FILE_NOT_FOUND, 24);
        return;
    }
    WideCharToMultiByte(CP_ACP, 0, safe, -1, narrow, sizeof narrow, NULL, NULL);
    sprintf_s(dir, MAX_PATH, "U:\\%s\\", narrow);
    if (strlen(dir) + 1 > size) {
        ret_stdcall(ERROR_INSUFFICIENT_BUFFER, 24);
        return;
    }
    for (i = 0; dir[i]; i++)
        MEM8(out + i) = (uint8_t)dir[i];
    MEM8(out + i) = 0;
    if (log_on())
        fprintf(stderr, "[SAVES] create \"%S\" (disposition %u) -> %s\n", name, disp, dir);
    ret_stdcall(0, 24);
}
