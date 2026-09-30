/*
 * Model import: a character mod's edited model (mod.ini [Character]
 * ModelFile = model.glb, a .glb from the Advanced tab's exporter, edited in
 * Blender) put onto the loaded game model it came from.
 *
 * The game keeps its own mesh (triangles, textures, bone weights); from the
 * file it takes:
 *   - the bones' rest positions (the skin's pivots), by bone name (boneNN),
 *     so a rig moved to fit another character's animations fits them;
 *   - every vertex's rest position and normal.
 * Blender splits and reorders vertices, so each game vertex is matched to the
 * file's: same material (texNN) and texture coordinate, nearest to where the
 * moved bones would put it (falling back to the nearest by position alone).
 *
 * Axes as the exporter's: glTF = game with z negated. Skinned meshes are read
 * in their rest pose: sum of weight * joint world * inverse bind * position.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp/gen/recomp_types.h"

#define SPLIT_ENTITY_VT 0x1B3FE8u
#define MAX_BONES 256

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

static void putf(uint32_t a, float f)
{
    memcpy((void *)XBOX_PTR(a), &f, 4);
}

/* ── a small JSON reader ────────────────────────────────────────────────── */

enum { J_NULL, J_NUM, J_STR, J_ARR, J_OBJ, J_BOOL };
typedef struct JV JV;
struct JV {
    int type, n;
    double num;
    char *str;
    char **keys;                /* objects */
    JV *kids;
};

static const char *jp_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static const char *jp_str(const char *p, char **out)
{
    size_t cap = 32, n = 0;
    char *s = (char *)malloc(cap);
    p++;                                                  /* opening quote */
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') {                          /* (names are ASCII: keep the low byte) */
                unsigned u = 0;
                int k;
                for (k = 0; k < 4 && *p; k++, p++)
                    u = u * 16 + (unsigned)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
                c = (char)(u < 128 ? u : '?');
            }
        }
        if (n + 2 > cap)
            s = (char *)realloc(s, cap *= 2);
        s[n++] = c;
    }
    s[n] = 0;
    *out = s;
    return *p ? p + 1 : p;
}

static const char *jp_val(const char *p, JV *v, int depth);

static const char *jp_list(const char *p, JV *v, int obj, int depth)
{
    int cap = 8;
    char close = obj ? '}' : ']';
    v->type = obj ? J_OBJ : J_ARR;
    v->kids = (JV *)calloc((size_t)cap, sizeof(JV));
    if (obj)
        v->keys = (char **)calloc((size_t)cap, sizeof(char *));
    p = jp_ws(p + 1);
    while (*p && *p != close) {
        if (v->n == cap) {
            cap *= 2;
            v->kids = (JV *)realloc(v->kids, (size_t)cap * sizeof(JV));
            if (obj)
                v->keys = (char **)realloc(v->keys, (size_t)cap * sizeof(char *));
        }
        memset(&v->kids[v->n], 0, sizeof(JV));
        if (obj) {
            if (*p != '"')
                return NULL;
            p = jp_ws(jp_str(p, &v->keys[v->n]));
            if (*p != ':')
                return NULL;
            p = jp_ws(p + 1);
        }
        if (!(p = jp_val(p, &v->kids[v->n], depth + 1)))
            return NULL;
        v->n++;
        p = jp_ws(p);
        if (*p == ',')
            p = jp_ws(p + 1);
    }
    return *p ? p + 1 : NULL;
}

static const char *jp_val(const char *p, JV *v, int depth)
{
    if (depth > 64)
        return NULL;
    p = jp_ws(p);
    if (*p == '{' || *p == '[')
        return jp_list(p, v, *p == '{', depth);
    if (*p == '"') {
        v->type = J_STR;
        return jp_str(p, &v->str);
    }
    if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
        v->type = J_BOOL;
        v->num = *p == 't';
        return p + (*p == 't' ? 4 : 5);
    }
    if (!strncmp(p, "null", 4)) {
        v->type = J_NULL;
        return p + 4;
    }
    {
        char *e;
        v->type = J_NUM;
        v->num = strtod(p, &e);
        return e == p ? NULL : e;
    }
}

static void jv_free(JV *v)
{
    int i;
    for (i = 0; i < v->n; i++) {
        jv_free(&v->kids[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->kids);
    free(v->keys);
    free(v->str);
}

static JV *jget(JV *o, const char *k)
{
    int i;
    if (!o || o->type != J_OBJ)
        return NULL;
    for (i = 0; i < o->n; i++)
        if (!strcmp(o->keys[i], k))
            return &o->kids[i];
    return NULL;
}

static JV *jat(JV *a, int i) { return a && a->type == J_ARR && i >= 0 && i < a->n ? &a->kids[i] : NULL; }
static double jnum(JV *v, double d) { return v && (v->type == J_NUM || v->type == J_BOOL) ? v->num : d; }

/* ── the .glb ───────────────────────────────────────────────────────────── */

typedef struct {
    JV root;
    uint8_t *bin;
    size_t nbin;
    uint8_t *file;
} Glb;

static int glb_open(Glb *g, const wchar_t *path)
{
    FILE *f;
    long len;
    uint32_t jl, bl;
    char *js;
    memset(g, 0, sizeof *g);
    if (_wfopen_s(&f, path, L"rb") || !f)
        return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    g->file = (uint8_t *)malloc((size_t)len + 1);
    if (!g->file || fread(g->file, 1, (size_t)len, f) != (size_t)len || len < 28 || memcmp(g->file, "glTF", 4)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    memcpy(&jl, g->file + 12, 4);
    if (20 + (long)jl > len)
        return 0;
    js = (char *)malloc(jl + 1);
    memcpy(js, g->file + 20, jl);
    js[jl] = 0;
    if (!jp_val(js, &g->root, 0)) {
        free(js);
        return 0;
    }
    free(js);
    if (20 + (long)jl + 8 <= len) {
        memcpy(&bl, g->file + 20 + jl, 4);
        g->bin = g->file + 20 + jl + 8;
        g->nbin = bl;
        if ((long)(20 + jl + 8 + bl) > len)
            g->nbin = (size_t)(len - (long)(20 + jl + 8));
    }
    return 1;
}

static void glb_close(Glb *g)
{
    jv_free(&g->root);
    free(g->file);
}

/* Accessor `ai` as floats (count x comps); normalized ints scaled to 0..1. */
static float *glb_acc(Glb *g, int ai, int *count, int *comps)
{
    JV *a = jat(jget(&g->root, "accessors"), ai), *v;
    int n, c, ct, stride, i, k, csize, norm;
    size_t off;
    const char *type;
    float *out;
    if (!a)
        return NULL;
    v = jat(jget(&g->root, "bufferViews"), (int)jnum(jget(a, "bufferView"), -1));
    type = jget(a, "type") ? jget(a, "type")->str : "";
    c = !strcmp(type, "SCALAR") ? 1 : !strcmp(type, "VEC2") ? 2 : !strcmp(type, "VEC3") ? 3 : !strcmp(type, "VEC4") ? 4
        : !strcmp(type, "MAT4") ? 16 : 0;
    n = (int)jnum(jget(a, "count"), 0);
    ct = (int)jnum(jget(a, "componentType"), 0);
    norm = (int)jnum(jget(a, "normalized"), 0);
    csize = ct == 5126 || ct == 5125 ? 4 : ct == 5123 || ct == 5122 ? 2 : 1;
    if (!v || !c || n <= 0)
        return NULL;
    stride = (int)jnum(jget(v, "byteStride"), 0);
    if (!stride)
        stride = c * csize;
    off = (size_t)jnum(jget(v, "byteOffset"), 0) + (size_t)jnum(jget(a, "byteOffset"), 0);
    if (off + (size_t)(n - 1) * (size_t)stride + (size_t)(c * csize) > g->nbin)
        return NULL;
    out = (float *)malloc((size_t)n * (size_t)c * sizeof(float));
    for (i = 0; i < n; i++) {
        const uint8_t *e = g->bin + off + (size_t)i * (size_t)stride;
        for (k = 0; k < c; k++) {
            float f;
            if (ct == 5126)
                memcpy(&f, e + k * 4, 4);
            else if (ct == 5125) { uint32_t u; memcpy(&u, e + k * 4, 4); f = (float)u; }
            else if (ct == 5123) { uint16_t u; memcpy(&u, e + k * 2, 2); f = norm ? u / 65535.0f : (float)u; }
            else if (ct == 5122) { int16_t u; memcpy(&u, e + k * 2, 2); f = norm ? u / 32767.0f : (float)u; }
            else if (ct == 5120) f = norm ? (int8_t)e[k] / 127.0f : (float)(int8_t)e[k];
            else f = norm ? e[k] / 255.0f : (float)e[k];
            out[(size_t)i * c + k] = f;
        }
    }
    *count = n;
    *comps = c;
    return out;
}

/* 4x4 column-major (glTF) */
typedef struct { float m[16]; } M4;

static M4 m4_mul(const M4 *a, const M4 *b)
{
    M4 r;
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0;
            for (k = 0; k < 4; k++)
                s += a->m[k * 4 + i] * b->m[j * 4 + k];
            r.m[j * 4 + i] = s;
        }
    return r;
}

static M4 node_local(JV *n)
{
    M4 r;
    JV *t = jget(n, "translation"), *q = jget(n, "rotation"), *s = jget(n, "scale"), *mm = jget(n, "matrix");
    float x = (float)jnum(jat(q, 0), 0), y = (float)jnum(jat(q, 1), 0), z = (float)jnum(jat(q, 2), 0), w = (float)jnum(jat(q, 3), 1);
    float sx = (float)jnum(jat(s, 0), 1), sy = (float)jnum(jat(s, 1), 1), sz = (float)jnum(jat(s, 2), 1);
    int i;
    if (mm && mm->n == 16) {
        for (i = 0; i < 16; i++)
            r.m[i] = (float)jnum(jat(mm, i), 0);
        return r;
    }
    r.m[0] = (1 - 2 * (y * y + z * z)) * sx; r.m[1] = (2 * (x * y + z * w)) * sx; r.m[2] = (2 * (x * z - y * w)) * sx; r.m[3] = 0;
    r.m[4] = (2 * (x * y - z * w)) * sy; r.m[5] = (1 - 2 * (x * x + z * z)) * sy; r.m[6] = (2 * (y * z + x * w)) * sy; r.m[7] = 0;
    r.m[8] = (2 * (x * z + y * w)) * sz; r.m[9] = (2 * (y * z - x * w)) * sz; r.m[10] = (1 - 2 * (x * x + y * y)) * sz; r.m[11] = 0;
    r.m[12] = (float)jnum(jat(t, 0), 0); r.m[13] = (float)jnum(jat(t, 1), 0); r.m[14] = (float)jnum(jat(t, 2), 0); r.m[15] = 1;
    return r;
}

/* ── the file's mesh, in its rest pose ──────────────────────────────────── */

typedef struct {
    int n;
    float *pos, *nrm, *uv;      /* n*3, n*3, n*2 */
    int *mat;                   /* texture index from the material's name (texNN), else -1 */
    float bone_pos[MAX_BONES][3];
    int has_bone[MAX_BONES];
} Src;

static int src_read(Glb *g, Src *s)
{
    JV *nodes = jget(&g->root, "nodes"), *skin = jat(jget(&g->root, "skins"), 0), *joints = jget(skin, "joints");
    JV *mats = jget(&g->root, "materials"), *mesh = NULL;
    int nn = nodes ? nodes->n : 0, i, *parent, nj, cnt, cc;
    M4 *world, *skinm = NULL;
    float *ibm = NULL;
    memset(s, 0, sizeof *s);
    if (!nn || !joints)
        return 0;
    parent = (int *)malloc((size_t)nn * sizeof(int));
    world = (M4 *)malloc((size_t)nn * sizeof(M4));
    for (i = 0; i < nn; i++)
        parent[i] = -1;
    for (i = 0; i < nn; i++) {
        JV *ch = jget(&nodes->kids[i], "children");
        int k;
        for (k = 0; ch && k < ch->n; k++) {
            int c = (int)jnum(&ch->kids[k], -1);
            if (c >= 0 && c < nn)
                parent[c] = i;
        }
    }
    for (i = 0; i < nn; i++) {                            /* world = parents' * local */
        int chain[64], d = 0, k, p;
        M4 m;
        for (p = i; p >= 0 && d < 64; p = parent[p])
            chain[d++] = p;
        m = node_local(&nodes->kids[chain[d - 1]]);
        for (k = d - 2; k >= 0; k--) {
            M4 l = node_local(&nodes->kids[chain[k]]);
            m = m4_mul(&m, &l);
        }
        world[i] = m;
        if (!mesh && jget(&nodes->kids[i], "mesh") && jget(&nodes->kids[i], "skin"))
            mesh = jat(jget(&g->root, "meshes"), (int)jnum(jget(&nodes->kids[i], "mesh"), -1));
    }
    /* joints: skin matrix = world * inverse bind; the bone's rest position */
    nj = joints->n;
    skinm = (M4 *)calloc((size_t)nj, sizeof(M4));
    if (jget(skin, "inverseBindMatrices"))
        ibm = glb_acc(g, (int)jnum(jget(skin, "inverseBindMatrices"), -1), &cnt, &cc);
    for (i = 0; i < nj; i++) {
        int ni = (int)jnum(&joints->kids[i], -1), b = -1;
        M4 ib;
        const char *name = ni >= 0 && ni < nn && jget(&nodes->kids[ni], "name") ? jget(&nodes->kids[ni], "name")->str : "";
        if (ni < 0 || ni >= nn)
            continue;
        if (ibm && i < cnt && cc == 16)
            memcpy(ib.m, ibm + i * 16, 64);
        else {
            memset(&ib, 0, sizeof ib);
            ib.m[0] = ib.m[5] = ib.m[10] = ib.m[15] = 1;
        }
        skinm[i] = m4_mul(&world[ni], &ib);
        if (!strncmp(name, "bone", 4))
            b = atoi(name + 4);
        if (b >= 0 && b < MAX_BONES) {
            s->bone_pos[b][0] = world[ni].m[12];
            s->bone_pos[b][1] = world[ni].m[13];
            s->bone_pos[b][2] = world[ni].m[14];
            s->has_bone[b] = 1;
        }
    }
    free(ibm);
    /* every primitive's vertices, skinned to rest */
    if (mesh) {
        JV *prims = jget(mesh, "primitives");
        int p;
        for (p = 0; prims && p < prims->n; p++) {
            JV *at = jget(&prims->kids[p], "attributes"), *mt;
            int np = 0, c1 = 0, nn2 = 0, c2 = 0, nu = 0, c3 = 0, nj2 = 0, c4 = 0, nw = 0, c5 = 0, v, tex = -1;
            float *P = glb_acc(g, (int)jnum(jget(at, "POSITION"), -1), &np, &c1);
            float *N = glb_acc(g, (int)jnum(jget(at, "NORMAL"), -1), &nn2, &c2);
            float *U = glb_acc(g, (int)jnum(jget(at, "TEXCOORD_0"), -1), &nu, &c3);
            float *J = glb_acc(g, (int)jnum(jget(at, "JOINTS_0"), -1), &nj2, &c4);
            float *W = glb_acc(g, (int)jnum(jget(at, "WEIGHTS_0"), -1), &nw, &c5);
            mt = jat(mats, (int)jnum(jget(&prims->kids[p], "material"), -1));
            if (mt && jget(mt, "name") && !strncmp(jget(mt, "name")->str, "tex", 3))
                tex = atoi(jget(mt, "name")->str + 3);
            if (P && c1 == 3) {
                s->pos = (float *)realloc(s->pos, (size_t)(s->n + np) * 12);
                s->nrm = (float *)realloc(s->nrm, (size_t)(s->n + np) * 12);
                s->uv = (float *)realloc(s->uv, (size_t)(s->n + np) * 8);
                s->mat = (int *)realloc(s->mat, (size_t)(s->n + np) * sizeof(int));
                for (v = 0; v < np; v++) {
                    float m[12] = { 0 }, tw = 0, *o = s->pos + (size_t)(s->n + v) * 3, *on = s->nrm + (size_t)(s->n + v) * 3, l;
                    const float *x = P + v * 3;
                    float nx = N && c2 == 3 && v < nn2 ? N[v * 3] : 0, ny = N && c2 == 3 && v < nn2 ? N[v * 3 + 1] : 1,
                          nz = N && c2 == 3 && v < nn2 ? N[v * 3 + 2] : 0;
                    int k, r;
                    for (k = 0; J && W && v < nj2 && v < nw && k < 4 && k < c4; k++) {
                        int jn = (int)J[v * c4 + k];
                        float w = W[v * c5 + k];
                        if (w <= 0 || jn < 0 || jn >= nj)
                            continue;
                        for (r = 0; r < 12; r++)
                            m[r] += w * skinm[jn].m[r];
                        tw += w;
                    }
                    if (tw <= 0) {                        /* unskinned: as stored */
                        memset(m, 0, sizeof m);
                        m[0] = m[5] = m[10] = 1;
                        tw = 1;
                    }
                    /* (column-major: m[0..2] column 0, m[4..6] column 1, m[8..10] column 2; translation in the
                     * 4th column, which the sum above left out: add it) */
                    {
                        float tx = 0, ty = 0, tz = 0;
                        for (k = 0; J && W && v < nj2 && v < nw && k < 4 && k < c4; k++) {
                            int jn = (int)J[v * c4 + k];
                            float w = W[v * c5 + k];
                            if (w <= 0 || jn < 0 || jn >= nj)
                                continue;
                            tx += w * skinm[jn].m[12];
                            ty += w * skinm[jn].m[13];
                            tz += w * skinm[jn].m[14];
                        }
                        o[0] = (m[0] * x[0] + m[4] * x[1] + m[8] * x[2] + tx) / tw;
                        o[1] = (m[1] * x[0] + m[5] * x[1] + m[9] * x[2] + ty) / tw;
                        o[2] = (m[2] * x[0] + m[6] * x[1] + m[10] * x[2] + tz) / tw;
                    }
                    on[0] = m[0] * nx + m[4] * ny + m[8] * nz;
                    on[1] = m[1] * nx + m[5] * ny + m[9] * nz;
                    on[2] = m[2] * nx + m[6] * ny + m[10] * nz;
                    l = sqrtf(on[0] * on[0] + on[1] * on[1] + on[2] * on[2]);
                    if (l > 1e-8f) { on[0] /= l; on[1] /= l; on[2] /= l; }
                    s->uv[(size_t)(s->n + v) * 2] = U && c3 == 2 && v < nu ? U[v * 2] : 0;
                    s->uv[(size_t)(s->n + v) * 2 + 1] = U && c3 == 2 && v < nu ? U[v * 2 + 1] : 0;
                    s->mat[s->n + v] = tex;
                }
                s->n += np;
            }
            free(P); free(N); free(U); free(J); free(W);
        }
    }
    free(parent);
    free(world);
    free(skinm);
    return s->n > 0;
}

static void src_free(Src *s)
{
    free(s->pos);
    free(s->nrm);
    free(s->uv);
    free(s->mat);
}

/* ── the game's pieces ──────────────────────────────────────────────────── */

typedef void (*PieceFn)(uint32_t piece, uint32_t si, int rigid_bone, void *ctx);

static void walk_entity(uint32_t ent, uint32_t table, int rigid_bone, PieceFn fn, void *ctx)
{
    if (!ent)
        return;
    if (MEM32(ent) == SPLIT_ENTITY_VT) {
        uint32_t n = MEM32(ent + 0x44), i;
        for (i = 0; i < n && i < 256; i++)
            fn(rel(ent + 0x48 + i * 4), table ? rel(table + i * 4) : 0, rigid_bone, ctx);
    } else
        fn(ent, table ? rel(table) : 0, rigid_bone, ctx);
}

static void walk_skin(uint32_t hdr, uint32_t skin, PieceFn fn, void *ctx)
{
    uint32_t ents = rel(hdr + 0x64 + 4), list, n, k;
    if (!ents)
        return;
    list = rel(skin + 0x3C);
    n = MEM32(skin + 0x38) + MEM32(skin + 0x40);
    for (k = 0; list && k < n && k < 64; k++) {
        uint32_t ref = list + k * 0x14, idx = MEM32(ref + 0xC) & 0xFFFFFF;
        walk_entity(MEM32(ents + idx * 0x20 + 0xC), rel(ref), -1, fn, ctx);
    }
    list = rel(skin + 0x4C);
    for (k = 0; list && k < MEM32(skin + 0x48) && k < 64; k++) {
        uint32_t ref = list + k * 0x14, idx = MEM32(ref + 0xC) & 0xFFFFFF;
        walk_entity(MEM32(ents + idx * 0x20 + 0xC), 0, (int)MEM32(ref + 8), fn, ctx);
    }
}

/* A piece's draw record: its batches (see buffy_export.c) -> each vertex's texture. */
static void piece_textures(uint32_t piece, uint32_t nv, int *tex)
{
    uint32_t hdr = MEM32(piece + 0x7C), rec = hdr ? MEM32(hdr + 0xC) : 0, ip, nb, bat, pos, b, i;
    for (i = 0; i < nv; i++)
        tex[i] = -1;
    if (!rec)
        return;
    ip = MEM32(rec + 4);
    nb = (ip - rec - 0x18) / 20;
    bat = rec + 0x18;
    {
        uint32_t w = MEM32(rec + 0x18), sz = w >> 8;
        if ((w & 0xFF) == 1 && sz >= 24 && (sz - 4) % 20 == 0 && 0x18 + sz + (sz - 4) / 20 * 4 == ip - rec) {
            nb = (sz - 4) / 20;
            bat = rec + 0x1C;
        } else if ((ip - rec - 0x18) % 20 == 4) {
            nb = (ip - rec - 0x1C) / 20;
            bat = rec + 0x1C;
        }
    }
    pos = ip;
    for (b = 0; b < nb && b < 256; b++) {
        uint32_t bt = bat + b * 20, n = MEM32(bt) + 2, t = MEM32(bt + 4), k;
        for (k = 0; k < n; k++) {
            uint32_t a = MEM16(pos + k * 2);
            if (a < nv && tex[a] < 0)
                tex[a] = (int)t;
        }
        pos += n * 2;
    }
}

typedef struct {
    Src *src;
    float old_piv[MAX_BONES][3], new_piv[MAX_BONES][3];
    int nbones;
    float scale;
    int matched, by_uv, by_pos, pieces;
    double dist;
    char *uvgrid_dummy;
} Ctx;

static void import_piece(uint32_t piece, uint32_t si, int rigid_bone, void *vc)
{
    Ctx *c = (Ctx *)vc;
    Src *s = c->src;
    uint32_t nv = MEM32(piece + 0x68), vd = rel(piece + 0x4C), pal_n = si ? MEM32(si) : 0, wrec = si ? rel(si + 8) : 0, i;
    int *tex;
    if (!vd || !nv || nv > 65536)
        return;
    c->pieces++;
    tex = (int *)malloc(nv * sizeof(int));
    piece_textures(piece, nv, tex);
    for (i = 0; i < nv; i++) {
        uint32_t a = vd + i * 24, k;
        float v[3], pr[3] = { 0, 0, 0 }, tw = 0, u0 = getf(a + 16), u1 = getf(a + 20), best = 1e30f, bestp = 1e30f;
        int bi = -1, bp = -1, j;
        v[0] = getf(a); v[1] = getf(a + 4); v[2] = -getf(a + 8);
        /* where the moved bones would put it */
        for (k = 0; k < 4; k++) {
            int bone = -1;
            float w = 0;
            if (si && wrec) {
                uint32_t r = wrec + i * 20, reg = MEM8(r + k), slot = (reg >= 60 ? reg - 3 : reg) / 3;
                w = getf(r + 4 + k * 4);
                if (!(w > 0 && w <= 1.001f) || slot >= pal_n)
                    continue;
                bone = MEM8(si + 0xC + slot);
            } else if (k == 0) {
                bone = rigid_bone;
                w = 1;
            }
            if (bone < 0 || bone >= c->nbones)
                continue;
            for (j = 0; j < 3; j++)
                pr[j] += w * (c->new_piv[bone][j] + c->scale * (v[j] - c->old_piv[bone][j]));
            tw += w;
        }
        if (tw > 0)
            for (j = 0; j < 3; j++)
                pr[j] /= tw;
        else
            memcpy(pr, v, sizeof pr);
        /* the file's vertex: same texture and coordinates, nearest the prediction */
        for (j = 0; j < s->n; j++) {
            float dx = s->pos[j * 3] - pr[0], dy = s->pos[j * 3 + 1] - pr[1], dz = s->pos[j * 3 + 2] - pr[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (d < bestp) {
                bestp = d;
                bp = j;
            }
            if (fabsf(s->uv[j * 2] - u0) < 2e-4f && fabsf(s->uv[j * 2 + 1] - u1) < 2e-4f &&
                (tex[i] < 0 || s->mat[j] < 0 || s->mat[j] == tex[i]) && d < best) {
                best = d;
                bi = j;
            }
        }
        if (bi < 0 || best > 0.25f * 0.25f) {                 /* (coordinates edited: position alone) */
            bi = bp;
            best = bestp;
            c->by_pos++;
        } else
            c->by_uv++;
        if (bi < 0)
            continue;
        c->matched++;
        c->dist += sqrt(best);
        putf(a, s->pos[bi * 3]);
        putf(a + 4, s->pos[bi * 3 + 1]);
        putf(a + 8, -s->pos[bi * 3 + 2]);
        {
            float nx = s->nrm[bi * 3], ny = s->nrm[bi * 3 + 1], nz = -s->nrm[bi * 3 + 2];
            int ix = (int)lroundf(nx * 1023.0f), iy = (int)lroundf(ny * 1023.0f), iz = (int)lroundf(nz * 511.0f);
            if (ix > 1023) ix = 1023; if (ix < -1023) ix = -1023;
            if (iy > 1023) iy = 1023; if (iy < -1023) iy = -1023;
            if (iz > 511) iz = 511; if (iz < -511) iz = -511;
            MEM32(a + 12) = ((uint32_t)ix & 0x7FF) | (((uint32_t)iy & 0x7FF) << 11) | (((uint32_t)iz & 0x3FF) << 22);
        }
    }
    free(tex);
}

/* The model file `hdr`'s skin data `skin` becomes the .glb at `path`; 1 when done
 * (or done already: its pivots are the file's). */
int buffy_model_import(uint32_t hdr, uint32_t skin, const wchar_t *path)
{
    static uint32_t s_done_skin;
    static float s_done_piv[3];
    Glb g;
    Src s;
    Ctx *c;
    uint32_t nb = skin ? MEM32(skin + 0x1C) : 0, pp = skin ? rel(skin + 0x20) : 0, hp = skin ? rel(skin + 0x28) : 0, b;
    float sum_new = 0, sum_old = 0;
    if (!hdr || !skin || !nb || nb > MAX_BONES || !pp || !hp || !path || !path[0])
        return 0;
    if (s_done_skin == skin && getf(pp) == s_done_piv[0] && getf(pp + 4) == s_done_piv[1] && getf(pp + 8) == s_done_piv[2])
        return 1;                                         /* (this load has it already) */
    if (!glb_open(&g, path)) {
        fprintf(stderr, "[IMPORT] %ls: not a readable .glb\n", path);
        return 0;
    }
    if (!src_read(&g, &s)) {
        fprintf(stderr, "[IMPORT] %ls: no skinned mesh\n", path);
        glb_close(&g);
        return 0;
    }
    c = (Ctx *)calloc(1, sizeof *c);
    c->src = &s;
    c->nbones = (int)nb;
    for (b = 0; b < nb; b++) {
        c->old_piv[b][0] = getf(pp + b * 16);
        c->old_piv[b][1] = getf(pp + b * 16 + 4);
        c->old_piv[b][2] = -getf(pp + b * 16 + 8);
        if (s.has_bone[b])
            memcpy(c->new_piv[b], s.bone_pos[b], 12);
        else
            memcpy(c->new_piv[b], c->old_piv[b], 12);
    }
    /* the overall scale: bone lengths, new over old */
    for (b = 0; b < nb; b++) {
        int p = (int)(int16_t)MEM16(hp + b * 8);
        float lo, ln;
        if (p < 0 || p >= (int)nb)
            continue;
        lo = sqrtf((c->old_piv[b][0] - c->old_piv[p][0]) * (c->old_piv[b][0] - c->old_piv[p][0]) +
                   (c->old_piv[b][1] - c->old_piv[p][1]) * (c->old_piv[b][1] - c->old_piv[p][1]) +
                   (c->old_piv[b][2] - c->old_piv[p][2]) * (c->old_piv[b][2] - c->old_piv[p][2]));
        ln = sqrtf((c->new_piv[b][0] - c->new_piv[p][0]) * (c->new_piv[b][0] - c->new_piv[p][0]) +
                   (c->new_piv[b][1] - c->new_piv[p][1]) * (c->new_piv[b][1] - c->new_piv[p][1]) +
                   (c->new_piv[b][2] - c->new_piv[p][2]) * (c->new_piv[b][2] - c->new_piv[p][2]));
        if (lo > 0.05f) {
            sum_old += lo;
            sum_new += ln;
        }
    }
    c->scale = sum_old > 0 ? sum_new / sum_old : 1;
    walk_skin(hdr, skin, import_piece, c);
    /* then the bones */
    for (b = 0; b < nb; b++) {
        putf(pp + b * 16, c->new_piv[b][0]);
        putf(pp + b * 16 + 4, c->new_piv[b][1]);
        putf(pp + b * 16 + 8, -c->new_piv[b][2]);
    }
    s_done_skin = skin;
    s_done_piv[0] = getf(pp);
    s_done_piv[1] = getf(pp + 4);
    s_done_piv[2] = getf(pp + 8);
    fprintf(stderr, "[IMPORT] %ls: %u bones, %d file vertices; %d pieces, %d vertices matched (%d by texture coordinates, "
                    "%d by position), mean distance %.4f, scale %.3f\n",
            path, nb, s.n, c->pieces, c->matched, c->by_uv, c->by_pos, c->matched ? c->dist / c->matched : 0.0, c->scale);
    free(c);
    src_free(&s);
    glb_close(&g);
    return 1;
}
