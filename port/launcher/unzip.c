/* Buffy the Vampire Slayer: Chaos Bleeds PC launcher - zip extraction (see unzip.h).
 *
 * A zip reader with its own inflate (deflate decoder, after zlib's "puff" by
 * Mark Adler): Windows has tar.exe to unpack archives, Wine / Proton has
 * not, and the updater and the FFmpeg download need zips to work there too. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "unzip.h"

/* ---- inflate ---------------------------------------------------------- */

typedef struct {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint8_t *out;
    size_t out_len, out_pos;
    uint32_t bitbuf;
    int bitcnt;
    int error;
} Inflate;

typedef struct {
    short count[16];   /* codes of each length */
    short symbol[320]; /* symbols in canonical order */
} Huffman;

static int bits(Inflate *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) {
            s->error = 1;
            return 0;
        }
        val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int stored(Inflate *s)
{
    unsigned len;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->in_pos + 4 > s->in_len)
        return 1;
    len = s->in[s->in_pos] | (s->in[s->in_pos + 1] << 8);
    if ((s->in[s->in_pos + 2] | (s->in[s->in_pos + 3] << 8)) != (~len & 0xFFFF))
        return 1;
    s->in_pos += 4;
    if (s->in_pos + len > s->in_len || s->out_pos + len > s->out_len)
        return 1;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return 0;
}

static int decode(Inflate *s, const Huffman *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        int count;
        code |= bits(s, 1);
        if (s->error)
            return -1;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* 0 if complete, < 0 if over-subscribed or incomplete (allowed for single codes). */
static int construct(Huffman *h, const short *length, int n)
{
    short offs[16];
    int symbol, len, left;
    for (len = 0; len <= 15; len++)
        h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++)
        h->count[length[symbol]]++;
    if (h->count[0] == n)
        return 0;
    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

static int codes(Inflate *s, const Huffman *lencode, const Huffman *distcode)
{
    static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                   3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                    6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                   7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int symbol;
    do {
        symbol = decode(s, lencode);
        if (symbol < 0)
            return 1;
        if (symbol < 256) {
            if (s->out_pos >= s->out_len)
                return 1;
            s->out[s->out_pos++] = (uint8_t)symbol;
        } else if (symbol > 256) {
            size_t len, dist;
            symbol -= 257;
            if (symbol >= 29)
                return 1;
            len = lbase[symbol] + bits(s, lext[symbol]);
            symbol = decode(s, distcode);
            if (symbol < 0 || symbol >= 30)
                return 1;
            dist = dbase[symbol] + bits(s, dext[symbol]);
            if (s->error || dist > s->out_pos || s->out_pos + len > s->out_len)
                return 1;
            while (len--) {
                s->out[s->out_pos] = s->out[s->out_pos - dist];
                s->out_pos++;
            }
        }
    } while (symbol != 256);
    return s->error;
}

static int fixed(Inflate *s)
{
    static Huffman lencode, distcode;
    static int built = 0;
    if (!built) {
        short lengths[288];
        int symbol;
        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < 288; symbol++) lengths[symbol] = 8;
        construct(&lencode, lengths, 288);
        for (symbol = 0; symbol < 30; symbol++) lengths[symbol] = 5;
        construct(&distcode, lengths, 30);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(Inflate *s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    Huffman lencode, distcode;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4, index, err;
    if (s->error || nlen > 286 || ndist > 30)
        return 1;
    for (index = 0; index < ncode; index++)
        lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    if (s->error || construct(&lencode, lengths, 19) != 0)
        return 1;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode), len = 0;
        if (symbol < 0)
            return 1;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            if (symbol == 16) {
                if (index == 0)
                    return 1;
                len = lengths[index - 1];
                symbol = 3 + bits(s, 2);
            } else if (symbol == 17) {
                symbol = 3 + bits(s, 3);
            } else {
                symbol = 11 + bits(s, 7);
            }
            if (index + symbol > nlen + ndist)
                return 1;
            while (symbol--)
                lengths[index++] = (short)len;
        }
    }
    if (s->error || lengths[256] == 0)
        return 1;
    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
        return 1;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
        return 1;
    return codes(s, &lencode, &distcode);
}

static int inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    Inflate s;
    int last, type, err;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.in_len = in_len;
    s.out = out;
    s.out_len = out_len;
    do {
        last = bits(&s, 1);
        type = bits(&s, 2);
        if (s.error)
            return 0;
        err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : 1;
        if (err)
            return 0;
    } while (!last);
    return s.out_pos == out_len;
}

/* ---- zip -------------------------------------------------------------- */

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Creates the folders of `path` (below `root`, which exists). */
static void make_dirs(WCHAR *path, size_t root_len)
{
    size_t i;
    for (i = root_len + 1; path[i]; i++) {
        if (path[i] == L'\\') {
            path[i] = 0;
            CreateDirectoryW(path, NULL);
            path[i] = L'\\';
        }
    }
}

static int write_file(const WCHAR *path, const uint8_t *data, size_t n)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    size_t done = 0;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    while (done < n) {
        DWORD chunk = (DWORD)((n - done) > (1u << 30) ? (1u << 30) : (n - done)), wrote = 0;
        if (!WriteFile(f, data + done, chunk, &wrote, NULL) || !wrote)
            break;
        done += wrote;
    }
    CloseHandle(f);
    return done == n;
}

int unzip_file(const WCHAR *zip, const WCHAR *to)
{
    HANDLE f, map = NULL;
    LARGE_INTEGER size;
    const uint8_t *z = NULL, *eocd = NULL;
    size_t n, i, root_len = wcslen(to);
    uint32_t count, cd_off, entry;
    int ok = 0;

    f = CreateFileW(zip, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 22 || size.QuadPart > 0xFFFFFFFFll
            || !(map = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL))
            || !(z = (const uint8_t *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)))
        goto done;
    n = (size_t)size.QuadPart;
    /* The end of central directory record (after it, a comment of up to 64 KB). */
    for (i = n - 22;; i--) {
        if (rd32(z + i) == 0x06054b50) {
            eocd = z + i;
            break;
        }
        if (i == 0 || n - i > 22 + 65535)
            break;
    }
    if (!eocd)
        goto done;
    count = rd16(eocd + 10);
    cd_off = rd32(eocd + 16);
    ok = 1;
    for (entry = 0; ok && entry < count; entry++) {
        const uint8_t *c = z + cd_off, *local, *data;
        uint32_t method, csize, usize, nlen, xlen, clen, loff, flags;
        WCHAR name[MAX_PATH], path[MAX_PATH * 2];
        char raw[MAX_PATH];
        int wn, k;
        if (cd_off + 46 > n || rd32(c) != 0x02014b50) {
            ok = 0;
            break;
        }
        flags = rd16(c + 8);
        method = rd16(c + 10);
        csize = rd32(c + 20);
        usize = rd32(c + 24);
        nlen = rd16(c + 28);
        xlen = rd16(c + 30);
        clen = rd16(c + 32);
        loff = rd32(c + 42);
        cd_off += 46 + nlen + xlen + clen;
        if (nlen == 0 || nlen >= MAX_PATH || (size_t)(c + 46 + nlen - z) > n || (flags & 1)) {
            ok = 0;  /* (encrypted) */
            break;
        }
        memcpy(raw, c + 46, nlen);
        raw[nlen] = 0;
        wn = MultiByteToWideChar((flags & 0x800) ? CP_UTF8 : CP_OEMCP, 0, raw, -1, name, MAX_PATH);
        if (!wn) {
            ok = 0;
            break;
        }
        for (k = 0; name[k]; k++)
            if (name[k] == L'/')
                name[k] = L'\\';
        /* Nothing outside `to`. */
        if (name[0] == L'\\' || wcschr(name, L':') || wcsstr(name, L"..\\") || !wcscmp(name, L"..")
                || (wcslen(name) >= 3 && !wcscmp(name + wcslen(name) - 3, L"\\.."))) {
            ok = 0;
            break;
        }
        if (swprintf_s(path, MAX_PATH * 2, L"%s\\%s", to, name) < 0) {
            ok = 0;
            break;
        }
        if (name[wcslen(name) - 1] == L'\\') {  /* a folder */
            make_dirs(path, root_len);
            continue;
        }
        make_dirs(path, root_len);
        local = z + loff;
        if (loff + 30 > n || rd32(local) != 0x04034b50) {
            ok = 0;
            break;
        }
        data = local + 30 + rd16(local + 26) + rd16(local + 28);
        if ((size_t)(data - z) + csize > n) {
            ok = 0;
            break;
        }
        if (method == 0 && csize == usize) {
            ok = write_file(path, data, usize);
        } else if (method == 8) {
            uint8_t *out = (uint8_t *)malloc(usize ? usize : 1);
            ok = out && inflate_raw(data, csize, out, usize) && write_file(path, out, usize);
            free(out);
        } else {
            ok = 0;  /* another method (or zip64) */
        }
    }
done:
    if (z)
        UnmapViewOfFile(z);
    if (map)
        CloseHandle(map);
    CloseHandle(f);
    return ok;
}
