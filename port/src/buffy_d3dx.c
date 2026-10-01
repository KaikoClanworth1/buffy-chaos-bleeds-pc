/**
 * The game's D3DX math, native.
 *
 * D3DX is the helper library linked into the game beside Xbox D3D: matrix
 * and vector math the engine calls thousands of times a frame (every
 * entity's transform, the camera, skinning). Translated from the Xbox's x87
 * code it was a few percent of the game thread; here it is plain C the
 * compiler vectorises. Each is listed in manual_functions.json. All are
 * stdcall and return their output pointer, which may be one of the inputs:
 * the result is built in a local first.
 *
 * Matrices are D3D's: 4x4 floats, row-major, vectors multiply on the left
 * (v' = v * M).
 */
#include <stdint.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"

static const float *in_f(uint32_t va) { return (const float *)XBOX_PTR(va); }

static void out_f(uint32_t va, const float *v, int n)
{
    memcpy((void *)XBOX_PTR(va), v, (size_t)n * sizeof(float));
}

/* D3DXMATRIX *D3DXMatrixMultiply(D3DXMATRIX *out, const D3DXMATRIX *a,
 * const D3DXMATRIX *b) -- 0x00147CA9: out = a * b. */
void D3DXMatrixMultiply_00147CA9(void)
{
    uint32_t out = MEM32(g_esp + 4);
    const float *a = in_f(MEM32(g_esp + 8)), *b = in_f(MEM32(g_esp + 12));
    float r[16];
    int i;
    for (i = 0; i < 4; i++) {
        const float a0 = a[i * 4], a1 = a[i * 4 + 1], a2 = a[i * 4 + 2], a3 = a[i * 4 + 3];
        r[i * 4 + 0] = a0 * b[0] + a1 * b[4] + a2 * b[8]  + a3 * b[12];
        r[i * 4 + 1] = a0 * b[1] + a1 * b[5] + a2 * b[9]  + a3 * b[13];
        r[i * 4 + 2] = a0 * b[2] + a1 * b[6] + a2 * b[10] + a3 * b[14];
        r[i * 4 + 3] = a0 * b[3] + a1 * b[7] + a2 * b[11] + a3 * b[15];
    }
    out_f(out, r, 16);
    g_eax = out;
    g_esp += 16;                                         /* ret 12 */
}

/* D3DXMATRIX *D3DXMatrixTranspose(D3DXMATRIX *out, const D3DXMATRIX *m) --
 * 0x00147A70. */
void D3DXMatrixTranspose_00147A70(void)
{
    uint32_t out = MEM32(g_esp + 4);
    const float *m = in_f(MEM32(g_esp + 8));
    float r[16];
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[i * 4 + j] = m[j * 4 + i];
    out_f(out, r, 16);
    g_eax = out;
    g_esp += 12;                                         /* ret 8 */
}

/* D3DXVECTOR4 *D3DXVec4Transform(D3DXVECTOR4 *out, const D3DXVECTOR4 *v,
 * const D3DXMATRIX *m) -- 0x001479D7: out = v * m. */
void D3DXVec4Transform_001479D7(void)
{
    uint32_t out = MEM32(g_esp + 4);
    const float *v = in_f(MEM32(g_esp + 8)), *m = in_f(MEM32(g_esp + 12));
    float r[4];
    int j;
    for (j = 0; j < 4; j++)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    out_f(out, r, 4);
    g_eax = out;
    g_esp += 16;                                         /* ret 12 */
}

/* D3DXVECTOR4 *D3DXVec3Transform(D3DXVECTOR4 *out, const D3DXVECTOR3 *v,
 * const D3DXMATRIX *m) -- 0x001478ED: out = (v, 1) * m. */
void D3DXVec3Transform_001478ED(void)
{
    uint32_t out = MEM32(g_esp + 4);
    const float *v = in_f(MEM32(g_esp + 8)), *m = in_f(MEM32(g_esp + 12));
    float r[4];
    int j;
    for (j = 0; j < 4; j++)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + m[12 + j];
    out_f(out, r, 4);
    g_eax = out;
    g_esp += 16;                                         /* ret 12 */
}

/* D3DXVECTOR3 *D3DXVec3TransformNormal(D3DXVECTOR3 *out, const D3DXVECTOR3 *v,
 * const D3DXMATRIX *m) -- 0x00147978: out = (v, 0) * m. */
void D3DXVec3TransformNormal_00147978(void)
{
    uint32_t out = MEM32(g_esp + 4);
    const float *v = in_f(MEM32(g_esp + 8)), *m = in_f(MEM32(g_esp + 12));
    float r[3];
    int j;
    for (j = 0; j < 3; j++)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j];
    out_f(out, r, 3);
    g_eax = out;
    g_esp += 16;                                         /* ret 12 */
}
