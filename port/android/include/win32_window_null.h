/* Win32 windowing on Android, for the renderer's window code
 * (xboxrecomp/src/kernel/nv2a_pb_d3d11.inc).
 *
 * There is one "window": the activity's surface (an ANativeWindow, which the
 * GPU layer's Vulkan backend takes as its window). CreateWindow hands it out
 * -- the first time; a second window (player 2's) does not exist -- its
 * client rectangle is the surface's size, and everything that moves, sizes,
 * styles or decorates windows, monitors and the cursor does nothing. The
 * window thread's message loop just waits (Android delivers input to
 * android_main.c instead). Functions in src/android_win32.c. */
#pragma once
#include <windows.h>
#include "timeapi.h"

typedef void *HMONITOR, *HINSTANCE_, *HBRUSH;
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct {
    UINT cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance;
    HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName; HICON hIconSm;
} WNDCLASSEXA;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
#define CCHDEVICENAME 32
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; wchar_t szDevice[CCHDEVICENAME]; } MONITORINFOEXW;

#ifndef WM_APP
#define WM_APP 0x8000
#endif
#ifndef WM_CLOSE
#define WM_CLOSE 0x0010
#endif
#ifndef WM_SIZE
#define WM_SIZE 0x0005
#endif
#ifndef WM_ACTIVATE
#define WM_ACTIVATE 0x0006
#endif
#ifndef WM_SETCURSOR
#define WM_SETCURSOR 0x0020
#endif
#ifndef WM_ACTIVATEAPP
#define WM_ACTIVATEAPP 0x001C
#endif
#ifndef WM_SYSKEYDOWN
#define WM_SYSKEYDOWN 0x0104
#endif
#ifndef WM_SYSCHAR
#define WM_SYSCHAR 0x0106
#endif
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef VK_F11
#define VK_F11 0x7A
#endif
#define WA_INACTIVE 0
#define HTCLIENT 1
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define WS_POPUP            0x80000000L
#define WS_VISIBLE          0x10000000L
#define CW_USEDEFAULT       ((int)0x80000000)
#define GWL_STYLE           (-16)
#define HWND_TOP            ((HWND)0)
#define HWND_NOTOPMOST      ((HWND)(intptr_t)-2)
#define SWP_NOSIZE          0x0001
#define SWP_NOMOVE          0x0002
#define SWP_NOACTIVATE      0x0010
#define SWP_FRAMECHANGED    0x0020
#define SWP_NOOWNERZORDER   0x0200
#define SWP_ASYNCWINDOWPOS  0x4000
#define SW_HIDE             0
#define SW_SHOWNA           8
#define SW_MINIMIZE         6
#define SW_RESTORE          9
#define SW_SHOW             5
#define SPI_GETWORKAREA     0x0030
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_CMONITORS 80
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define MONITOR_DEFAULTTONULL     0
#define MONITOR_DEFAULTTOPRIMARY  1
#define MONITOR_DEFAULTTONEAREST  2
#define IMAGE_ICON          1
#define IDC_ARROW           ((LPCSTR)(uintptr_t)32512)
#define MAKEINTRESOURCEA(i) ((LPCSTR)(uintptr_t)(i))
#define CDS_FULLSCREEN      0x00000004
#define DISP_CHANGE_SUCCESSFUL 0

#ifdef __cplusplus
extern "C" {
#endif
void *android_window(void);                  /* the activity's ANativeWindow (android_main.c) */
HWND  CreateWindowA(LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, void *param);
BOOL  GetClientRect(HWND h, RECT *r);
BOOL  GetWindowRect(HWND h, RECT *r);
BOOL  GetMessageA(MSG *m, HWND h, UINT lo, UINT hi);
BOOL  GetMonitorInfoA(HMONITOR m, MONITORINFO *mi);
BOOL  GetMonitorInfoW(HMONITOR m, MONITORINFO *mi);
BOOL  GetProcessTimes(HANDLE p, FILETIME *creation, FILETIME *exit_, FILETIME *kernel, FILETIME *user);
int   GetSystemMetrics(int i);
#ifdef __cplusplus
}
#endif

#define RegisterClassExA(wc)                    ((void)(wc), 1)
#define AdjustWindowRect(r, s, m)               ((void)(r), TRUE)
#define SystemParametersInfoA(a, b, c, d)       FALSE
#define LoadImageA(...)                         ((HANDLE)0)
#define LoadCursor(a, b)                        ((HCURSOR)0)
#define GetModuleHandleA(n)                     ((HMODULE)0)
#define DefWindowProcA(h, m, w, l)              ((LRESULT)0)
#define SetWindowPos(...)                       TRUE
#define SetWindowLongPtrA(h, i, v)              ((LONG_PTR)0)
#define ShowWindow(h, c)                        TRUE
#define ShowWindowAsync(h, c)                   TRUE
#define IsIconic(h)                             FALSE
#define ShowCursor(s)                           ((s) ? 0 : -1)
#define SetCursor(c)                            ((HCURSOR)0)
#define ClipCursor(r)                           TRUE
#define ClientToScreen(h, p)                    TRUE
#define PostMessageA(h, m, w, l)                TRUE
#define MonitorFromWindow(h, f)                 ((HMONITOR)(uintptr_t)1)
#define MonitorFromPoint(p, f)                  ((HMONITOR)(uintptr_t)1)
#define MonitorFromRect(r, f)                   ((HMONITOR)(uintptr_t)1)
#define ChangeDisplaySettingsExW(...)           ((LONG)-1)
#define SetRect(r, a, b, c, d)                  ((r)->left = (a), (r)->top = (b), (r)->right = (c), (r)->bottom = (d), TRUE)
