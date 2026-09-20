#include "Software.hpp"

#include <SDL2/SDL_error.h>
#include <SDL2/SDL_render.h>
#include <algorithm>
#include <cmath>

#include "GameWindow.hpp"
#include "Supervisor.hpp"

ZunGraphics *SoftwareGraphics::Init()
{
    SoftwareGraphics *gfx = new SoftwareGraphics;

    gfx->renderer = SDL_CreateRenderer(g_GameWindow.window, -1,
                                       SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
    if (!gfx->renderer)
    {
        delete gfx;
        Supervisor::DebugPrint("sdl renderer create failed: %s\n", SDL_GetError());
        return nullptr;
    }

    gfx->screenTexture = SDL_CreateTexture(gfx->renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, 640, 480);
    if (!gfx->screenTexture)
    {
        delete gfx;
        Supervisor::DebugPrint("sdl texture create failed: %s\n", SDL_GetError());
        return nullptr;
    }

    gfx->colorBuffer.resize(640 * 480, 0);
    gfx->depthBuffer.resize(640 * 480, 1.0f);

    for (i32 i = 0; i < 4; ++i)
    {
        gfx->transforms[i].Identity();
    }

    gfx->viewport.x = 0;
    gfx->viewport.y = 0;
    gfx->viewport.width = 640;
    gfx->viewport.height = 480;
    gfx->viewport.minZ = 0.0f;
    gfx->viewport.maxZ = 1.0f;

    // vertex buffers? whats that?
    g_Supervisor.cfg.noVertexBuffers = true;

    Supervisor::DebugPrint("using software rendering. this will be kinda hell\n");

    return gfx;
}

void SoftwareGraphics::Exit()
{
    if (screenTexture)
    {
        SDL_DestroyTexture(screenTexture);
        screenTexture = nullptr;
    }
    if (renderer)
    {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    textures.clear();
}

void SoftwareGraphics::SetFogRange(f32 nearPlane, f32 farPlane)
{
    fogNear = nearPlane;
    fogFar = farPlane;
}

void SoftwareGraphics::SetFogColor(ZunColor color)
{
    fogColor = color;
}

void SoftwareGraphics::SetColorOp(TextureOpComponent component, ColorOp op)
{
    if (component == COMPONENT_RGB)
    {
        colorOpRgb = op;
    }
    else
    {
        colorOpAlpha = op;
    }
}

void SoftwareGraphics::SetTextureFactor(ZunColor factor)
{
    textureFactor = factor;
    textureFactorWasSet = true;
}

void SoftwareGraphics::SetTextureArg(TextureArg arg)
{
    texArg = arg;
}

void SoftwareGraphics::SetTransformMatrix(TransformMatrix type, const ZunMatrix &matrix)
{
    transforms[type] = matrix;
}

void SoftwareGraphics::SetTextureFilter()
{
}

void SoftwareGraphics::GetViewport(ZunViewport &viewport)
{
    viewport = this->viewport;
}

void SoftwareGraphics::SetViewport(const ZunViewport &viewport)
{
    this->viewport = viewport;
}

void SoftwareGraphics::Enable(Capabilities cap)
{
    switch (cap)
    {
    case CAPS_BLEND:
        blend = true;
        break;
    case CAPS_ALPHA_TEST:
        alphaTest = true;
        break;
    case CAPS_DEPTH_TEST:
        depthTest = true;
        break;
    case CAPS_FOG:
        fog = true;
        break;
    }
}

void SoftwareGraphics::Disable(Capabilities cap)
{
    switch (cap)
    {
    case CAPS_BLEND:
        blend = false;
        break;
    case CAPS_ALPHA_TEST:
        alphaTest = false;
        break;
    case CAPS_DEPTH_TEST:
        depthTest = false;
        break;
    case CAPS_FOG:
        fog = false;
        break;
    }
}

void SoftwareGraphics::SetBlendMode(BlendMode srcMode, BlendMode dstMode)
{
    if (srcBlend == srcMode && dstBlend == dstMode)
    {
        return;
    }

    Flush();

    srcBlend = srcMode;
    dstBlend = dstMode;
}

void SoftwareGraphics::SetDepthMask(bool enable)
{
    if (depthMask == enable)
    {
        return;
    }

    Flush();

    depthMask = enable;
}

void SoftwareGraphics::SetDepthFunc(DepthFunc func)
{
    if (depthFunc == func)
    {
        return;
    }

    Flush();

    depthFunc = func;
}

void SoftwareGraphics::SetClearDepth(f32 depth)
{
    clearDepth = depth;
}

void SoftwareGraphics::SetClearColor(ZunColor color)
{
    clearColor = color;
}

void SoftwareGraphics::SetAlphaTestRef(u8 ref)
{
    alphaRef = ref;
}

void SoftwareGraphics::Clear(u32 clearBits)
{
    i32 startX = std::max(0, (i32)viewport.x);
    i32 startY = std::max(0, (i32)viewport.y);
    i32 endX = std::min(640, (i32)viewport.x + (i32)viewport.width);
    i32 endY = std::min(480, (i32)viewport.y + (i32)viewport.height);

    if (startX >= endX || startY >= endY)
    {
        return;
    }

    if (clearBits & CLEAR_COLOR_BUFFER)
    {
        for (i32 y = startY; y < endY; ++y)
        {
            std::fill(colorBuffer.begin() + (y * 640 + startX),
                      colorBuffer.begin() + (y * 640 + endX), clearColor.color);
        }
    }
    if (clearBits & CLEAR_DEPTH_BUFFER)
    {
        for (i32 y = startY; y < endY; ++y)
        {
            std::fill(depthBuffer.begin() + (y * 640 + startX),
                      depthBuffer.begin() + (y * 640 + endX), clearDepth);
        }
    }
}

GfxTextureHandle SoftwareGraphics::CreateTexture()
{
    return GfxTextureHandle(textureCounter++);
}

void SoftwareGraphics::BindTexture(GfxTextureHandle handle)
{
    currentTexture = handle.id;
}

void SoftwareGraphics::DeleteTexture(GfxTextureHandle handle)
{
    textures.erase(handle.id);
}

void SoftwareGraphics::SetTextureImage(u32 width, u32 height, PixelFormat fmt, PixelDataType type,
                                       const void *data)
{
    (void)fmt;
    (void)type;

    auto &tex = textures[currentTexture];
    tex.width = width;
    tex.height = height;
    tex.pixels.resize(width * height);

    const u8 *src = (const u8 *)data;
    for (size_t i = 0; i < width * height; ++i)
    {
        ZunColor c;
        c.bytes.r = src[i * 4 + 0];
        c.bytes.g = src[i * 4 + 1];
        c.bytes.b = src[i * 4 + 2];
        c.bytes.a = src[i * 4 + 3];
        tex.pixels[i] = c.color;
    }
}

void SoftwareGraphics::SetTextureSubImage(i32 xoffset, i32 yoffset, i32 width, i32 height,
                                          const void *data)
{
    auto &tex = textures[currentTexture];
    const u8 *src = (const u8 *)data;

    for (i32 y = 0; y < height; ++y)
    {
        for (i32 x = 0; x < width; ++x)
        {
            if (xoffset + x < (i32)tex.width && yoffset + y < (i32)tex.height)
            {
                ZunColor c;
                c.bytes.r = src[(y * width + x) * 4 + 0];
                c.bytes.g = src[(y * width + x) * 4 + 1];
                c.bytes.b = src[(y * width + x) * 4 + 2];
                c.bytes.a = src[(y * width + x) * 4 + 3];
                tex.pixels[(yoffset + y) * tex.width + (xoffset + x)] = c.color;
            }
        }
    }
}

void SoftwareGraphics::ReadPixels(i32 x, i32 y, i32 width, i32 height, void *pixels)
{
    u8 *dst = (u8 *)pixels;
    for (i32 py = 0; py < height; ++py)
    {
        for (i32 px = 0; px < width; ++px)
        {
            i32 sx = x + px;
            i32 sy = y + py;
            if (sx >= 0 && sx < 640 && sy >= 0 && sy < 480)
            {
                ZunColor c;
                c.color = colorBuffer[sy * 640 + sx];
                dst[(py * width + px) * 4 + 0] = c.bytes.r;
                dst[(py * width + px) * 4 + 1] = c.bytes.g;
                dst[(py * width + px) * 4 + 2] = c.bytes.b;
                dst[(py * width + px) * 4 + 3] = c.bytes.a;
            }
            else
            {
                dst[(py * width + px) * 4 + 0] = 0;
                dst[(py * width + px) * 4 + 1] = 0;
                dst[(py * width + px) * 4 + 2] = 0;
                dst[(py * width + px) * 4 + 3] = 0;
            }
        }
    }
}

ZunColor SoftwareGraphics::GetTexArgColor(ZunColor diffuse)
{
    switch (texArg)
    {
    case TEX_ARG_DIFFUSE:
        return diffuse;
        break;
    case TEX_ARG_TFACTOR:
        return textureFactor;
        break;
    case TEX_ARG_TEXTURE:
    default:
        return {0xFFFFFFFF};
    }
}

SoftwareVertex SoftwareGraphics::TransformVertex(const void *vData, i32 stride)
{
    SoftwareVertex out;

    if (stride == sizeof(VertexTex1DiffuseXyzrhw))
    {
        const VertexTex1DiffuseXyzrhw *v = (const VertexTex1DiffuseXyzrhw *)vData;
        out.x = v->pos.x;
        out.y = v->pos.y;
        out.z = v->pos.z;
        out.w = v->w;
        out.color = GetTexArgColor(v->color);
        out.u = v->textureUV.x;
        out.v = v->textureUV.y;
        out.fogCoord = v->pos.z;
        out.textured = true;
        out.screenSpace = true;
    }
    else if (stride == sizeof(VertexTex1DiffuseXyz))
    {
        const VertexTex1DiffuseXyz *v = (const VertexTex1DiffuseXyz *)vData;

        ZunMatrix temp = transforms[MATRIX_MODEL] * transforms[MATRIX_VIEW];
        ZunMatrix wvp = temp * transforms[MATRIX_PROJECTION];

        f32 x = v->position.x * wvp.m[0][0] + v->position.y * wvp.m[1][0] +
                v->position.z * wvp.m[2][0] + wvp.m[3][0];
        f32 y = v->position.x * wvp.m[0][1] + v->position.y * wvp.m[1][1] +
                v->position.z * wvp.m[2][1] + wvp.m[3][1];
        f32 z = v->position.x * wvp.m[0][2] + v->position.y * wvp.m[1][2] +
                v->position.z * wvp.m[2][2] + wvp.m[3][2];
        f32 w = v->position.x * wvp.m[0][3] + v->position.y * wvp.m[1][3] +
                v->position.z * wvp.m[2][3] + wvp.m[3][3];

        f32 rhw = (w != 0.0f) ? (1.0f / w) : 1.0f;
        out.x = viewport.x + (1.0f + x * rhw) * viewport.width * 0.5f;
        out.y = viewport.y + (1.0f - y * rhw) * viewport.height * 0.5f;
        out.z = viewport.minZ + z * rhw * (viewport.maxZ - viewport.minZ);
        out.w = rhw;
        out.color = GetTexArgColor(v->diffuse);

        const ZunMatrix &t = transforms[MATRIX_TEXTURE];
        out.u = v->textureUV.x * t.m[0][0] + v->textureUV.y * t.m[1][0] + t.m[2][0];
        out.v = v->textureUV.x * t.m[0][1] + v->textureUV.y * t.m[1][1] + t.m[2][1];
        out.fogCoord = w;
        out.textured = true;
        out.screenSpace = false;
    }
    else if (stride == sizeof(VertexDiffuseXyzrhw))
    {
        const VertexDiffuseXyzrhw *v = (const VertexDiffuseXyzrhw *)vData;

        out.x = v->pos.x;
        out.y = v->pos.y;
        out.z = v->pos.z;
        out.w = v->w;
        out.color = v->diffuse;
        out.u = 0.0f;
        out.v = 0.0f;
        out.fogCoord = v->pos.z;
        out.textured = false;
        out.screenSpace = true;
    }

    return out;
}

u32 SoftwareGraphics::Blend(u32 srcColor, u32 dstColor)
{
    if (!blend)
    {
        return srcColor;
    }

    ZunColor s;
    s.color = srcColor;
    ZunColor d;
    d.color = dstColor;
    ZunColor r;

    f32 srcAlpha = s.bytes.a / 255.0f;
    f32 invSrcAlpha = 1.0f - srcAlpha;

    f32 sr, sg, sb, sa;
    f32 dr, dg, db, da;

    if (srcBlend == BLEND_ALPHA)
    {
        sr = s.bytes.r * srcAlpha;
        sg = s.bytes.g * srcAlpha;
        sb = s.bytes.b * srcAlpha;
        sa = s.bytes.a * srcAlpha;
    }
    else if (srcBlend == BLEND_ONE)
    {
        sr = s.bytes.r;
        sg = s.bytes.g;
        sb = s.bytes.b;
        sa = s.bytes.a;
    }
    else
    {
        sr = s.bytes.r;
        sg = s.bytes.g;
        sb = s.bytes.b;
        sa = s.bytes.a;
    }

    if (dstBlend == BLEND_ALPHA)
    {
        dr = d.bytes.r * invSrcAlpha;
        dg = d.bytes.g * invSrcAlpha;
        db = d.bytes.b * invSrcAlpha;
        da = d.bytes.a * invSrcAlpha;
    }
    else if (dstBlend == BLEND_ONE)
    {
        dr = d.bytes.r;
        dg = d.bytes.g;
        db = d.bytes.b;
        da = d.bytes.a;
    }
    else
    {
        dr = 0;
        dg = 0;
        db = 0;
        da = 0;
    }

    r.bytes.r = (u8)std::min(255.0f, sr + dr);
    r.bytes.g = (u8)std::min(255.0f, sg + dg);
    r.bytes.b = (u8)std::min(255.0f, sb + db);
    r.bytes.a = (u8)std::min(255.0f, sa + da);

    return r.color;
}

u32 SoftwareGraphics::ApplyFog(u32 color, f32 z)
{
    if (!fog)
    {
        return color;
    }

    f32 f = (fogFar - z) / (fogFar - fogNear);
    if (f < 0.0f)
    {
        f = 0.0f;
    }
    if (f > 1.0f)
    {
        f = 1.0f;
    }

    ZunColor c;
    c.color = color;
    ZunColor r;
    r.bytes.a = c.bytes.a;
    r.bytes.r = (u8)(c.bytes.r * f + fogColor.bytes.r * (1.0f - f));
    r.bytes.g = (u8)(c.bytes.g * f + fogColor.bytes.g * (1.0f - f));
    r.bytes.b = (u8)(c.bytes.b * f + fogColor.bytes.b * (1.0f - f));

    return r.color;
}

void SoftwareGraphics::DrawTriangle(const SoftwareVertex &v0, SoftwareVertex &v1,
                                    SoftwareVertex &v2)
{
    auto edgeFunction = [](const SoftwareVertex &a, const SoftwareVertex &b, f32 x, f32 y) {
        return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
    };

    f32 area = edgeFunction(v0, v1, v2.x, v2.y);
    if (std::abs(area) < 0.0001f)
    {
        return;
    }

    if (area < 0.0f)
    {
        std::swap(v1, v2);
        area = -area;
    }

    f32 invArea = 1.0f / area;

    i32 minX = std::max((i32)viewport.x, (i32)std::floor(std::min({v0.x, v1.x, v2.x})));
    i32 maxX = std::min((i32)(viewport.x + viewport.width - 1),
                        (i32)std::ceil(std::max({v0.x, v1.x, v2.x})));
    i32 minY = std::max((i32)viewport.y, (i32)std::floor(std::min({v0.y, v1.y, v2.y})));
    i32 maxY = std::min((i32)(viewport.y + viewport.height - 1),
                        (i32)std::ceil(std::max({v0.y, v1.y, v2.y})));

    bool useTex = v0.textured && currentTexture != 0;

    for (i32 y = minY; y <= maxY; ++y)
    {
        for (i32 x = minX; x <= maxX; ++x)
        {
            f32 px = x + 0.5f;
            f32 py = y + 0.5f;

            f32 w0 = edgeFunction(v1, v2, px, py);
            f32 w1 = edgeFunction(v2, v0, px, py);
            f32 w2 = edgeFunction(v0, v1, px, py);

            if (w0 >= 0 && w1 >= 0 && w2 >= 0)
            {
                w0 *= invArea;
                w1 *= invArea;
                w2 *= invArea;

                f32 invW = w0 * v0.w + w1 * v1.w + w2 * v2.w;
                f32 w = (invW != 0.0f) ? (1.0f / invW) : 1.0f;

                f32 z;
                if (v0.screenSpace)
                {
                    z = w0 * v0.z + w1 * v1.z + w2 * v2.z;
                }
                else
                {
                    z = (w0 * v0.z * v0.w + w1 * v1.z * v1.w + w2 * v2.z * v2.w) * w;
                }

                if (depthTest)
                {
                    if (depthFunc == DEPTH_FUNC_LEQUAL && z > depthBuffer[y * 640 + x])
                    {
                        continue;
                    }
                }

                f32 u = (w0 * v0.u * v0.w + w1 * v1.u * v1.w + w2 * v2.u * v2.w) * w;
                f32 v = (w0 * v0.v * v0.w + w1 * v1.v * v1.w + w2 * v2.v * v2.w) * w;
                f32 fogCoord =
                    (w0 * v0.fogCoord * v0.w + w1 * v1.fogCoord * v1.w + w2 * v2.fogCoord * v2.w) *
                    w;

                ZunColor c0 = v0.color;
                ZunColor c1 = v1.color;
                ZunColor c2 = v2.color;

                f32 cr =
                    (w0 * c0.bytes.r * v0.w + w1 * c1.bytes.r * v1.w + w2 * c2.bytes.r * v2.w) * w;
                f32 cg =
                    (w0 * c0.bytes.g * v0.w + w1 * c1.bytes.g * v1.w + w2 * c2.bytes.g * v2.w) * w;
                f32 cb =
                    (w0 * c0.bytes.b * v0.w + w1 * c1.bytes.b * v1.w + w2 * c2.bytes.b * v2.w) * w;
                f32 ca =
                    (w0 * c0.bytes.a * v0.w + w1 * c1.bytes.a * v1.w + w2 * c2.bytes.a * v2.w) * w;

                ZunColor vColor;
                vColor.bytes.r = (u8)std::clamp(cr, 0.0f, 255.0f);
                vColor.bytes.g = (u8)std::clamp(cg, 0.0f, 255.0f);
                vColor.bytes.b = (u8)std::clamp(cb, 0.0f, 255.0f);
                vColor.bytes.a = (u8)std::clamp(ca, 0.0f, 255.0f);

                ZunColor texColor = SampleTexture(u, v);

                ZunColor finalColor;

                if (!useTex)
                {
                    finalColor = vColor;
                }
                else
                {
                    if (colorOpRgb == COLOR_OP_DISABLE)
                    {
                        finalColor.bytes.r = vColor.bytes.r;
                        finalColor.bytes.g = vColor.bytes.g;
                        finalColor.bytes.b = vColor.bytes.b;
                    }
                    else if (colorOpRgb == COLOR_OP_MODULATE)
                    {
                        finalColor.bytes.r = (texColor.bytes.r * vColor.bytes.r) / 255;
                        finalColor.bytes.g = (texColor.bytes.g * vColor.bytes.g) / 255;
                        finalColor.bytes.b = (texColor.bytes.b * vColor.bytes.b) / 255;
                    }
                    else if (colorOpRgb == COLOR_OP_ADD)
                    {
                        finalColor.bytes.r = std::min(255, texColor.bytes.r + vColor.bytes.r);
                        finalColor.bytes.g = std::min(255, texColor.bytes.g + vColor.bytes.g);
                        finalColor.bytes.b = std::min(255, texColor.bytes.b + vColor.bytes.b);
                    }
                    else
                    {
                        finalColor.bytes.r = texColor.bytes.r;
                        finalColor.bytes.g = texColor.bytes.g;
                        finalColor.bytes.b = texColor.bytes.b;
                    }

                    if (colorOpAlpha == COLOR_OP_DISABLE)
                    {
                        finalColor.bytes.a = vColor.bytes.a;
                    }
                    else if (colorOpAlpha == COLOR_OP_MODULATE)
                    {
                        finalColor.bytes.a = (texColor.bytes.a * vColor.bytes.a) / 255;
                    }
                    else if (colorOpAlpha == COLOR_OP_ADD)
                    {
                        finalColor.bytes.a = std::min(255, texColor.bytes.a + vColor.bytes.a);
                    }
                    else
                    {
                        finalColor.bytes.a = texColor.bytes.a;
                    }
                }

                if (alphaTest && finalColor.bytes.a < alphaRef)
                {
                    continue;
                }

                if (!v0.screenSpace || !v1.screenSpace || !v2.screenSpace)
                {
                    finalColor.color = ApplyFog(finalColor.color, fogCoord);
                }

                u32 dstColor = colorBuffer[y * 640 + x];
                u32 outColor = Blend(finalColor.color, dstColor);

                colorBuffer[y * 640 + x] = outColor;
                if (depthMask)
                {
                    depthBuffer[y * 640 + x] = z;
                }
            }
        }
    }
}

void SoftwareGraphics::DrawPrimitive(PrimitiveType type, i32 startVertex, i32 primitiveCount)
{
    assert(false);
}

void SoftwareGraphics::DrawPrimitiveUP(PrimitiveType type, i32 primitiveCount,
                                       const void *vertexData, i32 vertexStride)
{
    const u8 *data = (const u8 *)vertexData;

    if (type == PRIM_TRIANGLES)
    {
        for (i32 i = 0; i < primitiveCount; i++)
        {
            SoftwareVertex v0 = TransformVertex(data + (i * 3 + 0) * vertexStride, vertexStride);
            SoftwareVertex v1 = TransformVertex(data + (i * 3 + 1) * vertexStride, vertexStride);
            SoftwareVertex v2 = TransformVertex(data + (i * 3 + 2) * vertexStride, vertexStride);
            DrawTriangle(v0, v1, v2);
        }
    }
    else if (type == PRIM_TRIANGLE_STRIP)
    {
        for (i32 i = 0; i < primitiveCount; i++)
        {
            SoftwareVertex v0 = TransformVertex(data + i * vertexStride, vertexStride);
            SoftwareVertex v1 = TransformVertex(data + (i + 1) * vertexStride, vertexStride);
            SoftwareVertex v2 = TransformVertex(data + (i + 2) * vertexStride, vertexStride);

            if (i % 2 == 1)
            {
                SoftwareVertex temp = v1;
                v1 = v2;
                v2 = temp;
            }

            DrawTriangle(v0, v1, v2);
        }
    }
    else if (type == PRIM_TRIANGLE_FAN)
    {
        SoftwareVertex v0 = TransformVertex(data, vertexStride);
        for (i32 i = 0; i < primitiveCount; i++)
        {
            SoftwareVertex v1 = TransformVertex(data + (i + 1) * vertexStride, vertexStride);
            SoftwareVertex v2 = TransformVertex(data + (i + 2) * vertexStride, vertexStride);
            DrawTriangle(v0, v1, v2);
        }
    }
}

void SoftwareGraphics::SwapBuffers()
{
    SDL_UpdateTexture(screenTexture, NULL, colorBuffer.data(), 640 * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, screenTexture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

ZunColor SoftwareGraphics::SampleTexture(f32 u, f32 v)
{
    auto it = textures.find(currentTexture);
    if (it == textures.end() || it->second.width == 0 || it->second.height == 0)
    {
        return {0xFFFFFFFF};
    }

    SoftTexture &tex = it->second;

    f32 fu = u * tex.width - 0.5f;
    f32 fv = v * tex.height - 0.5f;

    i32 iu = (i32)std::floor(fu);
    i32 iv = (i32)std::floor(fv);
    f32 fracU = fu - iu;
    f32 fracV = fv - iv;

    i32 u0 = ((iu % (i32)tex.width) + (i32)tex.width) % (i32)tex.width;
    i32 u1 = (u0 + 1) % (i32)tex.width;
    i32 v0 = ((iv % (i32)tex.height) + (i32)tex.height) % (i32)tex.height;
    i32 v1 = (v0 + 1) % (i32)tex.height;

    ZunColor t00, t10, t01, t11;
    t00.color = tex.pixels[v0 * tex.width + u0];
    t10.color = tex.pixels[v0 * tex.width + u1];
    t01.color = tex.pixels[v1 * tex.width + u0];
    t11.color = tex.pixels[v1 * tex.width + u1];

    f32 w00 = (1.0f - fracU) * (1.0f - fracV);
    f32 w10 = fracU * (1.0f - fracV);
    f32 w01 = (1.0f - fracU) * fracV;
    f32 w11 = fracU * fracV;

    ZunColor result;
    result.bytes.r =
        (u8)(t00.bytes.r * w00 + t10.bytes.r * w10 + t01.bytes.r * w01 + t11.bytes.r * w11 + 0.5f);
    result.bytes.g =
        (u8)(t00.bytes.g * w00 + t10.bytes.g * w10 + t01.bytes.g * w01 + t11.bytes.g * w11 + 0.5f);
    result.bytes.b =
        (u8)(t00.bytes.b * w00 + t10.bytes.b * w10 + t01.bytes.b * w01 + t11.bytes.b * w11 + 0.5f);
    result.bytes.a =
        (u8)(t00.bytes.a * w00 + t10.bytes.a * w10 + t01.bytes.a * w01 + t11.bytes.a * w11 + 0.5f);

    return result;
}
