#ifdef TH_PS4_GNM

#include "graphics/Gnm.hpp"

#include "AnmManager.hpp"
#include "GameWindow.hpp"
#include "Supervisor.hpp"
#include "utils.hpp"

#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
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
    f32 textureFactor[4];
    f32 fogRange[4];
    f32 flags0[4]; // RGB op, alpha op, texture argument, texture enabled
    f32 flags1[4]; // screen space, alpha test, alpha reference, fog enabled
    f32 viewport[4];
    f32 debug[4]; // x: shader debug view, 0 = off
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

class Gnm : public ZunGraphics
{
  public:
    bool Setup();
    ~Gnm() override;
    void Exit() override;
    RendererType GetType() override;

    void SetFogRange(f32 nearPlane, f32 farPlane) override;
    void SetFogColor(ZunColor color) override;
    void SetColorOp(TextureOpComponent component, ColorOp op) override;
    void SetTextureFactor(ZunColor factor) override;
    void SetTextureArg(TextureArg arg) override;
    void SetTransformMatrix(TransformMatrix type, const ZunMatrix &matrix) override;
    void SetTextureFilter() override;
    void GetViewport(ZunViewport &viewport) override;
    void SetViewport(const ZunViewport &viewport) override;
    void Enable(Capabilities cap) override;
    void Disable(Capabilities cap) override;
    void SetBlendMode(BlendMode srcMode, BlendMode dstMode) override;
    void SetDepthMask(bool enable) override;
    void SetDepthFunc(DepthFunc func) override;
    void SetClearDepth(f32 depth) override;
    void SetClearColor(ZunColor color) override;
    void SetAlphaTestRef(u8 ref) override;
    void Clear(u32 clearBits) override;
    GfxTextureHandle CreateTexture() override;
    void BindTexture(GfxTextureHandle handle) override;
    void DeleteTexture(GfxTextureHandle handle) override;
    void SetTextureImage(u32 width, u32 height, PixelFormat fmt, PixelDataType type, const void *data) override;
    void SetTextureSubImage(i32 xoffset, i32 yoffset, i32 width, i32 height, const void *data) override;
    void ReadPixels(i32 x, i32 y, i32 width, i32 height, void *pixels) override;
    void DrawPrimitive(PrimitiveType type, i32 startVertex, i32 primitiveCount) override;
    void DrawPrimitiveUP(PrimitiveType type, i32 primitiveCount, const void *vertexData,
                         i32 vertexStride) override;
    void SwapBuffers() override;

  private:
    void *FrameAlloc(u32 size, u32 alignment);
    void BeginCommandBuffer();
    void SubmitAndWait();
    void ApplyViewport();
    bool CreateDisplayBuffers();
    Texture *BoundTexture();
    void WaitIfQueued(const Texture &tex);
    void DrawVertices(PrimitiveType type, const Vertex *vertices, i32 vertexCount,
                      bool screenSpace, bool hasTexCoords);
    static i32 VertexCount(PrimitiveType type, i32 primitiveCount);

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
    ZunColor m_FogColor = {};
    f32 m_FogNear = 0.0f, m_FogFar = 1.0f;
    ZunColor m_TextureFactor = {0xFFFFFFFF};
    ColorOp m_ColorOpRgb = COLOR_OP_MODULATE;
    ColorOp m_ColorOpAlpha = COLOR_OP_MODULATE;
    TextureArg m_TextureArg = TEX_ARG_DIFFUSE;
    bool m_BlendEnabled = false, m_AlphaTestEnabled = false;
    bool m_DepthTestEnabled = false, m_FogEnabled = false, m_DepthMask = true;
    BlendMode m_BlendSrc = BLEND_ALPHA, m_BlendDst = BLEND_ALPHA;
    DepthFunc m_DepthFunc = DEPTH_FUNC_LEQUAL;
    ZunViewport m_Viewport = {0, 0, 640, 480, 0.0f, 1.0f};
    f32 m_ClearColor[4] = {0, 0, 0, 1};
    f32 m_ClearDepth = 1.0f;
    u8 m_AlphaRef = 0;
    i32 m_DebugMode = 0; // shader debug view, picked by a file at startup
    bool m_AspectInitialized = false;
    bool m_LastWidescreen = false;
    // BeginCommandBuffer runs on every submit, several times per frame; the wipe below has
    // to happen only on the first one of a frame or it erases a half-drawn picture.
    bool m_FrameStart = true;
    u32 m_FullClearFrames = 0;

    std::vector<Texture> m_Textures;
    std::vector<u32> m_FreeTextures;
    u32 m_BoundTexture = 0; // index into m_Textures; the vector moves when it grows
    bool m_Exited = false;
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

// Dumps what a handful of draws actually asked for, on two frames a while into play, so the
// geometry the GPU receives can be compared against what PCB intends to draw.
namespace
{
u64 g_FrameIndex;
u32 g_DrawsThisFrame;
u32 g_ClearsThisFrame;
u32 g_DroppedThisFrame;    // draws refused before they reached the GPU
u32 g_StrideCounts[4];     // xyzrhw+uv, xyz+uv, xyzrhw, unknown
constexpr u64 kDumpFrames[] = {600, 1800};
bool DumpingThisFrame()
{
#ifdef TH_GNM_DEBUG_VIEWS
    for (u64 f : kDumpFrames)
    {
        if (g_FrameIndex == f)
        {
            return true;
        }
    }
#endif
    return false;
}
} // namespace

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

    BeginCommandBuffer();
#ifdef TH_GNM_DEBUG_VIEWS
    // Diagnostic builds can pick a shader view without rebuilding. Release builds ignore
    // this file so a stale /data/touhou/th07/gfxdebug cannot tint normal gameplay.
    if (FILE *f = std::fopen(TH_PS4_GAME_DIR "/gfxdebug", "rb"))
    {
        char c = '0';
        if (std::fread(&c, 1, 1, f) == 1 && c >= '0' && c <= '9')
        {
            m_DebugMode = c - '0';
        }
        std::fclose(f);
        PS4_Log("gnm: debug view %d", m_DebugMode);
    }
#endif
    PS4_Log("gnm: ready (%ux%u, vtx reg %d, vs set reg %d, ps set reg %d)", m_Display.screenw, m_Display.screenh,
            m_VtxBufferReg, m_VsSetReg, m_PsSetReg);
    return true;
}

Gnm::~Gnm()
{
    Exit();
}

void Gnm::Exit()
{
    if (m_Exited)
    {
        return;
    }
    m_Exited = true;
    displayctx_destroy(&m_Display);
    memalloc_destroy(&m_Garlic);
}

RendererType Gnm::GetType()
{
    // The timing code treats the software renderer as already synchronized. GNM flips on
    // vblank as well, and returning this avoids probing an SDL GL context which does not
    // exist in a full PS4 application.
    return RENDERER_SOFTWARE;
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

    const bool widescreen = g_Supervisor.cfg.windowed != 0;
    if (!m_AspectInitialized)
    {
        m_AspectInitialized = true;
        m_LastWidescreen = widescreen;
    }
    else if (m_LastWidescreen != widescreen)
    {
        m_LastWidescreen = widescreen;
        m_FullClearFrames = kNumFrames;
        PS4_Log("gnm: aspect ratio %s", widescreen ? "16:9" : "4:3");
    }
    if (m_FullClearFrames > 0 && m_FrameStart)
    {
        // When returning to 4:3, erase the old 16:9 image from both swap buffers so it
        // cannot remain visible in the new pillarbox bars.
        const f32 black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        gnmDrawCmdSetScreenScissor(&m_Cmd, 0, 0, m_Display.screenw, m_Display.screenh);
        clearcolortarget(&m_Cmd, &m_ColorTargets[m_CurFrame], black);
        m_FullClearFrames--;
    }
    m_FrameStart = false;
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
    // On PS4 the original Window/Fullscreen option is repurposed as 16:9/4:3. The former
    // stretches PCB's 640x480 output across the screen; the latter scales uniformly into a
    // centred 1440x1080 playfield.
    const bool widescreen = g_Supervisor.cfg.windowed != 0;
    const f32 scaleY = (f32)m_Display.screenh / 480.0f;
    const f32 scaleX = widescreen ? (f32)m_Display.screenw / 640.0f : scaleY;
    const i32 playWidth = (i32)(640.0f * scaleX);
    const i32 playLeft = ((i32)m_Display.screenw - playWidth) / 2;
    const i32 left = playLeft + (i32)((f32)m_Viewport.x * scaleX);
    const i32 top = (i32)((f32)m_Viewport.y * scaleY);
    const i32 right = left + (i32)((f32)m_Viewport.width * scaleX);
    const i32 bottom = top + (i32)((f32)m_Viewport.height * scaleY);
    const f32 zscale = (m_Viewport.maxZ - m_Viewport.minZ) * 0.5f;
    const f32 zoffset = (m_Viewport.maxZ + m_Viewport.minZ) * 0.5f;
    setupviewport(&m_Cmd, left, top, right, bottom, zscale, zoffset);
    // OpenGL and Direct3D clip a primitive to the clip volume, which lands exactly on the
    // viewport rectangle. This GPU clips to a much larger guard band instead and expects a
    // scissor, so without one the game's deliberately oversized quads (a full-width quad at
    // x -224..488 in a 640-wide space, for instance) spill over the HUD and the side bars.
    gnmDrawCmdSetScreenScissor(&m_Cmd, left, top, right, bottom);
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

void Gnm::SetColorOp(TextureOpComponent component, ColorOp op)
{
    if (component == COMPONENT_RGB)
    {
        m_ColorOpRgb = op;
    }
    else
    {
        m_ColorOpAlpha = op;
    }
}

void Gnm::SetTextureFactor(ZunColor factor)
{
    m_TextureFactor = factor;
}

void Gnm::SetTextureArg(TextureArg arg)
{
    m_TextureArg = arg;
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

void Gnm::GetViewport(ZunViewport &viewport)
{
    viewport = m_Viewport;
}

void Gnm::SetViewport(const ZunViewport &viewport)
{
    m_Viewport = viewport;
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
    case CAPS_ALPHA_TEST:
        m_AlphaTestEnabled = true;
        break;
    case CAPS_FOG:
        m_FogEnabled = true;
        break;
    }
}

void Gnm::Disable(Capabilities cap)
{
    switch (cap)
    {
    case CAPS_BLEND:
        m_BlendEnabled = false;
        break;
    case CAPS_DEPTH_TEST:
        m_DepthTestEnabled = false;
        break;
    case CAPS_ALPHA_TEST:
        m_AlphaTestEnabled = false;
        break;
    case CAPS_FOG:
        m_FogEnabled = false;
        break;
    }
}

void Gnm::SetBlendMode(BlendMode srcMode, BlendMode dstMode)
{
    // AnmManager batches 2D sprites. GLES flushes that batch before changing blend state;
    // without the same barrier here, normal sprites queued before an additive effect are
    // eventually drawn additively as well (the player turns pink/white, and bomb layers use
    // the wrong compositing mode).
    if (g_AnmManager && g_AnmManager->spritesToDraw != 0)
    {
        g_AnmManager->Flush();
    }
    m_BlendSrc = srcMode;
    m_BlendDst = dstMode;
}

void Gnm::SetDepthMask(bool enable)
{
    if (g_AnmManager && g_AnmManager->spritesToDraw != 0)
    {
        g_AnmManager->Flush();
    }
    m_DepthMask = enable;
}

void Gnm::SetDepthFunc(DepthFunc func)
{
    if (g_AnmManager && g_AnmManager->spritesToDraw != 0)
    {
        g_AnmManager->Flush();
    }
    m_DepthFunc = func;
}

void Gnm::SetClearDepth(f32 depth)
{
    m_ClearDepth = depth;
}

void Gnm::SetClearColor(ZunColor color)
{
    m_ClearColor[0] = ByteToFloat(color.bytes.r);
    m_ClearColor[1] = ByteToFloat(color.bytes.g);
    m_ClearColor[2] = ByteToFloat(color.bytes.b);
    m_ClearColor[3] = ByteToFloat(color.bytes.a);
}

void Gnm::SetAlphaTestRef(u8 ref)
{
    m_AlphaRef = ref;
}

void Gnm::Clear(u32 clearBits)
{
    g_ClearsThisFrame++;
    if (DumpingThisFrame() && g_ClearsThisFrame < 4)
    {
        PS4_Log("clear: bits %u, colour %.2f %.2f %.2f %.2f, viewport %d %d %dx%d", clearBits, m_ClearColor[0],
                m_ClearColor[1], m_ClearColor[2], m_ClearColor[3], m_Viewport.x, m_Viewport.y, m_Viewport.width,
                m_Viewport.height);
    }
    if (clearBits & CLEAR_COLOR_BUFFER)
    {
        const f32 black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        clearcolortarget(&m_Cmd, &m_ColorTargets[m_CurFrame], black);
        const f32 scale = (f32)m_Display.screenh / 480.0f;
        const i32 playWidth = (i32)(640.0f * scale);
        const i32 playLeft = ((i32)m_Display.screenw - playWidth) / 2;
        const i32 left = playLeft + (i32)((f32)m_Viewport.x * scale);
        const i32 top = (i32)((f32)m_Viewport.y * scale);
        const i32 right = left + (i32)((f32)m_Viewport.width * scale);
        const i32 bottom = top + (i32)((f32)m_Viewport.height * scale);
        gnmDrawCmdSetScreenScissor(&m_Cmd, left, top, right, bottom);
        clearcolortarget(&m_Cmd, &m_ColorTargets[m_CurFrame], m_ClearColor);
        // ApplyViewport below puts the viewport's own scissor back.
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
    SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_RGBA32, data, width * 4, SDL_PIXELFORMAT_ABGR8888,
                      (u8 *)tex.texels + ((u32)yoffset * tex.width + (u32)xoffset) * 4, tex.width * 4);
}

void Gnm::ReadPixels(i32 x, i32 y, i32 width, i32 height, void *pixels)
{
    SubmitAndWait();
    const u32 *src = (const u32 *)gnmRtGetBaseAddr(&m_ColorTargets[m_CurFrame]);
    const u32 pitch = gnmRtGetPitch(&m_ColorTargets[m_CurFrame]);
    u32 *dst = (u32 *)pixels;
    const f32 scale = (f32)m_Display.screenh / 480.0f;
    const i32 playWidth = (i32)(640.0f * scale);
    const i32 playLeft = ((i32)m_Display.screenw - playWidth) / 2;
    for (i32 row = 0; row < height; row++)
    {
        const i32 srcY = (i32)(((f32)y + (f32)row + 0.5f) * scale);
        if (srcY < 0 || srcY >= (i32)m_Display.screenh)
        {
            continue;
        }
        for (i32 col = 0; col < width; col++)
        {
            const i32 srcX = playLeft + (i32)(((f32)x + (f32)col + 0.5f) * scale);
            if (srcX >= 0 && srcX < (i32)m_Display.screenw)
            {
                dst[(std::size_t)row * width + col] = src[(std::size_t)srcY * pitch + srcX];
            }
        }
    }
}

i32 Gnm::VertexCount(PrimitiveType type, i32 primitiveCount)
{
    return type == PRIM_TRIANGLES ? primitiveCount * 3 : primitiveCount + 2;
}

void Gnm::DrawPrimitive(PrimitiveType type, i32 startVertex, i32 primitiveCount)
{
    static const Vertex quad[] = {
        {-128.0f, -128.0f, 0.0f, 0.0f, 0.0f, 0xFF, 0xFF, 0xFF, 0xFF},
        { 128.0f, -128.0f, 0.0f, 1.0f, 0.0f, 0xFF, 0xFF, 0xFF, 0xFF},
        {-128.0f,  128.0f, 0.0f, 0.0f, 1.0f, 0xFF, 0xFF, 0xFF, 0xFF},
        { 128.0f,  128.0f, 0.0f, 1.0f, 1.0f, 0xFF, 0xFF, 0xFF, 0xFF},
    };
    const i32 count = VertexCount(type, primitiveCount);
    if (startVertex < 0 || count <= 0 || startVertex + count > 4)
    {
        // The unit quad only has four vertices; anything else would read past it.
        g_DroppedThisFrame++;
        return;
    }
    Vertex *vertices = (Vertex *)FrameAlloc((u32)count * sizeof(Vertex), GNM_ALIGNMENT_BUFFER_BYTES);
    if (!vertices)
    {
        return;
    }
    std::memcpy(vertices, quad + startVertex, (size_t)count * sizeof(Vertex));
    DrawVertices(type, vertices, count, false, true);
}

void Gnm::DrawPrimitiveUP(PrimitiveType type, i32 primitiveCount, const void *vertexData,
                          i32 vertexStride)
{
    const i32 count = VertexCount(type, primitiveCount);
    if (!vertexData || count <= 0)
    {
        return;
    }

    Vertex *verts = (Vertex *)FrameAlloc((u32)count * sizeof(Vertex), GNM_ALIGNMENT_BUFFER_BYTES);
    if (!verts)
    {
        return;
    }

    bool screenSpace = false;
    bool hasTexCoords = false;
    for (i32 i = 0; i < count; i++)
    {
        Vertex &out = verts[i];
        out.u = out.v = 0.0f;
        out.r = out.g = out.b = out.a = 0xFF;

        const u8 *raw = (const u8 *)vertexData + (std::size_t)i * vertexStride;
        const ZunColor *color = nullptr;
        if (vertexStride == (i32)sizeof(VertexTex1DiffuseXyzrhw))
        {
            g_StrideCounts[0]++;
            const auto &in = *(const VertexTex1DiffuseXyzrhw *)raw;
            out.x = in.pos.x; out.y = in.pos.y; out.z = in.pos.z;
            out.u = in.textureUV.x; out.v = in.textureUV.y;
            color = &in.color;
            screenSpace = true;
            hasTexCoords = true;
        }
        else if (vertexStride == (i32)sizeof(VertexTex1DiffuseXyz))
        {
            g_StrideCounts[1]++;
            const auto &in = *(const VertexTex1DiffuseXyz *)raw;
            out.x = in.position.x; out.y = in.position.y; out.z = in.position.z;
            out.u = in.textureUV.x; out.v = in.textureUV.y;
            color = &in.diffuse;
            hasTexCoords = true;
        }
        else if (vertexStride == (i32)sizeof(VertexDiffuseXyzrhw))
        {
            g_StrideCounts[2]++;
            const auto &in = *(const VertexDiffuseXyzrhw *)raw;
            out.x = in.pos.x; out.y = in.pos.y; out.z = in.pos.z;
            color = &in.diffuse;
            screenSpace = true;
        }
        else
        {
            g_StrideCounts[3]++;
            static bool reported;
            if (!reported)
            {
                reported = true;
                PS4_Log("gnm: unsupported PCB vertex stride %d", vertexStride);
            }
            g_DroppedThisFrame++;
            return;
        }

        if (color)
        {
            out.r = color->bytes.r;
            out.g = color->bytes.g;
            out.b = color->bytes.b;
            out.a = color->bytes.a;
        }
    }

    // The HUD frame is built by repeating pieces of one texture. At 640x480 each texel maps
    // to a pixel and bilinear sampling lands dead centre, but scaled to 1080p every edge
    // pixel also picks up the neighbouring texel from outside the piece, drawing a bright
    // seam around each one -- the grid over the HUD. Pull each quad's texture coordinates
    // half a texel inwards, which is invisible but keeps the sampler inside the piece.
    const Texture *bound = BoundTexture();
    if (hasTexCoords && type == PRIM_TRIANGLES && count % 6 == 0 && bound && bound->width > 0 &&
        bound->height > 0)
    {
        const f32 insetU = 0.5f / (f32)bound->width;
        const f32 insetV = 0.5f / (f32)bound->height;
        for (i32 q = 0; q + 5 < count; q += 6)
        {
            f32 minU = verts[q].u, maxU = verts[q].u, minV = verts[q].v, maxV = verts[q].v;
            for (i32 i = 1; i < 6; i++)
            {
                minU = verts[q + i].u < minU ? verts[q + i].u : minU;
                maxU = verts[q + i].u > maxU ? verts[q + i].u : maxU;
                minV = verts[q + i].v < minV ? verts[q + i].v : minV;
                maxV = verts[q + i].v > maxV ? verts[q + i].v : maxV;
            }
            if (maxU - minU <= insetU * 2.0f || maxV - minV <= insetV * 2.0f)
            {
                continue; // too small to inset without collapsing it
            }
            const f32 loU = minU + insetU, hiU = maxU - insetU;
            const f32 loV = minV + insetV, hiV = maxV - insetV;
            for (i32 i = 0; i < 6; i++)
            {
                verts[q + i].u = verts[q + i].u < loU ? loU : (verts[q + i].u > hiU ? hiU : verts[q + i].u);
                verts[q + i].v = verts[q + i].v < loV ? loV : (verts[q + i].v > hiV ? hiV : verts[q + i].v);
            }
        }
    }

    DrawVertices(type, verts, count, screenSpace, hasTexCoords);
}


void Gnm::DrawVertices(PrimitiveType type, const Vertex *vertices, i32 count, bool screenSpace,
                       bool hasTexCoords)
{
    Constants *constants = (Constants *)FrameAlloc(sizeof(Constants), GNM_ALIGNMENT_BUFFER_BYTES);
    VsDescSet *vsset = (VsDescSet *)FrameAlloc(sizeof(VsDescSet), GNM_ALIGNMENT_BUFFER_BYTES);
    PsDescSet *psset = (PsDescSet *)FrameAlloc(sizeof(PsDescSet), GNM_ALIGNMENT_BUFFER_BYTES);
    GnmBuffer *vtxbuffers = (GnmBuffer *)FrameAlloc(sizeof(GnmBuffer) * 3, GNM_ALIGNMENT_BUFFER_BYTES);
    if (!constants || !vsset || !psset || !vtxbuffers)
    {
        return;
    }

    // ZunMatrix follows Direct3D's row-vector convention. Uploading its raw
    // row-major storage as a GLSL mat4 transposes it, so Model * View here
    // becomes View^T * Model^T in the shader: projection * view * model.
    const ZunMatrix modelview = m_Model * m_View;
    std::memcpy(constants->modelview, modelview.m, sizeof(constants->modelview));
    std::memcpy(constants->projection, m_Projection.m, sizeof(constants->projection));
    std::memcpy(constants->texmatrix, m_TextureMatrix.m, sizeof(constants->texmatrix));
    constants->fogColor[0] = ByteToFloat(m_FogColor.bytes.r);
    constants->fogColor[1] = ByteToFloat(m_FogColor.bytes.g);
    constants->fogColor[2] = ByteToFloat(m_FogColor.bytes.b);
    constants->fogColor[3] = ByteToFloat(m_FogColor.bytes.a);
    constants->textureFactor[0] = ByteToFloat(m_TextureFactor.bytes.r);
    constants->textureFactor[1] = ByteToFloat(m_TextureFactor.bytes.g);
    constants->textureFactor[2] = ByteToFloat(m_TextureFactor.bytes.b);
    constants->textureFactor[3] = ByteToFloat(m_TextureFactor.bytes.a);
    constants->fogRange[0] = m_FogNear;
    constants->fogRange[1] = m_FogFar;
    constants->fogRange[2] = constants->fogRange[3] = 0.0f;

    Texture *bound = BoundTexture();
    const bool useTexture = hasTexCoords && bound && bound->valid;
    if (useTexture)
    {
        bound->lastUsedBy = m_Submission;
    }
    constants->flags0[0] = (f32)m_ColorOpRgb;
    constants->flags0[1] = (f32)m_ColorOpAlpha;
    constants->flags0[2] = (f32)m_TextureArg;
    constants->flags0[3] = useTexture ? 1.0f : 0.0f;
    constants->flags1[0] = screenSpace ? 1.0f : 0.0f;
    constants->flags1[1] = m_AlphaTestEnabled ? 1.0f : 0.0f;
    constants->flags1[2] = ByteToFloat(m_AlphaRef);
    constants->flags1[3] = m_FogEnabled ? 1.0f : 0.0f;
    constants->debug[0] = (f32)m_DebugMode;
    constants->debug[1] = constants->debug[2] = constants->debug[3] = 0.0f;
    constants->viewport[0] = (f32)m_Viewport.x;
    constants->viewport[1] = (f32)m_Viewport.y;
    constants->viewport[2] = (f32)m_Viewport.width;
    constants->viewport[3] = (f32)m_Viewport.height;

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
    u8 *vertexBytes = const_cast<u8 *>(reinterpret_cast<const u8 *>(vertices));
    vtxbuffers[0] = gnmCreateVertexBuffer(vertexBytes, GNM_FMT_R32G32B32_FLOAT, stride, (u32)count);
    vtxbuffers[1] = gnmCreateVertexBuffer(vertexBytes + 12, GNM_FMT_R32G32_FLOAT, stride, (u32)count);
    vtxbuffers[2] = gnmCreateVertexBuffer(vertexBytes + 20, GNM_FMT_R8G8B8A8_UNORM, stride, (u32)count);

    const GnmPrimitiveSetup primsetup = {
        .cullmode = GNM_CULL_NONE,
        .frontface = GNM_FACE_CCW,
        .frontmode = GNM_FILL_SOLID,
        .backmode = GNM_FILL_SOLID,
        .provokemode = GNM_PROVOKINGVTX_FIRST,
    };
    gnmDrawCmdSetPrimitiveSetup(&m_Cmd, &primsetup);

    const GnmBlendOp srcmult = m_BlendSrc == BLEND_ALPHA ? GNM_BLEND_SRC_ALPHA : GNM_BLEND_ONE;
    const GnmBlendOp dstmult = m_BlendDst == BLEND_ALPHA ? GNM_BLEND_ONE_MINUS_SRC_ALPHA
                                                        : (m_BlendDst == BLEND_ONE ? GNM_BLEND_ONE : GNM_BLEND_ZERO);
    const GnmBlendControl blend = {
        // A debug view has to overwrite the pixel, or blending decides what we see and the
        // view says nothing about the value being inspected.
        .blendenabled = m_DebugMode == 0 && m_BlendEnabled,
        .colorfunc = GNM_COMB_DST_PLUS_SRC,
        .colorsrcmult = srcmult,
        .colordstmult = dstmult,
        .alphafunc = GNM_COMB_DST_PLUS_SRC,
        .alphasrcmult = srcmult,
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

    if (DumpingThisFrame() && g_DrawsThisFrame < 24)
    {
        const char *primName = type == PRIM_TRIANGLE_STRIP ? "strip" : type == PRIM_TRIANGLE_FAN ? "fan" : "list";
        PS4_Log("draw %2u: %s x%d %s tex=%s(%ux%u) ops=%d/%d arg=%d blend=%d/%d(%d) depth=%d/%d fog=%d",
                g_DrawsThisFrame, primName, count, screenSpace ? "screen" : "world",
                useTexture ? "yes" : "no", bound ? bound->width : 0, bound ? bound->height : 0,
                (int)m_ColorOpRgb, (int)m_ColorOpAlpha, (int)m_TextureArg, (int)m_BlendSrc, (int)m_BlendDst,
                m_BlendEnabled ? 1 : 0, m_DepthTestEnabled ? 1 : 0, m_DepthMask ? 1 : 0,
                m_FogEnabled ? 1 : 0);
        for (i32 i = 0; i < count && i < 4; i++)
        {
            const Vertex &v = vertices[i];
            PS4_Log("    v%d pos %.1f %.1f %.4f uv %.4f %.4f rgba %02x%02x%02x%02x", i, v.x, v.y, v.z, v.u, v.v,
                    v.r, v.g, v.b, v.a);
        }
        // The game batches sprites as six vertices each (two triangles). Measuring every
        // quad in the batch says whether the geometry itself is sane, which four sampled
        // vertices cannot.
        if (type == PRIM_TRIANGLES && count >= 6)
        {
            f32 minW = 1e9f, maxW = -1e9f, minH = 1e9f, maxH = -1e9f, sumW = 0.0f, sumH = 0.0f;
            i32 quads = 0, degenerate = 0;
            for (i32 q = 0; q + 5 < count; q += 6)
            {
                // Quad corners are stored in strip order: 0 = top left, 1 = top right,
                // 2 = bottom left, 3 = bottom right (vertices 3..5 repeat 1, 2 and 3).
                const f32 w = std::fabs(vertices[q + 1].x - vertices[q].x);
                const f32 h = std::fabs(vertices[q + 2].y - vertices[q].y);
                minW = w < minW ? w : minW;
                maxW = w > maxW ? w : maxW;
                minH = h < minH ? h : minH;
                maxH = h > maxH ? h : maxH;
                sumW += w;
                sumH += h;
                quads++;
                if (w < 0.5f || h < 0.5f)
                {
                    degenerate++;
                }
            }
            PS4_Log("    %d quads: width %.1f..%.1f (avg %.1f), height %.1f..%.1f (avg %.1f), %d degenerate",
                    quads, minW, maxW, sumW / (f32)quads, minH, maxH, sumH / (f32)quads, degenerate);
            for (i32 q = 0; q < 2 && q * 6 + 3 < count; q++)
            {
                const Vertex *v = vertices + q * 6;
                PS4_Log("    quad %d: (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)", q, v[0].x, v[0].y, v[1].x,
                        v[1].y, v[2].x, v[2].y, v[5].x, v[5].y);
            }
        }
        if (!screenSpace)
        {
            // The sprite's size, position and UV rect all live in these, so they say more
            // than the vertices do: the world quad is always the same +-128 square.
            PS4_Log("    model  %.3f %.3f %.3f | %.3f %.3f %.3f | pos %.2f %.2f %.2f", m_Model.m[0][0],
                    m_Model.m[0][1], m_Model.m[0][2], m_Model.m[1][0], m_Model.m[1][1], m_Model.m[1][2],
                    m_Model.m[3][0], m_Model.m[3][1], m_Model.m[3][2]);
            PS4_Log("    uv     scale %.4f %.4f | origin %.4f %.4f | row3 %.4f %.4f", m_TextureMatrix.m[0][0],
                    m_TextureMatrix.m[1][1], m_TextureMatrix.m[2][0], m_TextureMatrix.m[2][1],
                    m_TextureMatrix.m[3][0], m_TextureMatrix.m[3][1]);
            PS4_Log("    tfactor %02x%02x%02x%02x", m_TextureFactor.bytes.r, m_TextureFactor.bytes.g,
                    m_TextureFactor.bytes.b, m_TextureFactor.bytes.a);
        }
    }
    g_DrawsThisFrame++;

    const GnmPrimitiveType prim = type == PRIM_TRIANGLE_STRIP ? GNM_PT_TRISTRIP
                                  : type == PRIM_TRIANGLE_FAN ? GNM_PT_TRIFAN
                                                              : GNM_PT_TRILIST;
    gnmDrawCmdSetPrimitiveType(&m_Cmd, prim);
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

    if (DumpingThisFrame())
    {
        PS4_Log("frame %llu: %u draws, %u clears, %u dropped, strides %u/%u/%u/%u",
                (unsigned long long)g_FrameIndex, g_DrawsThisFrame, g_ClearsThisFrame, g_DroppedThisFrame,
                g_StrideCounts[0], g_StrideCounts[1], g_StrideCounts[2], g_StrideCounts[3]);
        PS4_Log("frame %llu: viewport %d %d %dx%d, proj %.4f %.4f %.4f %.4f", (unsigned long long)g_FrameIndex,
                m_Viewport.x, m_Viewport.y, m_Viewport.width, m_Viewport.height, m_Projection.m[0][0],
                m_Projection.m[1][1], m_Projection.m[2][2], m_Projection.m[3][2]);
    }
    g_FrameIndex++;
    g_DrawsThisFrame = 0;
    g_ClearsThisFrame = 0;
    g_DroppedThisFrame = 0;
    g_StrideCounts[0] = g_StrideCounts[1] = g_StrideCounts[2] = g_StrideCounts[3] = 0;

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

    // Debug view 8 keeps everything on one buffer. PCB never clears the colour buffer and
    // draws its trails on top of what is already on screen; with two buffers each frame
    // lands on the frame before last, which would smear those trails.
    if (m_DebugMode != 8)
    {
        m_CurFrame = (m_CurFrame + 1) % kNumFrames;
    m_FrameStart = true;
    }
    m_Frames[m_CurFrame].used = 0;
    BeginCommandBuffer();
}
} // namespace

namespace GnmBackend
{
ZunGraphics *Init()
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
