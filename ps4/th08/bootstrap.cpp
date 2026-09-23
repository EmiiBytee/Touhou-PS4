#include "ps4_videoout.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <orbis/SystemService.h>
#include <orbis/libkernel.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

void PS4_Log(const char *fmt, ...);
void PS4_Notify(const char *message);

namespace
{
constexpr const char *kWritableRoot = "/data/touhou/th08";
constexpr const char *kBundledRoot = "/app0/assets";

bool RegularFile(const std::string &path, std::uint64_t expectedSize)
{
    struct stat info = {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           static_cast<std::uint64_t>(info.st_size) == expectedSize;
}

bool DataReadyAt(const char *root)
{
    const std::string prefix(root);
    return RegularFile(prefix + "/th08.dat", 46838025ULL) &&
           RegularFile(prefix + "/thbgm.dat", 449961024ULL);
}

SDL_Surface *LoadIcon()
{
    SDL_Surface *loaded = IMG_Load("/app0/assets/icon0.png");
    if (loaded == nullptr)
    {
        PS4_Log("th08 bootstrap: icon load failed: %s", IMG_GetError());
        return nullptr;
    }
    SDL_Surface *argb = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(loaded);
    return argb;
}

void Present(SDL_Surface *surface)
{
    if (surface == nullptr)
    {
        return;
    }
    PS4_VideoOutBeginFrame(surface->w, surface->h, false);
    PS4_VideoOutScaleRows(static_cast<const std::uint32_t *>(surface->pixels), 0, 1);
    PS4_VideoOutFlip();
}
} // namespace

int main()
{
    PS4_Log("TH08 PS4 bootstrap 0.01 starting");
    if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
    {
        PS4_Log("th08 bootstrap: SDL_Init failed: %s", SDL_GetError());
    }
    const int imageFlags = IMG_INIT_PNG;
    if ((IMG_Init(imageFlags) & imageFlags) != imageFlags)
    {
        PS4_Log("th08 bootstrap: IMG_Init failed: %s", IMG_GetError());
    }

    const bool externalData = DataReadyAt(kWritableRoot);
    const bool bundledData = DataReadyAt(kBundledRoot);
    PS4_Log("th08 bootstrap: external DATs=%s bundled DATs=%s",
            externalData ? "ready" : "missing", bundledData ? "ready" : "missing");

    if (!PS4_VideoOutInit())
    {
        PS4_Notify("Touhou 8 bootstrap: VideoOut initialization failed");
        return 1;
    }

    SDL_Surface *icon = LoadIcon();
    for (int i = 0; i < 3; ++i)
    {
        Present(icon);
    }

    if (externalData || bundledData)
    {
        PS4_Notify("Touhou 8 PS4: game data found; native engine bring-up is next");
    }
    else
    {
        PS4_Notify("Touhou 8 PS4: copy th08.dat and thbgm.dat to /data/touhou/th08");
    }

    // Keep the native frame visible long enough to validate the package and screenshot it.
    // Options/Circle exits early; after thirty seconds the bootstrap closes on its own.
    const std::uint64_t deadline = sceKernelGetProcessTime() + 30000000ULL;
    bool running = true;
    while (running && sceKernelGetProcessTime() < deadline)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_CONTROLLERBUTTONDOWN &&
                 (event.cbutton.button == SDL_CONTROLLER_BUTTON_START ||
                  event.cbutton.button == SDL_CONTROLLER_BUTTON_B)))
            {
                running = false;
            }
        }
        sceKernelUsleep(16000);
    }

    if (icon != nullptr)
    {
        SDL_FreeSurface(icon);
    }
    IMG_Quit();
    SDL_Quit();
    PS4_Log("TH08 PS4 bootstrap exiting");
    return 0;
}
