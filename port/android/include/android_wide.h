/* 16-bit wide strings on Android (src/android_wide.c).
 *
 * The port and the engine are built with -fshort-wchar: wchar_t is 16 bits,
 * as on Windows, so L"..." text, WCHAR and the game's own UTF-16 are the same
 * thing. Android's C library has only 32-bit wide functions, so the ones the
 * port uses are its own, under other names (the macros below), and swprintf
 * follows MSVC: %s and %ls are wide strings, %hs and %S narrow. */
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t   w16_wcslen(const wchar_t *s);
size_t   w16_wcsnlen(const wchar_t *s, size_t n);
wchar_t *w16_wcscpy(wchar_t *d, const wchar_t *s);
wchar_t *w16_wcsncpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *w16_wcscat(wchar_t *d, const wchar_t *s);
int      w16_wcscmp(const wchar_t *a, const wchar_t *b);
int      w16_wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int      w16_wcsicmp(const wchar_t *a, const wchar_t *b);
int      w16_wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *w16_wcschr(const wchar_t *s, wchar_t c);
wchar_t *w16_wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *w16_wcsstr(const wchar_t *h, const wchar_t *n);
wchar_t *w16_wcspbrk(const wchar_t *s, const wchar_t *set);
long     w16_wcstol(const wchar_t *s, wchar_t **end, int base);
unsigned long w16_wcstoul(const wchar_t *s, wchar_t **end, int base);
wchar_t *w16_wmemcpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *w16_wmemset(wchar_t *d, wchar_t c, size_t n);
wchar_t *w16_wcsdup(const wchar_t *s);
int      w16_vswprintf(wchar_t *buf, size_t n, const wchar_t *fmt, va_list ap);
int      w16_swprintf(wchar_t *buf, size_t n, const wchar_t *fmt, ...);
int      w16_fwprintf(FILE *f, const wchar_t *fmt, ...);
wchar_t  w16_towlower(wchar_t c);
wchar_t  w16_towupper(wchar_t c);
int      w16_wfopen_s(FILE **f, const wchar_t *path, const wchar_t *mode);
FILE    *w16_wfopen(const wchar_t *path, const wchar_t *mode);

/* UTF-16 <-> UTF-8 (paths, logs) */
int      w16_to_utf8(const wchar_t *w, char *out, size_t n);
int      w16_from_utf8(const char *s, wchar_t *out, size_t n);

#define wcslen      w16_wcslen
#define wcsnlen     w16_wcsnlen
#define wcscpy      w16_wcscpy
#define wcsncpy     w16_wcsncpy
#define wcscat      w16_wcscat
#define wcscmp      w16_wcscmp
#define wcsncmp     w16_wcsncmp
#define _wcsicmp    w16_wcsicmp
#define _wcsnicmp   w16_wcsnicmp
#define wcscasecmp  w16_wcsicmp
#define wcsncasecmp w16_wcsnicmp
#define wcschr      w16_wcschr
#define wcsrchr     w16_wcsrchr
#define wcsstr      w16_wcsstr
#define wcspbrk     w16_wcspbrk
#define wcstol      w16_wcstol
#define wcstoul     w16_wcstoul
#define wmemcpy     w16_wmemcpy
#define wmemset     w16_wmemset
#define _wcsdup     w16_wcsdup
#define wcsdup      w16_wcsdup
#define vswprintf   w16_vswprintf
#define swprintf    w16_swprintf
#define swprintf_s  w16_swprintf
#define _snwprintf  w16_swprintf
#define fwprintf    w16_fwprintf
#define towlower    w16_towlower
#define towupper    w16_towupper
#define _wfopen_s   w16_wfopen_s
#define _wfopen     w16_wfopen
#define _wtoi(s)    ((int)w16_wcstol((s), NULL, 10))

#ifdef __cplusplus
}
#endif
