#include "Software.hpp"
#include "GameWindow.hpp"
#include "Supervisor.hpp"
#include "i18n.hpp"
#include "utils.hpp"
#include <SDL2/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include "graphics/SoftwareRaster.hpp"
#ifdef TH_PS4_BIG_APP
#include "ps4_videoout.hpp"
#endif



#ifdef TH_PS4_BIG_APP
void PS4_Log(const char *fmt, ...);
#endif

GfxInterface *Software::Init()
{
#ifdef TH_PS4_BIG_APP
    // No SDL window: frames go straight to SceVideoOut in SwapBuffers().
    Software *self = new Software();
    if (!PS4_VideoOutInit())
    {
        delete self;
        return NULL;
    }
#else
    SDL_Init(SDL_INIT_VIDEO);

    u32 flags = 0;
    i32 height = GAME_WINDOW_HEIGHT_REAL;
    i32 width = GAME_WINDOW_WIDTH_REAL;
    i32 x = SDL_WINDOWPOS_UNDEFINED;
    i32 y = SDL_WINDOWPOS_UNDEFINED;

    if (g_Supervisor.cfg.windowed == 0)
    {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    Software *self = new Software();

    SDL_Window *window = SDL_CreateWindow(TH_WINDOW_TITLE, x, y, width, height, flags);
    self->window = window;
    if (window == NULL)
    {
        delete self;
        return NULL;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    self->renderer = renderer;
    if (renderer == NULL)
    {
        delete self;
        return NULL;
    }

    SDL_Texture *framebufferTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                                        GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT);
    self->framebufferTexture = framebufferTexture;
    if (framebufferTexture == NULL)
    {
        delete self;
        return NULL;
    }
#endif
    // Shared by both paths (the big-app one used to skip this and drew nothing: sprites never
    // set the model matrix, so every vertex collapsed to the origin).
    self->model.Identity();
    self->view.Identity();
    self->projection.Identity();
    self->textureMatrix.Identity();

    u32 *framebuffer = new u32[GAME_WINDOW_WIDTH * GAME_WINDOW_HEIGHT];
    self->framebuffer = framebuffer;

    f32 *depthBuffer = new f32[GAME_WINDOW_WIDTH * GAME_WINDOW_HEIGHT];
    self->depthBuffer = depthBuffer;
    self->noVertexBuffer = g_Supervisor.cfg.opts & (1 << GCOS_DONT_USE_VERTEX_BUF);
    SoftwareRaster::Start();
    self->noFog = g_Supervisor.cfg.opts & (1 << GCOS_DONT_USE_FOG);

    utils::DebugPrint2(
        "WARNING: Using software rasterizer, which can be slow. If performance is bad, make sure you're compiling with "
        "optimizations (building as release), or go with another graphics backend if possible.");

    return self;
}

void Software::Exit()
{
    if (this->framebuffer)
    {
        Flush();
    }
    SoftwareRaster::Stop();
    if (this->renderer)
    {
        SDL_DestroyRenderer(this->renderer);
        this->renderer = NULL;
    }
    if (this->window)
    {
        SDL_DestroyWindow(this->window);
        this->window = NULL;
    }
    if (this->framebufferTexture)
    {
        SDL_DestroyTexture(this->framebufferTexture);
        this->framebufferTexture = NULL;
    }
    if (this->framebuffer)
    {
        delete[] this->framebuffer;
        this->framebuffer = NULL;
    }
    if (this->depthBuffer)
    {
        delete[] this->depthBuffer;
        this->depthBuffer = NULL;
    }
}

void Software::SetFogRange(f32 nearPlane, f32 farPlane)
{
    fogNear = nearPlane;
    fogFar = farPlane;
}

void Software::SetFogColor(ZunColor color)
{
    fogColor = color;
}

void Software::ToggleVertexAttribute(u8 attr, bool enable)
{
    if (attr & VERTEX_ATTR_TEX_COORD)
    {
        useTexCoord = enable;
    }
    if (attr & VERTEX_ATTR_DIFFUSE)
    {
        useDiffuse = enable;
    }
}

void Software::SetAttributePointer(VertexAttributeArrays attr, std::size_t stride, void *ptr)
{
    switch (attr)
    {
    case VERTEX_ARRAY_POSITION:
        this->vertexData = ptr;
        this->vertexStride = stride;
        break;
    case VERTEX_ARRAY_TEX_COORD:
        this->texCoordData = ptr;
        this->texCoordStride = stride;
        break;
    case VERTEX_ARRAY_DIFFUSE:
        this->diffuseData = ptr;
        this->diffuseStride = stride;
        break;
    }
}

void Software::SetColorOp(TextureOpComponent component, ColorOp op)
{

    if (component == COMPONENT_ALPHA)
    {
        return;
    }
    colorOp = op;
}

void Software::SetTextureFactor(ZunColor factor)
{
    textureFactor = factor;
}

void Software::SetTransformMatrix(TransformMatrix type, const ZunMatrix &matrix)
{
    switch (type)
    {
    case MATRIX_MODEL:
        model = matrix;
        break;
    case MATRIX_VIEW:
        view = matrix;
        break;
    case MATRIX_PROJECTION:
        projection = matrix;
        break;
    case MATRIX_TEXTURE:
        textureMatrix = matrix;
        break;
    }
}

void Software::Enable(Capabilities cap)
{
    if (cap == CAPS_DEPTH_TEST)
    {
        useDepthTest = true;
    }
}

void Software::SetBlendMode(BlendMode mode)
{
    blendMode = mode;
}

void Software::SetViewport(i32 x, i32 y, i32 width, i32 height)
{
    viewport[0] = x;
    viewport[1] = y;
    viewport[2] = width;
    viewport[3] = height;
}

void Software::GetViewport(u32 *viewport)
{
    for (int i = 0; i < 4; i++)
    {
        viewport[i] = this->viewport[i];
    }
}

void Software::GetDepthRange(f32 *depthRange)
{
    depthRange[0] = this->depthNear;
    depthRange[1] = this->depthFar;
}

inline ZunColor RGBAToZunColor(u8 r, u8 g, u8 b, u8 a)
{
    return ((ZunColor)a << 24) | ((ZunColor)r << 16) | ((ZunColor)g << 8) | (ZunColor)b;
}

inline ZunColor ColorDataToZunColor(ColorData colorData)
{
    return RGBAToZunColor(colorData.r, colorData.g, colorData.b, colorData.a);
}

void Software::SetClearColor(f32 r, f32 g, f32 b, f32 a)
{
    clearColor = RGBAToZunColor((u8)(r * 255), (u8)(g * 255), (u8)(b * 255), (u8)(a * 255));
}

void Software::SetTextureFilter()
{
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
}

void Software::SetClearDepth(f32 depth)
{
    clearDepth = depth;
}

void Software::SetDepthRange(f32 nearPlane, f32 farPlane)
{
    depthNear = nearPlane;
    depthFar = farPlane;
}

void Software::SetDepthMask(bool enable)
{
    depthMask = enable;
}

void Software::SetDepthFunc(DepthFunc func)
{
    depthFunc = func;
}

GfxTextureHandle Software::CreateTexture()
{
    std::unique_ptr<Texture> texture = std::unique_ptr<Texture>(new Texture());

    u32 id;
    if (!freeTextures.empty())
    {
        id = freeTextures.back();
        freeTextures.pop_back();
        textures[id] = std::move(texture);
    }
    else
    {
        id = textures.size();
        textures.push_back(std::move(texture));
    }

    return {id};
}

void Software::BindTexture(GfxTextureHandle handle)
{
    if (handle >= textures.size())
        return;
    if (!textures[handle.id])
        return;
    boundTexture = textures[handle.id].get();
}

void Software::DeleteTexture(GfxTextureHandle handle)
{
    Flush();
    if (handle.id >= textures.size())
        return;
    if (!textures[handle.id])
        return;
    textures[handle.id].reset();
    freeTextures.push_back(handle.id);
}

inline SDL_PixelFormatEnum GetSDLPixelFormat(PixelFormat fmt, PixelDataType type)
{
    switch (type)
    {
    case PIXEL_UNSIGNED_BYTE:
        if (fmt == PIXEL_RGB)
            return SDL_PIXELFORMAT_RGB24;
        else
            return SDL_PIXELFORMAT_RGBA32;
    case PIXEL_UNSIGNED_SHORT_4_4_4_4:
        return SDL_PIXELFORMAT_RGBA4444;
    case PIXEL_UNSIGNED_SHORT_5_5_5_1:
        return SDL_PIXELFORMAT_RGBA5551;
    case PIXEL_UNSIGNED_SHORT_5_6_5:
        return SDL_PIXELFORMAT_RGB565;
    }
}

void Software::SetTextureImage(u32 width, u32 height, PixelFormat fmt, PixelDataType type, const void *data)
{
    Flush();
    if (boundTexture)
    {
        u32 bpp = 2;
        if (type == PIXEL_UNSIGNED_BYTE)
        {
            if (fmt == PIXEL_RGB)
                bpp = 3;
            else
                bpp = 4;
        }
        boundTexture->texels.resize(width * height);
        if (data)
            SDL_ConvertPixels(width, height, GetSDLPixelFormat(fmt, type), data, width * bpp, SDL_PIXELFORMAT_ARGB8888,
                              boundTexture->texels.data(), width * sizeof(u32));
        boundTexture->width = width;
        boundTexture->height = height;
        boundTexture->format = fmt;
        boundTexture->type = type;
    }
}

void Software::SetTextureSubImage(i32 xoffset, i32 yoffset, i32 width, i32 height, const void *data)
{
    Flush();
    if (boundTexture)
    {
        SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_RGB24, data, width * 3, SDL_PIXELFORMAT_ARGB8888,
                          boundTexture->texels.data() + (yoffset * boundTexture->width) + xoffset,
                          boundTexture->width * sizeof(u32));
    }
}

void Software::ReadPixels(i32 x, i32 y, i32 width, i32 height, const void *pixels)
{
    Flush();
    u8 *dst = (u8 *)pixels;
    i32 pitch = width * 4;
    for (i32 row = 0; row < height; row++)
    {
        // The framebuffer holds ARGB8888 words; callers want RGBA bytes (SDL_PIXELFORMAT_RGBA32).
        const u32 *src = framebuffer + (GAME_WINDOW_HEIGHT - 1 - (y + row)) * GAME_WINDOW_WIDTH + x;
        u8 *out = dst + row * pitch;
        for (i32 col = 0; col < width; col++)
        {
            const u32 p = src[col];
            out[col * 4 + 0] = (p >> 16) & 0xFF;
            out[col * 4 + 1] = (p >> 8) & 0xFF;
            out[col * 4 + 2] = p & 0xFF;
            out[col * 4 + 3] = p >> 24;
        }
    }
}

inline ZunVec3 Software::ProjectToNDC(ZunVec3 vertex, ZunMatrix mv, ZunMatrix p, f32 &viewZ, f32 &W)
{
    ZunVec4 clip = mv * ZunVec4(vertex, 1.0f);
    viewZ = clip.z;
    clip = p * clip;
    ZunVec3 ndc = {clip.x, clip.y, clip.z};
    if (clip.w != 0)
    {
        ndc /= clip.w;
        W = 1.0f / clip.w;
    }

    return ndc;
}

inline ZunVec2 Software::ProjectTexCoordToNDC(ZunVec2 texCoord, ZunMatrix textureMatrix)
{
    ZunVec4 clip = textureMatrix * ZunVec4(ZunVec3(texCoord.x, texCoord.y, 1.0f), 1.0f);
    ZunVec2 ndc = {clip.x, clip.y};
    return ndc;
}

inline ZunVec3 Software::NDCToScreen(ZunVec3 vertex)
{
    ZunVec3 screen;
    screen.x = (vertex.x + 1) / 2.0f * viewport[2] + viewport[0];
    screen.y = (1 - (vertex.y + 1) / 2.0f) * viewport[3] + viewport[1];
    screen.z = vertex.z;
    return screen;
}

inline f32 EdgeFunction(ZunVec3 v0, ZunVec3 v1, ZunVec3 v2)
{
    return (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
}

inline u8 AlphaBlendU8(u8 src, u8 dst, u8 a, u8 ia)
{
    return (u8)ZUN_MIN((((u32)src * a + (u32)dst * ia + 128) >> 8), 255);
}

inline u8 LerpU8(u32 a, u32 b, u32 t)
{
    return (u8)((a * (255 - t) + b * t) >> 8);
}

inline u32 InterpZunColor(ZunColor src, ZunColor dst, u32 t)
{
    return RGBAToZunColor(LerpU8(ZunR(src), ZunR(dst), t), LerpU8(ZunG(src), ZunG(dst), t),
                          LerpU8(ZunB(src), ZunB(dst), t), ZunA(src));
}

inline ZunColor ZunColorMul(u32 a, u32 b)
{
    u32 R = (ZunR(a) * ZunR(b)) >> 8;
    u32 G = (ZunG(a) * ZunG(b)) >> 8;
    u32 B = (ZunB(a) * ZunB(b)) >> 8;
    u32 A = (ZunA(a) * ZunA(b)) >> 8;

    return RGBAToZunColor(R, G, B, A);
}

#ifdef TH_PS4_BIG_APP
static u64 s_SyncFlushes;
#endif

void Software::Flush()
{
#ifdef TH_PS4_BIG_APP
    s_SyncFlushes += !SoftwareRaster::Queue().empty();
#endif
    SoftwareRaster::FlushSync(framebuffer, depthBuffer);
}

void Software::Clear(u32 clearBits)
{
    RasterCmd c;
    c.isClear = true;
    c.clearBits = clearBits;
    c.clearColor = clearColor;
    c.clearDepth = clearDepth;
    SoftwareRaster::Queue().push_back(c);
}

void Software::Draw(PrimitiveType type, i32 start, i32 count)
{
    if (count == 0)
        return;
    u32 increment = type == PRIM_TRIANGLE_STRIP ? 1 : 3;
    u32 index = start;
    u32 last_index = start + count;
    if (type == PRIM_TRIANGLE_STRIP)
        last_index -= 2;
    ZunMatrix modelview = view * model;

    const bool useTexture = boundTexture && useTexCoord && !boundTexture->texels.empty();
    while (index < last_index)
    {
        f32 invw0 = 1, invw1 = 1, invw2 = 1;
        f32 viewZ0, viewZ1, viewZ2;
        f32 ndcZ0, ndcZ1, ndcZ2;
        ZunVec3 v0 =
            ProjectToNDC(*(ZunVec3 *)((u8 *)vertexData + vertexStride * index), modelview, projection, viewZ0, invw0);
        ZunVec3 v1 = ProjectToNDC(*(ZunVec3 *)((u8 *)vertexData + vertexStride * (index + 1)), modelview, projection,
                                  viewZ1, invw1);
        ZunVec3 v2 = ProjectToNDC(*(ZunVec3 *)((u8 *)vertexData + vertexStride * (index + 2)), modelview, projection,
                                  viewZ2, invw2);

        ZunVec2 tc0 = {0, 0}, tc1 = {0, 0}, tc2 = {0, 0};
        Diffuse diffuse0(0, 0, 0, 0), diffuse1(0, 0, 0, 0), diffuse2(0, 0, 0, 0);
        if (useTexCoord)
        {
            const ZunVec2 texDim = {static_cast<f32>(boundTexture ? boundTexture->width : 0),
                                    static_cast<f32>(boundTexture ? boundTexture->height : 0)};
            tc0 =
                ProjectTexCoordToNDC(*(ZunVec2 *)((u8 *)texCoordData + texCoordStride * index), textureMatrix) * texDim;
            tc1 = ProjectTexCoordToNDC(*(ZunVec2 *)((u8 *)texCoordData + texCoordStride * (index + 1)), textureMatrix) *
                  texDim;
            tc2 = ProjectTexCoordToNDC(*(ZunVec2 *)((u8 *)texCoordData + texCoordStride * (index + 2)), textureMatrix) *
                  texDim;
        }

        if (useDiffuse)
        {
            diffuse0 = Diffuse(*(ColorData *)((u8 *)diffuseData + diffuseStride * index));
            diffuse1 = Diffuse(*(ColorData *)((u8 *)diffuseData + diffuseStride * (index + 1)));
            diffuse2 = Diffuse(*(ColorData *)((u8 *)diffuseData + diffuseStride * (index + 2)));
        }

        if (type == PRIM_TRIANGLE_STRIP && ((index - start) & 1))
        {
            std::swap(v0, v1);
            std::swap(tc0, tc1);
            std::swap(invw0, invw1);
            std::swap(viewZ0, viewZ1);
            std::swap(diffuse0, diffuse1);
        }

        if (EdgeFunction(v0, v1, v2) < 0)
        {
            std::swap(v1, v2);
            std::swap(tc1, tc2);
            std::swap(invw1, invw2);
            std::swap(viewZ1, viewZ2);
            std::swap(diffuse1, diffuse2);
        }
        index += increment;

        ndcZ0 = v0.z;
        ndcZ1 = v1.z;
        ndcZ2 = v2.z;
        v0 = NDCToScreen(v0);
        v1 = NDCToScreen(v1);
        v2 = NDCToScreen(v2);

        const f32 minX = std::min({v0.x, v1.x, v2.x}), maxX = std::max({v0.x, v1.x, v2.x});
        const f32 minY = std::min({v0.y, v1.y, v2.y}), maxY = std::max({v0.y, v1.y, v2.y});
        if (!std::isfinite(minX) || !std::isfinite(maxX) || !std::isfinite(minY) || !std::isfinite(maxY))
        {
            continue;
        }
        RasterCmd c;
        c.isClear = false;
        c.xmin = std::max(viewport[0], (i32)std::floor(minX));
        c.xmax = std::min(viewport[0] + viewport[2] - 1, (i32)std::ceil(maxX));
        c.ymin = std::max(viewport[1], (i32)std::floor(minY));
        c.ymax = std::min(viewport[1] + viewport[3] - 1, (i32)std::ceil(maxY));
        c.xmin = std::max(c.xmin, 0);
        c.ymin = std::max(c.ymin, 0);
        c.xmax = std::min(c.xmax, (i32)GAME_WINDOW_WIDTH - 1);
        c.ymax = std::min(c.ymax, (i32)GAME_WINDOW_HEIGHT - 1);
        const f32 area = EdgeFunction(v0, v1, v2);
        if (c.xmin > c.xmax || c.ymin > c.ymax || area == 0.0f)
        {
            continue;
        }

        const ZunVec3 vP = ZunVec3(c.xmin + 0.5f, c.ymin + 0.5f, 0);
        const ZunVec3 edges = {EdgeFunction(v1, v2, vP), EdgeFunction(v2, v0, vP), EdgeFunction(v0, v1, vP)};
        const ZunVec3 e_dx = {v1.y - v2.y, v2.y - v0.y, v0.y - v1.y};
        const ZunVec3 e_dy = {v2.x - v1.x, v0.x - v2.x, v1.x - v0.x};
        const f32 invarea = 1.0f / area;
        c.w0 = edges * invarea;
        c.w_dx = e_dx * invarea;
        c.w_dy = e_dy * invarea;

        const ZunVec3 &w0 = c.w0, &w_dx = c.w_dx, &w_dy = c.w_dy;
        c.uv0 = (tc0 * invw0) * w0.x + (tc1 * invw1) * w0.y + (tc2 * invw2) * w0.z;
        c.uv_dx = (tc0 * invw0) * w_dx.x + (tc1 * invw1) * w_dx.y + (tc2 * invw2) * w_dx.z;
        c.uv_dy = (tc0 * invw0) * w_dy.x + (tc1 * invw1) * w_dy.y + (tc2 * invw2) * w_dy.z;
        const Diffuse dif0 = (diffuse0 * invw0) * w0.x + (diffuse1 * invw1) * w0.y + (diffuse2 * invw2) * w0.z;
        const Diffuse dif_dx = (diffuse0 * invw0) * w_dx.x + (diffuse1 * invw1) * w_dx.y + (diffuse2 * invw2) * w_dx.z;
        const Diffuse dif_dy = (diffuse0 * invw0) * w_dy.x + (diffuse1 * invw1) * w_dy.y + (diffuse2 * invw2) * w_dy.z;
        c.dif0 = {dif0.r, dif0.g, dif0.b, dif0.a};
        c.dif_dx = {dif_dx.r, dif_dx.g, dif_dx.b, dif_dx.a};
        c.dif_dy = {dif_dy.r, dif_dy.g, dif_dy.b, dif_dy.a};
        c.invw0 = invw0 * w0.x + invw1 * w0.y + invw2 * w0.z;
        c.invw_dx = invw0 * w_dx.x + invw1 * w_dx.y + invw2 * w_dx.z;
        c.invw_dy = invw0 * w_dy.x + invw1 * w_dy.y + invw2 * w_dy.z;
        c.ndcZ0 = w0.x * ndcZ0 * invw0 + w0.y * ndcZ1 * invw1 + w0.z * ndcZ2 * invw2;
        c.ndcZ_dx = w_dx.x * ndcZ0 * invw0 + w_dx.y * ndcZ1 * invw1 + w_dx.z * ndcZ2 * invw2;
        c.ndcZ_dy = w_dy.x * ndcZ0 * invw0 + w_dy.y * ndcZ1 * invw1 + w_dy.z * ndcZ2 * invw2;
        c.fogZ0 = viewZ0 * invw0 * w0.x + viewZ1 * invw1 * w0.y + viewZ2 * invw2 * w0.z;
        c.fogZ_dx = viewZ0 * invw0 * w_dx.x + viewZ1 * invw1 * w_dx.y + viewZ2 * invw2 * w_dx.z;
        c.fogZ_dy = viewZ0 * invw0 * w_dy.x + viewZ1 * invw1 * w_dy.y + viewZ2 * invw2 * w_dy.z;
        c.affine = invw0 == invw1 && invw1 == invw2 && invw0 != 0.0f;
        c.affineClipW = c.affine ? 1.0f / invw0 : 1.0f;

        c.useTexture = useTexture;
        c.texels = useTexture ? boundTexture->texels.data() : nullptr;
        c.texW = useTexture ? boundTexture->width : 1;
        c.texH = useTexture ? boundTexture->height : 1;
        c.useDepthTest = useDepthTest;
        c.depthMask = depthMask;
        c.depthLequal = depthFunc == DEPTH_FUNC_LEQUAL;
        c.noVertexBuffer = noVertexBuffer;
        // Fog only matters if some vertex is short of the fog start (the coefficient is
        // interpolated linearly in view depth, so the vertices bound it).
        c.fogActive = false;
        if (!noFog)
        {
            const f32 invFogDif = 1.0f / (fogFar - fogNear);
            for (f32 vz : {viewZ0, viewZ1, viewZ2})
            {
                c.fogActive |= !((fogFar - vz) * invFogDif >= 1.0f);
            }
        }
        c.bilinear = true;
        c.blendInvSrcAlpha = blendMode == BLEND_INV_SRC_ALPHA;
        c.colorOp = colorOp;
        c.textureFactor = textureFactor;
        c.fogColor = fogColor;
        c.fogFar = fogFar;
        c.invFogDif = 1.0f / (fogFar - fogNear);
        c.depthNear = depthNear;
        c.depthDif = depthFar - depthNear;
        SoftwareRaster::Queue().push_back(c);
    }
}

void Software::SwapBuffers()
{
#ifdef TH_PS4_BIG_APP
    // Frame N-1 was rasterized and scaled by the workers while the game built frame N:
    // show it, then hand frame N over and return to the game right away.
    static u64 s_LastReturn;
    static bool s_HavePending;
    static u64 s_GameUs, s_RasterUs, s_RasterOnlyUs, s_WaitUs, s_Frames, s_Commands;
    static u64 s_WindowStart = SoftwareRaster::NowUs();

    const u64 entry = SoftwareRaster::NowUs();
    const u64 rasterUs = SoftwareRaster::WaitAsync();
    const u64 rasterOnlyUs = SoftwareRaster::LastAsyncRasterUs();
    const u64 waited = SoftwareRaster::NowUs();
    const u64 commands = SoftwareRaster::Queue().size();
    if (s_HavePending)
    {
        PS4_VideoOutFlip();
    }
    PS4_VideoOutBeginFrame(GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT, g_PS4Widescreen);
    const u32 *frame = framebuffer;
    SoftwareRaster::SubmitAsync(framebuffer, depthBuffer,
                                [frame](i32 part, i32 parts) { PS4_VideoOutScaleRows(frame, part, parts); });
    s_HavePending = true;

    if (s_LastReturn != 0)
    {
        s_GameUs += (entry - s_LastReturn);
        s_RasterUs += rasterUs;
        s_RasterOnlyUs += rasterOnlyUs;
        s_Commands += commands;
        s_WaitUs += (waited - entry);
        s_Frames++;
    }
    s_LastReturn = SoftwareRaster::NowUs();
    const u64 windowUs = (s_LastReturn - s_WindowStart);
    if (windowUs >= 2000000 && s_Frames > 0)
    {
        u64 walked, shaded, slotMin, slotMax;
        SoftwareRaster::TakeStats(walked, shaded, slotMin, slotMax);
        PS4_Log("perf: %.0fk px walked, %.0fk shaded /frame | worker busy %.1f..%.1f ms /frame",
                walked / 1000.0 / s_Frames, shaded / 1000.0 / s_Frames, slotMin / 1000.0 / s_Frames,
                slotMax / 1000.0 / s_Frames);
        PS4_Log("perf: %.1f fps | game+setup %.1f ms | raster %.1f + scale %.1f ms | waiting %.1f ms | %llu tris, "
                "%.2f sync flushes /frame",
                s_Frames * 1e6 / windowUs, s_GameUs / 1000.0 / s_Frames, s_RasterOnlyUs / 1000.0 / s_Frames,
                (s_RasterUs - s_RasterOnlyUs) / 1000.0 / s_Frames, s_WaitUs / 1000.0 / s_Frames,
                (unsigned long long)(s_Commands / s_Frames), (double)s_SyncFlushes / s_Frames);
        s_GameUs = s_RasterUs = s_RasterOnlyUs = s_WaitUs = s_Frames = s_Commands = s_SyncFlushes = 0;
        s_WindowStart = s_LastReturn;
    }
    return;
#endif
    Flush();
    SDL_UpdateTexture(framebufferTexture, NULL, framebuffer, GAME_WINDOW_WIDTH * sizeof(u32));
    SDL_RenderCopy(renderer, framebufferTexture, NULL, NULL);
    SDL_RenderPresent(renderer);
}
