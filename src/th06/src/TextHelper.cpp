#include "TextHelper.hpp"
#include "GameErrorContext.hpp"
#include "GameWindow.hpp"
#include "Supervisor.hpp"
#include "i18n.hpp"

#include "thirdparty/sjis_converter.h"

#include <SDL2/SDL_ttf.h>
#include <algorithm>
#include <cstring>

static TTF_Font *g_Font;
#ifdef TH_THPATCH
// Latin text (translations) looks cramped in MS Gothic's half-width glyphs; thcrap's
// script_latin switches to Arial. Staged by stage_thcrap.py as patch/latin.ttf.
static TTF_Font *g_LatinFont;

// Highest ascender and lowest descender (relative to the baseline) of typical Latin glyphs.
static void LatinGlyphExtent(TTF_Font *font, i32 *top, i32 *bottom)
{
    *top = 0;
    *bottom = 0;
    for (const char *c = "bdfhklt|(HgjpqyQ"; *c != '\0'; c++)
    {
        int minx, maxx, miny, maxy, advance;
        if (TTF_GlyphMetrics(font, (Uint16)*c, &minx, &maxx, &miny, &maxy, &advance) == 0)
        {
            *top = std::max(*top, maxy);
            *bottom = std::min(*bottom, miny);
        }
    }
}

// Drops the first `rows` pixel rows of a rendered text surface.
static SDL_Surface *CropTop(SDL_Surface *text, i32 rows)
{
    if (text == NULL || rows <= 0 || rows >= text->h)
    {
        return text;
    }
    SDL_Surface *cropped = SDL_CreateRGBSurfaceWithFormat(0, text->w, text->h - rows, 32, text->format->format);
    if (cropped == NULL)
    {
        return text;
    }
    SDL_LockSurface(text);
    SDL_LockSurface(cropped);
    for (i32 y = 0; y < cropped->h; y++)
    {
        std::memcpy((u8 *)cropped->pixels + y * cropped->pitch, (u8 *)text->pixels + (y + rows) * text->pitch,
                    text->w * 4);
    }
    SDL_UnlockSurface(cropped);
    SDL_UnlockSurface(text);
    SDL_FreeSurface(text);
    return cropped;
}

static bool IsLatinText(const char *utf8)
{
    for (const u8 *p = (const u8 *)utf8; *p != 0;)
    {
        u32 cp = *p;
        i32 extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        cp &= extra == 0 ? 0x7F : (0x3F >> extra);
        for (p++; extra > 0 && (*p & 0xC0) == 0x80; extra--, p++)
        {
            cp = (cp << 6) | (*p & 0x3F);
        }
        // CJK and fullwidth forms need the Japanese font.
        if (cp >= 0x2E80)
        {
            return false;
        }
    }
    return true;
}
#endif

TextHelper::TextHelper()
{
    //    this->format = (D3DFORMAT)-1;
    //    this->width = 0;
    //    this->height = 0;
    //    this->hdc = 0;
    //    this->gdiObj2 = 0;
    //    this->gdiObj = 0;
    //    this->buffer = NULL;
}

TextHelper::~TextHelper()
{
    TTF_Quit();
}

#define TEXT_BUFFER_HEIGHT 64

// Extended to initialize all globals for text helper
ZunResult TextHelper::CreateTextBuffer()
{
    TTF_Init();

    // Primary font is MSゴシック, which is nonfree and has to be taken from a Windows install
    // Fallback is Noto Sans Regular (JP) which is redistributable
    if ((g_Font = TTF_OpenFont(TH_PRIMARY_FONT_FILENAME, 10), g_Font == NULL) &&
        (std::printf("%s\n", TTF_GetError()), g_Font = TTF_OpenFont(TH_FALLBACK_FONT_FILENAME, 10), g_Font == NULL))
    {
        std::printf("%s\n", TTF_GetError());

        g_GameErrorContext.Fatal(TH_ERR_FONTS_NOT_FOUND);
        return ZUN_ERROR;
    }
#ifdef TH_THPATCH
    g_LatinFont = THPatch_Enabled() ? TTF_OpenFont("patch/latin.ttf", 10) : NULL;
#endif

    g_TextBufferSurface =
#ifdef TH_THPATCH
        // Patched text.anm uses 512px-wide text textures, rendered here at 2x.
        SDL_CreateRGBSurfaceWithFormat(0, 1024, TEXT_BUFFER_HEIGHT, 32, SDL_PIXELFORMAT_RGBA32);
#else
        SDL_CreateRGBSurfaceWithFormat(0, GAME_WINDOW_WIDTH, TEXT_BUFFER_HEIGHT, 32, SDL_PIXELFORMAT_RGBA32);
#endif

    SDL_SetSurfaceBlendMode(g_TextBufferSurface, SDL_BLENDMODE_NONE);

    return ZUN_SUCCESS;
}

bool TextHelper::InvertAlpha(i32 x, i32 y, i32 spriteWidth, i32 fontHeight)
{
    u8 *bufferCursor;
    i32 gradientArea;
    i32 i = 0;

    gradientArea = spriteWidth * fontHeight;

    SDL_LockSurface(g_TextBufferSurface);

    // In D3D EoSD this function mostly inverts the alpha, but on A1R5G5B5 surfaces specifically it also
    //   creates a gradient. D3D EoSD will always attempt to create an A1R5G5B5 surface for the text buffer,
    //   will only attempt use other formats as a fallback, and in those cases the text will be bugged anyway.
    //   As part of the port from GDI to SDL_ttf, we've converted the text buffer surface to always be RGBA32
    //   and no longer need the alpha inversion, but we still want that gradient to be applied

    for (bufferCursor = (u8 *)g_TextBufferSurface->pixels; i < gradientArea; i++, bufferCursor += 4)
    {
        if (bufferCursor[3]) // A
        {
            bufferCursor[0] = bufferCursor[0] - bufferCursor[0] * i / gradientArea / 2; // R
            bufferCursor[1] = bufferCursor[1] - bufferCursor[1] * i / gradientArea / 2; // G
            bufferCursor[2] = bufferCursor[2] - bufferCursor[2] * i / gradientArea / 4; // B
        }
    }

    SDL_UnlockSurface(g_TextBufferSurface);

    return true;
}

// Text strings in asset files are encoded using Shift_JIS. This allows RenderTextToTexture to handle both UTF-8 and
// Shift_JIS. This also does not check for overlong encoding, but that shouldn't matter
bool isUTF8Encoded(const char *string)
{
#define UTF8_1BYTE_MASK 0x80
#define UTF8_2BYTE_MASK 0xE0
#define UTF8_3BYTE_MASK 0xF0
#define UTF8_4BYTE_MASK 0xF8

#define UTF8_2NDBYTE_MASK 0xC0

// 0xxx xxxx
#define UTF8_1BYTE_PREFIX 0x00
// 110x xxxx
#define UTF8_2BYTE_PREFIX 0xC0
// 1110 xxxx
#define UTF8_3BYTE_PREFIX 0xE0
// 1111 0xxx
#define UTF8_4BYTE_PREFIX 0xF0

// 10xx xxxx
#define UTF8_2NDBYTE_PREFIX 0x80

    bool isMultiByteParse = false;
    int codepointLen = 0;

    while (*string != '\0')
    {
        unsigned char c = *(unsigned char *)string;

        if (!isMultiByteParse)
        {
            if ((c & UTF8_1BYTE_MASK) != UTF8_1BYTE_PREFIX)
            {
                isMultiByteParse = true;

                if ((c & UTF8_2BYTE_MASK) == UTF8_2BYTE_PREFIX)
                    codepointLen = 1;
                else if ((c & UTF8_3BYTE_MASK) == UTF8_3BYTE_PREFIX)
                    codepointLen = 2;
                else if ((c & UTF8_4BYTE_MASK) == UTF8_4BYTE_PREFIX)
                    codepointLen = 3;
                else
                    return false;
            }
        }
        else
        {
            if ((c & UTF8_2NDBYTE_MASK) != UTF8_2NDBYTE_PREFIX)
                return false;

            if (--codepointLen == 0)
                isMultiByteParse = false;
        }

        string++;
    }

    return true;

#undef UTF8_1BYTE_MASK
#undef UTF8_2BYTE_MASK
#undef UTF8_3BYTE_MASK
#undef UTF8_4BYTE_MASK

#undef UTF8_2NDBYTE_MASK

#undef UTF8_1BYTE_PREFIX
#undef UTF8_2BYTE_PREFIX
#undef UTF8_3BYTE_PREFIX
#undef UTF8_4BYTE_PREFIX

#undef UTF8_2NDBYTE_PREFIX
}

void SurfaceOverwriteBlend(SDL_Surface *srcSurface, SDL_Surface *dstSurface, u32 x)
{
    // Source surface is A8R8G8B8
    // Dest surface is RGBA32
    // We want to overwrite dest unless source has alpha 0

    SDL_LockSurface(srcSurface);
    SDL_LockSurface(dstSurface);

    u32 *srcData = (u32 *)srcSurface->pixels;
    u8 *dstData = (u8 *)dstSurface->pixels;

    for (int i = 0; i < srcSurface->h; i++)
    {
        for (int j = 0; j < srcSurface->w; j++)
        {
            if ((srcData[j] & 0xFF00'0000) != 0)
            {
                dstData[i * dstSurface->pitch + (x + j) * 4] = (srcData[j] >> 16) & 0xFF;
                dstData[i * dstSurface->pitch + (x + j) * 4 + 1] = (srcData[j] >> 8) & 0xFF;
                dstData[i * dstSurface->pitch + (x + j) * 4 + 2] = srcData[j] & 0xFF;
                dstData[i * dstSurface->pitch + (x + j) * 4 + 3] = (srcData[j] >> 24) & 0xFF;
            }
        }

        srcData += srcSurface->pitch / 4;
    }

    SDL_UnlockSurface(dstSurface);
    SDL_UnlockSurface(srcSurface);
}

#ifdef TH_THPATCH
static i32 g_LayoutWidth;
static TextHelper::Align g_LayoutAlign;
static i32 g_LastTextWidth;

i32 TextHelper::LastTextWidth()
{
    return g_LastTextWidth;
}

void TextHelper::SetLayout(i32 visibleWidth, Align align)
{
    g_LayoutWidth = visibleWidth;
    g_LayoutAlign = align;
}

// Buffer x (at 2x) where a text surface of width textW starts under the current layout.
static i32 LayoutX(i32 xPos, i32 areaW, i32 textW)
{
    switch (g_LayoutAlign)
    {
    case TextHelper::ALIGN_RIGHT:
        return xPos * 2 + areaW - textW;
    case TextHelper::ALIGN_CENTER:
        return xPos * 2 + (areaW - textW) / 2;
    default:
        return xPos * 2;
    }
}

// Translated lines are often wider than the sprite the original text was laid out for;
// squeeze them horizontally to fit instead of cutting them off (thcrap's text_scale_x).
static SDL_Surface *FitTextWidth(SDL_Surface *text, i32 maxWidth)
{
    if (text == NULL || maxWidth <= 0 || text->w <= maxWidth)
    {
        return text;
    }
    SDL_Surface *fitted = SDL_CreateRGBSurfaceWithFormat(0, maxWidth, text->h, 32, text->format->format);
    if (fitted == NULL)
    {
        return text;
    }
    if (SDL_SoftStretchLinear(text, NULL, fitted, NULL) < 0)
    {
        SDL_FreeSurface(fitted);
        return text;
    }
    SDL_FreeSurface(text);
    return fitted;
}
#endif

void TextHelper::RenderTextToTexture(i32 xPos, i32 yPos, i32 spriteWidth, i32 spriteHeight, i32 fontHeight,
                                     i32 fontWidth, ZunColor textColor, ZunColor shadowColor, const char *string,
                                     TextureData *outTexture)
{
    char convertedText[1024];
    SDL_Rect finalCopyDst;
    SDL_Rect finalCopySrc;
    SDL_Rect shadowRect;
    SDL_Rect textRect;

    if (!isUTF8Encoded(string))
    {
        char *utf8 = sjis2utf8(string);
        strcpy(convertedText, utf8);
        free(utf8);
    }
    else
    {
        strcpy(convertedText, string);
    }

#ifdef TH_THPATCH
    TTF_Font *&font = (g_LatinFont != NULL && IsLatinText(convertedText)) ? g_LatinFont : g_Font;
#else
    TTF_Font *&font = g_Font;
#endif
    TTF_SetFontSize(font, fontHeight * 2);
#ifdef TH_THPATCH
    // The text rows are fontHeight*2-2 pixels tall (at 2x). MS Gothic's glyphs fill its
    // point size, but Latin fonts reserve extra room above (accents) and would come out
    // tiny or clipped. Size them so ascenders..descenders exactly fill the row, and crop the
    // unused space above the tallest ascender.
    i32 cropTop = 0;
    if (font == g_LatinFont)
    {
        i32 rowHeight = fontHeight * 2 - 2;
        i32 top, bottom;
        LatinGlyphExtent(font, &top, &bottom);
        if (top - bottom > 0)
        {
            TTF_SetFontSize(font, std::max(1, fontHeight * 2 * rowHeight / (top - bottom)));
            LatinGlyphExtent(font, &top, &bottom);
        }
        cropTop = std::max(0, TTF_FontAscent(font) - top);
    }
#endif

    finalCopySrc.x = 0;
    finalCopySrc.y = 0;
    finalCopySrc.w = spriteWidth * 2 - 2;
    finalCopySrc.h = fontHeight * 2 - 2;

    SDL_FillRect(g_TextBufferSurface, &finalCopySrc, 0);

    if (shadowColor != COLOR_WHITE)
    {
        SDL_Surface *shadowText;

        // Render shadow.
        SDL_Color sdlShadowColor;
        sdlShadowColor.a = 0xFF;
        sdlShadowColor.b = (shadowColor >> 16) & 0xFF;
        sdlShadowColor.g = (shadowColor >> 8) & 0xFF;
        sdlShadowColor.r = shadowColor & 0xFF;

        shadowText = TTF_RenderUTF8_Blended(font, convertedText, sdlShadowColor);
#ifdef TH_THPATCH
        i32 areaW = (g_LayoutWidth > 0 ? g_LayoutWidth : spriteWidth - xPos) * 2 - 5;
        shadowText = FitTextWidth(CropTop(shadowText, cropTop), areaW);
#endif

        if (shadowText != NULL)
        {
#ifdef TH_THPATCH
            shadowRect.x = LayoutX(xPos, areaW, shadowText->w) + 3;
#else
            shadowRect.x = xPos * 2 + 3;
#endif
            shadowRect.y = 2;
            shadowRect.w = shadowText->w;
            shadowRect.h = shadowText->h;

            SDL_SetSurfaceBlendMode(shadowText, SDL_BLENDMODE_NONE);
            SDL_BlitSurface(shadowText, NULL, g_TextBufferSurface, &shadowRect);

            SDL_FreeSurface(shadowText);
        }
    }

    SDL_Color sdlTextColor;
    sdlTextColor.a = 0xFF;
    sdlTextColor.b = (textColor >> 16) & 0xFF;
    sdlTextColor.g = (textColor >> 8) & 0xFF;
    sdlTextColor.r = textColor & 0xFF;

    SDL_Surface *regularText = TTF_RenderUTF8_Blended(font, convertedText, sdlTextColor);
#ifdef TH_THPATCH
    i32 textAreaW = (g_LayoutWidth > 0 ? g_LayoutWidth : spriteWidth - xPos) * 2 - 5;
    regularText = FitTextWidth(CropTop(regularText, cropTop), textAreaW);
    g_LastTextWidth = regularText != NULL ? regularText->w / 2 : 0;
#endif

    if (regularText != NULL)
    {
        textRect.x = xPos * 2;
        textRect.y = 0;
        textRect.w = regularText->w;
        textRect.h = regularText->h;

#ifdef TH_THPATCH
        SurfaceOverwriteBlend(regularText, g_TextBufferSurface, LayoutX(xPos, textAreaW, regularText->w));
#else
        SurfaceOverwriteBlend(regularText, g_TextBufferSurface, xPos * 2);
#endif

        SDL_FreeSurface(regularText);
    }

    // Once we get an API abstraction layer for surface operations, this needs to change
    //   We really shouldn't be clobbering the texture format
    if (!outTexture->textureData || outTexture->format != TEX_FMT_A8R8G8B8)
    {
        free(outTexture->textureData);
        outTexture->textureData = (u8 *)malloc(outTexture->width * outTexture->height * 4);
        memset(outTexture->textureData, 0, outTexture->width * outTexture->height * 4);
    }

    outTexture->format = TEX_FMT_A8R8G8B8;
    SDL_Surface *textureSurface = SDL_CreateRGBSurfaceWithFormatFrom(
        outTexture->textureData, outTexture->width, outTexture->height, SDL_BITSPERPIXEL(SDL_PIXELFORMAT_RGBA32),
        outTexture->width * SDL_BYTESPERPIXEL(SDL_PIXELFORMAT_RGBA32), SDL_PIXELFORMAT_RGBA32);

    InvertAlpha(0, 0, spriteWidth * 2, fontHeight * 2 + 6);

    finalCopyDst.x = 0;
    finalCopyDst.y = yPos;
    finalCopyDst.w = spriteWidth;
    finalCopyDst.h = 16;

    if (SDL_SoftStretchLinear(g_TextBufferSurface, &finalCopySrc, textureSurface, &finalCopyDst) < 0)
    {
        SDL_Log("SDL_BlitScaled failed! Error: %s", SDL_GetError());
    }

    g_AnmManager->SetCurrentTexture(outTexture->handle);

    g_GfxBackend->SetTextureImage(outTexture->width, outTexture->height, PIXEL_RGBA, PIXEL_UNSIGNED_BYTE,
                                  outTexture->textureData);

    SDL_FreeSurface(textureSurface);

    return;
}

// Extended to free all globals for text helper
void TextHelper::ReleaseTextBuffer()
{
#ifdef TH_THPATCH
    if (g_LatinFont != NULL)
    {
        TTF_CloseFont(g_LatinFont);
        g_LatinFont = NULL;
    }
#endif
    if (g_Font != NULL)
    {
        TTF_CloseFont(g_Font);
        g_Font = NULL;
    }

    if (g_TextBufferSurface != NULL)
    {
        SDL_FreeSurface(g_TextBufferSurface);
        g_TextBufferSurface = NULL;
    }

    return;
}
