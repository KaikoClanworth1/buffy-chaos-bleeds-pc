/* 16-bit wide strings on Android (include/android_wide.h): the wide C
 * library functions the port uses, for -fshort-wchar's 16-bit wchar_t, and
 * UTF-16 <-> UTF-8. */
#include <windows.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

size_t w16_wcslen(const wchar_t *s)
{
    const wchar_t *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t w16_wcsnlen(const wchar_t *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i])
        i++;
    return i;
}

wchar_t *w16_wcscpy(wchar_t *d, const wchar_t *s)
{
    wchar_t *r = d;
    while ((*d++ = *s++) != 0)
        ;
    return r;
}

wchar_t *w16_wcsncpy(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

wchar_t *w16_wcscat(wchar_t *d, const wchar_t *s)
{
    w16_wcscpy(d + w16_wcslen(d), s);
    return d;
}

int w16_wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (int)*a - (int)*b;
}

int w16_wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (int)*a - (int)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

wchar_t w16_towlower(wchar_t c) { return c < 128 ? (wchar_t)tolower(c) : c; }
wchar_t w16_towupper(wchar_t c) { return c < 128 ? (wchar_t)toupper(c) : c; }

int w16_wcsicmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && w16_towlower(*a) == w16_towlower(*b))
        a++, b++;
    return (int)w16_towlower(*a) - (int)w16_towlower(*b);
}

int w16_wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (w16_towlower(*a) != w16_towlower(*b))
            return (int)w16_towlower(*a) - (int)w16_towlower(*b);
        if (!*a)
            return 0;
    }
    return 0;
}

wchar_t *w16_wcschr(const wchar_t *s, wchar_t c)
{
    for (;; s++) {
        if (*s == c)
            return (wchar_t *)s;
        if (!*s)
            return NULL;
    }
}

wchar_t *w16_wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *r = NULL;
    for (;; s++) {
        if (*s == c)
            r = s;
        if (!*s)
            return (wchar_t *)r;
    }
}

wchar_t *w16_wcsstr(const wchar_t *h, const wchar_t *n)
{
    size_t ln = w16_wcslen(n);
    if (!ln)
        return (wchar_t *)h;
    for (; *h; h++)
        if (!w16_wcsncmp(h, n, ln))
            return (wchar_t *)h;
    return NULL;
}

wchar_t *w16_wcspbrk(const wchar_t *s, const wchar_t *set)
{
    for (; *s; s++)
        if (w16_wcschr(set, *s))
            return (wchar_t *)s;
    return NULL;
}

static unsigned long long w16_strtoull(const wchar_t *s, wchar_t **end, int base, int *neg)
{
    unsigned long long v = 0;
    const wchar_t *p = s;
    *neg = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '-' || *p == '+')
        *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2, base = 16;
    else if (base == 0)
        base = p[0] == '0' ? 8 : 10;
    for (;; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'z' ? *p - 'a' + 10
              : *p >= 'A' && *p <= 'Z' ? *p - 'A' + 10 : 99;
        if (d >= base)
            break;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end)
        *end = (wchar_t *)p;
    return v;
}

long w16_wcstol(const wchar_t *s, wchar_t **end, int base)
{
    int neg;
    unsigned long long v = w16_strtoull(s, end, base, &neg);
    return neg ? -(long)v : (long)v;
}

unsigned long w16_wcstoul(const wchar_t *s, wchar_t **end, int base)
{
    int neg;
    unsigned long long v = w16_strtoull(s, end, base, &neg);
    return neg ? (unsigned long)-(long)v : (unsigned long)v;
}

wchar_t *w16_wmemcpy(wchar_t *d, const wchar_t *s, size_t n)
{
    memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

wchar_t *w16_wmemset(wchar_t *d, wchar_t c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        d[i] = c;
    return d;
}

wchar_t *w16_wcsdup(const wchar_t *s)
{
    size_t n = (w16_wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *d = (wchar_t *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

/* ── UTF-16 <-> UTF-8 ── */

int w16_to_utf8(const wchar_t *w, char *out, size_t n)
{
    size_t o = 0;
    if (!n)
        return 0;
    for (; w && *w; w++) {
        unsigned c = *w;
        char b[4];
        int k = 0, i;
        if (c >= 0xD800 && c < 0xDC00 && w[1] >= 0xDC00 && w[1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (unsigned)(w[1] - 0xDC00);
            w++;
        }
        if (c < 0x80)
            b[k++] = (char)c;
        else if (c < 0x800)
            b[k++] = (char)(0xC0 | (c >> 6)), b[k++] = (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000)
            b[k++] = (char)(0xE0 | (c >> 12)), b[k++] = (char)(0x80 | ((c >> 6) & 0x3F)),
            b[k++] = (char)(0x80 | (c & 0x3F));
        else
            b[k++] = (char)(0xF0 | (c >> 18)), b[k++] = (char)(0x80 | ((c >> 12) & 0x3F)),
            b[k++] = (char)(0x80 | ((c >> 6) & 0x3F)), b[k++] = (char)(0x80 | (c & 0x3F));
        if (o + (size_t)k >= n)
            break;
        for (i = 0; i < k; i++)
            out[o++] = b[i];
    }
    out[o] = 0;
    return (int)o;
}

int w16_from_utf8(const char *s, wchar_t *out, size_t n)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    if (!n)
        return 0;
    while (p && *p && o + 1 < n) {
        unsigned c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) {
            c = ((c & 7) << 18) | ((p[0] & 0x3Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            p += 3;
        } else if (c >= 0xE0 && p[0] && p[1]) {
            c = ((c & 15) << 12) | ((p[0] & 0x3Fu) << 6) | (p[1] & 0x3Fu);
            p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = ((c & 31) << 6) | (p[0] & 0x3Fu);
            p += 1;
        }
        if (c >= 0x10000) {
            if (o + 2 >= n)
                break;
            c -= 0x10000;
            out[o++] = (wchar_t)(0xD800 + (c >> 10));
            out[o++] = (wchar_t)(0xDC00 + (c & 0x3FF));
        } else {
            out[o++] = (wchar_t)c;
        }
    }
    out[o] = 0;
    return (int)o;
}

/* ── swprintf, MSVC's way: %s / %ls wide, %hs / %S narrow ──
 * Each conversion is formatted by snprintf on its own (strings converted
 * through UTF-8 first) and the pieces put together. */
int w16_vswprintf(wchar_t *buf, size_t n, const wchar_t *fmt, va_list ap)
{
    size_t o = 0;
    const wchar_t *f = fmt;
    char spec[32], piece[512];
    if (!buf || !n)
        return -1;
#define PUT(c) do { if (o + 1 < n) buf[o] = (wchar_t)(c); o++; } while (0)
    while (*f) {
        int k = 0, longs = 0, shorts = 0, upper_s = 0;
        char conv;
        if (*f != '%') {
            PUT(*f++);
            continue;
        }
        if (f[1] == '%') {
            PUT('%');
            f += 2;
            continue;
        }
        spec[k++] = (char)*f++;
        /* flags, width, precision (a * takes an int argument) */
        while (*f && wcschr(L"-+ #0123456789.*", *f) && k < 20) {
            if (*f == '*') {
                k += snprintf(spec + k, sizeof spec - (size_t)k, "%d", va_arg(ap, int));
                f++;
            } else {
                spec[k++] = (char)*f++;
            }
        }
        /* length */
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'I' || *f == 'j' || *f == 't'
               || (*f == '6' && f[-1] == 'I') || (*f == '4' && f[-1] == '6')) {
            if (*f == 'l') longs++;
            if (*f == 'h') shorts++;
            if (*f == 'z' || *f == 'j' || *f == 't') longs = 2;
            if (*f == 'I' && f[1] == '6' && f[2] == '4') { longs = 2; f += 2; }
            f++;
        }
        conv = (char)*f;
        if (!*f)
            break;
        f++;
        if (conv == 'S') {
            upper_s = 1;
            conv = 's';
        }
        piece[0] = 0;
        if (conv == 's') {
            spec[k++] = 's';
            spec[k] = 0;
            if (shorts || upper_s) {
                const char *s = va_arg(ap, const char *);
                snprintf(piece, sizeof piece, spec, s ? s : "(null)");
            } else {
                const wchar_t *w = va_arg(ap, const wchar_t *);
                char u8[512];
                w16_to_utf8(w ? w : L"(null)", u8, sizeof u8);
                snprintf(piece, sizeof piece, spec, u8);
            }
            {
                wchar_t wp[512];
                int i;
                w16_from_utf8(piece, wp, 512);
                for (i = 0; wp[i]; i++)
                    PUT(wp[i]);
            }
            continue;
        } else if (conv == 'c' || conv == 'C') {
            int c = va_arg(ap, int);
            PUT(c);
            continue;
        } else if (strchr("diouxX", conv)) {
            if (longs >= 2) {
                spec[k++] = 'l', spec[k++] = 'l', spec[k++] = conv, spec[k] = 0;
                snprintf(piece, sizeof piece, spec, va_arg(ap, long long));
            } else if (longs == 1) {
                /* long is 32 bits in the Windows code this came from */
                spec[k++] = conv, spec[k] = 0;
                snprintf(piece, sizeof piece, spec, (int)va_arg(ap, long));
            } else {
                spec[k++] = conv, spec[k] = 0;
                snprintf(piece, sizeof piece, spec, va_arg(ap, int));
            }
        } else if (strchr("feEgGaA", conv)) {
            spec[k++] = conv, spec[k] = 0;
            snprintf(piece, sizeof piece, spec, va_arg(ap, double));
        } else if (conv == 'p') {
            spec[k++] = 'p', spec[k] = 0;
            snprintf(piece, sizeof piece, spec, va_arg(ap, void *));
        } else {
            continue;
        }
        {
            int i;
            for (i = 0; piece[i]; i++)
                PUT((unsigned char)piece[i]);
        }
    }
#undef PUT
    buf[o < n ? o : n - 1] = 0;
    return o < n ? (int)o : -1;
}

int w16_swprintf(wchar_t *buf, size_t n, const wchar_t *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = w16_vswprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* fwprintf: formatted as UTF-16, written as UTF-8 (bionic's wide stdio
 * reads 32-bit characters) */
int w16_fwprintf(FILE *f, const wchar_t *fmt, ...)
{
    wchar_t w[2048];
    char s[6144];
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = w16_vswprintf(w, sizeof w / sizeof w[0], fmt, ap);
    va_end(ap);
    if (r < 0)
        return r;
    w16_to_utf8(w, s, sizeof s);
    return fputs(s, f) < 0 ? -1 : r;
}

/* a Windows path (wide, backslashes) as this system's */
FILE *w16_wfopen(const wchar_t *path, const wchar_t *mode)
{
    char p[1024], m[16];
    android_path_from_wide(path, p, sizeof p);
    w16_to_utf8(mode, m, sizeof m);
    return fopen(p, m);
}

int w16_wfopen_s(FILE **f, const wchar_t *path, const wchar_t *mode)
{
    *f = w16_wfopen(path, mode);
    return *f ? 0 : errno;
}
