#pragma once

#include <array>
#include <cstdint>

#include "config/defines.h"

#if !defined(BIT_DEPTH)
#define BIT_DEPTH 10 // default
#endif

#if BIT_DEPTH != 8 && BIT_DEPTH != 10
#error "BIT_DEPTH must be 8 or 10"
#endif

#define DRIVER_RGB565_RED_BLUE_LEVEL_COUNT 32
#define DRIVER_RGB565_GREEN_LEVEL_COUNT 64
#define DRIVER_RGB888_LEVEL_COUNT 256
#define DRIVER_RGB_BLUE_CHANNEL_INDEX 2

struct Driver565Tables {
    std::array<uint32_t, DRIVER_RGB565_RED_BLUE_LEVEL_COUNT> red{};
    std::array<uint32_t, DRIVER_RGB565_RED_BLUE_LEVEL_COUNT> blue{};
    std::array<uint32_t, DRIVER_RGB565_GREEN_LEVEL_COUNT> green{};
};

// Expands a graphics RGB565 pixel into an RGB888 color.
uint32_t ExpandDriverRGB565(uint16_t stored);

// Builds packed gamma lookup tables for the driver's channel bit depth.
constexpr Driver565Tables MakeDriver565Tables(const uint16_t (&gamma)[DRIVER_RGB888_LEVEL_COUNT]) {
    Driver565Tables table;

    for (unsigned index = 0; index < DRIVER_RGB565_RED_BLUE_LEVEL_COUNT; ++index) {
        const unsigned expanded =
            (index << RGB565_RED_BLUE_EXPAND_SHIFT) | (index >> RGB565_RED_BLUE_REPLICATE_SHIFT);
        table.red[index] = gamma[expanded];
        table.blue[index] = uint32_t(gamma[expanded]) << (DRIVER_RGB_BLUE_CHANNEL_INDEX * BIT_DEPTH);
    }

    for (unsigned index = 0; index < DRIVER_RGB565_GREEN_LEVEL_COUNT; ++index) {
        const unsigned expanded =
            (index << RGB565_GREEN_EXPAND_SHIFT) | (index >> RGB565_GREEN_REPLICATE_SHIFT);
        table.green[index] = uint32_t(gamma[expanded]) << BIT_DEPTH;
    }

    return table;
}

// Maps a graphics RGB565 pixel through the driver's gamma lookup tables.
uint32_t PackDriverRGB565(uint16_t stored, const Driver565Tables &table);
