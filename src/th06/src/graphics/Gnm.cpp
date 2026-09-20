#ifdef TH_PS4_GNM

#include "graphics/Gnm.hpp"

#include "GameWindow.hpp"
#include "Supervisor.hpp"
#include "utils.hpp"

#include <SDL2/SDL.h>
#include <cstring>
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
extern bool g_PS4Widescreen;

namespace
{
// Per frame: vertices, uniform buffers and descriptor sets are bump-allocated out of one
// block and reused once the GPU is done with that frame.
constexpr u32 kFrameHeapSize = 8 * 1024 * 1024;
constexpr u32 kNumFrames = 2;
constexpr u32 kCmdBufferSize = 4 * 1024 * 1024;

struct Vertex
{
    f32 x, y, z;
    f32 u, v;
    u8 r, g, b, a;
};

// What the vertex shader reads (std140 layout of th_gnm.vert.glsl).
struct Constants
{
    f32 modelview[16];
    f32 projection[16];
    f32 texmatrix[16];
    f32 fogColor[4];
    f32 envDiffuse[4];
    f32 fogRange[4];
    f32 flags[4];
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

struct Texture
{
    GnmTexture desc;
    void *texels;
    u32 width, height;
    bool valid;
    // Which batch of commands last drew with it, so that editing a texture only waits for
    // the GPU when it would actually change something already queued.
    u64 lastUsedBy;
};

struct FrameHeap
{
    u8 *base;
    u32 used;
};

class Gnm : public GfxInterface
{
  public:
    bool Setup();
    ~Gnm() override;

    void SetFogRange(f32 nearPlane, f32 farPlane) override;
    void SetFogColor(ZunColor color) override;
    void ToggleVertexAttribute(u8 attr, bool enable) override;
    void SetAttributePointer(VertexAttributeArrays attr, std::size_t stride, void *ptr) override;
    void SetColorOp(TextureOpComponent component, ColorOp op) override;
    void SetTextureFactor(ZunColor factor) override;
    void SetTransformMatrix(TransformMatrix type, const ZunMatrix &matrix) override;
    void SetTextureFilter() override;
    void GetViewport(u32 *viewport) override;
    void GetDepthRange(f32 *depthRange) override;
    void SetViewport(i32 x, i32 y, i32 width, i32 height) override;
    void SetDepthRange(f32 nearPlane, f32 farPlane) override;
    void Enable(Capabilities cap) override;
    bool HasError() override;
    void SetBlendMode(BlendMode mode) override;
    void SetDepthMask(bool enable) override;
    void SetDepthFunc(DepthFunc func) override;
    void SetClearDepth(f32 depth) override;
    void SetClearColor(f32 r, f32 g, f32 b, f32 a) override;
    void Clear(u32 clearBits) override;
    GfxTextureHandle CreateTexture() override;
    void BindTexture(GfxTextureHandle handle) override;
    void DeleteTexture(GfxTextureHandle handle) override;
    void SetTextureImage(u32 width, u32 height, PixelFormat fmt, PixelDataType type, const void *data) override;
    void SetTextureSubImage(i32 xoffset, i32 yoffset, i32 width, i32 height, const void *data) override;
    void ReadPixels(i32 x, i32 y, i32 width, i32 height, const void *pixels) override;
    void Draw(PrimitiveType type, i32 start, i32 count) override;
    void SwapBuffers() override;

  private:
    void *FrameAlloc(u32 size, u32 alignment);
    void BeginCommandBuffer();
    void SubmitAndWait();
    void ApplyViewport();
    bool CreateDisplayBuffers();
    Texture *BoundTexture();
    void WaitIfQueued(const Texture &tex);

    MemoryAllocator m_Garlic = {};
    DisplayContext m_Display = {};
    GnmRenderTarget m_ColorTargets[kNumFrames] = {};
    GnmDepthRenderTarget m_DepthTarget = {};
    GnmCommandBuffer m_Cmd = {};
    void *m_CmdMem = nullptr;
    volatile u64 *m_Label = nullptr;
    FrameHeap m_Frames[kNumFrames] = {};
    u32 m_CurFrame = 0;
    u64 m_Submission = 1; // commands being recorded now; bumped on every submit

    GnmVsShader *m_Vs = nullptr;
    GnmPsShader *m_Ps = nullptr;
    void *m_FetchShader = nullptr;
    i32 m_VtxBufferReg = -1;
    i32 m_VsSetReg = -1;
    i32 m_PsSetReg = -1;

    // Drawing state, mirroring what the GL backends keep in GL itself.
    ZunMatrix m_Model, m_View, m_Projection, m_TextureMatrix;
    ZunColor m_FogColor = 0;
    f32 m_FogNear = 0.0f, m_FogFar = 1.0f;
    ZunColor m_TextureFactor = 0xFFFFFFFF;
    ColorOp m_ColorOp = COLOR_OP_MODULATE;
    bool m_UseTexCoord = false, m_UseDiffuse = false;
    bool m_BlendEnabled = false, m_DepthTestEnabled = false, m_DepthMask = true;
    BlendMode m_BlendMode = BLEND_INV_SRC_ALPHA;
    DepthFunc m_DepthFunc = DEPTH_FUNC_LEQUAL;
    f32 m_DepthNear = 0.0f, m_DepthFar = 1.0f;
    i32 m_ViewportX = 0, m_ViewportY = 0, m_ViewportW = GAME_WINDOW_WIDTH, m_ViewportH = GAME_WINDOW_HEIGHT;
    f32 m_ClearColor[4] = {0, 0, 0, 1};
    f32 m_ClearDepth = 1.0f;

    const void *m_PositionData = nullptr;
    const void *m_TexCoordData = nullptr;
    const void *m_DiffuseData = nullptr;
    std::size_t m_PositionStride = 0, m_TexCoordStride = 0, m_DiffuseStride = 0;

    std::vector<Texture> m_Textures;
    std::vector<u32> m_FreeTextures;
    u32 m_BoundTexture = 0; // index into m_Textures; the vector moves when it grows
    bool m_NoVertexBuffer = false, m_NoFog = false;
    bool m_Error = false;
};

Gnm *g_Backend = nullptr;

// The shader's own metadata misreports this one: its first instruction is
// "s_swappc_b64 s[0:1], s[0:1]", so the fetch shader pointer belongs in registers 0-1.
constexpr u32 kFetchShaderReg = 0;

i32 FindUsageSlot(const GnmInputUsageSlot *slots, u32 count, u8 usage)
{
    for (u32 i = 0; i < count; i++)
    {
        if (slots[i].usagetype == usage)
        {
            return slots[i].startregister;
        }
    }
    return -1;
}

inline f32 ByteToFloat(u32 v)
{
    return (f32)v / 255.0f;
}

SDL_PixelFormatEnum SDLFormatOf(PixelFormat fmt, PixelDataType type)
{
    switch (type)
    {
    case PIXEL_UNSIGNED_BYTE:
        return fmt == PIXEL_RGB ? SDL_PIXELFORMAT_RGB24 : SDL_PIXELFORMAT_RGBA32;
    case PIXEL_UNSIGNED_SHORT_4_4_4_4:
        return SDL_PIXELFORMAT_RGBA4444;
    case PIXEL_UNSIGNED_SHORT_5_5_5_1:
        return SDL_PIXELFORMAT_RGBA5551;
    case PIXEL_UNSIGNED_SHORT_5_6_5:
        return SDL_PIXELFORMAT_RGB565;
    }
    return SDL_PIXELFORMAT_RGBA32;
}

bool Gnm::Setup()
{
    m_Garlic = memalloc_init(128 * 1024 * 1024,
                             ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_READ |
                                 ORBIS_KERNEL_PROT_GPU_WRITE,
                             ORBIS_KERNEL_WC_GARLIC);

    if (!displayctx_init(&m_Display))
    {
        PS4_Log("gnm: display init failed");
        return false;
    }
    if (!CreateDisplayBuffers())
    {
        return false;
    }
    if (!initdepthtarget(&m_DepthTarget, &m_Garlic, m_Display.screenw, m_Display.screenh, m_Display.screenw, 1, 1,
                         GNM_Z_32_FLOAT, GNM_STENCIL_INVALID, gnmGpuMode()))
    {
        PS4_Log("gnm: depth target failed");
        return false;
    }
    if (!initclearutility(&m_Garlic))
    {
        PS4_Log("gnm: clear utility failed");
        return false;
    }

    if (!loadvshader(&m_Vs, &m_Garlic, "/app0/assets/misc/th_gnm.vert.sb") ||
        !loadpshader(&m_Ps, &m_Garlic, "/app0/assets/misc/th_gnm.frag.sb"))
    {
        PS4_Log("gnm: loading shaders failed");
        return false;
    }

    const GnmInputUsageSlot *vsslots = gnmVsShaderInputUsageSlotTable(m_Vs);
    const GnmInputUsageSlot *psslots = gnmPsShaderInputUsageSlotTable(m_Ps);
    m_VtxBufferReg = FindUsageSlot(vsslots, m_Vs->common.numinputusageslots, GNM_SHINPUTUSAGE_PTR_VERTEXBUFFERTABLE);
    m_VsSetReg = FindUsageSlot(vsslots, m_Vs->common.numinputusageslots, GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE);
    m_PsSetReg = FindUsageSlot(psslots, m_Ps->common.numinputusageslots, GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE);
    if (m_VtxBufferReg < 0 || m_VsSetReg < 0 || m_PsSetReg < 0)
    {
        PS4_Log("gnm: shaders don't ask for the pointers we have (%d %d %d)", m_VtxBufferReg, m_VsSetReg, m_PsSetReg);
        return false;
    }

    const GnmFetchShaderCreateInfo fetchci = {
        .regs = &m_Vs->registers,
        .inputusages = vsslots,
        .numinputusages = m_Vs->common.numinputusageslots,
        .vtxinputs = gnmVsShaderInputSemanticTable(m_Vs),
        .numvtxinputs = m_Vs->numinputsemantics,
    };
    u32 fetchsize = 0;
    if (gnmFetchShaderCalcSize(&fetchsize, &fetchci) != GNM_ERROR_OK)
    {
        PS4_Log("gnm: fetch shader size failed");
        return false;
    }
    m_FetchShader = memalloc_alloc(&m_Garlic, fetchsize, GNM_ALIGNMENT_FETCHSHADER_BYTES);
    GnmFetchShaderResults fetchres = {};
    if (!m_FetchShader || gnmCreateFetchShader(m_FetchShader, fetchsize, &fetchci, &fetchres) != GNM_ERROR_OK)
    {
        PS4_Log("gnm: fetch shader creation failed");
        return false;
    }
    gnmVsRegsSetFetchShaderModifier(&m_Vs->registers, &fetchres);

    m_CmdMem = memalloc_alloc(&m_Garlic, kCmdBufferSize, GNM_ALIGNMENT_BUFFER_BYTES);
    m_Label = (volatile u64 *)memalloc_alloc(&m_Garlic, sizeof(u64), sizeof(u64));
    for (u32 i = 0; i < kNumFrames; i++)
    {
        m_Frames[i].base = (u8 *)memalloc_alloc(&m_Garlic, kFrameHeapSize, GNM_ALIGNMENT_BUFFER_BYTES);
        if (!m_Frames[i].base)
        {
            PS4_Log("gnm: frame heap %u failed", i);
            return false;
        }
    }
    if (!m_CmdMem || !m_Label)
    {
        PS4_Log("gnm: command buffer allocation failed");
        return false;
    }

    m_Model.Identity();
    m_View.Identity();
    m_Projection.Identity();
    m_TextureMatrix.Identity();
    m_Textures.resize(1); // handle 0 means "no texture"
    m_NoVertexBuffer = g_Supervisor.cfg.opts & (1 << GCOS_DONT_USE_VERTEX_BUF);
    m_NoFog = g_Supervisor.cfg.opts & (1 << GCOS_DONT_USE_FOG);

    BeginCommandBuffer();
    PS4_Log("gnm: ready (%ux%u, vtx reg %d, vs set reg %d, ps set reg %d)", m_Display.screenw, m_Display.screenh,
            m_VtxBufferReg, m_VsSetReg, m_PsSetReg);
    return true;
}

Gnm::~Gnm()
{
    displayctx_destroy(&m_Display);
    memalloc_destroy(&m_Garlic);
}

void *Gnm::FrameAlloc(u32 size, u32 alignment)
{
    FrameHeap &heap = m_Frames[m_CurFrame];
    const u32 offset = (heap.used + alignment - 1) / alignment * alignment;
    if (offset + size > kFrameHeapSize)
    {
        // Out of scratch for this frame: drop the draw rather than scribble over the data
        // the GPU is still reading.
        static u32 s_Reported;
        if (s_Reported++ < 5)
        {
            PS4_Log("gnm: frame heap exhausted (%u bytes used)", heap.used);
        }
        return nullptr;
    }
    heap.used = offset + size;
    return heap.base + offset;
}

void Gnm::BeginCommandBuffer()
{
    m_Cmd = gnmCmdInit(m_CmdMem, kCmdBufferSize, nullptr, nullptr);
    gnmDrawCmdSetRenderTarget(&m_Cmd, 0, &m_ColorTargets[m_CurFrame]);
    gnmDrawCmdSetRenderTargetMask(&m_Cmd, 0xf);
    gnmDrawCmdSetDepthRenderTarget(&m_Cmd, &m_DepthTarget);
    ApplyViewport();
}

bool Gnm::CreateDisplayBuffers()
{
    // Built by hand instead of with the example's helper, which picks a tiled layout: the
    // game reads the screen back to draw the pause overlay over it, and only a linear
    // buffer can be read as plain rows of pixels.
    //
    // UNORM, not SRGB: the game's colours are already sRGB, like on a plain GL framebuffer,
    // so letting the GPU encode them again washes the picture out. Scan-out is told they are
    // sRGB, which is exactly how a GL framebuffer ends up on screen.
    void *addrs[kNumFrames] = {};
    for (u32 i = 0; i < kNumFrames; i++)
    {
        const GnmRenderTargetCreateInfo ci = {
            .colorfmt = GNM_FMT_R8G8B8A8_UNORM,
            .width = m_Display.screenw,
            .height = m_Display.screenh,
            .pitch = 0,
            .numslices = 1,
            .numsamples = 1,
            .numfragments = 1,
            .colortilemodehint = GNM_TM_DISPLAY_LINEAR_ALIGNED,
            .mingpumode = gnmGpuMode(),
        };
        if (gnmCreateRenderTarget(&m_ColorTargets[i], &ci) != GNM_ERROR_OK)
        {
            PS4_Log("gnm: creating display buffer %u failed", i);
            return false;
        }
        u64 size = 0;
        u32 align = 0;
        if (gnmRtCalcByteSize(&size, &align, &m_ColorTargets[i]) != GNM_ERROR_OK)
        {
            PS4_Log("gnm: sizing display buffer %u failed", i);
            return false;
        }
        addrs[i] = memalloc_alloc(&m_Garlic, size, align);
        if (!addrs[i])
        {
            PS4_Log("gnm: out of memory for display buffer %u", i);
            return false;
        }
        std::memset(addrs[i], 0, size);
        gnmRtSetBaseAddr(&m_ColorTargets[i], addrs[i]);
    }

    OrbisVideoOutBufferAttribute attr;
    std::memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
                                  m_Display.screenw, m_Display.screenh, gnmRtGetPitch(&m_ColorTargets[0]));
    const i32 rc = sceVideoOutRegisterBuffers(m_Display.videohandle, 0, addrs, kNumFrames, &attr);
    if (rc < 0)
    {
        PS4_Log("gnm: sceVideoOutRegisterBuffers failed %#x", rc);
        return false;
    }
    return true;
}

void Gnm::ApplyViewport()
{
    // GL puts the viewport origin at the bottom left; the GPU scans out from the top, and the
    // vertex shader flips Y to match, so the rectangle has to be flipped too.
    const i32 top = (i32)m_Display.screenh - (m_ViewportY + m_ViewportH);
    const f32 zscale = (m_DepthFar - m_DepthNear) * 0.5f;
    const f32 zoffset = (m_DepthFar + m_DepthNear) * 0.5f;
    setupviewport(&m_Cmd, m_ViewportX, top, m_ViewportX + m_ViewportW, top + m_ViewportH, zscale, zoffset);
}

void Gnm::SetFogRange(f32 nearPlane, f32 farPlane)
{
    m_FogNear = nearPlane;
    m_FogFar = farPlane;
}

void Gnm::SetFogColor(ZunColor color)
{
    m_FogColor = color;
}

void Gnm::ToggleVertexAttribute(u8 attr, bool enable)
{
    if (attr & VERTEX_ATTR_TEX_COORD)
    {
        m_UseTexCoord = enable;
    }
    if (attr & VERTEX_ATTR_DIFFUSE)
    {
        m_UseDiffuse = enable;
    }
}

void Gnm::SetAttributePointer(VertexAttributeArrays attr, std::size_t stride, void *ptr)
{
    switch (attr)
    {
    case VERTEX_ARRAY_POSITION:
        m_PositionData = ptr;
        m_PositionStride = stride;
        break;
    case VERTEX_ARRAY_TEX_COORD:
        m_TexCoordData = ptr;
        m_TexCoordStride = stride;
        break;
    case VERTEX_ARRAY_DIFFUSE:
        m_DiffuseData = ptr;
        m_DiffuseStride = stride;
        break;
    }
}

void Gnm::SetColorOp(TextureOpComponent component, ColorOp op)
{
    if (component == COMPONENT_ALPHA)
    {
        return;
    }
    m_ColorOp = op;
}

void Gnm::SetTextureFactor(ZunColor factor)
{
    m_TextureFactor = factor;
}

void Gnm::SetTransformMatrix(TransformMatrix type, const ZunMatrix &matrix)
{
    switch (type)
    {
    case MATRIX_MODEL:
        m_Model = matrix;
        break;
    case MATRIX_VIEW:
        m_View = matrix;
        break;
    case MATRIX_PROJECTION:
        m_Projection = matrix;
        break;
    case MATRIX_TEXTURE:
        m_TextureMatrix = matrix;
        break;
    }
}

void Gnm::SetTextureFilter()
{
}

void Gnm::GetViewport(u32 *viewport)
{
    viewport[0] = (u32)m_ViewportX;
    viewport[1] = (u32)m_ViewportY;
    viewport[2] = (u32)m_ViewportW;
    viewport[3] = (u32)m_ViewportH;
}

void Gnm::GetDepthRange(f32 *depthRange)
{
    depthRange[0] = m_DepthNear;
    depthRange[1] = m_DepthFar;
}

void Gnm::SetViewport(i32 x, i32 y, i32 width, i32 height)
{
    m_ViewportX = x;
    m_ViewportY = y;
    m_ViewportW = width;
    m_ViewportH = height;
    ApplyViewport();
}

void Gnm::SetDepthRange(f32 nearPlane, f32 farPlane)
{
    m_DepthNear = nearPlane;
    m_DepthFar = farPlane;
    ApplyViewport();
}

void Gnm::Enable(Capabilities cap)
{
    switch (cap)
    {
    case CAPS_BLEND:
        m_BlendEnabled = true;
        break;
    case CAPS_DEPTH_TEST:
        m_DepthTestEnabled = true;
        break;
    }
}

bool Gnm::HasError()
{
    return m_Error;
}

void Gnm::SetBlendMode(BlendMode mode)
{
    m_BlendMode = mode;
}

void Gnm::SetDepthMask(bool enable)
{
    m_DepthMask = enable;
}

void Gnm::SetDepthFunc(DepthFunc func)
{
    m_DepthFunc = func;
}

void Gnm::SetClearDepth(f32 depth)
{
    m_ClearDepth = depth;
}

void Gnm::SetClearColor(f32 r, f32 g, f32 b, f32 a)
{
    m_ClearColor[0] = r;
    m_ClearColor[1] = g;
    m_ClearColor[2] = b;
    m_ClearColor[3] = a;
}

void Gnm::Clear(u32 clearBits)
{
    const bool pillarboxed = m_ViewportW < (i32)m_Display.screenw || m_ViewportH < (i32)m_Display.screenh;
    if (clearBits & CLEAR_COLOR_BUFFER)
    {
        if (pillarboxed)
        {
            // The bars beside a 4:3 playfield are never drawn to, so they'd keep whatever
            // colour the stage clears with. Black them, then clear only the playfield.
            const f32 black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            clearcolortarget(&m_Cmd, &m_ColorTargets[m_CurFrame], black);
            const i32 top = (i32)m_Display.screenh - (m_ViewportY + m_ViewportH);
            gnmDrawCmdSetScreenScissor(&m_Cmd, m_ViewportX, top, m_ViewportX + m_ViewportW, top + m_ViewportH);
        }
        clearcolortarget(&m_Cmd, &m_ColorTargets[m_CurFrame], m_ClearColor);
        if (pillarboxed)
        {
            gnmDrawCmdSetScreenScissor(&m_Cmd, 0, 0, (i32)m_Display.screenw, (i32)m_Display.screenh);
        }
    }
    if (clearBits & CLEAR_DEPTH_BUFFER)
    {
        cleardepthtarget(&m_Cmd, &m_DepthTarget, m_ClearDepth);
    }
    // Clearing sets its own controls and viewport; put ours back.
    ApplyViewport();
    gnmDrawCmdSetRenderTarget(&m_Cmd, 0, &m_ColorTargets[m_CurFrame]);
    gnmDrawCmdSetRenderTargetMask(&m_Cmd, 0xf);
    gnmDrawCmdSetDepthRenderTarget(&m_Cmd, &m_DepthTarget);
}

GfxTextureHandle Gnm::CreateTexture()
{
    if (!m_FreeTextures.empty())
    {
        const u32 id = m_FreeTextures.back();
        m_FreeTextures.pop_back();
        m_Textures[id] = Texture{};
        return GfxTextureHandle(id);
    }
    m_Textures.push_back(Texture{});
    return GfxTextureHandle((u32)m_Textures.size() - 1);
}

void Gnm::BindTexture(GfxTextureHandle handle)
{
    m_BoundTexture = handle.id < m_Textures.size() ? handle.id : 0;
}

void Gnm::WaitIfQueued(const Texture &tex)
{
    if (tex.lastUsedBy == m_Submission)
    {
        SubmitAndWait();
    }
}

Texture *Gnm::BoundTexture()
{
    return m_BoundTexture != 0 ? &m_Textures[m_BoundTexture] : nullptr;
}

void Gnm::DeleteTexture(GfxTextureHandle handle)
{
    if (handle.id == 0 || handle.id >= m_Textures.size())
    {
        return;
    }
    Texture &tex = m_Textures[handle.id];
    if (m_BoundTexture == handle.id)
    {
        m_BoundTexture = 0;
    }
    if (tex.texels)
    {
        WaitIfQueued(tex);
        memalloc_free(&m_Garlic, tex.texels);
    }
    tex = Texture{};
    m_FreeTextures.push_back(handle.id);
}

void Gnm::SetTextureImage(u32 width, u32 height, PixelFormat fmt, PixelDataType type, const void *data)
{
    Texture *bound = BoundTexture();
    if (!bound || width == 0 || height == 0)
    {
        return;
    }
    Texture &tex = *bound;
    const u32 bytes = width * height * 4;
    if (tex.texels && (tex.width != width || tex.height != height))
    {
        WaitIfQueued(tex);
        memalloc_free(&m_Garlic, tex.texels);
        tex.texels = nullptr;
    }
    else if (tex.texels)
    {
        WaitIfQueued(tex);
    }
    if (!tex.texels)
    {
        tex.texels = memalloc_alloc(&m_Garlic, bytes, GNM_ALIGNMENT_SHADER_BYTES);
        if (!tex.texels)
        {
            PS4_Log("gnm: out of texture memory for %ux%u", width, height);
            tex.valid = false;
            return;
        }
    }
    tex.width = width;
    tex.height = height;

    if (data)
    {
        u32 bpp = 2;
        if (type == PIXEL_UNSIGNED_BYTE)
        {
            bpp = fmt == PIXEL_RGB ? 3 : 4;
        }
        SDL_ConvertPixels(width, height, SDLFormatOf(fmt, type), data, width * bpp, SDL_PIXELFORMAT_ABGR8888,
                          tex.texels, width * 4);
    }
    else
    {
        std::memset(tex.texels, 0, bytes);
    }

    const GnmTextureCreateInfo ci = {
        .format = GNM_FMT_R8G8B8A8_UNORM,
        .texturetype = GNM_TEXTURE_2D,
        .width = width,
        .height = height,
        .depth = 1,
        .pitch = width,
        .nummiplevels = 1,
        .numslices = 1,
        .numfragments = 1,
        .tilemodehint = GNM_TM_DISPLAY_LINEAR_GENERAL,
        .mingpumode = GNM_GPU_BASE,
    };
    tex.valid = gnmCreateTexture(&tex.desc, &ci) == GNM_ERROR_OK;
    if (tex.valid)
    {
        gnmTexSetBaseAddress(&tex.desc, tex.texels);
    }
}

void Gnm::SetTextureSubImage(i32 xoffset, i32 yoffset, i32 width, i32 height, const void *data)
{
    Texture *bound = BoundTexture();
    if (!bound || !bound->texels || !data)
    {
        return;
    }
    Texture &tex = *bound;
    // Only a texture this batch already draws with can be seen changing mid-flight.
    WaitIfQueued(tex);
    SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_RGB24, data, width * 3, SDL_PIXELFORMAT_ABGR8888,
                      (u8 *)tex.texels + ((u32)yoffset * tex.width + (u32)xoffset) * 4, tex.width * 4);
}

void Gnm::ReadPixels(i32 x, i32 y, i32 width, i32 height, const void *pixels)
{
    SubmitAndWait();
    const u32 *src = (const u32 *)gnmRtGetBaseAddr(&m_ColorTargets[m_CurFrame]);
    const u32 pitch = gnmRtGetPitch(&m_ColorTargets[m_CurFrame]);
    u8 *dst = (u8 *)pixels;
    for (i32 row = 0; row < height; row++)
    {
        // Callers expect the GL layout, whose first row is the bottom one.
        const i32 srcY = (i32)m_Display.screenh - 1 - (y + row);
        if (srcY < 0 || srcY >= (i32)m_Display.screenh)
        {
            continue;
        }
        std::memcpy(dst + (std::size_t)row * width * 4, src + (std::size_t)srcY * pitch + x, (std::size_t)width * 4);
    }
}

void Gnm::Draw(PrimitiveType type, i32 start, i32 count)
{
    if (count <= 0 || !m_PositionData)
    {
        return;
    }

    Vertex *verts = (Vertex *)FrameAlloc((u32)count * sizeof(Vertex), GNM_ALIGNMENT_BUFFER_BYTES);
    Constants *constants = (Constants *)FrameAlloc(sizeof(Constants), GNM_ALIGNMENT_BUFFER_BYTES);
    VsDescSet *vsset = (VsDescSet *)FrameAlloc(sizeof(VsDescSet), GNM_ALIGNMENT_BUFFER_BYTES);
    PsDescSet *psset = (PsDescSet *)FrameAlloc(sizeof(PsDescSet), GNM_ALIGNMENT_BUFFER_BYTES);
    GnmBuffer *vtxbuffers = (GnmBuffer *)FrameAlloc(sizeof(GnmBuffer) * 3, GNM_ALIGNMENT_BUFFER_BYTES);
    if (!verts || !constants || !vsset || !psset || !vtxbuffers)
    {
        return;
    }

    for (i32 i = 0; i < count; i++)
    {
        const i32 index = start + i;
        const ZunVec3 &pos = *(const ZunVec3 *)((const u8 *)m_PositionData + m_PositionStride * index);
        Vertex &v = verts[i];
        v.x = pos.x;
        v.y = pos.y;
        v.z = pos.z;
        if (m_UseTexCoord && m_TexCoordData)
        {
            const ZunVec2 &uv = *(const ZunVec2 *)((const u8 *)m_TexCoordData + m_TexCoordStride * index);
            v.u = uv.x;
            v.v = uv.y;
        }
        else
        {
            v.u = v.v = 0.0f;
        }
        if (m_UseDiffuse && m_DiffuseData)
        {
            // ZunColor is ARGB; the vertex format below reads R, G, B, A in memory order.
            const ZunColor color = *(const ZunColor *)((const u8 *)m_DiffuseData + m_DiffuseStride * index);
            v.r = (u8)(color >> 16);
            v.g = (u8)(color >> 8);
            v.b = (u8)color;
            v.a = (u8)(color >> 24);
        }
        else
        {
            v.r = v.g = v.b = v.a = 0xFF;
        }
    }

    const ZunMatrix modelview = m_View * m_Model;
    std::memcpy(constants->modelview, modelview.m, sizeof(constants->modelview));
    std::memcpy(constants->projection, m_Projection.m, sizeof(constants->projection));
    std::memcpy(constants->texmatrix, m_TextureMatrix.m, sizeof(constants->texmatrix));
    constants->fogColor[0] = ByteToFloat((m_FogColor >> 16) & 0xFF);
    constants->fogColor[1] = ByteToFloat((m_FogColor >> 8) & 0xFF);
    constants->fogColor[2] = ByteToFloat(m_FogColor & 0xFF);
    constants->fogColor[3] = ByteToFloat(m_FogColor >> 24);
    constants->envDiffuse[0] = ByteToFloat((m_TextureFactor >> 16) & 0xFF);
    constants->envDiffuse[1] = ByteToFloat((m_TextureFactor >> 8) & 0xFF);
    constants->envDiffuse[2] = ByteToFloat(m_TextureFactor & 0xFF);
    constants->envDiffuse[3] = ByteToFloat(m_TextureFactor >> 24);
    constants->fogRange[0] = m_FogNear;
    constants->fogRange[1] = m_FogFar;
    constants->fogRange[2] = constants->fogRange[3] = 0.0f;

    Texture *bound = BoundTexture();
    const bool useTexture = m_UseTexCoord && bound && bound->valid;
    if (useTexture)
    {
        bound->lastUsedBy = m_Submission;
    }
    constants->flags[0] = (f32)m_ColorOp;
    constants->flags[1] = useTexture ? 1.0f : 0.0f;
    constants->flags[2] = m_NoVertexBuffer ? 1.0f : 0.0f;
    // The GL backends compile fog and the vertex buffer choice into the shader; here both
    // are flags, since one shader has to serve every draw.
    constants->flags[3] = m_NoFog ? 1.0f : 0.0f;

    *vsset = VsDescSet{gnmCreateConstBuffer(constants, sizeof(Constants))};
    if (useTexture)
    {
        psset->texture = bound->desc;
    }
    else
    {
        std::memset(&psset->texture, 0, sizeof(psset->texture));
    }
    psset->sampler = GnmSampler{};
    psset->sampler.xyminfilter = GNM_FILTER_BILINEAR;
    psset->sampler.xymagfilter = GNM_FILTER_BILINEAR;
    psset->sampler.mipfilter = GNM_MIPFILTER_NONE;
    psset->sampler.zfilter = GNM_ZFILTER_POINT;
    psset->sampler.maxlod = 0;

    const u32 stride = sizeof(Vertex);
    vtxbuffers[0] = gnmCreateVertexBuffer(verts, GNM_FMT_R32G32B32_FLOAT, stride, (u32)count);
    vtxbuffers[1] = gnmCreateVertexBuffer((u8 *)verts + 12, GNM_FMT_R32G32_FLOAT, stride, (u32)count);
    vtxbuffers[2] = gnmCreateVertexBuffer((u8 *)verts + 20, GNM_FMT_R8G8B8A8_UNORM, stride, (u32)count);

    const GnmPrimitiveSetup primsetup = {
        .cullmode = GNM_CULL_NONE,
        .frontface = GNM_FACE_CCW,
        .frontmode = GNM_FILL_SOLID,
        .backmode = GNM_FILL_SOLID,
        .provokemode = GNM_PROVOKINGVTX_FIRST,
    };
    gnmDrawCmdSetPrimitiveSetup(&m_Cmd, &primsetup);

    const GnmBlendOp dstmult = m_BlendMode == BLEND_ONE ? GNM_BLEND_ONE : GNM_BLEND_ONE_MINUS_SRC_ALPHA;
    const GnmBlendControl blend = {
        .blendenabled = m_BlendEnabled,
        .colorfunc = GNM_COMB_DST_PLUS_SRC,
        .colorsrcmult = GNM_BLEND_SRC_ALPHA,
        .colordstmult = dstmult,
        .alphafunc = GNM_COMB_DST_PLUS_SRC,
        .alphasrcmult = GNM_BLEND_SRC_ALPHA,
        .alphadstmult = dstmult,
        .separatealphaenable = false,
    };
    gnmDrawCmdSetBlendControl(&m_Cmd, 0, &blend);

    const GnmDbRenderControl dbrenderctrl = {};
    GnmDepthStencilControl depthctrl = {};
    depthctrl.depthenable = m_DepthTestEnabled;
    depthctrl.zwrite = m_DepthMask;
    depthctrl.zfunc = m_DepthFunc == DEPTH_FUNC_LEQUAL ? GNM_DEPTH_COMPARE_LESSEQUAL : GNM_DEPTH_COMPARE_ALWAYS;
    depthctrl.stencilfunc = GNM_DEPTH_COMPARE_NEVER;
    depthctrl.stencilbackfunc = GNM_DEPTH_COMPARE_NEVER;
    gnmDrawCmdSetDbRenderControl(&m_Cmd, &dbrenderctrl);
    gnmDrawCmdSetDepthStencilControl(&m_Cmd, &depthctrl);

    gnmDrawCmdSetVsShader(&m_Cmd, &m_Vs->registers, 0);
    gnmDrawCmdSetPsShader(&m_Cmd, &m_Ps->registers);
    gnmDrawCmdSetPointerUserData(&m_Cmd, GNM_STAGE_VS, kFetchShaderReg, m_FetchShader);
    gnmDrawCmdSetPointerUserData(&m_Cmd, GNM_STAGE_VS, (u32)m_VtxBufferReg, vtxbuffers);
    gnmDrawCmdSetPointerUserData(&m_Cmd, GNM_STAGE_VS, (u32)m_VsSetReg, vsset);
    gnmDrawCmdSetPointerUserData(&m_Cmd, GNM_STAGE_PS, (u32)m_PsSetReg, psset);
    gnmDrawCmdSetPsInputUsage(&m_Cmd, gnmVsShaderExportSemanticTable(m_Vs), m_Vs->numexportsemantics,
                              gnmPsShaderInputSemanticTable(m_Ps), m_Ps->numinputsemantics);

    gnmDrawCmdSetPrimitiveType(&m_Cmd, type == PRIM_TRIANGLE_STRIP ? GNM_PT_TRISTRIP : GNM_PT_TRILIST);
    gnmDrawCmdDrawIndexAuto(&m_Cmd, (u32)count);
}

void Gnm::SubmitAndWait()
{
    if (m_Cmd.cmdptr == m_Cmd.beginptr)
    {
        return;
    }
    *m_Label = 0;
    gnmDrawCmdEventWriteEop(&m_Cmd, GNM_CACHE_FLUSH_TS, (u64)m_Label, GNM_DATA_SEL_SEND_DATA64, 1);

    void *dcbaddr = (void *)m_Cmd.beginptr;
    u32 dcbsize = (u32)((m_Cmd.cmdptr - m_Cmd.beginptr) * sizeof(u32));
    const i32 rc = sceGnmSubmitCommandBuffers(1, &dcbaddr, &dcbsize, nullptr, nullptr);

    // The label must be read as volatile, or the compiler hoists the load and this spins
    // forever even though the GPU has long finished.
    const u64 start = sceKernelGetProcessTime();
    while (*m_Label != 1)
    {
        if (sceKernelGetProcessTime() - start > 1000000)
        {
            static u32 s_Reported;
            if (s_Reported++ < 5)
            {
                PS4_Log("gnm: GPU didn't finish (submit rc %#x, label %llu)", rc, (unsigned long long)*m_Label);
            }
            break;
        }
    }
    m_Submission++;
    BeginCommandBuffer();
}

void Gnm::SwapBuffers()
{
    SubmitAndWait();

    if (!displayctx_flip(&m_Display, m_CurFrame))
    {
        static u32 s_Reported;
        if (s_Reported++ < 5)
        {
            PS4_Log("gnm: flip failed");
        }
    }
    sceGnmSubmitDone();

    static bool s_SplashHidden;
    if (!s_SplashHidden)
    {
        s_SplashHidden = true;
        PS4_Log("gnm: first frame shown, hiding splash: %#x", sceSystemServiceHideSplashScreen());
    }

    m_CurFrame = (m_CurFrame + 1) % kNumFrames;
    m_Frames[m_CurFrame].used = 0;
    BeginCommandBuffer();
}
} // namespace

namespace GnmBackend
{
GfxInterface *Init()
{
    Gnm *backend = new Gnm();
    if (!backend->Setup())
    {
        delete backend;
        return nullptr;
    }
    g_Backend = backend;
    return backend;
}
} // namespace GnmBackend

#endif // TH_PS4_GNM
