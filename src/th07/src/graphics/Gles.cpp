#include "Gles.hpp"

#include <GLES3/gl3.h>
#include <SDL_error.h>
#include <SDL_timer.h>
#include <SDL_video.h>

#include "AnmManager.hpp"
#include "GameWindow.hpp"
#include "Supervisor.hpp"

const char *vertexShaderSource =
    "#version 300 es\n"
    "uniform mat4 u_Model;\n"
    "uniform mat4 u_View;\n"
    "uniform mat4 u_Proj;\n"
    "uniform mat4 u_TextureMatrix;\n"
    "uniform bool u_ScreenSpace;\n"
    "uniform vec4 u_Viewport;\n"
    "\n"
    "layout(location = 0) in vec3 a_Position;\n"
    "layout(location = 1) in vec4 a_Color;\n"
    "layout(location = 2) in vec2 a_TexCoord;\n"
    "\n"
    "out vec4 v_Color;\n"
    "out vec2 v_TexCoord;\n"
    "out float v_FogFragCoord;\n"
    "\n"
    "void main() {\n"
    "    v_Color = a_Color;\n"
    "    if (u_ScreenSpace) {\n"
    "        float x = (a_Position.x - u_Viewport.x) / u_Viewport.z * 2.0 - 1.0;\n"
    "        float y = 1.0 - (a_Position.y - u_Viewport.y) / u_Viewport.w * 2.0;\n"
    "        gl_Position = vec4(x, y, a_Position.z, 1.0);\n"
    "        v_TexCoord = a_TexCoord;\n"
    "        v_FogFragCoord = a_Position.z;\n"
    "    } else {\n"
    "        vec4 worldPos = u_Model * vec4(a_Position, 1.0);\n"
    "        vec4 viewPos = u_View * worldPos;\n"
    "        gl_Position = u_Proj * viewPos;\n"
    "        v_TexCoord = (u_TextureMatrix * vec4(a_TexCoord, 1.0, 0.0)).xy;\n"
    "        v_FogFragCoord = length(viewPos.xyz);\n"
    "    }\n"
    "}\n";

const char *fragmentShaderSource =
    "#version 300 es\n"
    "precision mediump float;\n"
    "\n"
    "in vec4 v_Color;\n"
    "in vec2 v_TexCoord;\n"
    "in float v_FogFragCoord;\n"
    "\n"
    "uniform sampler2D u_Texture;\n"
    "uniform bool u_UseTexture;\n"
    "uniform int u_ColorOpRgb;\n"
    "uniform int u_ColorOpAlpha;\n"
    "uniform int u_TexArg;\n"
    "uniform vec4 u_TextureFactor;\n"
    "uniform bool u_AlphaTest;\n"
    "uniform float u_AlphaRef;\n"
    "uniform bool u_FogEnabled;\n"
    "uniform vec4 u_FogColor;\n"
    "uniform float u_FogNear;\n"
    "uniform float u_FogFar;\n"
    "\n"
    "out vec4 FragColor;\n"
    "\n"
    "void main() {\n"
    "    vec4 texColor = vec4(1.0);\n"
    "    if (u_UseTexture) {\n"
    "        texColor = texture(u_Texture, v_TexCoord);\n"
    "    }\n"
    "    \n"
    "    vec4 argColor = v_Color.bgra;\n"
    "    if (u_TexArg == 1) { // TEXTURE\n"
    "        argColor = vec4(1.0);\n"
    "    } else if (u_TexArg == 2) { // TFACTOR\n"
    "        argColor = u_TextureFactor;\n"
    "    }\n"
    "    \n"
    "    vec4 finalColor = v_Color;\n"
    "    \n"
    "    if (u_UseTexture) {\n"
    "        if (u_ColorOpRgb == 0) finalColor.rgb = texColor.rgb * argColor.rgb;\n"
    "        else if (u_ColorOpRgb == 1) finalColor.rgb = min(texColor.rgb + argColor.rgb, "
    "vec3(1.0));\n"
    "        else if (u_ColorOpRgb == 2) finalColor.rgb = texColor.rgb;\n"
    "        else if (u_ColorOpRgb == 3) finalColor.rgb = argColor.rgb;\n"
    "        \n"
    "        if (u_ColorOpAlpha == 0) finalColor.a = texColor.a * argColor.a;\n"
    "        else if (u_ColorOpAlpha == 1) finalColor.a = min(texColor.a + argColor.a, 1.0);\n"
    "        else if (u_ColorOpAlpha == 2) finalColor.a = texColor.a;\n"
    "        else if (u_ColorOpAlpha == 3) finalColor.a = argColor.a;\n"
    "    } else {\n"
    "        finalColor = argColor;\n"
    "    }\n"
    "    \n"
    "    if (u_AlphaTest && finalColor.a < u_AlphaRef) {\n"
    "        discard;\n"
    "    }\n"
    "    \n"
    "    if (u_FogEnabled) {\n"
    "        float f = (u_FogFar - v_FogFragCoord) / (u_FogFar - u_FogNear);\n"
    "        f = clamp(f, 0.0, 1.0);\n"
    "        finalColor.rgb = mix(u_FogColor.rgb, finalColor.rgb, f);\n"
    "    }\n"
    "    \n"
    "    FragColor = finalColor;\n"
    "}\n";

ZunGraphics *GlesGraphics::Init()
{
    GlesGraphics *gfx = new GlesGraphics;

    SDL_GLContext ctx = SDL_GL_CreateContext(g_GameWindow.window);
    if (!ctx)
    {
        delete gfx;
        Supervisor::DebugPrint("gles renderer create failed: %s\n", SDL_GetError());
        return nullptr;
    }
    gfx->ctx = ctx;

    SDL_GL_MakeCurrent(g_GameWindow.window, ctx);

    if (SDL_GL_SetSwapInterval(1) < 0)
    {
        // technically this isnt fatal we just go into 60 fps later on in gamewindow::render
        Supervisor::DebugPrint("SDL_GL_SetSwapInterval failed: %s\n", SDL_GetError());
    }

    RenderVertexInfo unitQuadData[4] = {{{-128.0f, -128.0f, 0.0f}, {0.0f, 0.0f}},
                                        {{128.0f, -128.0f, 0.0f}, {1.0f, 0.0f}},
                                        {{-128.0f, 128.0f, 0.0f}, {0.0f, 1.0f}},
                                        {{128.0f, 128.0f, 0.0f}, {1.0f, 1.0f}}};

    glGenVertexArrays(1, &gfx->unitQuadVao);
    glGenBuffers(1, &gfx->unitQuadVbo);
    glBindVertexArray(gfx->unitQuadVao);
    glBindBuffer(GL_ARRAY_BUFFER, gfx->unitQuadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(unitQuadData), unitQuadData, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertexInfo),
                          (void *)offsetof(RenderVertexInfo, position));
    glDisableVertexAttribArray(1);
    glVertexAttrib4f(1, 1.0f, 1.0f, 1.0f, 1.0f);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertexInfo),
                          (void *)offsetof(RenderVertexInfo, textureUV));
    glBindVertexArray(0);

    u32 vertexShader = CompileShader(GL_VERTEX_SHADER, vertexShaderSource);
    u32 fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

    if (vertexShader == 0 || fragmentShader == 0)
    {
        return nullptr;
    }

    gfx->shaderProgram = glCreateProgram();
    glAttachShader(gfx->shaderProgram, vertexShader);
    glAttachShader(gfx->shaderProgram, fragmentShader);
    glLinkProgram(gfx->shaderProgram);
    glUseProgram(gfx->shaderProgram);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    gfx->u_Model = glGetUniformLocation(gfx->shaderProgram, "u_Model");
    gfx->u_View = glGetUniformLocation(gfx->shaderProgram, "u_View");
    gfx->u_Proj = glGetUniformLocation(gfx->shaderProgram, "u_Proj");
    gfx->u_TextureMatrix = glGetUniformLocation(gfx->shaderProgram, "u_TextureMatrix");
    gfx->u_ScreenSpace = glGetUniformLocation(gfx->shaderProgram, "u_ScreenSpace");
    gfx->u_Viewport = glGetUniformLocation(gfx->shaderProgram, "u_Viewport");
    gfx->u_UseTexture = glGetUniformLocation(gfx->shaderProgram, "u_UseTexture");
    gfx->u_Texture = glGetUniformLocation(gfx->shaderProgram, "u_Texture");
    gfx->u_ColorOpRgb = glGetUniformLocation(gfx->shaderProgram, "u_ColorOpRgb");
    gfx->u_ColorOpAlpha = glGetUniformLocation(gfx->shaderProgram, "u_ColorOpAlpha");
    gfx->u_TexArg = glGetUniformLocation(gfx->shaderProgram, "u_TexArg");
    gfx->u_TextureFactor = glGetUniformLocation(gfx->shaderProgram, "u_TextureFactor");
    gfx->u_AlphaTest = glGetUniformLocation(gfx->shaderProgram, "u_AlphaTest");
    gfx->u_AlphaRef = glGetUniformLocation(gfx->shaderProgram, "u_AlphaRef");
    gfx->u_FogEnabled = glGetUniformLocation(gfx->shaderProgram, "u_FogEnabled");
    gfx->u_FogColor = glGetUniformLocation(gfx->shaderProgram, "u_FogColor");
    gfx->u_FogNear = glGetUniformLocation(gfx->shaderProgram, "u_FogNear");
    gfx->u_FogFar = glGetUniformLocation(gfx->shaderProgram, "u_FogFar");

    glGenVertexArrays(3, gfx->vaos);
    glGenBuffers(1, &gfx->vbo);

    i32 vertexStride = sizeof(VertexTex1DiffuseXyzrhw);
    glBindVertexArray(gfx->vaos[0]);
    glBindBuffer(GL_ARRAY_BUFFER, gfx->vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertexStride,
                          (void *)offsetof(VertexTex1DiffuseXyzrhw, pos));
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, vertexStride,
                          (void *)offsetof(VertexTex1DiffuseXyzrhw, color));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, vertexStride,
                          (void *)offsetof(VertexTex1DiffuseXyzrhw, textureUV));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    vertexStride = sizeof(VertexTex1DiffuseXyz);
    glBindVertexArray(gfx->vaos[1]);
    glBindBuffer(GL_ARRAY_BUFFER, gfx->vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertexStride,
                          (void *)offsetof(VertexTex1DiffuseXyz, position));
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, vertexStride,
                          (void *)offsetof(VertexTex1DiffuseXyz, diffuse));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, vertexStride,
                          (void *)offsetof(VertexTex1DiffuseXyz, textureUV));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    vertexStride = sizeof(VertexDiffuseXyzrhw);
    glBindVertexArray(gfx->vaos[2]);
    glBindBuffer(GL_ARRAY_BUFFER, gfx->vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertexStride,
                          (void *)offsetof(VertexDiffuseXyzrhw, pos));
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, vertexStride,
                          (void *)offsetof(VertexDiffuseXyzrhw, diffuse));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);

    for (i32 i = 0; i < 4; i++)
    {
        gfx->transforms[i].Identity();
    }

    glUniform1i(gfx->u_Texture, 0);

    Supervisor::DebugPrint("using gles rendering.\n");

    return gfx;
}

void GlesGraphics::Exit()
{
    SDL_GL_DeleteContext(this->ctx);
}

void GlesGraphics::SetFogRange(f32 nearPlane, f32 farPlane)
{
    fogNear = nearPlane;
    fogFar = farPlane;
}

void GlesGraphics::SetFogColor(ZunColor color)
{
    fogColor = color;
}

void GlesGraphics::SetColorOp(TextureOpComponent component, ColorOp op)
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

void GlesGraphics::SetTextureFactor(ZunColor factor)
{
    textureFactor = factor;
}

void GlesGraphics::SetTextureArg(TextureArg arg)
{
    texArg = arg;
}

void GlesGraphics::SetTransformMatrix(TransformMatrix type, const ZunMatrix &matrix)
{
    transforms[type] = matrix;
}

void GlesGraphics::SetTextureFilter()
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void GlesGraphics::GetViewport(ZunViewport &viewport)
{
    viewport = this->viewport;
}

void GlesGraphics::SetViewport(const ZunViewport &viewport)
{
    this->viewport = viewport;
    glViewport(viewport.x, 480 - (viewport.y + viewport.height), viewport.width, viewport.height);
}

void GlesGraphics::Enable(Capabilities cap)
{
    switch (cap)
    {
    case CAPS_BLEND:
        glEnable(GL_BLEND);
        break;
    case CAPS_DEPTH_TEST:
        glEnable(GL_DEPTH_TEST);
        break;
    case CAPS_ALPHA_TEST:
        alphaTestEnabled = true;
        break;
    case CAPS_FOG:
        fogEnabled = true;
        break;
    }
}

void GlesGraphics::Disable(Capabilities cap)
{
    switch (cap)
    {
    case CAPS_BLEND:
        glDisable(GL_BLEND);
        break;
    case CAPS_DEPTH_TEST:
        glDisable(GL_DEPTH_TEST);
        break;
    case CAPS_ALPHA_TEST:
        alphaTestEnabled = false;
        break;
    case CAPS_FOG:
        fogEnabled = false;
        break;
    }
}

void GlesGraphics::SetBlendMode(BlendMode srcMode, BlendMode dstMode)
{
    Flush();

    GLenum glSrcMode = GL_SRC_ALPHA;
    switch (srcMode)
    {
    case BLEND_ALPHA:
        glSrcMode = GL_SRC_ALPHA;
        break;
    case BLEND_ONE:
        glSrcMode = GL_ONE;
        break;
    case BLEND_NONE:
        glSrcMode = GL_ONE;
        break;
    }

    GLenum glDstMode = GL_ONE_MINUS_SRC_ALPHA;
    switch (dstMode)
    {
    case BLEND_ALPHA:
        glDstMode = GL_ONE_MINUS_SRC_ALPHA;
        break;
    case BLEND_ONE:
        glDstMode = GL_ONE;
        break;
    case BLEND_NONE:
        glDstMode = GL_ZERO;
        break;
    }
    glBlendFunc(glSrcMode, glDstMode);
}

void GlesGraphics::SetDepthMask(bool enable)
{
    Flush();

    depthMaskEnabled = enable;
    glDepthMask(enable);
}

void GlesGraphics::SetDepthFunc(DepthFunc func)
{
    Flush();

    switch (func)
    {
    case DEPTH_FUNC_LEQUAL:
        glDepthFunc(GL_LEQUAL);
        break;
    case DEPTH_FUNC_ALWAYS:
        glDepthFunc(GL_ALWAYS);
        break;
    }
}

void GlesGraphics::SetClearDepth(f32 depth)
{
    glClearDepthf(depth);
}

void GlesGraphics::SetClearColor(ZunColor color)
{
    glClearColor(color.bytes.r / 255.0f, color.bytes.g / 255.0f, color.bytes.b / 255.0f,
                 color.bytes.a / 255.0f);
}

void GlesGraphics::SetAlphaTestRef(u8 ref)
{
    alphaRef = ref;
}

void GlesGraphics::Clear(u32 clearBits)
{
    GLbitfield bits = 0;
    if (clearBits & CLEAR_COLOR_BUFFER)
    {
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (clearBits & CLEAR_DEPTH_BUFFER)
    {
        bits |= GL_DEPTH_BUFFER_BIT;
        if (!depthMaskEnabled)
        {
            glDepthMask(GL_TRUE);
        }
    }
    glClear(bits);
    if ((clearBits & CLEAR_DEPTH_BUFFER) && !depthMaskEnabled)
    {
        glDepthMask(GL_FALSE);
    }
}

GfxTextureHandle GlesGraphics::CreateTexture()
{
    GLuint tex;
    glGenTextures(1, &tex);
    return GfxTextureHandle(tex);
}

void GlesGraphics::BindTexture(GfxTextureHandle handle)
{
    glBindTexture(GL_TEXTURE_2D, handle.id);
}

void GlesGraphics::DeleteTexture(GfxTextureHandle handle)
{
    GLuint tex = handle.id;
    glDeleteTextures(1, &tex);
}

void GlesGraphics::SetTextureImage(u32 width, u32 height, PixelFormat fmt, PixelDataType type,
                                   const void *data)
{
    GLenum internalformat;
    GLenum format;
    GLenum datatype;

    switch (fmt)
    {
    case PIXEL_RGB:
        internalformat = GL_RGB;
        format = GL_RGB;
        break;
    case PIXEL_RGBA:
    default:
        internalformat = GL_RGBA;
        format = GL_RGBA;
        break;
    }

    switch (type)
    {
    case PIXEL_UNSIGNED_BYTE:
        datatype = GL_UNSIGNED_BYTE;
        break;
    case PIXEL_UNSIGNED_SHORT_5_5_5_1:
        datatype = GL_UNSIGNED_SHORT_5_5_5_1;
        break;
    case PIXEL_UNSIGNED_SHORT_5_6_5:
        datatype = GL_UNSIGNED_SHORT_5_6_5;
        break;
    case PIXEL_UNSIGNED_SHORT_4_4_4_4:
        datatype = GL_UNSIGNED_SHORT_4_4_4_4;
        break;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, internalformat, width, height, 0, format, datatype, data);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void GlesGraphics::SetTextureSubImage(i32 xoffset, i32 yoffset, i32 width, i32 height,
                                      const void *data)
{
    glTexSubImage2D(GL_TEXTURE_2D, 0, xoffset, yoffset, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                    data);
}

void GlesGraphics::ReadPixels(i32 x, i32 y, i32 width, i32 height, void *pixels)
{
    glReadPixels(x, 480 - (y + height), width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    u32 rowSize = width * 4;
    u8 *p = (u8 *)pixels;
    u8 *tempRow = new u8[rowSize];
    for (i32 i = 0; i < height / 2; ++i)
    {
        u8 *top = p + i * rowSize;
        u8 *bottom = p + (height - 1 - i) * rowSize;
        memcpy(tempRow, top, rowSize);
        memcpy(top, bottom, rowSize);
        memcpy(bottom, tempRow, rowSize);
    }
    delete[] tempRow;
}

void GlesGraphics::DrawPrimitive(PrimitiveType type, i32 startVertex, i32 primitiveCount)
{
    i32 vertexCount = 0;
    GLenum glMode = GL_TRIANGLES;

    if (type == PRIM_TRIANGLES)
    {
        vertexCount = primitiveCount * 3;
        glMode = GL_TRIANGLES;
    }
    else if (type == PRIM_TRIANGLE_STRIP)
    {
        vertexCount = primitiveCount + 2;
        glMode = GL_TRIANGLE_STRIP;
    }
    else if (type == PRIM_TRIANGLE_FAN)
    {
        vertexCount = primitiveCount + 2;
        glMode = GL_TRIANGLE_FAN;
    }

    glBindVertexArray(unitQuadVao);

    glUniform1i(u_ScreenSpace, false);
    glUniform1i(u_UseTexture, true);

    glUniform4f(u_Viewport, (f32)viewport.x, (f32)viewport.y, (f32)viewport.width,
                (f32)viewport.height);

    glUniformMatrix4fv(u_Model, 1, GL_FALSE, (GLfloat *)&transforms[MATRIX_MODEL]);
    glUniformMatrix4fv(u_View, 1, GL_FALSE, (GLfloat *)&transforms[MATRIX_VIEW]);
    glUniformMatrix4fv(u_Proj, 1, GL_FALSE, (GLfloat *)&transforms[MATRIX_PROJECTION]);
    glUniformMatrix4fv(u_TextureMatrix, 1, GL_FALSE, (GLfloat *)&transforms[MATRIX_TEXTURE]);

    glUniform1i(u_ColorOpRgb, colorOpRgb);
    glUniform1i(u_ColorOpAlpha, colorOpAlpha);
    glUniform1i(u_TexArg, texArg);

    glUniform4f(u_TextureFactor, textureFactor.bytes.r / 255.0f, textureFactor.bytes.g / 255.0f,
                textureFactor.bytes.b / 255.0f, textureFactor.bytes.a / 255.0f);

    glUniform1i(u_AlphaTest, alphaTestEnabled);
    glUniform1f(u_AlphaRef, alphaRef / 255.0f);

    glUniform1i(u_FogEnabled, fogEnabled);
    glUniform4f(u_FogColor, fogColor.bytes.r / 255.0f, fogColor.bytes.g / 255.0f,
                fogColor.bytes.b / 255.0f, fogColor.bytes.a / 255.0f);
    glUniform1f(u_FogNear, fogNear);
    glUniform1f(u_FogFar, fogFar);

    glDrawArrays(glMode, startVertex, vertexCount);
}

void GlesGraphics::DrawPrimitiveUP(PrimitiveType type, i32 primitiveCount, const void *vertexData,
                                   i32 vertexStride)
{
    i32 vertexCount = 0;
    GLenum glMode = GL_TRIANGLES;

    if (type == PRIM_TRIANGLES)
    {
        vertexCount = primitiveCount * 3;
        glMode = GL_TRIANGLES;
    }
    else if (type == PRIM_TRIANGLE_STRIP)
    {
        vertexCount = primitiveCount + 2;
        glMode = GL_TRIANGLE_STRIP;
    }
    else if (type == PRIM_TRIANGLE_FAN)
    {
        vertexCount = primitiveCount + 2;
        glMode = GL_TRIANGLE_FAN;
    }

    bool isScreenSpace = false;
    bool hasTex = false;
    switch (vertexStride)
    {
    case sizeof(VertexTex1DiffuseXyzrhw):
        isScreenSpace = true;
        hasTex = true;
        glBindVertexArray(vaos[0]);
        break;
    case sizeof(VertexTex1DiffuseXyz):
        isScreenSpace = false;
        hasTex = true;
        glBindVertexArray(vaos[1]);
        break;
    case sizeof(VertexDiffuseXyzrhw):
        isScreenSpace = true;
        hasTex = false;
        glBindVertexArray(vaos[2]);
        break;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertexCount * vertexStride, nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * vertexStride, vertexData);

    glUniform1i(u_ScreenSpace, isScreenSpace);
    glUniform1i(u_UseTexture, hasTex);

    glUniform4f(u_Viewport, (f32)viewport.x, (f32)viewport.y, (f32)viewport.width,
                (f32)viewport.height);

    glUniformMatrix4fv(u_Model, 1, GL_FALSE, (GLfloat *)&transforms[MATRIX_MODEL]);
    glUniformMatrix4fv(u_View, 1, GL_FALSE, (GLfloat *)&transforms[MATRIX_VIEW]);
    glUniformMatrix4fv(u_Proj, 1, GL_FALSE, (GLfloat *)&transforms[MATRIX_PROJECTION]);
    glUniformMatrix4fv(u_TextureMatrix, 1, GL_FALSE, (GLfloat *)&transforms[MATRIX_TEXTURE]);

    glUniform1i(u_ColorOpRgb, colorOpRgb);
    glUniform1i(u_ColorOpAlpha, colorOpAlpha);
    glUniform1i(u_TexArg, texArg);

    glUniform4f(u_TextureFactor, textureFactor.bytes.r / 255.0f, textureFactor.bytes.g / 255.0f,
                textureFactor.bytes.b / 255.0f, textureFactor.bytes.a / 255.0f);

    glUniform1i(u_AlphaTest, alphaTestEnabled);
    glUniform1f(u_AlphaRef, alphaRef / 255.0f);

    glUniform1i(u_FogEnabled, fogEnabled);
    glUniform4f(u_FogColor, fogColor.bytes.r / 255.0f, fogColor.bytes.g / 255.0f,
                fogColor.bytes.b / 255.0f, fogColor.bytes.a / 255.0f);
    glUniform1f(u_FogNear, fogNear);
    glUniform1f(u_FogFar, fogFar);

    glDrawArrays(glMode, 0, vertexCount);
}

void GlesGraphics::SwapBuffers()
{
    SDL_GL_SwapWindow(g_GameWindow.window);
}
