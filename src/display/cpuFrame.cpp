#include "display/include/cpuFrame.hpp"

#include <cstring>
#include "config/defines.h"

namespace CpuFrame {
/**
    Packs 8-bit RGB channels into a 16-bit RGB565 pixel.

    @param r Red channel intensity, from 0 to 255.
    @param g Green channel intensity, from 0 to 255.
    @param b Blue channel intensity, from 0 to 255.

    @return value - Packed 16-bit RGB565 color: five red, six green and five blue bits.
*/
uint16_t
PackRgb565Pixel (
    uint8_t r,
    uint8_t g,
    uint8_t b
    )
{
    return uint16_t((uint16_t(r & RGB565_RED_BLUE_SOURCE_MASK) << RGB565_RED_PACK_SHIFT) |
                    (uint16_t(g & RGB565_GREEN_SOURCE_MASK) << RGB565_GREEN_PACK_SHIFT) |
                    (b >> RGB565_RED_BLUE_REDUCE_SHIFT));
} // PackRgb565Pixel

/**
    Converts an RGB565 pixel to the graphics buffer's byte order.

    @param pixel Packed RGB565 pixel value.

    @return value - RGB565 word with its two bytes exchanged for graphics storage.
*/
uint16_t
SwapRgb565BytesForGraphics (
    uint16_t pixel
    )
{
    return uint16_t((pixel << BITS_PER_BYTE) | (pixel >> BITS_PER_BYTE));
} // SwapRgb565BytesForGraphics

/**
    Expands an RGB565 pixel into separate eight-bit color channels.

    @param pixel Packed RGB565 pixel value.

    @return value - Expanded channels encoded as 0x00RRGGBB.
*/
uint32_t
ExpandRgb565Channels (
    uint16_t pixel
    )
{
    uint32_t r;
    uint32_t g;
    uint32_t b;

    r = pixel >> RGB565_RED_SHIFT;
    g = (pixel >> RGB565_GREEN_SHIFT) & RGB565_GREEN_MASK;
    b = pixel & RGB565_BLUE_MASK;
    return (((r << RGB565_RED_BLUE_EXPAND_SHIFT) | (r >> RGB565_RED_BLUE_REPLICATE_SHIFT))
            << COLOR_RED_SHIFT) |
           (((g << RGB565_GREEN_EXPAND_SHIFT) | (g >> RGB565_GREEN_REPLICATE_SHIFT)) << COLOR_GREEN_SHIFT) |
           ((b << RGB565_RED_BLUE_EXPAND_SHIFT) | (b >> RGB565_RED_BLUE_REPLICATE_SHIFT));
} // ExpandRgb565Channels

/**
    Writes an RGB color into the RGB565 CPU framebuffer.

    Packs and stores one RGB565 pixel in graphics byte order. The caller-owned
    destination must provide two writable bytes.

    @param destination Writable destination buffer; the caller must provide sufficient storage.
    @param r Red channel intensity, from 0 to 255.
    @param g Green channel intensity, from 0 to 255.
    @param b Blue channel intensity, from 0 to 255.
*/
void
StoreRgbPixel (
    uint8_t* destination,
    uint8_t r,
    uint8_t g,
    uint8_t b
    )
{
    Pixel pixel;

    pixel = SwapRgb565BytesForGraphics(PackRgb565Pixel(r, g, b));
    std::memcpy(destination, &pixel, sizeof(pixel));
} // StoreRgbPixel

/**
    Reads a CPU framebuffer pixel as separate eight-bit color channels.

    @param source Read-only source bytes in the format required by this operation.

    @return value - The source CPU pixel expanded to 0x00RRGGBB.
*/
uint32_t
LoadRgbPixelChannels (
    const uint8_t* source
    )
{
    Pixel pixel;

    std::memcpy(&pixel, source, sizeof(pixel));
    return ExpandRgb565Channels(SwapRgb565BytesForGraphics(pixel));
} // LoadRgbPixelChannels

} // namespace CpuFrame
