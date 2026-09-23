// thcrap-lite glue for TH08: the parts of a translation patch that have to reach the game's
// own data structures rather than whole files.

#include "th_pch.h"

#include "AnmManager.hpp"
#include "Supervisor.hpp"
#include "thpatch.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <math.h>
#include <stdlib.h>

void PS4_Log(const char *fmt, ...);

namespace th08
{
namespace modern
{
namespace
{
struct SpriteRect
{
    i32 x, y, w, h;
};

SpriteRect ScaleRect(const AnmRawSprite *sprite, f32 scaleX, f32 scaleY, i32 limitW, i32 limitH)
{
    SpriteRect rect;
    rect.x = (i32)floorf(sprite->x * scaleX);
    rect.y = (i32)floorf(sprite->y * scaleY);
    rect.w = (i32)ceilf(sprite->width * scaleX);
    rect.h = (i32)ceilf(sprite->height * scaleY);
    if (rect.x < 0) rect.x = 0;
    if (rect.y < 0) rect.y = 0;
    if (rect.x + rect.w > limitW) rect.w = limitW - rect.x;
    if (rect.y + rect.h > limitH) rect.h = limitH - rect.y;
    return rect;
}

bool HasVisiblePixels(const SDL_Surface *image, const SpriteRect &rect)
{
    const u8 *pixels = static_cast<const u8 *>(image->pixels);
    for (i32 y = rect.y; y < rect.y + rect.h; y++)
    {
        const u8 *row = pixels + (size_t)y * image->pitch + (size_t)rect.x * 4;
        for (i32 x = 0; x < rect.w; x++)
        {
            if (row[x * 4 + 3] != 0) return true;
        }
    }
    return false;
}
} // namespace

// thcrap patches ANM sheets sprite by sprite, and the PS4 port does the same (TH07 settled
// this the hard way): a sprite left completely transparent in the patch keeps the game's
// own pixels, and a sprite with any visible pixel replaces the game's whole rectangle.
// Replacing the whole sheet would drop everything the patch leaves out on purpose, and
// blending it over the original would leave the Japanese showing through.
//
// The copy goes through D3DXLoadSurfaceFromSurface, which converts to the texture's own
// format (most of IN's sheets are A4R4G4B4) and marks it for upload, exactly like the game's
// own writes into its textures.
void ApplyThpatchSprites(IDirect3DTexture8 *texture, const AnmRawEntry *entry, const char *name)
{
    if (texture == NULL || entry == NULL || name == NULL || name[0] == '@') return;

    u32 size = 0;
    u8 *data = THPatch_LoadReplacement(name, &size);
    if (data == NULL) return;

    SDL_RWops *stream = SDL_RWFromConstMem(data, (int)size);
    SDL_Surface *loaded = stream != NULL ? IMG_Load_RW(stream, 1) : NULL;
    SDL_Surface *image = loaded != NULL ? SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0) : NULL;
    if (loaded != NULL) SDL_FreeSurface(loaded);
    free(data);
    if (image == NULL)
    {
        PS4_Log("thpatch: %s could not be decoded", name);
        return;
    }

    IDirect3DSurface8 *target = NULL;
    IDirect3DSurface8 *overlay = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT locked;
    i32 replaced = 0;

    if (texture->GetSurfaceLevel(0, &target) != D3D_OK || target->GetDesc(&desc) != D3D_OK ||
        g_Supervisor.d3dDevice->CreateImageSurface(image->w, image->h, D3DFMT_A8R8G8B8, &overlay) != D3D_OK ||
        overlay->LockRect(&locked, NULL, 0) != D3D_OK)
    {
        PS4_Log("thpatch: %s could not be applied", name);
        goto done;
    }

    // RGBA bytes into A8R8G8B8, which is B G R A in memory.
    for (i32 y = 0; y < image->h; y++)
    {
        const u8 *src = static_cast<const u8 *>(image->pixels) + (size_t)y * image->pitch;
        u8 *dst = static_cast<u8 *>(locked.pBits) + (size_t)y * locked.Pitch;
        for (i32 x = 0; x < image->w; x++, src += 4, dst += 4)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    }
    overlay->UnlockRect();

    {
        // thcrap lays the patch image over the texture's own pixels one to one, from the top
        // left corner, whatever its size: English sheets are often smaller than the originals
        // (IN's stg5txt is 512x128 over a 512x256 texture) and simply leave the rest alone.
        // Scaling the image to the texture instead stretched half of each title over the whole
        // sprite. So a sprite's rectangle is the same on both sides, cut to what the patch covers.
        const f32 entryW = entry->width > 0 ? (f32)entry->width : (f32)desc.Width;
        const f32 entryH = entry->height > 0 ? (f32)entry->height : (f32)desc.Height;
        const i32 coverW = image->w < (i32)desc.Width ? image->w : (i32)desc.Width;
        const i32 coverH = image->h < (i32)desc.Height ? image->h : (i32)desc.Height;
        const u32 *offsets = reinterpret_cast<const u32 *>(reinterpret_cast<const u8 *>(entry) + sizeof(AnmRawEntry));
        for (i32 i = 0; i < entry->numSprites; i++)
        {
            const AnmRawSprite *sprite =
                reinterpret_cast<const AnmRawSprite *>(reinterpret_cast<const u8 *>(entry) + offsets[i]);
            const SpriteRect rect = ScaleRect(sprite, desc.Width / entryW, desc.Height / entryH, coverW, coverH);
            if (rect.w <= 0 || rect.h <= 0) continue;
            if (!HasVisiblePixels(image, rect)) continue;

            RECT srcRect = {rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
            RECT dstRect = srcRect;
            if (D3DXLoadSurfaceFromSurface(target, NULL, &dstRect, overlay, NULL, &srcRect, D3DX_FILTER_NONE, 0) ==
                D3D_OK)
            {
                replaced++;
            }
        }
    }
    PS4_Log("thpatch: %s: %d of %d sprites replaced", name, (int)replaced, (int)entry->numSprites);

done:
    if (overlay != NULL) overlay->Release();
    if (target != NULL) target->Release();
    SDL_FreeSurface(image);
}
} // namespace modern
} // namespace th08
