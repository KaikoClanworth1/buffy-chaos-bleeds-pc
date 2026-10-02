/* The Win32 and MSVC C runtime the port uses beyond xboxrecomp's POSIX shim,
 * on Android (include/android_win32.h). */
#include <windows.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#undef fopen

/* ── the app ── */

static char s_data_dir[512] = ".";
static int  s_display_hz = 60;

void android_set_data_dir(const char *dir) { strcpy_s(s_data_dir, sizeof s_data_dir, dir); }
const char *android_data_dir(void) { return s_data_dir; }
void android_set_display_hz(int hz) { if (hz > 0) s_display_hz = hz; }
int  android_display_hz(void) { return s_display_hz; }

int xbox_path_fix_case(char *path, size_t size);   /* (kernel_path.c) */

void android_path(const char *in, char *out, size_t n)
{
    size_t i;
    strcpy_s(out, n, in ? in : "");
    for (i = 0; out[i]; i++)
        if (out[i] == '\\')
            out[i] = '/';
    xbox_path_fix_case(out, n);            /* (Windows names are case-insensitive) */
}

void android_path_from_wide(const wchar_t *in, char *out, size_t n)
{
    char u8[1024];
    w16_to_utf8(in, u8, sizeof u8);
    android_path(u8, out, n);
}

FILE *android_fopen(const char *path, const char *mode)
{
    char p[1024];
    android_path(path, p, sizeof p);
    return fopen(p, mode);
}

/* ── the MSVC C runtime ── */

int strcpy_s(char *dst, size_t n, const char *src)
{
    size_t len;
    if (!dst || !n)
        return EINVAL;
    len = src ? strlen(src) : 0;
    if (len >= n)
        len = n - 1;
    memmove(dst, src ? src : "", len);
    dst[len] = 0;
    return 0;
}

int strncpy_s(char *dst, size_t n, const char *src, size_t count)
{
    size_t len;
    if (!dst || !n)
        return EINVAL;
    len = src ? strlen(src) : 0;
    if (count != _TRUNCATE && len > count)
        len = count;
    if (len >= n)
        len = n - 1;
    memmove(dst, src ? src : "", len);
    dst[len] = 0;
    return 0;
}

int strcat_s(char *dst, size_t n, const char *src)
{
    size_t have;
    if (!dst || !n)
        return EINVAL;
    have = strnlen(dst, n);
    return have >= n ? EINVAL : strcpy_s(dst + have, n - have, src);
}

int wcscpy_s(wchar_t *dst, size_t n, const wchar_t *src)
{
    size_t len;
    if (!dst || !n)
        return EINVAL;
    len = src ? wcslen(src) : 0;
    if (len >= n)
        len = n - 1;
    if (len)
        memmove(dst, src, len * sizeof(wchar_t));
    dst[len] = 0;
    return 0;
}

int wcsncpy_s(wchar_t *dst, size_t n, const wchar_t *src, size_t count)
{
    size_t len;
    if (!dst || !n)
        return EINVAL;
    len = src ? wcslen(src) : 0;
    if (count != _TRUNCATE && len > count)
        len = count;
    if (len >= n)
        len = n - 1;
    if (len)
        memmove(dst, src, len * sizeof(wchar_t));
    dst[len] = 0;
    return 0;
}

int wcscat_s(wchar_t *dst, size_t n, const wchar_t *src)
{
    size_t have;
    if (!dst || !n)
        return EINVAL;
    have = wcsnlen(dst, n);
    return have >= n ? EINVAL : wcscpy_s(dst + have, n - have, src);
}

int fopen_s(FILE **f, const char *path, const char *mode)
{
    *f = android_fopen(path, mode);
    return *f ? 0 : errno;
}

int _putenv_s(const char *name, const char *value)
{
    return value && *value ? setenv(name, value, 1) : unsetenv(name);
}

int _putenv(const char *kv)
{
    char name[256];
    const char *eq = strchr(kv, '=');
    size_t n;
    if (!eq)
        return -1;
    n = (size_t)(eq - kv);
    if (n >= sizeof name)
        return -1;
    memcpy(name, kv, n);
    name[n] = 0;
    return _putenv_s(name, eq + 1);
}

int _dupenv_s(char **out, size_t *len, const char *name)
{
    const char *v = getenv(name);
    *out = v ? strdup(v) : NULL;
    if (len)
        *len = v ? strlen(v) + 1 : 0;
    return 0;
}

char *_strlwr(char *s)
{
    char *p;
    for (p = s; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return s;
}

char *_strupr(char *s)
{
    char *p;
    for (p = s; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    return s;
}

/* ── files and folders ── */

static void to_filetime(time_t t, FILETIME *ft)
{
    uint64_t v = ((uint64_t)t + 11644473600ull) * 10000000ull;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

static DWORD attrs_of(const struct stat *st)
{
    return S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

DWORD GetFileAttributesA(const char *path)
{
    char p[1024];
    struct stat st;
    android_path(path, p, sizeof p);
    return stat(p, &st) == 0 ? attrs_of(&st) : INVALID_FILE_ATTRIBUTES;
}

DWORD GetFileAttributesW(const wchar_t *path)
{
    char p[1024];
    struct stat st;
    android_path_from_wide(path, p, sizeof p);
    return stat(p, &st) == 0 ? attrs_of(&st) : INVALID_FILE_ATTRIBUTES;
}

static BOOL attr_data(const char *p, void *out)
{
    WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)out;
    struct stat st;
    if (stat(p, &st) != 0)
        return FALSE;
    memset(d, 0, sizeof *d);
    d->dwFileAttributes = attrs_of(&st);
    to_filetime(st.st_mtime, &d->ftCreationTime);
    to_filetime(st.st_atime, &d->ftLastAccessTime);
    to_filetime(st.st_mtime, &d->ftLastWriteTime);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}

BOOL GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *out)
{
    char p[1024];
    (void)level;
    android_path(path, p, sizeof p);
    return attr_data(p, out);
}

BOOL GetFileAttributesExW(const wchar_t *path, GET_FILEEX_INFO_LEVELS level, void *out)
{
    char p[1024];
    (void)level;
    android_path_from_wide(path, p, sizeof p);
    return attr_data(p, out);
}

BOOL CreateDirectoryA(const char *path, void *sa)
{
    char p[1024];
    (void)sa;
    android_path(path, p, sizeof p);
    return mkdir(p, 0775) == 0;
}

BOOL CreateDirectoryW(const wchar_t *path, void *sa)
{
    char p[1024];
    (void)sa;
    android_path_from_wide(path, p, sizeof p);
    return mkdir(p, 0775) == 0;
}

BOOL RemoveDirectoryA(const char *path)
{
    char p[1024];
    android_path(path, p, sizeof p);
    return rmdir(p) == 0;
}

BOOL RemoveDirectoryW(const wchar_t *path)
{
    char p[1024];
    android_path_from_wide(path, p, sizeof p);
    return rmdir(p) == 0;
}

BOOL DeleteFileA(const char *path)
{
    char p[1024];
    android_path(path, p, sizeof p);
    return unlink(p) == 0;
}

BOOL DeleteFileW(const wchar_t *path)
{
    char p[1024];
    android_path_from_wide(path, p, sizeof p);
    return unlink(p) == 0;
}

static BOOL copy_file(const char *from, const char *to, BOOL fail_if_exists)
{
    FILE *a, *b;
    char buf[65536];
    size_t n;
    struct stat st;
    if (fail_if_exists && stat(to, &st) == 0)
        return FALSE;
    if (!(a = fopen(from, "rb")))
        return FALSE;
    if (!(b = fopen(to, "wb"))) {
        fclose(a);
        return FALSE;
    }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) {
            fclose(a);
            fclose(b);
            return FALSE;
        }
    fclose(a);
    return fclose(b) == 0;
}

static BOOL move_file(const char *from, const char *to, DWORD flags)
{
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(to, &st) == 0)
        return FALSE;
    if (rename(from, to) == 0)
        return TRUE;
    if ((flags & MOVEFILE_COPY_ALLOWED) && copy_file(from, to, FALSE))
        return unlink(from) == 0;
    return FALSE;
}

BOOL MoveFileExA(const char *from, const char *to, DWORD flags)
{
    char a[1024], b[1024];
    android_path(from, a, sizeof a);
    android_path(to, b, sizeof b);
    return move_file(a, b, flags);
}

BOOL MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags)
{
    char a[1024], b[1024];
    android_path_from_wide(from, a, sizeof a);
    android_path_from_wide(to, b, sizeof b);
    return move_file(a, b, flags);
}

BOOL CopyFileA(const char *from, const char *to, BOOL fail_if_exists)
{
    char a[1024], b[1024];
    android_path(from, a, sizeof a);
    android_path(to, b, sizeof b);
    return copy_file(a, b, fail_if_exists);
}

BOOL CopyFileW(const wchar_t *from, const wchar_t *to, BOOL fail_if_exists)
{
    char a[1024], b[1024];
    android_path_from_wide(from, a, sizeof a);
    android_path_from_wide(to, b, sizeof b);
    return copy_file(a, b, fail_if_exists);
}

/* FindFirstFile: a folder read once, matched against the pattern's last
 * part (case-insensitively, as on Windows). */
typedef struct {
    char dir[1024];
    char pat[256];
    DIR *d;
} Find;

static int find_next(Find *f, char *name, size_t n, struct stat *st)
{
    struct dirent *e;
    while ((e = readdir(f->d)) != NULL) {
        char full[1400];
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD) != 0)
            continue;
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        if (stat(full, st) != 0)
            memset(st, 0, sizeof *st);
        strcpy_s(name, n, e->d_name);
        return 1;
    }
    return 0;
}

static void fill_common(const struct stat *st, DWORD *attrs, FILETIME *c, FILETIME *a, FILETIME *w, DWORD *hi, DWORD *lo)
{
    *attrs = attrs_of(st);
    to_filetime(st->st_mtime, c);
    to_filetime(st->st_atime, a);
    to_filetime(st->st_mtime, w);
    *hi = (DWORD)((uint64_t)st->st_size >> 32);
    *lo = (DWORD)st->st_size;
}

static Find *find_open(const char *pattern)
{
    Find *f = (Find *)calloc(1, sizeof *f);
    char p[1024], *slash;
    android_path(pattern, p, sizeof p);
    slash = strrchr(p, '/');
    if (slash) {
        *slash = 0;
        strcpy_s(f->dir, sizeof f->dir, *p ? p : "/");
        strcpy_s(f->pat, sizeof f->pat, slash + 1);
    } else {
        strcpy_s(f->dir, sizeof f->dir, ".");
        strcpy_s(f->pat, sizeof f->pat, p);
    }
    if (!strcmp(f->pat, "*.*"))
        strcpy_s(f->pat, sizeof f->pat, "*");
    if (!(f->d = opendir(f->dir))) {
        free(f);
        return NULL;
    }
    return f;
}

HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd)
{
    Find *f = find_open(pattern);
    if (!f)
        return INVALID_HANDLE_VALUE;
    if (!FindNextFileA(f, fd)) {
        FindClose(f);
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd)
{
    struct stat st;
    memset(fd, 0, sizeof *fd);
    if (!find_next((Find *)h, fd->cFileName, sizeof fd->cFileName, &st))
        return FALSE;
    fill_common(&st, &fd->dwFileAttributes, &fd->ftCreationTime, &fd->ftLastAccessTime, &fd->ftLastWriteTime,
                &fd->nFileSizeHigh, &fd->nFileSizeLow);
    return TRUE;
}

HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *fd)
{
    char p[1024];
    Find *f;
    w16_to_utf8(pattern, p, sizeof p);
    if (!(f = find_open(p)))
        return INVALID_HANDLE_VALUE;
    if (!FindNextFileW(f, fd)) {
        FindClose(f);
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd)
{
    struct stat st;
    char name[MAX_PATH * 3];
    memset(fd, 0, sizeof *fd);
    if (!find_next((Find *)h, name, sizeof name, &st))
        return FALSE;
    w16_from_utf8(name, fd->cFileName, MAX_PATH);
    fill_common(&st, &fd->dwFileAttributes, &fd->ftCreationTime, &fd->ftLastAccessTime, &fd->ftLastWriteTime,
                &fd->nFileSizeHigh, &fd->nFileSizeLow);
    return TRUE;
}

BOOL FindClose(HANDLE h)
{
    Find *f = (Find *)h;
    if (!f || h == INVALID_HANDLE_VALUE)
        return FALSE;
    closedir(f->d);
    free(f);
    return TRUE;
}

/* The "executable": in the data folder, written Windows-style so the port's
 * strrchr(path, '\\') finds the folder. */
DWORD GetModuleFileNameA(HMODULE m, char *out, DWORD n)
{
    (void)m;
    snprintf(out, n, "%s\\buffy_chaos_bleeds.exe", s_data_dir);
    return (DWORD)strlen(out);
}

DWORD GetModuleFileNameW(HMODULE m, wchar_t *out, DWORD n)
{
    char a[1024];
    GetModuleFileNameA(m, a, sizeof a);
    w16_from_utf8(a, out, n);
    return (DWORD)wcslen(out);
}

DWORD GetFullPathNameA(const char *in, DWORD n, char *out, char **file_part)
{
    char *slash;
    strcpy_s(out, n, in);
    if (file_part) {
        slash = strrchr(out, '\\');
        if (!slash)
            slash = strrchr(out, '/');
        *file_part = slash ? slash + 1 : out;
    }
    return (DWORD)strlen(out);
}

/* ── settings files (GetPrivateProfileString and friends) ──
 * Read whole for each call: they are small and read rarely. One lock, so
 * the game and the GPU thread do not write the same file at once. */
static pthread_mutex_t s_ini_lock = PTHREAD_MUTEX_INITIALIZER;

static char *ini_load(const char *path)
{
    char p[1024];
    FILE *f;
    long n;
    char *buf;
    android_path(path, p, sizeof p);
    if (!(f = fopen(p, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf) {
        n = (long)fread(buf, 1, (size_t)n, f);
        buf[n] = 0;
    }
    fclose(f);
    return buf;
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
}

/* Calls fn for each line: its section, key and value (key NULL for a
 * section line), until fn returns nonzero. */
static int ini_walk(char *text, int (*fn)(void *, const char *sec, const char *key, const char *val), void *ctx)
{
    char sec[128] = "", *line = text, *next;
    while (line && *line) {
        char l[1024], *eq;
        next = strchr(line, '\n');
        strncpy_s(l, sizeof l, line, next ? (size_t)(next - line) : _TRUNCATE);
        trim(l);
        if (l[0] == '[') {
            char *close = strchr(l, ']');
            if (close)
                *close = 0;
            strcpy_s(sec, sizeof sec, l + 1);
            if (fn(ctx, sec, NULL, NULL))
                return 1;
        } else if (l[0] && l[0] != ';' && (eq = strchr(l, '=')) != NULL) {
            *eq = 0;
            trim(l);
            if (fn(ctx, sec, l, eq + 1))
                return 1;
        }
        line = next ? next + 1 : NULL;
    }
    return 0;
}

typedef struct { const char *sec, *key; char *out; DWORD n; DWORD len; int found; } IniGet;

static int ini_get_fn(void *c, const char *sec, const char *key, const char *val)
{
    IniGet *g = (IniGet *)c;
    if (!key || strcasecmp(sec, g->sec))
        return 0;
    if (!g->key) {
        /* every key in the section, each followed by a 0, then another 0 */
        size_t k = strlen(key);
        if (g->len + k + 2 <= g->n) {
            memcpy(g->out + g->len, key, k + 1);
            g->len += (DWORD)k + 1;
        }
        g->found = 1;
        return 0;
    }
    if (strcasecmp(key, g->key))
        return 0;
    strcpy_s(g->out, g->n, val);
    g->len = (DWORD)strlen(g->out);
    g->found = 1;
    return 1;
}

DWORD GetPrivateProfileStringA(const char *sec, const char *key, const char *def, char *out, DWORD n, const char *path)
{
    IniGet g;
    char *text;
    if (!out || !n)
        return 0;
    out[0] = 0;
    memset(&g, 0, sizeof g);
    g.sec = sec;
    g.key = key;
    g.out = out;
    g.n = n;
    pthread_mutex_lock(&s_ini_lock);
    text = ini_load(path);
    if (text && sec)
        ini_walk(text, ini_get_fn, &g);
    free(text);
    pthread_mutex_unlock(&s_ini_lock);
    if (!key) {
        if (g.len < n)
            out[g.len] = 0;
        return g.len;
    }
    if (!g.found)
        strcpy_s(out, n, def ? def : "");
    return (DWORD)strlen(out);
}

UINT GetPrivateProfileIntA(const char *sec, const char *key, int def, const char *path)
{
    char v[64];
    GetPrivateProfileStringA(sec, key, "", v, sizeof v, path);
    return v[0] ? (UINT)strtol(v, NULL, 10) : (UINT)def;
}

BOOL WritePrivateProfileStringA(const char *sec, const char *key, const char *val, const char *path)
{
    char *text, *out, p[1024], cur[128] = "";
    char *line, *next;
    size_t cap, o = 0;
    int in_sec = 0, done = 0, seen_sec = 0;
    FILE *f;
    if (!sec)
        return FALSE;
    pthread_mutex_lock(&s_ini_lock);
    text = ini_load(path);
    cap = (text ? strlen(text) : 0) + (key ? strlen(key) : 0) + (val ? strlen(val) : 0) + strlen(sec) + 64;
    out = (char *)malloc(cap);
    if (!out) {
        free(text);
        pthread_mutex_unlock(&s_ini_lock);
        return FALSE;
    }
#define EMIT(s, len) do { memcpy(out + o, (s), (len)); o += (len); } while (0)
    for (line = text; line && *line; line = next) {
        char l[1024], *eq;
        size_t len;
        next = strchr(line, '\n');
        len = next ? (size_t)(next - line) : strlen(line);
        if (next)
            next++;
        strncpy_s(l, sizeof l, line, len);
        trim(l);
        if (l[0] == '[') {
            char *close = strchr(l, ']');
            if (in_sec && !done && key && val) {
                o += (size_t)sprintf(out + o, "%s=%s\n", key, val);   /* the end of its section */
                done = 1;
            }
            if (close)
                *close = 0;
            strcpy_s(cur, sizeof cur, l + 1);
            in_sec = !strcasecmp(cur, sec);
            seen_sec |= in_sec;
            if (in_sec && !key)
                continue;                                   /* the section deleted */
        } else if (in_sec) {
            if (!key)
                continue;
            if ((eq = strchr(l, '=')) != NULL) {
                *eq = 0;
                trim(l);
                if (!strcasecmp(l, key)) {
                    if (val && !done)
                        o += (size_t)sprintf(out + o, "%s=%s\n", key, val);
                    done = 1;
                    continue;
                }
            }
        }
        EMIT(line, len);
        out[o++] = '\n';
    }
    if (!done && key && val) {
        if (!seen_sec)
            o += (size_t)sprintf(out + o, "[%s]\n", sec);
        else if (!in_sec)
            o += (size_t)sprintf(out + o, "[%s]\n", sec);   /* (a section split in two: still read as one) */
        o += (size_t)sprintf(out + o, "%s=%s\n", key, val);
    }
#undef EMIT
    android_path(path, p, sizeof p);
    if ((f = fopen(p, "wb")) != NULL) {
        fwrite(out, 1, o, f);
        fclose(f);
    }
    free(text);
    free(out);
    pthread_mutex_unlock(&s_ini_lock);
    return f != NULL;
}

DWORD GetPrivateProfileStringW(const wchar_t *sec, const wchar_t *key, const wchar_t *def, wchar_t *out, DWORD n, const wchar_t *path)
{
    char s[256], k[256], d[1024], p[1024], o[2048];
    DWORD r;
    w16_to_utf8(sec, s, sizeof s);
    if (key)
        w16_to_utf8(key, k, sizeof k);
    w16_to_utf8(def ? def : L"", d, sizeof d);
    w16_to_utf8(path, p, sizeof p);
    r = GetPrivateProfileStringA(s, key ? k : NULL, d, o, sizeof o, p);
    if (!key) {
        /* a 0-separated list: converted piece by piece */
        DWORD i = 0, w = 0;
        while (i < r && w + 1 < n) {
            int len = w16_from_utf8(o + i, out + w, n - w);
            w += (DWORD)len + 1;
            i += (DWORD)strlen(o + i) + 1;
        }
        if (w < n)
            out[w] = 0;
        return w;
    }
    w16_from_utf8(o, out, n);
    return (DWORD)wcslen(out);
}

UINT GetPrivateProfileIntW(const wchar_t *sec, const wchar_t *key, int def, const wchar_t *path)
{
    char s[256], k[256], p[1024];
    w16_to_utf8(sec, s, sizeof s);
    w16_to_utf8(key, k, sizeof k);
    w16_to_utf8(path, p, sizeof p);
    return GetPrivateProfileIntA(s, k, def, p);
}

BOOL WritePrivateProfileStringW(const wchar_t *sec, const wchar_t *key, const wchar_t *val, const wchar_t *path)
{
    char s[256], k[256], v[2048], p[1024];
    w16_to_utf8(sec, s, sizeof s);
    if (key)
        w16_to_utf8(key, k, sizeof k);
    if (val)
        w16_to_utf8(val, v, sizeof v);
    w16_to_utf8(path, p, sizeof p);
    return WritePrivateProfileStringA(s, key ? k : NULL, val ? v : NULL, p);
}

/* ── the display: the phone's screen, one mode ── */

BOOL EnumDisplaySettingsW(const wchar_t *device, DWORD mode, DEVMODEW *dm)
{
    (void)device;
    if (mode != ENUM_CURRENT_SETTINGS && mode != 0)
        return FALSE;
    dm->dmBitsPerPel = 32;
    dm->dmDisplayFrequency = (DWORD)s_display_hz;
    dm->dmFields = DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    return TRUE;
}

BOOL EnumDisplaySettingsA(const char *device, DWORD mode, DEVMODEA *dm)
{
    (void)device;
    return EnumDisplaySettingsW(NULL, mode, dm);
}

/* ── keyboard focus and the mouse: the app has the screen to itself ── */

HWND GetForegroundWindow(void) { return (HWND)(uintptr_t)1; }

DWORD GetWindowThreadProcessId(HWND w, DWORD *pid)
{
    (void)w;
    if (pid)
        *pid = GetCurrentProcessId();
    return 0;
}

BOOL GetCursorPos(POINT *p) { (void)p; return FALSE; }
BOOL SetCursorPos(int x, int y) { (void)x; (void)y; return FALSE; }

/* CPU time as "cycles": nanoseconds of this thread's CPU time */
BOOL QueryThreadCycleTime(HANDLE th, ULONG64 *cycles)
{
    struct timespec t;
    (void)th;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    *cycles = (ULONG64)t.tv_sec * 1000000000ull + (ULONG64)t.tv_nsec;
    return TRUE;
}

/* ── the window: the activity's surface (include/win32_window_null.h) ── */
#include <android/native_window.h>
#include "win32_window_null.h"

static int s_window_given;

HWND CreateWindowA(LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
                   HINSTANCE inst, void *param)
{
    (void)cls; (void)title; (void)style; (void)x; (void)y; (void)w; (void)h; (void)parent; (void)menu; (void)inst;
    (void)param;
    if (s_window_given++)
        return NULL;                        /* one screen: no second window */
    return (HWND)android_window();
}

BOOL GetClientRect(HWND h, RECT *r)
{
    ANativeWindow *w = (ANativeWindow *)h;
    if (!w || w != android_window())
        return FALSE;
    r->left = r->top = 0;
    r->right = ANativeWindow_getWidth(w);
    r->bottom = ANativeWindow_getHeight(w);
    return r->right > 0 && r->bottom > 0;
}

BOOL GetWindowRect(HWND h, RECT *r)
{
    return GetClientRect(h, r);
}

BOOL GetMessageA(MSG *m, HWND h, UINT lo, UINT hi)
{
    (void)m; (void)h; (void)lo; (void)hi;
    for (;;)
        Sleep(3600 * 1000);                 /* no messages on Android */
}

BOOL GetMonitorInfoA(HMONITOR m, MONITORINFO *mi)
{
    RECT r = { 0, 0, 0, 0 };
    (void)m;
    GetClientRect((HWND)android_window(), &r);
    mi->rcMonitor = mi->rcWork = r;
    mi->dwFlags = 1;
    return TRUE;
}

BOOL GetMonitorInfoW(HMONITOR m, MONITORINFO *mi)
{
    return GetMonitorInfoA(m, mi);
}

int GetSystemMetrics(int i)
{
    return i == SM_CMONITORS ? 1 : 0;
}

/* The process's start: when the library was loaded (the game is the
 * process). The renderer counts its statistics' times from it. */
static FILETIME s_process_start;

__attribute__((constructor)) static void note_start(void)
{
    GetSystemTimeAsFileTime(&s_process_start);
}

BOOL GetProcessTimes(HANDLE p, FILETIME *creation, FILETIME *exit_, FILETIME *kernel, FILETIME *user)
{
    (void)p;
    if (creation) *creation = s_process_start;
    if (exit_) memset(exit_, 0, sizeof *exit_);
    if (kernel) memset(kernel, 0, sizeof *kernel);
    if (user) memset(user, 0, sizeof *user);
    return TRUE;
}
