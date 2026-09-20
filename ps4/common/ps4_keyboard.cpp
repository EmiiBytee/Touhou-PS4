// USB keyboard support. SDL's PS4 port has no keyboard driver, so the console's own
// keyboard service is read directly and folded into the same button mask the pad produces:
// both work at once, and either can be used at any moment.
//
// The keyboard library isn't linked: a game process doesn't get it resolved at load time
// and the app dies with PRX_NOT_RESOLVED_FUNCTION before it draws anything. It's loaded by
// hand instead, and everything here turns into a no-op if that fails.
#include <orbis/Keyboard.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <cstdint>
#include <cstring>

void PS4_Log(const char *fmt, ...);

namespace
{
// Buttons, mirroring TH_BUTTON_* in the game's Controller.hpp.
enum
{
    kShoot = 1 << 0,
    kBomb = 1 << 1,
    kFocus = 1 << 2,
    kMenu = 1 << 3,
    kUp = 1 << 4,
    kDown = 1 << 5,
    kLeft = 1 << 6,
    kRight = 1 << 7,
    kSkip = 1 << 8,
    kQ = 1 << 9,
    kS = 1 << 10,
    kHome = 1 << 11,
    kEnter = 1 << 12,
};

// USB HID usage IDs, which is what the keyboard reports.
enum
{
    kKeyA = 4,
    kKeyQ = 20,
    kKeyS = 22,
    kKeyX = 27,
    kKeyZ = 29,
    kKeyReturn = 40,
    kKeyEscape = 41,
    kKeyRight = 79,
    kKeyLeft = 80,
    kKeyDown = 81,
    kKeyUp = 82,
    kKeyHome = 74,
    kKeyKp1 = 89,
    kKeyKp2 = 90,
    kKeyKp3 = 91,
    kKeyKp4 = 92,
    kKeyKp6 = 94,
    kKeyKp7 = 95,
    kKeyKp8 = 96,
    kKeyKp9 = 97,
};

struct KeyMapping
{
    uint16_t key;
    uint16_t buttons;
};

// Same layout the PC build uses, so muscle memory carries over.
constexpr KeyMapping kMap[] = {
    {kKeyUp, kUp},
    {kKeyDown, kDown},
    {kKeyLeft, kLeft},
    {kKeyRight, kRight},
    {kKeyKp8, kUp},
    {kKeyKp2, kDown},
    {kKeyKp4, kLeft},
    {kKeyKp6, kRight},
    {kKeyKp7, kUp | kLeft},
    {kKeyKp9, kUp | kRight},
    {kKeyKp1, kDown | kLeft},
    {kKeyKp3, kDown | kRight},
    {kKeyZ, kShoot},
    {kKeyX, kBomb},
    {kKeyEscape, kMenu},
    {kKeyReturn, kEnter},
    {kKeyHome, kHome},
    {kKeyQ, kQ},
    {kKeyS, kS},
};

int g_Handle = -1;

int32_t (*g_Init)();
int32_t (*g_Open)(int32_t userId, int32_t type, int32_t index, void *param);
int32_t (*g_ReadState)(int32_t handle, OrbisKeyboardData *data);

// Loads libSceKeyboard and looks up what we need. False if the console won't give it to us.
bool LoadKeyboardModule()
{
    int32_t handle = (int32_t)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_KEYBOARD);
    if (handle < 0)
    {
        PS4_Log("keyboard: sysmodule load failed %#x, trying the sprx directly", handle);
        handle = (int32_t)sceKernelLoadStartModule("/system/common/lib/libSceKeyboard.sprx", 0, nullptr, 0,
                                                   nullptr, nullptr);
    }
    if (handle < 0)
    {
        PS4_Log("keyboard: no keyboard library (%#x); pad only", handle);
        return false;
    }

    void *init = nullptr, *open = nullptr, *read = nullptr;
    sceKernelDlsym(handle, "sceKeyboardInit", &init);
    sceKernelDlsym(handle, "sceKeyboardOpen", &open);
    sceKernelDlsym(handle, "sceKeyboardReadState", &read);
    if (!init || !open || !read)
    {
        PS4_Log("keyboard: missing entry points (%p %p %p); pad only", init, open, read);
        return false;
    }
    g_Init = (int32_t (*)())init;
    g_Open = (int32_t (*)(int32_t, int32_t, int32_t, void *))open;
    g_ReadState = (int32_t (*)(int32_t, OrbisKeyboardData *))read;
    return true;
}
} // namespace

void PS4_KeyboardInit()
{
    if (!LoadKeyboardModule())
    {
        return;
    }
    int32_t rc = g_Init();
    if (rc < 0 && rc != 0x80A40007 /* already initialised */)
    {
        PS4_Log("keyboard: sceKeyboardInit failed %#x", rc);
        return;
    }
    int32_t userId = 0;
    rc = sceUserServiceGetInitialUser(&userId);
    if (rc < 0)
    {
        PS4_Log("keyboard: no initial user %#x", rc);
        return;
    }
    g_Handle = g_Open(userId, 0 /* standard keyboard */, 0, nullptr);
    if (g_Handle < 0)
    {
        PS4_Log("keyboard: sceKeyboardOpen failed %#x", g_Handle);
        g_Handle = -1;
        return;
    }
    PS4_Log("keyboard: ready (handle %d)", g_Handle);
}

// Returns the TH_BUTTON_* mask currently held on the keyboard, or 0 when there is none.
uint16_t PS4_KeyboardButtons()
{
    if (g_Handle < 0)
    {
        return 0;
    }
    OrbisKeyboardData data;
    std::memset(&data, 0, sizeof(data));
    if (g_ReadState(g_Handle, &data) < 0)
    {
        return 0;
    }

    uint16_t buttons = 0;
    // Shift is focus and control skips, both as modifiers so either side works.
    if (data.mods & (ORBIS_KEYBOARD_MOD_LEFT_SHIFT | ORBIS_KEYBOARD_MOD_RIGHT_SHIFT))
    {
        buttons |= kFocus;
    }
    if (data.mods & (ORBIS_KEYBOARD_MOD_LEFT_CTRL | ORBIS_KEYBOARD_MOD_RIGHT_CTRL))
    {
        buttons |= kSkip;
    }

    const int nkeys = data.nkeys > 32 ? 32 : data.nkeys;
    for (int i = 0; i < nkeys; i++)
    {
        const uint16_t key = data.keycodes[i];
        for (const KeyMapping &m : kMap)
        {
            if (m.key == key)
            {
                buttons |= m.buttons;
                break;
            }
        }
    }
    return buttons;
}
