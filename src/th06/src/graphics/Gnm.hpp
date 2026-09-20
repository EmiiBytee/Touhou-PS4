#pragma once

#ifdef TH_PS4_GNM

#include "graphics/GfxInterface.hpp"

// GPU backend for the PS4, talking to GNM directly. Piglet (the system's GL) only works in
// mini apps, which can't record gameplay; this one runs in a full game and is what makes the
// console's own hardware do the drawing.
namespace GnmBackend
{
GfxInterface *Init();
}

#endif // TH_PS4_GNM
