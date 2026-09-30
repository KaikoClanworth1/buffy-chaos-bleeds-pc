/*
 * The native renderer's draws (docs/native-renderer.md). Included by
 * nv2a_pb_d3d11.inc, whose device, texture cache, vertex-program translator
 * and combiner compiler it shares.
 *
 * The game side (port/src/buffy_native.c) hooks the D3D8 library's draw calls
 * and hands each draw here as a NativeDraw: the vertex attributes and indices
 * in guest memory, the textures' headers, and the library's own render and
 * texture-stage state arrays. Nothing here reads the pushbuffer: the vertex
 * programs and constants are kept by the game side's hooks
 * (nv2a_native_vp_load / _const), and the state comes from D3D's arrays.
 *
 * Only the game thread calls in (native mode: the pushbuffer executor draws
 * nothing, so the shared caches below have one user).
 */

/* getenv, remembered: debug switches are checked on every draw, and the
 * CRT's getenv walks the whole environment each time. */
static const char *nenv(const char *name)
{
    static struct { const char *name; const char *value; } cache[48];
    static int n;
    int i;
    for (i = 0; i < n; i++)
        if (cache[i].name == name || !strcmp(cache[i].name, name))
            return cache[i].value;
    if (n < 48) {
        cache[n].name = name;
        cache[n].value = getenv(name);
        return cache[n++].value;
    }
    return getenv(name);
}

/* The D3D8 library's render-state indices (Xbox layout, as in its encode
 * table: 0..56 the pixel-shader combiner registers, 57..91 "simple" states
 * whose values are NV2A register values, then deferred and complex ones). */
enum {
    NRS_PS_ALPHA_IN = 0, NRS_PS_FINAL0 = 8, NRS_PS_FINAL1 = 9, NRS_PS_CONST0 = 10, NRS_PS_CONST1 = 18,
    NRS_PS_ALPHA_OUT = 26, NRS_PS_RGB_IN = 34, NRS_PS_COMPARE = 42, NRS_PS_FINALCONST0 = 43,
    NRS_PS_FINALCONST1 = 44, NRS_PS_RGB_OUT = 45, NRS_PS_COMBINERS = 53, NRS_PS_DOTMAP = 55,
    NRS_PS_INPUTTEX = 56,
    NRS_ZFUNC = 57, NRS_ALPHAFUNC = 58, NRS_ALPHABLEND = 59, NRS_ALPHATEST = 60, NRS_ALPHAREF = 61,
    NRS_SRCBLEND = 62, NRS_DSTBLEND = 63, NRS_ZWRITE = 64, NRS_COLORWRITE = 67, NRS_STENCILZFAIL = 68,
    NRS_STENCILPASS = 69, NRS_STENCILFUNC = 70, NRS_STENCILREF = 71, NRS_STENCILMASK = 72,
    NRS_STENCILWRITEMASK = 73, NRS_BLENDOP = 74,
    NRS_FOGENABLE = 92, NRS_FOGTABLEMODE = 93, NRS_FOGSTART = 94, NRS_FOGEND = 95, NRS_FOGDENSITY = 96,
    NRS_PSTEXTUREMODES = 136, NRS_FOGCOLOR = 138, NRS_ZENABLE = 143, NRS_STENCILENABLE = 144,
    NRS_STENCILFAIL = 145, NRS_FRONTFACE = 146, NRS_CULLMODE = 147, NRS_TEXTUREFACTOR = 148
};
/* Texture-stage state indices (32 DWORDs a stage). */
enum {
    NTS_ADDRESSU = 0, NTS_ADDRESSV = 1, NTS_ADDRESSW = 2, NTS_MAGFILTER = 3, NTS_MINFILTER = 4,
    NTS_MIPFILTER = 5, NTS_COLOROP = 12, NTS_COLORARG0 = 13, NTS_COLORARG1 = 14, NTS_COLORARG2 = 15,
    NTS_ALPHAOP = 16, NTS_ALPHAARG0 = 17, NTS_ALPHAARG1 = 18, NTS_ALPHAARG2 = 19, NTS_RESULTARG = 20,
    NTS_BORDERCOLOR = 29
};

typedef struct {
    uint32_t va, stride, fmt;          /* fmt: NV2A array format, low byte type | size << 4 (size 0: none) */
} NativeAttr;

typedef struct {
    uint32_t res;                      /* guest texture header (Common, Data, Lock, Format, Size), 0 none */
    uint32_t palette;                  /* guest palette header for P8, or 0 */
    uint32_t data, format;             /* when data: the texels and format SwitchTexture gave (swizzled) */
    ID3D11ShaderResourceView *srv;     /* when set: a render target drawn natively, sampled as is */
} NativeTex;

typedef struct {
    uint32_t prim;                     /* D3DPT: 1 points .. 10 polygon */
    uint32_t count;                    /* vertices, or indices when index_va */
    uint32_t index_va;                 /* 16-bit indices, or 0 */
    NativeAttr attr[16];
    int passthrough;                   /* screen-space positions (XYZRHW), no program */
    uint32_t vp_start;                 /* program start slot otherwise */
    NativeTex tex[4];
    int pixel_shader;                  /* the combiner registers in rs[0..56] are live */
    const uint32_t *rs;                /* D3D__RenderState */
    const uint32_t *tss;               /* D3D__TextureState, [4][32] */
} NativeDraw;

static uint32_t s_nvp_code[VP_SLOTS][4];     /* the program memory, as loaded */
static float    s_nconst[192][4];            /* the vertex shader constants */
static uint32_t s_nvp_gen = 1;               /* bumped on every program load */
static uint32_t s_nvb_pos, s_nib_pos;        /* ring positions in the shared vertex / index buffers */
static uint32_t s_ntex_regs[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint32_t *s_tex_src_saved;
static unsigned long long s_nstat[8];        /* draws: ok, no-vs, too-big, failed... */

/* A vertex shader's load block (the D3D library builds it when the shader is
 * created and copies it as is at LoadVertexShader): method headers with
 * their data -- program words (0x0B00..), and a shader's own constants
 * (0x1EA4 load slot, 0x0B80.. values) -- to program memory from `slot`. */
void nv2a_native_vp_load(uint32_t slot, const uint32_t *block, uint32_t dwords)
{
    s_nvp_gen++;
    uint32_t i = 0, word = slot * 4, cslot = 0, cword = 0;
    while (i < dwords) {
        uint32_t h = block[i++], count = (h >> 18) & 0x7FF, method = h & 0x1FFC, k;
        if (h & 3 || !count)                    /* not a method header: stop */
            break;
        for (k = 0; k < count && i < dwords; k++, i++) {
            uint32_t m = (h & 0x40000000u) ? method : method + k * 4;
            if (m >= 0x0B00 && m < 0x0B80) {
                if (word / 4 < VP_SLOTS)
                    s_nvp_code[word / 4][word & 3] = block[i];
                word++;
            } else if (m == 0x1E9C) {
                word = block[i] * 4;            /* the block's own load address */
            } else if (m == 0x1EA4) {
                cslot = block[i];
                cword = 0;
            } else if (m >= 0x0B80 && m < 0x0C00) {
                uint32_t c = cslot + cword / 4;
                if (c < 192)
                    memcpy(&s_nconst[c][cword & 3], &block[i], 4);
                cword++;
            }
        }
    }
}

void nv2a_native_const(uint32_t slot, const float *v, uint32_t vectors)
{
    uint32_t i;
    for (i = 0; i < vectors && slot + i < 192; i++)
        memcpy(s_nconst[slot + i], v + i * 4, 16);
}

/* The D3D library's constant pool, which SetShaderConstantMode writes to
 * slots 60..62 (programs use it for 0, 0.5, 1, 2 and -1 literals). */
void nv2a_native_const_pool(void)
{
    static const float pool[3][4] = { { 0.0f, 0.5f, 1.0f, 2.0f }, { -1.0f, 0.0f, 1.0f, 2.0f }, { 0.0f, 0.0f, -1.0f, 0.0f } };
    memcpy(s_nconst[60], pool, sizeof pool);
    s_nconst[0][3] = 1.0f;
}

void nv2a_native_const_get(uint32_t slot, float out[4])
{
    if (slot < 192)
        memcpy(out, s_nconst[slot], 16);
}

/* ── vertex shaders ── */

typedef struct {
    uint32_t hash, len, start;
    uint32_t code[VP_SLOTS][4];
    ID3D11VertexShader *vs;
    int uses_viewport;
    uint32_t mask;                     /* attributes read packed (see nvs_packed_inputs) */
} NVsEntry;

#define NVS_CACHE 256
static NVsEntry *s_nvs;
static int       s_nvs_count;
static ID3D11VertexShader *s_npass_vs;       /* screen-space positions */

/* Input signatures for building input layouts, by packed-attribute mask
 * (programs with the same mask share one). */
static struct { uint32_t mask; ID3DBlob *sig; } s_nvs_sigs[16];
static int s_nvs_nsig;
static ID3DBlob *s_nvs_sig;                  /* mask 0's */

static ID3DBlob *nvs_sig(uint32_t mask)
{
    int i;
    for (i = 0; i < s_nvs_nsig; i++)
        if (s_nvs_sigs[i].mask == mask)
            return s_nvs_sigs[i].sig;
    return NULL;
}

/* A program's source with attributes in `mask` read as packed 11:11:10
 * normals (NV2A's CMP type, which D3D11 has no format for): taken in as a
 * uint and unpacked as fetch_attr does. */
static void nvs_packed_inputs(const char *in, char *out, int cap, uint32_t mask)
{
    static const char vi[] = "struct VI { float4 v[16] : ATTR; };";
    static const char entry[] = "P vs(VI I) {";
    const char *p = strstr(in, vi), *q = strstr(in, entry);
    Sb b;
    int a;
    b.p = out; b.n = 0; b.cap = cap;
    out[0] = 0;
    if (!p || !q || q < p) { sb_add(&b, "%s", in); return; }
    sb_add(&b, "%.*s", (int)(p - in), in);
    sb_add(&b, "struct VI { float4 v[16]; };\n" "struct VR {");
    for (a = 0; a < 16; a++)
        sb_add(&b, (mask >> a & 1) ? " uint a%d : ATTR%d;" : " float4 a%d : ATTR%d;", a, a);
    sb_add(&b, " };\n");
    sb_add(&b, "float4 cmpn(uint t) { int x = ((int)(t << 21)) >> 21, y = ((int)((t >> 11) << 21)) >> 21, z = ((int)(t & 0xFFC00000u)) >> 22;\n"
               "  return float4(x / 1023.0, y / 1023.0, z / 511.0, 1.0); }\n");
    sb_add(&b, "%.*s", (int)(q - (p + sizeof vi - 1)), p + sizeof vi - 1);
    sb_add(&b, "P vs(VR R) { VI I;");
    for (a = 0; a < 16; a++)
        sb_add(&b, (mask >> a & 1) ? " I.v[%d] = cmpn(R.a%d);" : " I.v[%d] = R.a%d;", a, a);
    sb_add(&b, "%s", q + sizeof entry - 1);
}

static ID3D11VertexShader *nvs_compile_mask(const char *src, uint32_t mask)
{
    static char patched[70000];
    ID3DBlob *blob = NULL;
    ID3D11VertexShader *vs = NULL;
    if (mask) {
        nvs_packed_inputs(src, patched, sizeof patched, mask);
        src = patched;
    }
    if (FAILED(gpu_compile(src, "vs", "vs_4_0", &blob)) || !blob)
        return NULL;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                    ID3D10Blob_GetBufferSize(blob), NULL, &vs);
    if (!nvs_sig(mask) && s_nvs_nsig < 16) {  /* the layouts for this mask are made against it */
        s_nvs_sigs[s_nvs_nsig].mask = mask;
        s_nvs_sigs[s_nvs_nsig].sig = blob;
        s_nvs_nsig++;
        ID3D10Blob_AddRef(blob);
        if (!mask) s_nvs_sig = blob;
    }
    if (!s_vs_layout && !mask) {
        D3D11_INPUT_ELEMENT_DESC il[16];
        int a;
        for (a = 0; a < 16; a++) {
            il[a].SemanticName = "ATTR";
            il[a].SemanticIndex = (UINT)a;
            il[a].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            il[a].InputSlot = 0;
            il[a].AlignedByteOffset = (UINT)(a * 16);
            il[a].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            il[a].InstanceDataStepRate = 0;
        }
        ID3D11Device_CreateInputLayout(s_dev, il, 16, ID3D10Blob_GetBufferPointer(blob),
                                       ID3D10Blob_GetBufferSize(blob), &s_vs_layout);
    }
    ID3D10Blob_Release(blob);
    return vs;
}

static ID3D11VertexShader *nvs_compile(const char *src)
{
    return nvs_compile_mask(src, 0);
}

/* The program at `start`, translated (cached by its code and mask). */
static ID3D11VertexShader *nvs_program(uint32_t start, int *uses_viewport, uint32_t mask)
{
    static char src[65536];
    static NV2AVshProgram prog;
    uint32_t hash = 2166136261u, k, len;
    NVsEntry *e;
    int i;

    static uint32_t last_start = 0xFFFFFFFFu, last_gen, last_mask;
    static ID3D11VertexShader *last_vs;
    static int last_uv;
    if (start >= VP_SLOTS)
        return NULL;
    if (start == last_start && s_nvp_gen == last_gen && mask == last_mask && last_vs) {
        *uses_viewport = last_uv;
        return last_vs;
    }
    vp_decode_program((const uint32_t (*)[4])&s_nvp_code[start], (int)(VP_SLOTS - start), &prog);
    len = (uint32_t)prog.length;
    if (!len)
        return NULL;
    {
        const uint32_t *c = &s_nvp_code[start][0];
        for (k = 0; k < len * 4; k++)
            hash = (hash ^ c[k]) * 16777619u;
        hash ^= mask * 0x9E3779B1u;
    }
    if (!s_nvs && !(s_nvs = (NVsEntry *)calloc(NVS_CACHE, sizeof(NVsEntry))))
        return NULL;
    for (i = 0; i < s_nvs_count; i++) {
        e = &s_nvs[i];
        if (e->hash == hash && e->len == len && e->mask == mask && !memcmp(e->code, s_nvp_code[start], len * 16)) {
            *uses_viewport = e->uses_viewport;
            last_start = start; last_gen = s_nvp_gen; last_mask = mask; last_vs = e->vs; last_uv = e->uses_viewport;
            return e->vs;
        }
    }
    if (s_nvs_count >= NVS_CACHE || !vsh_generate(&prog, src, sizeof src))
        return NULL;
    e = &s_nvs[s_nvs_count];
    e->vs = nvs_compile_mask(src, mask);
    if (!e->vs)
        return NULL;
    e->hash = hash; e->len = len; e->start = start; e->mask = mask;
    memcpy(e->code, s_nvp_code[start], len * 16);
    e->uses_viewport = 0;
    for (k = 0; k < len; k++)
        for (i = 0; i < 3; i++)
            if (prog.insns[k].mac_src[i].reg_type == NV2A_VSH_REG_CONST && prog.insns[k].mac_src[i].reg_index == 58)
                e->uses_viewport = 1;
    *uses_viewport = e->uses_viewport;
    s_nvs_count++;
    last_start = start; last_gen = s_nvp_gen; last_mask = mask; last_vs = e->vs; last_uv = e->uses_viewport;
    return e->vs;
}

/* Screen-space vertices (XYZRHW): pixels of the 640x480 surface to clip space. */
static ID3D11VertexShader *nvs_passthrough(void)
{
    if (!s_npass_vs) {
        static char src[4096];
        sprintf_s(src, sizeof src, "%s"
            "P vs(VI I) {\n"
            "  P p; float4 v = I.v[0];\n"
            "  float w = (v.w != 0.0) ? 1.0 / v.w : 1.0;\n"
            "  p.p = float4(v.x / (vpp[1].x * 0.5) - 1.0, 1.0 - v.y / (vpp[1].y * 0.5), saturate(v.z), 1.0) * w;\n"
            "  p.c0 = saturate(I.v[3]); p.c1 = saturate(I.v[4]);\n"
            "  p.t0 = I.v[9]; p.t1 = I.v[10]; p.t2 = I.v[11]; p.t3 = I.v[12];\n"
            "  p.fog = float4(1, 0, 0, 1);\n"
            "  return p; }\n", s_vsh_head);
        s_npass_vs = nvs_compile(src);
    }
    return s_npass_vs;
}

/* ── pixel shaders: fixed function (texture stages) ── */

typedef struct {
    uint32_t op[4][6];                 /* per stage: colorop, arg1, arg2, alphaop, arg1, arg2 */
    uint32_t arg0[4][2];               /* colorarg0, alphaarg0 (3-argument ops) */
    uint32_t bound;                    /* bit per stage: a texture is bound */
} NFfKey;

typedef struct { NFfKey key; ID3D11PixelShader *ps; } NFfEntry;
#define NFF_CACHE 256
static NFfEntry s_nff[NFF_CACHE];
static int      s_nff_count;

/* A D3DTA argument. */
static void nff_arg(Sb *b, uint32_t a, int st, int alpha)
{
    const char *base;
    char tmp[32];
    switch (a & 0xF) {
    case 0:  base = "i.c0"; break;                   /* DIFFUSE */
    case 1:  base = st ? "cur" : "i.c0"; break;      /* CURRENT (stage 0: diffuse) */
    case 2:  sprintf_s(tmp, sizeof tmp, "t%d", st); base = tmp; break;   /* TEXTURE */
    case 3:  base = "cf0[0]"; break;                 /* TFACTOR */
    case 4:  base = "i.c1"; break;                   /* SPECULAR */
    case 5:  base = "tmp"; break;                    /* TEMP */
    default: base = "float4(1,1,1,1)"; break;
    }
    if (a & 0x20)                                     /* ALPHAREPLICATE */
        sb_add(b, (a & 0x10) ? "(1.0 - %s.aaaa)" : "(%s.aaaa)", base);
    else
        sb_add(b, (a & 0x10) ? "(1.0 - %s)" : "(%s)", base);
    (void)alpha;
}

/* One D3DTOP into `dst` (a float4 of which .rgb or .a is used). */
static void nff_op(Sb *b, uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2, int st, int alpha)
{
    sb_add(b, "  { float4 A = "); nff_arg(b, a1, st, alpha);
    sb_add(b, "; float4 B = "); nff_arg(b, a2, st, alpha);
    sb_add(b, "; float4 Z = "); nff_arg(b, a0, st, alpha);
    sb_add(b, "; float4 r;\n    ");
    switch (op) {
    case 2:  sb_add(b, "r = A;"); break;                                  /* SELECTARG1 */
    case 3:  sb_add(b, "r = B;"); break;                                  /* SELECTARG2 */
    case 4:  sb_add(b, "r = A * B;"); break;                              /* MODULATE */
    case 5:  sb_add(b, "r = A * B * 2.0;"); break;                        /* MODULATE2X */
    case 6:  sb_add(b, "r = A * B * 4.0;"); break;                        /* MODULATE4X */
    case 7:  sb_add(b, "r = A + B;"); break;                              /* ADD */
    case 8:  sb_add(b, "r = A + B - 0.5;"); break;                        /* ADDSIGNED */
    case 9:  sb_add(b, "r = (A + B - 0.5) * 2.0;"); break;                /* ADDSIGNED2X */
    case 10: sb_add(b, "r = A - B;"); break;                              /* SUBTRACT */
    case 11: sb_add(b, "r = A + B - A * B;"); break;                      /* ADDSMOOTH */
    case 12: sb_add(b, "r = lerp(B, A, i.c0.a);"); break;                 /* BLENDDIFFUSEALPHA */
    case 13: sb_add(b, "r = lerp(B, A, t%d.a);", st); break;              /* BLENDTEXTUREALPHA */
    case 14: sb_add(b, "r = lerp(B, A, cf0[0].a);"); break;               /* BLENDFACTORALPHA */
    case 15: sb_add(b, "r = A + B * (1.0 - t%d.a);", st); break;          /* BLENDTEXTUREALPHAPM */
    case 16: sb_add(b, "r = lerp(B, A, cur.a);"); break;                  /* BLENDCURRENTALPHA */
    case 18: sb_add(b, "r = A + A.a * B;"); break;                        /* MODULATEALPHA_ADDCOLOR */
    case 19: sb_add(b, "r = A * B + A.a;"); break;                        /* MODULATECOLOR_ADDALPHA */
    case 20: sb_add(b, "r = A + (1.0 - A.a) * B;"); break;                /* MODULATEINVALPHA_ADDCOLOR */
    case 21: sb_add(b, "r = (1.0 - A) * B + A.a;"); break;                /* MODULATEINVCOLOR_ADDALPHA */
    case 22: sb_add(b, "r = saturate(dot(A.rgb - 0.5, B.rgb - 0.5) * 4.0).xxxx;"); break;  /* DOTPRODUCT3 */
    case 23: sb_add(b, "r = Z + A * B;"); break;                          /* MULTIPLYADD */
    case 24: sb_add(b, "r = lerp(B, A, Z);"); break;                      /* LERP */
    default: sb_add(b, "r = A;"); break;
    }
    sb_add(b, "\n    %s = saturate(r)%s; }\n", alpha ? "res.a" : "res.rgb", alpha ? ".a" : ".rgb");
}

static int nff_generate(const NFfKey *k, char *out, int cap)
{
    Sb b;
    int st;
    b.p = out; b.n = 0; b.cap = cap;
    out[0] = 0;
    sb_add(&b, "%s", s_psh_head);
    sb_add(&b, "float4 ps(P i) : SV_Target {\n");
    sb_add(&b, "  float4 t0 = 0, t1 = 0, t2 = 0, t3 = 0, tmp = 0;\n");
    for (st = 0; st < 4; st++)
        if (k->bound & (1u << st))
            sb_add(&b, "  t%d = T%d.Sample(S%d, i.t%d.xy / (i.t%d.w != 0.0 ? i.t%d.w : 1.0) * texnorm[%d].xy);\n",
                   st, st, st, st, st, st, st);
    sb_add(&b, "  float4 cur = i.c0;\n");
    for (st = 0; st < 4; st++) {
        const uint32_t *o = k->op[st];
        if (o[0] <= 1)                                     /* DISABLE ends the cascade */
            break;
        sb_add(&b, "  { float4 res = cur;\n");
        nff_op(&b, o[0], k->arg0[st][0], o[1], o[2], st, 0);
        nff_op(&b, o[3] <= 1 ? 2 : o[3], k->arg0[st][1], o[3] <= 1 ? 1 : o[4], o[5], st, 1);
        sb_add(&b, "  cur = res; }\n");
    }
    sb_add(&b, "  float4 o = saturate(cur);\n");
    if (nenv("BUFFY_NATIVE_ALPHATEX") && (k->bound & 1))
        sb_add(&b, "  o.a = t0.a;\n");
    sb_add(&b, "  o.rgb = lerp(fogc.rgb, o.rgb, saturate(i.fog.x));\n");
    sb_add(&b, "  if (alpha.z > 0.5 && !cmpf(alpha.y, floor(o.a * 255.0 + 0.5), alpha.x)) discard;\n");
    sb_add(&b, "  return o;\n}\n");
    return b.n < cap - 1;
}

static ID3D11PixelShader *nff_get(const NFfKey *k)
{
    static char src[16384];
    ID3DBlob *blob = NULL;
    ID3D11PixelShader *ps = NULL;
    int i;
    for (i = 0; i < s_nff_count; i++)
        if (!memcmp(&s_nff[i].key, k, sizeof *k))
            return s_nff[i].ps;
    if (!nff_generate(k, src, sizeof src))
        return NULL;
    if (SUCCEEDED(gpu_compile(src, "ps", "ps_4_0", &blob)) && blob) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                       ID3D10Blob_GetBufferSize(blob), NULL, &ps);
        ID3D10Blob_Release(blob);
    } else {
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "  [NATIVE] fixed-function shader failed:\n%s\n", src);
    }
    if (s_nff_count < NFF_CACHE) {
        s_nff[s_nff_count].key = *k;
        s_nff[s_nff_count].ps = ps;
        s_nff_count++;
    }
    return ps;
}

/* ── state ── */

static void nstate_apply(const uint32_t *rs)
{
    static ID3D11BlendState *bcache[128]; static uint32_t bkey[128][2];
    static ID3D11DepthStencilState *dss[128]; static uint32_t dkey[128][4];
    static ID3D11RasterizerState *rsn;
    uint32_t k[4];
    int i;

    /* blend */
    k[0] = (rs[NRS_ALPHABLEND] ? 1u : 0u) | (rs[NRS_SRCBLEND] & 0xFFFF) << 1 | (rs[NRS_DSTBLEND] & 0x7FFF) << 17;
    k[1] = rs[NRS_COLORWRITE] ^ (rs[NRS_BLENDOP] << 4);
    for (i = 0; i < 128 && bcache[i] && (bkey[i][0] != k[0] || bkey[i][1] != k[1]); i++) ;
    if (i < 128 && !bcache[i]) {
        D3D11_BLEND_DESC d;
        uint32_t cm = rs[NRS_COLORWRITE];
        memset(&d, 0, sizeof d);
        d.RenderTarget[0].BlendEnable = rs[NRS_ALPHABLEND] != 0;
        d.RenderTarget[0].SrcBlend = gpu_blend(rs[NRS_SRCBLEND]);
        d.RenderTarget[0].DestBlend = gpu_blend(rs[NRS_DSTBLEND]);
        switch (rs[NRS_BLENDOP]) {
        case 0x800A: d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_SUBTRACT; break;
        case 0x800B: d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
        case 0x8007: d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MIN; break;
        case 0x8008: d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX; break;
        default:     d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; break;
        }
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask =
            ((cm & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
          | ((cm & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
          | ((cm & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
          | ((cm & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
        ID3D11Device_CreateBlendState(s_dev, &d, &bcache[i]);
        bkey[i][0] = k[0]; bkey[i][1] = k[1];
    }
    if (i < 128)
        ID3D11DeviceContext_OMSetBlendState(s_ctx, bcache[i], NULL, 0xFFFFFFFFu);

    /* depth + stencil */
    {
        static int nd;
        uint32_t zen = rs[NRS_ZENABLE] != 0, sten = rs[NRS_STENCILENABLE] != 0;
        k[0] = zen | (rs[NRS_ZFUNC] & 7) << 1 | (rs[NRS_ZWRITE] ? 16u : 0u) | (sten ? 32u : 0u)
             | (rs[NRS_STENCILFUNC] & 7) << 6 | (rs[NRS_STENCILWRITEMASK] & 0xFF) << 9 | (rs[NRS_STENCILMASK] & 0xFF) << 17;
        k[1] = sten ? rs[NRS_STENCILFAIL] : 0; k[2] = sten ? rs[NRS_STENCILZFAIL] : 0; k[3] = sten ? rs[NRS_STENCILPASS] : 0;
        for (i = 0; i < nd && memcmp(dkey[i], k, sizeof k); i++) ;
        if (i == nd && nd < 128) {
            D3D11_DEPTH_STENCIL_DESC d;
            memset(&d, 0, sizeof d);
            d.DepthEnable = zen;
            d.DepthWriteMask = rs[NRS_ZWRITE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            d.DepthFunc = zen ? gpu_cmp(rs[NRS_ZFUNC]) : D3D11_COMPARISON_ALWAYS;
            d.StencilEnable = sten;
            d.StencilReadMask = (UINT8)rs[NRS_STENCILMASK];
            d.StencilWriteMask = (UINT8)rs[NRS_STENCILWRITEMASK];
            d.FrontFace.StencilFunc = gpu_cmp(rs[NRS_STENCILFUNC]);
            d.FrontFace.StencilFailOp = gpu_stencil_op(rs[NRS_STENCILFAIL]);
            d.FrontFace.StencilDepthFailOp = gpu_stencil_op(rs[NRS_STENCILZFAIL]);
            d.FrontFace.StencilPassOp = gpu_stencil_op(rs[NRS_STENCILPASS]);
            d.BackFace = d.FrontFace;
            ID3D11Device_CreateDepthStencilState(s_dev, &d, &dss[nd]);
            memcpy(dkey[nd], k, sizeof k);
            nd++;
        }
        if (i < nd)
            ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss[i], rs[NRS_STENCILREF] & 0xFF);
    }

    if (!rsn) {
        D3D11_RASTERIZER_DESC d;
        memset(&d, 0, sizeof d);
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = FALSE;
        ID3D11Device_CreateRasterizerState(s_dev, &d, &rsn);
    }
    ID3D11DeviceContext_RSSetState(s_ctx, rsn);
}

/* ── textures: the header and stage state as NV2A texture registers, for
 * the shared cache (gpu_texture / tx_sampler read them through s_tex_src) ── */

static void ntex_regs(int st, const NativeTex *t, const uint32_t *tss)
{
    uint32_t base = (st * 0x40) / 4, fmt, size;
    uint32_t *r = s_ntex_regs;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
#define NTR(m) r[((m) - NV_TEX_FIRST) / 4 + base]
    memset(&NTR(0x1B00), 0, 0x40);
    if (!t->res && !t->data)
        return;
    if (t->data) {                                                     /* SwitchTexture's */
        fmt = t->format;
        size = 0;
        NTR(0x1B00) = t->data;
    } else {
        fmt = *(const uint32_t *)(mem + t->res + 0xC);
        size = *(const uint32_t *)(mem + t->res + 0x10);
        NTR(0x1B00) = *(const uint32_t *)(mem + t->res + 4);        /* offset */
    }
    NTR(0x1B04) = fmt;
    NTR(0x1B0C) = 0x40000000u | (0xFFFu << 18);                      /* enable, all mip levels */
    if (size) {                                                        /* linear: D3DSIZE */
        uint32_t w = (size & 0xFFF) + 1, h = ((size >> 12) & 0xFFF) + 1, pitch = ((size >> 24) + 1) * 64;
        NTR(0x1B10) = pitch << 16;
        NTR(0x1B1C) = (w << 16) | h;
    }
    if (t->palette)
        NTR(0x1B20) = *(const uint32_t *)(mem + t->palette + 4);
    {
        uint32_t au = tss[NTS_ADDRESSU], av = tss[NTS_ADDRESSV], aw = tss[NTS_ADDRESSW];
        if (!au || au > 5) au = 1;
        if (!av || av > 5) av = 1;
        if (!aw || aw > 5) aw = 1;
        NTR(0x1B08) = au | (av << 8) | (aw << 16);
    }
    {
        uint32_t mag = tss[NTS_MAGFILTER] >= 2 ? 2 : 1, min = tss[NTS_MINFILTER] >= 2 ? 2 : 1;
        uint32_t mip = tss[NTS_MIPFILTER], m;
        if (mip == 0) m = min;                                           /* no mips */
        else if (mip == 1) m = min == 2 ? 4 : 3;                         /* nearest mip */
        else m = min == 2 ? 6 : 5;                                       /* linear mip */
        NTR(0x1B14) = (m << 16) | (mag << 24);
    }
    NTR(0x1B24) = tss[NTS_BORDERCOLOR];
#undef NTR
}

/* ── the draw ── */

/* One attribute of one vertex, from guest memory, as float4 (w 1 by default).
 * The native renderer's own: the pushbuffer executor's fetch_attr looks at
 * that thread's batch state (an inline-array batch reads from its buffer
 * instead), and calling it from the game thread while the executor was in
 * such a batch failed the fetch -- a vertex fell back to (0,0,0,1): the
 * menu's transparent fade quad got an opaque corner and flashed dark, and
 * meshes stretched across the screen. */
static int ndraw_fetch(const NativeAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t type = a->fmt & 0xF, size = (a->fmt >> 4) & 0xF, i;
    const uint8_t *p;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!size || !a->stride || !a->va)
        return 0;
    p = mem + a->va + (size_t)index * a->stride;
    switch (type) {
    case 0:                                  /* D3DCOLOR: bytes B,G,R,A */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    case 1:                                  /* S1: signed short, normalised */
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i] / 32767.0f;
        return 1;
    case 5:                                  /* S32K: signed short, as is */
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* CMP: packed 11:11:10 signed normalised */
        uint32_t t = *(const uint32_t *)p;
        int32_t xx = (int32_t)(t << 21) >> 21;
        int32_t yy = (int32_t)((t >> 11) << 21) >> 21;
        int32_t zz = (int32_t)(t >> 22 << 22) >> 22;
        out[0] = (float)xx / 1023.0f;
        out[1] = (float)yy / 1023.0f;
        out[2] = (float)zz / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

/* ── GPU vertex buffers: the game's vertex data uploaded once and read by the
 * input assembler in its own formats (instead of unpacking every vertex of
 * every draw on the CPU). Streams are cached by address and stride, checked
 * against a hash of their bytes at most once a frame. ── */

typedef struct {
    uint32_t va, stride, bytes;
    uint64_t hash;
    uint32_t checked;                  /* s_frame_no of the last hash check */
    DWORD last_use;
    ID3D11Buffer *buf;
} NVbEntry;

#define NVB_CACHE 1024
static NVbEntry s_nvb[NVB_CACHE];

typedef struct {
    uint32_t key[17];                  /* per attribute: slot << 24 | offset << 8 | dxgi-ish code; [16] mask */
    ID3D11InputLayout *il;
} NIlEntry;

#define NIL_CACHE 256
static NIlEntry s_nil[NIL_CACHE];
static int s_nil_count;
static ID3D11Buffer *s_ndefaults;      /* 16 float4: what an absent attribute reads */
static unsigned long long s_nvb_stat[3]; /* GPU path, CPU fallback, uploads */

static uint64_t nvb_hash(const uint8_t *p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* The DXGI format an NV2A array format reads as, or UNKNOWN (CPU path). */
static DXGI_FORMAT nvb_format(uint32_t fmt)
{
    uint32_t type = fmt & 0xF, size = (fmt >> 4) & 0xF;
    switch (type) {
    case 2:
        return size == 1 ? DXGI_FORMAT_R32_FLOAT : size == 2 ? DXGI_FORMAT_R32G32_FLOAT
             : size == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : size == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
             : DXGI_FORMAT_UNKNOWN;
    case 0:
        return size == 4 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_UNKNOWN;
    case 4:
        return size == 4 ? DXGI_FORMAT_R8G8B8A8_UNORM : size == 2 ? DXGI_FORMAT_R8G8_UNORM
             : size == 1 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_UNKNOWN;
    case 1:
        return size == 4 ? DXGI_FORMAT_R16G16B16A16_SNORM : size == 2 ? DXGI_FORMAT_R16G16_SNORM
             : size == 1 ? DXGI_FORMAT_R16_SNORM : DXGI_FORMAT_UNKNOWN;
    case 6:                            /* CMP: packed 11:11:10, unpacked by the program (nvs_packed_inputs) */
        return size == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

/* Can the GPU read all of `d`'s attributes as they are? *mask: the packed ones. */
static int nvb_supported(const NativeDraw *d, uint32_t *mask)
{
    int a;
    *mask = 0;
    for (a = 0; a < 16; a++) {
        const NativeAttr *at = &d->attr[a];
        if (!at->fmt || !((at->fmt >> 4) & 0xF) || !at->va)
            continue;
        if (nvb_format(at->fmt) == DXGI_FORMAT_UNKNOWN || !at->stride)
            return 0;
        if ((at->fmt & 0xF) == 6)
            *mask |= 1u << a;
    }
    return 1;
}

/* The buffer for `bytes` of vertex data at `va` (made or refreshed). */
static ID3D11Buffer *nvb_stream(uint32_t va, uint32_t stride, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    NVbEntry *e = NULL;
    DWORD now = GetTickCount();
    int i, lru = 0;
    for (i = 0; i < NVB_CACHE; i++) {
        if (s_nvb[i].buf && s_nvb[i].va == va && s_nvb[i].stride == stride) { e = &s_nvb[i]; break; }
        if (!s_nvb[i].buf) { lru = i; break; }
        if (s_nvb[i].last_use < s_nvb[lru].last_use) lru = i;
    }
    if (e && e->bytes >= bytes) {
        e->last_use = now;
        if (e->checked == s_frame_no && nenv("BUFFY_NATIVE_VBCHECK")) {
            /* debug: data rewritten since this frame's check -> stale draw */
            static unsigned n;
            if (nvb_hash(mem + va, e->bytes) != e->hash && n++ < 40)
                fprintf(stderr, "[NATIVE] stale VB va=%08X stride=%u bytes=%u frame=%u\n",
                        va, stride, e->bytes, s_frame_no);
        }
        if (e->checked != s_frame_no) {
            uint64_t h = nvb_hash(mem + va, e->bytes);
            e->checked = s_frame_no;
            if (h != e->hash) {
                e->hash = h;
                ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->buf, 0, NULL, mem + va, 0, 0);
                s_nvb_stat[2]++;
            }
        }
        return e->buf;
    }
    if (!e)
        e = &s_nvb[lru];
    if (e->buf)
        ID3D11Buffer_Release(e->buf);
    memset(e, 0, sizeof *e);
    {
        D3D11_BUFFER_DESC bd;
        D3D11_SUBRESOURCE_DATA sd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (bytes + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sd.pSysMem = mem + va;
        sd.SysMemPitch = sd.SysMemSlicePitch = 0;
        if (bd.ByteWidth != bytes) {
            /* the data rounded up: whatever follows in guest memory is harmless */
        }
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, &sd, &e->buf)))
            return NULL;
    }
    e->va = va; e->stride = stride; e->bytes = bytes;
    e->hash = nvb_hash(mem + va, bytes);
    e->checked = s_frame_no;
    e->last_use = now;
    s_nvb_stat[2]++;
    return e->buf;
}

/* Set up the input assembler for `d`'s attributes straight from guest memory
 * (streams 0..n-1, defaults in slot 15). 0 when an attribute needs the CPU. */
static int nvb_bind(const NativeDraw *d, uint32_t vmax, uint32_t mask)
{
    ID3DBlob *sig = nvs_sig(mask);
    ID3D11Buffer *bufs[16];
    UINT strides[16], offs[16];
    uint32_t base[16], sstride[16], key[17];
    int nstream = 0, a, i;
    ID3D11InputLayout *il = NULL;

    if (!sig)
        return 0;
    memset(key, 0, sizeof key);
    for (a = 0; a < 16; a++) {
        const NativeAttr *at = &d->attr[a];
        DXGI_FORMAT f;
        if (!at->fmt || !((at->fmt >> 4) & 0xF) || !at->va) {
            key[a] = 0xFFFFFFFFu;              /* absent: the default */
            continue;
        }
        f = nvb_format(at->fmt);
        if (f == DXGI_FORMAT_UNKNOWN || !at->stride) {
            static unsigned seen[256];
            if (nenv("BUFFY_NATIVE_LOG") && seen[at->fmt & 0xFF]++ == 0)
                fprintf(stderr, "[NATIVE] vertex format %02X (attribute %d, stride %u) needs the CPU\n", at->fmt, a, at->stride);
            return 0;
        }
        /* the stream: attributes with this stride within one stride of each other */
        for (i = 0; i < nstream; i++)
            if (sstride[i] == at->stride && at->va >= base[i] && at->va < base[i] + at->stride)
                break;
        if (i == nstream) {
            int j;
            if (nstream >= 14)
                return 0;
            base[i] = at->va;
            sstride[i] = at->stride;
            /* an earlier attribute of the same stream may start lower */
            for (j = 0; j < a; j++)
                if (key[j] != 0xFFFFFFFFu && d->attr[j].stride == at->stride
                        && d->attr[j].va < base[i] && at->va - d->attr[j].va < at->stride)
                    base[i] = d->attr[j].va;
            nstream++;
        }
        key[a] = (uint32_t)i << 24 | ((at->va - base[i]) & 0xFFFF) << 8 | (uint32_t)f;
    }
    /* offsets relative to their stream's base (bases may have moved down) */
    for (a = 0; a < 16; a++)
        if (key[a] != 0xFFFFFFFFu) {
            int s = (int)(key[a] >> 24);
            key[a] = (uint32_t)s << 24 | ((d->attr[a].va - base[s]) & 0xFFFF) << 8 | (key[a] & 0xFF);
            if (d->attr[a].va - base[s] >= sstride[s])
                return 0;
        }
    key[16] = mask;
    for (i = 0; i < nstream; i++) {
        bufs[i] = nvb_stream(base[i], sstride[i], (vmax + 1) * sstride[i]);
        if (!bufs[i])
            return 0;
        strides[i] = sstride[i];
        offs[i] = 0;
    }
    /* the input layout for this arrangement */
    for (i = 0; i < s_nil_count; i++)
        if (!memcmp(s_nil[i].key, key, sizeof key)) { il = s_nil[i].il; break; }
    if (!il) {
        D3D11_INPUT_ELEMENT_DESC e[16];
        for (a = 0; a < 16; a++) {
            e[a].SemanticName = "ATTR";
            e[a].SemanticIndex = (UINT)a;
            if (key[a] == 0xFFFFFFFFu) {
                e[a].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                e[a].InputSlot = 15;
                e[a].AlignedByteOffset = (UINT)(a * 16);
                e[a].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                e[a].InstanceDataStepRate = 1;
            } else {
                e[a].Format = (DXGI_FORMAT)(key[a] & 0xFF);
                e[a].InputSlot = key[a] >> 24;
                e[a].AlignedByteOffset = (key[a] >> 8) & 0xFFFF;
                e[a].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                e[a].InstanceDataStepRate = 0;
            }
        }
        if (FAILED(ID3D11Device_CreateInputLayout(s_dev, e, 16, ID3D10Blob_GetBufferPointer(sig),
                                                  ID3D10Blob_GetBufferSize(sig), &il)))
            return 0;
        if (s_nil_count < NIL_CACHE) {
            memcpy(s_nil[s_nil_count].key, key, sizeof key);
            s_nil[s_nil_count].il = il;
            s_nil_count++;
        }
    }
    if (!s_ndefaults) {
        float def[16][4];
        D3D11_BUFFER_DESC bd;
        D3D11_SUBRESOURCE_DATA sd;
        memset(def, 0, sizeof def);
        for (a = 0; a < 16; a++) def[a][3] = 1.0f;
        def[3][0] = def[3][1] = def[3][2] = 1.0f;          /* colours: white */
        def[4][0] = def[4][1] = def[4][2] = 1.0f;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof def;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sd.pSysMem = def; sd.SysMemPitch = sd.SysMemSlicePitch = 0;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, &sd, &s_ndefaults)))
            return 0;
    }
    ID3D11DeviceContext_IASetInputLayout(s_ctx, il);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, (UINT)nstream, bufs, strides, offs);
    {
        UINT st16 = 0, of16 = 0;
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 15, 1, &s_ndefaults, &st16, &of16);
    }
    return 1;
}

/* The native draws' own constant buffers: dynamic, rewritten with a discard
 * whenever they change (not the emulated renderer's shared DEFAULT buffers
 * updated in place). */
static ID3D11Buffer *s_ncb_ps, *s_ncb_vs;

static ID3D11Buffer *ncb_write(ID3D11Buffer **buf, const void *data, UINT bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (!*buf) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (bytes + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, buf)))
            return NULL;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)*buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return *buf;
    memcpy(m.pData, data, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)*buf, 0);
    return *buf;
}

/* ── frame interpolation ──────────────────────────────────────────────────
 *
 * The game's logic runs 60 steps a second; above that the display gets a
 * frame in between each pair: every draw into the main picture is recorded
 * as it happens (its pipeline state, the slices of the shared vertex/index
 * rings it used -- copied, the rings wrap -- and its constants), and at the
 * present the frame is drawn again into a picture of its own with each
 * draw's vertex-shader constants (its matrices, bones included) halfway
 * between the previous frame's matching draw and this one's. A draw matches
 * by its programs, vertex and index data and count, in order; one without a
 * match, or whose constants jumped (a camera cut, a teleport), is drawn as it
 * is. Draws into other targets (the reflection, shadows) are not replayed:
 * the replay samples what they hold. */
typedef struct {
    uint8_t kind;                  /* 0 a draw, 1 a clear */
    uint8_t has_dsv, instanced;
    /* the match key */
    uint32_t key_va, key_idx, key_n, key_prim;
    void *key_vs, *key_ps;
    /* the pipeline */
    ID3D11InputLayout *il;
    ID3D11Buffer *vb[4];
    UINT stride[4], offs[4];
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[4];
    ID3D11BlendState *bs;
    float bf[4];
    UINT bmask;
    ID3D11DepthStencilState *dss;
    UINT sref;
    ID3D11RasterizerState *rs;
    D3D11_VIEWPORT vp;
    UINT nidx, ib_first;
    INT base;
    uint32_t k_at;                 /* into the frame's constants */
    /* a clear */
    UINT cflags;
    float ccol[4], cz;
    UINT8 cs;
} NRec;

#define NREC_KFLOATS (VS_CONSTS * 4)
#define NREC_OK(x) (isfinite(x) && fabsf(x) < 1e10f)   /* (unused constants hold garbage) */
#define NREC_PFLOATS 128
#define NREC_VB_VERTS (256u * 1024u)       /* GpuIn each (256 bytes): 64 MB */
#define NREC_IB_IDX   (4u * 1024u * 1024u)

typedef struct {
    NRec *r;
    int n, cap;
    float *k;                       /* NREC_KFLOATS + NREC_PFLOATS a draw */
    int kn, kcap;
} NFrame;

static NFrame s_ncur, s_nprev;
static int s_interp_on;
static ID3D11RenderTargetView *s_interp_main;
static const float *s_rec_k;        /* this draw's vertex constants (the k[] draw_one uploads) */
static ID3D11Buffer *s_rvb, *s_rib, *s_icb_vs, *s_icb_ps;
static uint32_t s_rvb_pos, s_rib_pos;
static int s_rec_full;
static ID3D11Texture2D *s_itex, *s_iztex;
static ID3D11RenderTargetView *s_irtv;
static ID3D11DepthStencilView *s_idsv;
static ID3D11ShaderResourceView *s_isrv;
static UINT s_iw, s_ih;
static DXGI_FORMAT s_ifmt;

/* The port: interpolation on or off, and this frame's main picture. */
void nv2a_native_interp_setup(int on, ID3D11RenderTargetView *main_rtv)
{
    s_interp_on = on;
    s_interp_main = main_rtv;
}

static NRec *nrec_new(NFrame *f)
{
    if (f->n == f->cap) {
        int c = f->cap ? f->cap * 2 : 1024;
        NRec *r = (NRec *)realloc(f->r, (size_t)c * sizeof(NRec));
        if (!r)
            return NULL;
        f->r = r;
        f->cap = c;
    }
    memset(&f->r[f->n], 0, sizeof(NRec));
    return &f->r[f->n++];
}

static float *nrec_consts(NFrame *f, uint32_t *at)
{
    int need = NREC_KFLOATS + NREC_PFLOATS;
    if (f->kn + need > f->kcap) {
        int c = f->kcap ? f->kcap * 2 : need * 1024;
        float *k;
        while (c < f->kn + need)
            c *= 2;
        if (!(k = (float *)realloc(f->k, (size_t)c * sizeof(float))))
            return NULL;
        f->k = k;
        f->kcap = c;
    }
    *at = (uint32_t)f->kn;
    f->kn += need;
    return f->k + *at;
}

static int nrec_buffers(void)
{
    D3D11_BUFFER_DESC bd;
    if (s_rvb && s_rib)
        return 1;
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = NREC_VB_VERTS * (UINT)sizeof(GpuIn);
    if (!s_rvb && FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_rvb)))
        return 0;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = NREC_IB_IDX * 4;
    if (!s_rib && FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_rib)))
        return 0;
    return 1;
}

/* From draw_one, just before its draw: record it if it goes to the main picture. */
static void nrec_draw(const NativeDraw *d, ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv, const float *pcb,
                      int gpu_vb, UINT vb_first, UINT nv, UINT ib_first, UINT nidx, D3D11_PRIMITIVE_TOPOLOGY topo)
{
    NRec *r;
    float *k;
    uint32_t at;
    int i;
    D3D11_BOX box;
    UINT nvp = 1;
    if (!s_interp_on || rtv != s_interp_main || s_rec_full || !s_rec_k || !nrec_buffers())
        return;
    if ((!gpu_vb && s_rvb_pos + nv > NREC_VB_VERTS) || s_rib_pos + nidx > NREC_IB_IDX) {
        s_rec_full = 1;                               /* (a frame too big: shown without its in-between) */
        return;
    }
    if (!(r = nrec_new(&s_ncur)) || !(k = nrec_consts(&s_ncur, &at))) {
        s_rec_full = 1;
        return;
    }
    r->kind = 0;
    r->key_va = d->attr[0].va;
    r->key_idx = d->index_va;
    r->key_n = d->count;
    r->key_prim = d->prim;
    ID3D11DeviceContext_IAGetInputLayout(s_ctx, &r->il);
    ID3D11DeviceContext_IAGetVertexBuffers(s_ctx, 0, 4, r->vb, r->stride, r->offs);
    r->topo = topo;
    ID3D11DeviceContext_VSGetShader(s_ctx, &r->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(s_ctx, &r->ps, NULL, NULL);
    r->key_vs = r->vs;
    r->key_ps = r->ps;
    ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 8, r->srv);
    ID3D11DeviceContext_PSGetSamplers(s_ctx, 0, 4, r->smp);
    ID3D11DeviceContext_OMGetBlendState(s_ctx, &r->bs, r->bf, &r->bmask);
    ID3D11DeviceContext_OMGetDepthStencilState(s_ctx, &r->dss, &r->sref);
    ID3D11DeviceContext_RSGetState(s_ctx, &r->rs);
    ID3D11DeviceContext_RSGetViewports(s_ctx, &nvp, &r->vp);
    r->has_dsv = dsv != NULL;
    r->nidx = nidx;
    r->instanced = gpu_vb;
    /* the ring slices, copied (the rings wrap within a frame) */
    if (!gpu_vb) {
        box.left = vb_first * (UINT)sizeof(GpuIn);
        box.right = (vb_first + nv) * (UINT)sizeof(GpuIn);
        box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_rvb, 0, s_rvb_pos * (UINT)sizeof(GpuIn), 0, 0,
                                                  (ID3D11Resource *)s_vs_vb, 0, &box);
        if (r->vb[0])
            ID3D11Buffer_Release(r->vb[0]);
        r->vb[0] = s_rvb;
        ID3D11Buffer_AddRef(s_rvb);
        r->offs[0] = 0;
        r->base = (INT)s_rvb_pos;
        s_rvb_pos += nv;
    }
    box.left = ib_first * 4;
    box.right = (ib_first + nidx) * 4;
    box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_rib, 0, s_rib_pos * 4, 0, 0, (ID3D11Resource *)s_ib, 0, &box);
    r->ib_first = s_rib_pos;
    s_rib_pos += nidx;
    memcpy(k, s_rec_k, NREC_KFLOATS * sizeof(float));
    memcpy(k + NREC_KFLOATS, pcb, NREC_PFLOATS * sizeof(float));
    r->k_at = at;
    (void)i;
}

/* The port's clears of the main picture. */
void nv2a_native_interp_clear(ID3D11RenderTargetView *rtv, UINT flags, const float c[4], float z, UINT8 stencil)
{
    NRec *r;
    if (!s_interp_on || rtv != s_interp_main || s_rec_full || !(r = nrec_new(&s_ncur)))
        return;
    r->kind = 1;
    r->cflags = flags;
    memcpy(r->ccol, c, sizeof r->ccol);
    r->cz = z;
    r->cs = stencil;
}

static void nrec_release(NFrame *f)
{
    int i, j;
    for (i = 0; i < f->n; i++) {
        NRec *r = &f->r[i];
        if (r->kind)
            continue;
        if (r->il) ID3D11InputLayout_Release(r->il);
        for (j = 0; j < 4; j++) if (r->vb[j]) ID3D11Buffer_Release(r->vb[j]);
        if (r->vs) ID3D11VertexShader_Release(r->vs);
        if (r->ps) ID3D11PixelShader_Release(r->ps);
        for (j = 0; j < 8; j++) if (r->srv[j]) ID3D11ShaderResourceView_Release(r->srv[j]);
        for (j = 0; j < 4; j++) if (r->smp[j]) ID3D11SamplerState_Release(r->smp[j]);
        if (r->bs) ID3D11BlendState_Release(r->bs);
        if (r->dss) ID3D11DepthStencilState_Release(r->dss);
        if (r->rs) ID3D11RasterizerState_Release(r->rs);
    }
}

/* After the present: this frame becomes the previous one (keys and constants). */
void nv2a_native_interp_frame_end(void)
{
    NFrame t;
    nrec_release(&s_nprev);
    /* the previous frame keeps only what matching needs: its COM objects go */
    t = s_nprev;
    s_nprev = s_ncur;
    s_ncur = t;
    s_ncur.n = 0;
    s_ncur.kn = 0;
    {
        int i;
        for (i = 0; i < s_nprev.n; i++) {
            NRec *r = &s_nprev.r[i];
            if (r->kind)
                continue;
            /* (the keys are compared as values from here on) */
            if (r->il) { ID3D11InputLayout_Release(r->il); r->il = NULL; }
            { int j; for (j = 0; j < 4; j++) if (r->vb[j]) { ID3D11Buffer_Release(r->vb[j]); r->vb[j] = NULL; } }
            if (r->vs) { ID3D11VertexShader_Release(r->vs); r->vs = NULL; }
            if (r->ps) { ID3D11PixelShader_Release(r->ps); r->ps = NULL; }
            { int j; for (j = 0; j < 8; j++) if (r->srv[j]) { ID3D11ShaderResourceView_Release(r->srv[j]); r->srv[j] = NULL; } }
            { int j; for (j = 0; j < 4; j++) if (r->smp[j]) { ID3D11SamplerState_Release(r->smp[j]); r->smp[j] = NULL; } }
            if (r->bs) { ID3D11BlendState_Release(r->bs); r->bs = NULL; }
            if (r->dss) { ID3D11DepthStencilState_Release(r->dss); r->dss = NULL; }
            if (r->rs) { ID3D11RasterizerState_Release(r->rs); r->rs = NULL; }
        }
    }
    s_rvb_pos = 0;
    s_rib_pos = 0;
    s_rec_full = 0;
}

static int interp_targets(ID3D11Texture2D *like)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(like, &td);
    if (s_itex && s_iw == td.Width && s_ih == td.Height && s_ifmt == td.Format)
        return 1;
    if (s_irtv) { ID3D11RenderTargetView_Release(s_irtv); s_irtv = NULL; }
    if (s_isrv) { ID3D11ShaderResourceView_Release(s_isrv); s_isrv = NULL; }
    if (s_idsv) { ID3D11DepthStencilView_Release(s_idsv); s_idsv = NULL; }
    if (s_itex) { ID3D11Texture2D_Release(s_itex); s_itex = NULL; }
    if (s_iztex) { ID3D11Texture2D_Release(s_iztex); s_iztex = NULL; }
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_itex)))
        return 0;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_itex, NULL, &s_irtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_itex, NULL, &s_isrv);
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_iztex)))
        ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)s_iztex, NULL, &s_idsv);
    s_iw = td.Width;
    s_ih = td.Height;
    s_ifmt = td.Format;
    return s_irtv && s_isrv;
}

/* key equality */
static int nrec_same(const NRec *a, const NRec *b)
{
    return a->kind == 0 && b->kind == 0 && a->key_va == b->key_va && a->key_idx == b->key_idx && a->key_n == b->key_n
           && a->key_prim == b->key_prim && a->key_vs == b->key_vs && a->key_ps == b->key_ps;
}

/* The frame in between (t: 0 the previous frame, 1 this one), drawn into a
 * picture of the main one's size; 0 when there is nothing to draw. */
int nv2a_native_interp_render(float t, ID3D11Texture2D *main_tex, ID3D11Texture2D **out_tex, ID3D11ShaderResourceView **out_srv)
{
    int i, blended = 0, drawn = 0, cuts = 0, *used;
    float *kb;
    if (!s_interp_on || s_rec_full || !s_ncur.n || !s_nprev.n || !main_tex || !interp_targets(main_tex))
        return 0;
    if (!(kb = (float *)malloc((NREC_KFLOATS) * sizeof(float))))
        return 0;
    if (!(used = (int *)calloc((size_t)s_nprev.n, sizeof(int)))) {
        free(kb);
        return 0;
    }
    if (!ncb_write(&s_icb_vs, kb, NREC_KFLOATS * sizeof(float)) || !ncb_write(&s_icb_ps, kb, NREC_PFLOATS * sizeof(float))) {
        free(kb);
        free(used);
        return 0;
    }
    {
        /* the picture starts as this frame's does (its first clear follows) */
        float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_irtv, black);
        if (s_idsv)
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s_idsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    {
        int pj = 0;                                      /* (matches are searched forward from the last) */
        for (i = 0; i < s_ncur.n; i++) {
            NRec *r = &s_ncur.r[i];
            const float *kc, *use;
            int j, m = -1;
            if (r->kind == 1) {
                if (r->cflags & 0xF0)
                    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_irtv, r->ccol);
                if ((r->cflags & 3) && s_idsv)
                    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s_idsv,
                        ((r->cflags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((r->cflags & 2) ? D3D11_CLEAR_STENCIL : 0), r->cz, r->cs);
                continue;
            }
            kc = s_ncur.k + r->k_at;
            use = kc;
            for (j = pj; j < s_nprev.n; j++)
                if (!used[j] && nrec_same(r, &s_nprev.r[j])) { m = j; break; }
            if (m < 0)
                for (j = 0; j < pj && j < s_nprev.n; j++)
                    if (!used[j] && nrec_same(r, &s_nprev.r[j])) { m = j; break; }
            if (m >= 0) {
                const float *kp = s_nprev.k + s_nprev.r[m].k_at;
                double diff = 0, mag = 0;
                used[m] = 1;
                pj = m + 1;
                for (j = 0; j < NREC_KFLOATS; j++) {
                    /* (unused slots can hold NaN or infinity: left out) */
                    if (!NREC_OK(kc[j]) || !NREC_OK(kp[j]))
                        continue;
                    diff += fabs((double)kc[j] - (double)kp[j]);
                    mag += fabs((double)kc[j]);
                }
                if (diff <= 0.25 * mag + 1e-6) {
                    for (j = 0; j < NREC_KFLOATS; j++)
                        kb[j] = NREC_OK(kc[j]) && NREC_OK(kp[j]) ? kp[j] + (kc[j] - kp[j]) * t : kc[j];
                    use = kb;
                    blended++;
                } else {
                    cuts++;
                    if (nenv("BUFFY_INTERP_LOG")) {
                        static int shown;
                        if (shown++ < 40) {
                            int worst = 0;
                            double wd = 0;
                            for (j = 0; j < NREC_KFLOATS; j++)
                                if (fabs((double)kc[j] - (double)kp[j]) > wd) { wd = fabs((double)kc[j] - (double)kp[j]); worst = j; }
                            fprintf(stderr, "[INTERP] jump: draw %d va %08X n %u ratio %.2f, most at c%d.%d %g -> %g\n", i, r->key_va,
                                    r->key_n, diff / (mag + 1e-9), worst / 4, worst % 4, kp[worst], kc[worst]);
                        }
                    }
                }
            }
            ncb_write(&s_icb_vs, use, NREC_KFLOATS * sizeof(float));
            ncb_write(&s_icb_ps, kc + NREC_KFLOATS, NREC_PFLOATS * sizeof(float));
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_irtv, r->has_dsv ? s_idsv : NULL);
            ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &r->vp);
            ID3D11DeviceContext_RSSetState(s_ctx, r->rs);
            ID3D11DeviceContext_OMSetBlendState(s_ctx, r->bs, r->bf, r->bmask);
            ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, r->dss, r->sref);
            ID3D11DeviceContext_IASetInputLayout(s_ctx, r->il);
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 4, r->vb, r->stride, r->offs);
            ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_rib, DXGI_FORMAT_R32_UINT, 0);
            ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, r->topo);
            ID3D11DeviceContext_VSSetShader(s_ctx, r->vs, NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 1, 1, &s_icb_vs);
            ID3D11DeviceContext_PSSetShader(s_ctx, r->ps, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_icb_ps);
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, r->srv);
            ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, r->smp);
            if (r->instanced)
                ID3D11DeviceContext_DrawIndexedInstanced(s_ctx, r->nidx, 1, r->ib_first, 0, 0);
            else
                ID3D11DeviceContext_DrawIndexed(s_ctx, r->nidx, r->ib_first, r->base);
            drawn++;
        }
    }
    {
        ID3D11ShaderResourceView *none[8] = { 0 };
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, none);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    }
    free(kb);
    free(used);
    if (nenv("BUFFY_INTERP_LOG")) {
        static DWORD last;
        static int frames;
        frames++;
        if (GetTickCount() - last > 2000) {
            fprintf(stderr, "[INTERP] %d draws: %d blended, %d jumped (drawn as they are), %d unmatched; %.1f game frames a second\n",
                    drawn, blended, cuts, drawn - blended - cuts, last ? frames * 1000.0 / (GetTickCount() - last) : 0.0);
            last = GetTickCount();
            frames = 0;
        }
    }
    if (cuts > drawn / 2)
        return 0;                                        /* a cut: no frame in between */
    *out_tex = s_itex;
    *out_srv = s_isrv;
    return drawn > 0;
}

static int s_ntrace_gpu_vb;
static uint32_t s_ntrace_px[2];
/* non-stalling probes: the page pixel just before / after draw BUFFY_FLASHTRACE_AT, per frame */
static ID3D11Texture2D *s_nprobe[8][2];
static uint32_t s_nprobe_frame[8];

static void nprobe_copy(ID3D11RenderTargetView *rtv, ID3D11Texture2D **dst)
{
    ID3D11Resource *res = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_BOX b;
    if (!rtv) return;
    ID3D11RenderTargetView_GetResource(rtv, &res);
    if (!res) return;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    b.left = (UINT)(0.25f * (float)td.Width); b.top = (UINT)(0.45f * (float)td.Height); b.front = 0;
    b.right = b.left + 1; b.bottom = b.top + 1; b.back = 1;
    if (!*dst) {
        td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ID3D11Device_CreateTexture2D(s_dev, &td, NULL, dst);
    }
    if (*dst)
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)*dst, 0, 0, 0, 0, res, 0, &b);
    ID3D11Resource_Release(res);
}

static uint32_t nprobe_read(ID3D11Texture2D *t)
{
    D3D11_MAPPED_SUBRESOURCE mp;
    uint32_t v = 0xDEADBEEF;
    if (t && SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)t, 0, D3D11_MAP_READ, 0, &mp))) {
        v = *(const uint32_t *)mp.pData;
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)t, 0);
    }
    return v;
}
static uint32_t s_ntrace_vb[16];               /* the last draw read the game's buffers on the GPU */

/* Draw into `rtv` / `dsv` (pw x ph pixels showing the game's 640x480).
 * 0 if it could not (the caller counts). */
/* debug (BUFFY_FLASHTRACE): one pixel of the target, read back now */
static uint32_t ntrace_pixel(ID3D11RenderTargetView *rtv, float u, float v)
{
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *st = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_BOX b;
    uint32_t out = 0xDEADBEEF;
    if (!rtv) return out;
    ID3D11RenderTargetView_GetResource(rtv, &res);
    if (!res) return out;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    b.left = (UINT)(u * (float)td.Width); b.top = (UINT)(v * (float)td.Height); b.front = 0;
    b.right = b.left + 1; b.bottom = b.top + 1; b.back = 1;
    td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) {
        D3D11_MAPPED_SUBRESOURCE mp;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)st, 0, 0, 0, 0, res, 0, &b);
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
            out = *(const uint32_t *)mp.pData;
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11Resource_Release(res);
    return out;
}

static int nv2a_native_draw_one(const NativeDraw *d, ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv,
                                uint32_t pw, uint32_t ph, uint32_t sw, uint32_t sh)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t vmin = 0, vmax, nidx = 0, i, n = d->count, bound = 0, cube = 0;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[4];
    UINT stride = sizeof(GpuIn), offs = 0;
    D3D11_VIEWPORT vp;
    int uses_viewport = 0, st;
    float cb[128];
    const uint16_t *idx = d->index_va ? (const uint16_t *)(mem + d->index_va) : NULL;
    UINT vb_first = 0, ib_first = 0;
    int gpu_vb = 0, try_gpu = 0;
    uint32_t gpu_mask = 0;

    if (!n || !rtv) return 0;
    if (!s_gin && !(s_gin = (GpuIn *)malloc(sizeof(GpuIn) * GPU_MAX_VERTS))) return 0;

    /* vertices referenced */
    if (idx) {
        vmin = 0xFFFFFFFFu; vmax = 0;
        for (i = 0; i < n; i++) {
            if (idx[i] < vmin) vmin = idx[i];
            if (idx[i] > vmax) vmax = idx[i];
        }
    } else {
        vmax = n - 1;
    }
    if (vmax - vmin + 1 > GPU_MAX_VERTS) { s_nstat[2]++; return 0; }

    /* the vertex shader */
    {
        uint32_t mask = 0;
        try_gpu = idx && !nenv("BUFFY_NATIVE_CPUVB") && nvb_supported(d, &mask) && !(d->passthrough && mask);
        gpu_mask = try_gpu ? mask : 0;
    }
    vs = d->passthrough ? nvs_passthrough() : nvs_program(d->vp_start, &uses_viewport, gpu_mask);
    if (!vs || !s_vs_layout) { s_nstat[1]++; return 0; }

    /* the inputs: the game's own buffers on the GPU (indexed draws), else
     * unpacked here (float4 x 16 a vertex) */
    gpu_vb = try_gpu && nvb_bind(d, vmax, gpu_mask);
    s_ntrace_gpu_vb = gpu_vb;
    if (try_gpu && !gpu_vb && gpu_mask && !d->passthrough)
        vs = nvs_program(d->vp_start, &uses_viewport, 0);     /* the plain variant after all */
    s_nvb_stat[gpu_vb ? 0 : 1]++;
    if (!gpu_vb)
    for (i = vmin; i <= vmax; i++) {
        GpuIn *g = &s_gin[i - vmin];
        int a;
        for (a = 0; a < 16; a++)
            if (!ndraw_fetch(&d->attr[a], i, g->a[a])) {
                g->a[a][0] = g->a[a][1] = g->a[a][2] = 0.0f;
                g->a[a][3] = 1.0f;
                if (a == 3 || a == 4) g->a[a][0] = g->a[a][1] = g->a[a][2] = 1.0f;   /* colours: white */
            }
    }
    /* appended to a ring (discarded only when it wraps): a discard a draw
     * renamed the whole 16 MB buffer every time */
    if (!gpu_vb) {
        uint32_t nv = vmax - vmin + 1;
        D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (s_nvb_pos + nv > GPU_MAX_VERTS || nenv("BUFFY_NATIVE_DISCARD")) { s_nvb_pos = 0; how = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vs_vb, 0, how, 0, &map)))
            return 0;
        memcpy((GpuIn *)map.pData + s_nvb_pos, s_gin, sizeof(GpuIn) * nv);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vs_vb, 0);
        vb_first = s_nvb_pos;
        s_nvb_pos += nv;
    }

    /* the indices, as a list */
#define V(k) (idx ? (uint32_t)idx[k] - (gpu_vb ? 0u : vmin) : (uint32_t)(k))
#define PUT(v) do { if (nidx < GPU_MAX_IDX) s_gi[nidx++] = (v); } while (0)
    switch (d->prim) {
    case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; for (i = 0; i < n; i++) PUT(V(i)); break;
    case 2: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; for (i = 0; i + 1 < n; i += 2) { PUT(V(i)); PUT(V(i + 1)); } break;
    case 3: case 4:
        topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        for (i = 0; i + 1 < n; i++) { PUT(V(i)); PUT(V(i + 1)); }
        if (d->prim == 3 && n > 2) { PUT(V(n - 1)); PUT(V(0)); }
        break;
    case 5: for (i = 0; i + 2 < n; i += 3) { PUT(V(i)); PUT(V(i + 1)); PUT(V(i + 2)); } break;
    case 6:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) { PUT(V(i + 1)); PUT(V(i)); PUT(V(i + 2)); }
            else       { PUT(V(i)); PUT(V(i + 1)); PUT(V(i + 2)); }
        }
        break;
    case 7: case 10: for (i = 1; i + 1 < n; i++) { PUT(V(0)); PUT(V(i)); PUT(V(i + 1)); } break;
    case 8:
        for (i = 0; i + 3 < n; i += 4) {
            PUT(V(i)); PUT(V(i + 1)); PUT(V(i + 2));
            PUT(V(i)); PUT(V(i + 2)); PUT(V(i + 3));
        }
        break;
    case 9:
        for (i = 0; i + 3 < n; i += 2) {
            PUT(V(i)); PUT(V(i + 1)); PUT(V(i + 3));
            PUT(V(i)); PUT(V(i + 3)); PUT(V(i + 2));
        }
        break;
    default: return 0;
    }
#undef V
#undef PUT
    if (!nidx) return 0;
    {
        D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (s_nib_pos + nidx > GPU_MAX_IDX || nenv("BUFFY_NATIVE_DISCARD")) { s_nib_pos = 0; how = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ib, 0, how, 0, &map)))
            return 0;
        memcpy((uint32_t *)map.pData + s_nib_pos, s_gi, nidx * 4);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);
        ib_first = s_nib_pos;
        s_nib_pos += nidx;
    }

    /* textures */
    memset(srv, 0, sizeof srv);
    memset(cb, 0, sizeof cb);
    s_tex_src_saved = s_tex_src;
    s_tex_src = s_ntex_regs;
    for (st = 0; st < 4; st++) {
        int mips = 0, cub = 0;
        float norm[2] = { 1.0f, 1.0f };
        ID3D11ShaderResourceView *v;
        ntex_regs(st, &d->tex[st], d->tss + st * 32);
        if (d->tex[st].srv) {
            v = d->tex[st].srv;                    /* rendered natively: no texels in memory */
            mips = 0;
            if (!(*(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + d->tex[st].res + 0xC) & 0xF0000000u)
                    && *(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + d->tex[st].res + 0x10)) {
                /* linear: NV2A addresses it in texels */
                uint32_t size = *(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + d->tex[st].res + 0x10);
                norm[0] = 1.0f / (float)((size & 0xFFF) + 1);
                norm[1] = 1.0f / (float)(((size >> 12) & 0xFFF) + 1);
            }
        } else {
            v = (d->tex[st].res || d->tex[st].data) ? gpu_texture(st, norm, &mips, &cub) : NULL;
        }
        if (v && cub) { srv[4 + st] = v; cube |= 1u << st; }
        else if (v) srv[st] = v;
        if (v) bound |= 1u << st;
        smp[st] = v ? tx_sampler(st, mips) : s_samp[0];
        cb[8 + st * 4] = norm[0];
        cb[9 + st * 4] = norm[1];
    }
    s_tex_src = s_tex_src_saved;

    /* the pixel shader */
    if (d->pixel_shader) {
        PsKey k;
        memset(&k, 0, sizeof k);
        for (i = 0; i < 8; i++) {
            k.cicw[i] = d->rs[NRS_PS_RGB_IN + i];
            k.cocw[i] = d->rs[NRS_PS_RGB_OUT + i];
            k.aicw[i] = d->rs[NRS_PS_ALPHA_IN + i];
            k.aocw[i] = d->rs[NRS_PS_ALPHA_OUT + i];
            gpu_argb(d->rs[NRS_PS_CONST0 + i], &cb[24 + i * 4]);
            gpu_argb(d->rs[NRS_PS_CONST1 + i], &cb[56 + i * 4]);
        }
        k.control = d->rs[NRS_PS_COMBINERS];
        k.fin0 = d->rs[NRS_PS_FINAL0];
        k.fin1 = d->rs[NRS_PS_FINAL1];
        k.stage_prog = d->rs[NRS_PSTEXTUREMODES] & 0xFFFFF;
        k.tex_bound = bound;
        k.tex_cube = cube;
        gpu_argb(d->rs[NRS_PS_FINALCONST0], &cb[88]);
        gpu_argb(d->rs[NRS_PS_FINALCONST1], &cb[92]);
        ps = NULL;
        {
            /* the shared combiner cache, by its key */
            static char src[32768];
            ID3DBlob *blob = NULL;
            for (i = 0; i < (uint32_t)s_ps_count; i++)
                if (!memcmp(&s_ps_cache[i].key, &k, sizeof k)) { ps = s_ps_cache[i].ps; break; }
            if (i == (uint32_t)s_ps_count && psh_generate(&k, src, sizeof src)
                    && SUCCEEDED(gpu_compile(src, "ps", "ps_4_0", &blob)) && blob) {
                ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                               ID3D10Blob_GetBufferSize(blob), NULL, &ps);
                ID3D10Blob_Release(blob);
                if (s_ps_count < PS_CACHE) {
                    s_ps_cache[s_ps_count].key = k;
                    s_ps_cache[s_ps_count].ps = ps;
                    s_ps_count++;
                }
            }
        }
    } else {
        NFfKey k;
        memset(&k, 0, sizeof k);
        for (st = 0; st < 4; st++) {
            const uint32_t *t = d->tss + st * 32;
            k.op[st][0] = t[NTS_COLOROP]; k.op[st][1] = t[NTS_COLORARG1]; k.op[st][2] = t[NTS_COLORARG2];
            k.op[st][3] = t[NTS_ALPHAOP]; k.op[st][4] = t[NTS_ALPHAARG1]; k.op[st][5] = t[NTS_ALPHAARG2];
            k.arg0[st][0] = t[NTS_COLORARG0]; k.arg0[st][1] = t[NTS_ALPHAARG0];
            if (t[NTS_COLOROP] <= 1) break;
            /* a later stage whose texture is missing but which takes one
             * ends the cascade (as the library's combiner setup has it) */
            if (st > 0 && !(bound & (1u << st))
                    && ((t[NTS_COLORARG1] & 0xF) == 2 || (t[NTS_COLORARG2] & 0xF) == 2 || (t[NTS_COLORARG0] & 0xF) == 2)) {
                k.op[st][0] = 1;
                break;
            }
        }
        k.bound = bound;
        gpu_argb(d->rs[NRS_TEXTUREFACTOR], &cb[24]);
        ps = nff_get(&k);
    }
    if (!ps) { s_nstat[3]++; return 0; }

    if (nenv("BUFFY_NATIVE_TEXLOG")) {
        static int shown, seen;
        if (++seen >= atoi(nenv("BUFFY_NATIVE_TEXLOG")) && (seen % 97) == 0 && shown++ < 24) {
            const uint8_t *m = (const uint8_t *)xbox_GetMemoryOffset();
            fprintf(stderr, "  [NATIVE] tex: ps %d bound %X | t0 %08X fmt %08X data %08X | op %u %X %X / %u %X %X | ctl %08X modes %08X\n",
                    d->pixel_shader, bound, d->tex[0].res,
                    d->tex[0].res ? *(const uint32_t *)(m + d->tex[0].res + 0xC) : 0,
                    d->tex[0].res ? *(const uint32_t *)(m + d->tex[0].res + 4) : 0,
                    d->tss[NTS_COLOROP], d->tss[NTS_COLORARG1], d->tss[NTS_COLORARG2],
                    d->tss[NTS_ALPHAOP], d->tss[NTS_ALPHAARG1], d->tss[NTS_ALPHAARG2],
                    d->rs[NRS_PS_COMBINERS], d->rs[NRS_PSTEXTUREMODES]);
            fprintf(stderr, "      theirs: t0 off %08X fmt %08X  t1 off %08X  t2 off %08X | ours t1 %08X t2 %08X t3 %08X\n",
                    s_tex_reg[(0x1B00 - NV_TEX_FIRST) / 4], s_tex_reg[(0x1B04 - NV_TEX_FIRST) / 4],
                    s_tex_reg[(0x1B40 - NV_TEX_FIRST) / 4], s_tex_reg[(0x1B80 - NV_TEX_FIRST) / 4],
                    d->tex[1].res, d->tex[2].res, d->tex[3].res);
        }
    }
    if (nenv("BUFFY_NATIVE_PSLOG")) {
        /* debug: each new (pixel path, textures, stage ops) combination once */
        static uint32_t seen[256][4];
        static int nseen;
        uint32_t key[4];
        int j;
        key[0] = (uint32_t)d->pixel_shader | bound << 4 | (d->index_va ? 0x100u : 0u);
        key[1] = d->tss[NTS_COLOROP] | d->tss[NTS_ALPHAOP] << 8 | d->tss[32 + NTS_COLOROP] << 16 | d->tss[32 + NTS_ALPHAOP] << 24;
        key[2] = d->pixel_shader ? d->rs[NRS_PS_COMBINERS] : d->tss[NTS_COLORARG1] | d->tss[NTS_COLORARG2] << 8 | d->tss[32 + NTS_COLORARG1] << 16 | d->tss[32 + NTS_COLORARG2] << 24;
        key[3] = d->pixel_shader ? d->rs[NRS_PSTEXTUREMODES] : d->tss[64 + NTS_COLOROP];
        for (j = 0; j < nseen && memcmp(seen[j], key, 16); j++) ;
        if (j == nseen && nseen < 256) {
            memcpy(seen[nseen++], key, 16);
            fprintf(stderr, "  [NATIVE] combo %d: ps %d bound %X %s | ops %08X args %08X extra %08X\n", nseen,
                    d->pixel_shader, bound, d->index_va ? "indexed" : "up", key[1], key[2], key[3]);
        }
    }
    /* pixel constants: alpha test, fog colour */
    cb[0] = (float)(d->rs[NRS_ALPHAREF] & 0xFF);
    cb[1] = (float)(d->rs[NRS_ALPHAFUNC] & 7);
    cb[2] = d->rs[NRS_ALPHATEST] ? 1.0f : 0.0f;
    gpu_argb(d->rs[NRS_FOGCOLOR], &cb[4]);
    {
        static float last_cb[128];
        static int have;
        if (!have || memcmp(last_cb, cb, sizeof cb)) {
            memcpy(last_cb, cb, sizeof cb);
            have = 1;
            ncb_write(&s_ncb_ps, cb, sizeof cb);
        }
    }

    /* vertex constants and parameters (see vsh_generate's epilogue) */
    {
        static float k[VS_CONSTS][4];
        memcpy(k, s_nconst, sizeof s_nconst);
        memset(k[192], 0, sizeof(float) * 16);
        k[192][0] = uses_viewport ? 1.0f : 0.0f;
        {
            /* W-buffering (D3DZB_USEW, which the title's 3D uses): depth is W
             * mapped through the clip range, as on the NV2A (vsh_generate's
             * epilogue). The range is D3D's, set once; it's read from the
             * pushbuffer state, which runs a little behind the draws, so it is
             * kept from when that state last showed W mode (whose range starts
             * above 0; Z mode's is 0..2^24). */
            static float wmin, wmax;
            float cmin, cmax;
            memcpy(&cmin, &KEL(0x0394), 4);
            memcpy(&cmax, &KEL(0x0398), 4);
            if ((KEL(0x0290) & 0x10000) && cmin > 0.0f && cmax > cmin)
                wmin = cmin, wmax = cmax;
            if (d->rs[NRS_ZENABLE] == 2 && wmax > wmin && !nenv("BUFFY_NATIVE_NOW")) {
                k[192][1] = 1.0f;
                k[192][2] = wmin;
                k[192][3] = wmax;
            }
        }
        k[193][0] = (float)sw;
        k[193][1] = (float)sh;
        /* fog: D3D's table mode as the NV2A fog unit's, on the program's oFog
         * (fogf in vsh_generate: exp 0x800, exp2 0x801, else linear) */
        {
            uint32_t mode = d->rs[NRS_FOGTABLEMODE];
            float start, end, dens;
            memcpy(&start, &d->rs[NRS_FOGSTART], 4);
            memcpy(&end, &d->rs[NRS_FOGEND], 4);
            memcpy(&dens, &d->rs[NRS_FOGDENSITY], 4);
            k[193][2] = d->rs[NRS_FOGENABLE] ? 1.0f : 0.0f;
            if (mode == 0) {                                  /* vertex fog: oFog is the factor */
                k[193][3] = 9729.0f; k[194][0] = 1.0f; k[194][1] = 1.0f;
            } else if (mode == 1) {                                  /* EXP */
                k[193][3] = 2048.0f; k[194][0] = 1.0f; k[194][1] = -dens * 1.442695f / 16.0f;
            } else if (mode == 2) {                           /* EXP2 */
                k[193][3] = 2049.0f; k[194][0] = 1.0f; k[194][1] = dens * sqrtf(1.442695f / 32.0f);
            } else {                                          /* LINEAR */
                float r = end - start;
                if (r == 0.0f) r = 1.0f;
                k[193][3] = 9729.0f; k[194][0] = 1.0f + end / r; k[194][1] = -1.0f / r;
            }
            if (nenv("BUFFY_NATIVE_FOGLOG")) {
                static int shown;
                if ((d->rs[NRS_FOGENABLE] || KEL(0x02A4)) && shown++ < 6)
                    fprintf(stderr, "  [NATIVE] fog: enable %u mode %u start %g end %g density %g color %08X | theirs mode %X p %g %g on %u\n",
                            d->rs[NRS_FOGENABLE], mode, start, end, dens, d->rs[NRS_FOGCOLOR],
                            KEL(0x029C), *(const float *)&KEL(0x09C0), *(const float *)&KEL(0x09C4), KEL(0x02A4));
            }
        }
        s_rec_k = &k[0][0];                          /* (for frame interpolation's record) */
        {
            static float last_k[VS_CONSTS][4];
            static int have;
            if (!have || memcmp(last_k, k, sizeof k)) {
                memcpy(last_k, k, sizeof k);
                have = 1;
                ncb_write(&s_ncb_vs, k, sizeof k);
            }
        }
    }

    nstate_apply(d->rs);
    if (nenv("BUFFY_NATIVE_WLOG")) {
        /* distinct (draw kind, zenable, their W bit, clip range) seen */
        static uint32_t seen[32][4];
        static int n;
        uint32_t key[4];
        int i;
        key[0] = (d->index_va ? 1u : 0u) | d->rs[NRS_ZENABLE] << 4 | ((KEL(0x0290) >> 16) & 1) << 8 | (uses_viewport ? 1u : 0u) << 12;
        key[1] = KEL(0x0394); key[2] = KEL(0x0398); key[3] = 0;
        for (i = 0; i < n && memcmp(seen[i], key, 12); i++) ;
        if (i == n && n < 32) {
            float cmin, cmax;
            memcpy(seen[n++], key, sizeof key);
            memcpy(&cmin, &key[1], 4); memcpy(&cmax, &key[2], 4);
            fprintf(stderr, "  [NATIVE] w: indexed %u zenable %u theirs W %u viewport %u clip %g..%g c58z %g c59z %g frame %u\n",
                    key[0] & 1, d->rs[NRS_ZENABLE], (key[0] >> 8) & 1, (key[0] >> 12) & 1, cmin, cmax,
                    s_nconst[58][2], s_nconst[59][2], s_frame_no);
        }
    }
    if (nenv("BUFFY_NATIVE_ZLOG")) {
        static int shown, seen;
        if (!d->index_va && d->rs[NRS_ZENABLE] && ++seen >= atoi(nenv("BUFFY_NATIVE_ZLOG")) && shown++ < 8) {
            float cmin, cmax;
            memcpy(&cmin, &KEL(0x0394), 4); memcpy(&cmax, &KEL(0x0398), 4);
            fprintf(stderr, "  [NATIVE] z: theirs control %08X clip %g..%g test %u mask %u func %X | ours zenable %u zwrite %u zfunc %X c58z %g c59z %g\n",
                    KEL(0x0290), cmin, cmax, KEL(0x030C), KEL(0x035C), KEL(0x0354), d->rs[NRS_ZENABLE], d->rs[NRS_ZWRITE],
                    d->rs[NRS_ZFUNC], s_nconst[58][2], s_nconst[59][2]);
            fprintf(stderr, "      stencil en %u func %X ref %X mask %X wmask %X fail %X zfail %X pass %X | theirs en %u func %X ref %X\n",
                    d->rs[NRS_STENCILENABLE], d->rs[NRS_STENCILFUNC], d->rs[NRS_STENCILREF], d->rs[NRS_STENCILMASK],
                    d->rs[NRS_STENCILWRITEMASK], d->rs[NRS_STENCILFAIL], d->rs[NRS_STENCILZFAIL], d->rs[NRS_STENCILPASS],
                    KEL(0x032C), KEL(0x0364), KEL(0x0368));
        }
    }
    if (nenv("BUFFY_NATIVE_NODEPTH_UP") && !d->index_va) {
        static ID3D11DepthStencilState *nd;
        if (!nd) {
            D3D11_DEPTH_STENCIL_DESC dd;
            memset(&dd, 0, sizeof dd);
            dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            ID3D11Device_CreateDepthStencilState(s_dev, &dd, &nd);
        }
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, nd, 0);
    }
    if (nenv("BUFFY_NATIVE_FLAT")) {
        /* debug: every draw flat magenta, no depth -- is the geometry on screen? */
        static ID3D11PixelShader *flat;
        static ID3D11DepthStencilState *nodepth;
        if (!flat) {
            static char src[8192];
            ID3DBlob *blob = NULL;
            sprintf_s(src, sizeof src, "%sfloat4 ps(P i) : SV_Target { return float4(1, 0, 1, 1); }\n", s_psh_head);
            if (SUCCEEDED(gpu_compile(src, "ps", "ps_4_0", &blob)) && blob) {
                ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &flat);
                ID3D10Blob_Release(blob);
            }
        }
        if (!nodepth) {
            D3D11_DEPTH_STENCIL_DESC dd;
            memset(&dd, 0, sizeof dd);
            dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            ID3D11Device_CreateDepthStencilState(s_dev, &dd, &nodepth);
        }
        if (flat) ps = flat;
        if (atoi(nenv("BUFFY_NATIVE_FLAT")) == 2 && nvs_passthrough())
            vs = nvs_passthrough();                /* positions as screen pixels, whatever they are */
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, nodepth, 0);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    }
    if (nenv("BUFFY_NATIVE_TRACE")) {
        static int shown, seen;
        if ((!nenv("BUFFY_NATIVE_TRACE_UP") || !d->index_va) && ++seen >= atoi(nenv("BUFFY_NATIVE_TRACE")) && shown++ < 12) {
            fprintf(stderr, "  [NATIVE] uses_viewport %d prog len? start %u\n", uses_viewport, d->vp_start);
            GpuIn *g = &s_gin[0];
            fprintf(stderr, "  [NATIVE] draw prim %u n %u verts %u..%u vs %p pass %d start %u | v0 pos %g %g %g %g col %g %g %g %g t0 %g %g | c0 %g %g %g %g c58 %g %g %g c59 %g %g %g\n",
                    d->prim, n, vmin, vmax, (void *)vs, d->passthrough, d->vp_start,
                    g->a[0][0], g->a[0][1], g->a[0][2], g->a[0][3], g->a[3][0], g->a[3][1], g->a[3][2], g->a[3][3],
                    g->a[9][0], g->a[9][1], s_nconst[96][0], s_nconst[96][1], s_nconst[96][2], s_nconst[96][3],
                    s_nconst[58][0], s_nconst[58][1], s_nconst[58][2], s_nconst[59][0], s_nconst[59][1], s_nconst[59][2]);
        }
    }
    if (nenv("BUFFY_NATIVE_VERIFY")) {
        /* debug: our program memory and constants against the pushbuffer's
         * (the executor still reads it in native mode; it lags a little) */
        static int shown;
        if (shown < 20) {
            int dc = 0, dp = 0, first_c = -1, first_p = -1, j;
            for (j = 0; j < 192; j++)
                if (memcmp(s_nconst[j], s_vp_const[j], 16)) { if (first_c < 0) first_c = j; dc++; }
            for (j = 0; j < VP_SLOTS; j++)
                if (memcmp(s_nvp_code[j], s_vp_code[j], 16)) { if (first_p < 0) first_p = j; dp++; }
            if (dc && nenv("BUFFY_NATIVE_VERIFY")[0] == '2') {
                shown++;
                for (j = 0; j < 192; j++)
                    if (memcmp(s_nconst[j], s_vp_const[j], 16))
                        fprintf(stderr, "  [NATIVE] c%d ours %g %g %g %g theirs %g %g %g %g\n", j,
                                s_nconst[j][0], s_nconst[j][1], s_nconst[j][2], s_nconst[j][3],
                                s_vp_const[j][0], s_vp_const[j][1], s_vp_const[j][2], s_vp_const[j][3]);
            } else if (dc || dp) {
                shown++;
                fprintf(stderr, "  [NATIVE] verify: %d constants differ (first %d: ours %g %g %g %g theirs %g %g %g %g), %d program slots differ (first %d), start ours %u theirs %u\n",
                        dc, first_c, first_c >= 0 ? s_nconst[first_c][0] : 0.0f, first_c >= 0 ? s_nconst[first_c][1] : 0.0f,
                        first_c >= 0 ? s_nconst[first_c][2] : 0.0f, first_c >= 0 ? s_nconst[first_c][3] : 0.0f,
                        first_c >= 0 ? s_vp_const[first_c][0] : 0.0f, first_c >= 0 ? s_vp_const[first_c][1] : 0.0f,
                        first_c >= 0 ? s_vp_const[first_c][2] : 0.0f, first_c >= 0 ? s_vp_const[first_c][3] : 0.0f,
                        dp, first_p, d->vp_start, s_vp_start);
            }
        }
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, dsv);
    /* the title's viewport when the program applies one (c58 scale, c59
     * offset: the epilogue maps into it), else the whole surface */
    if (uses_viewport && s_nconst[58][0] != 0.0f) {
        float hx = fabsf(s_nconst[58][0]), hy = fabsf(s_nconst[58][1]);
        float sx = (float)pw / (float)sw, sy = (float)ph / (float)sh;
        vp.TopLeftX = (s_nconst[59][0] - hx) * sx;
        vp.TopLeftY = (s_nconst[59][1] - hy) * sy;
        vp.Width = 2.0f * hx * sx;
        vp.Height = 2.0f * hy * sy;
    } else {
        vp.TopLeftX = 0; vp.TopLeftY = 0;
        vp.Width = (float)pw; vp.Height = (float)ph;
    }
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    if (!gpu_vb) {
        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_vs_layout);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vs_vb, &stride, &offs);
    }
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, topo);
    ID3D11DeviceContext_VSSetShader(s_ctx, vs, NULL, 0);
    {
        ID3D11Buffer *vcb = s_ncb_vs ? s_ncb_vs : s_vs_cb;
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 1, 1, &vcb);
    }
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    {
        ID3D11Buffer *pcb = s_ncb_ps ? s_ncb_ps : s_cb;
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &pcb);
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    nrec_draw(d, rtv, dsv, cb, gpu_vb, vb_first, gpu_vb ? 0u : vmax - vmin + 1, ib_first, nidx, topo);
    if (gpu_vb)
        ID3D11DeviceContext_DrawIndexedInstanced(s_ctx, nidx, 1, ib_first, 0, 0);
    else
        ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, ib_first, (INT)vb_first);
    s_nstat[0]++;
    return 1;
}

/* Counts since the last call: drawn, no vertex shader, too big, no pixel shader. */
void nv2a_native_stats(unsigned long long out[4])
{
    memcpy(out, s_nstat, sizeof(unsigned long long) * 4);
    memset(s_nstat, 0, sizeof s_nstat);
    if (nenv("BUFFY_NATIVE_LOG")) {
        fprintf(stderr, "[NATIVE] vertex buffers: %llu draws on the GPU, %llu unpacked, %llu uploads\n",
                s_nvb_stat[0], s_nvb_stat[1], s_nvb_stat[2]);
        memset(s_nvb_stat, 0, sizeof s_nvb_stat);
    }
}

/* A resource's Data (a physical address) as a guest address. */
uint32_t nv2a_native_resolve(uint32_t phys)
{
    return dma_resolve(phys);
}

/* Debug: the program at `start` -- which inputs it reads, constants and
 * outputs, instruction by instruction (MAC op/dst, sources). */
void nv2a_native_dump_program(uint32_t start)
{
    static NV2AVshProgram prog;
    int k, j;
    if (start >= VP_SLOTS)
        return;
    vp_decode_program((const uint32_t (*)[4])&s_nvp_code[start], (int)(VP_SLOTS - start), &prog);
    fprintf(stderr, "  [NATIVE] program at %u: %d instructions\n", start, prog.length);
    for (k = 0; k < prog.length && k < 24; k++) {
        const NV2AVshInstruction *q = &prog.insns[k];
        fprintf(stderr, "    %2d mac %d -> t%d o%d m%X ilu %d -> t%d o%d |", k, q->mac_op, q->mac_dst.temp_reg,
                q->mac_dst.output_reg, q->mac_dst.write_mask, q->ilu_op, q->ilu_dst.temp_reg, q->ilu_dst.output_reg);
        for (j = 0; j < 3; j++)
            fprintf(stderr, " %c%d", "tvc"[q->mac_src[j].reg_type % 3], q->mac_src[j].reg_index);
        fprintf(stderr, " ilu %c%d\n", "tvc"[q->ilu_src.reg_type % 3], q->ilu_src.reg_index);
    }
}

/* ── the draw trace: every draw of the last frames, kept so a frame that
 * flashes (gpu_dark_sample) can be written out next to the frame before it
 * and compared draw by draw. ── */

typedef struct {
    uint32_t frame, seq, prim, count, index_va, vp_start, passthrough, ps, result, gpu_vb;
    uint32_t tex_res[2], tex_data[2], tex_rt[2];
    uint32_t blend, src, dst, zen, zwrite, zfunc, cull, alphatest, alpharef, colorwrite;
    uint32_t attr0_va, attr0_stride, attr0_fmt;
    uint32_t vhash, ihash, khash, rtv;
    uint32_t op0, op1;
    uint32_t g_vs, g_ps, g_srv0, g_srv1, g_smp0, g_bs, g_dss, g_rs, g_il, g_vb0, g_ib, g_dsv, g_pscb;
    float vp[4];
    uint32_t t0_hdr_data, t0_hdr_fmt, t0_hdr_size;
    float ext[4];                      /* non-indexed float positions: min x, max x, min y, max y */
    uint32_t prog_hash, prog_len, vs_obj;
    uint32_t tex0_texel[4], tex0_w, tex0_h, tex0_fmt;
    uint32_t px_before, px_after;
    uint32_t vbytes_before[16], vbytes_after[16];
    uint32_t tss0[8], tfactor, alphafunc, fog;
    uint32_t attr_va[4], attr_fmt[4], attr_stride[4];
    float c0_first[4];
    float gin_c[4][4];                 /* what was uploaded: attribute 2 of the first 4 vertices */
    float gin_p[4][4];
} NTraceRec;

#define NTRACE_FRAMES 8
#define NTRACE_DRAWS 1024
static NTraceRec s_ntrace[NTRACE_FRAMES][NTRACE_DRAWS];
static uint32_t s_ntrace_frame[NTRACE_FRAMES], s_ntrace_n[NTRACE_FRAMES];

#define NTRACE_KDRAWS 8                /* draws a frame whose constants are kept whole */
static float s_ntrace_k[NTRACE_FRAMES][NTRACE_KDRAWS][192][4];

static uint32_t ntrace_hash(const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull, v;
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        memcpy(&v, b + i, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

int nv2a_native_draw(const NativeDraw *d, ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv,
                     uint32_t pw, uint32_t ph, uint32_t sw, uint32_t sh)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    unsigned slot = s_frame_no % NTRACE_FRAMES;
    NTraceRec *t;
    int res;
    if (!nenv("BUFFY_FLASHTRACE") && !nenv("BUFFY_NATIVE_SKIPDRAW"))
        return nv2a_native_draw_one(d, rtv, dsv, pw, ph, sw, sh);   /* no tracing: the plain draw */
    s_ntrace_gpu_vb = 0;
    if (!d->index_va && d->count <= 4 && d->attr[0].va)
        memcpy(s_ntrace_vb, mem + d->attr[0].va, 64);
    {
        /* test: BUFFY_NATIVE_SKIPDRAW=n,m,... skips those draws of each frame */
        static uint64_t skip;
        static int parsed;
        uint32_t at = s_ntrace_frame[slot] == s_frame_no ? s_ntrace_n[slot] : 0;
        if (!parsed) {
            const char *e = nenv("BUFFY_NATIVE_SKIPDRAW");
            parsed = 1;
            while (e && *e) {
                int v = atoi(e);
                if (v >= 0 && v < 64) skip |= 1ull << v;
                e = strchr(e, ',');
                if (e) e++;
            }
        }
        if (at < 64 && (skip >> at) & 1)
            res = 0;
        else if (nenv("BUFFY_FLASHTRACE_AT") && at == (uint32_t)atoi(nenv("BUFFY_FLASHTRACE_AT"))) {
            unsigned k = s_frame_no % 8;
            nprobe_copy(rtv, &s_nprobe[k][0]);
            res = nv2a_native_draw_one(d, rtv, dsv, pw, ph, sw, sh);
            nprobe_copy(rtv, &s_nprobe[k][1]);
            s_nprobe_frame[k] = s_frame_no;
        } else if (at < 24 && nenv("BUFFY_FLASHTRACE_PIXELS")) {
            /* the frame's first draws: the left page pixel around each one */
            s_ntrace_px[0] = ntrace_pixel(rtv, 0.25f, 0.45f);
            res = nv2a_native_draw_one(d, rtv, dsv, pw, ph, sw, sh);
            s_ntrace_px[1] = ntrace_pixel(rtv, 0.25f, 0.45f);
        } else
            res = nv2a_native_draw_one(d, rtv, dsv, pw, ph, sw, sh);
    }
    if (s_ntrace_frame[slot] != s_frame_no) {
        s_ntrace_frame[slot] = s_frame_no;
        s_ntrace_n[slot] = 0;
    }
    if (s_ntrace_n[slot] >= NTRACE_DRAWS)
        return res;
    t = &s_ntrace[slot][s_ntrace_n[slot]];
    memset(t, 0, sizeof *t);
    t->frame = s_frame_no;
    t->seq = s_ntrace_n[slot]++;
    t->prim = d->prim; t->count = d->count; t->index_va = d->index_va;
    t->px_before = s_ntrace_px[0]; t->px_after = s_ntrace_px[1];
    memcpy(t->tss0, d->tss + 12, sizeof t->tss0);
    {
        int a;
        for (a = 0; a < 4; a++) {
            t->attr_va[a] = d->attr[a].va; t->attr_fmt[a] = d->attr[a].fmt; t->attr_stride[a] = d->attr[a].stride;
        }
        if (d->attr[2].fmt)
            ndraw_fetch(&d->attr[2], 0, t->c0_first);
        if (!d->index_va && d->count <= 4 && s_gin && !s_ntrace_gpu_vb) {
            uint32_t v;
            for (v = 0; v < d->count; v++) {
                memcpy(t->gin_c[v], s_gin[v].a[2], 16);
                memcpy(t->gin_p[v], s_gin[v].a[0], 16);
            }
        }
    }          /* COLOROP .. RESULTARG of stage 0 */
    t->tfactor = d->rs[NRS_TEXTUREFACTOR]; t->alphafunc = d->rs[NRS_ALPHAFUNC]; t->fog = d->rs[NRS_FOGENABLE];
    if (!d->index_va && d->count <= 4 && d->attr[0].va) {
        memcpy(t->vbytes_before, s_ntrace_vb, 64);
        memcpy(t->vbytes_after, mem + d->attr[0].va, 64);
    }
    s_ntrace_px[0] = s_ntrace_px[1] = 0;
    t->vp_start = d->vp_start; t->passthrough = (uint32_t)d->passthrough; t->ps = (uint32_t)d->pixel_shader;
    t->result = (uint32_t)res; t->gpu_vb = (uint32_t)s_ntrace_gpu_vb;
    t->tex_res[0] = d->tex[0].res; t->tex_res[1] = d->tex[1].res;
    t->tex_data[0] = d->tex[0].data; t->tex_data[1] = d->tex[1].data;
    if (d->tex[0].res && nenv("BUFFY_NATIVE_TEXWATCH")) {
        /* debug: a small texture's bytes, logged whenever they change */
        uint32_t data = *(const uint32_t *)(mem + d->tex[0].res + 4), fmtw = *(const uint32_t *)(mem + d->tex[0].res + 0xC);
        if ((data & 0x0FFFFFFF) == (uint32_t)strtoul(nenv("BUFFY_NATIVE_TEXWATCH"), NULL, 16)) {
            static uint32_t last_h, n;
            const uint32_t *px = (const uint32_t *)(mem + (data & 0x0FFFFFFF));
            uint32_t h = ntrace_hash(px, 128);
            if (h != last_h && n++ < 200)
                fprintf(stderr, "[TEXWATCH] frame %u draw %u: fmt %08X %08X %08X %08X %08X ... %08X\n", s_frame_no,
                        s_ntrace_n[slot], fmtw, px[0], px[1], px[2], px[3], px[31]);
            last_h = h;
        }
    }
    if (d->tex[0].res) {
        t->t0_hdr_data = *(const uint32_t *)(mem + d->tex[0].res + 4);
        t->t0_hdr_fmt = *(const uint32_t *)(mem + d->tex[0].res + 0xC);
        t->t0_hdr_size = *(const uint32_t *)(mem + d->tex[0].res + 0x10);
    }
    t->tex_rt[0] = d->tex[0].srv != NULL; t->tex_rt[1] = d->tex[1].srv != NULL;
    t->blend = d->rs[59]; t->src = d->rs[62]; t->dst = d->rs[63];
    t->zen = d->rs[143]; t->zwrite = d->rs[64]; t->zfunc = d->rs[57]; t->cull = d->rs[147];
    t->alphatest = d->rs[60]; t->alpharef = d->rs[61]; t->colorwrite = d->rs[67];
    t->attr0_va = d->attr[0].va; t->attr0_stride = d->attr[0].stride; t->attr0_fmt = d->attr[0].fmt;
    if (d->attr[0].va && d->attr[0].stride) {
        uint32_t bytes = d->attr[0].stride * (d->index_va ? 64u : d->count);
        if (bytes > 1024) bytes = 1024;
        t->vhash = ntrace_hash(mem + d->attr[0].va, bytes);
    }
    if (!d->index_va && d->attr[0].va && d->attr[0].stride && (d->attr[0].fmt & 0xF) == 2 && ((d->attr[0].fmt >> 4) & 0xF) >= 2
            && d->count <= 4096) {
        uint32_t v;
        t->ext[0] = t->ext[2] = 1e30f; t->ext[1] = t->ext[3] = -1e30f;
        for (v = 0; v < d->count; v++) {
            const float *pp = (const float *)(mem + d->attr[0].va + v * d->attr[0].stride);
            if (pp[0] < t->ext[0]) t->ext[0] = pp[0];
            if (pp[0] > t->ext[1]) t->ext[1] = pp[0];
            if (pp[1] < t->ext[2]) t->ext[2] = pp[1];
            if (pp[1] > t->ext[3]) t->ext[3] = pp[1];
        }
    }
    if (d->index_va)
        t->ihash = ntrace_hash(mem + d->index_va, (d->count > 256 ? 256 : d->count) * 2);
    t->khash = ntrace_hash(s_nconst, sizeof s_nconst);
    if (d->vp_start < VP_SLOTS && !d->passthrough) {
        /* the program as it runs: up to its final instruction */
        uint32_t n = 0;
        while (d->vp_start + n < VP_SLOTS && n < 136) {
            n++;
            if (s_nvp_code[d->vp_start + n - 1][3] & 1)
                break;
        }
        t->prog_len = n;
        t->prog_hash = ntrace_hash(s_nvp_code[d->vp_start], n * 16);
    }
    if (t->seq < NTRACE_KDRAWS)
        memcpy(s_ntrace_k[slot][t->seq], s_nconst, sizeof s_nconst);
    t->rtv = (uint32_t)(uintptr_t)rtv;
    t->op0 = d->tss[NTS_COLOROP] | d->tss[NTS_COLORARG1] << 8 | d->tss[NTS_COLORARG2] << 16 | d->tss[NTS_ALPHAOP] << 24;
    if (res) {
        /* what the GPU was actually given for this draw */
        ID3D11VertexShader *gvs = NULL; ID3D11PixelShader *gps = NULL;
        ID3D11ShaderResourceView *gsrv[2] = { NULL, NULL }; ID3D11SamplerState *gsmp = NULL;
        ID3D11BlendState *gbs = NULL; ID3D11DepthStencilState *gdss = NULL; ID3D11RasterizerState *grs = NULL;
        ID3D11InputLayout *gil = NULL; ID3D11Buffer *gvb = NULL, *gib = NULL, *gcb = NULL;
        ID3D11RenderTargetView *grtv = NULL; ID3D11DepthStencilView *gdsv = NULL;
        D3D11_VIEWPORT gvp; UINT nvp = 1, st = 0, of = 0, sref = 0; DXGI_FORMAT ifmt; float bf[4];
        ID3D11DeviceContext_VSGetShader(s_ctx, &gvs, NULL, NULL);
        ID3D11DeviceContext_PSGetShader(s_ctx, &gps, NULL, NULL);
        ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 2, gsrv);
        ID3D11DeviceContext_PSGetSamplers(s_ctx, 0, 1, &gsmp);
        ID3D11DeviceContext_OMGetBlendState(s_ctx, &gbs, bf, &sref);
        ID3D11DeviceContext_OMGetDepthStencilState(s_ctx, &gdss, &sref);
        ID3D11DeviceContext_RSGetState(s_ctx, &grs);
        ID3D11DeviceContext_IAGetInputLayout(s_ctx, &gil);
        ID3D11DeviceContext_IAGetVertexBuffers(s_ctx, 0, 1, &gvb, &st, &of);
        ID3D11DeviceContext_IAGetIndexBuffer(s_ctx, &gib, &ifmt, &of);
        ID3D11DeviceContext_PSGetConstantBuffers(s_ctx, 0, 1, &gcb);
        ID3D11DeviceContext_OMGetRenderTargets(s_ctx, 1, &grtv, &gdsv);
        ID3D11DeviceContext_RSGetViewports(s_ctx, &nvp, &gvp);
        if (gsrv[0] && d->count <= 8 && !d->index_va) {
            /* small screen quads: what their texture holds on the GPU now */
            ID3D11Resource *res = NULL;
            ID3D11ShaderResourceView_GetResource(gsrv[0], &res);
            if (res) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D *st = NULL;
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
                t->tex0_w = td.Width; t->tex0_h = td.Height; t->tex0_fmt = td.Format;
                if (td.Width <= 64 && td.Height <= 64 && td.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    td.MiscFlags = 0; td.MipLevels = 1; td.ArraySize = 1;
                    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) {
                        D3D11_MAPPED_SUBRESOURCE mp;
                        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)st, 0, 0, 0, 0, res, 0, NULL);
                        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
                            const uint32_t *px = (const uint32_t *)mp.pData;
                            t->tex0_texel[0] = px[0]; t->tex0_texel[1] = px[1];
                            t->tex0_texel[2] = px[td.Width - 1];
                            t->tex0_texel[3] = ((const uint32_t *)((const uint8_t *)mp.pData + mp.RowPitch * (td.Height - 1)))[0];
                            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
                        }
                        ID3D11Texture2D_Release(st);
                    }
                }
                ID3D11Resource_Release(res);
            }
        }
        t->g_vs = (uint32_t)(uintptr_t)gvs; t->g_ps = (uint32_t)(uintptr_t)gps;
        t->g_srv0 = (uint32_t)(uintptr_t)gsrv[0]; t->g_srv1 = (uint32_t)(uintptr_t)gsrv[1];
        t->g_smp0 = (uint32_t)(uintptr_t)gsmp; t->g_bs = (uint32_t)(uintptr_t)gbs;
        t->g_dss = (uint32_t)(uintptr_t)gdss; t->g_rs = (uint32_t)(uintptr_t)grs;
        t->g_il = (uint32_t)(uintptr_t)gil; t->g_vb0 = (uint32_t)(uintptr_t)gvb; t->g_ib = (uint32_t)(uintptr_t)gib;
        t->g_dsv = (uint32_t)(uintptr_t)gdsv; t->g_pscb = (uint32_t)(uintptr_t)gcb;
        t->vp[0] = gvp.TopLeftX; t->vp[1] = gvp.TopLeftY; t->vp[2] = gvp.Width; t->vp[3] = gvp.Height;
        if (gvs) ID3D11VertexShader_Release(gvs);
        if (gps) ID3D11PixelShader_Release(gps);
        if (gsrv[0]) ID3D11ShaderResourceView_Release(gsrv[0]);
        if (gsrv[1]) ID3D11ShaderResourceView_Release(gsrv[1]);
        if (gsmp) ID3D11SamplerState_Release(gsmp);
        if (gbs) ID3D11BlendState_Release(gbs);
        if (gdss) ID3D11DepthStencilState_Release(gdss);
        if (grs) ID3D11RasterizerState_Release(grs);
        if (gil) ID3D11InputLayout_Release(gil);
        if (gvb) ID3D11Buffer_Release(gvb);
        if (gib) ID3D11Buffer_Release(gib);
        if (gcb) ID3D11Buffer_Release(gcb);
        if (grtv) ID3D11RenderTargetView_Release(grtv);
        if (gdsv) ID3D11DepthStencilView_Release(gdsv);
    }
    t->op1 = d->tss[32 + NTS_COLOROP] | d->tss[32 + NTS_COLORARG1] << 8 | d->tss[32 + NTS_COLORARG2] << 16
           | d->tss[32 + NTS_ALPHAOP] << 24;
    return res;
}

/* Write frame `f`'s draws and frame f-1's to the log (once per flash, a few
 * flashes a run). */
static void nv2a_native_dump_frames(uint32_t f, const char *why)
{
    static int dumps;
    uint32_t k;
    if (dumps >= 6)
        return;
    dumps++;
    for (k = 0; k < 2; k++) {
        uint32_t fr = f - 1 + k, pk = fr % 8;
        if (s_nprobe_frame[pk] == fr && s_nprobe[pk][0])
            fprintf(stderr, "[TRACEPROBE] frame %u draw %s: page pixel before %08X after %08X\n", fr,
                    nenv("BUFFY_FLASHTRACE_AT"), nprobe_read(s_nprobe[pk][0]), nprobe_read(s_nprobe[pk][1]));
    }
    for (k = 0; k < 2; k++) {
        uint32_t fr = f - 1 + k, slot = fr % NTRACE_FRAMES, i;
        if (s_ntrace_frame[slot] != fr) {
            fprintf(stderr, "[TRACE] frame %u (%s): no longer kept\n", fr, k ? why : "the frame before");
            continue;
        }
        fprintf(stderr, "[TRACE] frame %u (%s): %u draws\n", fr, k ? why : "the frame before", s_ntrace_n[slot]);
        for (i = 0; i < s_ntrace_n[slot]; i++) {
            const NTraceRec *t = &s_ntrace[slot][i];
            if (t->result >= 0x100) {
                fprintf(stderr, "[TRACE] %u #%u %s %08X %08X %u\n", fr, t->seq,
                        t->result == 0x101 ? "CLEAR flags/colour/rects" : "RENDERTARGET", t->count, t->index_va, t->vp_start);
                continue;
            }
            fprintf(stderr, "[TRACE] %u #%u %s p%u n%u ix%08X vp%u pt%u ps%u gvb%u | t0 %08X/%08X%s t1 %08X/%08X%s"
                            " | bl%u %X>%X z%u/%u/%X cull%X at%u/%02X cw%X | a0 %08X s%u f%02X | vh %08X ih %08X kh %08X rt %08X"
                            " | op %08X %08X\n",
                    fr, t->seq, t->result ? "ok" : "SKIP", t->prim, t->count, t->index_va, t->vp_start, t->passthrough,
                    t->ps, t->gpu_vb, t->tex_res[0], t->tex_data[0], t->tex_rt[0] ? "(rt)" : "",
                    t->tex_res[1], t->tex_data[1], t->tex_rt[1] ? "(rt)" : "",
                    t->blend, t->src, t->dst, t->zen, t->zwrite, t->zfunc, t->cull, t->alphatest, t->alpharef,
                    t->colorwrite, t->attr0_va, t->attr0_stride, t->attr0_fmt, t->vhash, t->ihash, t->khash, t->rtv,
                    t->op0, t->op1);
            if (t->tex_res[0])
                fprintf(stderr, "[TRACET] %u #%u t0 data %08X format %08X size %08X\n", fr, t->seq,
                        t->t0_hdr_data, t->t0_hdr_fmt, t->t0_hdr_size);
            if (t->tex0_w)
                fprintf(stderr, "[TRACEQ] %u #%u tex %ux%u fmt %u texels %08X %08X %08X %08X\n", fr, t->seq, t->tex0_w,
                        t->tex0_h, t->tex0_fmt, t->tex0_texel[0], t->tex0_texel[1], t->tex0_texel[2], t->tex0_texel[3]);
            if (t->vbytes_before[1] || t->vbytes_after[1] || t->vbytes_before[4] || t->vbytes_after[4]) {
                uint32_t w;
                fprintf(stderr, "[TRACEV] %u #%u before", fr, t->seq);
                for (w = 0; w < 16; w++) fprintf(stderr, " %08X", t->vbytes_before[w]);
                fprintf(stderr, " after");
                for (w = 0; w < 16; w++) fprintf(stderr, " %08X", t->vbytes_after[w]);
                fputc(10, stderr);
            }
            fprintf(stderr, "[TRACEA] %u #%u a0 %08X/%u/%02X a1 %08X/%u/%02X a2 %08X/%u/%02X a3 %08X/%u/%02X v2[0] %g %g %g %g\n",
                    fr, t->seq, t->attr_va[0], t->attr_stride[0], t->attr_fmt[0], t->attr_va[1], t->attr_stride[1], t->attr_fmt[1],
                    t->attr_va[2], t->attr_stride[2], t->attr_fmt[2], t->attr_va[3], t->attr_stride[3], t->attr_fmt[3],
                    t->c0_first[0], t->c0_first[1], t->c0_first[2], t->c0_first[3]);
            if (t->gin_p[1][0] || t->gin_p[2][0])
                fprintf(stderr, "[TRACEU] %u #%u uploaded colour a %g %g %g %g | pos %g,%g,%g,%g %g,%g,%g,%g\n", fr, t->seq,
                        t->gin_c[0][3], t->gin_c[1][3], t->gin_c[2][3], t->gin_c[3][3],
                        t->gin_p[0][0], t->gin_p[0][1], t->gin_p[0][2], t->gin_p[0][3], t->gin_p[2][0], t->gin_p[2][1], t->gin_p[2][2], t->gin_p[2][3]);
            fprintf(stderr, "[TRACES] %u #%u tss0 %X %X %X %X / %X %X %X %X tfactor %08X afunc %X fog %u\n", fr, t->seq,
                    t->tss0[0], t->tss0[1], t->tss0[2], t->tss0[3], t->tss0[4], t->tss0[5], t->tss0[6], t->tss0[7],
                    t->tfactor, t->alphafunc, t->fog);
            if (t->px_before || t->px_after)
                fprintf(stderr, "[TRACEPX] %u #%u page pixel %08X -> %08X\n", fr, t->seq, t->px_before, t->px_after);
            fprintf(stderr, "[TRACEP] %u #%u program %u insns hash %08X\n", fr, t->seq, t->prog_len, t->prog_hash);
            if (t->ext[1] > t->ext[0])
                fprintf(stderr, "[TRACEX] %u #%u x %g..%g y %g..%g\n", fr, t->seq, t->ext[0], t->ext[1], t->ext[2], t->ext[3]);
            fprintf(stderr, "[TRACEG] %u #%u vs %08X ps %08X srv %08X %08X smp %08X bs %08X dss %08X rs %08X il %08X vb %08X ib %08X dsv %08X cb %08X vp %g %g %g %g\n",
                    fr, t->seq, t->g_vs, t->g_ps, t->g_srv0, t->g_srv1, t->g_smp0, t->g_bs, t->g_dss, t->g_rs,
                    t->g_il, t->g_vb0, t->g_ib, t->g_dsv, t->g_pscb, t->vp[0], t->vp[1], t->vp[2], t->vp[3]);
        }
    }
    {
        /* the constants of the first draws: every register that moved from
         * the frame before, with both values */
        uint32_t s0 = (f - 1) % NTRACE_FRAMES, s1 = f % NTRACE_FRAMES, i, c;
        if (s_ntrace_frame[s0] == f - 1 && s_ntrace_frame[s1] == f)
            for (i = 0; i < NTRACE_KDRAWS && i < s_ntrace_n[s0] && i < s_ntrace_n[s1]; i++)
                for (c = 0; c < 192; c++) {
                    const float *a = s_ntrace_k[s0][i][c], *b = s_ntrace_k[s1][i][c];
                    if (memcmp(a, b, 16))
                        fprintf(stderr, "[TRACEK] %u #%u c%u: %g %g %g %g -> %g %g %g %g\n", f, i, c,
                                a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                }
    }
    fflush(stderr);
}

/* A non-draw event in the trace (a clear, a render-target switch): kind 1
 * clear (a flags, b colour, c rects), 2 render target (a surface). */
void nv2a_native_trace_note(uint32_t kind, uint32_t a, uint32_t b, uint32_t c)
{
    unsigned slot = s_frame_no % NTRACE_FRAMES;
    NTraceRec *t;
    if (!nenv("BUFFY_FLASHTRACE"))
        return;
    if (s_ntrace_frame[slot] != s_frame_no) {
        s_ntrace_frame[slot] = s_frame_no;
        s_ntrace_n[slot] = 0;
    }
    if (s_ntrace_n[slot] >= NTRACE_DRAWS)
        return;
    t = &s_ntrace[slot][s_ntrace_n[slot]];
    memset(t, 0, sizeof *t);
    t->frame = s_frame_no;
    t->seq = s_ntrace_n[slot]++;
    t->result = 0x100 + kind;
    t->count = a; t->index_va = b; t->vp_start = c;
}
