void PS4_KeyboardInit();
// PS4-specific platform glue shared by the th06/th07 ports.
//
// Everything here runs from a static constructor, i.e. before the game's main():
//  - moves the working directory to the game data folder (the games use relative paths),
//  - redirects stdout/stderr to a log file there,
//  - points SDL at the Piglet + shader compiler modules the user provides,
//  - registers a GameController mapping for the DualShock 4.

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <orbis/libkernel.h>

#include "thpatch/thpatch.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <pthread.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TH_PS4_GAME_DIR
#error "TH_PS4_GAME_DIR must be defined by the build"
#endif

// Retail firmware strips the GLSL compiler, so libSceShaccVSH.sprx (and the matching
// libScePigletv2VSH.sprx) have to be supplied by the user: either in /data, or bundled
// into a personal pkg (TH_BUNDLE_ASSETS). We never ship them.
#define TH_PS4_MODULES_DIR "/data/touhou/modules"
#define TH_PS4_BUNDLED_MODULES_DIR "/app0/modules"
// Where other Piglet homebrew (sm64 port, OpenTyrian, ...) conventionally expects them.
#define TH_PS4_SHARED_MODULES_DIR "/data/self/system/common/lib"

// Read-only game data bundled into the pkg (TH_BUNDLE_ASSETS builds). Writable files
// (config, score, replays, log) always live in TH_PS4_GAME_DIR.
#define TH_PS4_BUNDLED_ASSETS_DIR "/app0/assets"

// PacBrew's SDL joystick driver builds the GUID from the first 16 bytes of the device
// name ("Sony DualShock 4 V2") and exposes: 0 Cross, 1 Circle, 2 Square, 3 Triangle,
// 4 Options, 6 Touchpad, 7 L3, 8 R3, 9 L1, 10 R1, 11-14 D-pad, axes 0-3 sticks, 4-5 L2/R2.
static const char kDualShock4Mapping[] =
    "536f6e79204475616c53686f636b2034,PS4 Controller,"
    "a:b0,b:b1,x:b2,y:b3,start:b4,back:b6,leftstick:b7,rightstick:b8,"
    "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";

void PS4_Notify(const char *message)
{
    OrbisNotificationRequest req;
    std::memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    std::snprintf(req.message, sizeof(req.message), "%s", message);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static bool FileExists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool HasPigletModules(const char *dir)
{
    std::string d(dir);
    return FileExists((d + "/libSceShaccVSH.sprx").c_str()) && FileExists((d + "/libScePigletv2VSH.sprx").c_str());
}

extern "C" FILE *__real_fopen(const char *path, const char *mode);
extern "C" int __real_mkdir(const char *path, mode_t mode);

static FILE *g_PS4Log;

// Log lines are written by a thread of their own. Each line costs three kernel calls (the
// kernel log is relayed over the network while klog is connected) plus a flush to the disk,
// and doing that on the game thread showed up as frame drops whenever the ports logged
// anything mid-game. The writer wakes for every line, so what is lost to a crash is at most
// the line being logged at that moment; the kernel's own crash dump goes to klog regardless.
static pthread_mutex_t g_LogLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_LogWake = PTHREAD_COND_INITIALIZER;
static std::string g_LogPending;
static int g_LogWriter; // 0 not started, 1 running, -1 could not start: write inline

static void WriteLogLines(const std::string &lines)
{
    size_t start = 0;
    while (start < lines.size())
    {
        size_t end = lines.find('\n', start);
        if (end == std::string::npos) end = lines.size();
        const std::string line = lines.substr(start, end - start);
        sceKernelDebugOutText(0, "[th-ps4] ");
        sceKernelDebugOutText(0, line.c_str());
        sceKernelDebugOutText(0, "\n");
        start = end + 1;
    }
    if (g_PS4Log != NULL)
    {
        std::fwrite(lines.data(), 1, lines.size(), g_PS4Log);
        std::fflush(g_PS4Log);
    }
}

static void *LogWriterMain(void *)
{
    std::string batch;
    for (;;)
    {
        pthread_mutex_lock(&g_LogLock);
        while (g_LogPending.empty())
        {
            pthread_cond_wait(&g_LogWake, &g_LogLock);
        }
        batch.swap(g_LogPending);
        pthread_mutex_unlock(&g_LogLock);
        WriteLogLines(batch);
        batch.clear();
    }
    return NULL;
}

// Goes to the kernel log (GoldHEN klog, port 3232) and to ps4_log.txt in the game dir.
void PS4_Log(const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    pthread_mutex_lock(&g_LogLock);
    if (g_LogWriter == 0)
    {
        pthread_t thread;
        g_LogWriter = pthread_create(&thread, NULL, LogWriterMain, NULL) == 0 ? 1 : -1;
        if (g_LogWriter == 1) pthread_detach(thread);
    }
    if (g_LogWriter == 1)
    {
        g_LogPending += buf;
        g_LogPending += '\n';
        pthread_cond_signal(&g_LogWake);
        pthread_mutex_unlock(&g_LogLock);
        return;
    }
    pthread_mutex_unlock(&g_LogLock);
    WriteLogLines(std::string(buf) + "\n");
}

static bool IsRelative(const char *path)
{
    return path != NULL && path[0] != '/' && path[0] != '\0';
}

// chdir() does not take effect in the PS4 sandbox, but the games do step into their own
// subdirectories with it (TH08 uses replay/ and backup/), so a virtual working directory
// below the game directory reproduces that and every relative path resolves against it.
static std::string g_Cwd = TH_PS4_GAME_DIR;

// Joins base and path, resolving "." and ".." and normalizing the backslashes the decomp
// still carries from the Windows original.
static std::string Rebase(const char *base, const char *path)
{
    std::string joined = base;
    if (IsRelative(path))
    {
        joined += '/';
        for (const char *p = path; *p != '\0'; p++)
        {
            joined += *p == '\\' ? '/' : *p;
        }
    }
    else
    {
        joined = path;
    }

    std::vector<std::string> parts;
    std::string component;
    for (std::size_t i = 0; i <= joined.size(); i++)
    {
        if (i < joined.size() && joined[i] != '/')
        {
            component += joined[i];
            continue;
        }
        if (component == "..")
        {
            if (!parts.empty())
            {
                parts.pop_back();
            }
        }
        else if (!component.empty() && component != ".")
        {
            parts.push_back(component);
        }
        component.clear();
    }

    std::string out;
    for (std::size_t i = 0; i < parts.size(); i++)
    {
        out += '/';
        out += parts[i];
    }
    return out.empty() ? std::string("/") : out;
}

// Relative paths live under the virtual working directory; read-only misses fall back to
// the same path inside the pkg's bundled assets, which needs the part below the game
// directory rather than below the current one.
static std::string LocalPath(const char *path)
{
    return IsRelative(path) ? Rebase(g_Cwd.c_str(), path) : std::string(path);
}

static bool BundledPath(const std::string &local, std::string *out)
{
    const std::string root = TH_PS4_GAME_DIR;
    if (local.compare(0, root.size(), root) != 0)
    {
        return false;
    }
    *out = std::string(TH_PS4_BUNDLED_ASSETS_DIR) + local.substr(root.size());
    return true;
}

// mkdir -p for the writable game directory.
static void MakeDirs(const char *path)
{
    std::string partial;
    for (const char *p = path; *p != '\0'; p++)
    {
        partial += *p;
        if (p[1] == '/' || p[1] == '\0')
        {
            __real_mkdir(partial.c_str(), 0777);
        }
    }
}

static void PS4_EarlyInit()
{
    MakeDirs(TH_PS4_GAME_DIR);
    // Best effort only; see Rebase().
    int chdirRes = chdir(TH_PS4_GAME_DIR);

    g_PS4Log = __real_fopen(TH_PS4_GAME_DIR "/ps4_log.txt", "w");
    PS4_Log("game dir %s: writable=%s, chdir=%d", TH_PS4_GAME_DIR, g_PS4Log != NULL ? "yes" : "NO", chdirRes);
    if (g_PS4Log == NULL)
    {
        PS4_Notify("Touhou: cannot write to " TH_PS4_GAME_DIR);
    }
    PS4_Log("bundled assets: %s", FileExists(TH_PS4_BUNDLED_ASSETS_DIR) ? "yes" : "no");

#ifndef TH_PS4_GNM
    // /data first so the user can always override what the pkg bundles.
    const char *modulesDir = HasPigletModules(TH_PS4_MODULES_DIR)           ? TH_PS4_MODULES_DIR
                             : HasPigletModules(TH_PS4_SHARED_MODULES_DIR)  ? TH_PS4_SHARED_MODULES_DIR
                             : HasPigletModules(TH_PS4_BUNDLED_MODULES_DIR) ? TH_PS4_BUNDLED_MODULES_DIR
                                                                            : NULL;
    if (modulesDir != NULL)
    {
        SDL_SetHint("SDL_PS4_PIGLET_MODULES_PATH", modulesDir);
        PS4_Log("using piglet + shacc from %s", modulesDir);
    }
    else
    {
        PS4_Log("shader compiler not found, rendering will fail");
        PS4_Notify("Touhou: missing libSceShaccVSH.sprx / libScePigletv2VSH.sprx in " TH_PS4_MODULES_DIR);
    }
#else
    PS4_Log("GNM backend: Piglet modules are not needed");
#endif

    SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG, kDualShock4Mapping);

    // Relative, so it resolves to /data/touhou/<game>/patch or the pkg's assets/patch.
    PS4_KeyboardInit();
    THPatch_Init("patch");
}

// Linked with --wrap=fopen, so this catches every fopen() in the game, SDL and freetype.
// Relative paths go to TH_PS4_GAME_DIR; read-only opens that miss there fall back to the
// pkg's bundled assets.
extern "C" FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (!IsRelative(path))
    {
        return __real_fopen(path, mode);
    }

    std::string local = LocalPath(path);
    FILE *f = __real_fopen(local.c_str(), mode);
    bool readOnly = mode[0] == 'r' && std::strchr(mode, '+') == NULL;
    std::string bundled;
    if (f == NULL && readOnly && BundledPath(local, &bundled))
    {
        f = __real_fopen(bundled.c_str(), mode);
        PS4_Log("fopen(%s, %s) -> %s", path, mode, f != NULL ? bundled.c_str() : "NOT FOUND");
    }
    else
    {
        PS4_Log("fopen(%s, %s) -> %s", path, mode, f != NULL ? local.c_str() : "FAILED");
    }
    return f;
}

extern "C" int __wrap_mkdir(const char *path, mode_t mode)
{
    return __real_mkdir(LocalPath(path).c_str(), mode);
}

// Only TH08 links these wrappers so far. TH06 and TH07 are already validated on hardware
// with the plain game-directory rebase and would need their own retest to change it.
#ifdef TH_PS4_VIRTUAL_CWD
extern "C" int __real_chdir(const char *path);
extern "C" int __real_rename(const char *from, const char *to);

// Always succeeds: the directory is virtual, and the games treat a failure here as fatal.
extern "C" int __wrap_chdir(const char *path)
{
    if (path == NULL || path[0] == '\0')
    {
        return -1;
    }
    g_Cwd = Rebase(g_Cwd.c_str(), path);
    __real_chdir(g_Cwd.c_str()); // best effort; see above
    MakeDirs(g_Cwd.c_str());
    PS4_Log("chdir(%s) -> %s", path, g_Cwd.c_str());
    return 0;
}

// TH08 rotates its score backups with rename() on bare file names.
extern "C" int __wrap_rename(const char *from, const char *to)
{
    return __real_rename(LocalPath(from).c_str(), LocalPath(to).c_str());
}
#endif

// Same policy as __wrap_fopen, for callers that do not go through fopen(): TH08 reads and
// writes everything with the Win32 file API, which the POSIX adapter implements on open(),
// stat() and unlink(). Those cannot be wrapped the way fopen() is, because the wrapped
// allocator and SDL need the real ones.
extern "C" void PS4_ResolveGamePath(const char *path, int readOnly, char *out, size_t outSize)
{
    if (out == NULL || outSize == 0)
    {
        return;
    }
    if (!IsRelative(path))
    {
        std::snprintf(out, outSize, "%s", path);
        return;
    }

    std::string local = LocalPath(path);
    std::string bundled;
    if (readOnly && !FileExists(local.c_str()) && BundledPath(local, &bundled) &&
        FileExists(bundled.c_str()))
    {
        local = bundled;
    }
    std::snprintf(out, outSize, "%s", local.c_str());
}

namespace
{
struct EarlyInit
{
    EarlyInit()
    {
        PS4_EarlyInit();
    }
} g_EarlyInit;
} // namespace

// --- SDL2_ttf < 2.0.18 shims (see ps4_compat.hpp) ---------------------------------

#if !SDL_TTF_VERSION_ATLEAST(2, 0, 18)

#undef TTF_OpenFont
#undef TTF_SetFontSize
#undef TTF_CloseFont

namespace
{
struct FontFamily
{
    std::string path;
    std::map<int, TTF_Font *> sizes;
};

// Every font handed to the game belongs to exactly one family; closing any of them
// closes the whole family.
std::map<TTF_Font *, FontFamily *> g_FontFamilies;
} // namespace

TTF_Font *PS4_TTF_OpenFont(const char *file, int ptsize)
{
    TTF_Font *font = TTF_OpenFont(file, ptsize);
    if (font != NULL)
    {
        FontFamily *family = new FontFamily{file, {{ptsize, font}}};
        g_FontFamilies[font] = family;
    }
    return font;
}

int PS4_TTF_SetFontSize(TTF_Font *&font, int ptsize)
{
    auto it = g_FontFamilies.find(font);
    if (it == g_FontFamilies.end())
    {
        return -1;
    }
    FontFamily *family = it->second;

    auto sized = family->sizes.find(ptsize);
    if (sized != family->sizes.end())
    {
        font = sized->second;
        return 0;
    }

    TTF_Font *resized = TTF_OpenFont(family->path.c_str(), ptsize);
    if (resized == NULL)
    {
        return -1;
    }
    TTF_SetFontStyle(resized, TTF_GetFontStyle(font));
    family->sizes[ptsize] = resized;
    g_FontFamilies[resized] = family;
    font = resized;
    return 0;
}

void PS4_TTF_CloseFont(TTF_Font *font)
{
    auto it = g_FontFamilies.find(font);
    if (it == g_FontFamilies.end())
    {
        TTF_CloseFont(font);
        return;
    }
    FontFamily *family = it->second;
    for (auto &[size, sizedFont] : family->sizes)
    {
        g_FontFamilies.erase(sizedFont);
        TTF_CloseFont(sizedFont);
    }
    delete family;
}

#endif
