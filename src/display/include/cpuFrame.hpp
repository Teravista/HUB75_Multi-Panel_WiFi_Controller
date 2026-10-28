#pragma once

#include <cstdint>

#ifndef CPU_FRAME_RGB565
#define CPU_FRAME_RGB565 0
#endif

namespace CpuFrame {
#if CPU_FRAME_RGB565
using Pixel = uint16_t;
#else
using Pixel = uint32_t;
#endif
constexpr unsigned PixelBytes = sizeof(Pixel);

// Packs 8-bit RGB channels into a 16-bit RGB565 pixel.
uint16_t PackRgb565Pixel(uint8_t r, uint8_t g, uint8_t b);

// Converts an RGB565 pixel to the graphics buffer's byte order.
uint16_t SwapRgb565BytesForGraphics(uint16_t pixel);

// Expands an RGB565 pixel into an RGB888 color.
uint32_t ExpandRgb565ToRgb888(uint16_t pixel);

// Writes an RGB color using the configured CPU framebuffer format.
void StoreRgbPixel(uint8_t* destination, uint8_t r, uint8_t g, uint8_t b);

// Reads a CPU framebuffer pixel as an RGB888 color.
uint32_t LoadRgbPixelAsRgb888(const uint8_t* source);

}
