#pragma once

#include <GLES3/gl3.h>
#include <SDL_video.h>

#include "AnmManager.hpp"
#include "ZunGraphics.hpp"

class GlesGraphics : public ZunGraphics
{
  public:
    static ZunGraphics *Init();

    ~GlesGraphics() override
    {
        Exit();
    }

    void Exit() override;

    RendererType GetType() override
    {
        return RENDERER_OPENGLES;
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
    SDL_GLContext ctx;
    u32 shaderProgram;
    u32 vaos[3];
    u32 vbo;
    u8 alphaRef = 0;
    TextureArg texArg = TEX_ARG_DIFFUSE;
    ZunColor textureFactor = {0xFFFFFFFF};
    ColorOp colorOpRgb = COLOR_OP_MODULATE;
    ColorOp colorOpAlpha = COLOR_OP_MODULATE;

    GLuint unitQuadVao = 0;
    GLuint unitQuadVbo = 0;

    ZunMatrix transforms[4];
    ZunViewport viewport;
    bool fogEnabled = false;
    f32 fogNear = 0.0f;
    f32 fogFar = 1.0f;
    ZunColor fogColor = {0};
    bool alphaTestEnabled = false;
    bool depthMaskEnabled = true;

    GLint u_Model, u_View, u_Proj, u_TextureMatrix;
    GLint u_ScreenSpace, u_Viewport;
    GLint u_UseTexture, u_Texture;
    GLint u_ColorOpRgb, u_ColorOpAlpha, u_TexArg, u_TextureFactor;
    GLint u_AlphaTest, u_AlphaRef;
    GLint u_FogEnabled, u_FogColor, u_FogNear, u_FogFar;

    u64 prevTicks = 0;

    void Flush()
    {
        if (g_AnmManager && g_AnmManager->spritesToDraw != 0)
        {
            g_AnmManager->Flush();
        }
    }

    static GLuint CompileShader(GLenum type, const char *source)
    {
        u32 shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint isCompiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &isCompiled);
        if (!isCompiled)
        {
            char log[512];
            glGetShaderInfoLog(shader, 512, nullptr, log);
            Supervisor::DebugPrint("shader compile error: %s\n", log);
            return 0;
        }
        return shader;
    }
};
