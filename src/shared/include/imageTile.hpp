#pragma once

#include <cstddef>
#include <cstdint>

/**
    Borrows one tightly packed pixel rectangle inside a logical image.

    Coordinates are relative to the image before panel mapping or placement.
    The order identifies standard little-endian RGB565 or ready-to-store panel bytes. The producer
    owns the pixels and may reuse them as soon as the synchronous sink returns.
    A tile is provisional until the enclosing frame passes its integrity checks.
*/
struct ImageTile {
    enum class Order : uint8_t { Canonical, Panel };

    const uint8_t* pixels = nullptr;
    size_t bytes = 0;
    uint16_t left = 0;
    uint16_t top = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t pixelBytes = 0;
    Order order = Order::Canonical;

    using Sink = bool (*)(void* context, const ImageTile& tile);
};
