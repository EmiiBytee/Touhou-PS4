#pragma once

// software renderer. this is horrifyingly slow, unless you're running on release (where it is still
// slower but at least not horrible)

#include <SDL2/SDL.h>
#include <unordered_map>
#include <vector>

#include "AnmManager.hpp"
#include "ZunGraphics.hpp"

struct SoftTexture
{
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> pixels;
};

struct SoftwareVertex
{
    f32 x, y, z, w;
    f32 fogCoord;
    ZunColor color;
    f32 u, v;
    bool textured;
    bool screenSpace;
};

class SoftwareGraphics : public ZunGraphics
{
  public:
    static ZunGraphics *Init();

    ~SoftwareGraphics() override
    {
        Exit();
    }

    void Exit() override;

    RendererType GetType() override
    {
        return RENDERER_SOFTWARE;
    }

    void SetFogRange(f32 nearPlane, f32 farPlane) override;
    void SetFogColor(ZunColor color) override;
    void SetColorOp(TextureOpComponent component, ColorOp op) override;
    void SetTextureFactor(ZunColor factor) override;
    void SetTransformMatrix(TransformMatrix type, const ZunMatrix &matrix) override;
    void SetTextureFilter() override;

    void GetViewport(ZunViewport &viewport) override;
    void SetViewport(const ZunViewport &viewport) override;

    void Enable(Capabilities cap) override;
    void Disable(Capabilities cap) override;

    void SetBlendMode(BlendMode srcMode, BlendMode dstMode) override;
    void SetDepthMask(bool enable) override;
    void SetDepthFunc(DepthFunc func) override;

    void SetTextureArg(TextureArg arg) override;

    void SetClearDepth(f32 depth) override;
    void SetClearColor(ZunColor color) override;
    void SetAlphaTestRef(u8 ref) override;
    void Clear(u32 clearBits) override;

    GfxTextureHandle CreateTexture() override;
    void BindTexture(GfxTextureHandle handle) override;
    void DeleteTexture(GfxTextureHandle handle) override;
    void SetTextureImage(u32 width, u32 height, PixelFormat fmt, PixelDataType type,
                         const void *data) override;
    void SetTextureSubImage(i32 xoffset, i32 yoffset, i32 width, i32 height,
                            const void *data) override;

    void ReadPixels(i32 x, i32 y, i32 width, i32 height, void *pixels) override;
    void DrawPrimitive(PrimitiveType type, i32 startVertex, i32 primitiveCount) override;
    void DrawPrimitiveUP(PrimitiveType type, i32 primitiveCount, const void *vertexData,
                         i32 vertexStride) override;

    void SwapBuffers() override;

  private:
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *screenTexture = nullptr;

    std::vector<u32> colorBuffer;
    std::vector<f32> depthBuffer;

    std::unordered_map<u32, SoftTexture> textures;
    u32 textureCounter = 1;
    u32 currentTexture = 0;

    ZunMatrix transforms[4];
    ZunViewport viewport;

    bool depthTest = false;
    bool depthMask = true;
    bool blend = false;
    bool fog = false;
    bool alphaTest = false;

    DepthFunc depthFunc = DEPTH_FUNC_LEQUAL;
    BlendMode srcBlend = BLEND_ALPHA;
    BlendMode dstBlend = BLEND_ALPHA;

    ColorOp colorOpRgb = COLOR_OP_MODULATE;
    ColorOp colorOpAlpha = COLOR_OP_MODULATE;

    TextureArg texArg = TEX_ARG_DIFFUSE;

    ZunColor clearColor = {0};
    f32 clearDepth = 1.0f;
    u8 alphaRef = 0;

    f32 fogNear = 0.0f;
    f32 fogFar = 1.0f;
    ZunColor fogColor = {0};

    ZunColor textureFactor = {0xFFFFFFFF};
    bool textureFactorWasSet = false;

    void Flush()
    {
        if (g_AnmManager && g_AnmManager->spritesToDraw != 0)
        {
            g_AnmManager->Flush();
        }
    }

    SoftwareVertex TransformVertex(const void *vData, i32 stride);
    void DrawTriangle(const SoftwareVertex &v0, SoftwareVertex &v1, SoftwareVertex &v2);
    u32 Blend(u32 src, u32 dst);
    u32 ApplyFog(u32 color, f32 z);
    ZunColor SampleTexture(f32 u, f32 v);
    ZunColor GetTexArgColor(ZunColor diffuse);
};
