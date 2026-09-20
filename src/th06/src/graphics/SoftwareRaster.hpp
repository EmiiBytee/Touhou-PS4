#pragma once

// Deferred, multi-threaded rasterizer behind the Software backend.
//
// Software::Draw()/Clear() only set up commands; they are rasterized later by a pool of
// threads, each owning interleaved blocks of framebuffer rows (so each pixel is written by
// exactly one thread, in submission order). Frames can be rasterized asynchronously while the
// game computes the next one.

#include "ZunColor.hpp"
#include "ZunMath.hpp"
#include "inttypes.hpp"
#include "graphics/GfxInterface.hpp"

#include <functional>
#include <vector>

struct RasterDiffuse
{
    f32 r, g, b, a;
};

struct RasterCmd
{
    bool isClear;

    // Clear
    u32 clearBits;
    ZunColor clearColor;
    f32 clearDepth;

    // Triangle: bounds, and every attribute at (xmin + 0.5, ymin + 0.5) plus per-pixel steps.
    i32 xmin, xmax, ymin, ymax;
    ZunVec3 w0, w_dx, w_dy;
    ZunVec2 uv0, uv_dx, uv_dy;
    f32 invw0, invw_dx, invw_dy;
    f32 ndcZ0, ndcZ_dx, ndcZ_dy;
    f32 fogZ0, fogZ_dx, fogZ_dy;
    RasterDiffuse dif0, dif_dx, dif_dy;
    // Orthographic triangles (all vertices share w) don't need a divide per pixel.
    bool affine;
    f32 affineClipW;
    // Fog changes some pixel of this triangle (false: skip the fog math entirely).
    bool fogActive;

    // Render state at the time of the draw.
    const u32 *texels;
    i32 texW, texH;
    bool useTexture, useDepthTest, depthMask, depthLequal, noVertexBuffer, blendInvSrcAlpha;
    bool bilinear;
    ColorOp colorOp;
    ZunColor textureFactor, fogColor;
    f32 fogFar, invFogDif, depthNear, depthDif;
};

namespace SoftwareRaster
{
void Start();
void Stop();

// Commands recorded for the frame being built.
std::vector<RasterCmd> &Queue();

// Waits for any asynchronous frame, then rasterizes the queue now (and clears it).
void FlushSync(u32 *color, f32 *depth);

// Hands the queue to the workers and returns immediately. After rasterizing, every worker
// calls postRaster(index, count) (e.g. to scale its share of the frame to the screen).
void SubmitAsync(u32 *color, f32 *depth, std::function<void(i32, i32)> postRaster);

// Waits for the asynchronous frame, if any. Returns how long it took to finish (us).
u64 WaitAsync();
// Wall-clock microseconds. (On the PS4, std::chrono::steady_clock turned out to measure
// process CPU time, summed over every thread.)
u64 NowUs();
// Of the last WaitAsync() time, how long rasterizing took (the rest is postRaster).
u64 LastAsyncRasterUs();
// Pixel counters and per-worker raster time accumulated since the last call.
void TakeStats(u64 &walked, u64 &shaded, u64 &slotUsMin, u64 &slotUsMax);
} // namespace SoftwareRaster
