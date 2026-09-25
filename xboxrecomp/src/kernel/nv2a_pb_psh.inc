/*
 * NV2A pixel pipeline -> HLSL. Included by nv2a_pb_d3d11.inc.
 *
 * Xbox Direct3D 8 has no pixel shader hardware in the PC sense: both its
 * fixed-function texture stages and its "ps.1.x" shaders are compiled by the
 * XDK into NV2A register-combiner settings, which reach the GPU as ordinary
 * pushbuffer methods. This file rebuilds a pixel shader from those registers
 * (semantics follow xemu's psh.c):
 *
 *   texture stages  0x1E70 SHADER_STAGE_PROGRAM, 5 bits a stage
 *   general stages  COLOR/ALPHA_ICW (inputs A B C D) and _OCW (outputs), up
 *                   to 8, count in COMBINER_CONTROL
 *   final combiner  SPECULAR_FOG_CW0/1: rgb = A*B + (1-A)*C + D, a = G
 *
 * Shaders are generated once per distinct setting and cached. Constants
 * (combiner factors, fog colour, alpha reference) go in the constant buffer
 * so they never force a new shader.
 */

#include <stdarg.h>

#define M_ALPHA_ICW   0x0260
#define M_SPECFOG_CW0 0x0288
#define M_SPECFOG_CW1 0x028C
#define M_FACTOR0     0x0A60
#define M_FACTOR1     0x0A80
#define M_ALPHA_OCW   0x0AA0
#define M_COLOR_ICW   0x0AC0
#define M_SPECFOG_FAC 0x1E20
#define M_COLOR_OCW   0x1E40
#define M_COMB_CTL    0x1E60
#define M_STAGE_PROG  0x1E70
#define KEL(m)        s_kel[(m) / 4]

typedef struct {
    uint32_t cicw[8], cocw[8], aicw[8], aocw[8];
    uint32_t control, fin0, fin1, stage_prog;
    uint32_t tex_bound;     /* bit per stage: a texture is bound */
    uint32_t tex_cube;      /* bit per stage: bound texture is a cube map */
} PsKey;

typedef struct {
    PsKey key;
    ID3D11PixelShader *ps;
} PsEntry;

#define PS_CACHE 256
static PsEntry s_ps_cache[PS_CACHE];
static int     s_ps_count;
static uint64_t s_stage_mode_seen[32];   /* RECOMP_VP_STATS: texture modes used */

/* Common HLSL: constant buffer, resources, interpolants, helpers. */
static const char *s_psh_head =
"cbuffer C : register(b0) {\n"
"  float4 alpha;       /* x ref (0..1), y func, z enabled */\n"
"  float4 fogc;        /* rgb fog colour */\n"
"  float4 texnorm[4];  /* xy coordinate multiplier per stage */\n"
"  float4 cf0[8];      /* combiner factor 0, per stage */\n"
"  float4 cf1[8];      /* combiner factor 1, per stage */\n"
"  float4 fcf[2];      /* final combiner factors */\n"
"  float4 bump[4];     /* bump env matrix per stage (00 01 10 11) */\n"
"  float4 bumpl[4];    /* x luminance scale, y offset */\n"
"};\n"
"Texture2D T0 : register(t0); Texture2D T1 : register(t1);\n"
"Texture2D T2 : register(t2); Texture2D T3 : register(t3);\n"
"TextureCube Q0 : register(t4); TextureCube Q1 : register(t5);\n"
"TextureCube Q2 : register(t6); TextureCube Q3 : register(t7);\n"
"SamplerState S0 : register(s0); SamplerState S1 : register(s1);\n"
"SamplerState S2 : register(s2); SamplerState S3 : register(s3);\n"
"struct P { float4 p:SV_Position; float4 c0:COLOR0; float4 c1:COLOR1;\n"
"           float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"           float4 fog:TEXCOORD4; };\n"
"bool cmpf(float f, float a, float b) {\n"
"  if (f < 0.5) return false; if (f < 1.5) return a < b; if (f < 2.5) return a == b;\n"
"  if (f < 3.5) return a <= b; if (f < 4.5) return a > b; if (f < 5.5) return a != b;\n"
"  if (f < 6.5) return a >= b; return true; }\n"
"float2 sgn8(float2 u) { float2 b = floor(u * 255.0 + 0.5); return (b > 127.5 ? b - 256.0 : b) / 127.0; }\n"
"float2 pj(float4 t) { return t.xy / (t.w != 0.0 ? t.w : 1.0); }\n";

typedef struct { char *p; int n, cap; } Sb;

static void sb_add(Sb *b, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (b->n >= b->cap - 1)
        return;
    va_start(ap, fmt);
    r = vsnprintf(b->p + b->n, (size_t)(b->cap - b->n), fmt, ap);
    va_end(ap);
    if (r > 0)
        b->n = (b->n + r < b->cap - 1) ? b->n + r : b->cap - 1;
}

/* Register name for combiner register `reg` in general stage `st`
 * (st < 0: final combiner). */
static const char *psh_reg(int reg, int st, int unique0, int unique1, char *tmp)
{
    switch (reg) {
    case 1:
        if (st < 0) return "fcf[0]";
        sprintf(tmp, "cf0[%d]", unique0 ? st : 0);
        return tmp;
    case 2:
        if (st < 0) return "fcf[1]";
        sprintf(tmp, "cf1[%d]", unique1 ? st : 0);
        return tmp;
    case 3:  return "fogr";
    case 4:  return "v0";
    case 5:  return "v1";
    case 8:  return "t0";
    case 9:  return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    case 14: return "sumr";
    case 15: return "efr";
    default: return "zr";
    }
}

/* One mapped input: 8 bits = reg | alpha<<4 | map<<5. */
static void psh_input(Sb *b, uint32_t in, int st, int alpha_path, int u0, int u1)
{
    char tmp[16], src[48];
    const char *r = psh_reg((int)(in & 0xF), st, u0, u1, tmp);
    int asel = (in >> 4) & 1, map = (int)((in >> 5) & 7);

    if (alpha_path)
        sprintf(src, "%s.%s", r, asel ? "a" : "b");
    else
        sprintf(src, asel ? "%s.aaa" : "%s.rgb", r);
    switch (map) {
    case 0: sb_add(b, "max(%s, 0.0)", src); break;
    case 1: sb_add(b, "(1.0 - saturate(%s))", src); break;
    case 2: sb_add(b, "(2.0 * max(%s, 0.0) - 1.0)", src); break;
    case 3: sb_add(b, "(-2.0 * max(%s, 0.0) + 1.0)", src); break;
    case 4: sb_add(b, "(max(%s, 0.0) - 0.5)", src); break;
    case 5: sb_add(b, "(-max(%s, 0.0) + 0.5)", src); break;
    case 6: sb_add(b, "(%s)", src); break;
    default: sb_add(b, "(-(%s))", src); break;
    }
}

static const char *psh_outmap(uint32_t flags)
{
    switch ((flags >> 3) & 7) {
    case 1:  return "(%s - 0.5)";
    case 2:  return "(%s * 2.0)";
    case 3:  return "((%s - 0.5) * 2.0)";
    case 4:  return "(%s * 4.0)";
    case 6:  return "(%s * 0.5)";
    default: return "(%s)";
    }
}

static const char *psh_dst(int reg)
{
    switch (reg) {
    case 4: return "v0"; case 5: return "v1";
    case 8: return "t0"; case 9: return "t1"; case 10: return "t2"; case 11: return "t3";
    case 12: return "r0"; case 13: return "r1";
    default: return NULL;
    }
}

/* One half (rgb or alpha) of general stage i: computes into temporaries. */
static void psh_half(Sb *b, int i, int alpha_path, uint32_t icw, uint32_t ocw, int u0, int u1, int msb)
{
    const char *ty = alpha_path ? "float" : "float3";
    const char *px = alpha_path ? "a" : "c";
    uint32_t flags = ocw >> 12;
    const char *om = psh_outmap(flags);
    char e[64];
    int k;
    static const char nm[4] = { 'A', 'B', 'C', 'D' };

    for (k = 0; k < 4; k++) {
        sb_add(b, "  %s %s%c%d = ", ty, px, nm[k], i);
        psh_input(b, (icw >> (24 - 8 * k)) & 0xFF, i, alpha_path, u0, u1);
        sb_add(b, ";\n");
    }
    if (!alpha_path && (flags & 2))
        sb_add(b, "  float3 %sab%d = dot(%sA%d, %sB%d).xxx;\n", px, i, px, i, px, i);
    else
        sb_add(b, "  %s %sab%d = %sA%d * %sB%d;\n", ty, px, i, px, i, px, i);
    if (!alpha_path && (flags & 1))
        sb_add(b, "  float3 %scd%d = dot(%sC%d, %sD%d).xxx;\n", px, i, px, i, px, i);
    else
        sb_add(b, "  %s %scd%d = %sC%d * %sD%d;\n", ty, px, i, px, i, px, i);
    if (flags & 4)
        sb_add(b, "  %s %ss%d = %s ? %scd%d : %sab%d;\n", ty, px, i,
               msb ? "(r0.a >= 0.5)" : "((uint(r0.a * 255.0 + 0.5) & 1u) != 0u)", px, i, px, i);
    else
        sb_add(b, "  %s %ss%d = %sab%d + %scd%d;\n", ty, px, i, px, i, px, i);
    sprintf(e, "%sab%d", px, i); sb_add(b, "  %s = clamp(", e); sb_add(b, om, e); sb_add(b, ", -1.0, 1.0);\n");
    sprintf(e, "%scd%d", px, i); sb_add(b, "  %s = clamp(", e); sb_add(b, om, e); sb_add(b, ", -1.0, 1.0);\n");
    sprintf(e, "%ss%d", px, i);  sb_add(b, "  %s = clamp(", e); sb_add(b, om, e); sb_add(b, ", -1.0, 1.0);\n");
}

static void psh_writes(Sb *b, int i, int alpha_path, uint32_t ocw)
{
    const char *px = alpha_path ? "a" : "c";
    const char *sw = alpha_path ? "a" : "rgb";
    const char *d;
    if ((d = psh_dst((int)(ocw & 0xF))) != NULL)        sb_add(b, "  %s.%s = %scd%d;\n", d, sw, px, i);
    if ((d = psh_dst((int)((ocw >> 4) & 0xF))) != NULL) sb_add(b, "  %s.%s = %sab%d;\n", d, sw, px, i);
    if ((d = psh_dst((int)((ocw >> 8) & 0xF))) != NULL) sb_add(b, "  %s.%s = %ss%d;\n", d, sw, px, i);
}

/* Final combiner input: unsigned identity or invert only. */
static void psh_final_input(Sb *b, uint32_t in, int alpha_path)
{
    char tmp[16], src[48];
    const char *r = psh_reg((int)(in & 0xF), -1, 0, 0, tmp);
    int asel = (in >> 4) & 1, inv = (in >> 5) & 1;
    if (alpha_path)
        sprintf(src, "%s.%s", r, asel ? "a" : "b");
    else
        sprintf(src, asel ? "%s.aaa" : "%s.rgb", r);
    sb_add(b, inv ? "(1.0 - saturate(%s))" : "saturate(%s)", src);
}

/* Texture fetch for stage i with shader mode `mode`. */
static void psh_texture(Sb *b, int i, uint32_t mode, const PsKey *k)
{
    int bound = (k->tex_bound >> i) & 1, cube = (k->tex_cube >> i) & 1;
    s_stage_mode_seen[mode & 31]++;
    switch (mode) {
    case 0:                                   /* PROGRAM_NONE */
        break;
    case 1: case 2:                           /* PROJECT2D / PROJECT3D */
        if (bound && !cube)
            sb_add(b, "  t%d = T%d.Sample(S%d, pj(i.t%d) * texnorm[%d].xy);\n", i, i, i, i, i);
        else if (bound)
            sb_add(b, "  t%d = Q%d.Sample(S%d, i.t%d.xyz);\n", i, i, i, i);
        break;
    case 3:                                   /* CUBE_MAP */
        if (bound && cube)
            sb_add(b, "  t%d = Q%d.Sample(S%d, i.t%d.xyz);\n", i, i, i, i);
        else if (bound)
            sb_add(b, "  t%d = T%d.Sample(S%d, i.t%d.xy * texnorm[%d].xy);\n", i, i, i, i, i);
        break;
    case 4:                                   /* PASS_THROUGH */
        sb_add(b, "  t%d = saturate(i.t%d);\n", i, i);
        break;
    case 5:                                   /* CLIP_PLANE: kill if any coord < 0 */
        sb_add(b, "  if (any(i.t%d < 0.0)) discard;\n", i);
        break;
    case 6: case 7:                           /* BUMPENVMAP (_LUMINANCE) */
        if (i > 0 && bound) {
            sb_add(b, "  { float2 d = sgn8(t%d.rg);\n", i - 1);
            sb_add(b, "    float2 uv = i.t%d.xy + float2(dot(d, bump[%d].xz), dot(d, bump[%d].yw));\n", i, i, i);
            sb_add(b, "    t%d = T%d.Sample(S%d, uv * texnorm[%d].xy);\n", i, i, i, i);
            if (mode == 7)
                sb_add(b, "    t%d.rgb *= saturate(t%d.b * bumpl[%d].x + bumpl[%d].y);\n", i, i - 1, i, i);
            sb_add(b, "  }\n");
        }
        break;
    case 15:                                  /* DEPENDENT_AR */
        if (i > 0 && bound)
            sb_add(b, "  t%d = T%d.Sample(S%d, t%d.ar);\n", i, i, i, i - 1);
        break;
    case 16:                                  /* DEPENDENT_GB */
        if (i > 0 && bound)
            sb_add(b, "  t%d = T%d.Sample(S%d, t%d.gb);\n", i, i, i, i - 1);
        break;
    case 17:                                  /* DOT_PRODUCT (feeds later stages) */
        if (i > 0)
            sb_add(b, "  t%d = dot(i.t%d.xyz, t%d.rgb * 2.0 - 1.0).xxxx;\n", i, i, i - 1);
        break;
    default:                                  /* dot-product families: approximate */
        if (bound && !cube)
            sb_add(b, "  t%d = T%d.Sample(S%d, i.t%d.xy * texnorm[%d].xy);\n", i, i, i, i, i);
        break;
    }
}

static int psh_generate(const PsKey *k, char *out, int cap)
{
    Sb b;
    int i, n = (int)(k->control & 0xFF);
    int u0 = (k->control >> 12) & 1, u1 = (k->control >> 16) & 1, msb = (k->control >> 8) & 1;

    b.p = out; b.n = 0; b.cap = cap;
    out[0] = 0;
    sb_add(&b, "%s", s_psh_head);
    sb_add(&b, "float4 ps(P i) : SV_Target {\n");
    sb_add(&b, "  float4 zr = 0, t0 = 0, t1 = 0, t2 = 0, t3 = 0;\n");
    sb_add(&b, "  float4 v0 = i.c0, v1 = i.c1;\n");
    sb_add(&b, "  float4 fogr = float4(fogc.rgb, saturate(i.fog.x));\n");
    for (i = 0; i < 4; i++)
        psh_texture(&b, i, (k->stage_prog >> (5 * i)) & 0x1F, k);
    sb_add(&b, "  float4 r0 = float4(0, 0, 0, t0.a), r1 = 0;\n");
    if (n > 8) n = 8;
    for (i = 0; i < n; i++) {
        sb_add(&b, "  /* stage %d */\n", i);
        psh_half(&b, i, 0, k->cicw[i], k->cocw[i], u0, u1, msb);
        psh_half(&b, i, 1, k->aicw[i], k->aocw[i], u0, u1, msb);
        psh_writes(&b, i, 0, k->cocw[i]);
        psh_writes(&b, i, 1, k->aocw[i]);
        /* Blue-to-alpha: the rgb product's blue also lands in its alpha. */
        if ((k->cocw[i] >> 12) & 0x80) {
            const char *d = psh_dst((int)((k->cocw[i] >> 4) & 0xF));
            if (d) sb_add(&b, "  %s.a = cab%d.b;\n", d, i);
        }
        if ((k->cocw[i] >> 12) & 0x40) {
            const char *d = psh_dst((int)(k->cocw[i] & 0xF));
            if (d) sb_add(&b, "  %s.a = ccd%d.b;\n", d, i);
        }
    }
    sb_add(&b, "  float4 o;\n");
    if (k->fin0 || k->fin1) {
        uint32_t fl = k->fin1 & 0xFF;
        /* Special final-combiner registers. */
        sb_add(&b, "  float4 sumr = float4(%s + %s, 0);\n",
               (fl & 0x40) ? "(1.0 - saturate(v1.rgb))" : "saturate(v1.rgb)",
               (fl & 0x20) ? "(1.0 - saturate(r0.rgb))" : "saturate(r0.rgb)");
        if (fl & 0x80)
            sb_add(&b, "  sumr = saturate(sumr);\n");
        sb_add(&b, "  float4 efr = float4(");
        psh_final_input(&b, (k->fin1 >> 24) & 0xFF, 0);
        sb_add(&b, " * ");
        psh_final_input(&b, (k->fin1 >> 16) & 0xFF, 0);
        sb_add(&b, ", 0);\n");
        sb_add(&b, "  float3 fA = "); psh_final_input(&b, (k->fin0 >> 24) & 0xFF, 0); sb_add(&b, ";\n");
        sb_add(&b, "  float3 fB = "); psh_final_input(&b, (k->fin0 >> 16) & 0xFF, 0); sb_add(&b, ";\n");
        sb_add(&b, "  float3 fC = "); psh_final_input(&b, (k->fin0 >> 8) & 0xFF, 0); sb_add(&b, ";\n");
        sb_add(&b, "  float3 fD = "); psh_final_input(&b, k->fin0 & 0xFF, 0); sb_add(&b, ";\n");
        sb_add(&b, "  o.rgb = saturate(fA * fB + (1.0 - fA) * fC + fD);\n");
        sb_add(&b, "  o.a = "); psh_final_input(&b, (k->fin1 >> 8) & 0xFF, 1); sb_add(&b, ";\n");
    } else {
        sb_add(&b, "  o = saturate(r0);\n");
    }
    sb_add(&b, "  if (alpha.z > 0.5 && !cmpf(alpha.y, floor(o.a * 255.0 + 0.5), alpha.x)) discard;\n");
    sb_add(&b, "  return o;\n}\n");
    return b.n < cap - 1;
}

static void psh_key(PsKey *k, uint32_t bound, uint32_t cube)
{
    int i, n;
    memset(k, 0, sizeof *k);
    k->control = KEL(M_COMB_CTL);
    n = (int)(k->control & 0xFF);
    if (n > 8) n = 8;
    for (i = 0; i < n; i++) {
        k->cicw[i] = KEL(M_COLOR_ICW + 4 * i);
        k->cocw[i] = KEL(M_COLOR_OCW + 4 * i);
        k->aicw[i] = KEL(M_ALPHA_ICW + 4 * i);
        k->aocw[i] = KEL(M_ALPHA_OCW + 4 * i);
    }
    k->fin0 = KEL(M_SPECFOG_CW0);
    k->fin1 = KEL(M_SPECFOG_CW1);
    k->stage_prog = KEL(M_STAGE_PROG) & 0xFFFFF;
    k->tex_bound = bound;
    k->tex_cube = cube;
}

static ID3D11PixelShader *psh_get(uint32_t bound, uint32_t cube)
{
    PsKey k;
    int i;
    static char src[32768];
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D11PixelShader *ps = NULL;

    static int last;
    psh_key(&k, bound, cube);
    if (last < s_ps_count && !memcmp(&s_ps_cache[last].key, &k, sizeof k))
        return s_ps_cache[last].ps;         /* consecutive draws share state */
    for (i = 0; i < s_ps_count; i++)
        if (!memcmp(&s_ps_cache[i].key, &k, sizeof k)) {
            last = i;
            return s_ps_cache[i].ps;
        }
    if (!psh_generate(&k, src, sizeof src)) {
        fprintf(stderr, "  [GPU11] combiner shader too long\n");
        return NULL;
    }
    { LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a);
    if (FAILED(gpu_compile(src, "ps", "ps_4_0", &blob))) { blob = NULL; }
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    if (getenv("RECOMP_PB_PROFILE"))
        fprintf(stderr, "  [HITCH] pixel shader #%d compiled in %.1f ms\n", s_ps_count,
                (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart); }
    if (!blob) {
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "  [GPU11] combiner shader failed: %s\n%s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", src);
        if (err) ID3D10Blob_Release(err);
    } else {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                       ID3D10Blob_GetBufferSize(blob), NULL, &ps);
        ID3D10Blob_Release(blob);
    }
    if (getenv("RECOMP_PSH_DUMP")) {
        static int dumped;
        if (dumped++ < 40)
            fprintf(stderr, "  [PSH] #%d ctl %08X prog %05X fin %08X %08X bound %X\n%s\n",
                    s_ps_count, k.control, k.stage_prog, k.fin0, k.fin1, bound, src);
    }
    if (s_ps_count < PS_CACHE) {
        s_ps_cache[s_ps_count].key = k;
        s_ps_cache[s_ps_count].ps = ps;
        s_ps_count++;
    }
    return ps;
}

/* ── texel decoding (all NV2A colour formats the title can hand us) ──── */

static uint32_t tx_bpp(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x14: case 0x19: case 0x1B: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x15: case 0x16:
    case 0x17: case 0x18: case 0x1A: case 0x1C: case 0x1D: case 0x20: case 0x24: case 0x25:
    case 0x27: case 0x28: case 0x29: case 0x2C: case 0x30: case 0x31: case 0x35:
        return 2;
    default:
        return 4;
    }
}

static int tx_swizzled(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x0B: case 0x19: case 0x1A: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x31: case 0x3A: case 0x3B: case 0x3C:
        return 1;
    default:
        return 0;
    }
}

static uint32_t x5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t x6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t x4(uint32_t v) { return v * 17; }

/* One texel as A8R8G8B8. `p` points at the texel. */
static uint32_t tx_texel(uint32_t fmt, const uint8_t *p, const uint32_t *pal)
{
    uint32_t t;
    switch (fmt) {
    case 0x06: case 0x12:                         return *(const uint32_t *)p;
    case 0x07: case 0x1E:                         return *(const uint32_t *)p | 0xFF000000u;
    case 0x3A: case 0x3F:
        t = *(const uint32_t *)p;
        return (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
    case 0x3B: case 0x40:
        t = *(const uint32_t *)p;
        return ((t & 0xFF) << 24) | (((t >> 8) & 0xFF) << 16) | (((t >> 16) & 0xFF) << 8) | (t >> 24);
    case 0x3C: case 0x41:
        t = *(const uint32_t *)p;
        return ((t & 0xFF) << 24) | (t >> 8);
    case 0x02: case 0x10:
        t = *(const uint16_t *)p;
        return ((t & 0x8000) ? 0xFF000000u : 0) | x5((t >> 10) & 31) << 16 | x5((t >> 5) & 31) << 8 | x5(t & 31);
    case 0x03: case 0x1C:
        t = *(const uint16_t *)p;
        return 0xFF000000u | x5((t >> 10) & 31) << 16 | x5((t >> 5) & 31) << 8 | x5(t & 31);
    case 0x04: case 0x1D:
        t = *(const uint16_t *)p;
        return x4(t >> 12) << 24 | x4((t >> 8) & 15) << 16 | x4((t >> 4) & 15) << 8 | x4(t & 15);
    case 0x05: case 0x11:
        t = *(const uint16_t *)p;
        return 0xFF000000u | x5(t >> 11) << 16 | x6((t >> 5) & 63) << 8 | x5(t & 31);
    case 0x27: case 0x25:                         /* R6G5B5 */
        t = *(const uint16_t *)p;
        return 0xFF000000u | x6(t >> 10) << 16 | x5((t >> 5) & 31) << 8 | x5(t & 31);
    case 0x00: case 0x13:                         /* Y8 */
        t = p[0];
        return 0xFF000000u | t << 16 | t << 8 | t;
    case 0x01: case 0x1B:                         /* AY8: same value everywhere */
        t = p[0];
        return t << 24 | t << 16 | t << 8 | t;
    case 0x19: case 0x1F:                         /* A8 */
        return (uint32_t)p[0] << 24 | 0x00FFFFFFu;
    case 0x1A: case 0x20:                         /* A8Y8: low byte Y, high A */
        t = *(const uint16_t *)p;
        return (t >> 8) << 24 | (t & 0xFF) << 16 | (t & 0xFF) << 8 | (t & 0xFF);
    case 0x28: case 0x17:                         /* G8B8 */
        t = *(const uint16_t *)p;
        return 0xFF000000u | (t >> 8) << 8 | (t & 0xFF);
    case 0x29: case 0x16:                         /* R8B8 */
        t = *(const uint16_t *)p;
        return 0xFF000000u | (t >> 8) << 16 | (t & 0xFF);
    case 0x0B:                                    /* I8 palette */
        return pal ? pal[p[0]] : 0xFFFF00FFu;
    case 0x31: case 0x35:                         /* Y16 */
        t = p[1];
        return 0xFF000000u | t << 16 | t << 8 | t;
    case 0x2A: case 0x2B: case 0x2E: case 0x2F:   /* depth 24 as grey */
        t = *(const uint32_t *)p >> 24;
        return 0xFF000000u | t << 16 | t << 8 | t;
    case 0x2C: case 0x2D: case 0x30:              /* depth 16 */
        t = p[1];
        return 0xFF000000u | t << 16 | t << 8 | t;
    case 0x24: {                                  /* YUY2-ish: take luma */
        t = p[0];
        return 0xFF000000u | t << 16 | t << 8 | t;
    }
    default:
        return 0xFFFF00FFu;
    }
}

/* Decode a whole level into px (w*h A8R8G8B8). */
static void tx_decode_level(uint32_t fmt, const uint8_t *base, uint32_t w, uint32_t h,
                            uint32_t pitch, const uint32_t *pal, uint32_t *px)
{
    uint32_t x, y, bpp = tx_bpp(fmt);
    if (fmt == 0x24 || fmt == 0x25) {
        /* 4:2:2 video (movie frames): 0x24 bytes Y0 U Y1 V, 0x25 U Y0 V Y1.
         * Two pixels share a chroma pair; BT.601 video range. */
        if (!pitch) pitch = w * 2;
        for (y = 0; y < h; y++) {
            const uint8_t *r = base + (size_t)y * pitch;
            for (x = 0; x + 1 < w + 1 && x < w; x += 2) {
                int y0, y1, u, v, k;
                if (fmt == 0x24) { y0 = r[0]; u = r[1]; y1 = r[2]; v = r[3]; }
                else             { u = r[0]; y0 = r[1]; v = r[2]; y1 = r[3]; }
                for (k = 0; k < 2 && x + k < w; k++) {
                    int c = ((k ? y1 : y0) - 16) * 298, d = u - 128, e = v - 128;
                    int R = (c + 409 * e + 128) >> 8;
                    int G = (c - 100 * d - 208 * e + 128) >> 8;
                    int B = (c + 516 * d + 128) >> 8;
                    R = R < 0 ? 0 : (R > 255 ? 255 : R);
                    G = G < 0 ? 0 : (G > 255 ? 255 : G);
                    B = B < 0 ? 0 : (B > 255 ? 255 : B);
                    px[y * w + x + k] = 0xFF000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
                }
                r += 4;
            }
        }
        return;
    }
    if (tx_swizzled(fmt)) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                px[y * w + x] = tx_texel(fmt, base + (size_t)swizzle_offset(x, y, w, h) * bpp, pal);
    } else {
        if (!pitch) pitch = w * bpp;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                px[y * w + x] = tx_texel(fmt, base + (size_t)y * pitch + (size_t)x * bpp, pal);
    }
}

/* ── samplers ─────────────────────────────────────────────────────────── */

static D3D11_TEXTURE_ADDRESS_MODE tx_addr(uint32_t m)
{
    switch (m & 0xF) {
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3:  return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5:  return D3D11_TEXTURE_ADDRESS_CLAMP;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static ID3D11SamplerState *tx_sampler(int st, int has_mips)
{
    static ID3D11SamplerState *cache[64];
    static uint32_t key[64][3];
    static int count;
    uint32_t addr = tex_reg(st, 0x1B08) & 0x000F0F0Fu;
    uint32_t filt = tex_reg(st, 0x1B14) & 0x0FFF0000u;
    uint32_t border = tex_reg(st, 0x1B24);
    uint32_t minf = (filt >> 16) & 0xFF, magf = (filt >> 24) & 0xF;
    D3D11_SAMPLER_DESC d;
    int i, lin_min, lin_mag, lin_mip, use_mip;

    if (!has_mips) filt |= 0x80000000u;
    if (has_mips == 2) filt |= 0x40000000u;
    for (i = 0; i < count; i++)
        if (key[i][0] == addr && key[i][1] == filt && key[i][2] == border)
            return cache[i];
    lin_min = minf == 2 || minf == 4 || minf == 6 || minf == 7 || minf == 0;
    lin_mag = magf != 1;
    use_mip = has_mips && minf >= 3 && minf <= 6;
    lin_mip = minf == 5 || minf == 6;
    if (has_mips == 2) {
        /* A texture-pack replacement, often far larger than the original:
         * always mipmapped, so drawn at the original size it does not shimmer. */
        use_mip = lin_mip = 1;
    }
    memset(&d, 0, sizeof d);
    d.Filter = (D3D11_FILTER)((lin_min ? 0x10 : 0) | (lin_mag ? 0x04 : 0) | (lin_mip ? 0x01 : 0));
    d.AddressU = tx_addr(addr);
    d.AddressV = tx_addr(addr >> 8);
    d.AddressW = tx_addr(addr >> 16);
    d.BorderColor[0] = ((border >> 16) & 0xFF) / 255.0f;
    d.BorderColor[1] = ((border >> 8) & 0xFF) / 255.0f;
    d.BorderColor[2] = (border & 0xFF) / 255.0f;
    d.BorderColor[3] = (border >> 24) / 255.0f;
    d.MaxLOD = use_mip ? D3D11_FLOAT32_MAX : 0.0f;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (count == 64) {
        ID3D11SamplerState_Release(cache[63]);
        count = 63;
    }
    ID3D11Device_CreateSamplerState(s_dev, &d, &cache[count]);
    key[count][0] = addr; key[count][1] = filt; key[count][2] = border;
    return cache[count++];
}
