#pragma once

#include <cstddef>
#include <cstdint>

// The fixed descriptor fits eighteen rectangle records before its checksum.
#define PREPARED_FRAME_MAX_REGIONS 18

// Describes one packed rectangle in physical CPU-framebuffer coordinates.
struct PreparedFrameRegion {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
};

/**
    Describes the extra preview and display representations of a stored media item.

    The original media descriptor remains logical and unchanged. These regions describe
    panel-corrected, color-corrected CPU pixels packed rectangle by rectangle, retaining
    only image pixels rather than generated black borders. Storage validates the layout
    signature before allowing display consumers to copy these bytes without remapping.
*/
struct PreparedFrameLayout {
    bool present = false;
    bool displayPresent = false;
    bool compatible = false;
    uint8_t pixelBytes = 0;
    uint8_t regionCount = 0;
    uint32_t frameBytes = 0;
    uint32_t signature = 0;
    uint32_t previewChecksum = 0;
    uint64_t previewOffset = 0;
    uint64_t displayOffset = 0;
    PreparedFrameRegion regions[PREPARED_FRAME_MAX_REGIONS]{};
};
