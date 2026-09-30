/*
 * buffy_export.c -- the model exporter (the launcher's Advanced tab).
 *
 * BUFFY_EXPORT_MODEL=<hash> (hex) and BUFFY_EXPORT_DIR=<folder>: once the game
 * is up, the model file is loaded (every section, so its streamed textures are
 * in memory), each of its skins is written as <folder>\<hash>_skin<n>.glb --
 * glTF 2.0, which Blender opens as it is -- and the game quits (exit code 0
 * when at least one skin was written).
 *
 * What a model file holds (all offsets in it are relative to the field that
 * holds them; "hdr" is the loaded file's header, file record +0x28):
 *   hdr +0x54/+0x58  sections: count, 0x14-byte entries (+0 the section's hash)
 *   hdr +0x64        entities: count word, then (+4, relative) 0x20-byte
 *                    entries, +0xC the entity object
 *   hdr +0x74        skins: count word, then 0x1C-byte entries, +0xC the data
 *   hdr +0x48        the texture array (runtime): blocks of 64 entries of 0x4C
 *                    bytes, +0x2C an Xbox texture (+0x30 data, +0x38 format)
 *   skin +0x1C       bone count; +0x20 pivots (16 bytes a bone); +0x28 the
 *                    hierarchy (8 bytes a bone, the parent word first); +0x30
 *                    the bone ids ((index, id) word pairs, id 0xFFFF ends)
 *   skin +0x38/+0x40 skinned pieces (count: +0x38 + +0x40) at +0x3C, 0x14
 *                    bytes each: +0 (relative) the pieces' skin info table,
 *                    +0xC the entity index (low 24 bits)
 *   skin +0x48/+0x4C rigid pieces: 0x14 bytes each, +8 the bone, +0xC the entity
 *   entity           an EXGeoSplitEntity (vtable 0x1B3FE8: +0x44 pieces, +0x48
 *                    their relative pointers) or one EXGeoEntity piece
 *   piece            +0x68 vertices, +0x4C (relative) 24 bytes each: position
 *                    (3 floats), normal (packed 11:11:10), uv (2 floats);
 *                    +0x7C a draw header whose +0xC is the draw record: 6 words,
 *                    then batches of 5 words (index count, texture, vertex
 *                    range, flags, 0), then the batches' triangle strips (u16)
 *   skin info        per piece: +0 the palette's size, +0xC the palette (skeleton
 *                    bone numbers), +8 (relative) the vertices' weights, 20
 *                    bytes each: 4 palette slots (u8), 4 weights (float)
 *
 * Axes: the Xbox's are left-handed, glTF's right-handed: z is negated and the
 * triangles are turned to face their vertex normals.
 */
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp/gen/recomp_types.h"

#if defined(_MSC_VER)
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

void EXGeoFile_LoadGeoFile_000C69C0(void);
void EXGeoHeader_LoadSectionFile_000C67D0(void);
void EXGeoCommonArray_FindIndex_000CBFB0(void);

#define SPLIT_ENTITY_VT 0x1B3FE8u

/* ── calling the game ───────────────────────────────────────────────────── */

static uint32_t call_game(void (*f)(void), uint32_t self, int n, const uint32_t *args)
{
    uint32_t esp0 = g_esp, regs[4] = { g_ebx, g_esi, g_edi, g_ebp }, r;
    int i;
    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    MEM32(g_esp) = 0;                                   /* return address */
    g_ecx = self;
    f();
    r = g_eax;
    g_esp = esp0;
    g_ebx = regs[0]; g_esi = regs[1]; g_edi = regs[2]; g_ebp = regs[3];
    return r;
}

static uint32_t rel(uint32_t p)
{
    uint32_t v = MEM32(p);
    return v ? p + v : 0;
}

static float getf(uint32_t a)
{
    float f;
    memcpy(&f, (const void *)XBOX_PTR(a), 4);
    return f;
}

/* ── growable buffers ───────────────────────────────────────────────────── */

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void *bgrow(Buf *b, size_t add)
{
    if (b->n + add > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + add)
            c *= 2;
        b->p = (uint8_t *)realloc(b->p, c);
        b->cap = c;
    }
    b->n += add;
    return b->p + b->n - add;
}

static void bput(Buf *b, const void *d, size_t n) { memcpy(bgrow(b, n), d, n); }
static void balign(Buf *b) { while (b->n & 3) *(uint8_t *)bgrow(b, 1) = 0; }

static void jprintf(Buf *j, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        bput(j, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}

/* ── textures ───────────────────────────────────────────────────────────── */

static uint32_t tx_swz(uint32_t x, uint32_t y, uint32_t w, uint32_t h)   /* Morton order (Xbox swizzle) */
{
    uint32_t off = 0, bit = 1, mw = w - 1, mh = h - 1, sx = 1, sy = 1;
    (void)sx; (void)sy;
    while (mw || mh) {
        if (mw) {
            if (x & 1) off |= bit;
            bit <<= 1; x >>= 1; mw >>= 1;
        }
        if (mh) {
            if (y & 1) off |= bit;
            bit <<= 1; y >>= 1; mh >>= 1;
        }
    }
    return off;
}

static uint32_t c565(uint32_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
}

/* A DXT block (8 bytes of colour) into 16 ARGB texels; alpha given (or 1-bit for DXT1). */
static void dxt_color(const uint8_t *p, uint32_t *out, int dxt1)
{
    uint32_t c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8, pal[4], bits = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
    int i;
    pal[0] = c565(c0) | 0xFF000000u;
    pal[1] = c565(c1) | 0xFF000000u;
    for (i = 0; i < 2; i++) {
        uint32_t a = pal[0], b = pal[1], r = 0, k;
        for (k = 0; k < 24; k += 8) {
            uint32_t ca = (a >> k) & 0xFF, cb = (b >> k) & 0xFF, v;
            if (c0 > c1 || !dxt1)
                v = i == 0 ? (2 * ca + cb) / 3 : (ca + 2 * cb) / 3;
            else
                v = i == 0 ? (ca + cb) / 2 : 0;
            r |= v << k;
        }
        pal[2 + i] = r | 0xFF000000u;
    }
    if (dxt1 && c0 <= c1)
        pal[3] = 0;                                          /* transparent */
    for (i = 0; i < 16; i++)
        out[i] = pal[(bits >> (2 * i)) & 3];
}

/* The texture as ARGB texels (w*h); 0 when its format is not one decoded here. */
static uint32_t *tx_decode(uint32_t data, uint32_t fmtw, uint32_t sizew, uint32_t *pw, uint32_t *ph)
{
    uint32_t fmt = (fmtw >> 8) & 0xFF, w = 1u << ((fmtw >> 20) & 15), h = 1u << ((fmtw >> 24) & 15), x, y, *px;
    const uint8_t *src = (const uint8_t *)XBOX_PTR((data & 0x0FFFFFFFu) | 0x80000000u);
    if (sizew) {                                             /* linear: its size in the size word */
        w = (sizew & 0xFFF) + 1;
        h = ((sizew >> 12) & 0xFFF) + 1;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return NULL;
    px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!px)
        return NULL;
    *pw = w;
    *ph = h;
    if (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == 0x0C ? 8 : 16, bx, by, blk[16];
        for (by = 0; by < bh; by++)
            for (bx = 0; bx < bw; bx++) {
                const uint8_t *b = src + (by * bw + bx) * bs;
                int i;
                if (fmt == 0x0C)
                    dxt_color(b, blk, 1);
                else {
                    dxt_color(b + 8, blk, 0);
                    for (i = 0; i < 16; i++) {
                        uint32_t a;
                        if (fmt == 0x0E)
                            a = ((b[i / 2] >> ((i & 1) * 4)) & 15) * 17;
                        else {
                            uint32_t a0 = b[0], a1 = b[1], code;
                            uint64_t bits = 0;
                            int k;
                            for (k = 0; k < 6; k++)
                                bits |= (uint64_t)b[2 + k] << (8 * k);
                            code = (uint32_t)(bits >> (3 * i)) & 7;
                            if (code == 0) a = a0;
                            else if (code == 1) a = a1;
                            else if (a0 > a1) a = ((8 - code) * a0 + (code - 1) * a1) / 7;
                            else if (code == 6) a = 0;
                            else if (code == 7) a = 255;
                            else a = ((6 - code) * a0 + (code - 1) * a1) / 5;
                        }
                        blk[i] = (blk[i] & 0x00FFFFFFu) | a << 24;
                    }
                }
                for (i = 0; i < 16; i++) {
                    uint32_t tx = bx * 4 + (i & 3), ty = by * 4 + i / 4;
                    if (tx < w && ty < h)
                        px[ty * w + tx] = blk[i];
                }
            }
        return px;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t i = sizew ? y * w + x : tx_swz(x, y, w, h), t;
            switch (fmt) {
            case 0x06: case 0x12:                        /* A8R8G8B8 */
                t = ((const uint32_t *)src)[i];
                break;
            case 0x07: case 0x1E:                        /* X8R8G8B8 */
                t = ((const uint32_t *)src)[i] | 0xFF000000u;
                break;
            case 0x05: case 0x11:                        /* R5G6B5 */
                t = c565(((const uint16_t *)src)[i]) | 0xFF000000u;
                break;
            case 0x04: case 0x1D: {                      /* A4R4G4B4 */
                uint32_t v = ((const uint16_t *)src)[i];
                t = ((v >> 12) * 17) << 24 | (((v >> 8) & 15) * 17) << 16 | (((v >> 4) & 15) * 17) << 8 | (v & 15) * 17;
                break;
            }
            case 0x02: case 0x10: case 0x03: case 0x1C: {   /* A1R5G5B5 / X1R5G5B5 */
                uint32_t v = ((const uint16_t *)src)[i], r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                t = ((v & 0x8000) || fmt == 0x03 || fmt == 0x1C ? 0xFF000000u : 0)
                    | ((r << 3) | (r >> 2)) << 16 | ((g << 3) | (g >> 2)) << 8 | ((b << 3) | (b >> 2));
                break;
            }
            case 0x00: case 0x13:                        /* Y8 */
                t = src[i] * 0x010101u | 0xFF000000u;
                break;
            case 0x19: case 0x1F:                        /* A8 */
                t = (uint32_t)src[i] << 24 | 0xFFFFFF;
                break;
            default:
                free(px);
                return NULL;
            }
            px[y * w + x] = t;
        }
    return px;
}

static const GUID ex_CLSID_WICImagingFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID ex_IID_IWICImagingFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID ex_GUID_ContainerFormatPng = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID ex_GUID_WICPixelFormat32bppBGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

/* ARGB texels (the same bytes as BGRA) as a PNG appended to `out`. */
static int png_encode(const uint32_t *px, uint32_t w, uint32_t h, Buf *out)
{
    static IWICImagingFactory *f;
    IStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    WICPixelFormatGUID pf = ex_GUID_WICPixelFormat32bppBGRA;
    STATSTG ss;
    LARGE_INTEGER zero;
    ULONG got;
    int ok = 0;
    if (!f) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&ex_CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &ex_IID_IWICImagingFactory, (void **)&f)))
            return 0;
    }
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &st)))
        return 0;
    if (SUCCEEDED(f->lpVtbl->CreateEncoder(f, &ex_GUID_ContainerFormatPng, NULL, &enc))
            && SUCCEEDED(enc->lpVtbl->Initialize(enc, st, WICBitmapEncoderNoCache))
            && SUCCEEDED(enc->lpVtbl->CreateNewFrame(enc, &fr, NULL))
            && SUCCEEDED(fr->lpVtbl->Initialize(fr, NULL))
            && SUCCEEDED(fr->lpVtbl->SetSize(fr, w, h))
            && SUCCEEDED(fr->lpVtbl->SetPixelFormat(fr, &pf))
            && SUCCEEDED(fr->lpVtbl->WritePixels(fr, h, w * 4, w * h * 4, (BYTE *)px))
            && SUCCEEDED(fr->lpVtbl->Commit(fr)) && SUCCEEDED(enc->lpVtbl->Commit(enc))
            && SUCCEEDED(st->lpVtbl->Stat(st, &ss, STATFLAG_NONAME))) {
        size_t n = (size_t)ss.cbSize.QuadPart;
        zero.QuadPart = 0;
        st->lpVtbl->Seek(st, zero, STREAM_SEEK_SET, NULL);
        if (SUCCEEDED(st->lpVtbl->Read(st, bgrow(out, n), (ULONG)n, &got)) && got == n)
            ok = 1;
        else
            out->n -= n;
    }
    if (fr) fr->lpVtbl->Release(fr);
    if (enc) enc->lpVtbl->Release(enc);
    st->lpVtbl->Release(st);
    return ok;
}

/* ── one skin as glTF ───────────────────────────────────────────────────── */

typedef struct { float p[3], n[3], uv[2], w[4]; uint16_t j[4]; } Vert;
typedef struct { int tex; uint32_t *idx; size_t n, cap; } Prim;

static Vert  *s_v;
static size_t s_nv, s_capv;
static Prim   s_prim[64];
static int    s_nprim;

static void add_vert(const Vert *v)
{
    if (s_nv == s_capv) {
        s_capv = s_capv ? s_capv * 2 : 4096;
        s_v = (Vert *)realloc(s_v, s_capv * sizeof *s_v);
    }
    s_v[s_nv++] = *v;
}

static Prim *prim_for(int tex)
{
    int i;
    for (i = 0; i < s_nprim; i++)
        if (s_prim[i].tex == tex)
            return &s_prim[i];
    if (s_nprim == 64)
        return &s_prim[63];
    memset(&s_prim[s_nprim], 0, sizeof s_prim[0]);
    s_prim[s_nprim].tex = tex;
    return &s_prim[s_nprim++];
}

static void add_tri(Prim *pr, uint32_t a, uint32_t b, uint32_t c)
{
    const float *A = s_v[a].p, *B = s_v[b].p, *C = s_v[c].p;
    float e1[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, e2[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
    float fn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] }, d = 0;
    int k;
    for (k = 0; k < 3; k++)
        d += fn[k] * (s_v[a].n[k] + s_v[b].n[k] + s_v[c].n[k]);
    if (d < 0) {
        uint32_t t = b;
        b = c;
        c = t;
    }
    if (pr->n + 3 > pr->cap) {
        pr->cap = pr->cap ? pr->cap * 2 : 1024;
        pr->idx = (uint32_t *)realloc(pr->idx, pr->cap * 4);
    }
    pr->idx[pr->n++] = a;
    pr->idx[pr->n++] = b;
    pr->idx[pr->n++] = c;
}

/* A piece's vertices (weighted by `si`, or all to `rigid_bone`) and triangles. */
static void add_piece(uint32_t piece, uint32_t si, int rigid_bone)
{
    uint32_t nv = MEM32(piece + 0x68), vd = rel(piece + 0x4C), base = (uint32_t)s_nv, i;
    uint32_t hdr = MEM32(piece + 0x7C), rec = hdr ? MEM32(hdr + 0xC) : 0, ip, nb, b, pos, bat;
    uint32_t pal_n = si ? MEM32(si) : 0, wrec = si ? rel(si + 8) : 0;
    if (!vd || !nv || nv > 65536 || !rec)
        return;
    if (getenv("BUFFY_EXPORT_WDUMP") && si && wrec) {
        uint8_t seen[256] = { 0 };
        fprintf(stderr, "[EXPORT] piece %08X: %u verts, palette %u:", piece, nv, pal_n);
        for (i = 0; i < pal_n && i < 64; i++)
            fprintf(stderr, " %u", MEM8(si + 0xC + i));
        fprintf(stderr, "; si");
        for (i = 0; i < 0xC; i += 4)
            fprintf(stderr, " %08X", MEM32(si + i));
        fprintf(stderr, "; slots");
        for (i = 0; i < nv; i++) {
            uint32_t k;
            for (k = 0; k < 4; k++)
                if (getf(wrec + i * 20 + 4 + k * 4) > 0.001f)
                    seen[MEM8(wrec + i * 20 + k)] = 1;
        }
        for (i = 0; i < 256; i++)
            if (seen[i])
                fprintf(stderr, " %u", i);
        fprintf(stderr, "\n");
    }
    for (i = 0; i < nv; i++) {
        uint32_t a = vd + i * 24, pn = MEM32(a + 12);
        int nx = (int)(pn & 0x7FF), ny = (int)((pn >> 11) & 0x7FF), nz = (int)((pn >> 22) & 0x3FF), k;
        Vert v;
        memset(&v, 0, sizeof v);
        if (nx & 0x400) nx -= 0x800;
        if (ny & 0x400) ny -= 0x800;
        if (nz & 0x200) nz -= 0x400;
        v.p[0] = getf(a); v.p[1] = getf(a + 4); v.p[2] = -getf(a + 8);
        v.n[0] = nx / 1023.0f; v.n[1] = ny / 1023.0f; v.n[2] = -nz / 511.0f;
        v.uv[0] = getf(a + 16); v.uv[1] = getf(a + 20);
        if (si && wrec) {
            uint32_t r = wrec + i * 20;
            float sum = 0;
            for (k = 0; k < 4; k++) {
                /* shader registers: 3 a bone, from 0, skipping 57-59 (58/59 hold
                 * the viewport), so the 20th bone of a piece is at 60 */
                uint32_t reg = MEM8(r + (uint32_t)k), slot = (reg >= 60 ? reg - 3 : reg) / 3;
                v.j[k] = slot < pal_n ? MEM8(si + 0xC + slot) : 0;
                v.w[k] = k < 4 ? getf(r + 4 + (uint32_t)k * 4) : 0;
                if (!(v.w[k] >= 0 && v.w[k] <= 1.001f))
                    v.w[k] = 0;
                sum += v.w[k];
            }
            if (sum <= 0) {
                v.w[0] = 1;
                sum = 1;
            }
            for (k = 0; k < 4; k++)
                v.w[k] /= sum;
        } else {
            v.j[0] = (uint16_t)(rigid_bone >= 0 ? rigid_bone : 0);
            v.w[0] = 1;
        }
        add_vert(&v);
    }
    ip = MEM32(rec + 4);
    nb = (ip - rec - 0x18) / 20;
    bat = rec + 0x18;
    {
        /* some models (p01_buff) put a size word before the batches (low byte 1,
         * then their byte count + 4) and a word per batch after them */
        uint32_t w = MEM32(rec + 0x18), sz = w >> 8;
        if ((w & 0xFF) == 1 && sz >= 24 && (sz - 4) % 20 == 0 && 0x18 + sz + (sz - 4) / 20 * 4 == ip - rec) {
            nb = (sz - 4) / 20;
            bat = rec + 0x1C;
        } else if ((ip - rec - 0x18) % 20 == 4) {           /* (p04_spik: a word before, none after) */
            nb = (ip - rec - 0x1C) / 20;
            bat = rec + 0x1C;
        }
    }
    if (getenv("BUFFY_EXPORT_BATCHES")) {
        uint32_t q;
        fprintf(stderr, "[EXPORT] piece %08X rec %08X (%u verts):", piece, rec, nv);
        for (q = 0; q < 0x18 + nb * 20 && q < 0x100; q += 4)
            fprintf(stderr, " %08X", MEM32(rec + q));
        fprintf(stderr, "\n");
    }
    pos = ip;
    for (b = 0; b < nb && b < 256; b++) {
        uint32_t bt = bat + b * 20, n = MEM32(bt) + 2, tex = MEM32(bt + 4), k;   /* (a strip: its triangles + 2) */
        Prim *pr = prim_for((int)tex);
        for (k = 0; k + 2 < n; k++) {
            uint32_t a = MEM16(pos + k * 2), c1 = MEM16(pos + k * 2 + 2), c2 = MEM16(pos + k * 2 + 4);
            if (a == c1 || c1 == c2 || a == c2 || a >= nv || c1 >= nv || c2 >= nv)
                continue;
            add_tri(pr, base + a, base + c1, base + c2);
        }
        pos += n * 2;
    }
}

static void add_entity(uint32_t ent, uint32_t table, int rigid_bone)
{
    if (!ent)
        return;
    if (MEM32(ent) == SPLIT_ENTITY_VT) {
        uint32_t n = MEM32(ent + 0x44), i;
        for (i = 0; i < n && i < 256; i++)
            add_piece(rel(ent + 0x48 + i * 4), table ? rel(table + i * 4) : 0, rigid_bone);
    } else
        add_piece(ent, table ? rel(table) : 0, rigid_bone);
}

static uint32_t tex_entry(uint32_t hdr, uint32_t t)
{
    uint32_t arr = MEM32(hdr + 0x48), blocks = arr ? MEM32(arr + 4) : 0, blk;
    if (!blocks || t >= MEM32(hdr + 0x44))
        return 0;
    blk = MEM32(blocks + (t >> 6) * 4);
    return blk ? blk + (t & 63) * 0x4C : 0;
}

/* accessor helper */
static int s_nacc, s_nview;
static void view_and_acc(Buf *j, Buf *bin, const void *data, size_t bytes, int target, int ctype, size_t count,
                         const char *type, const char *minmax, int first)
{
    size_t off;
    balign(bin);
    off = bin->n;
    bput(bin, data, bytes);
    (void)first;
    jprintf(j, "%s{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u%s}", s_nview ? "," : "", (unsigned)off, (unsigned)bytes,
            target == 34963 ? ",\"target\":34963" : target == 34962 ? ",\"target\":34962" : "");
    s_nview++;
    (void)ctype; (void)count; (void)type; (void)minmax;
}

static int export_skin(uint32_t geo, uint32_t hdr, uint32_t hash, int si, uint32_t skin, const char *dir)
{
    uint32_t nb = MEM32(skin + 0x1C), hp = rel(skin + 0x28), pp = rel(skin + 0x20), ents = rel(hdr + 0x64 + 4);
    uint32_t list, n, k, i;
    Buf json = { 0 }, views = { 0 }, accs = { 0 }, bin = { 0 };
    char path[MAX_PATH];
    FILE *f;
    int t, nmat = 0, texmap[64], ok = 0;
    (void)geo;
    if (!nb || nb > 255 || !hp || !pp || !ents)
        return 0;
    s_nv = 0;
    for (t = 0; t < s_nprim; t++)
        free(s_prim[t].idx);
    s_nprim = 0;
    /* skinned pieces */
    list = rel(skin + 0x3C);
    n = MEM32(skin + 0x38) + MEM32(skin + 0x40);
    for (k = 0; list && k < n && k < 64; k++) {
        uint32_t ref = list + k * 0x14, idx = MEM32(ref + 0xC) & 0xFFFFFF;
        add_entity(MEM32(ents + idx * 0x20 + 0xC), rel(ref), -1);
    }
    /* rigid pieces */
    list = rel(skin + 0x4C);
    for (k = 0; list && k < MEM32(skin + 0x48) && k < 64; k++) {
        uint32_t ref = list + k * 0x14, idx = MEM32(ref + 0xC) & 0xFFFFFF;
        add_entity(MEM32(ents + idx * 0x20 + 0xC), 0, (int)MEM32(ref + 8));
    }
    if (!s_nv || !s_nprim)
        return 0;

    s_nacc = s_nview = 0;
    /* vertex attributes */
    {
        float *pos = (float *)malloc(s_nv * 12), *nrm = (float *)malloc(s_nv * 12), *uv = (float *)malloc(s_nv * 8),
              *wt = (float *)malloc(s_nv * 16), mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        uint16_t *jn = (uint16_t *)malloc(s_nv * 8);
        size_t v;
        for (v = 0; v < s_nv; v++) {
            for (k = 0; k < 3; k++) {
                float l = sqrtf(s_v[v].n[0] * s_v[v].n[0] + s_v[v].n[1] * s_v[v].n[1] + s_v[v].n[2] * s_v[v].n[2]);
                pos[v * 3 + k] = s_v[v].p[k];
                nrm[v * 3 + k] = l > 0 ? s_v[v].n[k] / l : (k == 1 ? 1.0f : 0.0f);
                if (s_v[v].p[k] < mn[k]) mn[k] = s_v[v].p[k];
                if (s_v[v].p[k] > mx[k]) mx[k] = s_v[v].p[k];
            }
            uv[v * 2] = s_v[v].uv[0];
            uv[v * 2 + 1] = s_v[v].uv[1];
            for (k = 0; k < 4; k++) {
                wt[v * 4 + k] = s_v[v].w[k];
                jn[v * 4 + k] = s_v[v].j[k] < nb ? s_v[v].j[k] : 0;
            }
        }
        view_and_acc(&views, &bin, pos, s_nv * 12, 34962, 0, 0, 0, 0, 1);
        jprintf(&accs, "{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]}",
                (unsigned)s_nv, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
        view_and_acc(&views, &bin, nrm, s_nv * 12, 34962, 0, 0, 0, 0, 0);
        jprintf(&accs, ",{\"bufferView\":1,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"}", (unsigned)s_nv);
        view_and_acc(&views, &bin, uv, s_nv * 8, 34962, 0, 0, 0, 0, 0);
        jprintf(&accs, ",{\"bufferView\":2,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"}", (unsigned)s_nv);
        view_and_acc(&views, &bin, jn, s_nv * 8, 34962, 0, 0, 0, 0, 0);
        jprintf(&accs, ",{\"bufferView\":3,\"componentType\":5123,\"count\":%u,\"type\":\"VEC4\"}", (unsigned)s_nv);
        view_and_acc(&views, &bin, wt, s_nv * 16, 34962, 0, 0, 0, 0, 0);
        jprintf(&accs, ",{\"bufferView\":4,\"componentType\":5126,\"count\":%u,\"type\":\"VEC4\"}", (unsigned)s_nv);
        free(pos); free(nrm); free(uv); free(wt); free(jn);
    }
    /* inverse bind matrices (column-major): the rest pose is the pivots, unrotated */
    {
        float *ibm = (float *)calloc(nb, 64);
        for (k = 0; k < nb; k++) {
            float *m = ibm + k * 16;
            m[0] = m[5] = m[10] = m[15] = 1;
            m[12] = -getf(pp + k * 16);
            m[13] = -getf(pp + k * 16 + 4);
            m[14] = getf(pp + k * 16 + 8);
        }
        view_and_acc(&views, &bin, ibm, nb * 64, 0, 0, 0, 0, 0, 0);
        jprintf(&accs, ",{\"bufferView\":5,\"componentType\":5126,\"count\":%u,\"type\":\"MAT4\"}", nb);
        free(ibm);
    }
    /* indices, one accessor per texture */
    for (t = 0; t < s_nprim; t++) {
        view_and_acc(&views, &bin, s_prim[t].idx, s_prim[t].n * 4, 34963, 0, 0, 0, 0, 0);
        jprintf(&accs, ",{\"bufferView\":%d,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}", s_nview - 1,
                (unsigned)s_prim[t].n);
    }
    /* textures -> materials */
    {
        Buf imgs = { 0 }, mats = { 0 }, texs = { 0 };
        int nimg = 0;
        for (t = 0; t < s_nprim; t++) {
            uint32_t te = tex_entry(hdr, (uint32_t)s_prim[t].tex), w = 0, h = 0, *px = NULL;
            texmap[t] = -1;
            if (te && MEM32(te + 0x30))
                px = tx_decode(MEM32(te + 0x30), MEM32(te + 0x38), MEM32(te + 0x3C), &w, &h);
            if (!px)
                fprintf(stderr, "[EXPORT]   texture %d: entry %08X data %08X format %08X size %08X (not decoded)\n", s_prim[t].tex,
                        te, te ? MEM32(te + 0x30) : 0, te ? MEM32(te + 0x38) : 0, te ? MEM32(te + 0x3C) : 0);
            if (px) {
                size_t before;
                balign(&bin);
                before = bin.n;
                if (png_encode(px, w, h, &bin)) {
                    jprintf(&views, ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}", (unsigned)before, (unsigned)(bin.n - before));
                    s_nview++;
                    jprintf(&imgs, "%s{\"bufferView\":%d,\"mimeType\":\"image/png\",\"name\":\"tex%d\"}", nimg ? "," : "", s_nview - 1,
                            s_prim[t].tex);
                    jprintf(&texs, "%s{\"source\":%d,\"sampler\":0}", nimg ? "," : "", nimg);
                    texmap[t] = nimg++;
                }
                free(px);
            }
            jprintf(&mats, "%s{\"name\":\"tex%d\",\"pbrMetallicRoughness\":{%s\"metallicFactor\":0,\"roughnessFactor\":1},"
                           "\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5}", nmat ? "," : "", s_prim[t].tex, "");
            if (texmap[t] >= 0) {
                /* rewrite: the material with its texture */
                char tmp[160];
                mats.n -= strlen("\"metallicFactor\":0,\"roughnessFactor\":1},\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5}");
                snprintf(tmp, sizeof tmp, "\"baseColorTexture\":{\"index\":%d},\"metallicFactor\":0,\"roughnessFactor\":1},"
                                          "\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5}", texmap[t]);
                bput(&mats, tmp, strlen(tmp));
            }
            nmat++;
        }
        /* JSON */
        jprintf(&json, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Buffy the Vampire Slayer: Chaos Bleeds PC port\"},");
        jprintf(&json, "\"extras\":{\"buffyModel\":\"%08X\",\"buffySkin\":%d,\"buffySkinId\":\"%08X\",\"buffyBones\":%u},", hash, si,
                MEM32(rel(hdr + 0x74 + 4) + (uint32_t)si * 0x1C), nb);
        jprintf(&json, "\"scene\":0,\"scenes\":[{\"nodes\":[%u,%u]}],\"nodes\":[", nb, nb + 1);
        for (k = 0; k < nb; k++) {
            int par = (int16_t)MEM16(hp + k * 8), c, first = 1;
            float tx = getf(pp + k * 16), ty = getf(pp + k * 16 + 4), tz = -getf(pp + k * 16 + 8);
            if (par >= 0 && (uint32_t)par < nb) {
                tx -= getf(pp + (uint32_t)par * 16);
                ty -= getf(pp + (uint32_t)par * 16 + 4);
                tz += getf(pp + (uint32_t)par * 16 + 8);
            }
            jprintf(&json, "%s{\"name\":\"bone%02u\",\"translation\":[%g,%g,%g]", k ? "," : "", k, tx, ty, tz);
            for (c = 0; c < (int)nb; c++)
                if ((int16_t)MEM16(hp + (uint32_t)c * 8) == (int)k) {
                    jprintf(&json, "%s%d", first ? ",\"children\":[" : ",", c);
                    first = 0;
                }
            jprintf(&json, "%s}", first ? "" : "]");
        }
        jprintf(&json, ",{\"name\":\"Armature\",\"children\":[");
        for (k = 0, i = 0; k < nb; k++)
            if ((int16_t)MEM16(hp + k * 8) < 0 || (uint32_t)(int16_t)MEM16(hp + k * 8) >= nb)
                jprintf(&json, "%s%u", i++ ? "," : "", k);
        jprintf(&json, "]},{\"name\":\"Model_%08X_skin%d\",\"mesh\":0,\"skin\":0}],", hash, si);
        jprintf(&json, "\"skins\":[{\"inverseBindMatrices\":5,\"skeleton\":%u,\"joints\":[", nb);
        for (k = 0; k < nb; k++)
            jprintf(&json, "%s%u", k ? "," : "", k);
        jprintf(&json, "]}],\"meshes\":[{\"name\":\"Model_%08X_skin%d\",\"primitives\":[", hash, si);
        for (t = 0; t < s_nprim; t++)
            jprintf(&json, "%s{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"JOINTS_0\":3,\"WEIGHTS_0\":4},"
                           "\"indices\":%d,\"material\":%d}", t ? "," : "", 6 + t, t);
        jprintf(&json, "]}],\"materials\":[");
        bput(&json, mats.p, mats.n);
        jprintf(&json, "]");
        if (nimg) {
            jprintf(&json, ",\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987}],\"images\":[");
            bput(&json, imgs.p, imgs.n);
            jprintf(&json, "],\"textures\":[");
            bput(&json, texs.p, texs.n);
            jprintf(&json, "]");
        }
        jprintf(&json, ",\"accessors\":[");
        bput(&json, accs.p, accs.n);
        jprintf(&json, "],\"bufferViews\":[");
        bput(&json, views.p, views.n);
        balign(&bin);
        jprintf(&json, "],\"buffers\":[{\"byteLength\":%u}]}", (unsigned)bin.n);
        while (json.n & 3)
            bput(&json, " ", 1);
        free(imgs.p); free(mats.p); free(texs.p);
    }
    /* .glb: header, JSON chunk, BIN chunk */
    snprintf(path, sizeof path, "%s\\%08X_skin%d.glb", dir, hash, si);
    if (!fopen_s(&f, path, "wb") && f) {
        uint32_t hdr3[3] = { 0x46546C67u, 2, (uint32_t)(12 + 8 + json.n + 8 + bin.n) }, ch[2];
        fwrite(hdr3, 4, 3, f);
        ch[0] = (uint32_t)json.n; ch[1] = 0x4E4F534Au;
        fwrite(ch, 4, 2, f);
        fwrite(json.p, 1, json.n, f);
        ch[0] = (uint32_t)bin.n; ch[1] = 0x004E4942u;
        fwrite(ch, 4, 2, f);
        fwrite(bin.p, 1, bin.n, f);
        fclose(f);
        ok = 1;
        fprintf(stderr, "[EXPORT] %s: %u bones, %u vertices, %d materials\n", path, nb, (unsigned)s_nv, s_nprim);
    }
    free(json.p); free(views.p); free(accs.p); free(bin.p);
    return ok;
}

/* ── the export run ─────────────────────────────────────────────────────── */

static int export_model(uint32_t hash, const char *dir)
{
    uint32_t args[3], geo, hdr, secs, ns, k, skins, nsk, written = 0;
    args[0] = hash;
    args[1] = 0;
    geo = call_game(EXGeoFile_LoadGeoFile_000C69C0, 0, 2, args);
    hdr = geo ? MEM32(geo + 0x28) : 0;
    if (!hdr) {
        fprintf(stderr, "[EXPORT] model %08X: not loaded\n", hash);
        return 0;
    }
    /* every section: the skins' and their textures */
    ns = (uint32_t)(int16_t)MEM16(hdr + 0x54);
    secs = rel(hdr + 0x58);
    for (k = 0; secs && k < ns && k < 256; k++) {
        uint32_t h = MEM32(secs + k * 0x14);
        if (!h)
            continue;
        args[0] = h;
        args[1] = 0;
        call_game(EXGeoHeader_LoadSectionFile_000C67D0, hdr, 2, args);
    }
    nsk = (uint32_t)(int16_t)MEM16(hdr + 0x74);
    skins = rel(hdr + 0x74 + 4);
    fprintf(stderr, "[EXPORT] model %08X: %u sections, %u skins, %u textures\n", hash, ns, nsk, MEM32(hdr + 0x44));
    for (k = 0; skins && k < nsk && k < 64; k++) {
        uint32_t data = MEM32(skins + k * 0x1C + 0xC);
        if (data && export_skin(geo, hdr, hash, (int)k, data, dir))
            written++;
    }
    return written > 0;
}

/* Each frame (buffy_mods.c, the main update): once the game has run a little,
 * the export, then the game quits. */
void buffy_export_frame(void)
{
    static int frames, done;
    const char *m = getenv("BUFFY_EXPORT_MODEL"), *d = getenv("BUFFY_EXPORT_DIR");
    if (!m || done || ++frames < 30)
        return;
    done = 1;
    {
        char dir[MAX_PATH];
        const char *q = m;
        int ok = 0;
        snprintf(dir, sizeof dir, "%s", d && *d ? d : ".");
        CreateDirectoryA(dir, NULL);
        while (*q) {                                     /* (one model, or several: hash,hash,...) */
            char *e;
            uint32_t hash = (uint32_t)strtoul(q, &e, 16);
            if (e == q)
                break;
            if (hash && export_model(hash, dir))
                ok = 1;
            fflush(stderr);
            q = *e == ',' ? e + 1 : e;
        }
        fprintf(stderr, "[EXPORT] %s\n", ok ? "done" : "nothing written");
        fflush(stderr);
        ExitProcess(ok ? 0 : 1);
    }
}
