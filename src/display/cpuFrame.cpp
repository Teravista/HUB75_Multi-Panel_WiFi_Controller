#include "display/include/cpuFrame.hpp"

#include <cstring>
#include "config/defines.h"

namespace CpuFrame {
// Packs 8-bit RGB channels into a 16-bit RGB565 pixel.
uint16_t Pack565(uint8_t r, uint8_t g, uint8_t b) {
    return uint16_t((uint16_t(r & RGB565_RED_BLUE_SOURCE_MASK) << RGB565_RED_PACK_SHIFT) |
                    (uint16_t(g & RGB565_GREEN_SOURCE_MASK) << RGB565_GREEN_PACK_SHIFT) |
                    (b >> RGB565_RED_BLUE_REDUCE_SHIFT));
}

// Converts an RGB565 pixel to the graphics buffer's byte order.
uint16_t Graphics565(uint16_t pixel) { return uint16_t((pixel << BITS_PER_BYTE) | (pixel >> BITS_PER_BYTE)); }

// Expands an RGB565 pixel into an RGB888 color.
uint32_t Expand565(uint16_t pixel) {
    const uint32_t r = pixel >> RGB565_RED_SHIFT, g = (pixel >> RGB565_GREEN_SHIFT) & RGB565_GREEN_MASK,
                   b = pixel & RGB565_BLUE_MASK;
    return (((r << RGB565_RED_BLUE_EXPAND_SHIFT) | (r >> RGB565_RED_BLUE_REPLICATE_SHIFT))
            << RGB888_RED_SHIFT) |
           (((g << RGB565_GREEN_EXPAND_SHIFT) | (g >> RGB565_GREEN_REPLICATE_SHIFT)) << RGB888_GREEN_SHIFT) |
           ((b << RGB565_RED_BLUE_EXPAND_SHIFT) | (b >> RGB565_RED_BLUE_REPLICATE_SHIFT));
}

// Writes an RGB color using the configured CPU framebuffer format.
void Store(uint8_t* destination, uint8_t r, uint8_t g, uint8_t b) {
#if CPU_FRAME_RGB565
    const Pixel pixel = Graphics565(Pack565(r, g, b));
#else
    const Pixel pixel = (uint32_t(r) << RGB888_RED_SHIFT) | (uint32_t(g) << RGB888_GREEN_SHIFT) | b;
#endif
    std::memcpy(destination, &pixel, sizeof(pixel));
}

// Reads a CPU framebuffer pixel as an RGB888 color.
uint32_t Load(const uint8_t* source) {
    Pixel pixel;
    std::memcpy(&pixel, source, sizeof(pixel));
#if CPU_FRAME_RGB565
    return Expand565(Graphics565(pixel));
#else
    return pixel;
#endif
}

}
