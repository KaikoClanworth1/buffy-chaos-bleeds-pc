/*
 * Texture packs: dumping and replacing the game's textures, as Dolphin and
 * PCSX2 do. Included by nv2a_pb_d3d11.inc; gpu_texture() calls in here.
 *
 *   <root>\dump\   each texture the game uploads, once, as a PNG (Dump on)
 *   <root>\load\   PNG / DDS replacements, any size, subfolders allowed (Load on)
 *
 * A texture is named buffy_<w>x<h>_<hash>_<fmt>.png. The hash is 64 bits of
 * the texture's level-0 contents (and palette), format and size -- not its
 * address -- so it is the same every run. Any file in load\ whose name holds
 * that 16-digit hash replaces the texture, whatever else the name says.
 * Replacements get a full mip chain (box filter) and are kept for the rest of
 * the run; Prefetch loads them all on a background thread at start-up.
 * PNGs are written on a background thread, so dumping does not stall a frame.
 *
 * Cube maps are left alone.
 */
#include <wincodec.h>
#if defined(_MSC_VER)
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

/* WIC GUIDs, local so no GUID library is needed. */
static const GUID tp_CLSID_WICImagingFactory =
    { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID tp_IID_IWICImagingFactory =
    { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID tp_GUID_ContainerFormatPng =
    { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID tp_GUID_WICPixelFormat32bppBGRA =
    { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

typedef struct {
    uint64_t hash;
    WCHAR   *path;
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    int      failed;
} TpEntry;

typedef struct TpDump {
    struct TpDump *next;
    uint64_t hash;
    uint32_t w, h, fmt;
    uint32_t *px;
} TpDump;

static struct {
    WCHAR    root[MAX_PATH];
    int      dump, load, prefetch;
    int      inited;
    TpEntry *ent;                /* open addressing by hash; built once at init */
    uint32_t cap, n;
    uint64_t *dumped;            /* hashes already in dump\ (render thread only) */
    uint32_t dcap, dn;
    CRITICAL_SECTION lock;       /* entry textures, the dump queue */
    TpDump  *qhead, *qtail;
    HANDLE   qevent;
    uint32_t replaced, dumped_run, loaded;
} s_tp;

/* Called by the game before the first frame. root: the textures_replacement
 * folder. Env: BUFFY_TEX_DUMP / BUFFY_TEX_LOAD / BUFFY_TEX_PREFETCH = 0|1 and
 * BUFFY_TEX_DIR override. */
void nv2a_gpu_set_texture_pack(const WCHAR *root, int dump, int load, int prefetch)
{
    const char *e;
    wcscpy_s(s_tp.root, MAX_PATH, root ? root : L"");
    if ((e = getenv("BUFFY_TEX_DIR")) != NULL)
        MultiByteToWideChar(CP_ACP, 0, e, -1, s_tp.root, MAX_PATH);
    s_tp.dump = (e = getenv("BUFFY_TEX_DUMP")) != NULL ? atoi(e) != 0 : dump;
    s_tp.load = (e = getenv("BUFFY_TEX_LOAD")) != NULL ? atoi(e) != 0 : load;
    s_tp.prefetch = (e = getenv("BUFFY_TEX_PREFETCH")) != NULL ? atoi(e) != 0 : prefetch;
    if (!s_tp.root[0])
        s_tp.dump = s_tp.load = 0;
}

static int tp_active(void) { return s_tp.dump || s_tp.load; }

/* ── hashing ── */

static uint64_t tp_mix(const uint8_t *p, size_t n, uint64_t h)
{
    const uint64_t P = 0x9E3779B97F4A7C15ull;
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h ^= v * 0xC2B2AE3D27D4EB4Full;
        h = ((h << 31) | (h >> 33)) * P;
        p += 8; n -= 8;
    }
    while (n--) {
        h ^= *p++ * 0x165667B19E3779F9ull;
        h = ((h << 11) | (h >> 53)) * P;
    }
    return h;
}

static uint64_t tp_fmix(uint64_t h)
{
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

/* The texture's identity: format, size, level 0 (only the pixels of each row
 * of a linear texture, not its pitch padding) and palette. */
static uint64_t tp_texture_hash(const uint8_t *lvl0, uint32_t fmt, uint32_t block, uint32_t w, uint32_t h,
                                int linear, uint32_t pitch, const uint32_t *pal, uint32_t npal)
{
    uint64_t k = tp_fmix(((uint64_t)fmt << 48) ^ ((uint64_t)w << 24) ^ h ^ 0x42554646ull);
    if (linear) {
        uint32_t y, row = w * tx_bpp(fmt);
        for (y = 0; y < h; y++)
            k = tp_mix(lvl0 + (size_t)y * pitch, row, k);
    } else {
        k = tp_mix(lvl0, tx_level_bytes(fmt, block, w, h), k);
    }
    if (pal && npal)
        k = tp_mix((const uint8_t *)pal, npal * 4, k);
    return tp_fmix(k);
}

/* ── WIC ── */

static IWICImagingFactory *tp_wic(void)
{
    IWICImagingFactory *f = NULL;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);     /* S_FALSE / RPC_E_CHANGED_MODE are fine */
    if (FAILED(CoCreateInstance(&tp_CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &tp_IID_IWICImagingFactory, (void **)&f)))
        return NULL;
    return f;
}

static int tp_write_png(IWICImagingFactory *f, const WCHAR *path, const uint32_t *px, uint32_t w, uint32_t h)
{
    IWICStream *s = NULL;
    IWICBitmapEncoder *e = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *pb = NULL;
    WICPixelFormatGUID pf = tp_GUID_WICPixelFormat32bppBGRA;
    int ok = 0;
    if (FAILED(IWICImagingFactory_CreateStream(f, &s)) ||
        FAILED(IWICStream_InitializeFromFilename(s, path, GENERIC_WRITE)) ||
        FAILED(IWICImagingFactory_CreateEncoder(f, &tp_GUID_ContainerFormatPng, NULL, &e)) ||
        FAILED(IWICBitmapEncoder_Initialize(e, (IStream *)s, WICBitmapEncoderNoCache)) ||
        FAILED(IWICBitmapEncoder_CreateNewFrame(e, &fr, &pb)) ||
        FAILED(IWICBitmapFrameEncode_Initialize(fr, pb)) ||
        FAILED(IWICBitmapFrameEncode_SetSize(fr, w, h)) ||
        FAILED(IWICBitmapFrameEncode_SetPixelFormat(fr, &pf)))
        goto done;
    if (memcmp(&pf, &tp_GUID_WICPixelFormat32bppBGRA, sizeof pf))
        goto done;
    if (FAILED(IWICBitmapFrameEncode_WritePixels(fr, h, w * 4, w * 4 * h, (BYTE *)px)) ||
        FAILED(IWICBitmapFrameEncode_Commit(fr)) ||
        FAILED(IWICBitmapEncoder_Commit(e)))
        goto done;
    ok = 1;
done:
    if (pb) IPropertyBag2_Release(pb);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (e) IWICBitmapEncoder_Release(e);
    if (s) IWICStream_Release(s);
    if (!ok)
        DeleteFileW(path);
    return ok;
}

/* An image file as 32-bit BGRA (malloc'd), or NULL. */
static uint32_t *tp_read_image(IWICImagingFactory *f, const WCHAR *path, uint32_t *w, uint32_t *h)
{
    IWICBitmapDecoder *d = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    uint32_t *px = NULL;
    UINT iw = 0, ih = 0;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, &d)) ||
        FAILED(IWICBitmapDecoder_GetFrame(d, 0, &fr)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(f, &cv)) ||
        FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &tp_GUID_WICPixelFormat32bppBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(IWICFormatConverter_GetSize(cv, &iw, &ih)) ||
        !iw || !ih || iw > 16384 || ih > 16384)
        goto done;
    px = (uint32_t *)malloc((size_t)iw * ih * 4);
    if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, iw * 4, iw * 4 * ih, (BYTE *)px))) {
        free(px);
        px = NULL;
    }
    *w = iw; *h = ih;
done:
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (d) IWICBitmapDecoder_Release(d);
    return px;
}

/* ── replacements ── */

/* A file name's 16-hex-digit token, or 0. */
static uint64_t tp_name_hash(const WCHAR *name)
{
    const WCHAR *p = name;
    while (*p) {
        const WCHAR *q = p;
        uint64_t v = 0;
        int n = 0;
        while (iswxdigit(*q) && n < 17) {
            v = (v << 4) | (uint64_t)(iswdigit(*q) ? *q - L'0' : (towlower(*q) - L'a' + 10));
            q++; n++;
        }
        if (n == 16 && (*q == L'_' || *q == L'.' || *q == 0) && (p == name || p[-1] == L'_'))
            return v;
        p = q > p ? q : p + 1;
    }
    return 0;
}

static TpEntry *tp_find(uint64_t hash)
{
    uint32_t i;
    if (!s_tp.cap)
        return NULL;
    for (i = (uint32_t)hash & (s_tp.cap - 1); s_tp.ent[i].path; i = (i + 1) & (s_tp.cap - 1))
        if (s_tp.ent[i].hash == hash)
            return &s_tp.ent[i];
    return NULL;
}

static void tp_add(uint64_t hash, const WCHAR *path)
{
    uint32_t i;
    if (s_tp.n * 2 >= s_tp.cap) {
        TpEntry *old = s_tp.ent;
        uint32_t oc = s_tp.cap, k;
        s_tp.cap = oc ? oc * 2 : 1024;
        s_tp.ent = (TpEntry *)calloc(s_tp.cap, sizeof(TpEntry));
        s_tp.n = 0;
        for (k = 0; k < oc; k++)
            if (old[k].path) {
                for (i = (uint32_t)old[k].hash & (s_tp.cap - 1); s_tp.ent[i].path; i = (i + 1) & (s_tp.cap - 1)) ;
                s_tp.ent[i] = old[k];
                s_tp.n++;
            }
        free(old);
    }
    if (tp_find(hash))
        return;                                /* first one found wins */
    for (i = (uint32_t)hash & (s_tp.cap - 1); s_tp.ent[i].path; i = (i + 1) & (s_tp.cap - 1)) ;
    s_tp.ent[i].hash = hash;
    s_tp.ent[i].path = _wcsdup(path);
    s_tp.n++;
}

static void tp_scan(const WCHAR *dir, int depth)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const WCHAR *ext;
        uint64_t hash;
        if (fd.cFileName[0] == L'.')
            continue;
        swprintf_s(p, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8)
                tp_scan(p, depth + 1);
            continue;
        }
        ext = wcsrchr(fd.cFileName, L'.');
        if (!ext || (_wcsicmp(ext, L".png") && _wcsicmp(ext, L".dds")))
            continue;
        if ((hash = tp_name_hash(fd.cFileName)) != 0)
            tp_add(hash, p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* BGRA image -> immutable texture with a full box-filtered mip chain. */
static int tp_make_texture(uint32_t *px, uint32_t w, uint32_t h, ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sub[15];
    uint32_t *lv[15];
    uint32_t levels = 1, l, lw = w, lh = h;
    int ok = 0;
    while ((lw > 1 || lh > 1) && levels < 15) {
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
        levels++;
    }
    lv[0] = px;
    lw = w; lh = h;
    for (l = 1; l < levels; l++) {
        uint32_t nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1, x, y;
        const uint32_t *s = lv[l - 1];
        uint32_t *d = (uint32_t *)malloc((size_t)nw * nh * 4);
        if (!d) { levels = l; break; }
        for (y = 0; y < nh; y++)
            for (x = 0; x < nw; x++) {
                uint32_t x0 = x * 2 < lw ? x * 2 : lw - 1, x1 = x * 2 + 1 < lw ? x * 2 + 1 : x0;
                uint32_t y0 = y * 2 < lh ? y * 2 : lh - 1, y1 = y * 2 + 1 < lh ? y * 2 + 1 : y0;
                uint32_t a = s[y0 * lw + x0], b = s[y0 * lw + x1], c = s[y1 * lw + x0], e = s[y1 * lw + x1];
                uint32_t out = 0, sh;
                for (sh = 0; sh < 32; sh += 8)
                    out |= ((((a >> sh) & 255) + ((b >> sh) & 255) + ((c >> sh) & 255) + ((e >> sh) & 255) + 2) / 4) << sh;
                d[y * nw + x] = out;
            }
        lv[l] = d;
        lw = nw; lh = nh;
    }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = levels; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    lw = w;
    for (l = 0; l < levels; l++) {
        sub[l].pSysMem = lv[l];
        sub[l].SysMemPitch = lw * 4;
        sub[l].SysMemSlicePitch = 0;
        lw = lw > 1 ? lw / 2 : 1;
    }
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, sub, tex))) {
        if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)*tex, NULL, srv)))
            ok = 1;
        else {
            ID3D11Texture2D_Release(*tex);
            *tex = NULL;
        }
    }
    for (l = 1; l < levels; l++)
        free(lv[l]);
    return ok;
}

/* Decode and upload one entry (any thread; the device is free-threaded). */
static void tp_load_entry(IWICImagingFactory *f, TpEntry *e)
{
    uint32_t w = 0, h = 0, *px;
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    int ok = 0;
    px = f ? tp_read_image(f, e->path, &w, &h) : NULL;
    if (px) {
        ok = tp_make_texture(px, w, h, &tex, &srv);
        free(px);
    }
    EnterCriticalSection(&s_tp.lock);
    if (e->srv || e->failed) {
        if (srv) ID3D11ShaderResourceView_Release(srv);
        if (tex) ID3D11Texture2D_Release(tex);
    } else if (ok) {
        e->tex = tex;
        e->srv = srv;
        s_tp.loaded++;
    } else {
        e->failed = 1;
        fwprintf(stderr, L"  [TEXPACK] could not load %s\n", e->path);
    }
    LeaveCriticalSection(&s_tp.lock);
}

static DWORD WINAPI tp_prefetch_thread(LPVOID arg)
{
    IWICImagingFactory *f = tp_wic();
    uint32_t i;
    DWORD t0 = GetTickCount();
    (void)arg;
    for (i = 0; i < s_tp.cap; i++)
        if (s_tp.ent[i].path && !s_tp.ent[i].srv && !s_tp.ent[i].failed)
            tp_load_entry(f, &s_tp.ent[i]);
    fprintf(stderr, "  [TEXPACK] prefetched %u replacements in %lu ms\n", s_tp.loaded, GetTickCount() - t0);
    if (f) IWICImagingFactory_Release(f);
    return 0;
}

/* ── dumping ── */

static int tp_dumped(uint64_t hash, int add)
{
    uint32_t i;
    if (s_tp.dcap) {
        for (i = (uint32_t)hash & (s_tp.dcap - 1); s_tp.dumped[i]; i = (i + 1) & (s_tp.dcap - 1))
            if (s_tp.dumped[i] == hash)
                return 1;
    }
    if (!add)
        return 0;
    if ((s_tp.dn + 1) * 2 >= s_tp.dcap) {
        uint64_t *old = s_tp.dumped;
        uint32_t oc = s_tp.dcap, k;
        s_tp.dcap = oc ? oc * 2 : 4096;
        s_tp.dumped = (uint64_t *)calloc(s_tp.dcap, sizeof(uint64_t));
        for (k = 0; k < oc; k++)
            if (old[k]) {
                for (i = (uint32_t)old[k] & (s_tp.dcap - 1); s_tp.dumped[i]; i = (i + 1) & (s_tp.dcap - 1)) ;
                s_tp.dumped[i] = old[k];
            }
        free(old);
    }
    for (i = (uint32_t)hash & (s_tp.dcap - 1); s_tp.dumped[i]; i = (i + 1) & (s_tp.dcap - 1)) ;
    s_tp.dumped[i] = hash;
    s_tp.dn++;
    return 0;
}

static void tp_scan_dumped(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%s\\*.png", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        uint64_t hash = tp_name_hash(fd.cFileName);
        if (hash)
            tp_dumped(hash, 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static DWORD WINAPI tp_dump_thread(LPVOID arg)
{
    IWICImagingFactory *f = tp_wic();
    (void)arg;
    for (;;) {
        TpDump *d;
        WaitForSingleObject(s_tp.qevent, INFINITE);
        for (;;) {
            WCHAR path[MAX_PATH];
            EnterCriticalSection(&s_tp.lock);
            d = s_tp.qhead;
            if (d) {
                s_tp.qhead = d->next;
                if (!s_tp.qhead) s_tp.qtail = NULL;
            }
            LeaveCriticalSection(&s_tp.lock);
            if (!d)
                break;
            swprintf_s(path, MAX_PATH, L"%s\\dump\\buffy_%ux%u_%016llx_%02X.png", s_tp.root, d->w, d->h,
                       (unsigned long long)d->hash, d->fmt);
            if (f && tp_write_png(f, path, d->px, d->w, d->h))
                InterlockedIncrement((volatile LONG *)&s_tp.dumped_run);
            free(d->px);
            free(d);
        }
    }
}

/* Queue level 0 (BGRA, w x h) for dumping, once per hash. */
static void tp_dump(uint64_t hash, uint32_t fmt, uint32_t w, uint32_t h, const uint32_t *px)
{
    TpDump *d;
    if (tp_dumped(hash, 1))
        return;
    d = (TpDump *)malloc(sizeof *d);
    if (!d)
        return;
    d->px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!d->px) { free(d); return; }
    memcpy(d->px, px, (size_t)w * h * 4);
    d->next = NULL; d->hash = hash; d->w = w; d->h = h; d->fmt = fmt;
    EnterCriticalSection(&s_tp.lock);
    if (s_tp.qtail) s_tp.qtail->next = d; else s_tp.qhead = d;
    s_tp.qtail = d;
    LeaveCriticalSection(&s_tp.lock);
    SetEvent(s_tp.qevent);
}

/* ── start-up, lookup ── */

static void tp_init(void)
{
    WCHAR d[MAX_PATH];
    s_tp.inited = 1;
    if (!tp_active())
        return;
    InitializeCriticalSection(&s_tp.lock);
    CreateDirectoryW(s_tp.root, NULL);
    if (s_tp.load) {
        swprintf_s(d, MAX_PATH, L"%s\\load", s_tp.root);
        CreateDirectoryW(d, NULL);
        tp_scan(d, 0);
        fwprintf(stderr, L"  [TEXPACK] %u replacement textures in %s%s\n", s_tp.n, d,
                 s_tp.prefetch && s_tp.n ? L" (prefetching)" : L"");
        if (s_tp.prefetch && s_tp.n)
            CloseHandle(CreateThread(NULL, 0, tp_prefetch_thread, NULL, 0, NULL));
    }
    if (s_tp.dump) {
        swprintf_s(d, MAX_PATH, L"%s\\dump", s_tp.root);
        CreateDirectoryW(d, NULL);
        tp_scan_dumped(d);
        s_tp.qevent = CreateEventW(NULL, FALSE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, tp_dump_thread, NULL, 0, NULL));
        fwprintf(stderr, L"  [TEXPACK] dumping new textures to %s (%u already there)\n", d, s_tp.dn);
    }
}

/* The replacement for `hash`, AddRef'd into *tex / *srv, or 0. */
static int tp_replacement(uint64_t hash, ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv)
{
    static IWICImagingFactory *f;
    TpEntry *e;
    int ok = 0;
    if (!s_tp.load || !s_tp.n || !(e = tp_find(hash)))
        return 0;
    if (!e->srv && !e->failed) {
        if (!f) f = tp_wic();
        tp_load_entry(f, e);
    }
    EnterCriticalSection(&s_tp.lock);
    if (e->srv) {
        ID3D11Texture2D_AddRef(e->tex);
        ID3D11ShaderResourceView_AddRef(e->srv);
        *tex = e->tex;
        *srv = e->srv;
        ok = 1;
    }
    LeaveCriticalSection(&s_tp.lock);
    if (!ok)
        return 0;
    if (s_tp.replaced++ < 20 || getenv("BUFFY_TEX_LOG"))
        fwprintf(stderr, L"  [TEXPACK] replaced %016llx\n", (unsigned long long)hash);
    return 1;
}
