#pragma once

#include <cstdint>

namespace CpuFrame {
using Pixel = uint16_t;
constexpr unsigned PixelBytes = sizeof(Pixel);

// Packs 8-bit RGB channels into a 16-bit RGB565 pixel.
uint16_t PackRgb565Pixel(uint8_t r, uint8_t g, uint8_t b);

// Converts an RGB565 pixel to the graphics buffer's byte order.
uint16_t SwapRgb565BytesForGraphics(uint16_t pixel);

// Expands an RGB565 pixel into separate eight-bit color channels.
uint32_t ExpandRgb565Channels(uint16_t pixel);

// Writes an RGB color into the RGB565 CPU framebuffer.
void StoreRgbPixel(uint8_t* destination, uint8_t r, uint8_t g, uint8_t b);

// Reads a CPU framebuffer pixel as separate eight-bit color channels.
uint32_t LoadRgbPixelChannels(const uint8_t* source);

}
