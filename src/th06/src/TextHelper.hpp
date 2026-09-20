#pragma once

#include "AnmManager.hpp"
#include "ZunColor.hpp"
#include "ZunResult.hpp"
#include "inttypes.hpp"

// #include <d3d8.h>

// struct FormatInfo
//{
//     D3DFORMAT format;
//     i32 bitCount;
//     u32 alphaMask;
//     u32 redMask;
//     u32 greenMask;
//     u32 blueMask;
// };
struct TextHelper
{
    static ZunResult CreateTextBuffer();
    static void ReleaseTextBuffer();
    static void RenderTextToTexture(i32 xPos, i32 yPos, i32 spriteWidth, i32 spriteHeight, i32 fontHeight,
                                    i32 fontWidth, ZunColor textColor, ZunColor shadowColor, const char *string,
                                    TextureData *outTexture);
#ifdef TH_THPATCH
    // Lay out the next RenderTextToTexture() call by its real pixel width inside the visible
    // sprite width (thcrap's text_sprite_width / right_align / center_align binhacks).
    enum Align
    {
        ALIGN_LEFT,
        ALIGN_RIGHT,
        ALIGN_CENTER,
    };
    static void SetLayout(i32 visibleWidth, Align align);
    // Width in sprite pixels of the text drawn by the last RenderTextToTexture() call.
    static i32 LastTextWidth();
#endif

    TextHelper();
    ~TextHelper();
    //    bool AllocateBufferWithFallback(i32 width, i32 height, D3DFORMAT format);
    //    bool TryAllocateBuffer(i32 width, i32 height, D3DFORMAT format);
    //    FormatInfo *GetFormatInfo(D3DFORMAT format);
    //    bool ReleaseBuffer() const;
    static bool InvertAlpha(i32 x, i32 y, i32 spriteWidth, i32 fontHeight);
    //    bool CopyTextToSurface(IDirect3DSurface8 *outSurface);

    //    D3DFORMAT format;
    i32 width;
    i32 height;
    u32 imageSizeInBytes;
    i32 imageWidthInBytes;
    //    HDC hdc;
    //    HGDIOBJ gdiObj;
    //    HGDIOBJ gdiObj2;
    u8 *buffer;
};
