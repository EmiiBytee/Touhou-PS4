#include "graphics/SoftwareRaster.hpp"

#include "GameWindow.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef TH_PS4
#include <orbis/libkernel.h>
void PS4_Log(const char *fmt, ...);
#endif

namespace
{
constexpr i32 kRowBlock = 8;
constexpr u8 kAlphaThreshold = 4;

inline u8 ChA(ZunColor c)
{
    return c >> 24;
}
inline u8 ChR(ZunColor c)
{
    return (c >> 16) & 0xFF;
}
inline u8 ChG(ZunColor c)
{
    return (c >> 8) & 0xFF;
}
inline u8 ChB(ZunColor c)
{
    return c & 0xFF;
}
inline ZunColor Pack(u32 r, u32 g, u32 b, u32 a)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Per-channel lerp of two ARGB colors, f in [0, 256].
inline u32 Lerp2(u32 a, u32 b, u32 f)
{
    const u32 inv = 256 - f;
    const u32 rb = (((a & 0x00FF00FF) * inv + (b & 0x00FF00FF) * f) >> 8) & 0x00FF00FF;
    const u32 ag = (((a >> 8) & 0x00FF00FF) * inv + ((b >> 8) & 0x00FF00FF) * f) & 0xFF00FF00;
    return rb | ag;
}

inline i32 FastFloor(f32 v)
{
    return (i32)(v + 32768.0f) - 32768;
}

// Per-slot counters for the perf log (padded so slots don't share cache lines).
struct alignas(64) SlotStats
{
    u64 walked; // pixels whose edge test ran
    u64 shaded; // pixels that reached the framebuffer write
    u64 us;     // time spent rasterizing
};
SlotStats g_Stats[16];

struct Frame
{
    const std::vector<RasterCmd> *commands;
    u32 *color;
    f32 *depth;
    i32 slots;          // threads sharing the rows
    bool mainTakesSlot0; // sync flushes: the calling thread rasterizes slot 0
    std::function<void(i32, i32)> postRaster;
};

// ---------------------------------------------------------------------------------------
// Pixel pipeline, specialized on the per-triangle features that matter for speed.
// ---------------------------------------------------------------------------------------
template <bool kTex, bool kAffine, bool kFog, bool kDepth>
void RasterRow(const RasterCmd &c, i32 y, u32 *fbRow, f32 *dbRow, SlotStats &stats)
{
    const f32 dy = (f32)(y - c.ymin);
    const ZunVec3 wRow = c.w0 + c.w_dy * dy;

    // Walk only the covered part of the row (+1 pixel of slack each side; the exact
    // per-pixel edge test below decides). Narrow triangles just use their bounds.
    i32 xs = c.xmin, xe = c.xmax;
    if (xe - xs > 8)
    {
        f32 lo = 0.0f, hi = (f32)(c.xmax - c.xmin);
        const f32 we[3] = {wRow.x, wRow.y, wRow.z};
        const f32 wd[3] = {c.w_dx.x, c.w_dx.y, c.w_dx.z};
        for (i32 e = 0; e < 3; e++)
        {
            if (wd[e] > 0.0f)
            {
                lo = std::max(lo, -we[e] / wd[e]);
            }
            else if (wd[e] < 0.0f)
            {
                hi = std::min(hi, -we[e] / wd[e]);
            }
            else if (we[e] < 0.0f)
            {
                return;
            }
        }
        if (!(lo <= hi + 1.0f))
        {
            return;
        }
        xs = c.xmin + std::max(0, (i32)std::floor(lo) - 1);
        xe = c.xmin + std::min(c.xmax - c.xmin, (i32)std::ceil(hi) + 1);
    }
    const f32 dx = (f32)(xs - c.xmin);
    stats.walked += xe - xs + 1;

    ZunVec3 w = wRow + c.w_dx * dx;
    ZunVec2 uv = c.uv0 + c.uv_dy * dy + c.uv_dx * dx;
    f32 invw = c.invw0 + c.invw_dy * dy + c.invw_dx * dx;
    f32 ndcZ = c.ndcZ0 + c.ndcZ_dy * dy + c.ndcZ_dx * dx;
    f32 fogZ = c.fogZ0 + c.fogZ_dy * dy + c.fogZ_dx * dx;
    RasterDiffuse dif = {c.dif0.r + c.dif_dy.r * dy + c.dif_dx.r * dx, c.dif0.g + c.dif_dy.g * dy + c.dif_dx.g * dx,
                         c.dif0.b + c.dif_dy.b * dy + c.dif_dx.b * dx, c.dif0.a + c.dif_dy.a * dy + c.dif_dx.a * dx};
    const bool needDiffuse = !kTex || c.noVertexBuffer;
    const i32 texW = c.texW, texH = c.texH;
    // GL_REPEAT. Textures aren't always powers of two (portraits, text), so no masking.
    const auto wrapU = [texW](i32 u) { return (u32)u < (u32)texW ? u : ((u % texW) + texW) % texW; };
    const auto wrapV = [texH](i32 v) { return (u32)v < (u32)texH ? v : ((v % texH) + texH) % texH; };

    for (i32 x = xs; x <= xe; x++, w += c.w_dx, uv += c.uv_dx, invw += c.invw_dx, ndcZ += c.ndcZ_dx,
             fogZ += c.fogZ_dx, dif.r += c.dif_dx.r, dif.g += c.dif_dx.g, dif.b += c.dif_dx.b, dif.a += c.dif_dx.a)
    {
        if (!(w.x >= 0 && w.y >= 0 && w.z >= 0))
        {
            continue;
        }
        const f32 clipW = kAffine ? c.affineClipW : 1.0f / invw;
        f32 depth = 0;
        if (kDepth)
        {
            depth = ((ndcZ * clipW) * 0.5f + 0.5f) * c.depthDif + c.depthNear;
            if (c.depthLequal && depth > dbRow[x])
            {
                continue;
            }
        }

        ZunColor diffuse = 0;
        if (needDiffuse)
        {
            diffuse = Pack((u8)(dif.r * clipW), (u8)(dif.g * clipW), (u8)(dif.b * clipW), (u8)(dif.a * clipW));
        }

        ZunColor fragArg1;
        if (kTex)
        {
            if (c.bilinear)
            {
                // GL_LINEAR: sample centers are at texel centers, wrap addressing.
                const f32 u = uv.x * clipW - 0.5f, v = uv.y * clipW - 0.5f;
                const i32 iu = FastFloor(u), iv = FastFloor(v);
                const u32 fu = (u32)((u - (f32)iu) * 256.0f), fv = (u32)((v - (f32)iv) * 256.0f);
                const u32 *row0 = c.texels + wrapV(iv) * texW;
                const u32 *row1 = c.texels + wrapV(iv + 1) * texW;
                const i32 u0 = wrapU(iu), u1 = wrapU(iu + 1);
                fragArg1 = Lerp2(Lerp2(row0[u0], row0[u1], fu), Lerp2(row1[u0], row1[u1], fu), fv);
            }
            else
            {
                const i32 u = uv.x * clipW, v = uv.y * clipW;
                fragArg1 = c.texels[wrapV(v) * texW + wrapU(u)];
            }
        }
        else
        {
            fragArg1 = diffuse;
        }
        const ZunColor fragArg2 = c.noVertexBuffer ? diffuse : c.textureFactor;

        ZunColor frag;
        switch (c.colorOp)
        {
        case COLOR_OP_MODULATE:
            frag = Pack((ChR(fragArg1) * ChR(fragArg2)) >> 8, (ChG(fragArg1) * ChG(fragArg2)) >> 8,
                        (ChB(fragArg1) * ChB(fragArg2)) >> 8, (ChA(fragArg1) * ChA(fragArg2)) >> 8);
            break;
        case COLOR_OP_ADD:
            // Saturating, like D3DTOP_ADD / GL_ADD.
            frag = Pack(std::min(ChR(fragArg1) + ChR(fragArg2), 255), std::min(ChG(fragArg1) + ChG(fragArg2), 255),
                        std::min(ChB(fragArg1) + ChB(fragArg2), 255), (ChA(fragArg1) * ChA(fragArg2)) >> 8);
            break;
        default:
            frag = fragArg1;
        }

        const u8 srcA = ChA(frag);
        if (srcA < kAlphaThreshold)
        {
            continue;
        }

        if (kFog)
        {
            const f32 fogCoefficient = (c.fogFar - fogZ * clipW) * c.invFogDif;
            const u32 t = 255 - (u32)(std::min(std::max(fogCoefficient, 0.0f), 1.0f) * 255.0f);
            frag = (Lerp2(frag, c.fogColor, t + (t >> 7)) & 0x00FFFFFF) | ((u32)srcA << 24);
        }

        stats.shaded++;
        if (kDepth && c.depthMask)
        {
            dbRow[x] = depth;
        }
        const ZunColor dst = fbRow[x];
        const u32 dstF = c.blendInvSrcAlpha ? 255 - srcA : 255;
        fbRow[x] = Pack(std::min((ChR(frag) * srcA + ChR(dst) * dstF + 128) >> 8, 255u),
                        std::min((ChG(frag) * srcA + ChG(dst) * dstF + 128) >> 8, 255u),
                        std::min((ChB(frag) * srcA + ChB(dst) * dstF + 128) >> 8, 255u), srcA);
    }
}

template <bool kTex, bool kAffine, bool kFog, bool kDepth>
void RasterTriangle(const RasterCmd &c, i32 slot, i32 slots, u32 *fb, f32 *db)
{
    const i32 firstBlock = c.ymin / kRowBlock;
    const i32 lastBlock = c.ymax / kRowBlock;
    for (i32 block = firstBlock + ((slot - firstBlock % slots) + slots) % slots; block <= lastBlock;
         block += slots)
    {
        const i32 yStart = std::max(c.ymin, block * kRowBlock);
        const i32 yEnd = std::min(c.ymax, block * kRowBlock + kRowBlock - 1);
        for (i32 y = yStart; y <= yEnd; y++)
        {
            RasterRow<kTex, kAffine, kFog, kDepth>(c, y, fb + y * GAME_WINDOW_WIDTH, db + y * GAME_WINDOW_WIDTH,
                                                   g_Stats[slot]);
        }
    }
}

using RasterFn = void (*)(const RasterCmd &, i32, i32, u32 *, f32 *);

template <bool kTex, bool kAffine, bool kFog>
constexpr RasterFn PickDepth(bool depth)
{
    return depth ? RasterTriangle<kTex, kAffine, kFog, true> : RasterTriangle<kTex, kAffine, kFog, false>;
}

RasterFn PickKernel(const RasterCmd &c)
{
    if (c.useTexture)
    {
        if (c.affine)
        {
            return c.fogActive ? PickDepth<true, true, true>(c.useDepthTest)
                               : PickDepth<true, true, false>(c.useDepthTest);
        }
        return c.fogActive ? PickDepth<true, false, true>(c.useDepthTest)
                           : PickDepth<true, false, false>(c.useDepthTest);
    }
    if (c.affine)
    {
        return c.fogActive ? PickDepth<false, true, true>(c.useDepthTest)
                           : PickDepth<false, true, false>(c.useDepthTest);
    }
    return c.fogActive ? PickDepth<false, false, true>(c.useDepthTest)
                       : PickDepth<false, false, false>(c.useDepthTest);
}

void RasterizeSlot(const Frame &f, i32 slot)
{
    struct Timer
    {
        u64 start;
        i32 slot;
        ~Timer()
        {
            g_Stats[slot].us += SoftwareRaster::NowUs() - start;
        }
    } timer{SoftwareRaster::NowUs(), slot};
    for (const RasterCmd &c : *f.commands)
    {
        if (!c.isClear)
        {
            PickKernel(c)(c, slot, f.slots, f.color, f.depth);
            continue;
        }
        for (i32 block = slot; block * kRowBlock < GAME_WINDOW_HEIGHT; block += f.slots)
        {
            const i32 y0 = block * kRowBlock;
            const i32 y1 = std::min(y0 + kRowBlock, (i32)GAME_WINDOW_HEIGHT);
            if (c.clearBits & CLEAR_COLOR_BUFFER)
            {
                std::fill(f.color + y0 * GAME_WINDOW_WIDTH, f.color + y1 * GAME_WINDOW_WIDTH, c.clearColor);
            }
            if (c.clearBits & CLEAR_DEPTH_BUFFER)
            {
                std::fill(f.depth + y0 * GAME_WINDOW_WIDTH, f.depth + y1 * GAME_WINDOW_WIDTH, c.clearDepth);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------
// Worker pool
// ---------------------------------------------------------------------------------------
std::vector<RasterCmd> g_Queues[2];
i32 g_Recording;

std::vector<std::thread> g_Workers;
std::mutex g_Mutex;
std::condition_variable g_WorkReady, g_WorkDone;
u32 g_Generation;
i32 g_Pending;
bool g_Quit;
bool g_AsyncInFlight;
Frame g_Frame;
std::atomic<i32> g_RasterArrived;
u64 g_SubmitTime;
u64 g_LastAsyncUs;
u64 g_LastRasterUs; // submit -> every row rasterized (the rest of g_LastAsyncUs is scaling)

u64 MicrosSinceSubmit()
{
    return SoftwareRaster::NowUs() - g_SubmitTime;
}

void PinToCore(i32 core, const char *what)
{
#ifdef TH_PS4
    // Threads inherit their creator's affinity: without this every worker can end up
    // sharing the game thread's core, and nothing runs in parallel.
    // OpenOrbis declares these without their real signatures.
    const auto getAffinity = reinterpret_cast<int (*)(OrbisPthread, uint64_t *)>(&scePthreadGetaffinity);
    const auto setAffinity = reinterpret_cast<int (*)(OrbisPthread, uint64_t)>(&scePthreadSetaffinity);
    const OrbisPthread self = scePthreadSelf();
    uint64_t before = 0;
    getAffinity(self, &before);
    const int rc = setAffinity(self, 1ull << core);
    uint64_t after = 0;
    getAffinity(self, &after);
    PS4_Log("raster: %s affinity %#llx -> core %d (rc %#x, now %#llx)", what, (unsigned long long)before, core, rc,
            (unsigned long long)after);
#else
    (void)core;
    (void)what;
#endif
}

void WorkerMain(i32 worker)
{
    PinToCore(worker + 1, "worker");
    u32 seen = 0;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(g_Mutex);
            g_WorkReady.wait(lock, [&] { return g_Quit || g_Generation != seen; });
            if (g_Quit)
            {
                return;
            }
            seen = g_Generation;
        }
        const Frame &f = g_Frame;
        const i32 slot = f.mainTakesSlot0 ? worker + 1 : worker;
        if (slot < f.slots)
        {
            RasterizeSlot(f, slot);
            if (f.postRaster)
            {
                // Every row must be rasterized before anyone reads the framebuffer.
                if (g_RasterArrived.fetch_add(1) == f.slots - 1)
                {
                    g_LastRasterUs = MicrosSinceSubmit();
                }
                while (g_RasterArrived.load() < f.slots)
                {
                    std::this_thread::yield();
                }
                f.postRaster(slot, f.slots);
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (--g_Pending == 0)
            {
                g_LastAsyncUs = MicrosSinceSubmit();
                g_WorkDone.notify_all();
            }
        }
    }
}

void Dispatch(const Frame &f)
{
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Frame = f;
        g_RasterArrived = 0;
        g_Pending = (i32)g_Workers.size();
        g_SubmitTime = SoftwareRaster::NowUs();
        g_Generation++;
    }
    g_WorkReady.notify_all();
}

void WaitWorkers()
{
    std::unique_lock<std::mutex> lock(g_Mutex);
    g_WorkDone.wait(lock, [] { return g_Pending == 0; });
}
} // namespace

namespace SoftwareRaster
{
u64 NowUs()
{
#ifdef TH_PS4
    return sceKernelGetProcessTime();
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

void Start()
{
    if (!g_Workers.empty())
    {
        return;
    }
#ifdef TH_PS4
    // A PS4 game gets 6 of the 8 Jaguar cores: the game thread plus 5 raster workers.
    const i32 workers = 5;
#else
    const i32 workers = std::max(1, (i32)std::min(8u, std::thread::hardware_concurrency()) - 1);
#endif
    PinToCore(0, "game thread");
#ifdef TH_PS4
    {
        // How fast is one core at the rasterizer's bread and butter?
        static u32 buf[4096];
        for (u32 i = 0; i < 4096; i++)
        {
            buf[i] = i * 0x9E3779B9u;
        }
        const auto t0 = SoftwareRaster::NowUs();
        u32 acc = 0;
        for (u32 n = 0; n < 1000000; n++)
        {
            acc += Lerp2(buf[n & 4095], buf[(n * 7) & 4095], n & 255);
        }
        const auto t1 = SoftwareRaster::NowUs();
        volatile f32 facc = 1.0f;
        for (u32 n = 0; n < 1000000; n++)
        {
            facc = facc * 0.999f + 1.0f / (f32)(n + 1);
        }
        const auto t2 = SoftwareRaster::NowUs();
        PS4_Log("raster: bench 1M lerps %lld us, 1M float divs %lld us (acc %u)",
                (long long)(t1 - t0),
                (long long)(t2 - t1), acc);
    }
#endif
    g_Quit = false;
    for (i32 i = 0; i < workers; i++)
    {
        g_Workers.emplace_back(WorkerMain, i);
    }
}

void Stop()
{
    WaitAsync();
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Quit = true;
    }
    g_WorkReady.notify_all();
    for (std::thread &t : g_Workers)
    {
        t.join();
    }
    g_Workers.clear();
    g_Queues[0].clear();
    g_Queues[1].clear();
}

std::vector<RasterCmd> &Queue()
{
    return g_Queues[g_Recording];
}

u64 WaitAsync()
{
    if (!g_AsyncInFlight)
    {
        return 0;
    }
    WaitWorkers();
    g_AsyncInFlight = false;
    g_Queues[g_Recording ^ 1].clear();
    return g_LastAsyncUs;
}

u64 LastAsyncRasterUs()
{
    return g_LastRasterUs;
}

void TakeStats(u64 &walked, u64 &shaded, u64 &slotUsMin, u64 &slotUsMax)
{
    walked = shaded = slotUsMax = 0;
    slotUsMin = ~0ull;
    const i32 slots = (i32)g_Workers.size() + 1;
    for (i32 i = 0; i < slots; i++)
    {
        walked += g_Stats[i].walked;
        shaded += g_Stats[i].shaded;
        if (i < (i32)g_Workers.size()) // slot 5 only exists in sync flushes
        {
            slotUsMin = std::min(slotUsMin, g_Stats[i].us);
            slotUsMax = std::max(slotUsMax, g_Stats[i].us);
        }
        g_Stats[i] = {};
    }
}

void FlushSync(u32 *color, f32 *depth)
{
    WaitAsync();
    std::vector<RasterCmd> &q = g_Queues[g_Recording];
    if (q.empty())
    {
        return;
    }
    Frame f{&q, color, depth, (i32)g_Workers.size() + 1, true, nullptr};
    Dispatch(f);
    RasterizeSlot(f, 0);
    WaitWorkers();
    q.clear();
}

void SubmitAsync(u32 *color, f32 *depth, std::function<void(i32, i32)> postRaster)
{
    WaitAsync();
    Frame f{&g_Queues[g_Recording], color, depth, (i32)g_Workers.size(), false, std::move(postRaster)};
    g_Recording ^= 1;
    g_AsyncInFlight = true;
    Dispatch(f);
}
} // namespace SoftwareRaster
