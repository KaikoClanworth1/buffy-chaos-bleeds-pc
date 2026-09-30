/* ── updates: the newest release on GitHub ───────────────────────────────
 *
 * Included by buffy_launcher.c. At start-up (Play tab, "Check for updates
 * when the launcher starts", on by default) a thread asks GitHub for the
 * releases, picks the newest version that has a .zip, and if it is newer than
 * this build (PORT_VERSION) offers it. Updating downloads the zip beside the
 * launcher (update\), checks its SHA-256 against the one GitHub lists, unpacks
 * it (unzip.c; tar.exe as a fallback - Wine / Proton has none) and puts its files over the launcher folder and
 * the game folder: the programs, the Visual C++ runtime, the read-me, the
 * compiled shaders and the mods that come with the release. Nothing else is
 * touched: saved games (UDATA, TDATA, SaveData), settings, launcher.ini, the
 * game's data from the disc and the player's own mods stay as they are. A mod
 * that came with the release is replaced only when the new release has a new
 * version of it (its old mod.ini is kept as mod.ini.old); which mods are on
 * lives in mods\.launcher\enabled.txt and is kept.
 *
 * Files in use (the running launcher, its runtime DLL) cannot be written but
 * can be renamed: they are moved aside as <name>.update-old, listed in
 * update\cleanup.txt and deleted by the next launcher start. The launcher then
 * restarts itself.
 *
 * (tests) BUFFY_UPDATE_API=<url> reads the release list from there instead,
 * BUFFY_UPDATE_VERSION=<x.y.z> pretends this build is that version.
 */

#define UPDATE_API  L"https://api.github.com/repos/KaikoClanworth1/buffy-chaos-bleeds-pc/releases?per_page=20"

typedef struct {
    int   ok;                       /* the list was read */
    int   newer;                    /* ...and has a newer version than this one */
    WCHAR version[32];              /* newest version, "0.3.0" */
    WCHAR name[128];                /* the release's title */
    WCHAR page[512];                /* its web page */
    WCHAR zip_url[1024];
    WCHAR sha256[65];               /* lower-case hex, or "" when GitHub gave none */
    uint64_t zip_size;
    WCHAR notes[4096];
    WCHAR error[256];
} UpdateInfo;

static UpdateInfo    s_upd;                 /* the last check (main thread) */
static volatile LONG s_upd_busy;            /* a check or update running */
static int           s_upd_manual;          /* the check was the button's: say "up to date" */

static const WCHAR *port_version(void)
{
    static WCHAR v[32];
    if (!v[0] && !GetEnvironmentVariableW(L"BUFFY_UPDATE_VERSION", v, 32))
        wcscpy_s(v, 32, PORT_VERSION);
    return v;
}

/* "v1.2.3" -> comparable number (four parts of 0-65535). */
static uint64_t version_key(const WCHAR *s)
{
    uint64_t k = 0;
    int part;
    while (*s && (*s < L'0' || *s > L'9'))
        s++;
    for (part = 0; part < 4; part++) {
        uint64_t n = 0;
        while (*s >= L'0' && *s <= L'9')
            n = n * 10 + (uint64_t)(*s++ - L'0');
        k = (k << 16) | (n > 0xFFFF ? 0xFFFF : n);
        if (*s == L'.')
            s++;
        else
            s = L"";                     /* the end, or "-beta" and the like: ignored */
    }
    return k;
}

/* ── a little JSON reading (the text stays as it is; these find things) ── */

static const char *js_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* Past one value, or NULL if it is broken. */
static const char *js_skip(const char *p)
{
    p = js_ws(p);
    if (!p || !*p)
        return NULL;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            if (*p == '\\' && p[1])
                p++;
        return *p ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = js_ws(p + 1);
        if (*p == close)
            return p + 1;
        for (;;) {
            if (close == '}') {
                p = js_skip(p);                          /* the key */
                p = js_ws(p);
                if (!p || *p != ':')
                    return NULL;
                p++;
            }
            p = js_ws(js_skip(p));
            if (!p)
                return NULL;
            if (*p == ',') {
                p++;
                continue;
            }
            return *p == close ? p + 1 : NULL;
        }
    }
    while (*p && !strchr(",}] \t\r\n", *p))
        p++;
    return p;
}

/* An object's member, or NULL. */
static const char *js_get(const char *obj, const char *key)
{
    size_t kl = strlen(key);
    const char *p = js_ws(obj);
    if (!p || *p != '{')
        return NULL;
    p = js_ws(p + 1);
    while (p && *p == '"') {
        const char *k = p + 1, *e = js_skip(p);
        int match;
        if (!e)
            return NULL;
        match = (size_t)(e - 1 - k) == kl && !memcmp(k, key, kl);
        p = js_ws(e);
        if (*p != ':')
            return NULL;
        p = js_ws(p + 1);
        if (match)
            return p;
        p = js_ws(js_skip(p));
        if (!p || *p != ',')
            return NULL;
        p = js_ws(p + 1);
    }
    return NULL;
}

/* An array's i-th value, or NULL. */
static const char *js_at(const char *arr, int i)
{
    const char *p = js_ws(arr);
    if (!p || *p != '[')
        return NULL;
    p = js_ws(p + 1);
    if (*p == ']')
        return NULL;
    while (i-- > 0) {
        p = js_ws(js_skip(p));
        if (!p || *p != ',')
            return NULL;
        p = js_ws(p + 1);
    }
    return p;
}

static void utf8_put(char **o, unsigned c)
{
    char *q = *o;
    if (c < 0x80) *q++ = (char)c;
    else if (c < 0x800) { *q++ = (char)(0xC0 | c >> 6); *q++ = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { *q++ = (char)(0xE0 | c >> 12); *q++ = (char)(0x80 | (c >> 6 & 63)); *q++ = (char)(0x80 | (c & 63)); }
    else { *q++ = (char)(0xF0 | c >> 18); *q++ = (char)(0x80 | (c >> 12 & 63)); *q++ = (char)(0x80 | (c >> 6 & 63)); *q++ = (char)(0x80 | (c & 63)); }
    *o = q;
}

static unsigned hex4(const char *p)
{
    unsigned c = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char h = p[i];
        c = c << 4 | (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
        if (!h)
            break;
    }
    return c;
}

/* A string value, unescaped, into out ("" when it is not a string). */
static void js_str(const char *v, WCHAR *out, int cap)
{
    const char *e;
    char *buf, *o;
    out[0] = 0;
    v = js_ws(v);
    if (!v || *v != '"' || !(e = js_skip(v)))
        return;
    buf = (char *)malloc((size_t)(e - v) * 2 + 8);
    if (!buf)
        return;
    for (o = buf, v++; v < e - 1; v++) {
        if (*v != '\\') {
            *o++ = *v;
            continue;
        }
        switch (*++v) {
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'b': case 'f': break;
        case 'u': {
            unsigned c = hex4(v + 1);
            v += 4;
            if (c >= 0xD800 && c < 0xDC00 && v[1] == '\\' && v[2] == 'u') {
                unsigned lo = hex4(v + 3);
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                v += 6;
            }
            utf8_put(&o, c);
            break;
        }
        default: *o++ = *v; break;                  /* \" \\ \/ */
        }
    }
    *o = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, cap))
        out[cap - 1] = 0;
    free(buf);
}

/* Progress of the update: the Play tab's line (or stderr, for --update). */
static void post_update(int percent, const WCHAR *fmt, ...)
{
    WCHAR t[256];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(t, 256, fmt, ap);
    va_end(ap);
    if (s_wnd)
        PostMessageW(s_wnd, WM_APP_UPDPROG, (WPARAM)percent, (LPARAM)wdup(t));
    else if (percent == 100 || percent % 25 == 0)
        fwprintf(stderr, L"%s\n", t);
}

/* ── HTTP (WinHTTP, the system's proxy settings) ──────────────────────── */

/* GET url: into *body (malloc'd, 0-terminated) or, with a file, into the
 * file with progress. 1 ok. */
static int http_get(const WCHAR *url, char **body, HANDLE file, uint64_t size_hint, WCHAR *err, int errn)
{
    URL_COMPONENTSW uc;
    WCHAR host[256], path[2048], ua[64];
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, sl = sizeof status, got;
    char *buf = NULL, chunk[65536];
    size_t len = 0, cap = 0;
    uint64_t total = 0;
    int ok = 0, last_pm = -1;

    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        swprintf_s(err, errn, L"Bad address.");
        return 0;
    }
    swprintf_s(ua, 64, L"BuffyLauncher/%s", PORT_VERSION);
    ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req || !WinHttpSendRequest(req, file ? WINHTTP_NO_ADDITIONAL_HEADERS
                                              : L"Accept: application/vnd.github+json\r\n",
                                    (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
             || !WinHttpReceiveResponse(req, NULL)) {
        swprintf_s(err, errn, L"No connection to GitHub (error %lu).", GetLastError());
        goto done;
    }
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sl, WINHTTP_NO_HEADER_INDEX);
    if (file && !size_hint) {
        WCHAR cl[32];
        DWORD cll = sizeof cl;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &cll,
                                WINHTTP_NO_HEADER_INDEX))
            size_hint = _wcstoui64(cl, NULL, 10);
    }
    if (status != 200) {
        swprintf_s(err, errn, status == 403 || status == 429 ? L"GitHub is busy (too many checks); try again later."
                                                             : L"GitHub answered %lu.", status);
        goto done;
    }
    for (;;) {
        if (s_cancel && file) {
            swprintf_s(err, errn, L"Stopped.");
            goto done;
        }
        if (!WinHttpReadData(req, chunk, sizeof chunk, &got)) {
            swprintf_s(err, errn, L"The download broke off (error %lu).", GetLastError());
            goto done;
        }
        if (!got)
            break;
        total += got;
        if (file) {
            DWORD w;
            if (!WriteFile(file, chunk, got, &w, NULL) || w != got) {
                swprintf_s(err, errn, L"Could not write the download (error %lu).", GetLastError());
                goto done;
            }
            if (size_hint) {
                int pm = (int)(total * 100 / size_hint);
                if (pm != last_pm) {
                    last_pm = pm;
                    if (s_http_progress)
                        s_http_progress(pm > 100 ? 100 : pm);
                    else
                        post_update(pm > 100 ? 100 : pm, L"Downloading the update... %d%%", pm > 100 ? 100 : pm);
                }
            }
        } else {
            if (len + got + 1 > cap) {
                char *g;
                cap = (len + got + 1) * 2;
                if (cap > (32u << 20) || !(g = (char *)realloc(buf, cap))) {
                    swprintf_s(err, errn, L"The answer was too big.");
                    goto done;
                }
                buf = g;
            }
            memcpy(buf + len, chunk, got);
            len += got;
        }
    }
    if (body) {
        if (!buf)
            buf = (char *)calloc(1, 1);
        else
            buf[len] = 0;
        *body = buf;
        buf = NULL;
    }
    ok = 1;
done:
    free(buf);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

/* ── the check ─────────────────────────────────────────────────────────── */

static void update_check_run(UpdateInfo *u)
{
    WCHAR api[1024];
    char *body = NULL;
    const char *rel;
    uint64_t best = 0;
    int i;
    memset(u, 0, sizeof *u);
    if (!GetEnvironmentVariableW(L"BUFFY_UPDATE_API", api, 1024))
        wcscpy_s(api, 1024, UPDATE_API);
    if (!http_get(api, &body, NULL, 0, u->error, 256))
        return;
    /* the newest version that is published (not a draft) and has a .zip */
    for (i = 0; (rel = js_at(body, i)) != NULL; i++) {
        const char *assets = js_get(rel, "assets"), *a, *d = js_get(rel, "draft");
        WCHAR tag[64];
        uint64_t key;
        int k;
        if (d && !strncmp(d, "true", 4))
            continue;
        js_str(js_get(rel, "tag_name"), tag, 64);
        key = version_key(tag);
        if (!tag[0] || key <= best)
            continue;
        for (k = 0; assets && (a = js_at(assets, k)) != NULL; k++) {
            WCHAR an[260], dg[128];
            size_t n;
            js_str(js_get(a, "name"), an, 260);
            n = wcslen(an);
            if (n < 4 || _wcsicmp(an + n - 4, L".zip"))
                continue;
            best = key;
            wcscpy_s(u->version, 32, tag[0] == L'v' || tag[0] == L'V' ? tag + 1 : tag);
            js_str(js_get(rel, "name"), u->name, 128);
            js_str(js_get(rel, "html_url"), u->page, 512);
            js_str(js_get(rel, "body"), u->notes, 4096);
            js_str(js_get(a, "browser_download_url"), u->zip_url, 1024);
            js_str(js_get(a, "digest"), dg, 128);
            u->sha256[0] = 0;
            if (!_wcsnicmp(dg, L"sha256:", 7) && wcslen(dg + 7) == 64) {
                wcscpy_s(u->sha256, 65, dg + 7);
                _wcslwr_s(u->sha256, 65);
            }
            {
                const char *sz = js_get(a, "size");
                u->zip_size = sz ? _strtoui64(sz, NULL, 10) : 0;
            }
            break;
        }
    }
    free(body);
    u->ok = 1;
    u->newer = best && best > version_key(port_version());
    if (!best)
        wcscpy_s(u->error, 256, L"No release to download was found.");
}

static DWORD WINAPI update_check_thread(LPVOID unused)
{
    UpdateInfo *u = (UpdateInfo *)malloc(sizeof *u);
    (void)unused;
    if (u)
        update_check_run(u);
    PostMessageW(s_wnd, WM_APP_UPDCHECK, 0, (LPARAM)u);
    return 0;
}

static void update_check(int manual)
{
    if (InterlockedCompareExchange(&s_upd_busy, 1, 0))
        return;
    s_upd_manual = manual;
    set_text(ID_UPD_STATUS, L"Checking for updates...");
    EnableWindow(ctl(ID_UPD_CHECK), FALSE);
    CloseHandle(CreateThread(NULL, 0, update_check_thread, NULL, 0, NULL));
}

/* ── the update ────────────────────────────────────────────────────────── */

static WCHAR s_upd_err[512];
static FILE *s_upd_cleanup;                 /* update\cleanup.txt, while updating */

static void rm_tree(const WCHAR *dir)
{
    SHFILEOPSTRUCTW op;
    WCHAR from[MAX_PATH + 2];
    if (!dir_exists(dir))
        return;
    memset(from, 0, sizeof from);
    wcscpy_s(from, MAX_PATH, dir);
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

/* Put one file in place; a file in use is renamed aside first. */
static int upd_put(const WCHAR *src, const WCHAR *dst)
{
    WCHAR old[MAX_PATH];
    if (CopyFileW(src, dst, FALSE))
        return 1;
    swprintf_s(old, MAX_PATH, L"%s.update-old", dst);
    DeleteFileW(old);
    if (file_exists(dst) && MoveFileExW(dst, old, MOVEFILE_REPLACE_EXISTING)) {
        if (s_upd_cleanup)
            fwprintf(s_upd_cleanup, L"%s\n", old);
        if (CopyFileW(src, dst, FALSE))
            return 1;
        MoveFileExW(old, dst, MOVEFILE_REPLACE_EXISTING);           /* put it back */
    }
    swprintf_s(s_upd_err, 512, L"Could not replace %s (error %lu).", dst, GetLastError());
    return 0;
}

/* Copy a folder over another: every file (missing_only: only the ones not
 * there yet). */
static int upd_tree(const WCHAR *src, const WCHAR *dst, int missing_only)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    mkdirs(dst);
    join(pat, src, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        WCHAR a[MAX_PATH], b[MAX_PATH];
        if (fd.cFileName[0] == L'.')
            continue;
        join(a, src, fd.cFileName);
        join(b, dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = upd_tree(a, b, missing_only) && ok;
        else if (!missing_only || !file_exists(b))
            ok = upd_put(a, b) && ok;
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* The release's mods: new ones copied in; one already there replaced when
 * the release has another version of it (its mod.ini kept as mod.ini.old),
 * else only its missing files added. */
static int upd_mods(const WCHAR *src, const WCHAR *dst)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    join(pat, src, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        WCHAR a[MAX_PATH], b[MAX_PATH], ia[MAX_PATH], ib[MAX_PATH], va[32], vb[32];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
            continue;
        join(a, src, fd.cFileName);
        join(b, dst, fd.cFileName);
        join(ia, a, L"mod.ini");
        join(ib, b, L"mod.ini");
        GetPrivateProfileStringW(L"Mod", L"Version", L"", va, 32, ia);
        GetPrivateProfileStringW(L"Mod", L"Version", L"", vb, 32, ib);
        if (dir_exists(b) && file_exists(ib) && wcscmp(va, vb)) {
            WCHAR keep[MAX_PATH];
            join(keep, b, L"mod.ini.old");
            CopyFileW(ib, keep, FALSE);
            ok = upd_tree(a, b, 0) && ok;
        } else {
            ok = upd_tree(a, b, 1) && ok;
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* The unpacked release over one folder. */
static int upd_install(const WCHAR *src, const WCHAR *dst, int with_launcher)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    join(pat, src, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        WCHAR a[MAX_PATH], b[MAX_PATH];
        if (fd.cFileName[0] == L'.')
            continue;
        join(a, src, fd.cFileName);
        join(b, dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!_wcsicmp(fd.cFileName, L"mods"))
                ok = upd_mods(a, b) && ok;
            else
                ok = upd_tree(a, b, 0) && ok;
        } else if (with_launcher || _wcsicmp(fd.cFileName, L"Buffy Launcher.exe") || file_exists(b)) {
            ok = upd_put(a, b) && ok;
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* The game exe open somewhere (running): it cannot be opened alone. */
static int game_exe_busy(const WCHAR *dir)
{
    WCHAR exe[MAX_PATH];
    HANDLE f;
    join(exe, dir, GAME_EXE);
    if (!file_exists(exe))
        return 0;
    f = CreateFileW(exe, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(f);
    return 0;
}

static int sha256_file(const WCHAR *path, WCHAR *hex)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    UCHAR digest[32], buf[65536];
    DWORD got;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    int ok = 0, i;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0
            && BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0) {
        ok = 1;
        while (ReadFile(f, buf, sizeof buf, &got, NULL) && got)
            if (BCryptHashData(hh, buf, got, 0) != 0)
                ok = 0;
        ok = ok && BCryptFinishHash(hh, digest, 32, 0) == 0;
    }
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    for (i = 0; ok && i < 32; i++)
        swprintf_s(hex + i * 2, 3, L"%02x", digest[i]);
    return ok;
}

/* The unpacked release's own folder (the zip has one folder at the top). */
static int find_release_root(const WCHAR *unpack, WCHAR *root)
{
    WCHAR pat[MAX_PATH], t[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    join(t, unpack, GAME_EXE);
    if (file_exists(t)) {
        wcscpy_s(root, MAX_PATH, unpack);
        return 1;
    }
    join(pat, unpack, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
            swprintf_s(root, MAX_PATH, L"%s\\%s", unpack, fd.cFileName);
            join(t, root, GAME_EXE);
            if (file_exists(t)) {
                FindClose(h);
                return 1;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

/* The whole update (a worker thread, or the --update test). 1 ok; the
 * reason in s_upd_err when not. */
static int update_apply_run(const UpdateInfo *u)
{
    WCHAR dir[MAX_PATH], zip[MAX_PATH], unpack[MAX_PATH], root[MAX_PATH], t[MAX_PATH], hex[65];
    HANDLE f;
    int ok;
    s_upd_err[0] = 0;
    if (!u->zip_url[0]) {
        wcscpy_s(s_upd_err, 512, L"There is no update to download.");
        return 0;
    }
    if (game_exe_busy(s_launcher_dir) || (s_game_dir[0] && game_exe_busy(s_game_dir))) {
        wcscpy_s(s_upd_err, 512, L"Close the game first, then update.");
        return 0;
    }
    join(dir, s_launcher_dir, L"update");
    join(zip, dir, L"download.zip");
    join(unpack, dir, L"unpack");
    rm_tree(unpack);
    if (!mkdirs(dir) && !dir_exists(dir)) {
        swprintf_s(s_upd_err, 512, L"Could not make %s. Is the launcher's folder read-only (e.g. in Program Files)?", dir);
        return 0;
    }
    /* download */
    f = CreateFileW(zip, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        swprintf_s(s_upd_err, 512, L"Could not write in %s (error %lu).", dir, GetLastError());
        return 0;
    }
    post_update(0, L"Downloading the update...");
    ok = http_get(u->zip_url, NULL, f, u->zip_size, s_upd_err, 512);
    CloseHandle(f);
    if (!ok)
        return 0;
    /* check */
    post_update(100, L"Checking the download...");
    if (u->sha256[0]) {
        if (!sha256_file(zip, hex) || wcscmp(hex, u->sha256)) {
            wcscpy_s(s_upd_err, 512, L"The download is damaged (its SHA-256 does not match the release's). Try again.");
            DeleteFileW(zip);
            return 0;
        }
    } else if (u->zip_size) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(zip, GetFileExInfoStandard, &fa)
                || (((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) != u->zip_size) {
            wcscpy_s(s_upd_err, 512, L"The download is incomplete. Try again.");
            return 0;
        }
    }
    /* unpack */
    post_update(100, L"Unpacking the update...");
    mkdirs(unpack);
    if (!unpack_zip(zip, unpack) || !find_release_root(unpack, root)) {
        wcscpy_s(s_upd_err, 512, L"The update could not be unpacked.");
        return 0;
    }
    join(t, root, L"Buffy Launcher.exe");
    if (!file_exists(t)) {
        wcscpy_s(s_upd_err, 512, L"The download is not a release of this port.");
        return 0;
    }
    /* install: the launcher's folder, then the game's if it is another */
    post_update(100, L"Installing the update...");
    join(t, dir, L"cleanup.txt");
    if (_wfopen_s(&s_upd_cleanup, t, L"a, ccs=UTF-8"))
        s_upd_cleanup = NULL;
    ok = upd_install(root, s_launcher_dir, 1);
    if (ok && s_game_dir[0] && _wcsicmp(s_game_dir, s_launcher_dir) && has_program(s_game_dir))
        ok = upd_install(root, s_game_dir, 0);
    if (s_upd_cleanup)
        fclose(s_upd_cleanup);
    s_upd_cleanup = NULL;
    rm_tree(unpack);
    DeleteFileW(zip);
    if (ok)
        WritePrivateProfileStringW(L"Launcher", L"UpdatedFrom", port_version(), s_launcher_ini);
    return ok;
}

static DWORD WINAPI update_apply_thread(LPVOID p)
{
    int ok = update_apply_run((UpdateInfo *)p);
    PostMessageW(s_wnd, WM_APP_UPDDONE, ok, (LPARAM)wdup(s_upd_err));
    return 0;
}

static void update_apply(void)
{
    if (!s_upd.newer || s_busy)
        return;
    if (InterlockedCompareExchange(&s_upd_busy, 1, 0))
        return;
    s_cancel = 0;
    EnableWindow(ctl(ID_UPD_APPLY), FALSE);
    EnableWindow(ctl(ID_UPD_CHECK), FALSE);
    EnableWindow(ctl(ID_PLAY), FALSE);
    set_text(ID_UPD_STATUS, L"Downloading the update...");
    CloseHandle(CreateThread(NULL, 0, update_apply_thread, &s_upd, 0, NULL));
}

/* The files the last update moved aside (in use then). */
static void update_cleanup(void)
{
    WCHAR list[MAX_PATH], line[MAX_PATH + 8];
    FILE *f;
    int left = 0;
    swprintf_s(list, MAX_PATH, L"%s\\update\\cleanup.txt", s_launcher_dir);
    if (_wfopen_s(&f, list, L"r, ccs=UTF-8") || !f)
        return;
    while (fgetws(line, MAX_PATH + 8, f)) {
        size_t n = wcslen(line);
        while (n && (line[n - 1] == L'\n' || line[n - 1] == L'\r'))
            line[--n] = 0;
        if (n > 11 && !wcscmp(line + n - 11, L".update-old") && file_exists(line) && !DeleteFileW(line))
            left = 1;
    }
    fclose(f);
    if (!left)
        DeleteFileW(list);
}

/* The Play tab's update lines, from s_upd. */
static void update_show(void)
{
    WCHAR t[256];
    if (s_upd_busy)
        return;
    if (s_upd.newer)
        swprintf_s(t, 256, L"Version %s is available (you have %s).", s_upd.version, port_version());
    else if (s_upd.ok && s_upd.version[0])
        swprintf_s(t, 256, L"Version %s: up to date.", port_version());
    else if (s_upd.error[0])
        swprintf_s(t, 256, L"Version %s. Could not check for updates: %s", port_version(), s_upd.error);
    else
        swprintf_s(t, 256, L"Version %s.", port_version());
    set_text(ID_UPD_STATUS, t);
    EnableWindow(ctl(ID_UPD_CHECK), TRUE);
    EnableWindow(ctl(ID_UPD_APPLY), s_upd.newer);
    EnableWindow(ctl(ID_UPD_NOTES), s_upd.page[0] != 0);
    set_text(ID_UPD_APPLY, s_upd.newer ? L"Update now" : L"Update");
}

/* WM_APP_UPDCHECK: a check finished. */
static void update_checked(UpdateInfo *u)
{
    InterlockedExchange(&s_upd_busy, 0);
    if (!u)
        return;
    s_upd = *u;
    free(u);
    update_show();
    if (s_upd.newer) {
        WCHAR skip[32], t[1400], notes[900];
        GetPrivateProfileStringW(L"Launcher", L"SkipVersion", L"", skip, 32, s_launcher_ini);
        if (!s_upd_manual && !wcscmp(skip, s_upd.version))
            return;                                   /* said no to this one: the Play tab still offers it */
        wcsncpy_s(notes, 900, s_upd.notes, _TRUNCATE);
        if (wcslen(s_upd.notes) >= 899)
            wcscat_s(notes, 900, L"...");
        swprintf_s(t, 1400, L"Version %s of the PC port is available (you have %s).%s%s\n\n"
                            L"Update now? Saved games, settings and your own mods are kept.",
                   s_upd.version, port_version(), notes[0] ? L"\n\n" : L"", notes);
        if (MessageBoxW(s_wnd, t, GAME_TITLE L" - Update", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            update_apply();
        else
            WritePrivateProfileStringW(L"Launcher", L"SkipVersion", s_upd.version, s_launcher_ini);
    } else if (s_upd_manual && s_upd.ok && s_upd.version[0]) {
        WCHAR t[200];
        swprintf_s(t, 200, L"This is the newest version (%s).", port_version());
        set_text(ID_UPD_STATUS, t);
    }
}

/* WM_APP_UPDDONE: the update finished; restart into the new launcher. */
static void update_done(int ok, WCHAR *msg)
{
    InterlockedExchange(&s_upd_busy, 0);
    EnableWindow(ctl(ID_PLAY), TRUE);
    if (!ok) {
        WCHAR t[700];
        swprintf_s(t, 700, L"The update did not finish: %s", msg ? msg : L"");
        update_show();
        set_text(ID_UPD_STATUS, t);
        free(msg);
        return;
    }
    free(msg);
    if (is_game_folder(s_game_dir))
        embed_game_icon(s_game_dir);
    {
        WCHAR exe[MAX_PATH], cmd[MAX_PATH + 16], t[200];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        swprintf_s(t, 200, L"Updated to version %s. The launcher restarts now.", s_upd.version);
        MessageBoxW(s_wnd, t, GAME_TITLE L" - Update", MB_OK | MB_ICONINFORMATION);
        join(exe, s_launcher_dir, L"Buffy Launcher.exe");
        swprintf_s(cmd, MAX_PATH + 16, L"\"%s\" --updated", exe);
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (s_settings_dirty && is_game_folder(s_game_dir))
            settings_save();
        save_launcher_ini();
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, s_launcher_dir, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            DestroyWindow(s_wnd);
        } else {
            set_text(ID_UPD_STATUS, L"Updated. Start the launcher again to use the new version.");
        }
    }
}
