/*
 * NV2A vertex programs and a textured, blended, depth-tested rasteriser for
 * the pushbuffer executor. Included by nv2a_pb_exec.c (shares its state).
 *
 * Titles that draw through vertex programs (NV097_SET_TRANSFORM_EXECUTION_MODE
 * = PROGRAM) never put screen-space positions in their vertex arrays, so the
 * executor's original path skipped every batch they drew. This runs the
 * program the title uploaded, per vertex, on the CPU:
 *
 *   - program words: SET_TRANSFORM_PROGRAM_LOAD selects the slot, each
 *     SET_TRANSFORM_PROGRAM write fills one dword, four per slot;
 *   - constants: SET_TRANSFORM_CONSTANT_LOAD selects the register (0..191,
 *     the NV2A index -- D3D's c0 is 96), four floats per register;
 *   - the microcode is decoded by d3d8_vsh_parse() (the same decoder the D3D11
 *     translator uses) and interpreted here.
 *
 * Xbox D3D appends the viewport transform to every vertex shader, so oPos.xy
 * leaves the program in surface pixels and oPos.z in depth-buffer units; w is
 * kept for perspective-correct interpolation.
 *
 * Pixel pipeline: texture stage 0 modulated by oD0 (an approximation of the
 * register combiners, which are not interpreted yet), alpha test, the common
 * blend factors, and a per-surface float depth buffer.
 */
#include "../d3d/d3d8_vsh.h"

#define VP_SLOTS      136
#define VP_CONSTS     192

static uint32_t       s_vp_code[VP_SLOTS][4];
static uint32_t       s_vp_load;          /* slot the next PROGRAM write fills */
static uint32_t       s_vp_start;
static uint32_t       s_vp_mode;          /* 0 fixed, 2 program */
static float          s_vp_const[VP_CONSTS][4];
static uint32_t       s_vp_const_load;
static NV2AVshProgram s_vp_prog;
static int            s_vp_dirty = 1;
static int            s_vp_batch;         /* current batch runs the program */
static int            s_vp_uses_viewport; /* program applies D3D's viewport (reads c[58]) */

/* Last value written to each 3D-class method (0x0000-0x1FFC). */
static uint32_t s_kel[0x800];

/* Render state the pixel pipeline honours. */
static uint32_t s_rs_alpha_test, s_rs_alpha_func = 0x207, s_rs_alpha_ref;
static uint32_t s_rs_blend, s_rs_sfactor = 1, s_rs_dfactor = 0;
static uint32_t s_rs_depth_test, s_rs_depth_func = 0x203, s_rs_depth_mask = 1;
static uint32_t s_rs_color_mask = 0x01010101;

/* Depth buffer: one float per pixel of the surface being drawn. */
#define VP_DEPTH_W 1024
#define VP_DEPTH_H 1024
static float *s_depth;

/* Observe a method. Returns 1 if it was fully consumed here. */
static int vp_method(uint32_t method, uint32_t param)
{
    float f;
    memcpy(&f, &param, 4);

    /* Shadow of every 3D-class register: the GPU backend reads combiner,
     * fog, cull and stencil state straight from here. */
    if (method < 0x2000)
        s_kel[method / 4] = param;

    if (method >= 0x0B00 && method < 0x0B80) {           /* SET_TRANSFORM_PROGRAM */
        uint32_t slot = (method - 0x0B00) / 4;
        if (s_vp_load < VP_SLOTS)
            s_vp_code[s_vp_load][slot % 4] = param;
        if (slot % 4 == 3)
            s_vp_load++;
        s_vp_dirty = 1;
        return 1;
    }
    if (method >= 0x0B80 && method < 0x0C00) {           /* SET_TRANSFORM_CONSTANT */
        uint32_t slot = (method - 0x0B80) / 4;
        if (s_vp_const_load < VP_CONSTS)
            s_vp_const[s_vp_const_load][slot % 4] = f;
        if (slot % 4 == 3)
            s_vp_const_load++;
        return 1;
    }
    /* The viewport methods land in transform-context slots that vertex
     * programs read as constants: scale in c[58] and offset in c[59] (D3D's
     * c-38 / c-37, used by the viewport epilogue it appends to every shader).
     * Not consumed: the fixed-function path records them too. */
    if (method >= 0x0AF0 && method < 0x0B00) {            /* SET_VIEWPORT_SCALE */
        s_vp_const[58][(method - 0x0AF0) / 4] = f;
        return 0;
    }
    if (method >= 0x0A20 && method < 0x0A30) {            /* SET_VIEWPORT_OFFSET */
        s_vp_const[59][(method - 0x0A20) / 4] = f;
        return 0;
    }
    /* Every texture-stage register, all four stages: the GPU backend decodes
     * each stage from these. Not consumed; stage 0 is also decoded below. */
    if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST) {
        s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
        s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
        return 0;
    }
    switch (method) {
    case 0x1E94: s_vp_mode = param & 3; return 1;       /* TRANSFORM_EXECUTION_MODE */
    case 0x1E9C: s_vp_load = param; return 1;            /* TRANSFORM_PROGRAM_LOAD */
    case 0x1EA0: s_vp_start = param; s_vp_dirty = 1; return 1;
    case 0x1EA4: s_vp_const_load = param; return 1;      /* TRANSFORM_CONSTANT_LOAD */
    case 0x0300: s_rs_alpha_test = param; return 1;
    case 0x033C: s_rs_alpha_func = param; return 1;
    case 0x0340: s_rs_alpha_ref = param & 0xFF; return 1;
    case 0x0304: s_rs_blend = param; return 1;
    case 0x0344: s_rs_sfactor = param; return 1;
    case 0x0348: s_rs_dfactor = param; return 1;
    case 0x030C: s_rs_depth_test = param; return 1;
    case 0x0354: s_rs_depth_func = param; return 1;
    case 0x035C: s_rs_depth_mask = param; return 1;
    case 0x0358: s_rs_color_mask = param; return 1;
    case 0x1D94:                                          /* CLEAR_SURFACE: depth part */
        if ((param & 1) && s_depth) {
            size_t i, n = (size_t)VP_DEPTH_W * VP_DEPTH_H;
            for (i = 0; i < n; i++)
                s_depth[i] = 3.4e38f;
        }
        return 0;                                         /* colour clear stays below */
    default:
        return 0;
    }
}

/* ── Interpreter ───────────────────────────────────────────────────────── */

typedef struct { float v[4]; } Vec4;

static void vp_fetch(const NV2AVshSrcOperand *s, const float in[16][4],
                     const float r[13][4], int a0, float out[4])
{
    const float *src;
    int idx = s->reg_index;

    switch (s->reg_type) {
    case NV2A_VSH_REG_TEMP:
        src = r[idx < 13 ? idx : 12];
        break;
    case NV2A_VSH_REG_INPUT:
        src = in[idx & 15];
        break;
    default:
        if (s->rel_addr)
            idx += a0;
        if (idx < 0) idx = 0;
        if (idx >= VP_CONSTS) idx = VP_CONSTS - 1;
        src = s_vp_const[idx];
        break;
    }
    out[0] = src[s->swizzle.x & 3];
    out[1] = src[s->swizzle.y & 3];
    out[2] = src[s->swizzle.z & 3];
    out[3] = src[s->swizzle.w & 3];
    if (s->negate) {
        out[0] = -out[0]; out[1] = -out[1]; out[2] = -out[2]; out[3] = -out[3];
    }
}

static void vp_store(const NV2AVshDstOperand *d, const float val[4],
                     float r[13][4], float o[13][4])
{
    int c;
    /* Low nibble: temp write mask; high nibble: output write mask (x = bit 3). */
    for (c = 0; c < 4; c++) {
        if ((d->write_mask & (8 >> c)) && d->temp_reg >= 0 && d->temp_reg < 13) {
            r[d->temp_reg][c] = val[c];
            if (d->temp_reg == 12)
                o[NV2A_VSH_OUT_POS][c] = val[c];
        }
        if (((d->write_mask >> 4) & (8 >> c))
                && d->output_reg != NV2A_VSH_OUT_NONE && d->output_reg < 13) {
            o[d->output_reg][c] = val[c];
            /* R12 is oPos: a later instruction reading R12 sees it. */
            if (d->output_reg == NV2A_VSH_OUT_POS)
                r[12][c] = val[c];
        }
    }
}

/* NV2A vertex program decoder.
 *
 * d3d8_vsh_parse() decodes real title microcode as all-NOPs (a D3D pass-through
 * program whose MAC field clearly reads MOV came out as mac 0), so this uses
 * the NV2A field layout directly -- the same table xemu decodes with. Fields
 * are (dword, first bit, width) within the four dwords of a slot. */
static uint32_t vp_fld(const uint32_t *t, int dw, int bit, int len)
{
    return (t[dw] >> bit) & ((1u << len) - 1u);
}

static void vp_decode_src(const uint32_t *t, int which, NV2AVshSrcOperand *s)
{
    uint32_t mux, reg, neg, sx, sy, sz, sw;
    if (which == 0) {        /* A */
        neg = vp_fld(t, 1, 8, 1);
        sx = vp_fld(t, 1, 6, 2); sy = vp_fld(t, 1, 4, 2); sz = vp_fld(t, 1, 2, 2); sw = vp_fld(t, 1, 0, 2);
        reg = vp_fld(t, 2, 28, 4); mux = vp_fld(t, 2, 26, 2);
    } else if (which == 1) { /* B */
        neg = vp_fld(t, 2, 25, 1);
        sx = vp_fld(t, 2, 23, 2); sy = vp_fld(t, 2, 21, 2); sz = vp_fld(t, 2, 19, 2); sw = vp_fld(t, 2, 17, 2);
        reg = vp_fld(t, 2, 13, 4); mux = vp_fld(t, 2, 11, 2);
    } else {                 /* C */
        neg = vp_fld(t, 2, 10, 1);
        sx = vp_fld(t, 2, 8, 2); sy = vp_fld(t, 2, 6, 2); sz = vp_fld(t, 2, 4, 2); sw = vp_fld(t, 2, 2, 2);
        reg = (vp_fld(t, 2, 0, 2) << 2) | vp_fld(t, 3, 30, 2);
        mux = vp_fld(t, 3, 28, 2);
    }
    s->negate = (int)neg;
    s->swizzle.x = (uint8_t)sx; s->swizzle.y = (uint8_t)sy;
    s->swizzle.z = (uint8_t)sz; s->swizzle.w = (uint8_t)sw;
    s->rel_addr = 0;
    switch (mux) {
    case 2:  s->reg_type = NV2A_VSH_REG_INPUT; s->reg_index = (int)vp_fld(t, 1, 9, 4); break;
    case 3:  s->reg_type = NV2A_VSH_REG_CONST; s->reg_index = (int)vp_fld(t, 1, 13, 8);
             s->rel_addr = (int)vp_fld(t, 3, 1, 1); break;
    default: s->reg_type = NV2A_VSH_REG_TEMP;  s->reg_index = (int)reg; break;
    }
}

static int vp_decode_program(const uint32_t (*code)[4], int max, NV2AVshProgram *p)
{
    int i;
    memset(p, 0, sizeof *p);
    for (i = 0; i < max && i < NV2A_VS_MAX_INSTRUCTIONS; i++) {
        const uint32_t *t = code[i];
        NV2AVshInstruction *q = &p->insns[i];
        uint32_t mac_mask = vp_fld(t, 3, 24, 4), ilu_mask = vp_fld(t, 3, 16, 4);
        uint32_t o_mask = vp_fld(t, 3, 12, 4), orb = vp_fld(t, 3, 11, 1);
        uint32_t o_addr = vp_fld(t, 3, 3, 8), o_mux = vp_fld(t, 3, 2, 1);
        int out_r = (int)vp_fld(t, 3, 20, 4);

        q->mac_op = (NV2AVshMacOp)vp_fld(t, 1, 21, 4);
        q->ilu_op = (NV2AVshIluOp)vp_fld(t, 1, 25, 3);
        vp_decode_src(t, 0, &q->mac_src[0]);
        vp_decode_src(t, 1, &q->mac_src[1]);
        vp_decode_src(t, 2, &q->mac_src[2]);
        q->ilu_src = q->mac_src[2];

        q->mac_dst.temp_reg = mac_mask ? out_r : -1;
        q->mac_dst.output_reg = NV2A_VSH_OUT_NONE;
        q->mac_dst.write_mask = (uint8_t)mac_mask;
        /* A paired ILU op (MAC active in the same slot) can only write R1,
         * whatever the MAC writes -- D3D's viewport epilogue pairs
         * `mul oPos.xyz` with `rcc r1.x, r12.w` and then reads r1. */
        q->ilu_dst.temp_reg = ilu_mask ? ((q->mac_op != NV2A_VSH_MAC_NOP) ? 1 : out_r) : -1;
        q->ilu_dst.output_reg = NV2A_VSH_OUT_NONE;
        q->ilu_dst.write_mask = (uint8_t)ilu_mask;
        /* The output write (o[] when ORB=1) comes from the unit OUT_MUX picks.
         * Constant writes (ORB=0) are not modelled. */
        if (o_mask && orb) {
            NV2AVshDstOperand *d = o_mux ? &q->ilu_dst : &q->mac_dst;
            d->output_reg = (NV2AVshOutputReg)(o_addr < 13 ? o_addr : NV2A_VSH_OUT_NONE);
            /* The o[] write has its own mask; temp writes keep theirs. Stored
             * in the high nibble so vp_store can apply each separately. */
            d->write_mask = (uint8_t)(d->write_mask | (o_mask << 4));
        }
        q->is_final = (int)vp_fld(t, 3, 0, 1);
        p->length = i + 1;
        if (q->is_final)
            break;
    }
    return p->length;
}

/* Run the current program for one vertex. `o` receives outputs indexed by
 * NV2AVshOutputReg (0 = oPos, 3 = oD0, 9 = oT0 ...). */
/* Decode the current program once after it changes. */
static void vp_prepare(void)
{
    if (s_vp_dirty) {
        uint32_t start = s_vp_start < VP_SLOTS ? s_vp_start : 0;
        vp_decode_program((const uint32_t (*)[4])&s_vp_code[start], (int)(VP_SLOTS - start), &s_vp_prog);
        {
            int k, j;
            s_vp_uses_viewport = 0;
            for (k = 0; k < s_vp_prog.length; k++)
                for (j = 0; j < 3; j++)
                    if (s_vp_prog.insns[k].mac_src[j].reg_type == NV2A_VSH_REG_CONST
                            && s_vp_prog.insns[k].mac_src[j].reg_index == 58)
                        s_vp_uses_viewport = 1;
        }
        s_vp_dirty = 0;
        {
            static int shown;
            int k;
            if (s_vp_prog.length > 2 && shown++ < 2)
                for (k = 0; k < s_vp_prog.length && k < 40; k++) {
                    const NV2AVshInstruction *q = &s_vp_prog.insns[k];
                    fprintf(stderr, "  [VP] insn %d: %08X %08X %08X %08X mac %d dst t%d o%d m%X ilu %d dst t%d o%d m%X final %d\n",
                            k, s_vp_code[start + k][0], s_vp_code[start + k][1],
                            s_vp_code[start + k][2], s_vp_code[start + k][3],
                            q->mac_op, q->mac_dst.temp_reg, q->mac_dst.output_reg, q->mac_dst.write_mask,
                            q->ilu_op, q->ilu_dst.temp_reg, q->ilu_dst.output_reg, q->ilu_dst.write_mask,
                            q->is_final);
                }
        }
    }
}

static void vp_run(const float in[16][4], float o[13][4])
{
    float r[13][4];
    int a0 = 0, i, c;

    vp_prepare();
    memset(r, 0, sizeof r);
    memset(o, 0, sizeof(float) * 13 * 4);
    o[3][0] = o[3][1] = o[3][2] = o[3][3] = 1.0f;         /* oD0 defaults white */
    o[4][3] = 1.0f;                                       /* oD1 (0,0,0,1) */
    for (i = 9; i < 13; i++)
        o[i][3] = 1.0f;                                   /* oT* (0,0,0,1): q = 1 */

    for (i = 0; i < s_vp_prog.length; i++) {
        const NV2AVshInstruction *ins = &s_vp_prog.insns[i];
        float A[4] = {0}, B[4] = {0}, C[4] = {0}, m[4] = {0}, l[4] = {0};
        int has_mac = ins->mac_op != NV2A_VSH_MAC_NOP;
        int has_ilu = ins->ilu_op != NV2A_VSH_ILU_NOP;

        /* MAC and ILU read the same pre-instruction state. */
        if (has_mac) {
            vp_fetch(&ins->mac_src[0], in, r, a0, A);
            vp_fetch(&ins->mac_src[1], in, r, a0, B);
            vp_fetch(&ins->mac_src[2], in, r, a0, C);
            switch (ins->mac_op) {
            case NV2A_VSH_MAC_MOV: for (c = 0; c < 4; c++) m[c] = A[c]; break;
            case NV2A_VSH_MAC_MUL: for (c = 0; c < 4; c++) m[c] = A[c] * B[c]; break;
            case NV2A_VSH_MAC_ADD: for (c = 0; c < 4; c++) m[c] = A[c] + C[c]; break;
            case NV2A_VSH_MAC_MAD: for (c = 0; c < 4; c++) m[c] = A[c] * B[c] + C[c]; break;
            case NV2A_VSH_MAC_DP3:
                m[0] = m[1] = m[2] = m[3] = A[0]*B[0] + A[1]*B[1] + A[2]*B[2]; break;
            case NV2A_VSH_MAC_DPH:
                m[0] = m[1] = m[2] = m[3] = A[0]*B[0] + A[1]*B[1] + A[2]*B[2] + B[3]; break;
            case NV2A_VSH_MAC_DP4:
                m[0] = m[1] = m[2] = m[3] = A[0]*B[0] + A[1]*B[1] + A[2]*B[2] + A[3]*B[3]; break;
            case NV2A_VSH_MAC_DST:
                m[0] = 1.0f; m[1] = A[1] * B[1]; m[2] = A[2]; m[3] = B[3]; break;
            case NV2A_VSH_MAC_MIN: for (c = 0; c < 4; c++) m[c] = A[c] < B[c] ? A[c] : B[c]; break;
            case NV2A_VSH_MAC_MAX: for (c = 0; c < 4; c++) m[c] = A[c] > B[c] ? A[c] : B[c]; break;
            case NV2A_VSH_MAC_SLT: for (c = 0; c < 4; c++) m[c] = A[c] < B[c] ? 1.0f : 0.0f; break;
            case NV2A_VSH_MAC_SGE: for (c = 0; c < 4; c++) m[c] = A[c] >= B[c] ? 1.0f : 0.0f; break;
            case NV2A_VSH_MAC_ARL: m[0] = m[1] = m[2] = m[3] = 0; break;
            default: for (c = 0; c < 4; c++) m[c] = 0.0f; break;
            }
        }
        if (has_ilu) {
            float x;
            vp_fetch(&ins->ilu_src, in, r, a0, C);
            x = C[0];
            switch (ins->ilu_op) {
            case NV2A_VSH_ILU_MOV: for (c = 0; c < 4; c++) l[c] = C[c]; break;
            case NV2A_VSH_ILU_RCP:
                l[0] = l[1] = l[2] = l[3] = (x == 0.0f) ? 3.4e38f : 1.0f / x; break;
            case NV2A_VSH_ILU_RCC: {
                float v = (x == 0.0f) ? 1.884467e19f : 1.0f / x;
                if (v > 0) { if (v < 5.42101e-20f) v = 5.42101e-20f; if (v > 1.884467e19f) v = 1.884467e19f; }
                else       { if (v > -5.42101e-20f) v = -5.42101e-20f; if (v < -1.884467e19f) v = -1.884467e19f; }
                l[0] = l[1] = l[2] = l[3] = v;
                break;
            }
            case NV2A_VSH_ILU_RSQ:
                l[0] = l[1] = l[2] = l[3] = (x == 0.0f) ? 3.4e38f : 1.0f / sqrtf(fabsf(x)); break;
            case NV2A_VSH_ILU_EXP:
                l[0] = powf(2.0f, floorf(x)); l[1] = x - floorf(x);
                l[2] = powf(2.0f, x); l[3] = 1.0f; break;
            case NV2A_VSH_ILU_LOG: {
                float ax = fabsf(x);
                if (ax == 0.0f) { l[0] = -3.4e38f; l[1] = 1.0f; l[2] = -3.4e38f; }
                else {
                    int e; float mnt = frexpf(ax, &e);
                    l[0] = (float)(e - 1); l[1] = mnt * 2.0f; l[2] = log2f(ax);
                }
                l[3] = 1.0f;
                break;
            }
            case NV2A_VSH_ILU_LIT: {
                float nl = C[0] > 0 ? C[0] : 0, nh = C[1] > 0 ? C[1] : 0;
                float pw = C[3]; if (pw > 127.9961f) pw = 127.9961f; if (pw < -127.9961f) pw = -127.9961f;
                l[0] = 1.0f; l[1] = nl; l[2] = (C[0] > 0) ? powf(nh, pw) : 0.0f; l[3] = 1.0f;
                break;
            }
            default: for (c = 0; c < 4; c++) l[c] = 0.0f; break;
            }
        }
        if (has_mac) {
            if (ins->mac_op == NV2A_VSH_MAC_ARL)
                a0 = (int)floorf(A[0] + 0.001f);
            else
                vp_store(&ins->mac_dst, m, r, o);
        }
        if (has_ilu)
            vp_store(&ins->ilu_dst, l, r, o);
        {
            static int traced_vertices;
            if (s_vp_prog.length > 2 && traced_vertices < 1) {
                fprintf(stderr, "  [VPT] %2d mac %d A(%g %g %g %g) B(%g %g %g %g) C(%g %g %g %g) -> m(%g %g %g %g) | ilu %d -> l(%g %g %g %g) | r12(%g %g %g %g)\n",
                        i, ins->mac_op,
                        has_mac ? A[0] : 0, has_mac ? A[1] : 0, has_mac ? A[2] : 0, has_mac ? A[3] : 0,
                        has_mac ? B[0] : 0, has_mac ? B[1] : 0, has_mac ? B[2] : 0, has_mac ? B[3] : 0,
                        C[0], C[1], C[2], C[3],
                        has_mac ? m[0] : 0, has_mac ? m[1] : 0, has_mac ? m[2] : 0, has_mac ? m[3] : 0,
                        ins->ilu_op, has_ilu ? l[0] : 0, has_ilu ? l[1] : 0, has_ilu ? l[2] : 0, has_ilu ? l[3] : 0,
                        r[12][0], r[12][1], r[12][2], r[12][3]);
                if (ins->is_final || i == s_vp_prog.length - 1)
                    traced_vertices++;
            }
        }
        if (ins->is_final)
            break;
    }
    /* R12 is oPos. */
    if (o[0][0] == 0 && o[0][1] == 0 && o[0][2] == 0 && o[0][3] == 0)
        memcpy(o[0], r[12], sizeof(float) * 4);
}

/* ── Vertex processing with a small per-batch cache ────────────────────── */

typedef struct {
    float x, y, z, rhw;     /* screen position, 1/w */
    float col[4];           /* oD0 */
    float uv[2];            /* oT0 */
    float spec[4];          /* oD1 */
    float tex[4][4];        /* oT0..oT3 */
    float fog;              /* oFog.x */
    uint32_t tag;           /* index + 1 in this batch, 0 = empty */
} VpVert;

#define VP_CACHE 1024
static VpVert   s_vp_cache[VP_CACHE];
static uint32_t s_vp_gen;

static void vp_batch_begin(void)
{
    s_vp_gen++;
    if (!s_vp_gen) {
        memset(s_vp_cache, 0, sizeof s_vp_cache);
        s_vp_gen = 1;
    }
}

static uint32_t s_vp_cache_gen[VP_CACHE];

static const VpVert *vp_vertex(uint32_t index)
{
    VpVert *v = &s_vp_cache[index % VP_CACHE];
    float in[16][4], o[13][4], w;
    int a;

    if (s_vp_cache_gen[index % VP_CACHE] == s_vp_gen && v->tag == index + 1)
        return v;
    for (a = 0; a < 16; a++)
        fetch_attr(&s_gpu.attr[a], index, in[a]);
    vp_run(in, o);

    w = o[0][3];
    if (fabsf(w) < 1e-8f) w = 1.0f;
    v->x = o[0][0];
    v->y = o[0][1];
    v->z = o[0][2];
    v->rhw = 1.0f / w;
    for (a = 0; a < 4; a++) {
        float c = o[3][a];
        v->col[a] = c < 0 ? 0 : (c > 1 ? 1 : c);
    }
    v->uv[0] = o[9][0];
    v->uv[1] = o[9][1];
    for (a = 0; a < 4; a++) {
        float c = o[4][a];
        v->spec[a] = c < 0 ? 0 : (c > 1 ? 1 : c);
        memcpy(v->tex[a], o[9 + a], sizeof(float) * 4);
    }
    {
        static int shown;
        if (s_vp_prog.length > 2 && shown++ < 12)
            fprintf(stderr, "  [VP] len %d start %u  v0 (%.3f %.3f %.3f %.3f) -> pos (%.2f %.2f %.4f w %.3f) "
                            "col (%.2f %.2f %.2f %.2f) uv (%.3f %.3f)\n",
                    s_vp_prog.length, s_vp_start, in[0][0], in[0][1], in[0][2], in[0][3],
                    o[0][0], o[0][1], o[0][2], o[0][3],
                    o[3][0], o[3][1], o[3][2], o[3][3], o[9][0], o[9][1]);
    }
    v->fog = o[5][0];
    v->tag = index + 1;
    s_vp_cache_gen[index % VP_CACHE] = s_vp_gen;
    return v;
}

/* ── Pixel pipeline ─────────────────────────────────────────────────────── */

static int vp_compare(uint32_t func, float a, float b)
{
    switch (func & 7) {           /* GL_NEVER=0x200 ... GL_ALWAYS=0x207 */
    case 0: return 0;
    case 1: return a <  b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a >  b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return 1;
    }
}

static float vp_factor(uint32_t f, int ch, const float s[4], const float d[4])
{
    switch (f) {
    case 0x0000: return 0.0f;
    case 0x0001: return 1.0f;
    case 0x0300: return s[ch];
    case 0x0301: return 1.0f - s[ch];
    case 0x0302: return s[3];
    case 0x0303: return 1.0f - s[3];
    case 0x0304: return d[3];
    case 0x0305: return 1.0f - d[3];
    case 0x0306: return d[ch];
    case 0x0307: return 1.0f - d[ch];
    case 0x0308: { float a = s[3], b = 1.0f - d[3]; return ch == 3 ? 1.0f : (a < b ? a : b); }
    default:     return 1.0f;
    }
}

static void vp_unpack(uint32_t argb, float c[4])
{
    c[0] = ((argb >> 16) & 0xFF) / 255.0f;
    c[1] = ((argb >> 8) & 0xFF) / 255.0f;
    c[2] = (argb & 0xFF) / 255.0f;
    c[3] = (argb >> 24) / 255.0f;
}

static uint32_t vp_pack(const float c[4])
{
    int k; uint32_t b[4];
    for (k = 0; k < 4; k++) {
        float v = c[k] < 0 ? 0 : (c[k] > 1 ? 1 : c[k]);
        b[k] = (uint32_t)(v * 255.0f + 0.5f);
    }
    return (b[3] << 24) | (b[0] << 16) | (b[1] << 8) | b[2];
}

/* ── Diagnostics (RECOMP_VP_STATS) ─────────────────────────────────────── */
static int      s_st_on = -1;
static int      s_st_noalpha;   /* RECOMP_VP_NOALPHA: diagnostic */
static uint64_t s_st_tris, s_st_px, s_st_depth_rej, s_st_alpha_rej;
static uint64_t s_st_fmt_ok[128], s_st_fmt_fail[128];
static uint32_t s_st_surf[8], s_st_surf_n[8];
static uint64_t s_st_mode[4];
static uint64_t s_st_prim[16], s_st_degen, s_st_off;

static void vp_stats_report(void)
{
    static DWORD last;
    int i;
    if (s_st_on < 0) {
        s_st_on = getenv("RECOMP_VP_STATS") != NULL;
        s_st_noalpha = getenv("RECOMP_VP_NOALPHA") != NULL;
    }
    if (!s_st_on || GetTickCount() - last < 5000)
        return;
    last = GetTickCount();
    fprintf(stderr, "  [VPSTAT] tris %llu px %llu depth-rej %llu alpha-rej %llu | batches prog %llu fixed %llu skipped %llu\n",
            s_st_tris, s_st_px, s_st_depth_rej, s_st_alpha_rej, s_st_mode[2], s_st_mode[0], s_st_mode[3]);
    fprintf(stderr, "  [VPSTAT] prims:");
    for (i = 0; i < 16; i++) if (s_st_prim[i]) fprintf(stderr, " %d=%llu", i, s_st_prim[i]);
    fprintf(stderr, " | degenerate %llu offscreen %llu\n", s_st_degen, s_st_off);
    memset(s_st_prim, 0, sizeof s_st_prim); s_st_degen = s_st_off = 0;
    fprintf(stderr, "  [VPSTAT] tex fmt ok/fail:");
    for (i = 0; i < 128; i++)
        if (s_st_fmt_ok[i] || s_st_fmt_fail[i])
            fprintf(stderr, " %02X=%llu/%llu", i, s_st_fmt_ok[i], s_st_fmt_fail[i]);
    fprintf(stderr, "\n  [VPSTAT] surfaces:");
    for (i = 0; i < 8; i++)
        if (s_st_surf[i])
            fprintf(stderr, " %08X=%u", s_st_surf[i], s_st_surf_n[i]);
    fprintf(stderr, "\n");
    memset(s_st_fmt_ok, 0, sizeof s_st_fmt_ok); memset(s_st_fmt_fail, 0, sizeof s_st_fmt_fail);
    memset(s_st_surf, 0, sizeof s_st_surf); memset(s_st_surf_n, 0, sizeof s_st_surf_n);
    memset(s_st_mode, 0, sizeof s_st_mode);
    s_st_tris = s_st_px = s_st_depth_rej = s_st_alpha_rej = 0;
}

static void vp_stats_surface(uint32_t off)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (s_st_surf[i] == off || !s_st_surf[i]) {
            s_st_surf[i] = off;
            s_st_surf_n[i]++;
            return;
        }
    }
}

static void vp_raster_triangle(const VpVert *a, const VpVert *b, const VpVert *c)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area, minx, maxx, miny, maxy;
    int x0, x1, y0, y1, x, y;
    int textured = s_gpu.tex.valid;
    int swz = textured && (d3d8_format_is_swizzled(s_gpu.tex.color)
                           || d3d8_format_dxt_block_bytes(s_gpu.tex.color));

    if (bpp != 4 && bpp != 2)
        return;
    /* Behind the camera (clip w <= 0): its "screen" position is a reflection,
     * and drawing it smears a huge triangle across the frame. Not clipped
     * yet -- dropped. */
    if (a->rhw <= 0.0f || b->rhw <= 0.0f || c->rhw <= 0.0f) {
        s_st_off++;
        return;
    }
    area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (fabsf(area) < 1e-6f || area != area) {
        s_st_degen++;
        vp_stats_report();
        return;
    }
    if (!surface_begin_batch(mem))
        return;
    if (!s_depth) {
        size_t i, n = (size_t)VP_DEPTH_W * VP_DEPTH_H;
        s_depth = (float *)malloc(n * sizeof(float));
        if (s_depth)
            for (i = 0; i < n; i++)
                s_depth[i] = 3.4e38f;
    }

    minx = fminf(a->x, fminf(b->x, c->x));  maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    miny = fminf(a->y, fminf(b->y, c->y));  maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    x0 = (int)floorf(minx); x1 = (int)ceilf(maxx);
    y0 = (int)floorf(miny); y1 = (int)ceilf(maxy);
    if (x0 < (int)s_gpu.clip_x) x0 = (int)s_gpu.clip_x;
    if (y0 < (int)s_gpu.clip_y) y0 = (int)s_gpu.clip_y;
    if (x1 > (int)(s_gpu.clip_x + s_gpu.clip_w)) x1 = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (y1 > (int)(s_gpu.clip_y + s_gpu.clip_h)) y1 = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (x1 > VP_DEPTH_W) x1 = VP_DEPTH_W;
    if (y1 > VP_DEPTH_H) y1 = VP_DEPTH_H;
    if (x0 >= x1 || y0 >= y1) {
        s_st_off++;
        if (s_st_off < 4 && s_st_on > 0)
            fprintf(stderr, "  [VPSTAT] offscreen tri (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) clip %u,%u %ux%u\n",
                    a->x, a->y, b->x, b->y, c->x, c->y, s_gpu.clip_x, s_gpu.clip_y, s_gpu.clip_w, s_gpu.clip_h);
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = y0; y < y1; y++) {
        uint8_t *row = s_surface + (size_t)y * s_gpu.pitch;
        float py = (float)y + 0.5f;
        for (x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) / area;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) / area;
            float w2 = 1.0f - w0 - w1;
            float z, pw, col[4], dst[4], out[4];
            int k;

            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            z = w0 * a->z + w1 * b->z + w2 * c->z;
            if (s_rs_depth_test) {
                float *dp = &s_depth[(size_t)y * VP_DEPTH_W + x];
                if (!vp_compare(s_rs_depth_func, z, *dp)) {
                    s_st_depth_rej++;
                    continue;
                }
                if (s_rs_depth_mask)
                    *dp = z;
            }
            /* perspective-correct weights */
            pw = w0 * a->rhw + w1 * b->rhw + w2 * c->rhw;
            if (pw == 0.0f) pw = 1.0f;
            {
                float p0 = w0 * a->rhw / pw, p1 = w1 * b->rhw / pw, p2 = w2 * c->rhw / pw;
                for (k = 0; k < 4; k++)
                    col[k] = p0 * a->col[k] + p1 * b->col[k] + p2 * c->col[k];
                if (textured) {
                    float u = p0 * a->uv[0] + p1 * b->uv[0] + p2 * c->uv[0];
                    float v = p0 * a->uv[1] + p1 * b->uv[1] + p2 * c->uv[1];
                    uint32_t texel;
                    if (swz) { u *= (float)s_gpu.tex.width; v *= (float)s_gpu.tex.height; }
                    if (u < 0) u = 0;
                    if (v < 0) v = 0;
                    int ok = sample_texture((uint32_t)u, (uint32_t)v, &texel);
                    if (ok) s_st_fmt_ok[s_gpu.tex.color & 127]++; else s_st_fmt_fail[s_gpu.tex.color & 127]++;
                    if (ok) {
                        float t[4];
                        vp_unpack(texel, t);
                        for (k = 0; k < 4; k++)
                            col[k] *= t[k];
                    }
                }
            }
            if (s_rs_alpha_test && !s_st_noalpha
                    && !vp_compare(s_rs_alpha_func, col[3] * 255.0f, (float)s_rs_alpha_ref)) {
                s_st_alpha_rej++;
                continue;
            }

            if (bpp == 4) {
                uint32_t *pp = &((uint32_t *)row)[x];
                vp_unpack(*pp, dst);
                if (s_rs_blend) {
                    for (k = 0; k < 4; k++)
                        out[k] = col[k] * vp_factor(s_rs_sfactor, k, col, dst)
                               + dst[k] * vp_factor(s_rs_dfactor, k, col, dst);
                } else {
                    memcpy(out, col, sizeof out);
                }
                *pp = vp_pack(out);
            } else {
                uint16_t *pp = &((uint16_t *)row)[x];
                uint32_t t = *pp;
                dst[0] = ((t >> 11) & 0x1F) / 31.0f;
                dst[1] = ((t >> 5) & 0x3F) / 63.0f;
                dst[2] = (t & 0x1F) / 31.0f;
                dst[3] = 1.0f;
                if (s_rs_blend) {
                    for (k = 0; k < 4; k++)
                        out[k] = col[k] * vp_factor(s_rs_sfactor, k, col, dst)
                               + dst[k] * vp_factor(s_rs_dfactor, k, col, dst);
                } else {
                    memcpy(out, col, sizeof out);
                }
                {
                    uint32_t p = vp_pack(out);
                    *pp = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
                }
            }
            s_gpu.pixels++;
            s_st_px++;
        }
    }
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
    s_st_tris++;
    vp_stats_surface(s_gpu.color_offset);
    vp_stats_report();
}

static void vp_raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2)
{
    const VpVert *a = vp_vertex(i0);
    VpVert va = *a;
    const VpVert *b = vp_vertex(i1);
    VpVert vb = *b;
    const VpVert *c = vp_vertex(i2);
    VpVert vc = *c;
    vp_raster_triangle(&va, &vb, &vc);
}
