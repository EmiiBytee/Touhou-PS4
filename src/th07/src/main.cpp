#include <SDL2/SDL.h>
#include <cstdio>

// pull in gameerrorcontext::flush before anmmanager::releasesurfaces
#include "AnmManager.hpp"
#include "Chain.hpp"
#include "Controller.hpp"
#include "FileSystem.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "GameWindow.hpp"
#include "ResultScreen.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"
#include "ZunResult.hpp"
#include "dxutil.hpp"

void AnmManager::TakeScreenshotIfRequested()
{
    if (this->screenshotTextureId >= 0)
    {
        TakeScreenshot(this->screenshotTextureId, this->screenshotSrcLeft, this->screenshotSrcTop,
                       this->screenshotSrcWidth, this->screenshotSrcHeight, this->screenshotDstLeft,
                       this->screenshotDstTop, this->screenshotDstWidth, this->screenshotDstHeight);
        this->screenshotTextureId = -1;
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    i32 res;

    res = RENDER_RESULT_KEEP_RUNNING;

    if (g_Supervisor.LoadConfig("th07.cfg") != ZUN_SUCCESS)
    {
        goto stop;
    }

    GameWindow::ChecksumExecutable();
    g_GameWindow.frequency = SDL_GetPerformanceFrequency();

start:
    if (GameWindow::CreateGameWindow())
    {
        goto stop;
    }

    if (GameWindow::InitInterface())
    {
        goto stop;
    }

    if (GameWindow::InitRendering())
    {
        goto stop;
    }

    g_SoundPlayer.InitializeSound();
    Controller::ResetKeyboard();
    g_AnmManager = new AnmManager();
    if (!g_Supervisor.cfg.windowed)
    {
        SDL_ShowCursor(SDL_DISABLE);
    }
    res = g_Supervisor.RegisterChain();
    if (res != ZUN_SUCCESS)
    {
        if (res == ZUN_ERROR)
        {
            goto cleanup;
        }
        res = RENDER_RESULT_EXIT_ERROR;
        goto cleanup;
    }
    res = RENDER_RESULT_KEEP_RUNNING;
    g_GameWindow.curFrame = -30;
    while (!g_GameWindow.isAppClosing)
    {
        SDL_Event e;

        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
            case SDL_WINDOWEVENT:
                switch (e.window.event)
                {
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    g_GameWindow.isAppActive = 1;
                    g_GameWindow.isAppInactive = 0;
                    if (!g_Supervisor.cfg.windowed)
                    {
                        SDL_ShowCursor(SDL_DISABLE);
                    }
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    g_GameWindow.isAppActive = 0;
                    g_GameWindow.isAppInactive = 1;
                    SDL_ShowCursor(SDL_ENABLE);
                    break;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!g_Supervisor.controller)
                {
                    g_Supervisor.controller = SDL_GameControllerOpen(e.cdevice.which);
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (g_Supervisor.controller)
                {
                    SDL_Joystick *joy = SDL_GameControllerGetJoystick(g_Supervisor.controller);

                    if (SDL_JoystickInstanceID(joy) == e.cdevice.which)
                    {
                        SDL_GameControllerClose(g_Supervisor.controller);
                        g_Supervisor.controller = nullptr;
                    }
                }
                break;
            case SDL_QUIT:
                g_GameWindow.isAppClosing = true;
                break;
            }
        }

        res = g_GameWindow.Render();
        if (res != RENDER_RESULT_KEEP_RUNNING)
        {
            break;
        }
        g_Supervisor.flags = g_Supervisor.flags & 0xffffffef;
    }

cleanup:
    if (g_GameManager.plst.base.magic != 0)
    {
        ResultScreen::RegisterChain(2);
    }
    g_Chain.Release();
    while (g_SoundPlayer.ProcessQueues())
        ;

stop:
    g_SoundPlayer.Release();
    delete g_AnmManager;
    g_AnmManager = NULL;

    SAFE_DELETE(g_Supervisor.gfxDevice);
    if (g_GameWindow.window)
    {
        SDL_DestroyWindow(g_GameWindow.window);
        g_GameWindow.window = NULL;
    }
    SDL_ShowCursor(SDL_ENABLE);
    if (res == RENDER_RESULT_EXIT_ERROR)
    {
        g_GameErrorContext.m_BufferEnd = g_GameErrorContext.m_Buffer;
        *g_GameErrorContext.m_BufferEnd = '\0';
        g_GameErrorContext.Log("再起動を要するオプションが変更されたので再起動します\n");
        goto start;
    }
    FileSystem::WriteDataToFile("th07.cfg", &g_Supervisor.cfg, sizeof(GameConfiguration));
    g_GameErrorContext.Flush();
    return 0;
}
