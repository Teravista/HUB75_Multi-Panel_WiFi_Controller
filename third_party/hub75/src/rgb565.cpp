#include "rgb565.hpp"

// Expands a graphics RGB565 pixel into an RGB888 color.
uint32_t ExpandDriverRGB565(uint16_t stored) {
    // PicoGraphics stores RGB565 words in the opposite byte order to wire RGB565.
    const uint16_t pixel = uint16_t((stored << BITS_PER_BYTE) | (stored >> BITS_PER_BYTE));
    const uint32_t red = pixel >> RGB565_RED_SHIFT;
    const uint32_t green = (pixel >> RGB565_GREEN_SHIFT) & RGB565_GREEN_MASK;
    const uint32_t blue = pixel & RGB565_BLUE_MASK;

    const uint32_t expandedRed =
        (red << RGB565_RED_BLUE_EXPAND_SHIFT) | (red >> RGB565_RED_BLUE_REPLICATE_SHIFT);
    const uint32_t expandedGreen =
        (green << RGB565_GREEN_EXPAND_SHIFT) | (green >> RGB565_GREEN_REPLICATE_SHIFT);
    const uint32_t expandedBlue =
        (blue << RGB565_RED_BLUE_EXPAND_SHIFT) | (blue >> RGB565_RED_BLUE_REPLICATE_SHIFT);

    return (expandedRed << RGB888_RED_SHIFT) | (expandedGreen << RGB888_GREEN_SHIFT) | expandedBlue;
}

// Maps a graphics RGB565 pixel through the driver's gamma lookup tables.
uint32_t PackDriverRGB565(uint16_t stored, const Driver565Tables &table) {
    const uint16_t pixel = uint16_t((stored << BITS_PER_BYTE) | (stored >> BITS_PER_BYTE));
    const uint32_t red = pixel >> RGB565_RED_SHIFT;
    const uint32_t green = (pixel >> RGB565_GREEN_SHIFT) & RGB565_GREEN_MASK;
    const uint32_t blue = pixel & RGB565_BLUE_MASK;

    return table.red[red] | table.green[green] | table.blue[blue];
}
