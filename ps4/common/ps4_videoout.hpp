// Native display path for big-app builds (TH_PS4_BIG_APP): Piglet doesn't work outside
// mini apps, so frames rendered on the CPU are scaled and flipped through SceVideoOut.
#pragma once

#include <cstdint>

// Opens the main video output with 1920x1080 display buffers. False on failure.
bool PS4_VideoOutInit();

// Frame presentation in three steps, so scaling can be split across threads:
//  1. BeginFrame picks the free display buffer and the layout (4:3 pillarboxed, or stretched
//     to 16:9 when `widescreen`); main thread only.
//  2. ScaleRows scales (bilinear) a srcW x srcH ARGB8888 frame into output rows
//     [part * 1080 / parts, (part + 1) * 1080 / parts); thread-safe for distinct parts.
//  3. Flip queues the frame for the next vblank (BeginFrame waits for the buffer to free up).
void PS4_VideoOutBeginFrame(int srcW, int srcH, bool widescreen);
void PS4_VideoOutScaleRows(const uint32_t *src, int part, int parts);
void PS4_VideoOutFlip();
