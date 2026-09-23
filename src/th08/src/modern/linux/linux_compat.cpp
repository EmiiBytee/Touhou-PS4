// POSIX/SDL implementation of the Win32, DirectInput and DirectSound surface the
// reconstructed TH08 sources call into. Shared by the Linux and PS4 targets: the pieces
// that genuinely differ (font discovery, CP932 conversion, directory globbing, window
// creation and game controllers) are selected below with TH_PS4.
#ifdef TH_PS4
#include "modern/ps4/th08_ps4_compat.hpp"
#else
#include "linux_compat.hpp"
#endif

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <dinput.h>
#include <dsound.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <map>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#ifdef TH_PS4
#include <dirent.h>
#include <fnmatch.h>
#include <sched.h>
void PS4_Log(const char *fmt, ...);
extern "C" void PS4_ResolveGamePath(const char *path, int readOnly, char *out, size_t outSize);
// Right edge (text-buffer pixels) of the sprite being written, set around each of the game's
// DrawText calls; TextOutA narrows a line that would run past it. 0 means no limit.
extern "C" int g_Ps4TextFitRight = 0;
#else
#include <dlfcn.h>
#include <fontconfig/fontconfig.h>
#include <glob.h>
#include <iconv.h>
#endif

namespace
{
enum HandleKind { HANDLE_FILE, HANDLE_THREAD, HANDLE_EVENT, HANDLE_MUTEX, HANDLE_FIND };

struct LinuxHandle
{
    explicit LinuxHandle(HandleKind value) : kind(value) {}
    virtual ~LinuxHandle() {}
    HandleKind kind;
};

struct FileHandle : LinuxHandle
{
    explicit FileHandle(int value) : LinuxHandle(HANDLE_FILE), fd(value) {}
    ~FileHandle() { if (fd >= 0) close(fd); }
    int fd;
};

struct ThreadHandle : LinuxHandle
{
    ThreadHandle() : LinuxHandle(HANDLE_THREAD), finished(false), joined(false), result(0), id(0) {}
    pthread_t thread;
    volatile bool finished;
    bool joined;
    DWORD result;
    DWORD id;
    LPTHREAD_START_ROUTINE start;
    LPVOID parameter;
};

struct EventHandle : LinuxHandle
{
    EventHandle(bool manualReset, bool initial)
        : LinuxHandle(HANDLE_EVENT), manual(manualReset), signaled(initial)
    {
        pthread_mutex_init(&mutex, NULL);
        pthread_cond_init(&condition, NULL);
    }
    ~EventHandle()
    {
        pthread_cond_destroy(&condition);
        pthread_mutex_destroy(&mutex);
    }
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool manual;
    bool signaled;
};

struct MutexHandle : LinuxHandle
{
    MutexHandle() : LinuxHandle(HANDLE_MUTEX) { pthread_mutex_init(&mutex, NULL); }
    ~MutexHandle() { pthread_mutex_destroy(&mutex); }
    pthread_mutex_t mutex;
};

struct FindHandle : LinuxHandle
{
    FindHandle() : LinuxHandle(HANDLE_FIND), index(0) {}
    std::vector<std::string> paths;
    size_t index;
};

struct GdiObject
{
    enum Kind { BITMAP, FONT } kind;
    virtual ~GdiObject() {}
};

struct GdiBitmap : GdiObject
{
    GdiBitmap(int width_, int height_, int bits_)
        : width(width_), height(height_ < 0 ? -height_ : height_), bits(bits_)
    {
        kind = BITMAP;
        pitch = ((width * bits + 31) / 32) * 4;
        pixels.resize(pitch * height);
    }
    int width, height, bits, pitch;
    std::vector<BYTE> pixels;
};

struct GdiFont : GdiObject
{
    GdiFont() : font(NULL), latin(NULL), latinOffsetY(0), shared(false) { kind = FONT; }
    explicit GdiFont(TTF_Font *font_) : font(font_), latin(NULL), latinOffsetY(0), shared(false) { kind = FONT; }
    ~GdiFont()
    {
        if (shared) return;
        if (font != NULL) TTF_CloseFont(font);
        if (latin != NULL) TTF_CloseFont(latin);
    }
    TTF_Font *font;
    // PS4: the faces belong to the process-wide font cache (see CreateFontA), not to this object.
    bool shared;
    // PS4: the translation patch's Latin font for text without Japanese, and how far down to
    // draw it so its baseline meets the Japanese font's.
    TTF_Font *latin;
    int latinOffsetY;
};

struct GdiDc
{
    GdiDc() : bitmap(NULL), font(&stockFont), color(0xffffffff) {}
    GdiBitmap *bitmap;
    GdiFont stockFont;
    GdiFont *font;
    COLORREF color;
};

DWORD g_lastError;
WNDPROC g_windowProcedure;
SDL_Window *g_window;
std::map<DWORD, std::vector<MSG> > g_threadMessages;
pthread_mutex_t g_messageMutex = PTHREAD_MUTEX_INITIALIZER;

#ifndef TH_PS4
std::string ExecutableSiblingPath(const char *filename)
{
    char path[PATH_MAX + 1];
    ssize_t count = readlink("/proc/self/exe", path, PATH_MAX);
    if (count <= 0 || count > PATH_MAX)
        return std::string();
    path[count] = '\0';
    char *separator = strrchr(path, '/');
    if (separator == NULL)
        return std::string();
    separator[1] = '\0';
    return std::string(path) + filename;
}

// The PS4 has no desktop window, so its icon comes from the pkg's sce_sys/icon0.png.
void SetApplicationIcon(SDL_Window *window)
{
    if (window == NULL)
        return;
    const std::string iconPath = ExecutableSiblingPath("th08-modern.png");
    if (iconPath.empty())
        return;
    SDL_Surface *icon = IMG_Load(iconPath.c_str());
    if (icon == NULL)
        return;
    SDL_SetWindowIcon(window, icon);
    SDL_FreeSurface(icon);
}
#endif

DWORD CurrentThreadIdImpl()
{
    return static_cast<DWORD>(pthread_self());
}

#ifdef TH_PS4
// SDL carries its own iconv, which is what the PS4 build has instead of the host's.
std::string ConvertCp932ToUtf8(const char *text, size_t length)
{
    if (text == NULL || length == 0) return std::string();
    char *converted = SDL_iconv_string("UTF-8", "CP932", text, length + 1);
    if (converted == NULL)
    {
        // SDL spells the same encoding either way depending on its build.
        converted = SDL_iconv_string("UTF-8", "SHIFT_JIS", text, length + 1);
    }
    if (converted == NULL) return std::string(text, length);
    std::string result(converted);
    SDL_free(converted);
    return result;
}

// One bundled font, resolved through the wrapped fopen(): the user's own msgothic.ttc in
// /data/touhou/th08, else the copy a personal pkg carries in /app0/assets.
const char *ResolveJapaneseFont()
{
    return "msgothic.ttc";
}

// The translation patch's Latin font, staged as patch/latin.ttf (Aroania for IN, from
// script_latin), stands in for the Japanese font on text without Japanese, as thcrap does
// on Windows. Strings never mix the two, so the Latin text is fitted to the Japanese font's
// cell on its own terms: sized so the tallest ascender to the deepest descender fills 86% of
// the cell, and moved so it starts 6% down -- the Latin fonts' own line metrics carry a lot of
// empty space, which made TH06's English tiny and cut its descenders when used as-is. (At 92%
// the bold face ran wider than IN's dialogue rows.)
struct LatinFit
{
    int size;
    int offsetY;
};

void OpenLatinFont(GdiFont *result, int height)
{
    static const char *const kLatinFont = "patch/latin.ttf";
    static bool unavailable = false;
    static std::map<int, LatinFit> fits;
    if (unavailable || result == NULL || result->font == NULL || height <= 0) return;

    std::map<int, LatinFit>::iterator known = fits.find(height);
    if (known == fits.end())
    {
        TTF_Font *probe = TTF_OpenFont(kLatinFont, height);
        if (probe == NULL)
        {
            unavailable = true;
            return;
        }
        int top = 0, bottom = 0;
        for (const char *c = "bdfhkltgjpqy"; *c != '\0'; ++c)
        {
            int minX, maxX, minY, maxY, advance;
            if (TTF_GlyphMetrics(probe, static_cast<Uint16>(*c), &minX, &maxX, &minY, &maxY, &advance) == 0)
            {
                if (maxY > top) top = maxY;
                if (minY < bottom) bottom = minY;
            }
        }
        TTF_CloseFont(probe);
        const int cell = TTF_FontHeight(result->font);
        if (top - bottom <= 0 || cell <= 0)
        {
            unavailable = true;
            return;
        }
        LatinFit fit;
        fit.size = static_cast<int>(height * (0.86 * cell) / (top - bottom) + 0.5);
        if (fit.size < 1) fit.size = 1;
        // Where the glyph tops land once drawn at that size.
        TTF_Font *sized = TTF_OpenFont(kLatinFont, fit.size);
        if (sized == NULL)
        {
            unavailable = true;
            return;
        }
        int sizedTop = 0;
        for (const char *c = "bdfhklt"; *c != '\0'; ++c)
        {
            int minX, maxX, minY, maxY, advance;
            if (TTF_GlyphMetrics(sized, static_cast<Uint16>(*c), &minX, &maxX, &minY, &maxY, &advance) == 0 &&
                maxY > sizedTop)
                sizedTop = maxY;
        }
        fit.offsetY = static_cast<int>(cell * 0.06 + 0.5) - (TTF_FontAscent(sized) - sizedTop);
        TTF_CloseFont(sized);
        known = fits.insert(std::make_pair(height, fit)).first;
        PS4_Log("latin font: %d px text -> %s at %d, offset %d", height, kLatinFont, fit.size, fit.offsetY);
    }

    // Opened once per size and weight: every text draw creates and deletes its font, and
    // reopening the file each time stalled the frame.
    const bool bold = (TTF_GetFontStyle(result->font) & TTF_STYLE_BOLD) != 0;
    static std::map<std::pair<int, bool>, TTF_Font *> opened;
    const std::pair<int, bool> key(known->second.size, bold);
    std::map<std::pair<int, bool>, TTF_Font *>::iterator cached = opened.find(key);
    if (cached == opened.end())
    {
        TTF_Font *latin = TTF_OpenFont(kLatinFont, known->second.size);
        if (latin == NULL) return;
        TTF_SetFontHinting(latin, TTF_HINTING_LIGHT);
        if (bold) TTF_SetFontStyle(latin, TTF_STYLE_BOLD);
        cached = opened.insert(std::make_pair(key, latin)).first;
    }
    result->latin = cached->second;
    result->latinOffsetY = known->second.offsetY;
}
#else
std::string ConvertCp932ToUtf8(const char *text, size_t length)
{
    if (text == NULL || length == 0) return std::string();
    iconv_t converter = iconv_open("UTF-8", "CP932");
    if (converter == reinterpret_cast<iconv_t>(-1)) return std::string(text, length);

    std::vector<char> output(length * 4 + 8, 0);
    char *input = const_cast<char *>(text);
    char *destination = &output[0];
    size_t inputLeft = length;
    size_t outputLeft = output.size() - 1;
    if (iconv(converter, &input, &inputLeft, &destination, &outputLeft) == static_cast<size_t>(-1))
    {
        iconv_close(converter);
        return std::string(text, length);
    }
    iconv_close(converter);
    return std::string(&output[0], destination - &output[0]);
}

const char *ResolveJapaneseFont()
{
    static std::string path;
    static bool resolved;
    if (resolved) return path.empty() ? NULL : path.c_str();
    resolved = true;

    const char *overridePath = getenv("TH08_FONT");
    if (overridePath != NULL && access(overridePath, R_OK) == 0)
    {
        path = overridePath;
        return path.c_str();
    }

    if (!FcInit()) return NULL;
    FcPattern *pattern = FcPatternCreate();
    if (pattern == NULL) return NULL;
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>("VL Gothic"));
    FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8 *>("ja"));
    FcPatternAddInteger(pattern, FC_SPACING, FC_MONO);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (match == NULL) return NULL;
    FcChar8 *file = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != NULL)
        path = reinterpret_cast<const char *>(file);
    FcPatternDestroy(match);
    return path.empty() ? NULL : path.c_str();
}
#endif

void PutGdiTextPixel(GdiBitmap *bitmap, int x, int y, COLORREF color, BYTE coverage)
{
    if (bitmap == NULL || x < 0 || y < 0 || x >= bitmap->width || y >= bitmap->height || coverage == 0) return;
    const int red = color & 0xff;
    const int green = (color >> 8) & 0xff;
    const int blue = (color >> 16) & 0xff;
    BYTE *pixel = &bitmap->pixels[y * bitmap->pitch + x * bitmap->bits / 8];
    if (bitmap->bits == 16)
    {
        uint16_t packed;
        memcpy(&packed, pixel, sizeof(packed));
        int oldRed = ((packed >> 10) & 0x1f) * 255 / 31;
        int oldGreen = ((packed >> 5) & 0x1f) * 255 / 31;
        int oldBlue = (packed & 0x1f) * 255 / 31;
        oldRed = (oldRed * (255 - coverage) + red * coverage) / 255;
        oldGreen = (oldGreen * (255 - coverage) + green * coverage) / 255;
        oldBlue = (oldBlue * (255 - coverage) + blue * coverage) / 255;
        packed = static_cast<uint16_t>(((oldRed >> 3) << 10) | ((oldGreen >> 3) << 5) | (oldBlue >> 3));
        memcpy(pixel, &packed, sizeof(packed));
    }
    else if (bitmap->bits == 32)
    {
        pixel[0] = static_cast<BYTE>((pixel[0] * (255 - coverage) + blue * coverage) / 255);
        pixel[1] = static_cast<BYTE>((pixel[1] * (255 - coverage) + green * coverage) / 255);
        pixel[2] = static_cast<BYTE>((pixel[2] * (255 - coverage) + red * coverage) / 255);
        pixel[3] = 0;
    }
}

void *ThreadTrampoline(void *opaque)
{
    ThreadHandle *handle = static_cast<ThreadHandle *>(opaque);
    handle->id = CurrentThreadIdImpl();
    handle->result = handle->start(handle->parameter);
    handle->finished = true;
    return NULL;
}

bool FillFindData(FindHandle *handle, WIN32_FIND_DATAA *data)
{
    if (handle->index >= handle->paths.size())
        return false;
    memset(data, 0, sizeof(*data));
    const std::string &path = handle->paths[handle->index++];
    const char *name = strrchr(path.c_str(), '/');
    strncpy(data->cFileName, name != NULL ? name + 1 : path.c_str(), MAX_PATH - 1);
    struct stat info;
    if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode))
        data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    return true;
}

void PumpSdlEvents(MSG *message, bool *hasMessage)
{
    *hasMessage = false;
    // The PS4 target never initializes video (GNM owns the display), so the events
    // subsystem is what says whether there is a queue to read.
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
        return;
    SDL_Event event;
    if (!SDL_PollEvent(&event))
        return;
    memset(message, 0, sizeof(*message));
    message->hwnd = reinterpret_cast<HWND>(g_window);
    if (event.type == SDL_QUIT)
        message->message = WM_CLOSE;
    else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
    {
        message->message = WM_ACTIVATEAPP;
        message->wParam = TRUE;
    }
    else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
    {
        message->message = WM_ACTIVATEAPP;
        message->wParam = FALSE;
    }
    else
        message->message = 0;
    *hasMessage = true;
}

void FillKeyboard(BYTE *state, bool directInput)
{
    memset(state, 0, 256);
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
#define MAP_KEY(win, sdl) state[(win)] = keys[(sdl)] ? 0x80 : 0
    if (directInput)
    {
        MAP_KEY(DIK_UP, SDL_SCANCODE_UP); MAP_KEY(DIK_DOWN, SDL_SCANCODE_DOWN);
        MAP_KEY(DIK_LEFT, SDL_SCANCODE_LEFT); MAP_KEY(DIK_RIGHT, SDL_SCANCODE_RIGHT);
        MAP_KEY(DIK_NUMPAD1, SDL_SCANCODE_KP_1); MAP_KEY(DIK_NUMPAD2, SDL_SCANCODE_KP_2);
        MAP_KEY(DIK_NUMPAD3, SDL_SCANCODE_KP_3); MAP_KEY(DIK_NUMPAD4, SDL_SCANCODE_KP_4);
        MAP_KEY(DIK_NUMPAD6, SDL_SCANCODE_KP_6); MAP_KEY(DIK_NUMPAD7, SDL_SCANCODE_KP_7);
        MAP_KEY(DIK_NUMPAD8, SDL_SCANCODE_KP_8); MAP_KEY(DIK_NUMPAD9, SDL_SCANCODE_KP_9);
        MAP_KEY(DIK_HOME, SDL_SCANCODE_HOME); MAP_KEY(DIK_P, SDL_SCANCODE_P);
        MAP_KEY(DIK_D, SDL_SCANCODE_D); MAP_KEY(DIK_Z, SDL_SCANCODE_Z); MAP_KEY(DIK_X, SDL_SCANCODE_X);
        MAP_KEY(DIK_LSHIFT, SDL_SCANCODE_LSHIFT); MAP_KEY(DIK_RSHIFT, SDL_SCANCODE_RSHIFT);
        MAP_KEY(DIK_ESCAPE, SDL_SCANCODE_ESCAPE); MAP_KEY(DIK_LCONTROL, SDL_SCANCODE_LCTRL);
        MAP_KEY(DIK_RCONTROL, SDL_SCANCODE_RCTRL); MAP_KEY(DIK_Q, SDL_SCANCODE_Q);
        MAP_KEY(DIK_S, SDL_SCANCODE_S); MAP_KEY(DIK_R, SDL_SCANCODE_R); MAP_KEY(DIK_RETURN, SDL_SCANCODE_RETURN);
    }
    else
    {
        MAP_KEY(VK_UP, SDL_SCANCODE_UP); MAP_KEY(VK_DOWN, SDL_SCANCODE_DOWN);
        MAP_KEY(VK_LEFT, SDL_SCANCODE_LEFT); MAP_KEY(VK_RIGHT, SDL_SCANCODE_RIGHT);
        MAP_KEY(VK_NUMPAD1, SDL_SCANCODE_KP_1); MAP_KEY(VK_NUMPAD2, SDL_SCANCODE_KP_2);
        MAP_KEY(VK_NUMPAD3, SDL_SCANCODE_KP_3); MAP_KEY(VK_NUMPAD4, SDL_SCANCODE_KP_4);
        MAP_KEY(VK_NUMPAD6, SDL_SCANCODE_KP_6); MAP_KEY(VK_NUMPAD7, SDL_SCANCODE_KP_7);
        MAP_KEY(VK_NUMPAD8, SDL_SCANCODE_KP_8); MAP_KEY(VK_NUMPAD9, SDL_SCANCODE_KP_9);
        MAP_KEY(VK_HOME, SDL_SCANCODE_HOME); MAP_KEY('P', SDL_SCANCODE_P); MAP_KEY('D', SDL_SCANCODE_D);
        MAP_KEY('Z', SDL_SCANCODE_Z); MAP_KEY('X', SDL_SCANCODE_X); MAP_KEY(VK_SHIFT, SDL_SCANCODE_LSHIFT);
        MAP_KEY(VK_ESCAPE, SDL_SCANCODE_ESCAPE); MAP_KEY(VK_CONTROL, SDL_SCANCODE_LCTRL);
        MAP_KEY('Q', SDL_SCANCODE_Q); MAP_KEY('S', SDL_SCANCODE_S); MAP_KEY('R', SDL_SCANCODE_R);
        MAP_KEY(VK_RETURN, SDL_SCANCODE_RETURN);
    }
#undef MAP_KEY
}
} // namespace

extern "C" SDL_Window *th08_linux_get_window() { return g_window; }

extern "C" {
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD, LPVOID, DWORD disposition, DWORD, HANDLE)
{
    int flags = (access & (GENERIC_WRITE | FILE_APPEND_DATA)) ? O_WRONLY : O_RDONLY;
    if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) flags = O_RDWR;
    if (access & FILE_APPEND_DATA) flags |= O_APPEND;
    if (disposition == CREATE_ALWAYS) flags |= O_CREAT | O_TRUNC;
    if (disposition == OPEN_ALWAYS) flags |= O_CREAT;
#ifdef TH_PS4
    // This is how the game reaches every one of its files, and chdir() has no effect in the
    // PS4 sandbox, so the path is rebased onto the writable game directory or the pkg's own
    // bundled assets.
    char resolved[PATH_MAX];
    PS4_ResolveGamePath(path, (flags & (O_WRONLY | O_RDWR | O_CREAT)) == 0, resolved, sizeof(resolved));
    int fd = open(resolved, flags, 0666);
    if (fd < 0)
    {
        g_lastError = errno;
        PS4_Log("CreateFileA(%s) -> %s FAILED (%d)", path, resolved, errno);
        return INVALID_HANDLE_VALUE;
    }
#else
    int fd = open(path, flags, 0666);
    if (fd < 0) { g_lastError = errno; return INVALID_HANDLE_VALUE; }
#endif
    return new FileHandle(fd);
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPVOID security, DWORD disposition, DWORD attrs, HANDLE templ)
{
    char converted[PATH_MAX];
    if (wcstombs(converted, path, sizeof(converted) - 1) == static_cast<size_t>(-1))
        return INVALID_HANDLE_VALUE;
    converted[sizeof(converted) - 1] = 0;
    return CreateFileA(converted, access, share, security, disposition, attrs, templ);
}

BOOL ReadFile(HANDLE raw, LPVOID data, DWORD size, LPDWORD readSize, LPVOID)
{
    if (raw == INVALID_HANDLE_VALUE || raw == NULL) return FALSE;
    ssize_t result = read(static_cast<FileHandle *>(raw)->fd, data, size);
    if (readSize != NULL) *readSize = result < 0 ? 0 : static_cast<DWORD>(result);
    return result >= 0;
}

BOOL WriteFile(HANDLE raw, LPCVOID data, DWORD size, LPDWORD written, LPVOID)
{
    if (raw == INVALID_HANDLE_VALUE || raw == NULL) return FALSE;
    ssize_t result = write(static_cast<FileHandle *>(raw)->fd, data, size);
    if (written != NULL) *written = result < 0 ? 0 : static_cast<DWORD>(result);
    return result >= 0;
}

DWORD SetFilePointer(HANDLE raw, LONG offset, LONG *, DWORD origin)
{
    int whence = origin == FILE_BEGIN ? SEEK_SET : origin == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    off_t result = lseek(static_cast<FileHandle *>(raw)->fd, offset, whence);
    return result < 0 ? static_cast<DWORD>(-1) : static_cast<DWORD>(result);
}

DWORD GetFileSize(HANDLE raw, LPDWORD high)
{
    struct stat info;
    if (fstat(static_cast<FileHandle *>(raw)->fd, &info) != 0) return static_cast<DWORD>(-1);
    if (high != NULL) *high = static_cast<DWORD>(static_cast<unsigned long long>(info.st_size) >> 32);
    return static_cast<DWORD>(info.st_size);
}

BOOL CloseHandle(HANDLE raw)
{
    if (raw == NULL || raw == INVALID_HANDLE_VALUE) return FALSE;
    LinuxHandle *handle = static_cast<LinuxHandle *>(raw);
    if (handle->kind == HANDLE_THREAD)
    {
        ThreadHandle *thread = static_cast<ThreadHandle *>(handle);
        if (!thread->finished) return TRUE;
        if (!thread->joined) pthread_join(thread->thread, NULL);
    }
    delete handle;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE raw) { return fsync(static_cast<FileHandle *>(raw)->fd) == 0; }

#ifdef TH_PS4
BOOL DeleteFileA(LPCSTR path)
{
    char resolved[PATH_MAX];
    PS4_ResolveGamePath(path, 0, resolved, sizeof(resolved));
    return unlink(resolved) == 0;
}
#else
BOOL DeleteFileA(LPCSTR path) { return unlink(path) == 0; }
#endif

DWORD GetFileAttributesW(LPCWSTR path)
{
    char converted[PATH_MAX];
    if (wcstombs(converted, path, sizeof(converted) - 1) == static_cast<size_t>(-1)) return INVALID_FILE_ATTRIBUTES;
#ifdef TH_PS4
    char resolved[PATH_MAX];
    PS4_ResolveGamePath(converted, 1, resolved, sizeof(resolved));
    memcpy(converted, resolved, sizeof(converted));
#endif
    struct stat info;
    if (stat(converted, &info) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL SetCurrentDirectoryW(LPCWSTR path)
{
    char converted[PATH_MAX];
    if (wcstombs(converted, path, sizeof(converted) - 1) == static_cast<size_t>(-1)) return FALSE;
    return chdir(converted) == 0;
}

#ifdef TH_PS4
// The game only ever globs bare file names (replays, score backups) after stepping into the
// directory that holds them, so the search runs in whatever the shared glue currently calls
// the working directory.
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *data)
{
    if (pattern == NULL) return INVALID_HANDLE_VALUE;
    const char *name = strrchr(pattern, '/');
    std::string relative = ".";
    if (name != NULL)
    {
        relative.assign(pattern, name - pattern);
        name++;
    }
    else
    {
        name = pattern;
    }
    char resolved[PATH_MAX];
    PS4_ResolveGamePath(relative.c_str(), 1, resolved, sizeof(resolved));
    const std::string directory = resolved;

    DIR *dir = opendir(directory.c_str());
    if (dir == NULL) return INVALID_HANDLE_VALUE;
    FindHandle *handle = new FindHandle();
    while (const struct dirent *entry = readdir(dir))
    {
        // FNM_CASEFOLD is deliberate: these names come from a case-insensitive original.
        if (fnmatch(name, entry->d_name, FNM_CASEFOLD) == 0)
            handle->paths.push_back(directory + "/" + entry->d_name);
    }
    closedir(dir);
    if (!FillFindData(handle, data)) { delete handle; return INVALID_HANDLE_VALUE; }
    return handle;
}
#else
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *data)
{
    glob_t result;
    if (glob(pattern, 0, NULL, &result) != 0) return INVALID_HANDLE_VALUE;
    FindHandle *handle = new FindHandle();
    for (size_t i = 0; i < result.gl_pathc; ++i) handle->paths.push_back(result.gl_pathv[i]);
    globfree(&result);
    if (!FillFindData(handle, data)) { delete handle; return INVALID_HANDLE_VALUE; }
    return handle;
}
#endif

BOOL FindNextFileA(HANDLE raw, WIN32_FIND_DATAA *data) { return FillFindData(static_cast<FindHandle *>(raw), data); }
BOOL FindClose(HANDLE raw)
{
    if (raw == NULL || raw == INVALID_HANDLE_VALUE)
        return FALSE;
    LinuxHandle *handle = static_cast<LinuxHandle *>(raw);
    if (handle->kind != HANDLE_FIND)
        return FALSE;
    delete static_cast<FindHandle *>(handle);
    return TRUE;
}
void Sleep(DWORD milliseconds) { usleep(static_cast<useconds_t>(milliseconds) * 1000); }

DWORD timeGetTime(void)
{
    struct timeval value; gettimeofday(&value, NULL);
    return static_cast<DWORD>(value.tv_sec * 1000ULL + value.tv_usec / 1000);
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *value) { value->QuadPart = 1000000; return TRUE; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *value)
{
    struct timeval time; gettimeofday(&time, NULL);
    value->QuadPart = time.tv_sec * 1000000LL + time.tv_usec; return TRUE;
}
DWORD GetCurrentThreadId(void) { return CurrentThreadIdImpl(); }

HANDLE CreateThread(LPVOID, size_t, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD, LPDWORD id)
{
    ThreadHandle *handle = new ThreadHandle(); handle->start = start; handle->parameter = parameter;
    if (pthread_create(&handle->thread, NULL, ThreadTrampoline, handle) != 0) { delete handle; return NULL; }
    while (handle->id == 0) sched_yield();
    if (id != NULL) *id = handle->id;
    return handle;
}

BOOL PostThreadMessageA(DWORD id, UINT message, WPARAM wparam, LPARAM lparam)
{
    MSG value; memset(&value, 0, sizeof(value)); value.message = message; value.wParam = wparam; value.lParam = lparam;
    pthread_mutex_lock(&g_messageMutex); g_threadMessages[id].push_back(value); pthread_mutex_unlock(&g_messageMutex);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE raw, DWORD timeout)
{
    // GameplaySetupThread clears Supervisor::runningSubthreadHandle from inside the thread
    // itself, while the main thread can be in ThreadClose() between its own null check and
    // this call -- which is what a spell practice retry does. Win32 answers WAIT_FAILED for
    // a null handle; dereferencing it killed the process with SIGSEGV.
    if (raw == NULL || raw == INVALID_HANDLE_VALUE) return WAIT_FAILED;
    LinuxHandle *base = static_cast<LinuxHandle *>(raw);
    DWORD start = timeGetTime();
    for (;;)
    {
        if (base->kind == HANDLE_THREAD && static_cast<ThreadHandle *>(base)->finished) return WAIT_OBJECT_0;
        if (base->kind == HANDLE_EVENT)
        {
            EventHandle *event = static_cast<EventHandle *>(base);
            pthread_mutex_lock(&event->mutex);
            if (event->signaled)
            {
                if (!event->manual) event->signaled = false;
                pthread_mutex_unlock(&event->mutex); return WAIT_OBJECT_0;
            }
            pthread_mutex_unlock(&event->mutex);
        }
        if (timeout != INFINITE && timeGetTime() - start >= timeout) return WAIT_TIMEOUT;
        usleep(1000);
    }
}

DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL, DWORD timeout, DWORD)
{
    DWORD start = timeGetTime();
    for (;;)
    {
        for (DWORD i = 0; i < count; ++i) if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) return i;
        pthread_mutex_lock(&g_messageMutex);
        bool hasMessages = !g_threadMessages[CurrentThreadIdImpl()].empty();
        pthread_mutex_unlock(&g_messageMutex);
        if (hasMessages) return count;
        if (timeout != INFINITE && timeGetTime() - start >= timeout) return WAIT_TIMEOUT;
        usleep(1000);
    }
}

HANDLE CreateEventA(LPVOID, BOOL manual, BOOL initial, LPCSTR) { return new EventHandle(manual != FALSE, initial != FALSE); }
BOOL SetEvent(HANDLE raw)
{
    EventHandle *event = static_cast<EventHandle *>(raw); pthread_mutex_lock(&event->mutex);
    event->signaled = true; pthread_cond_broadcast(&event->condition); pthread_mutex_unlock(&event->mutex); return TRUE;
}
UINT_PTR SetTimer(HWND, UINT_PTR id, UINT, void *) { return id != 0 ? id : 1; }
BOOL KillTimer(HWND, UINT_PTR) { return TRUE; }
HANDLE CreateMutexA(LPVOID, BOOL, LPCSTR) { g_lastError = 0; return new MutexHandle(); }
DWORD GetLastError(void) { return g_lastError; }
void InitializeCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_init(value, NULL); }
void DeleteCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_destroy(value); }
void EnterCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_lock(value); }
void LeaveCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_unlock(value); }

BOOL PeekMessageA(MSG *message, HWND, UINT, UINT, UINT)
{
    pthread_mutex_lock(&g_messageMutex);
    std::vector<MSG> &queue = g_threadMessages[CurrentThreadIdImpl()];
    if (!queue.empty()) { *message = queue.front(); queue.erase(queue.begin()); pthread_mutex_unlock(&g_messageMutex); return TRUE; }
    pthread_mutex_unlock(&g_messageMutex);
    bool hasMessage; PumpSdlEvents(message, &hasMessage); return hasMessage;
}
BOOL TranslateMessage(const MSG *) { return TRUE; }
LRESULT DispatchMessageA(const MSG *message) { return g_windowProcedure != NULL ? g_windowProcedure(message->hwnd, message->message, message->wParam, message->lParam) : 0; }
LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) { return 0; }
BOOL RegisterClassA(const WNDCLASSA *value) { g_windowProcedure = value->lpfnWndProc; return TRUE; }

#ifdef TH_PS4
// A PS4 application has no window: GNM owns the display and scans out its own buffers, so
// this only brings up the event and controller subsystems and hands back a token HWND for
// the game to carry around. The 4:3 / 16:9 choice is applied by the renderer instead.
HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HANDLE, HINSTANCE, LPVOID)
{
    if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
    {
        PS4_Log("win32: SDL_Init failed: %s", SDL_GetError());
        return NULL;
    }
    g_window = NULL;
    return reinterpret_cast<HWND>(1);
}

BOOL DestroyWindow(HWND) { SDL_Quit(); return TRUE; }
#else
HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR title, DWORD style, int, int, int width, int height, HWND, HANDLE, HINSTANCE, LPVOID)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    { fprintf(stderr, "th08-modern: SDL_Init failed: %s\n", SDL_GetError()); return NULL; }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (style == WS_OVERLAPPEDWINDOW) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    else { width = 640; height = 480; }
    g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (g_window == NULL) fprintf(stderr, "th08-modern: SDL_CreateWindow failed: %s\n", SDL_GetError());
    else SetApplicationIcon(g_window);
    return reinterpret_cast<HWND>(g_window);
}

BOOL DestroyWindow(HWND) { if (g_window != NULL) SDL_DestroyWindow(g_window); g_window = NULL; SDL_Quit(); return TRUE; }
#endif
BOOL ShowWindow(HWND, int) { return TRUE; }
BOOL MoveWindow(HWND, int, int, int, int, BOOL) { return TRUE; }
int ShowCursor(BOOL show) { return SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }
HCURSOR SetCursor(HCURSOR value) { return value; }
HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return reinterpret_cast<HCURSOR>(1); }
HGDIOBJ GetStockObject(int) { return NULL; }
int GetSystemMetrics(int metric) { return metric == SM_CYCAPTION ? 24 : 4; }
BOOL SystemParametersInfoA(UINT, UINT, PVOID value, UINT) { if (value != NULL) *static_cast<BOOL *>(value) = FALSE; return TRUE; }
HWND GetForegroundWindow(void) { return reinterpret_cast<HWND>(g_window); }
DWORD GetWindowThreadProcessId(HWND, LPDWORD process) { if (process != NULL) *process = getpid(); return GetCurrentThreadId(); }
BOOL AttachThreadInput(DWORD, DWORD, BOOL) { return TRUE; }
HWND SetActiveWindow(HWND window) { if (g_window != NULL) SDL_RaiseWindow(g_window); return window; }
LONG GetWindowLongA(HWND, int) { return 0; }
BOOL WINNLSEnableIME(HWND, BOOL) { return TRUE; }
BOOL GetKeyboardState(BYTE *state) { FillKeyboard(state, false); return TRUE; }
BOOL SetKeyboardState(const BYTE *) { return TRUE; }
int MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT) { fprintf(stderr, "%s: %s\n", title ? title : "TH08", text ? text : ""); return 0; }
int MessageBoxW(HWND, LPCWSTR text, LPCWSTR title, UINT) { fwprintf(stderr, L"%ls: %ls\n", title ? title : L"TH08", text ? text : L""); return 0; }

DWORD GetModuleFileNameA(HMODULE, LPSTR buffer, DWORD size)
{
    ssize_t count = readlink("/proc/self/exe", buffer, size - 1); if (count < 0) return 0;
    buffer[count] = 0; return static_cast<DWORD>(count);
}
DWORD GetConsoleTitleA(LPSTR buffer, DWORD size) { if (size) buffer[0] = 0; return 0; }
void GetStartupInfoA(STARTUPINFOA *value) { DWORD size = value->cb; memset(value, 0, size); value->cb = size; }
int MultiByteToWideChar(UINT, DWORD, LPCSTR source, int sourceSize, LPWSTR dest, int destSize)
{
    size_t result = mbstowcs(dest, source, destSize); return result == static_cast<size_t>(-1) ? 0 : static_cast<int>(result + (sourceSize < 0));
}
DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD error, DWORD, LPSTR buffer, DWORD size, va_list *)
{
    const char *message = strerror(error); if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) *reinterpret_cast<char **>(buffer) = strdup(message);
    else if (size) { strncpy(buffer, message, size - 1); buffer[size - 1] = 0; } return strlen(message);
}
LPVOID LocalFree(LPVOID value) { free(value); return NULL; }
HGLOBAL GlobalAlloc(UINT, size_t size) { return calloc(1, size); }
HGLOBAL GlobalFree(HGLOBAL value) { free(value); return NULL; }
#ifdef TH_PS4
// Nothing the game loads this way (Windows shell and IME helpers) exists here, and the
// callers all treat a missing module as "feature unavailable".
HMODULE LoadLibraryA(LPCSTR) { return NULL; }
void *GetProcAddress(HMODULE, LPCSTR) { return NULL; }
#else
HMODULE LoadLibraryA(LPCSTR path) { return dlopen(path, RTLD_NOW); }
void *GetProcAddress(HMODULE module, LPCSTR name) { return dlsym(module, name); }
#endif
HDC CreateCompatibleDC(HDC) { return new GdiDc(); }
BOOL DeleteDC(HDC value) { delete static_cast<GdiDc *>(value); return TRUE; }
HGDIOBJ SelectObject(HDC dcRaw, HGDIOBJ objectRaw)
{
    if (dcRaw == NULL || objectRaw == NULL) return NULL;
    GdiDc *dc = static_cast<GdiDc *>(dcRaw);
    GdiObject *object = static_cast<GdiObject *>(objectRaw);
    if (object->kind == GdiObject::BITMAP)
    {
        GdiBitmap *old = dc->bitmap;
        dc->bitmap = static_cast<GdiBitmap *>(object);
        return old;
    }
    GdiFont *old = dc->font;
    dc->font = static_cast<GdiFont *>(object);
    return old;
}
BOOL DeleteObject(HGDIOBJ value) { delete static_cast<GdiObject *>(value); return TRUE; }
int SetBkMode(HDC, int mode) { return mode; }
COLORREF SetTextColor(HDC raw, COLORREF color) { GdiDc *dc = static_cast<GdiDc *>(raw); COLORREF old = dc->color; dc->color = color; return old; }
HFONT CreateFontA(int height, int, int, int, int weight, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR)
{
    const char *path = ResolveJapaneseFont();
    if (path == NULL) return new GdiFont();
    if (!TTF_WasInit() && TTF_Init() != 0)
    {
        fprintf(stderr, "th08-modern: SDL_ttf initialization failed: %s\n", TTF_GetError());
        return new GdiFont();
    }
#ifdef TH_PS4
    // TextHelper creates a font for every string it draws and deletes it right after. On the
    // console, opening msgothic.ttc (and the Latin font) that often is file I/O in the middle
    // of a frame, so each size and weight is opened once and the GDI objects share it.
    static std::map<std::pair<int, bool>, GdiFont *> fonts;
    const std::pair<int, bool> key(height < 0 ? -height : height, weight >= FW_SEMIBOLD);
    std::map<std::pair<int, bool>, GdiFont *>::iterator known = fonts.find(key);
    if (known == fonts.end())
    {
        TTF_Font *font = TTF_OpenFont(path, key.first);
        if (font == NULL)
        {
            fprintf(stderr, "th08-modern: unable to load Japanese font %s: %s\n", path, TTF_GetError());
            return new GdiFont();
        }
        if (key.second) TTF_SetFontStyle(font, TTF_STYLE_BOLD);
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
        GdiFont *master = new GdiFont(font);
        OpenLatinFont(master, key.first);
        master->shared = true;
        known = fonts.insert(std::make_pair(key, master)).first;
    }
    GdiFont *result = new GdiFont();
    result->font = known->second->font;
    result->latin = known->second->latin;
    result->latinOffsetY = known->second->latinOffsetY;
    result->shared = true;
    return result;
#else
    TTF_Font *font = TTF_OpenFont(path, height < 0 ? -height : height);
    if (font == NULL)
    {
        fprintf(stderr, "th08-modern: unable to load Japanese font %s: %s\n", path, TTF_GetError());
        return new GdiFont();
    }
    if (weight >= FW_SEMIBOLD) TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    return new GdiFont(font);
#endif
}
HBITMAP CreateDIBSection(HDC, const void *infoRaw, UINT, VOID **pixels, HANDLE, DWORD)
{
    const BITMAPINFO *info = static_cast<const BITMAPINFO *>(infoRaw); GdiBitmap *bitmap = new GdiBitmap(info->bmiHeader.biWidth, info->bmiHeader.biHeight, info->bmiHeader.biBitCount);
    *pixels = bitmap->pixels.empty() ? NULL : &bitmap->pixels[0]; return bitmap;
}
BOOL TextOutA(HDC dcRaw, int x, int y, LPCSTR text, int length)
{
    if (dcRaw == NULL || text == NULL || length <= 0) return FALSE;
    GdiDc *dc = static_cast<GdiDc *>(dcRaw);
    if (dc->bitmap == NULL || dc->font == NULL || dc->font->font == NULL) return FALSE;
    std::string utf8 = ConvertCp932ToUtf8(text, static_cast<size_t>(length));
    SDL_Color white = {255, 255, 255, 255};
    TTF_Font *face = dc->font->font;
    int fitRight = 0;
#ifdef TH_PS4
    fitRight = g_Ps4TextFitRight;
#endif
#ifdef TH_PS4
    // Translated text is plain ASCII: draw it with the patch's Latin font, as thcrap does.
    // Anything with Japanese in it stays with the Japanese font, which has every glyph.
    bool ascii = dc->font->latin != NULL;
    for (int i = 0; ascii && i < length; ++i)
        if (static_cast<unsigned char>(text[i]) >= 0x80) ascii = false;
    if (ascii)
    {
        face = dc->font->latin;
        y += dc->font->latinOffsetY;
    }
#endif
    SDL_Surface *rendered = TTF_RenderUTF8_Blended(face, utf8.c_str(), white);
    if (rendered == NULL) return FALSE;
    SDL_Surface *glyph = SDL_ConvertSurfaceFormat(rendered, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(rendered);
    if (glyph == NULL) return FALSE;
    if (SDL_MUSTLOCK(glyph)) SDL_LockSurface(glyph);
    // A line that would run past the right edge of the sprite it fills is narrowed to fit,
    // as thcrap does with long translations, instead of being cut off by the sprite. Each
    // narrowed column averages the source columns it covers.
    int drawWidth = glyph->w;
    if (fitRight > 0 && x + glyph->w > fitRight - 2 && fitRight - 2 - x > 0) drawWidth = fitRight - 2 - x;
    for (int row = 0; row < glyph->h; ++row)
    {
        const BYTE *source = static_cast<const BYTE *>(glyph->pixels) + row * glyph->pitch;
        if (drawWidth == glyph->w)
        {
            for (int column = 0; column < glyph->w; ++column)
                PutGdiTextPixel(dc->bitmap, x + column, y + row, dc->color, source[column * 4 + 3]);
            continue;
        }
        for (int column = 0; column < drawWidth; ++column)
        {
            const int first = column * glyph->w / drawWidth;
            int last = (column + 1) * glyph->w / drawWidth;
            if (last <= first) last = first + 1;
            int sum = 0;
            for (int s = first; s < last; ++s) sum += source[s * 4 + 3];
            PutGdiTextPixel(dc->bitmap, x + column, y + row, dc->color, static_cast<BYTE>(sum / (last - first)));
        }
    }
    if (SDL_MUSTLOCK(glyph)) SDL_UnlockSurface(glyph);
    SDL_FreeSurface(glyph);
    return TRUE;
}
HRESULT CoInitialize(LPVOID) { return S_OK; }
void CoUninitialize(void) {}
HRESULT CoCreateInstance(REFGUID, LPVOID, DWORD, REFIID, LPVOID *) { return E_NOTIMPL; }
} // extern C

extern const GUID CLSID_ShellLink = {0};
extern const GUID IID_IShellLink = {1};
extern const GUID IID_IPersistFile = {2};
const GUID GUID_NULL = {0};
const GUID IID_IDirectSoundNotify = {3};
const GUID IID_IDirectInput8A = {4};
const GUID GUID_SysKeyboard = {5};
const GUID DIPROP_RANGE = {6};
const DIDATAFORMAT c_dfDIKeyboard = {sizeof(DIDATAFORMAT)};
const DIDATAFORMAT c_dfDIJoystick = {sizeof(DIDATAFORMAT)};

MMRESULT timeGetDevCaps(TIMECAPS *caps, UINT) { caps->wPeriodMin = 1; caps->wPeriodMax = 1000; return 0; }
MMRESULT timeBeginPeriod(UINT) { return 0; }
MMRESULT timeEndPeriod(UINT) { return 0; }
UINT timeSetEvent(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT) { static UINT id = 1; return id++; }
MMRESULT timeKillEvent(UINT) { return 0; }
MMRESULT midiOutOpen(HMIDIOUT *handle, UINT, DWORD_PTR, DWORD_PTR, DWORD) { *handle = reinterpret_cast<HMIDIOUT>(1); return 0; }
MMRESULT midiOutClose(HMIDIOUT) { return 0; }
MMRESULT midiOutReset(HMIDIOUT) { return 0; }
MMRESULT midiOutPrepareHeader(HMIDIOUT, LPMIDIHDR, UINT) { return 0; }
MMRESULT midiOutUnprepareHeader(HMIDIOUT, LPMIDIHDR, UINT) { return 0; }
MMRESULT midiOutLongMsg(HMIDIOUT, LPMIDIHDR header, UINT) { header->dwFlags |= 1; return 0; }
MMRESULT midiOutShortMsg(HMIDIOUT, DWORD) { return 0; }
MMRESULT joyGetPosEx(UINT, JOYINFOEX *) { return 1; }
MMRESULT joyGetDevCapsA(UINT_PTR, JOYCAPSA *caps, UINT) { memset(caps, 0, sizeof(*caps)); caps->wXmax = caps->wYmax = 65535; return 1; }

#ifdef TH_PS4
// The DualShock 4 is the real input device on this platform, so the joystick half of the
// DirectInput surface is implemented for it rather than stubbed out. Filling rgbButtons in
// this order makes TH08's stock controller mapping (shot 0, bomb 1, focus 2, skip 3,
// menu 4) land on the buttons the other two ports already use:
//
//   Cross shoot, Circle bomb, R1 focus, Triangle skip, Options pause.
//
// Keeping focus on its own button also keeps it off the shot button, which is what the
// game's own "shot = slow" option would otherwise do.
enum Ds4DirectInputButton
{
    DS4_BUTTON_SHOOT,  // Cross
    DS4_BUTTON_BOMB,   // Circle
    DS4_BUTTON_FOCUS,  // R1
    DS4_BUTTON_SKIP,   // Triangle
    DS4_BUTTON_MENU,   // Options
    DS4_BUTTON_SQUARE,
    DS4_BUTTON_L1,
    DS4_BUTTON_COUNT
};

SDL_GameController *g_controller;

SDL_GameController *EnsureController()
{
    if (g_controller != NULL && SDL_GameControllerGetAttached(g_controller)) return g_controller;
    if (g_controller != NULL)
    {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
    }
    for (int index = 0; index < SDL_NumJoysticks(); ++index)
    {
        if (!SDL_IsGameController(index)) continue;
        g_controller = SDL_GameControllerOpen(index);
        if (g_controller != NULL)
        {
            PS4_Log("pad: opened %s", SDL_GameControllerName(g_controller));
            break;
        }
    }
    return g_controller;
}

// Matches the -1000..1000 range TH08 asks DirectInput for in Supervisor::ControllerCallback.
LONG AxisToDirectInput(Sint16 value)
{
    const LONG scaled = static_cast<LONG>(value) * 1000 / 32767;
    return scaled < -1000 ? -1000 : (scaled > 1000 ? 1000 : scaled);
}

void FillJoystick(DIJOYSTATE2 *state)
{
    memset(state, 0, sizeof(*state));
    for (int index = 0; index < 4; ++index) state->rgdwPOV[index] = 0xffffffffu;

    SDL_PumpEvents();
    SDL_GameController *pad = EnsureController();
    if (pad == NULL) return;

    state->lX = AxisToDirectInput(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX));
    state->lY = AxisToDirectInput(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY));

    // The D-pad has to move the player and the menus as well, so it feeds the same axes.
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) state->lX = -1000;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) state->lX = 1000;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) state->lY = -1000;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) state->lY = 1000;

    static const SDL_GameControllerButton kButtons[DS4_BUTTON_COUNT] = {
        SDL_CONTROLLER_BUTTON_A,             // Cross
        SDL_CONTROLLER_BUTTON_B,             // Circle
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, // R1
        SDL_CONTROLLER_BUTTON_Y,             // Triangle
        SDL_CONTROLLER_BUTTON_START,         // Options
        SDL_CONTROLLER_BUTTON_X,             // Square
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  // L1
    };
    for (int index = 0; index < DS4_BUTTON_COUNT; ++index)
        state->rgbButtons[index] = SDL_GameControllerGetButton(pad, kButtons[index]) ? 0x80 : 0;
}
#endif

class LinuxInputDevice : public IDirectInputDevice8A
{
  public:
    explicit LinuxInputDevice(bool keyboard_) : refs(1), keyboard(keyboard_) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT GetCapabilities(DIDEVCAPS *caps) { caps->dwAxes = keyboard ? 0 : 2; caps->dwButtons = keyboard ? 0 : 32; return S_OK; }
    HRESULT EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) { return S_OK; }
    HRESULT GetDeviceState(DWORD size, LPVOID data)
    {
        if (keyboard && size >= 256) FillKeyboard(static_cast<BYTE *>(data), true);
#ifdef TH_PS4
        else if (!keyboard && size >= sizeof(DIJOYSTATE2)) FillJoystick(static_cast<DIJOYSTATE2 *>(data));
#endif
        else memset(data, 0, size); return S_OK;
    }
    HRESULT SetDataFormat(const DIDATAFORMAT *) { return S_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
    HRESULT SetProperty(REFGUID, const DIPROPHEADER *) { return S_OK; }
    HRESULT Acquire() { return S_OK; }
    HRESULT Unacquire() { return S_OK; }
    HRESULT Poll() { return S_OK; }
  private:
    ULONG refs; bool keyboard;
};

class LinuxDirectInput : public IDirectInput8A
{
  public:
    LinuxDirectInput() : refs(1) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT CreateDevice(REFGUID guid, IDirectInputDevice8A **device, LPVOID)
    { *device = new LinuxInputDevice(guid.Data1 == GUID_SysKeyboard.Data1); return S_OK; }
#ifdef TH_PS4
    // Supervisor only ever creates its joystick from inside this enumeration, so the pad
    // has to be reported here for the game to open it at all.
    HRESULT EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA callback, LPVOID context, DWORD)
    {
        if (callback == NULL || EnsureController() == NULL) return S_OK;
        DIDEVICEINSTANCEA instance;
        memset(&instance, 0, sizeof(instance));
        instance.dwSize = sizeof(instance);
        instance.guidInstance.Data1 = 0x44533400u; // "DS4", anything but GUID_SysKeyboard
        instance.guidProduct = instance.guidInstance;
        SDL_strlcpy(instance.tszInstanceName, "PS4 Controller", MAX_PATH);
        SDL_strlcpy(instance.tszProductName, "PS4 Controller", MAX_PATH);
        callback(&instance, context);
        return S_OK;
    }
#else
    HRESULT EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) { return S_OK; }
#endif
  private: ULONG refs;
};

HRESULT DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID *out, LPVOID) { *out = new LinuxDirectInput(); return S_OK; }

class LinuxSoundBuffer;

SDL_AudioDeviceID g_audioDevice;
std::vector<LinuxSoundBuffer *> g_soundBuffers;

void LockAudio()
{
    if (g_audioDevice != 0) SDL_LockAudioDevice(g_audioDevice);
}

void UnlockAudio()
{
    if (g_audioDevice != 0) SDL_UnlockAudioDevice(g_audioDevice);
}

class LinuxSoundNotify : public IDirectSoundNotify
{
  public:
    explicit LinuxSoundNotify(LinuxSoundBuffer *buffer_) : refs(1), buffer(buffer_) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT SetNotificationPositions(DWORD count, const DSBPOSITIONNOTIFY *positions);
  private: ULONG refs; LinuxSoundBuffer *buffer;
};

class LinuxSoundBuffer : public IDirectSoundBuffer
{
  public:
    explicit LinuxSoundBuffer(const DSBUFFERDESC *desc)
        : refs(1), playing(false), looping(false), position(0), cursorFrame(0.0), volume(0), pan(0),
          hasFormat(false)
    {
        memset(&format, 0, sizeof(format));
        if (desc != NULL)
        {
            bytes.resize(desc->dwBufferBytes);
            if (desc->lpwfxFormat != NULL) { format = *desc->lpwfxFormat; hasFormat = true; }
        }
        LockAudio(); g_soundBuffers.push_back(this); UnlockAudio();
    }
    LinuxSoundBuffer(const LinuxSoundBuffer &other)
        : refs(1), bytes(other.bytes), playing(false), looping(false), position(0), cursorFrame(0.0),
          volume(other.volume), pan(other.pan), format(other.format), hasFormat(other.hasFormat)
    { LockAudio(); g_soundBuffers.push_back(this); UnlockAudio(); }
    ~LinuxSoundBuffer()
    {
        LockAudio();
        for (std::vector<LinuxSoundBuffer *>::iterator it = g_soundBuffers.begin(); it != g_soundBuffers.end(); ++it)
            if (*it == this) { g_soundBuffers.erase(it); break; }
        UnlockAudio();
    }
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT QueryInterface(REFIID, void **out) { *out = new LinuxSoundNotify(this); return S_OK; }
    HRESULT GetCurrentPosition(LPDWORD play, LPDWORD write)
    {
        LockAudio();
        if (play) *play = position;
        if (write) *write = position;
        UnlockAudio();
        return S_OK;
    }
    HRESULT GetStatus(LPDWORD status)
    { LockAudio(); *status = playing ? DSBSTATUS_PLAYING : 0; UnlockAudio(); return S_OK; }
    HRESULT Initialize(void *, const DSBUFFERDESC *) { return S_OK; }
    // DirectSound's Lock only hands out a region of this one buffer; it does not stop the
    // mixer, which keeps playing the part that was not locked. Holding the device lock from
    // Lock to Unlock instead stalled every other sound call for as long as the caller kept
    // the region -- and the BGM streamer keeps it across a read from thbgm.dat, so each
    // refill froze the game thread on its next sound effect. Only a resize, which moves the
    // storage the mixer reads, needs the lock.
    HRESULT Lock(DWORD offset, DWORD length, LPVOID *first, LPDWORD firstSize, LPVOID *second, LPDWORD secondSize, DWORD)
    {
        if (bytes.empty()) { LockAudio(); bytes.resize(length ? length : 1); UnlockAudio(); }
        offset %= bytes.size(); if (length == 0 || length > bytes.size()) length = bytes.size();
        DWORD contiguous = static_cast<DWORD>(bytes.size() - offset); if (contiguous > length) contiguous = length;
        *first = &bytes[offset]; *firstSize = contiguous; if (second) *second = length > contiguous ? &bytes[0] : NULL; if (secondSize) *secondSize = length - contiguous; return S_OK;
    }
    HRESULT Play(DWORD, DWORD, DWORD flags)
    { LockAudio(); playing = true; looping = (flags & DSBPLAY_LOOPING) != 0; UnlockAudio(); return S_OK; }
    HRESULT SetCurrentPosition(DWORD value)
    {
        LockAudio();
        position = bytes.empty() ? 0 : value % bytes.size();
        cursorFrame = FrameBytes() != 0 ? static_cast<double>(position / FrameBytes()) : 0.0;
        UnlockAudio();
        return S_OK;
    }
    HRESULT SetFormat(const WAVEFORMATEX *value)
    { if (value != NULL) { LockAudio(); format = *value; hasFormat = true; UnlockAudio(); } return S_OK; }
    HRESULT SetVolume(LONG value) { LockAudio(); volume = value; UnlockAudio(); return S_OK; }
    HRESULT SetPan(LONG value) { LockAudio(); pan = value; UnlockAudio(); return S_OK; }
    HRESULT Stop() { LockAudio(); playing = false; UnlockAudio(); return S_OK; }
    HRESULT Unlock(LPVOID, DWORD, LPVOID, DWORD) { return S_OK; }
    HRESULT Restore() { return S_OK; }
    void SetNotifications(DWORD count, const DSBPOSITIONNOTIFY *positions)
    {
        LockAudio();
        if (count == 0)
            notifications.clear();
        else
            notifications.assign(positions, positions + count);
        UnlockAudio();
    }
    void Mix(Sint16 *output, int outputFrames)
    {
        const DWORD frameBytes = FrameBytes();
        if (!playing || !hasFormat || bytes.empty() || frameBytes == 0 || format.wFormatTag != WAVE_FORMAT_PCM)
            return;
        const DWORD sourceFrames = static_cast<DWORD>(bytes.size() / frameBytes);
        if (sourceFrames == 0) return;
        const double step = static_cast<double>(format.nSamplesPerSec) / 44100.0;
        const float gain = volume <= DSBVOLUME_MIN ? 0.0f : powf(10.0f, static_cast<float>(volume) / 2000.0f);
        const float panValue = pan < -10000 ? -1.0f : pan > 10000 ? 1.0f : static_cast<float>(pan) / 10000.0f;
        const float leftGain = gain * (panValue > 0.0f ? 1.0f - panValue : 1.0f);
        const float rightGain = gain * (panValue < 0.0f ? 1.0f + panValue : 1.0f);
        const DWORD oldPosition = position;
        bool wrapped = false;

        for (int frame = 0; frame < outputFrames && playing; ++frame)
        {
            DWORD sourceFrame = static_cast<DWORD>(cursorFrame);
            if (sourceFrame >= sourceFrames)
            {
                if (!looping) { playing = false; break; }
                cursorFrame -= sourceFrames; sourceFrame = static_cast<DWORD>(cursorFrame); wrapped = true;
            }
            const BYTE *source = &bytes[sourceFrame * frameBytes];
            int left, right;
            if (format.wBitsPerSample == 8)
            {
                left = (static_cast<int>(source[0]) - 128) << 8;
                right = format.nChannels > 1 ? (static_cast<int>(source[1]) - 128) << 8 : left;
            }
            else if (format.wBitsPerSample == 16)
            {
                INT16 leftSample, rightSample;
                memcpy(&leftSample, source, sizeof(leftSample));
                if (format.nChannels > 1) memcpy(&rightSample, source + sizeof(INT16), sizeof(rightSample));
                else rightSample = leftSample;
                left = leftSample; right = rightSample;
            }
            else
                break;

            int mixedLeft = output[frame * 2] + static_cast<int>(left * leftGain);
            int mixedRight = output[frame * 2 + 1] + static_cast<int>(right * rightGain);
            if (mixedLeft < -32768) mixedLeft = -32768; else if (mixedLeft > 32767) mixedLeft = 32767;
            if (mixedRight < -32768) mixedRight = -32768; else if (mixedRight > 32767) mixedRight = 32767;
            output[frame * 2] = static_cast<Sint16>(mixedLeft);
            output[frame * 2 + 1] = static_cast<Sint16>(mixedRight);
            cursorFrame += step;
        }

        if (cursorFrame >= sourceFrames)
        {
            if (looping) { cursorFrame = fmod(cursorFrame, static_cast<double>(sourceFrames)); wrapped = true; }
            else { cursorFrame = sourceFrames; playing = false; }
        }
        position = static_cast<DWORD>(cursorFrame) * frameBytes;
        for (size_t index = 0; index < notifications.size(); ++index)
        {
            const DWORD offset = notifications[index].dwOffset;
            if ((!wrapped && oldPosition <= offset && position > offset) ||
                (wrapped && (offset >= oldPosition || offset < position)))
                SetEvent(notifications[index].hEventNotify);
        }
    }
  private:
    DWORD FrameBytes() const
    { return hasFormat && format.nBlockAlign != 0 ? format.nBlockAlign : 0; }
    ULONG refs;
    std::vector<BYTE> bytes;
    bool playing, looping;
    DWORD position;
    double cursorFrame;
    LONG volume, pan;
    WAVEFORMATEX format;
    bool hasFormat;
    std::vector<DSBPOSITIONNOTIFY> notifications;
};

HRESULT LinuxSoundNotify::SetNotificationPositions(DWORD count, const DSBPOSITIONNOTIFY *positions)
{ if (buffer == NULL || (count != 0 && positions == NULL)) return E_INVALIDARG; buffer->SetNotifications(count, positions); return S_OK; }

void AudioCallback(void *, Uint8 *stream, int length)
{
    memset(stream, 0, length);
    Sint16 *output = reinterpret_cast<Sint16 *>(stream);
    const int frames = length / (sizeof(Sint16) * 2);
    for (size_t index = 0; index < g_soundBuffers.size(); ++index)
        g_soundBuffers[index]->Mix(output, frames);
}

void EnsureAudio()
{
    if (g_audioDevice != 0) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    { fprintf(stderr, "th08-modern: SDL audio initialization failed: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec requested, obtained;
    memset(&requested, 0, sizeof(requested));
    requested.freq = 44100; requested.format = AUDIO_S16SYS; requested.channels = 2;
    requested.samples = 1024; requested.callback = AudioCallback;
    g_audioDevice = SDL_OpenAudioDevice(NULL, 0, &requested, &obtained, 0);
    if (g_audioDevice == 0)
    { fprintf(stderr, "th08-modern: SDL audio device unavailable: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(g_audioDevice, 0);
}

void ShutdownAudio()
{
    if (g_audioDevice == 0) return;
    SDL_CloseAudioDevice(g_audioDevice); g_audioDevice = 0;
}

class LinuxDirectSound : public IDirectSound8
{
  public:
    LinuxDirectSound() : refs(1) { EnsureAudio(); }
    ~LinuxDirectSound() { ShutdownAudio(); }
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT CreateSoundBuffer(const DSBUFFERDESC *desc, IDirectSoundBuffer **out, LPVOID) { *out = new LinuxSoundBuffer(desc); return S_OK; }
    HRESULT DuplicateSoundBuffer(IDirectSoundBuffer *source, IDirectSoundBuffer **out) { *out = new LinuxSoundBuffer(*static_cast<LinuxSoundBuffer *>(source)); return S_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
  private: ULONG refs;
};

HRESULT DirectSoundCreate8(const GUID *, LPDIRECTSOUND8 *out, LPVOID) { *out = new LinuxDirectSound(); return S_OK; }
