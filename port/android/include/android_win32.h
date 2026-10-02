/* The Win32 and MSVC C runtime the port uses beyond xboxrecomp's POSIX shim
 * (platform/win32_compat.h), on Android. Force-included into the port and
 * the kernel (CMakeLists.txt); the functions are in src/android_win32.c and
 * src/android_wide.c.
 *
 * Paths stay as the port builds them, Windows-style (backslashes, from
 * GetModuleFileName); each function here turns them into this system's. The
 * "executable" is in the app's data folder (android_set_data_dir), so what
 * the Windows build keeps beside the exe -- settings, saves, mods, logs --
 * is kept there. */
#pragma once
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wchar.h>
#include "platform/xbox_winnt.h"
#include "android_wide.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── the app ── */
void android_set_data_dir(const char *dir);   /* where the "exe" is */
const char *android_data_dir(void);
int  android_display_hz(void);               /* the screen's refresh rate */
void android_path(const char *in, char *out, size_t n);           /* Windows-style -> this system's */
void android_path_from_wide(const wchar_t *in, char *out, size_t n);

/* ── the MSVC C runtime's _s and _ functions ── */
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define sprintf_s(buf, n, ...)   snprintf((buf), (n), __VA_ARGS__)
#define vsprintf_s(buf, n, f, a) vsnprintf((buf), (n), (f), (a))
#define _snprintf                snprintf
#define _vsnprintf               vsnprintf
#define _stricmp                 strcasecmp
#define _strnicmp                strncasecmp
#define stricmp                  strcasecmp
#define _strdup                  strdup
#define _fileno                  fileno
#define _isatty                  isatty
#define _getpid                  getpid
#define _ftelli64                ftello
#define _fseeki64                fseeko
#define _unlink                  unlink

int  strcpy_s(char *dst, size_t n, const char *src);
int  strncpy_s(char *dst, size_t n, const char *src, size_t count);
int  strcat_s(char *dst, size_t n, const char *src);
int  wcscpy_s(wchar_t *dst, size_t n, const wchar_t *src);
int  wcsncpy_s(wchar_t *dst, size_t n, const wchar_t *src, size_t count);
int  wcscat_s(wchar_t *dst, size_t n, const wchar_t *src);
int  fopen_s(FILE **f, const char *path, const char *mode);
int  _putenv(const char *kv);
int  _putenv_s(const char *name, const char *value);
int  _dupenv_s(char **out, size_t *len, const char *name);
char *_strlwr(char *s);
char *_strupr(char *s);

/* fopen with a Windows-style path */
FILE *android_fopen(const char *path, const char *mode);
#define fopen android_fopen

/* ── Win32 constants the port names ── */
#ifndef THREAD_SUSPEND_RESUME
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT    0x0008
#endif
#define INVALID_FILE_ATTRIBUTES      ((DWORD)-1)
#define FILE_ATTRIBUTE_READONLY      0x00000001u
#define FILE_ATTRIBUTE_HIDDEN        0x00000002u
#define FILE_ATTRIBUTE_ARCHIVE       0x00000020u
#define FILE_FLAG_SEQUENTIAL_SCAN    0x08000000u
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
#define THREAD_MODE_BACKGROUND_END   0x00020000
#define MOVEFILE_REPLACE_EXISTING    0x1
#define MOVEFILE_COPY_ALLOWED        0x2
#define MOVEFILE_WRITE_THROUGH       0x8
#define ENUM_CURRENT_SETTINGS        ((DWORD)-1)
#define DM_PELSWIDTH                 0x00080000
#define DM_PELSHEIGHT                0x00100000
#define DM_BITSPERPEL                0x00040000
#define DM_DISPLAYFREQUENCY          0x00400000

/* ── keyboard and mouse (a Bluetooth keyboard: android_input.c) ── */
#ifndef VK_LSHIFT
#define VK_LSHIFT   0xA0
#define VK_RSHIFT   0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU    0xA4
#define VK_RMENU    0xA5
#endif
#ifndef VK_XBUTTON1
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#endif
#ifndef VK_NUMPAD0
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#endif
#ifndef VK_F6
#define VK_F6  0x75
#define VK_F7  0x76
#define VK_F8  0x77
#define VK_F9  0x78
#define VK_F10 0x79
#endif
#ifndef VK_OEM_1
#define VK_OEM_1 0xBA
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#endif
#ifndef VK_OEM_PLUS
#define VK_OEM_PLUS   0xBB
#define VK_OEM_COMMA  0xBC
#define VK_OEM_MINUS  0xBD
#define VK_OEM_PERIOD 0xBE
#endif
HWND  GetForegroundWindow(void);
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid);
BOOL  GetCursorPos(POINT *p);
BOOL  SetCursorPos(int x, int y);
BOOL  QueryThreadCycleTime(HANDLE th, ULONG64 *cycles);

/* ── files and folders ── */
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[MAX_PATH];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    wchar_t cFileName[MAX_PATH];
    wchar_t cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;

DWORD  GetFileAttributesA(const char *path);
DWORD  GetFileAttributesW(const wchar_t *path);
BOOL   GetFileAttributesExW(const wchar_t *path, GET_FILEEX_INFO_LEVELS level, void *out);
BOOL   GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *out);
BOOL   CreateDirectoryA(const char *path, void *sa);
BOOL   CreateDirectoryW(const wchar_t *path, void *sa);
BOOL   RemoveDirectoryA(const char *path);
BOOL   RemoveDirectoryW(const wchar_t *path);
BOOL   DeleteFileA(const char *path);
BOOL   DeleteFileW(const wchar_t *path);
BOOL   MoveFileExA(const char *from, const char *to, DWORD flags);
BOOL   MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags);
BOOL   CopyFileA(const char *from, const char *to, BOOL fail_if_exists);
BOOL   CopyFileW(const wchar_t *from, const wchar_t *to, BOOL fail_if_exists);
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd);
HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *fd);
BOOL   FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
BOOL   FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd);
BOOL   FindClose(HANDLE h);
DWORD  GetModuleFileNameA(HMODULE m, char *out, DWORD n);
DWORD  GetModuleFileNameW(HMODULE m, wchar_t *out, DWORD n);
DWORD  GetFullPathNameA(const char *in, DWORD n, char *out, char **file_part);

/* ── settings files ── */
DWORD GetPrivateProfileStringA(const char *sec, const char *key, const char *def, char *out, DWORD n, const char *path);
UINT  GetPrivateProfileIntA(const char *sec, const char *key, int def, const char *path);
BOOL  WritePrivateProfileStringA(const char *sec, const char *key, const char *val, const char *path);
DWORD GetPrivateProfileStringW(const wchar_t *sec, const wchar_t *key, const wchar_t *def, wchar_t *out, DWORD n, const wchar_t *path);
UINT  GetPrivateProfileIntW(const wchar_t *sec, const wchar_t *key, int def, const wchar_t *path);
BOOL  WritePrivateProfileStringW(const wchar_t *sec, const wchar_t *key, const wchar_t *val, const wchar_t *path);

/* ── the display ── */
typedef struct {
    wchar_t dmDeviceName[32];
    WORD  dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    DWORD dmFields, dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
} DEVMODEW, DEVMODEA;
BOOL EnumDisplaySettingsW(const wchar_t *device, DWORD mode, DEVMODEW *dm);
BOOL EnumDisplaySettingsA(const char *device, DWORD mode, DEVMODEA *dm);

#ifdef __cplusplus
}
#endif
