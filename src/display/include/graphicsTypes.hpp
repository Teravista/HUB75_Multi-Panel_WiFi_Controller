#pragma once
#include "config/defines.h"
#include "hub75.hpp"

#if CPU_FRAME_RGB565
using CpuGraphics = pimoroni::PicoGraphics_PenRGB565;
#else
using CpuGraphics = pimoroni::PicoGraphics_PenRGB888;
#endif
