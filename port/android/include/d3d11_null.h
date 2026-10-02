/* Direct3D 11 with no device, for the renderer's raw D3D11 paths on Android
 * (xboxrecomp/src/kernel/nv2a_pb_d3d11.inc).
 *
 * The renderer draws through the GPU layer (gpu.h, Vulkan here). What raw
 * Direct3D 11 is left in it -- the old emulated path, debug tracers -- runs
 * only with a D3D11 device (s_dev / s_ctx), which Android never has. So the
 * types are opaque, the descriptions have Direct3D's fields, the enums its
 * values (gpu.h's enums share them), and every call fails or does nothing.
 *
 * Windowing (win32_window_null.h) is the same idea for the window code. */
#pragma once
#include <windows.h>
#include "win32_window_null.h"

typedef uint8_t UINT8;
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Resource ID3D11Resource;
typedef struct ID3D11Texture2D ID3D11Texture2D;
typedef struct ID3D11Buffer ID3D11Buffer;
typedef struct ID3D11ShaderResourceView ID3D11ShaderResourceView;
typedef struct ID3D11RenderTargetView ID3D11RenderTargetView;
typedef struct ID3D11DepthStencilView ID3D11DepthStencilView;
typedef struct ID3D11VertexShader ID3D11VertexShader;
typedef struct ID3D11PixelShader ID3D11PixelShader;
typedef struct ID3D11InputLayout ID3D11InputLayout;
typedef struct ID3D11SamplerState ID3D11SamplerState;
typedef struct ID3D11BlendState ID3D11BlendState;
typedef struct ID3D11DepthStencilState ID3D11DepthStencilState;
typedef struct ID3D11RasterizerState ID3D11RasterizerState;
typedef struct IDXGISwapChain IDXGISwapChain;
typedef struct ID3D10Blob ID3D10Blob, ID3DBlob;

static const GUID IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

/* ── enums (Direct3D's values) ── */
typedef enum {
    DXGI_FORMAT_UNKNOWN = 0, DXGI_FORMAT_R32G32B32A32_FLOAT = 2, DXGI_FORMAT_R32G32B32_FLOAT = 6,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10, DXGI_FORMAT_R32G32_FLOAT = 16, DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R32_FLOAT = 41, DXGI_FORMAT_D32_FLOAT = 40, DXGI_FORMAT_R32_UINT = 42,
    DXGI_FORMAT_D24_UNORM_S8_UINT = 45, DXGI_FORMAT_R16_UINT = 57, DXGI_FORMAT_B5G6R5_UNORM = 85,
    DXGI_FORMAT_B5G5R5A1_UNORM = 86, DXGI_FORMAT_B8G8R8A8_UNORM = 87, DXGI_FORMAT_B8G8R8X8_UNORM = 88,
    DXGI_FORMAT_BC1_UNORM = 71, DXGI_FORMAT_BC2_UNORM = 74, DXGI_FORMAT_BC3_UNORM = 77,
    DXGI_FORMAT_B4G4R4A4_UNORM = 115
} DXGI_FORMAT;
typedef enum { D3D11_USAGE_DEFAULT = 0, D3D11_USAGE_IMMUTABLE = 1, D3D11_USAGE_DYNAMIC = 2, D3D11_USAGE_STAGING = 3 } D3D11_USAGE;
enum {
    D3D11_BIND_VERTEX_BUFFER = 0x1, D3D11_BIND_INDEX_BUFFER = 0x2, D3D11_BIND_CONSTANT_BUFFER = 0x4,
    D3D11_BIND_SHADER_RESOURCE = 0x8, D3D11_BIND_RENDER_TARGET = 0x20, D3D11_BIND_DEPTH_STENCIL = 0x40
};
enum { D3D11_CPU_ACCESS_WRITE = 0x10000, D3D11_CPU_ACCESS_READ = 0x20000 };
typedef enum { D3D11_MAP_READ = 1, D3D11_MAP_WRITE = 2, D3D11_MAP_READ_WRITE = 3, D3D11_MAP_WRITE_DISCARD = 4,
               D3D11_MAP_WRITE_NO_OVERWRITE = 5 } D3D11_MAP;
enum { D3D11_MAP_FLAG_DO_NOT_WAIT = 0x100000 };
enum { D3D11_CLEAR_DEPTH = 0x1, D3D11_CLEAR_STENCIL = 0x2 };
typedef enum { D3D11_INPUT_PER_VERTEX_DATA = 0, D3D11_INPUT_PER_INSTANCE_DATA = 1 } D3D11_INPUT_CLASSIFICATION;
typedef enum { D3D11_FILTER_MIN_MAG_MIP_POINT = 0, D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0x15 } D3D11_FILTER;
typedef enum { D3D11_TEXTURE_ADDRESS_WRAP = 1, D3D11_TEXTURE_ADDRESS_MIRROR = 2, D3D11_TEXTURE_ADDRESS_CLAMP = 3,
               D3D11_TEXTURE_ADDRESS_BORDER = 4, D3D11_TEXTURE_ADDRESS_MIRROR_ONCE = 5 } D3D11_TEXTURE_ADDRESS_MODE;
typedef enum {
    D3D11_BLEND_ZERO = 1, D3D11_BLEND_ONE = 2, D3D11_BLEND_SRC_COLOR = 3, D3D11_BLEND_INV_SRC_COLOR = 4,
    D3D11_BLEND_SRC_ALPHA = 5, D3D11_BLEND_INV_SRC_ALPHA = 6, D3D11_BLEND_DEST_ALPHA = 7, D3D11_BLEND_INV_DEST_ALPHA = 8,
    D3D11_BLEND_DEST_COLOR = 9, D3D11_BLEND_INV_DEST_COLOR = 10, D3D11_BLEND_SRC_ALPHA_SAT = 11,
    D3D11_BLEND_BLEND_FACTOR = 14, D3D11_BLEND_INV_BLEND_FACTOR = 15
} D3D11_BLEND;
typedef enum { D3D11_BLEND_OP_ADD = 1, D3D11_BLEND_OP_SUBTRACT = 2, D3D11_BLEND_OP_REV_SUBTRACT = 3,
               D3D11_BLEND_OP_MIN = 4, D3D11_BLEND_OP_MAX = 5 } D3D11_BLEND_OP;
enum { D3D11_COLOR_WRITE_ENABLE_RED = 1, D3D11_COLOR_WRITE_ENABLE_GREEN = 2, D3D11_COLOR_WRITE_ENABLE_BLUE = 4,
       D3D11_COLOR_WRITE_ENABLE_ALPHA = 8, D3D11_COLOR_WRITE_ENABLE_ALL = 15 };
typedef enum { D3D11_COMPARISON_NEVER = 1, D3D11_COMPARISON_LESS = 2, D3D11_COMPARISON_EQUAL = 3,
               D3D11_COMPARISON_LESS_EQUAL = 4, D3D11_COMPARISON_GREATER = 5, D3D11_COMPARISON_NOT_EQUAL = 6,
               D3D11_COMPARISON_GREATER_EQUAL = 7, D3D11_COMPARISON_ALWAYS = 8 } D3D11_COMPARISON_FUNC;
typedef enum { D3D11_STENCIL_OP_KEEP = 1, D3D11_STENCIL_OP_ZERO = 2, D3D11_STENCIL_OP_REPLACE = 3,
               D3D11_STENCIL_OP_INCR_SAT = 4, D3D11_STENCIL_OP_DECR_SAT = 5, D3D11_STENCIL_OP_INVERT = 6,
               D3D11_STENCIL_OP_INCR = 7, D3D11_STENCIL_OP_DECR = 8 } D3D11_STENCIL_OP;
typedef enum { D3D11_DEPTH_WRITE_MASK_ZERO = 0, D3D11_DEPTH_WRITE_MASK_ALL = 1 } D3D11_DEPTH_WRITE_MASK;
typedef enum { D3D11_FILL_WIREFRAME = 2, D3D11_FILL_SOLID = 3 } D3D11_FILL_MODE;
typedef enum { D3D11_CULL_NONE = 1, D3D11_CULL_FRONT = 2, D3D11_CULL_BACK = 3 } D3D11_CULL_MODE;
typedef enum { D3D11_PRIMITIVE_TOPOLOGY_POINTLIST = 1, D3D11_PRIMITIVE_TOPOLOGY_LINELIST = 2,
               D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP = 3, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4,
               D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5 } D3D11_PRIMITIVE_TOPOLOGY;
#define D3D11_FLOAT32_MAX 3.402823466e+38f

/* ── descriptions ── */
typedef struct { UINT Count, Quality; } DXGI_SAMPLE_DESC;
typedef struct {
    UINT Width, Height, MipLevels, ArraySize; DXGI_FORMAT Format; DXGI_SAMPLE_DESC SampleDesc;
    D3D11_USAGE Usage; UINT BindFlags, CPUAccessFlags, MiscFlags;
} D3D11_TEXTURE2D_DESC;
typedef struct { UINT ByteWidth; D3D11_USAGE Usage; UINT BindFlags, CPUAccessFlags, MiscFlags, StructureByteStride; } D3D11_BUFFER_DESC;
typedef struct {
    D3D11_FILTER Filter; D3D11_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW; FLOAT MipLODBias;
    UINT MaxAnisotropy; D3D11_COMPARISON_FUNC ComparisonFunc; FLOAT BorderColor[4], MinLOD, MaxLOD;
} D3D11_SAMPLER_DESC;
typedef struct { void *pData; UINT RowPitch, DepthPitch; } D3D11_MAPPED_SUBRESOURCE;
typedef struct { UINT left, top, front, right, bottom, back; } D3D11_BOX;
typedef struct {
    LPCSTR SemanticName; UINT SemanticIndex; DXGI_FORMAT Format; UINT InputSlot, AlignedByteOffset;
    D3D11_INPUT_CLASSIFICATION InputSlotClass; UINT InstanceDataStepRate;
} D3D11_INPUT_ELEMENT_DESC;
typedef struct { FLOAT TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth; } D3D11_VIEWPORT;
typedef struct {
    BOOL BlendEnable; D3D11_BLEND SrcBlend, DestBlend; D3D11_BLEND_OP BlendOp;
    D3D11_BLEND SrcBlendAlpha, DestBlendAlpha; D3D11_BLEND_OP BlendOpAlpha; UINT8 RenderTargetWriteMask;
} D3D11_RENDER_TARGET_BLEND_DESC;
typedef struct { BOOL AlphaToCoverageEnable, IndependentBlendEnable; D3D11_RENDER_TARGET_BLEND_DESC RenderTarget[8]; } D3D11_BLEND_DESC;
typedef struct { D3D11_STENCIL_OP StencilFailOp, StencilDepthFailOp, StencilPassOp; D3D11_COMPARISON_FUNC StencilFunc; } D3D11_DEPTH_STENCILOP_DESC;
typedef struct {
    BOOL DepthEnable; D3D11_DEPTH_WRITE_MASK DepthWriteMask; D3D11_COMPARISON_FUNC DepthFunc; BOOL StencilEnable;
    UINT8 StencilReadMask, StencilWriteMask; D3D11_DEPTH_STENCILOP_DESC FrontFace, BackFace;
} D3D11_DEPTH_STENCIL_DESC;
typedef struct {
    D3D11_FILL_MODE FillMode; D3D11_CULL_MODE CullMode; BOOL FrontCounterClockwise; INT DepthBias;
    FLOAT DepthBiasClamp, SlopeScaledDepthBias; BOOL DepthClipEnable, ScissorEnable, MultisampleEnable, AntialiasedLineEnable;
} D3D11_RASTERIZER_DESC;
typedef struct { UINT dummy; } DXGI_SWAP_CHAIN_DESC;

/* ── calls: none of these run without a device ── */
#define D3D11_NULL_FAIL ((HRESULT)0x80004005L)
#define D3DCompile(...)                                   D3D11_NULL_FAIL
#define ID3D10Blob_Release(b)                             ((void)(b), 0u)
#define ID3D10Blob_GetBufferPointer(b)                    ((void)(b), (void *)0)
#define ID3D10Blob_GetBufferSize(b)                       ((void)(b), (SIZE_T)0)
#define IDXGISwapChain_GetBuffer(...)                     D3D11_NULL_FAIL
#define ID3D11Device_CreateTexture2D(...)                 D3D11_NULL_FAIL
#define ID3D11Device_CreateBuffer(...)                    D3D11_NULL_FAIL
#define ID3D11Device_CreateVertexShader(...)              D3D11_NULL_FAIL
#define ID3D11Device_CreatePixelShader(...)               D3D11_NULL_FAIL
#define ID3D11Device_CreateInputLayout(...)               D3D11_NULL_FAIL
#define ID3D11Device_CreateSamplerState(...)              D3D11_NULL_FAIL
#define ID3D11Device_CreateShaderResourceView(...)        D3D11_NULL_FAIL
#define ID3D11Device_CreateRenderTargetView(...)          D3D11_NULL_FAIL
#define ID3D11Device_CreateDepthStencilView(...)          D3D11_NULL_FAIL
#define ID3D11Device_CreateBlendState(...)                D3D11_NULL_FAIL
#define ID3D11Device_CreateDepthStencilState(...)         D3D11_NULL_FAIL
#define ID3D11Device_CreateRasterizerState(...)           D3D11_NULL_FAIL
#define ID3D11DeviceContext_Map(...)                      D3D11_NULL_FAIL
#define ID3D11DeviceContext_Unmap(...)                    ((void)0)
#define ID3D11DeviceContext_UpdateSubresource(...)        ((void)0)
#define ID3D11DeviceContext_CopySubresourceRegion(...)    ((void)0)
#define ID3D11DeviceContext_CopyResource(...)             ((void)0)
#define ID3D11DeviceContext_ClearRenderTargetView(...)    ((void)0)
#define ID3D11DeviceContext_ClearDepthStencilView(...)    ((void)0)
#define ID3D11DeviceContext_OMSetRenderTargets(...)       ((void)0)
#define ID3D11DeviceContext_OMGetRenderTargets(...)       ((void)0)
#define ID3D11DeviceContext_OMSetBlendState(...)          ((void)0)
#define ID3D11DeviceContext_OMGetBlendState(...)          ((void)0)
#define ID3D11DeviceContext_OMSetDepthStencilState(...)   ((void)0)
#define ID3D11DeviceContext_OMGetDepthStencilState(...)   ((void)0)
#define ID3D11DeviceContext_RSSetViewports(...)           ((void)0)
#define ID3D11DeviceContext_RSGetViewports(...)           ((void)0)
#define ID3D11DeviceContext_RSSetState(...)               ((void)0)
#define ID3D11DeviceContext_RSGetState(...)               ((void)0)
#define ID3D11DeviceContext_IASetVertexBuffers(...)       ((void)0)
#define ID3D11DeviceContext_IAGetVertexBuffers(...)       ((void)0)
#define ID3D11DeviceContext_IASetIndexBuffer(...)         ((void)0)
#define ID3D11DeviceContext_IAGetIndexBuffer(...)         ((void)0)
#define ID3D11DeviceContext_IASetInputLayout(...)         ((void)0)
#define ID3D11DeviceContext_IAGetInputLayout(...)         ((void)0)
#define ID3D11DeviceContext_IASetPrimitiveTopology(...)   ((void)0)
#define ID3D11DeviceContext_VSSetShader(...)              ((void)0)
#define ID3D11DeviceContext_VSGetShader(...)              ((void)0)
#define ID3D11DeviceContext_VSSetConstantBuffers(...)     ((void)0)
#define ID3D11DeviceContext_PSSetShader(...)              ((void)0)
#define ID3D11DeviceContext_PSGetShader(...)              ((void)0)
#define ID3D11DeviceContext_PSSetShaderResources(...)     ((void)0)
#define ID3D11DeviceContext_PSGetShaderResources(...)     ((void)0)
#define ID3D11DeviceContext_PSSetSamplers(...)            ((void)0)
#define ID3D11DeviceContext_PSGetSamplers(...)            ((void)0)
#define ID3D11DeviceContext_PSSetConstantBuffers(...)     ((void)0)
#define ID3D11DeviceContext_PSGetConstantBuffers(...)     ((void)0)
#define ID3D11DeviceContext_DrawIndexed(...)              ((void)0)
#define ID3D11DeviceContext_Draw(...)                     ((void)0)
#define ID3D11Texture2D_GetDesc(t, d)                     ((void)(t), memset((d), 0, sizeof *(d)), (void)0)
#define ID3D11ShaderResourceView_GetResource(v, r)        ((void)(v), *(r) = NULL, (void)0)
#define ID3D11Texture2D_Release(x)                        ((void)(x), 0u)
#define ID3D11Resource_Release(x)                         ((void)(x), 0u)
#define ID3D11Buffer_Release(x)                           ((void)(x), 0u)
#define ID3D11ShaderResourceView_Release(x)               ((void)(x), 0u)
#define ID3D11RenderTargetView_Release(x)                 ((void)(x), 0u)
#define ID3D11DepthStencilView_Release(x)                 ((void)(x), 0u)
#define ID3D11VertexShader_Release(x)                     ((void)(x), 0u)
#define ID3D11PixelShader_Release(x)                      ((void)(x), 0u)
#define ID3D11InputLayout_Release(x)                      ((void)(x), 0u)
#define ID3D11SamplerState_Release(x)                     ((void)(x), 0u)
#define ID3D11BlendState_Release(x)                       ((void)(x), 0u)
#define ID3D11DepthStencilState_Release(x)                ((void)(x), 0u)
#define ID3D11RasterizerState_Release(x)                  ((void)(x), 0u)
