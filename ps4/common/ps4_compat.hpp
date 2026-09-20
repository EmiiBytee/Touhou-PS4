// Force-included (-include) into every PS4 translation unit.
// Papers over the gap between the SDL2 2.0.18 / SDL2_ttf 2.0.15 that PacBrew ships and
// the newer SDL2 APIs the portable decomps use.
#pragma once

#ifdef __cplusplus

#include <SDL2/SDL_endian.h>
#include <SDL2/SDL_ttf.h>
#include <strings.h>

// Some OpenOrbis SDK header combinations define strings.h's include guard before its
// prototypes are emitted. miniaudio calls this POSIX function directly.
extern "C" int strcasecmp(const char *left, const char *right);

// Added in SDL 2.26. Every PS4 is x86_64, so floats follow the byte order.
#ifndef SDL_FLOATWORDORDER
#define SDL_FLOATWORDORDER SDL_BYTEORDER
#endif

#if !SDL_TTF_VERSION_ATLEAST(2, 0, 18)
// TTF_SetFontSize() only exists since SDL2_ttf 2.0.18. Emulate it by reopening the font
// at the requested size (cached), which is why the font is passed by reference here.
TTF_Font *PS4_TTF_OpenFont(const char *file, int ptsize);
int PS4_TTF_SetFontSize(TTF_Font *&font, int ptsize);
void PS4_TTF_CloseFont(TTF_Font *font);
#define TTF_OpenFont PS4_TTF_OpenFont
#define TTF_SetFontSize PS4_TTF_SetFontSize
#define TTF_CloseFont PS4_TTF_CloseFont
#endif

#endif
