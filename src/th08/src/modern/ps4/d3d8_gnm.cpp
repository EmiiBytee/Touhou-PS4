// Direct3D 8 implementation for the PS4, talking to GNM directly.
//
// This is the PS4 counterpart of src/modern/linux/d3d8_compat.cpp and deliberately keeps
// the same boundary: TH08 still calls the IDirect3DDevice8 surface it was written against,
// vertices are still transformed on the CPU, and textures and surfaces are still ordinary
// lockable pixel buffers. Only the part that puts pixels on screen is different.
//
// Piglet (the system's GL) is not used: it only works in mini applications, which cannot
// record gameplay. Talking to GNM keeps this a full game.
//
// The structure follows src/th07/src/graphics/Gnm.cpp, the same renderer already proven on
// hardware for Perfect Cherry Blossom. What is new here is the Direct3D 8 state
// translation, the separate colour and alpha texture stages TH08 depends on, per-vertex fog
// and the lockable back buffer TH08 copies to and captures from.

#include "modern/linux/d3d8_internal.hpp"

#include "Supervisor.hpp"

#include <SDL2/SDL.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

extern "C"
{
#include <gnm/drawcommandbuffer.h>
#include <gnm/gpuaddr/gpuaddr.h>
#include <gnm/platform.h>
#include <gnmdriver.h>

#include "displayctx.h"
#include "memalloc.h"
#include "misc.h"
}

#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

void PS4_Log(const char *fmt, ...);
void PS4_Notify(const char *message);

// Diagnostic: game code sets this around one draw it wants reported, having flushed the
// sprite batch either side of it so that draw arrives here on its own.
extern "C" int g_Ps4TraceDraw = 0;

namespace
{
// Per frame: vertices, uniform buffers and descriptor sets are bump-allocated out of one
// block and reused once the GPU is done with that frame.
const uint32_t kFrameHeapSize = 12 * 1024 * 1024;
const uint32_t kNumFrames = 2;
const uint32_t kCmdBufferSize = 4 * 1024 * 1024;
const uint32_t kGarlicHeapSize = 192 * 1024 * 1024;

// The reconstructed game draws in a 640x480 space; everything below scales that to the
// console's own output resolution.
const float kGameWidth = 640.0f;
const float kGameHeight = 480.0f;

// The OpenGL backend nudges pre-transformed vertices half a pixel to match how GL samples
// pixel centres. Here the game's 640x480 space is scaled to the console's output, so that
// half pixel would become one and a half real pixels and shift every sprite; this GPU
// already samples at pixel centres, exactly as Direct3D expects, so no offset is applied.
// Sprite-edge bleeding is dealt with in InsetQuadTexCoords() instead, where it belongs.
const float kScreenSpaceHalfPixel = 0.0f;

// Must match the constants in th08_gnm.frag.glsl.
enum ShaderArgument
{
    SHADER_ARG_DIFFUSE,
    SHADER_ARG_TEXTURE,
    SHADER_ARG_TFACTOR
};
enum ShaderOperation
{
    SHADER_OP_MODULATE,
    SHADER_OP_SELECTARG1,
    SHADER_OP_DISABLE
};

// Screen-space vertices carry a pixel coordinate with w = 1; world-space ones carry clip
// space, so the GPU can clip against the near plane and interpolate perspective-correctly.
struct Vertex
{
    float x, y, z, w;
    float fog;
    float u, v;
    uint8_t r, g, b, a;
};

// What the shaders read (std140 layout of th08_gnm.vert.glsl).
struct Constants
{
    float fogColor[4];
    float textureFactor[4];
    float fogRange[4];
    float flags0[4]; // RGB op, alpha op, RGB arg1, RGB arg2
    float flags1[4]; // texture enabled, alpha test func, alpha reference, fog enabled
    float flags2[4]; // alpha arg1, alpha arg2, screen space, unused
    float viewport[4];
    float debug[4];
};

struct VsDescSet
{
    GnmBuffer constbuf;
};

struct PsDescSet
{
    GnmTexture texture;
    GnmSampler sampler;
};

struct FrameHeap
{
    uint8_t *base;
    uint32_t used;
};

UINT BytesPerPixel(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: return 2;
    default: return 4;
    }
}

// Decoded into the byte order GNM_FMT_R8G8B8A8_UNORM expects, so a decoded row can be
// handed to the GPU without a second conversion pass.
void DecodePixel(const BYTE *source, D3DFORMAT format, BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0]; rgba[3] = 255; break;
    case D3DFMT_R5G6B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 11) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 63) * 255 / 63);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31); rgba[3] = 255; break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 10) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 31) * 255 / 31);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31);
        rgba[3] = format == D3DFMT_A1R5G5B5 && !(pixel & 0x8000) ? 0 : 255; break;
    case D3DFMT_A4R4G4B4:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 8) & 15) * 17);
        rgba[1] = static_cast<BYTE>(((pixel >> 4) & 15) * 17);
        rgba[2] = static_cast<BYTE>((pixel & 15) * 17);
        rgba[3] = static_cast<BYTE>(((pixel >> 12) & 15) * 17); break;
    default:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0];
        rgba[3] = format == D3DFMT_X8R8G8B8 ? 255 : source[3]; break;
    }
}

void EncodePixel(BYTE *destination, D3DFORMAT format, const BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0]; break;
    case D3DFMT_R5G6B5:
        pixel = static_cast<WORD>(((rgba[0] * 31 / 255) << 11) |
                                  ((rgba[1] * 63 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        pixel = static_cast<WORD>(((format == D3DFMT_X1R5G5B5 || rgba[3] >= 128) ? 0x8000 : 0) |
                                  ((rgba[0] * 31 / 255) << 10) |
                                  ((rgba[1] * 31 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_A4R4G4B4:
        pixel = static_cast<WORD>(((rgba[3] >> 4) << 12) | ((rgba[0] >> 4) << 8) |
                                  ((rgba[1] >> 4) << 4) | (rgba[2] >> 4));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    default:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0];
        destination[3] = format == D3DFMT_X8R8G8B8 ? 255 : rgba[3]; break;
    }
}

void Identity(D3DMATRIX *matrix)
{
    memset(matrix, 0, sizeof(*matrix));
    matrix->_11 = matrix->_22 = matrix->_33 = matrix->_44 = 1.0f;
}

inline float ByteToFloat(DWORD value)
{
    return static_cast<float>(value & 255) / 255.0f;
}

// On PS4 the original Window/Fullscreen configuration option is repurposed as 16:9 / 4:3,
// the same way the TH06 and TH07 ports do it.
inline bool WidescreenRequested()
{
    return th08::g_Supervisor.cfg.windowed != 0;
}

class Ps4Texture;
class Ps4Device;

class Ps4Surface : public IDirect3DSurface8
{
  public:
    Ps4Surface(UINT width_, UINT height_, D3DFORMAT format_, Ps4Device *device_, Ps4Texture *owner_)
        : refs(1), width(width_), height(height_), format(format_), device(device_), owner(owner_),
          dirty(device_ == NULL)
    {
        pitch = width * BytesPerPixel(format);
        pixels.resize(pitch * height);
    }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT GetDesc(D3DSURFACE_DESC *description)
    {
        if (description == NULL) return E_INVALIDARG;
        memset(description, 0, sizeof(*description));
        description->Format = format; description->Type = D3DRTYPE_SURFACE;
        description->Pool = IsBackbuffer() ? D3DPOOL_DEFAULT : D3DPOOL_SYSTEMMEM;
        description->Size = static_cast<UINT>(pixels.size());
        description->Width = width; description->Height = height; return S_OK;
    }
    HRESULT LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags);
    HRESULT UnlockRect();
    HRESULT GetDC(HDC *dc)
    { if (dc == NULL) return E_INVALIDARG; *dc = CreateCompatibleDC(NULL); return *dc ? S_OK : E_FAIL; }
    HRESULT ReleaseDC(HDC dc) { return DeleteDC(dc) ? S_OK : E_FAIL; }

    bool IsBackbuffer() const { return device != NULL; }

    ULONG refs;
    UINT width, height, pitch;
    D3DFORMAT format;
    // Only the back buffer has a device: that is what makes it read from, and flush to, the
    // render target the GPU scans out.
    Ps4Device *device;
    Ps4Texture *owner;
    bool dirty;
    std::vector<BYTE> pixels;
};

class Ps4Texture : public IDirect3DTexture8
{
  public:
    Ps4Texture(UINT width, UINT height, D3DFORMAT format)
        : refs(1), priority(0), texels(NULL), gpuWidth(0), gpuHeight(0), valid(false),
          uploaded(false), lastUsedBy(0), surface(NULL)
    {
        memset(&desc, 0, sizeof(desc));
        surface = new Ps4Surface(width, height, format, NULL, this);
    }
    ~Ps4Texture();
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    DWORD SetPriority(DWORD value) { DWORD old = priority; priority = value; return old; }
    void PreLoad() {}
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *description)
    { return level == 0 ? surface->GetDesc(description) : E_INVALIDARG; }
    HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface8 **result)
    {
        if (level != 0 || result == NULL) return E_INVALIDARG;
        surface->AddRef(); *result = surface; return S_OK;
    }
    HRESULT LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
    { return level == 0 ? surface->LockRect(locked, rect, flags) : E_INVALIDARG; }
    HRESULT UnlockRect(UINT level)
    { if (level != 0) return E_INVALIDARG; uploaded = false; return surface->UnlockRect(); }

    ULONG refs;
    DWORD priority;
    GnmTexture desc;
    void *texels;
    UINT gpuWidth, gpuHeight;
    bool valid;
    bool uploaded;
    // Which batch of commands last drew with it, so that editing a texture only waits for
    // the GPU when it would actually change something already queued.
    uint64_t lastUsedBy;
    Ps4Surface *surface;
};

class Ps4VertexBuffer : public IDirect3DVertexBuffer8
{
  public:
    explicit Ps4VertexBuffer(UINT size) : refs(1), bytes(size) {}
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT Lock(UINT offset, UINT size, BYTE **data, DWORD)
    {
        if (data == NULL || offset > bytes.size()) return E_INVALIDARG;
        if (size == 0) size = static_cast<UINT>(bytes.size() - offset);
        if (offset + size > bytes.size()) return E_INVALIDARG;
        *data = bytes.empty() ? NULL : &bytes[offset]; return S_OK;
    }
    HRESULT Unlock() { return S_OK; }
    ULONG refs;
    std::vector<BYTE> bytes;
};

UINT VertexCount(D3DPRIMITIVETYPE type, UINT primitiveCount)
{
    if (type == D3DPT_POINTLIST) return primitiveCount;
    if (type == D3DPT_LINELIST) return primitiveCount * 2;
    if (type == D3DPT_LINESTRIP) return primitiveCount + 1;
    if (type == D3DPT_TRIANGLELIST) return primitiveCount * 3;
    return primitiveCount + 2;
}

GnmDepthCompare CompareFunction(DWORD function)
{
    switch (function)
    {
    case D3DCMP_NEVER: return GNM_DEPTH_COMPARE_NEVER;
    case D3DCMP_LESS: return GNM_DEPTH_COMPARE_LESS;
    case D3DCMP_EQUAL: return GNM_DEPTH_COMPARE_EQUAL;
    case D3DCMP_LESSEQUAL: return GNM_DEPTH_COMPARE_LESSEQUAL;
    case D3DCMP_GREATER: return GNM_DEPTH_COMPARE_GREATER;
    case D3DCMP_NOTEQUAL: return GNM_DEPTH_COMPARE_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GNM_DEPTH_COMPARE_GREATEREQUAL;
    default: return GNM_DEPTH_COMPARE_ALWAYS;
    }
}

GnmBlendOp BlendFactor(DWORD factor)
{
    switch (factor)
    {
    case D3DBLEND_ZERO: return GNM_BLEND_ZERO;
    case D3DBLEND_ONE: return GNM_BLEND_ONE;
    case D3DBLEND_SRCCOLOR: return GNM_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return GNM_BLEND_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return GNM_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return GNM_BLEND_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return GNM_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GNM_BLEND_ONE_MINUS_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR: return GNM_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GNM_BLEND_ONE_MINUS_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT: return GNM_BLEND_SRC_ALPHA_SATURATE;
    default: return GNM_BLEND_ONE;
    }
}

float ArgumentSelector(DWORD argument)
{
    switch (argument & D3DTA_SELECTMASK)
    {
    case D3DTA_TEXTURE: return static_cast<float>(SHADER_ARG_TEXTURE);
    case D3DTA_TFACTOR: return static_cast<float>(SHADER_ARG_TFACTOR);
    default: return static_cast<float>(SHADER_ARG_DIFFUSE);
    }
}

float OperationSelector(DWORD operation)
{
    switch (operation)
    {
    case D3DTOP_DISABLE: return static_cast<float>(SHADER_OP_DISABLE);
    case D3DTOP_SELECTARG1: return static_cast<float>(SHADER_OP_SELECTARG1);
    default: return static_cast<float>(SHADER_OP_MODULATE);
    }
}

// Menus and the HUD are built by repeating pieces of one texture. At 640x480 each texel maps
// to a pixel and sampling lands dead centre, but scaled to 1080p every edge pixel also picks
// up the neighbouring texel from outside the piece, drawing a bright seam around each one --
// the grid over the HUD. Pull each quad's texture coordinates half a texel inwards, which is
// invisible but keeps the sampler inside the piece.
//
// The game batches sprites as two triangles of three vertices each, so a run of six
// vertices is one quad.
void InsetQuadTexCoords(Vertex *vertices, UINT count, UINT textureWidth, UINT textureHeight)
{
    if (textureWidth == 0 || textureHeight == 0) return;
    const float insetU = 0.5f / static_cast<float>(textureWidth);
    const float insetV = 0.5f / static_cast<float>(textureHeight);
    for (UINT quad = 0; quad + 5 < count; quad += 6)
    {
        float minU = vertices[quad].u, maxU = minU;
        float minV = vertices[quad].v, maxV = minV;
        for (UINT index = 1; index < 6; ++index)
        {
            const Vertex &vertex = vertices[quad + index];
            if (vertex.u < minU) minU = vertex.u;
            if (vertex.u > maxU) maxU = vertex.u;
            if (vertex.v < minV) minV = vertex.v;
            if (vertex.v > maxV) maxV = vertex.v;
        }
        if (maxU - minU <= insetU * 2.0f || maxV - minV <= insetV * 2.0f)
        {
            continue; // too small to inset without collapsing it
        }
        const float lowU = minU + insetU, highU = maxU - insetU;
        const float lowV = minV + insetV, highV = maxV - insetV;
        for (UINT index = 0; index < 6; ++index)
        {
            Vertex &vertex = vertices[quad + index];
            vertex.u = vertex.u < lowU ? lowU : (vertex.u > highU ? highU : vertex.u);
            vertex.v = vertex.v < lowV ? lowV : (vertex.v > highV ? highV : vertex.v);
        }
    }
}

bool TextureOperationUsesTexture(DWORD operation, DWORD argument1, DWORD argument2)
{
    if (operation == D3DTOP_DISABLE) return false;
    if (operation == D3DTOP_SELECTARG1)
        return (argument1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
    return (argument1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE ||
           (argument2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
}

// The shader's own metadata misreports this one: its first instruction is
// "s_swappc_b64 s[0:1], s[0:1]", so the fetch shader pointer belongs in registers 0-1.
const uint32_t kFetchShaderReg = 0;

int FindUsageSlot(const GnmInputUsageSlot *slots, uint32_t count, uint8_t usage)
{
    for (uint32_t i = 0; i < count; i++)
        if (slots[i].usagetype == usage) return slots[i].startregister;
    return -1;
}

class Ps4Device : public IDirect3DDevice8
{
  public:
    Ps4Device()
        : refs(1), backbuffer(NULL), texture(NULL), vertexBuffer(NULL), fvf(0), streamStride(0),
          cmdMem(NULL), label(NULL), vs(NULL), ps(NULL), fetchShader(NULL), vtxBufferReg(-1),
          vsSetReg(-1), psSetReg(-1), curFrame(0), submission(1), ready(false), frameStart(true),
          fullClearFrames(0), aspectInitialized(false), lastWidescreen(false), debugMode(0),
          lastTransform(-1), presentCount(0), statDraws(0), statVertices(0), statUploads(0), statSubmits(0),
          statFrames(0), statStallUs(0), flushTexture(NULL)
    {
        memset(&garlic, 0, sizeof(garlic));
        memset(&display, 0, sizeof(display));
        memset(colorTargets, 0, sizeof(colorTargets));
        memset(&depthTarget, 0, sizeof(depthTarget));
        memset(&cmd, 0, sizeof(cmd));
        memset(frames, 0, sizeof(frames));
        memset(renderStates, 0, sizeof(renderStates));
        memset(textureStates, 0, sizeof(textureStates));
        Identity(&world); Identity(&view); Identity(&projection); Identity(&textureTransform);
        memset(&viewport, 0, sizeof(viewport));
    }
    ~Ps4Device()
    {
        if (texture != NULL) texture->Release();
        if (flushTexture != NULL) flushTexture->Release();
        if (vertexBuffer != NULL) vertexBuffer->Release();
        if (backbuffer != NULL) { backbuffer->device = NULL; backbuffer->Release(); }
        if (ready)
        {
            displayctx_destroy(&display);
            memalloc_destroy(&garlic);
        }
    }

    bool Setup(const D3DPRESENT_PARAMETERS &parameters);

    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT TestCooperativeLevel() { return S_OK; }
    HRESULT Reset(D3DPRESENT_PARAMETERS *parameters)
    {
        if (parameters == NULL) return E_INVALIDARG;
        return ResetBackbuffer(*parameters) ? S_OK : E_FAIL;
    }
    HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *);
    HRESULT GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE, IDirect3DSurface8 **result)
    {
        if (index != 0 || result == NULL || backbuffer == NULL) return E_INVALIDARG;
        backbuffer->AddRef(); *result = backbuffer; return S_OK;
    }
    HRESULT CreateTexture(UINT width, UINT height, UINT, DWORD, D3DFORMAT format, D3DPOOL,
                          IDirect3DTexture8 **result)
    {
        if (result == NULL || width == 0 || height == 0) return E_INVALIDARG;
        if (format == D3DFMT_UNKNOWN) format = D3DFMT_A8R8G8B8;
        *result = new(std::nothrow) Ps4Texture(width, height, format);
        return *result != NULL ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT CreateVertexBuffer(UINT size, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer8 **result)
    {
        if (result == NULL) return E_INVALIDARG;
        *result = new(std::nothrow) Ps4VertexBuffer(size);
        return *result != NULL ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE, BOOL,
                               IDirect3DSurface8 **result)
    { return CreateSurface(width, height, format, result); }
    HRESULT CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8 **result)
    { return CreateSurface(width, height, format, result); }
    HRESULT CopyRects(IDirect3DSurface8 *sourceRaw, const RECT *sourceRects, UINT count,
                      IDirect3DSurface8 *destinationRaw, const POINT *destinationPoints);
    HRESULT BeginScene() { return S_OK; }
    HRESULT EndScene() { return S_OK; }
    HRESULT Clear(DWORD, const D3DRECT *, DWORD flags, D3DCOLOR color, float depth, DWORD);
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
    {
        if (matrix == NULL) return E_INVALIDARG;
        if (state == D3DTS_WORLD) world = *matrix;
        else if (state == D3DTS_VIEW) view = *matrix;
        else if (state == D3DTS_PROJECTION) projection = *matrix;
        else if (state == D3DTS_TEXTURE0) textureTransform = *matrix;
        return S_OK;
    }
    HRESULT SetViewport(const D3DVIEWPORT8 *value)
    {
        if (value == NULL) return E_INVALIDARG;
        viewport = *value; ApplyViewport(); return S_OK;
    }
    HRESULT GetViewport(D3DVIEWPORT8 *value)
    { if (value == NULL) return E_INVALIDARG; *value = viewport; return S_OK; }
    HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
    { if (static_cast<UINT>(state) < 256) renderStates[state] = value; return S_OK; }
    HRESULT SetTexture(DWORD stage, IDirect3DTexture8 *value)
    {
        if (stage != 0) return S_OK;
        Ps4Texture *next = static_cast<Ps4Texture *>(value);
        if (next != NULL) next->AddRef();
        if (texture != NULL) texture->Release();
        texture = next; return S_OK;
    }
    HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value)
    { if (stage == 0 && static_cast<UINT>(state) < 32) textureStates[state] = value; return S_OK; }
    HRESULT SetVertexShader(DWORD value) { fvf = value; return S_OK; }
    HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer8 *value, UINT stride)
    {
        if (stream != 0) return E_INVALIDARG;
        Ps4VertexBuffer *next = static_cast<Ps4VertexBuffer *>(value);
        if (next != NULL) next->AddRef();
        if (vertexBuffer != NULL) vertexBuffer->Release();
        vertexBuffer = next; streamStride = stride; return S_OK;
    }
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
    {
        if (vertexBuffer == NULL || streamStride == 0) return E_FAIL;
        UINT offset = startVertex * streamStride, count = VertexCount(type, primitiveCount);
        if (offset + count * streamStride > vertexBuffer->bytes.size()) return E_INVALIDARG;
        return Draw(type, primitiveCount, &vertexBuffer->bytes[offset], streamStride);
    }
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void *vertices, UINT stride)
    {
        return vertices != NULL
                   ? Draw(type, primitiveCount, static_cast<const BYTE *>(vertices), stride)
                   : E_INVALIDARG;
    }
    HRESULT GetDeviceCaps(D3DCAPS8 *caps);
    HRESULT ResourceManagerDiscardBytes(DWORD) { return S_OK; }

    // Used by the back buffer surface and by textures that are edited mid-frame.
    void ReadBackbuffer(Ps4Surface *surface);
    void WaitIfQueued(uint64_t lastUsedBy) { if (lastUsedBy == submission) SubmitAndWait(); }
    void UploadTexture(Ps4Texture &tex);
    // A real Direct3D 8 device writes a CopyRects or an unlocked back buffer straight into
    // the render target, in submission order. TH08 depends on that: TitleScreen::OnDraw
    // blits its background onto the back buffer and then draws the menu entries over it, so
    // deferring the blit to Present would paint the background on top of the menu.
    void FlushBackbufferIfDirty(Ps4Surface *surface)
    {
        if (surface == backbuffer) FlushBackbuffer();
    }

  private:
    HRESULT CreateSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8 **result)
    {
        if (result == NULL || width == 0 || height == 0) return E_INVALIDARG;
        if (format == D3DFMT_UNKNOWN) format = D3DFMT_A8R8G8B8;
        *result = new(std::nothrow) Ps4Surface(width, height, format, NULL, NULL);
        return *result != NULL ? S_OK : E_OUTOFMEMORY;
    }
    bool CreateDisplayBuffers();
    bool ResetBackbuffer(const D3DPRESENT_PARAMETERS &parameters);
    void *FrameAlloc(uint32_t size, uint32_t alignment);
    void BeginCommandBuffer();
    void SubmitAndWait();
    void ApplyViewport();
    void ApplyDrawTransform(bool screenSpace);
    void ScaledRect(float x, float y, float width, float height, int *left, int *top, int *right,
                    int *bottom) const;
    void ScaledViewportRect(int *left, int *top, int *right, int *bottom) const;
    void TransformPosition(const float *position, bool transformed, Vertex *out);
    HRESULT Draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const BYTE *data, UINT stride);
    void DrawVertices(D3DPRIMITIVETYPE type, const Vertex *vertices, UINT count, bool useTexture,
                      bool screenSpace);
    void FlushBackbuffer();

    ULONG refs;
    Ps4Surface *backbuffer;
    Ps4Texture *texture;
    Ps4VertexBuffer *vertexBuffer;
    DWORD fvf;
    UINT streamStride;

    MemoryAllocator garlic;
    DisplayContext display;
    GnmRenderTarget colorTargets[kNumFrames];
    GnmDepthRenderTarget depthTarget;
    GnmCommandBuffer cmd;
    void *cmdMem;
    volatile uint64_t *label;
    FrameHeap frames[kNumFrames];

    GnmVsShader *vs;
    GnmPsShader *ps;
    void *fetchShader;
    int vtxBufferReg, vsSetReg, psSetReg;

    uint32_t curFrame;
    uint64_t submission; // commands being recorded now; bumped on every submit
    bool ready;
    // BeginCommandBuffer runs on every submit, several times per frame; the full wipe has
    // to happen only on the first one of a frame or it erases a half-drawn picture.
    bool frameStart;
    uint32_t fullClearFrames;
    bool aspectInitialized, lastWidescreen;
    int debugMode;
    // Which mapping ApplyDrawTransform() last wrote: 1 screen space, 0 world space, -1 for
    // nothing yet or no longer valid.
    int lastTransform;
    unsigned long presentCount;

    // What a second of rendering cost, reported by Present(). A mid-frame submit drains the
    // whole queued command buffer before the CPU may carry on, so it is the one number here
    // that gets more expensive the more the frame had already queued.
    unsigned long statDraws, statVertices, statUploads, statSubmits, statFrames;
    uint64_t statStallUs;

    // Direct3D 8 state, mirroring what the OpenGL backend leaves in GL itself.
    DWORD renderStates[256], textureStates[32];
    D3DMATRIX world, view, projection, textureTransform;
    D3DVIEWPORT8 viewport;

    // Reused by the back buffer flush so presenting does not allocate a texture per frame.
    Ps4Texture *flushTexture;
};

Ps4Texture::~Ps4Texture()
{
    surface->owner = NULL;
    surface->Release();
}

HRESULT Ps4Surface::LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
{
    if (locked == NULL) return E_INVALIDARG;
    if (IsBackbuffer() && (flags & D3DLOCK_READONLY)) device->ReadBackbuffer(this);
    UINT left = rect != NULL && rect->left > 0 ? static_cast<UINT>(rect->left) : 0;
    UINT top = rect != NULL && rect->top > 0 ? static_cast<UINT>(rect->top) : 0;
    if (left >= width || top >= height) return E_INVALIDARG;
    locked->Pitch = pitch;
    locked->pBits = &pixels[top * pitch + left * BytesPerPixel(format)];
    return S_OK;
}

HRESULT Ps4Surface::UnlockRect()
{
    dirty = true;
    if (IsBackbuffer()) device->FlushBackbufferIfDirty(this);
    return S_OK;
}

bool Ps4Device::Setup(const D3DPRESENT_PARAMETERS &parameters)
{
    garlic = memalloc_init(kGarlicHeapSize,
                           ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW |
                               ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE,
                           ORBIS_KERNEL_WC_GARLIC);

    if (!displayctx_init(&display))
    {
        PS4_Log("d3d8: display init failed");
        return false;
    }
    if (!CreateDisplayBuffers()) return false;
    if (!initdepthtarget(&depthTarget, &garlic, display.screenw, display.screenh, display.screenw, 1, 1,
                         GNM_Z_32_FLOAT, GNM_STENCIL_INVALID, gnmGpuMode()))
    {
        PS4_Log("d3d8: depth target failed");
        return false;
    }
    if (!initclearutility(&garlic))
    {
        PS4_Log("d3d8: clear utility failed");
        return false;
    }
    if (!loadvshader(&vs, &garlic, "/app0/assets/misc/th08_gnm.vert.sb") ||
        !loadpshader(&ps, &garlic, "/app0/assets/misc/th08_gnm.frag.sb"))
    {
        PS4_Log("d3d8: loading shaders failed");
        return false;
    }

    const GnmInputUsageSlot *vsslots = gnmVsShaderInputUsageSlotTable(vs);
    const GnmInputUsageSlot *psslots = gnmPsShaderInputUsageSlotTable(ps);
    vtxBufferReg = FindUsageSlot(vsslots, vs->common.numinputusageslots, GNM_SHINPUTUSAGE_PTR_VERTEXBUFFERTABLE);
    vsSetReg = FindUsageSlot(vsslots, vs->common.numinputusageslots, GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE);
    psSetReg = FindUsageSlot(psslots, ps->common.numinputusageslots, GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE);
    if (vtxBufferReg < 0 || vsSetReg < 0 || psSetReg < 0)
    {
        PS4_Log("d3d8: shaders don't ask for the pointers we have (%d %d %d)", vtxBufferReg, vsSetReg, psSetReg);
        return false;
    }

    GnmFetchShaderCreateInfo fetchci;
    memset(&fetchci, 0, sizeof(fetchci));
    fetchci.regs = &vs->registers;
    fetchci.inputusages = vsslots;
    fetchci.numinputusages = vs->common.numinputusageslots;
    fetchci.vtxinputs = gnmVsShaderInputSemanticTable(vs);
    fetchci.numvtxinputs = vs->numinputsemantics;
    uint32_t fetchsize = 0;
    if (gnmFetchShaderCalcSize(&fetchsize, &fetchci) != GNM_ERROR_OK)
    {
        PS4_Log("d3d8: fetch shader size failed");
        return false;
    }
    fetchShader = memalloc_alloc(&garlic, fetchsize, GNM_ALIGNMENT_FETCHSHADER_BYTES);
    GnmFetchShaderResults fetchres;
    memset(&fetchres, 0, sizeof(fetchres));
    if (fetchShader == NULL || gnmCreateFetchShader(fetchShader, fetchsize, &fetchci, &fetchres) != GNM_ERROR_OK)
    {
        PS4_Log("d3d8: fetch shader creation failed");
        return false;
    }
    gnmVsRegsSetFetchShaderModifier(&vs->registers, &fetchres);

    cmdMem = memalloc_alloc(&garlic, kCmdBufferSize, GNM_ALIGNMENT_BUFFER_BYTES);
    label = (volatile uint64_t *)memalloc_alloc(&garlic, sizeof(uint64_t), sizeof(uint64_t));
    for (uint32_t i = 0; i < kNumFrames; i++)
    {
        frames[i].base = (uint8_t *)memalloc_alloc(&garlic, kFrameHeapSize, GNM_ALIGNMENT_BUFFER_BYTES);
        if (frames[i].base == NULL)
        {
            PS4_Log("d3d8: frame heap %u failed", i);
            return false;
        }
    }
    if (cmdMem == NULL || label == NULL)
    {
        PS4_Log("d3d8: command buffer allocation failed");
        return false;
    }

    // Direct3D 8's own defaults, which TH08 relies on for the states it never sets itself.
    renderStates[D3DRS_TEXTUREFACTOR] = 0xffffffffu;
    renderStates[D3DRS_SRCBLEND] = D3DBLEND_SRCALPHA;
    renderStates[D3DRS_DESTBLEND] = D3DBLEND_INVSRCALPHA;
    renderStates[D3DRS_ZWRITEENABLE] = TRUE;
    renderStates[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    renderStates[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    textureStates[D3DTSS_COLOROP] = D3DTOP_MODULATE;
    textureStates[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    textureStates[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
    textureStates[D3DTSS_ALPHAOP] = D3DTOP_MODULATE;
    textureStates[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    textureStates[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
    textureStates[D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
    textureStates[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
    textureStates[D3DTSS_MINFILTER] = D3DTEXF_POINT;
    textureStates[D3DTSS_MAGFILTER] = D3DTEXF_POINT;

    ready = true;
    if (!ResetBackbuffer(parameters))
    {
        ready = false;
        return false;
    }

    BeginCommandBuffer();
#ifdef TH_GNM_DEBUG_VIEWS
    // Diagnostic builds can pick a shader view without rebuilding. Release builds ignore
    // this file so a stale gfxdebug cannot tint normal gameplay.
    if (FILE *file = fopen(TH_PS4_GAME_DIR "/gfxdebug", "rb"))
    {
        char selected = '0';
        if (fread(&selected, 1, 1, file) == 1 && selected >= '0' && selected <= '9')
            debugMode = selected - '0';
        fclose(file);
        PS4_Log("d3d8: debug view %d", debugMode);
    }
#endif
    PS4_Log("d3d8: GNM ready (%ux%u, vtx reg %d, vs set reg %d, ps set reg %d)", display.screenw,
            display.screenh, vtxBufferReg, vsSetReg, psSetReg);
    return true;
}

bool Ps4Device::CreateDisplayBuffers()
{
    // Built by hand instead of with the example's helper, which picks a tiled layout: TH08
    // reads the screen back (AnmManager::CaptureToTexture) and copies image surfaces onto
    // it, and only a linear buffer can be addressed as plain rows of pixels.
    //
    // UNORM, not SRGB: the game's colours are already sRGB, exactly like the pixels a plain
    // GL framebuffer receives, so letting the GPU encode them again washes the picture out.
    // Scan-out is told they are sRGB, which is how a GL framebuffer reaches the screen.
    void *addrs[kNumFrames];
    memset(addrs, 0, sizeof(addrs));
    for (uint32_t i = 0; i < kNumFrames; i++)
    {
        GnmRenderTargetCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.colorfmt = GNM_FMT_R8G8B8A8_UNORM;
        ci.width = display.screenw;
        ci.height = display.screenh;
        ci.pitch = 0;
        ci.numslices = 1;
        ci.numsamples = 1;
        ci.numfragments = 1;
        ci.colortilemodehint = GNM_TM_DISPLAY_LINEAR_ALIGNED;
        ci.mingpumode = gnmGpuMode();
        if (gnmCreateRenderTarget(&colorTargets[i], &ci) != GNM_ERROR_OK)
        {
            PS4_Log("d3d8: creating display buffer %u failed", i);
            return false;
        }
        uint64_t size = 0;
        uint32_t align = 0;
        if (gnmRtCalcByteSize(&size, &align, &colorTargets[i]) != GNM_ERROR_OK)
        {
            PS4_Log("d3d8: sizing display buffer %u failed", i);
            return false;
        }
        addrs[i] = memalloc_alloc(&garlic, size, align);
        if (addrs[i] == NULL)
        {
            PS4_Log("d3d8: out of memory for display buffer %u", i);
            return false;
        }
        memset(addrs[i], 0, size);
        gnmRtSetBaseAddr(&colorTargets[i], addrs[i]);
    }

    OrbisVideoOutBufferAttribute attr;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
                                  display.screenw, display.screenh, gnmRtGetPitch(&colorTargets[0]));
    const int rc = sceVideoOutRegisterBuffers(display.videohandle, 0, addrs, kNumFrames, &attr);
    if (rc < 0)
    {
        PS4_Log("d3d8: sceVideoOutRegisterBuffers failed %#x", rc);
        return false;
    }
    return true;
}

bool Ps4Device::ResetBackbuffer(const D3DPRESENT_PARAMETERS &parameters)
{
    UINT width = parameters.BackBufferWidth != 0 ? parameters.BackBufferWidth : 640;
    UINT height = parameters.BackBufferHeight != 0 ? parameters.BackBufferHeight : 480;
    D3DFORMAT format = parameters.BackBufferFormat;
    if (format == D3DFMT_UNKNOWN) format = D3DFMT_X8R8G8B8;

    if (backbuffer != NULL)
    {
        backbuffer->device = NULL;
        backbuffer->Release();
    }
    backbuffer = new(std::nothrow) Ps4Surface(width, height, format, this, NULL);
    if (backbuffer == NULL) return false;
    backbuffer->dirty = false;
    if (flushTexture != NULL)
    {
        flushTexture->Release();
        flushTexture = NULL;
    }

    viewport.X = viewport.Y = 0;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    // Both swap buffers still hold whatever the previous mode drew.
    fullClearFrames = kNumFrames;
    return true;
}

void *Ps4Device::FrameAlloc(uint32_t size, uint32_t alignment)
{
    FrameHeap &heap = frames[curFrame];
    const uint32_t offset = (heap.used + alignment - 1) / alignment * alignment;
    if (offset + size > kFrameHeapSize)
    {
        // Out of scratch for this frame: drop the draw rather than scribble over data the
        // GPU is still reading.
        static uint32_t reported;
        if (reported++ < 5) PS4_Log("d3d8: frame heap exhausted (%u bytes used)", heap.used);
        return NULL;
    }
    heap.used = offset + size;
    return heap.base + offset;
}

// Maps a rectangle of the game's own 640x480 space onto the screen. On PS4 the original
// Window/Fullscreen option is repurposed as 16:9/4:3. The former stretches that space
// across the screen; the latter scales it uniformly into a centred, pillarboxed playfield.
void Ps4Device::ScaledRect(float x, float y, float width, float height, int *left, int *top,
                           int *right, int *bottom) const
{
    const bool widescreen = WidescreenRequested();
    const float scaleY = static_cast<float>(display.screenh) / kGameHeight;
    const float scaleX = widescreen ? static_cast<float>(display.screenw) / kGameWidth : scaleY;
    const int playWidth = static_cast<int>(kGameWidth * scaleX);
    const int playLeft = (static_cast<int>(display.screenw) - playWidth) / 2;
    *left = playLeft + static_cast<int>(x * scaleX);
    *top = static_cast<int>(y * scaleY);
    *right = *left + static_cast<int>(width * scaleX);
    *bottom = *top + static_cast<int>(height * scaleY);
}

void Ps4Device::ScaledViewportRect(int *left, int *top, int *right, int *bottom) const
{
    ScaledRect(static_cast<float>(viewport.X), static_cast<float>(viewport.Y),
               static_cast<float>(viewport.Width), static_cast<float>(viewport.Height),
               left, top, right, bottom);
}

// Direct3D 8 gives the viewport two separate jobs, and a pre-transformed vertex only sees
// one of them:
//
//   * a D3DFVF_XYZRHW position is an absolute render-target pixel coordinate. The viewport
//     does not move or scale it; it only clips it. The whole back buffer is therefore what
//     normalized device coordinates have to map onto.
//   * an untransformed position goes through the viewport transform, so clip space maps onto
//     the viewport rectangle itself.
//
// TH08 relies on the difference: it draws the HUD and the menus with the viewport set to the
// full 640x480, but AsciiManager also narrows it to the arcade region and Background narrows
// it to the playfield. Mapping pre-transformed vertices onto whichever rectangle happened to
// be current stretches them and pushes parts of the HUD, such as the boss life bar, out of
// the visible area for as long as that viewport stays set.
void Ps4Device::ApplyDrawTransform(bool screenSpace)
{
    if (!ready) return;
    // Bullet-heavy frames issue hundreds of draws; re-emitting an unchanged viewport and
    // scissor for each one only grows the command buffer.
    const int wanted = screenSpace ? 1 : 0;
    if (lastTransform == wanted) return;
    lastTransform = wanted;

    int left, top, right, bottom;
    if (screenSpace && backbuffer != NULL)
    {
        ScaledRect(0.0f, 0.0f, static_cast<float>(backbuffer->width),
                   static_cast<float>(backbuffer->height), &left, &top, &right, &bottom);
    }
    else
    {
        ScaledViewportRect(&left, &top, &right, &bottom);
    }
    // Direct3D's depth range is 0 at the near plane and 1 at the far one, while this GPU's
    // viewport transform expects a clip-space z of -1..1, so the same halving glOrtho gives
    // the OpenGL backend is applied here. Depth stays monotonic, which is all the comparison
    // needs, both vertex paths land on the same scale, and Clear() maps its own value the
    // same way.
    const float zscale = (viewport.MaxZ - viewport.MinZ) * 0.5f;
    const float zoffset = (viewport.MaxZ + viewport.MinZ) * 0.5f;
    setupviewport(&cmd, left, top, right, bottom, zscale, zoffset);

    // The clipping half of the viewport's job. OpenGL and Direct3D clip a primitive to the
    // clip volume, which lands exactly on the viewport rectangle; this GPU clips to a much
    // larger guard band and expects a scissor, so without one the game's deliberately
    // oversized quads spill over the HUD and the pillarbox bars.
    int scissorLeft, scissorTop, scissorRight, scissorBottom;
    ScaledViewportRect(&scissorLeft, &scissorTop, &scissorRight, &scissorBottom);
    gnmDrawCmdSetScreenScissor(&cmd, scissorLeft, scissorTop, scissorRight, scissorBottom);
}

// Called whenever what a transform would produce has changed: a new command buffer, or a
// new Direct3D viewport. Whatever the next draw is, it then emits the mapping it needs.
void Ps4Device::ApplyViewport()
{
    lastTransform = -1;
    ApplyDrawTransform(false);
}

void Ps4Device::BeginCommandBuffer()
{
    cmd = gnmCmdInit(cmdMem, kCmdBufferSize, NULL, NULL);
    gnmDrawCmdSetRenderTarget(&cmd, 0, &colorTargets[curFrame]);
    gnmDrawCmdSetRenderTargetMask(&cmd, 0xf);
    gnmDrawCmdSetDepthRenderTarget(&cmd, &depthTarget);

    const bool widescreen = WidescreenRequested();
    if (!aspectInitialized)
    {
        aspectInitialized = true;
        lastWidescreen = widescreen;
    }
    else if (lastWidescreen != widescreen)
    {
        lastWidescreen = widescreen;
        fullClearFrames = kNumFrames;
        PS4_Log("d3d8: aspect ratio %s", widescreen ? "16:9" : "4:3");
    }
    if (fullClearFrames > 0 && frameStart)
    {
        // When returning to 4:3, erase the old 16:9 image from both swap buffers so it
        // cannot remain visible in the new pillarbox bars.
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        gnmDrawCmdSetScreenScissor(&cmd, 0, 0, display.screenw, display.screenh);
        clearcolortarget(&cmd, &colorTargets[curFrame], black);
        fullClearFrames--;
        gnmDrawCmdSetRenderTarget(&cmd, 0, &colorTargets[curFrame]);
        gnmDrawCmdSetRenderTargetMask(&cmd, 0xf);
        gnmDrawCmdSetDepthRenderTarget(&cmd, &depthTarget);
    }
    frameStart = false;
    ApplyViewport();
}

void Ps4Device::SubmitAndWait()
{
    if (cmd.cmdptr == cmd.beginptr) return;
    statSubmits++;
    const uint64_t stallStart = sceKernelGetProcessTime();
    *label = 0;
    gnmDrawCmdEventWriteEop(&cmd, GNM_CACHE_FLUSH_TS, (uint64_t)label, GNM_DATA_SEL_SEND_DATA64, 1);

    void *dcbaddr = (void *)cmd.beginptr;
    uint32_t dcbsize = (uint32_t)((cmd.cmdptr - cmd.beginptr) * sizeof(uint32_t));
    const int rc = sceGnmSubmitCommandBuffers(1, &dcbaddr, &dcbsize, NULL, NULL);

    // The label must be read as volatile, or the compiler hoists the load and this spins
    // forever even though the GPU has long finished.
    const uint64_t start = sceKernelGetProcessTime();
    while (*label != 1)
    {
        if (sceKernelGetProcessTime() - start > 1000000)
        {
            static uint32_t reported;
            if (reported++ < 5)
                PS4_Log("d3d8: GPU didn't finish (submit rc %#x, label %llu)", rc,
                        (unsigned long long)*label);
            break;
        }
    }
    statStallUs += sceKernelGetProcessTime() - stallStart;
    submission++;
    BeginCommandBuffer();
}

HRESULT Ps4Device::GetDeviceCaps(D3DCAPS8 *caps)
{
    if (caps == NULL) return E_INVALIDARG;
    memset(caps, 0, sizeof(*caps));
    caps->DeviceType = D3DDEVTYPE_HAL;
    caps->Caps2 = D3DCAPS2_CANRENDERWINDOWED;
    caps->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
    caps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION |
                    D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                    D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY;
    caps->MaxTextureWidth = caps->MaxTextureHeight = 4096;
    caps->MaxTextureBlendStages = 1;
    caps->MaxSimultaneousTextures = 1;
    caps->MaxPrimitiveCount = 0x100000;
    caps->MaxStreams = 1;
    caps->MaxStreamStride = 256;
    caps->TextureOpCaps = D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_SELECTARG1;
    return S_OK;
}

void Ps4Device::UploadTexture(Ps4Texture &tex)
{
    if (tex.uploaded && !tex.surface->dirty) return;
    statUploads++;

    const UINT width = tex.surface->width;
    const UINT height = tex.surface->height;
    if (width == 0 || height == 0) return;

    if (tex.texels != NULL && (tex.gpuWidth != width || tex.gpuHeight != height))
    {
        WaitIfQueued(tex.lastUsedBy);
        memalloc_free(&garlic, tex.texels);
        tex.texels = NULL;
    }
    else if (tex.texels != NULL)
    {
        // Only a texture this batch already draws with can be seen changing mid-flight.
        WaitIfQueued(tex.lastUsedBy);
    }
    if (tex.texels == NULL)
    {
        tex.texels = memalloc_alloc(&garlic, static_cast<size_t>(width) * height * 4,
                                    GNM_ALIGNMENT_SHADER_BYTES);
        if (tex.texels == NULL)
        {
            PS4_Log("d3d8: out of texture memory for %ux%u", width, height);
            tex.valid = false;
            return;
        }
        tex.gpuWidth = width;
        tex.gpuHeight = height;

        GnmTextureCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.format = GNM_FMT_R8G8B8A8_UNORM;
        ci.texturetype = GNM_TEXTURE_2D;
        ci.width = width;
        ci.height = height;
        ci.depth = 1;
        ci.pitch = width;
        ci.nummiplevels = 1;
        ci.numslices = 1;
        ci.numfragments = 1;
        ci.tilemodehint = GNM_TM_DISPLAY_LINEAR_GENERAL;
        ci.mingpumode = GNM_GPU_BASE;
        tex.valid = gnmCreateTexture(&tex.desc, &ci) == GNM_ERROR_OK;
        if (!tex.valid)
        {
            PS4_Log("d3d8: gnmCreateTexture failed for %ux%u", width, height);
            return;
        }
        gnmTexSetBaseAddress(&tex.desc, tex.texels);
    }

    const D3DFORMAT format = tex.surface->format;
    const UINT bytes = BytesPerPixel(format);
    BYTE *destination = static_cast<BYTE *>(tex.texels);
    if (format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8)
    {
        // The common case: only the channel order differs, so swizzle without the per-pixel
        // format dispatch.
        const bool opaque = format == D3DFMT_X8R8G8B8;
        for (UINT y = 0; y < height; ++y)
        {
            const BYTE *source = &tex.surface->pixels[y * tex.surface->pitch];
            BYTE *row = destination + static_cast<size_t>(y) * width * 4;
            for (UINT x = 0; x < width; ++x, source += 4, row += 4)
            {
                row[0] = source[2]; row[1] = source[1]; row[2] = source[0];
                row[3] = opaque ? 255 : source[3];
            }
        }
    }
    else
    {
        for (UINT y = 0; y < height; ++y)
        {
            const BYTE *source = &tex.surface->pixels[y * tex.surface->pitch];
            BYTE *row = destination + static_cast<size_t>(y) * width * 4;
            for (UINT x = 0; x < width; ++x, source += bytes, row += 4)
                DecodePixel(source, format, row);
        }
    }
    tex.uploaded = true;
    tex.surface->dirty = false;
}

void Ps4Device::ReadBackbuffer(Ps4Surface *surface)
{
    if (surface == NULL || surface->width == 0 || surface->height == 0) return;
    // Everything queued has to have landed before the target can be read as pixels.
    SubmitAndWait();

    const uint32_t *source = (const uint32_t *)gnmRtGetBaseAddr(&colorTargets[curFrame]);
    const uint32_t sourcePitch = gnmRtGetPitch(&colorTargets[curFrame]);
    const bool widescreen = WidescreenRequested();
    const float scaleY = static_cast<float>(display.screenh) / static_cast<float>(surface->height);
    const float scaleX = widescreen
                             ? static_cast<float>(display.screenw) / static_cast<float>(surface->width)
                             : scaleY;
    const int playWidth = static_cast<int>(static_cast<float>(surface->width) * scaleX);
    const int playLeft = (static_cast<int>(display.screenw) - playWidth) / 2;
    const UINT bytes = BytesPerPixel(surface->format);

    for (UINT y = 0; y < surface->height; ++y)
    {
        const int sourceY = static_cast<int>((static_cast<float>(y) + 0.5f) * scaleY);
        if (sourceY < 0 || sourceY >= static_cast<int>(display.screenh)) continue;
        for (UINT x = 0; x < surface->width; ++x)
        {
            const int sourceX = playLeft + static_cast<int>((static_cast<float>(x) + 0.5f) * scaleX);
            if (sourceX < 0 || sourceX >= static_cast<int>(display.screenw)) continue;
            const uint32_t pixel = source[static_cast<size_t>(sourceY) * sourcePitch + sourceX];
            BYTE rgba[4];
            rgba[0] = static_cast<BYTE>(pixel & 255);
            rgba[1] = static_cast<BYTE>((pixel >> 8) & 255);
            rgba[2] = static_cast<BYTE>((pixel >> 16) & 255);
            rgba[3] = static_cast<BYTE>((pixel >> 24) & 255);
            EncodePixel(&surface->pixels[y * surface->pitch + x * bytes], surface->format, rgba);
        }
    }
    surface->dirty = false;
}

void Ps4Device::FlushBackbuffer()
{
    if (backbuffer == NULL || !backbuffer->dirty) return;
    const UINT width = backbuffer->width;
    const UINT height = backbuffer->height;
    if (width == 0 || height == 0) return;

    // TH08 copies image surfaces straight onto the back buffer (loading screens, the
    // EoSD-style frame). Those pixels only exist on the CPU, so they reach the screen as a
    // textured quad covering the whole back buffer.
    if (flushTexture == NULL)
    {
        flushTexture = new(std::nothrow) Ps4Texture(width, height, backbuffer->format);
        if (flushTexture == NULL) return;
    }
    const size_t copy = backbuffer->pixels.size() < flushTexture->surface->pixels.size()
                            ? backbuffer->pixels.size()
                            : flushTexture->surface->pixels.size();
    // The title screen blits the same background onto the back buffer on every single
    // frame, so the conversion and the upload -- which can stall on the GPU -- only run
    // when the pixels actually changed. The quad itself still has to be drawn each frame,
    // because the render target was cleared.
    if (!flushTexture->uploaded || memcmp(&flushTexture->surface->pixels[0], &backbuffer->pixels[0], copy) != 0)
    {
        memcpy(&flushTexture->surface->pixels[0], &backbuffer->pixels[0], copy);
        flushTexture->surface->dirty = true;
        flushTexture->uploaded = false;
        UploadTexture(*flushTexture);
    }
    if (!flushTexture->valid) return;

    Vertex *vertices = (Vertex *)FrameAlloc(4 * sizeof(Vertex), GNM_ALIGNMENT_BUFFER_BYTES);
    if (vertices == NULL) return;
    const float right = static_cast<float>(width);
    const float bottom = static_cast<float>(height);
    for (int index = 0; index < 4; ++index)
    {
        vertices[index].z = 0.0f;
        vertices[index].w = 1.0f;
        vertices[index].fog = 0.0f;
        vertices[index].r = vertices[index].g = vertices[index].b = vertices[index].a = 255;
    }
    vertices[0].x = 0.0f;  vertices[0].y = 0.0f;   vertices[0].u = 0.0f; vertices[0].v = 0.0f;
    vertices[1].x = right; vertices[1].y = 0.0f;   vertices[1].u = 1.0f; vertices[1].v = 0.0f;
    vertices[2].x = 0.0f;  vertices[2].y = bottom; vertices[2].u = 0.0f; vertices[2].v = 1.0f;
    vertices[3].x = right; vertices[3].y = bottom; vertices[3].u = 1.0f; vertices[3].v = 1.0f;

    // The quad has to cover the whole back buffer and overwrite it whatever the game left
    // in the Direct3D state, so the state that matters is forced for this one draw.
    const D3DVIEWPORT8 savedViewport = viewport;
    Ps4Texture *savedTexture = texture;
    DWORD savedStates[256];
    DWORD savedStages[32];
    memcpy(savedStates, renderStates, sizeof(savedStates));
    memcpy(savedStages, textureStates, sizeof(savedStages));

    viewport.X = viewport.Y = 0;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    ApplyViewport();
    renderStates[D3DRS_ALPHABLENDENABLE] = FALSE;
    renderStates[D3DRS_ALPHATESTENABLE] = FALSE;
    renderStates[D3DRS_ZENABLE] = FALSE;
    renderStates[D3DRS_ZWRITEENABLE] = FALSE;
    renderStates[D3DRS_FOGENABLE] = FALSE;
    textureStates[D3DTSS_COLOROP] = D3DTOP_SELECTARG1;
    textureStates[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    textureStates[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
    textureStates[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    textureStates[D3DTSS_MINFILTER] = D3DTEXF_LINEAR;
    textureStates[D3DTSS_MAGFILTER] = D3DTEXF_LINEAR;
    textureStates[D3DTSS_ADDRESSU] = D3DTADDRESS_CLAMP;
    textureStates[D3DTSS_ADDRESSV] = D3DTADDRESS_CLAMP;
    texture = flushTexture;

    DrawVertices(D3DPT_TRIANGLESTRIP, vertices, 4, true, true);

    texture = savedTexture;
    memcpy(renderStates, savedStates, sizeof(savedStates));
    memcpy(textureStates, savedStages, sizeof(savedStages));
    viewport = savedViewport;
    ApplyViewport();
    backbuffer->dirty = false;
}

HRESULT Ps4Device::Present(const RECT *, const RECT *, HWND, const RGNDATA *)
{
    if (g_Ps4TraceDraw == 2)
    {
        PS4_Log("d3d8 after-bar: present (backbuffer %s)",
                backbuffer != NULL && backbuffer->dirty ? "dirty: CPU copy drawn over the frame" : "clean");
        g_Ps4TraceDraw = 0;
    }
    FlushBackbuffer();
    SubmitAndWait();
    presentCount++;

    if (!displayctx_flip(&display, curFrame))
    {
        static uint32_t reported;
        if (reported++ < 5) PS4_Log("d3d8: flip failed");
    }
    sceGnmSubmitDone();

    static bool splashHidden;
    if (!splashHidden)
    {
        splashHidden = true;
        PS4_Log("d3d8: first frame shown, hiding splash: %#x", sceSystemServiceHideSplashScreen());
    }

    // One line a second with what the frames cost. "stall" is time the CPU spent waiting for
    // the GPU inside a frame, which is what a texture edited mid-flight forces.
    {
        const uint64_t now = sceKernelGetProcessTime();
        static uint64_t lastReport;
        if (lastReport == 0) lastReport = now;
        statFrames++;
        if (now - lastReport >= 1000000)
        {
            PS4_Log("d3d8 perf: %lu fps, %lu draws/f, %lu verts/f, %lu uploads/f, %lu submits/f, "
                    "%lu us stalled/f",
                    statFrames, statDraws / statFrames, statVertices / statFrames, statUploads / statFrames,
                    statSubmits / statFrames, (unsigned long)(statStallUs / statFrames));
            lastReport = now;
            statFrames = statDraws = statVertices = statUploads = statSubmits = 0;
            statStallUs = 0;
        }
    }

    curFrame = (curFrame + 1) % kNumFrames;
    frameStart = true;
    frames[curFrame].used = 0;
    BeginCommandBuffer();
    return S_OK;
}

HRESULT Ps4Device::Clear(DWORD, const D3DRECT *, DWORD flags, D3DCOLOR color, float depth, DWORD)
{
    if (g_Ps4TraceDraw == 2)
        PS4_Log("d3d8 after-bar: clear flags=%lx color=%08lx vp=%lu,%lu %lux%lu", flags, color, viewport.X,
                viewport.Y, viewport.Width, viewport.Height);
    if ((flags & D3DCLEAR_TARGET) != 0)
    {
        const float clearColor[4] = {
            ByteToFloat(color >> 16), ByteToFloat(color >> 8), ByteToFloat(color),
            ByteToFloat(color >> 24)
        };
        int left, top, right, bottom;
        ScaledViewportRect(&left, &top, &right, &bottom);
        gnmDrawCmdSetScreenScissor(&cmd, left, top, right, bottom);
        clearcolortarget(&cmd, &colorTargets[curFrame], clearColor);
    }
    if ((flags & D3DCLEAR_ZBUFFER) != 0)
    {
        // Mapped exactly like ApplyViewport maps a vertex's depth, so a cleared pixel and a
        // drawn one are still compared on the same scale.
        cleardepthtarget(&cmd, &depthTarget, depth * 0.5f + 0.5f);
    }
    // Clearing sets its own controls and viewport; put ours back.
    ApplyViewport();
    gnmDrawCmdSetRenderTarget(&cmd, 0, &colorTargets[curFrame]);
    gnmDrawCmdSetRenderTargetMask(&cmd, 0xf);
    gnmDrawCmdSetDepthRenderTarget(&cmd, &depthTarget);
    return S_OK;
}

HRESULT Ps4Device::CopyRects(IDirect3DSurface8 *sourceRaw, const RECT *sourceRects, UINT count,
                             IDirect3DSurface8 *destinationRaw, const POINT *destinationPoints)
{
    LinuxSurfaceAccess source, destination;
    if (!th08_linux_surface_access(sourceRaw, &source, true) ||
        !th08_linux_surface_access(destinationRaw, &destination, false)) return E_INVALIDARG;
    if (source.format != destination.format) return E_NOTIMPL;
    if (count == 0) count = 1;
    const UINT bytes = BytesPerPixel(source.format);
    for (UINT index = 0; index < count; ++index)
    {
        RECT rect;
        if (sourceRects != NULL) rect = sourceRects[index];
        else { rect.left = 0; rect.top = 0; rect.right = source.width; rect.bottom = source.height; }
        POINT point;
        point.x = destinationPoints != NULL ? destinationPoints[index].x : 0;
        point.y = destinationPoints != NULL ? destinationPoints[index].y : 0;
        UINT copyWidth = rect.right > rect.left ? rect.right - rect.left : 0;
        UINT copyHeight = rect.bottom > rect.top ? rect.bottom - rect.top : 0;
        if (point.x < 0 || point.y < 0 || rect.left < 0 || rect.top < 0) continue;
        if (static_cast<UINT>(point.x) + copyWidth > destination.width) copyWidth = destination.width - point.x;
        if (static_cast<UINT>(point.y) + copyHeight > destination.height) copyHeight = destination.height - point.y;
        for (UINT y = 0; y < copyHeight; ++y)
            memcpy(destination.pixels + (point.y + y) * destination.pitch + point.x * bytes,
                   source.pixels + (rect.top + y) * source.pitch + rect.left * bytes, copyWidth * bytes);
    }
    th08_linux_surface_changed(destinationRaw);
    return S_OK;
}

void Ps4Device::TransformPosition(const float *position, bool transformed, Vertex *out)
{
    if (transformed)
    {
        out->x = position[0] + kScreenSpaceHalfPixel;
        out->y = position[1] + kScreenSpaceHalfPixel;
        out->z = position[2];
        out->w = 1.0f;
        out->fog = 0.0f;
        return;
    }

    // World, view and projection, in Direct3D's row-vector convention. The perspective
    // divide is deliberately NOT done here: handing the rasterizer pre-divided screen
    // coordinates would make it interpolate texture coordinates linearly in screen space
    // and would drop the near-plane clip, which is what the OpenGL backend does and what
    // wrecks the stages with 3D backgrounds.
    float vector[4] = {position[0], position[1], position[2], 1.0f};
    const D3DMATRIX *matrices[3] = {&world, &view, &projection};
    for (int index = 0; index < 3; ++index)
    {
        const D3DMATRIX &m = *matrices[index];
        float next[4];
        next[0] = vector[0] * m._11 + vector[1] * m._21 + vector[2] * m._31 + vector[3] * m._41;
        next[1] = vector[0] * m._12 + vector[1] * m._22 + vector[2] * m._32 + vector[3] * m._42;
        next[2] = vector[0] * m._13 + vector[1] * m._23 + vector[2] * m._33 + vector[3] * m._43;
        next[3] = vector[0] * m._14 + vector[1] * m._24 + vector[2] * m._34 + vector[3] * m._44;
        memcpy(vector, next, sizeof(vector));
        // Fog is a distance in eye space, which only exists between the view and the
        // projection matrix, so it is taken here instead of being derived from depth.
        if (index == 1) out->fog = fabsf(vector[2]);
    }
    out->x = vector[0];
    out->y = vector[1];
    out->z = vector[2];
    out->w = vector[3];
}

HRESULT Ps4Device::Draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const BYTE *data, UINT stride)
{
    if (!ready) return E_FAIL;
    const UINT count = VertexCount(type, primitiveCount);
    if (count == 0) return S_OK;
    statDraws++;
    statVertices += count;

    const bool transformed = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    UINT offset = transformed ? 16 : 12;
    if (fvf & D3DFVF_NORMAL) offset += 12;
    if (fvf & D3DFVF_PSIZE) offset += 4;
    const bool hasDiffuse = (fvf & D3DFVF_DIFFUSE) != 0;
    const UINT colorOffset = offset;
    if (hasDiffuse) offset += 4;
    if (fvf & D3DFVF_SPECULAR) offset += 4;
    const bool hasTexture = (fvf & D3DFVF_TEXCOUNT_MASK) != 0;
    const UINT textureOffset = offset;

    Vertex *vertices = (Vertex *)FrameAlloc(count * sizeof(Vertex), GNM_ALIGNMENT_BUFFER_BYTES);
    if (vertices == NULL) return E_OUTOFMEMORY;

    for (UINT index = 0; index < count; ++index)
    {
        const BYTE *vertex = data + index * stride;
        Vertex &out = vertices[index];
        TransformPosition(reinterpret_cast<const float *>(vertex), transformed, &out);
        const D3DCOLOR color =
            hasDiffuse ? *reinterpret_cast<const D3DCOLOR *>(vertex + colorOffset) : 0xffffffffu;
        out.r = static_cast<uint8_t>((color >> 16) & 255);
        out.g = static_cast<uint8_t>((color >> 8) & 255);
        out.b = static_cast<uint8_t>(color & 255);
        out.a = static_cast<uint8_t>((color >> 24) & 255);
        if (hasTexture)
        {
            const float *uv = reinterpret_cast<const float *>(vertex + textureOffset);
            if (transformed)
            {
                out.u = uv[0];
                out.v = uv[1];
            }
            else
            {
                out.u = uv[0] * textureTransform._11 + uv[1] * textureTransform._21 + textureTransform._31;
                out.v = uv[0] * textureTransform._12 + uv[1] * textureTransform._22 + textureTransform._32;
            }
        }
        else
        {
            out.u = out.v = 0.0f;
        }
    }

    const bool usesTexture =
        texture != NULL &&
        (TextureOperationUsesTexture(textureStates[D3DTSS_COLOROP], textureStates[D3DTSS_COLORARG1],
                                     textureStates[D3DTSS_COLORARG2]) ||
         TextureOperationUsesTexture(textureStates[D3DTSS_ALPHAOP], textureStates[D3DTSS_ALPHAARG1],
                                     textureStates[D3DTSS_ALPHAARG2]));
    const bool useTexture = hasTexture && usesTexture;
    // Only the batched 2D sprites, which are what the seam is visible on. A 3D mesh is also
    // a triangle list, but six of its vertices are not necessarily one quad and its texture
    // coordinates commonly tile past 0..1, so clamping them there would distort the stage.
    if (useTexture && transformed && type == D3DPT_TRIANGLELIST && count % 6 == 0)
    {
        InsetQuadTexCoords(vertices, count, texture->surface->width, texture->surface->height);
    }
    // Mode 2: one line per draw from the boss life bar to the end of the frame, to find what
    // covers the bar. Screen rectangle in game pixels (pre-transformed draws only).
    if (g_Ps4TraceDraw == 2)
    {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for (UINT index = 0; index < count; ++index)
        {
            const Vertex &vertex = vertices[index];
            if (vertex.x < x0) x0 = vertex.x;
            if (vertex.x > x1) x1 = vertex.x;
            if (vertex.y < y0) y0 = vertex.y;
            if (vertex.y > y1) y1 = vertex.y;
            if (vertex.z < z0) z0 = vertex.z;
            if (vertex.z > z1) z1 = vertex.z;
        }
        PS4_Log("d3d8 after-bar: t=%d n=%u ss=%d tex=%dx%d rect=%.0f,%.0f..%.0f,%.0f z=%.2f..%.2f rgba0=%02x%02x%02x%02x "
                "blend=%lu/%lu/%lu z=%lu/%lu/%lu atest=%lu fog=%lu cop=%lu/%lu aop=%lu vp=%lu,%lu %lux%lu",
                (int)type, count, (int)transformed, useTexture ? (int)texture->surface->width : 0,
                useTexture ? (int)texture->surface->height : 0, x0, y0, x1, y1, z0, z1, vertices[0].r,
                vertices[0].g, vertices[0].b, vertices[0].a, renderStates[D3DRS_ALPHABLENDENABLE],
                renderStates[D3DRS_SRCBLEND], renderStates[D3DRS_DESTBLEND], renderStates[D3DRS_ZENABLE],
                renderStates[D3DRS_ZWRITEENABLE], renderStates[D3DRS_ZFUNC], renderStates[D3DRS_ALPHATESTENABLE],
                renderStates[D3DRS_FOGENABLE], textureStates[D3DTSS_COLOROP], textureStates[D3DTSS_COLORARG1],
                textureStates[D3DTSS_ALPHAOP], viewport.X, viewport.Y, viewport.Width, viewport.Height);
    }
    // Set around one isolated draw by whoever is being investigated -- the lasers, which
    // the game hands over with a white diffuse and which never reach the screen.
    else if (g_Ps4TraceDraw)
    {
        PS4_Log("d3d8 trace: type=%d count=%u fvf=%08x transformed=%d hasTex=%d useTex=%d", (int)type, count,
                (unsigned)fvf, (int)transformed, (int)hasTexture, (int)useTexture);
        if (texture != NULL)
            PS4_Log("d3d8 trace: tex %ux%u fmt=%d valid=%d uploaded=%d", texture->surface->width,
                    texture->surface->height, (int)texture->surface->format, (int)texture->valid,
                    (int)texture->uploaded);
        else
            PS4_Log("d3d8 trace: no texture bound");
        for (UINT index = 0; index < count && index < 4; ++index)
        {
            const Vertex &vertex = vertices[index];
            PS4_Log("d3d8 trace: v%u xyzw=%.1f,%.1f,%.4f,%.2f uv=%.4f,%.4f rgba=%02x%02x%02x%02x", index, vertex.x,
                    vertex.y, vertex.z, vertex.w, vertex.u, vertex.v, vertex.r, vertex.g, vertex.b, vertex.a);
        }
        PS4_Log("d3d8 trace: blend=%lu src=%lu dst=%lu z=%lu zw=%lu zfunc=%lu atest=%lu aref=%lu",
                renderStates[D3DRS_ALPHABLENDENABLE], renderStates[D3DRS_SRCBLEND], renderStates[D3DRS_DESTBLEND],
                renderStates[D3DRS_ZENABLE], renderStates[D3DRS_ZWRITEENABLE], renderStates[D3DRS_ZFUNC],
                renderStates[D3DRS_ALPHATESTENABLE], renderStates[D3DRS_ALPHAREF]);
        PS4_Log("d3d8 trace: cop=%lu ca1=%lu ca2=%lu aop=%lu aa1=%lu aa2=%lu viewport=%lu,%lu %lux%lu",
                textureStates[D3DTSS_COLOROP], textureStates[D3DTSS_COLORARG1], textureStates[D3DTSS_COLORARG2],
                textureStates[D3DTSS_ALPHAOP], textureStates[D3DTSS_ALPHAARG1], textureStates[D3DTSS_ALPHAARG2],
                viewport.X, viewport.Y, viewport.Width, viewport.Height);
        float fogStart, fogEnd;
        memcpy(&fogStart, &renderStates[D3DRS_FOGSTART], sizeof(float));
        memcpy(&fogEnd, &renderStates[D3DRS_FOGEND], sizeof(float));
        int scissorLeft, scissorTop, scissorRight, scissorBottom;
        ScaledViewportRect(&scissorLeft, &scissorTop, &scissorRight, &scissorBottom);
        PS4_Log("d3d8 trace: fog=%lu mode=%lu start=%.2f end=%.2f color=%08lx tfactor=%08lx minz=%.2f maxz=%.2f "
                "scissor=%d,%d..%d,%d",
                renderStates[D3DRS_FOGENABLE], renderStates[D3DRS_FOGVERTEXMODE], fogStart, fogEnd,
                renderStates[D3DRS_FOGCOLOR], renderStates[D3DRS_TEXTUREFACTOR], viewport.MinZ, viewport.MaxZ,
                scissorLeft, scissorTop, scissorRight, scissorBottom);
    }
    DrawVertices(type, vertices, count, useTexture, transformed);
    return S_OK;
}

void Ps4Device::DrawVertices(D3DPRIMITIVETYPE type, const Vertex *vertices, UINT count, bool useTexture,
                             bool screenSpace)
{
    Constants *constants = (Constants *)FrameAlloc(sizeof(Constants), GNM_ALIGNMENT_BUFFER_BYTES);
    VsDescSet *vsset = (VsDescSet *)FrameAlloc(sizeof(VsDescSet), GNM_ALIGNMENT_BUFFER_BYTES);
    PsDescSet *psset = (PsDescSet *)FrameAlloc(sizeof(PsDescSet), GNM_ALIGNMENT_BUFFER_BYTES);
    GnmBuffer *vtxbuffers = (GnmBuffer *)FrameAlloc(sizeof(GnmBuffer) * 4, GNM_ALIGNMENT_BUFFER_BYTES);
    if (constants == NULL || vsset == NULL || psset == NULL || vtxbuffers == NULL) return;

    if (useTexture)
    {
        UploadTexture(*texture);
        if (!texture->valid) useTexture = false;
    }
    // After the upload, which can submit the batch and start a fresh command buffer with
    // the resting transform in it.
    ApplyDrawTransform(screenSpace);

    const DWORD fogColor = renderStates[D3DRS_FOGCOLOR];
    constants->fogColor[0] = ByteToFloat(fogColor >> 16);
    constants->fogColor[1] = ByteToFloat(fogColor >> 8);
    constants->fogColor[2] = ByteToFloat(fogColor);
    constants->fogColor[3] = 1.0f;
    const DWORD factor = renderStates[D3DRS_TEXTUREFACTOR];
    constants->textureFactor[0] = ByteToFloat(factor >> 16);
    constants->textureFactor[1] = ByteToFloat(factor >> 8);
    constants->textureFactor[2] = ByteToFloat(factor);
    constants->textureFactor[3] = ByteToFloat(factor >> 24);
    // The fog range render states hold a float bit pattern, not an integer.
    memcpy(&constants->fogRange[0], &renderStates[D3DRS_FOGSTART], sizeof(float));
    memcpy(&constants->fogRange[1], &renderStates[D3DRS_FOGEND], sizeof(float));
    constants->fogRange[2] = constants->fogRange[3] = 0.0f;

    constants->flags0[0] = OperationSelector(textureStates[D3DTSS_COLOROP]);
    constants->flags0[1] = OperationSelector(textureStates[D3DTSS_ALPHAOP]);
    constants->flags0[2] = ArgumentSelector(textureStates[D3DTSS_COLORARG1]);
    constants->flags0[3] = ArgumentSelector(textureStates[D3DTSS_COLORARG2]);
    constants->flags1[0] = useTexture ? 1.0f : 0.0f;
    constants->flags1[1] = renderStates[D3DRS_ALPHATESTENABLE]
                               ? static_cast<float>(renderStates[D3DRS_ALPHAFUNC])
                               : 0.0f;
    constants->flags1[2] = ByteToFloat(renderStates[D3DRS_ALPHAREF]);
    constants->flags1[3] = renderStates[D3DRS_FOGENABLE] &&
                                   renderStates[D3DRS_FOGVERTEXMODE] == D3DFOG_LINEAR
                               ? 1.0f
                               : 0.0f;
    constants->flags2[0] = ArgumentSelector(textureStates[D3DTSS_ALPHAARG1]);
    constants->flags2[1] = ArgumentSelector(textureStates[D3DTSS_ALPHAARG2]);
    constants->flags2[2] = screenSpace ? 1.0f : 0.0f;
    constants->flags2[3] = 0.0f;
    // What the shader maps a pre-transformed pixel coordinate against: the whole back
    // buffer, because that is the space those coordinates are in. ApplyDrawTransform() has
    // put the matching rectangle in the hardware viewport.
    constants->viewport[0] = 0.0f;
    constants->viewport[1] = 0.0f;
    constants->viewport[2] = static_cast<float>(backbuffer != NULL ? backbuffer->width : viewport.Width);
    constants->viewport[3] = static_cast<float>(backbuffer != NULL ? backbuffer->height : viewport.Height);
    constants->debug[0] = static_cast<float>(debugMode);
    constants->debug[1] = constants->debug[2] = constants->debug[3] = 0.0f;

    vsset->constbuf = gnmCreateConstBuffer(constants, sizeof(Constants));
    if (useTexture)
    {
        psset->texture = texture->desc;
        texture->lastUsedBy = submission;
    }
    else
    {
        memset(&psset->texture, 0, sizeof(psset->texture));
    }
    psset->sampler = GnmSampler();
    psset->sampler.xyminfilter =
        textureStates[D3DTSS_MINFILTER] == D3DTEXF_LINEAR ? GNM_FILTER_BILINEAR : GNM_FILTER_POINT;
    psset->sampler.xymagfilter =
        textureStates[D3DTSS_MAGFILTER] == D3DTEXF_LINEAR ? GNM_FILTER_BILINEAR : GNM_FILTER_POINT;
    psset->sampler.mipfilter = GNM_MIPFILTER_NONE;
    psset->sampler.zfilter = GNM_ZFILTER_POINT;
    psset->sampler.maxlod = 0;
    psset->sampler.clampx = textureStates[D3DTSS_ADDRESSU] == D3DTADDRESS_CLAMP
                                ? GNM_TEX_CLAMP_CLAMP_LAST_TEXEL
                                : GNM_TEX_CLAMP_WRAP;
    psset->sampler.clampy = textureStates[D3DTSS_ADDRESSV] == D3DTADDRESS_CLAMP
                                ? GNM_TEX_CLAMP_CLAMP_LAST_TEXEL
                                : GNM_TEX_CLAMP_WRAP;
    psset->sampler.clampz = GNM_TEX_CLAMP_WRAP;

    const uint32_t stride = sizeof(Vertex);
    uint8_t *vertexBytes = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(vertices));
    vtxbuffers[0] = gnmCreateVertexBuffer(vertexBytes, GNM_FMT_R32G32B32A32_FLOAT, stride, count);
    vtxbuffers[1] = gnmCreateVertexBuffer(vertexBytes + 16, GNM_FMT_R32_FLOAT, stride, count);
    vtxbuffers[2] = gnmCreateVertexBuffer(vertexBytes + 20, GNM_FMT_R32G32_FLOAT, stride, count);
    vtxbuffers[3] = gnmCreateVertexBuffer(vertexBytes + 28, GNM_FMT_R8G8B8A8_UNORM, stride, count);

    GnmPrimitiveSetup primsetup;
    memset(&primsetup, 0, sizeof(primsetup));
    primsetup.cullmode = GNM_CULL_NONE;
    primsetup.frontface = GNM_FACE_CCW;
    primsetup.frontmode = GNM_FILL_SOLID;
    primsetup.backmode = GNM_FILL_SOLID;
    primsetup.provokemode = GNM_PROVOKINGVTX_FIRST;
    gnmDrawCmdSetPrimitiveSetup(&cmd, &primsetup);

    const GnmBlendOp srcmult = BlendFactor(renderStates[D3DRS_SRCBLEND]);
    const GnmBlendOp dstmult = BlendFactor(renderStates[D3DRS_DESTBLEND]);
    GnmBlendControl blend;
    memset(&blend, 0, sizeof(blend));
    // A debug view has to overwrite the pixel, or blending decides what we see and the view
    // says nothing about the value being inspected.
    blend.blendenabled = debugMode == 0 && renderStates[D3DRS_ALPHABLENDENABLE] != 0;
    blend.colorfunc = GNM_COMB_DST_PLUS_SRC;
    blend.colorsrcmult = srcmult;
    blend.colordstmult = dstmult;
    blend.alphafunc = GNM_COMB_DST_PLUS_SRC;
    blend.alphasrcmult = srcmult;
    blend.alphadstmult = dstmult;
    blend.separatealphaenable = false;
    gnmDrawCmdSetBlendControl(&cmd, 0, &blend);

    GnmDbRenderControl dbrenderctrl;
    memset(&dbrenderctrl, 0, sizeof(dbrenderctrl));
    GnmDepthStencilControl depthctrl;
    memset(&depthctrl, 0, sizeof(depthctrl));
    depthctrl.depthenable = renderStates[D3DRS_ZENABLE] != 0;
    depthctrl.zwrite = renderStates[D3DRS_ZWRITEENABLE] != 0;
    depthctrl.zfunc = CompareFunction(renderStates[D3DRS_ZFUNC]);
    depthctrl.stencilfunc = GNM_DEPTH_COMPARE_NEVER;
    depthctrl.stencilbackfunc = GNM_DEPTH_COMPARE_NEVER;
    gnmDrawCmdSetDbRenderControl(&cmd, &dbrenderctrl);
    gnmDrawCmdSetDepthStencilControl(&cmd, &depthctrl);

    gnmDrawCmdSetVsShader(&cmd, &vs->registers, 0);
    gnmDrawCmdSetPsShader(&cmd, &ps->registers);
    gnmDrawCmdSetPointerUserData(&cmd, GNM_STAGE_VS, kFetchShaderReg, fetchShader);
    gnmDrawCmdSetPointerUserData(&cmd, GNM_STAGE_VS, (uint32_t)vtxBufferReg, vtxbuffers);
    gnmDrawCmdSetPointerUserData(&cmd, GNM_STAGE_VS, (uint32_t)vsSetReg, vsset);
    gnmDrawCmdSetPointerUserData(&cmd, GNM_STAGE_PS, (uint32_t)psSetReg, psset);
    gnmDrawCmdSetPsInputUsage(&cmd, gnmVsShaderExportSemanticTable(vs), vs->numexportsemantics,
                              gnmPsShaderInputSemanticTable(ps), ps->numinputsemantics);

    GnmPrimitiveType prim;
    switch (type)
    {
    case D3DPT_POINTLIST: prim = GNM_PT_POINTLIST; break;
    case D3DPT_LINELIST: prim = GNM_PT_LINELIST; break;
    case D3DPT_LINESTRIP: prim = GNM_PT_LINESTRIP; break;
    case D3DPT_TRIANGLESTRIP: prim = GNM_PT_TRISTRIP; break;
    case D3DPT_TRIANGLEFAN: prim = GNM_PT_TRIFAN; break;
    default: prim = GNM_PT_TRILIST; break;
    }
    gnmDrawCmdSetPrimitiveType(&cmd, prim);
    gnmDrawCmdDrawIndexAuto(&cmd, count);
}

class Ps4Direct3D : public IDirect3D8
{
  public:
    Ps4Direct3D() : refs(1) {}
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT GetAdapterDisplayMode(UINT, D3DDISPLAYMODE *mode)
    {
        if (mode == NULL) return E_INVALIDARG;
        mode->Width = 640;
        mode->Height = 480;
        mode->RefreshRate = 60;
        mode->Format = D3DFMT_X8R8G8B8;
        return S_OK;
    }
    HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT)
    { return S_OK; }
    HRESULT CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *parameters,
                         IDirect3DDevice8 **result)
    {
        if (result == NULL || parameters == NULL) return E_INVALIDARG;
        Ps4Device *device = new(std::nothrow) Ps4Device();
        if (device == NULL) return E_OUTOFMEMORY;
        if (!device->Setup(*parameters))
        {
            PS4_Notify("Touhou 8: the GPU renderer failed to start");
            device->Release();
            return E_FAIL;
        }
        *result = device;
        return S_OK;
    }

  private:
    ULONG refs;
};
} // namespace

// Shared with d3dx8_compat.cpp, which is the very file the Linux target uses; the names are
// kept so that adapter stays platform independent.
bool th08_linux_surface_access(IDirect3DSurface8 *surfaceRaw, LinuxSurfaceAccess *access, bool readBackbuffer)
{
    Ps4Surface *surface = static_cast<Ps4Surface *>(surfaceRaw);
    if (surface == NULL || access == NULL || surface->pixels.empty()) return false;
    if (readBackbuffer && surface->IsBackbuffer()) surface->device->ReadBackbuffer(surface);
    access->pixels = &surface->pixels[0];
    access->width = surface->width;
    access->height = surface->height;
    access->pitch = surface->pitch;
    access->format = surface->format;
    return true;
}

void th08_linux_surface_changed(IDirect3DSurface8 *surfaceRaw)
{
    Ps4Surface *surface = static_cast<Ps4Surface *>(surfaceRaw);
    if (surface == NULL) return;
    surface->dirty = true;
    if (surface->owner != NULL) surface->owner->uploaded = false;
    // Writes to the back buffer have to reach the render target now, before whatever the
    // game draws next.
    if (surface->IsBackbuffer()) surface->device->FlushBackbufferIfDirty(surface);
}

// The render audit is a Linux-only diagnostic; these exist so the shared header keeps one
// declaration per platform.
bool th08_linux_texture_region_stats(IDirect3DTexture8 *, float, float, float, float, D3DCOLOR,
                                     LinuxTextureRegionStats *)
{
    return false;
}

bool th08_linux_begin_framebuffer_probe(IDirect3DDevice8 *, int, int, int, int) { return false; }

bool th08_linux_end_framebuffer_probe(IDirect3DDevice8 *, LinuxFramebufferDeltaStats *) { return false; }

extern "C" IDirect3D8 *Direct3DCreate8(UINT sdkVersion)
{
    (void)sdkVersion;
    return new(std::nothrow) Ps4Direct3D();
}
