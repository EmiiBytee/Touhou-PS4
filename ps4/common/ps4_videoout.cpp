#include "ps4_videoout.hpp"

#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <cstring>
#include <vector>

void PS4_Log(const char *fmt, ...);

namespace
{
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
// Three buffers: while one frame waits for its vblank, the next can already be scaled.
constexpr int kNumBuffers = 3;
constexpr size_t kBufferSize = (size_t)kWidth * kHeight * 4;
constexpr size_t kAlignment = 2 * 1024 * 1024;
constexpr int kMemoryTypeWcGarlic = 3;     // SCE_KERNEL_WC_GARLIC: what scan-out reads from
constexpr int kProtCpuGpuReadWrite = 0x33; // CPU RW | GPU RW
constexpr uint32_t kPixelFormatA8R8G8B8Srgb = 0x80000000;
constexpr uint32_t kTilingLinear = 1;

int g_Video = -1;
OrbisKernelEqueue g_FlipQueue;
uint32_t *g_Buffers[kNumBuffers];
int g_Current;
int64_t g_FrameId;
// Per buffer: layout it was last cleared for (-1 = never), so the bars get blacked once.
int g_ClearedFor[kNumBuffers] = {-1, -1, -1};

// Bilinear scaling tables for the current layout: for each output column/row, the first of
// the two source texels and the weight of the second (0..256).
struct Layout
{
    int key = -1;
    int srcW = 0, srcH = 0, dstW = 0, offX = 0;
    std::vector<uint16_t> x0, y0;
    std::vector<uint16_t> fx, fy;
} g_Layout;

// Scratch rows for each scaling part (thread_local would need __cxa_thread_atexit_impl,
// which the PS4 runtime doesn't provide). Sized by BeginFrame on the main thread.
constexpr int kMaxParts = 16;
struct Scratch
{
    std::vector<uint32_t> hrow[2], out;
} g_Scratch[kMaxParts];

void BuildAxis(int src, int dst, std::vector<uint16_t> &i0, std::vector<uint16_t> &f)
{
    i0.resize(dst);
    f.resize(dst);
    for (int d = 0; d < dst; d++)
    {
        // Sample at output pixel centers, like GL_LINEAR.
        float s = (d + 0.5f) * src / dst - 0.5f;
        if (s < 0)
        {
            s = 0;
        }
        int i = (int)s;
        if (i > src - 2)
        {
            i = src - 2;
            s = (float)(src - 1);
        }
        i0[d] = (uint16_t)i;
        f[d] = (uint16_t)((s - i) * 256.0f + 0.5f);
    }
}

inline uint32_t Lerp2(uint32_t a, uint32_t b, uint32_t f)
{
    const uint32_t inv = 256 - f;
    const uint32_t rb = (((a & 0x00FF00FF) * inv + (b & 0x00FF00FF) * f) >> 8) & 0x00FF00FF;
    const uint32_t ag = (((a >> 8) & 0x00FF00FF) * inv + ((b >> 8) & 0x00FF00FF) * f) & 0xFF00FF00;
    return rb | ag;
}

void WaitForFlip(int64_t frameId)
{
    // Bounded: a missing flip event must never hang the game.
    for (int tries = 0; tries < 8; tries++)
    {
        OrbisVideoOutFlipStatus status;
        sceVideoOutGetFlipStatus(g_Video, &status);
        if (status.flipArg >= frameId)
        {
            return;
        }
        OrbisKernelEvent event;
        int count = 0;
        OrbisKernelUseconds timeoutUs = 20000;
        sceKernelWaitEqueue(g_FlipQueue, &event, 1, &count, &timeoutUs);
    }
    static int s_Reported;
    if (s_Reported++ < 5)
    {
        PS4_Log("videoout: flip %lld not seen", (long long)frameId);
    }
}
} // namespace

bool PS4_VideoOutInit()
{
    g_Video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (g_Video < 0)
    {
        PS4_Log("videoout: sceVideoOutOpen failed %#x", g_Video);
        return false;
    }
    if (sceKernelCreateEqueue(&g_FlipQueue, "th-ps4 flip") < 0 ||
        sceVideoOutAddFlipEvent(g_FlipQueue, g_Video, nullptr) < 0)
    {
        PS4_Log("videoout: flip queue setup failed");
        return false;
    }

    size_t total = (kBufferSize * kNumBuffers + kAlignment - 1) / kAlignment * kAlignment;
    off_t phys = 0;
    void *mem = nullptr;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, kAlignment, kMemoryTypeWcGarlic,
                                           &phys);
    if (rc < 0 || (rc = sceKernelMapDirectMemory(&mem, total, kProtCpuGpuReadWrite, 0, phys, kAlignment)) < 0)
    {
        PS4_Log("videoout: display memory allocation failed %#x", rc);
        return false;
    }
    std::memset(mem, 0, total);
    for (int i = 0; i < kNumBuffers; i++)
    {
        g_Buffers[i] = (uint32_t *)((uint8_t *)mem + i * kBufferSize);
    }

    OrbisVideoOutBufferAttribute attr;
    std::memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute(&attr, kPixelFormatA8R8G8B8Srgb, kTilingLinear, 0, kWidth, kHeight, kWidth);
    rc = sceVideoOutRegisterBuffers(g_Video, 0, (void *const *)g_Buffers, kNumBuffers, &attr);
    if (rc < 0)
    {
        PS4_Log("videoout: sceVideoOutRegisterBuffers failed %#x", rc);
        return false;
    }
    sceVideoOutSetFlipRate(g_Video, 0); // 60 Hz
    PS4_Log("videoout: ready (%dx%d, %d buffers)", kWidth, kHeight, kNumBuffers);
    return true;
}

void PS4_VideoOutBeginFrame(int srcW, int srcH, bool widescreen)
{
    // g_Current was last shown by flip g_FrameId - 2; it's off screen once g_FrameId - 1 is up.
    // This is also what paces the game to the display rate.
    if (g_Video >= 0 && g_FrameId >= 2)
    {
        WaitForFlip(g_FrameId - 1);
    }
    const int dstW = widescreen ? kWidth : kHeight * srcW / srcH;
    const int key = (widescreen ? 1 : 0) | (srcW << 1) | (srcH << 13);
    if (g_Layout.key != key)
    {
        g_Layout.key = key;
        g_Layout.srcW = srcW;
        g_Layout.srcH = srcH;
        g_Layout.dstW = dstW;
        g_Layout.offX = (kWidth - dstW) / 2;
        BuildAxis(srcW, dstW, g_Layout.x0, g_Layout.fx);
        BuildAxis(srcH, kHeight, g_Layout.y0, g_Layout.fy);
        for (Scratch &s : g_Scratch)
        {
            s.hrow[0].resize(dstW);
            s.hrow[1].resize(dstW);
            s.out.resize(dstW);
        }
    }
    // The pillarbox bars are never written by ScaleRows: black them once per layout.
    const int layout = widescreen ? 1 : 0;
    if (g_ClearedFor[g_Current] != layout)
    {
        std::memset(g_Buffers[g_Current], 0, kBufferSize);
        g_ClearedFor[g_Current] = layout;
    }
}

void PS4_VideoOutScaleRows(const uint32_t *src, int part, int parts)
{
    if (g_Video < 0 || part >= kMaxParts)
    {
        return;
    }
    const Layout &L = g_Layout;
    uint32_t *dst = g_Buffers[g_Current];
    const int yBegin = part * kHeight / parts;
    const int yEnd = (part + 1) * kHeight / parts;

    // Source rows interpolated horizontally, cached: each is reused by ~2 output rows.
    // Display memory is write-combined, so output rows are built here too and copied once.
    std::vector<uint32_t> *hrow = g_Scratch[part].hrow;
    std::vector<uint32_t> &out = g_Scratch[part].out;
    int hrowSrc[2] = {-1, -1};

    auto horizontal = [&](int srcY) -> const uint32_t * {
        for (int i = 0; i < 2; i++)
        {
            if (hrowSrc[i] == srcY)
            {
                return hrow[i].data();
            }
        }
        const int slot = hrowSrc[0] == srcY - 1 ? 1 : 0; // keep the row just above
        uint32_t *h = hrow[slot].data();
        const uint32_t *s = src + (size_t)srcY * L.srcW;
        for (int x = 0; x < L.dstW; x++)
        {
            const int i = L.x0[x];
            h[x] = Lerp2(s[i], s[i + 1], L.fx[x]);
        }
        hrowSrc[slot] = srcY;
        return h;
    };

    for (int y = yBegin; y < yEnd; y++)
    {
        const int sy = L.y0[y];
        const uint32_t fy = L.fy[y];
        const uint32_t *a = horizontal(sy);
        const uint32_t *b = horizontal(sy + 1);
        // horizontal() may have overwritten `a` while producing `b`: re-fetch.
        a = horizontal(sy);
        uint32_t *o = out.data();
        for (int x = 0; x < L.dstW; x++)
        {
            o[x] = Lerp2(a[x], b[x], fy) | 0xFF000000;
        }
        std::memcpy(dst + (size_t)y * kWidth + L.offX, o, L.dstW * 4);
    }
}

void PS4_VideoOutFlip()
{
    if (g_Video < 0)
    {
        return;
    }
    g_FrameId++;
    int rc = sceVideoOutSubmitFlip(g_Video, g_Current, ORBIS_VIDEO_OUT_FLIP_VSYNC, g_FrameId);
    if (rc < 0 && g_FrameId < 5)
    {
        PS4_Log("videoout: sceVideoOutSubmitFlip(%d) failed %#x", g_Current, rc);
    }
    if (g_FrameId == 1)
    {
        // A big app stays covered by the system's (here black) launch splash until it says
        // it's ready. SDL's PS4 video driver does this for Piglet builds; we must do it too.
        PS4_Log("videoout: first frame shown, hiding splash: %#x", sceSystemServiceHideSplashScreen());
    }
    g_Current = (g_Current + 1) % kNumBuffers;
}
