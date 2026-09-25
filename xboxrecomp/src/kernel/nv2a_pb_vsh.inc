/*
 * NV2A vertex programs -> HLSL vertex shaders. Included by nv2a_pb_d3d11.inc.
 *
 * The CPU interpreter (nv2a_pb_vp.inc) is exact but costs ~400 ns a vertex,
 * which in gameplay is most of a frame. This translates each distinct
 * program into a D3D11 vertex shader with the same semantics, so the CPU only
 * unpacks the sixteen input attributes and the GPU runs the program:
 *
 *   - MAC and ILU read pre-instruction state, then MAC stores, then ILU
 *   - write masks per unit; temp R12 and oPos are the same register
 *   - c[a0.x + n] relative constants; ARL is floor(x + 0.001)
 *   - oPos is screen space (D3D's viewport epilogue), turned back into clip
 *     space exactly as gpu_vertex() does, with the title's depth mapping
 *     (W-buffer or Z through CLIP_MIN/MAX) and fog factor
 *
 * The draw-trace frame (BUFFY_DRAWLOG) and RECOMP_CPU_VP=1 keep the CPU path.
 */

#define VS_CACHE 256
#define VS_CONSTS 196           /* c[0..191] + 4 parameter vectors */

typedef struct {
    uint32_t hash, len;
    uint32_t code[VP_SLOTS][4];
    ID3D11VertexShader *vs;
    int uses_viewport;
} VsEntry;

typedef struct { float a[16][4]; } GpuIn;

static VsEntry          *s_vs_cache;
static int               s_vs_count;
static ID3D11InputLayout *s_vs_layout;
static ID3D11Buffer      *s_vs_cb, *s_vs_vb;
static GpuIn             *s_gin;
static float             s_vs_cb_shadow[VS_CONSTS][4];
static ID3DBlob          *s_vs_sig;           /* any compiled VS, for the layout */

static const char *s_vsh_head =
"cbuffer K : register(b1) { float4 c[192]; float4 vpp[4]; };\n"
"/* vpp[0]: uses_viewport, zpersp, clip min, clip max\n"
"   vpp[1]: surface w, h, fog enable, fog mode\n"
"   vpp[2]: fog p0, p1 */\n"
"struct VI { float4 v[16] : ATTR; };\n"
"struct P { float4 p:SV_Position; float4 c0:COLOR0; float4 c1:COLOR1;\n"
"           float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"           float4 fog:TEXCOORD4; };\n"
"float fogf(float d) {\n"
"  if (vpp[1].z < 0.5) return 1.0;\n"
"  float m = vpp[1].w, p0 = vpp[2].x, p1 = vpp[2].y, f;\n"
"  if (m >= 2051.0 && m <= 2054.0) d = abs(d);\n"
"  if (m == 2048.0 || m == 2051.0) f = p0 + exp2(d * p1 * 16.0) - 1.0;\n"
"  else if (m == 2049.0 || m == 2052.0) f = p0 + exp2(-d * d * p1 * p1 * 32.0) - 1.0;\n"
"  else f = p0 + d * p1 - 1.0;\n"
"  return saturate(f); }\n";

static const char *vsh_mask(uint32_t m, char *buf)
{
    int n = 0;
    if (m & 8) buf[n++] = 'x';
    if (m & 4) buf[n++] = 'y';
    if (m & 2) buf[n++] = 'z';
    if (m & 1) buf[n++] = 'w';
    buf[n] = 0;
    return buf;
}

static void vsh_src(Sb *b, const NV2AVshSrcOperand *s)
{
    static const char sw[4] = { 'x', 'y', 'z', 'w' };
    int idx = s->reg_index;
    sb_add(b, "%s", s->negate ? "-" : "");
    switch (s->reg_type) {
    case NV2A_VSH_REG_TEMP:  sb_add(b, "r[%d]", idx < 13 ? idx : 12); break;
    case NV2A_VSH_REG_INPUT: sb_add(b, "I.v[%d]", idx & 15); break;
    default:
        if (s->rel_addr) sb_add(b, "c[clamp(a0 + %d, 0, 191)]", idx);
        else             sb_add(b, "c[%d]", idx < 0 ? 0 : (idx > 191 ? 191 : idx));
        break;
    }
    sb_add(b, ".%c%c%c%c", sw[s->swizzle.x & 3], sw[s->swizzle.y & 3],
           sw[s->swizzle.z & 3], sw[s->swizzle.w & 3]);
}

static void vsh_store(Sb *b, const NV2AVshDstOperand *d, const char *val)
{
    char mk[5];
    uint32_t tm = d->write_mask & 0xF, om = (d->write_mask >> 4) & 0xF;
    if (tm && d->temp_reg >= 0 && d->temp_reg < 13) {
        vsh_mask(tm, mk);
        sb_add(b, "  r[%d].%s = %s.%s;\n", d->temp_reg, mk, val, mk);
        if (d->temp_reg == 12)
            sb_add(b, "  o[0].%s = %s.%s;\n", mk, val, mk);
    }
    if (om && d->output_reg != NV2A_VSH_OUT_NONE && d->output_reg < 13) {
        vsh_mask(om, mk);
        sb_add(b, "  o[%d].%s = %s.%s;\n", (int)d->output_reg, mk, val, mk);
        if (d->output_reg == NV2A_VSH_OUT_POS)
            sb_add(b, "  r[12].%s = %s.%s;\n", mk, val, mk);
    }
}

static int vsh_generate(const NV2AVshProgram *pr, char *out, int cap)
{
    Sb b;
    int i;
    b.p = out; b.n = 0; b.cap = cap;
    sb_add(&b, "%s", s_vsh_head);
    sb_add(&b, "P vs(VI I) {\n");
    sb_add(&b, "  float4 r[13], o[13]; int a0 = 0; int k;\n");
    sb_add(&b, "  [unroll] for (k = 0; k < 13; k++) { r[k] = 0; o[k] = 0; }\n");
    sb_add(&b, "  o[3] = 1; o[4].w = 1; o[9].w = 1; o[10].w = 1; o[11].w = 1; o[12].w = 1;\n");
    for (i = 0; i < pr->length; i++) {
        const NV2AVshInstruction *q = &pr->insns[i];
        int has_mac = q->mac_op != NV2A_VSH_MAC_NOP, has_ilu = q->ilu_op != NV2A_VSH_ILU_NOP;
        if (!has_mac && !has_ilu) {
            if (q->is_final) break;
            continue;
        }
        sb_add(&b, "  { /* %d */\n", i);
        if (has_mac) {
            sb_add(&b, "  float4 A = "); vsh_src(&b, &q->mac_src[0]); sb_add(&b, ";\n");
            sb_add(&b, "  float4 B = "); vsh_src(&b, &q->mac_src[1]); sb_add(&b, ";\n");
            sb_add(&b, "  float4 C = "); vsh_src(&b, &q->mac_src[2]); sb_add(&b, ";\n");
            switch (q->mac_op) {
            case NV2A_VSH_MAC_MOV: sb_add(&b, "  float4 m = A;\n"); break;
            case NV2A_VSH_MAC_MUL: sb_add(&b, "  float4 m = A * B;\n"); break;
            case NV2A_VSH_MAC_ADD: sb_add(&b, "  float4 m = A + C;\n"); break;
            case NV2A_VSH_MAC_MAD: sb_add(&b, "  float4 m = A * B + C;\n"); break;
            case NV2A_VSH_MAC_DP3: sb_add(&b, "  float4 m = dot(A.xyz, B.xyz).xxxx;\n"); break;
            case NV2A_VSH_MAC_DPH: sb_add(&b, "  float4 m = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"); break;
            case NV2A_VSH_MAC_DP4: sb_add(&b, "  float4 m = dot(A, B).xxxx;\n"); break;
            case NV2A_VSH_MAC_DST: sb_add(&b, "  float4 m = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
            case NV2A_VSH_MAC_MIN: sb_add(&b, "  float4 m = min(A, B);\n"); break;
            case NV2A_VSH_MAC_MAX: sb_add(&b, "  float4 m = max(A, B);\n"); break;
            case NV2A_VSH_MAC_SLT: sb_add(&b, "  float4 m = float4(A < B);\n"); break;
            case NV2A_VSH_MAC_SGE: sb_add(&b, "  float4 m = float4(A >= B);\n"); break;
            default:               sb_add(&b, "  float4 m = 0;\n"); break;
            }
        }
        if (has_ilu) {
            sb_add(&b, "  float4 L = "); vsh_src(&b, &q->ilu_src); sb_add(&b, ";\n");
            sb_add(&b, "  float x = L.x; float4 l;\n");
            switch (q->ilu_op) {
            case NV2A_VSH_ILU_MOV: sb_add(&b, "  l = L;\n"); break;
            case NV2A_VSH_ILU_RCP: sb_add(&b, "  l = (x == 0.0 ? 3.4e38 : 1.0 / x).xxxx;\n"); break;
            case NV2A_VSH_ILU_RCC:
                sb_add(&b, "  { float v = (x == 0.0) ? 1.884467e19 : 1.0 / x;\n"
                           "    v = v > 0 ? clamp(v, 5.42101e-20, 1.884467e19) : clamp(v, -1.884467e19, -5.42101e-20);\n"
                           "    l = v.xxxx; }\n");
                break;
            case NV2A_VSH_ILU_RSQ: sb_add(&b, "  l = (x == 0.0 ? 3.4e38 : rsqrt(abs(x))).xxxx;\n"); break;
            case NV2A_VSH_ILU_EXP: sb_add(&b, "  l = float4(exp2(floor(x)), x - floor(x), exp2(x), 1.0);\n"); break;
            case NV2A_VSH_ILU_LOG:
                sb_add(&b, "  { float ax = abs(x);\n"
                           "    if (ax == 0.0) l = float4(-3.4e38, 1.0, -3.4e38, 1.0);\n"
                           "    else { float e = floor(log2(ax)); l = float4(e, ax / exp2(e), log2(ax), 1.0); } }\n");
                break;
            case NV2A_VSH_ILU_LIT:
                sb_add(&b, "  { float nl = max(L.x, 0.0), nh = max(L.y, 0.0), pw = clamp(L.w, -127.9961, 127.9961);\n"
                           "    l = float4(1.0, nl, L.x > 0.0 ? pow(nh, pw) : 0.0, 1.0); }\n");
                break;
            default: sb_add(&b, "  l = 0;\n"); break;
            }
        }
        if (has_mac) {
            if (q->mac_op == NV2A_VSH_MAC_ARL)
                sb_add(&b, "  a0 = (int)floor(A.x + 0.001);\n");
            else
                vsh_store(&b, &q->mac_dst, "m");
        }
        if (has_ilu)
            vsh_store(&b, &q->ilu_dst, "l");
        sb_add(&b, "  }\n");
        if (q->is_final)
            break;
    }
    sb_add(&b,
        "  if (all(o[0] == 0)) o[0] = r[12];\n"
        "  P p;\n"
        "  float w = o[0].w; if (abs(w) < 1e-8) w = 1.0;\n"
        "  float zd = vpp[0].y > 0.5 ? w : o[0].z, zn;\n"
        "  if (vpp[0].w > vpp[0].z) zn = (zd - vpp[0].z) / (vpp[0].w - vpp[0].z);\n"
        "  else zn = c[58].z != 0.0 ? (o[0].z - c[59].z) / c[58].z : 0.5;\n"
        "  if (vpp[0].x > 0.5 && c[58].x != 0.0 && c[58].y != 0.0)\n"
        "    p.p = float4((o[0].x - c[59].x) / c[58].x * w, (o[0].y - c[59].y) / c[58].y * w, zn * w, w);\n"
        "  else\n"
        "    p.p = float4(o[0].x / (vpp[1].x * 0.5) - 1.0, 1.0 - o[0].y / (vpp[1].y * 0.5), saturate(zn), 1.0);\n"
        "  p.c0 = saturate(o[3]); p.c1 = saturate(o[4]);\n"
        "  p.t0 = o[9]; p.t1 = o[10]; p.t2 = o[11]; p.t3 = o[12];\n"
        "  p.fog = float4(fogf(o[5].x), 0, 0, 1);\n"
        "  return p;\n}\n");
    return b.n < cap - 1;
}

static int vsh_init(void)
{
    D3D11_BUFFER_DESC bd;
    s_vs_cache = (VsEntry *)calloc(VS_CACHE, sizeof(VsEntry));
    s_gin = (GpuIn *)malloc(sizeof(GpuIn) * GPU_MAX_VERTS);
    if (!s_vs_cache || !s_gin)
        return 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(GpuIn) * GPU_MAX_VERTS;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vs_vb)))
        return 0;
    bd.ByteWidth = sizeof s_vs_cb_shadow;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = 0;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vs_cb)))
        return 0;
    return 1;
}

/* The shader for the current program, or NULL (use the CPU path). */
static ID3D11VertexShader *vsh_get(void)
{
    static char src[65536];
    static int last = -1;
    uint32_t start = s_vp_start < VP_SLOTS ? s_vp_start : 0;
    uint32_t len, hash = 2166136261u, k;
    const uint32_t *code;
    ID3DBlob *blob = NULL, *err = NULL;
    VsEntry *e;
    int i;

    vp_prepare();
    len = (uint32_t)s_vp_prog.length;
    if (!len || len > VP_SLOTS - start)
        return NULL;
    code = &s_vp_code[start][0];
    for (k = 0; k < len * 4; k++)
        hash = (hash ^ code[k]) * 16777619u;
    if (last >= 0 && s_vs_cache[last].hash == hash && s_vs_cache[last].len == len
            && !memcmp(s_vs_cache[last].code, code, len * 16))
        return s_vs_cache[last].vs;
    for (i = 0; i < s_vs_count; i++) {
        e = &s_vs_cache[i];
        if (e->hash == hash && e->len == len && !memcmp(e->code, code, len * 16)) {
            last = i;
            return e->vs;
        }
    }
    if (s_vs_count >= VS_CACHE)
        return NULL;
    e = &s_vs_cache[s_vs_count];
    if (!vsh_generate(&s_vp_prog, src, sizeof src))
        return NULL;
    { LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a);
    if (FAILED(gpu_compile(src, "vs", "vs_4_0", &blob))) { blob = NULL; }
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    if (getenv("RECOMP_PB_PROFILE"))
        fprintf(stderr, "  [HITCH] vertex shader #%d compiled in %.1f ms\n", s_vs_count,
                (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart); }
    if (!blob) {
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "  [GPU11] vertex program shader failed: %s\n%s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", src);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                    ID3D10Blob_GetBufferSize(blob), NULL, &e->vs);
    if (!s_vs_layout) {
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
    if (!e->vs || !s_vs_layout)
        return NULL;
    e->hash = hash;
    e->len = len;
    memcpy(e->code, code, len * 16);
    e->uses_viewport = s_vp_uses_viewport;
    last = s_vs_count++;
    return e->vs;
}

/* Constants and parameters for the program; uploaded only when changed. */
static void vsh_constants(float sw, float sh)
{
    float p[4][4];
    memset(p, 0, sizeof p);
    p[0][0] = s_vp_uses_viewport ? 1.0f : 0.0f;
    p[0][1] = (KEL(0x0290) & 0x10000) ? 1.0f : 0.0f;
    memcpy(&p[0][2], &KEL(0x0394), 4);
    memcpy(&p[0][3], &KEL(0x0398), 4);
    p[1][0] = sw;
    p[1][1] = sh;
    p[1][2] = KEL(0x02A4) ? 1.0f : 0.0f;
    p[1][3] = (float)KEL(0x029C);
    memcpy(&p[2][0], &KEL(0x09C0), 4);
    memcpy(&p[2][1], &KEL(0x09C4), 4);
    if (memcmp(s_vs_cb_shadow, s_vp_const, sizeof(float) * 192 * 4)
            || memcmp(s_vs_cb_shadow[192], p, sizeof p)) {
        memcpy(s_vs_cb_shadow, s_vp_const, sizeof(float) * 192 * 4);
        memcpy(s_vs_cb_shadow[192], p, sizeof p);
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_vs_cb, 0, NULL, s_vs_cb_shadow, 0, 0);
    }
}

/* The raw inputs of one vertex (what vp_vertex feeds the interpreter). */
static void vsh_input(uint32_t index, GpuIn *g)
{
    int a;
    for (a = 0; a < 16; a++)
        fetch_attr(&s_gpu.attr[a], index, g->a[a]);
}
