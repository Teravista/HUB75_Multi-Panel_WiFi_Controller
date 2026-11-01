#pragma once

#include <cstddef>
#include <cstdint>

#ifndef PACKED_PANEL_HOT_CODE_IN_RAM
#define PACKED_PANEL_HOT_CODE_IN_RAM 1
#endif

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE && PACKED_PANEL_HOT_CODE_IN_RAM
#include <pico.h>
#define PACKED_PANEL_HOT_CODE(name) __no_inline_not_in_flash_func(name)
#else
#define PACKED_PANEL_HOT_CODE(name) name
#endif

#if defined(__GNUC__)
#define PACKED_PANEL_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define PACKED_PANEL_ALWAYS_INLINE inline
#endif

#define PACKED_PANEL_MAX_OUTPUTS 2u
#define PACKED_PANEL_ROW_HALVES 2u
#define PACKED_PANEL_PIXEL_BITS 6u
#define PACKED_PANEL_PIXELS_PER_WORD 5u
#define PACKED_PANEL_WORD_BITS 30u
#define PACKED_PANEL_PLANES_PER_GROUP (PACKED_PANEL_WORD_BITS / PACKED_PANEL_PIXEL_BITS)
#define PACKED_PANEL_PIXEL_MASK 63u
#define PACKED_PANEL_CHANNELS_PER_HALF 3u
#define PACKED_PANEL_GAMMA_VALUES 256u
#define PACKED_PANEL_565_RED_BLUE_VALUES 32u
#define PACKED_PANEL_565_GREEN_VALUES 64u
#define PACKED_PANEL_MAX_DEPTH 10u
#define PACKED_PANEL_GREEN_SHIFT 8u
#define PACKED_PANEL_RED_SHIFT 16u
#define PACKED_PANEL_BYTE_MASK 255u
#define PACKED_PANEL_565_RED_SHIFT 11u
#define PACKED_PANEL_565_GREEN_SHIFT 5u
#define PACKED_PANEL_565_GREEN_MASK 63u
#define PACKED_PANEL_565_RED_BLUE_MASK 31u
#define PACKED_PANEL_BYTE_BITS 8u
#define PACKED_PANEL_RAW_PIXEL_BYTES 3u
#define PACKED_PANEL_565_RED_BLUE_EXPAND_SHIFT 3u
#define PACKED_PANEL_565_RED_BLUE_REPEAT_SHIFT 2u
#define PACKED_PANEL_565_GREEN_EXPAND_SHIFT 2u
#define PACKED_PANEL_565_GREEN_REPEAT_SHIFT 4u
#define PACKED_PANEL_565_PIXEL_MASK 65535u
#define PACKED_PANEL_GREEN_BIT 1u
#define PACKED_PANEL_BLUE_BIT 2u
#define PACKED_PANEL_BGR_BLUE_BYTE 0u
#define PACKED_PANEL_BGR_GREEN_BYTE 1u
#define PACKED_PANEL_BGR_RED_BYTE 2u

// Rotate the first chain only in dual-output mode to preserve the portrait panel arrangement.
#ifndef PACKED_PANEL_DUAL_FIRST_OUTPUT_ROTATE_180
#define PACKED_PANEL_DUAL_FIRST_OUTPUT_ROTATE_180 1
#endif

#ifndef PACKED_PANEL_DATA_CLOCK_DIVIDER
#define PACKED_PANEL_DATA_CLOCK_DIVIDER 1.0f
#endif

/**
    Prepares compact, gamma-corrected output bits for one or two panel chains.

    The driver owns the two scan allocations and their publication boundary. This
    component borrows an unmodified physical graphics frame and writes each row's
    ten brightness planes directly into the inactive scan allocation. Five six-bit
    column pairs share a word. A small gamma-spreading table avoids repeating color
    conversion during scans; there is no third image buffer or change to SD media.
    Preparation runs on the foreground core before the driver publishes the frame.
    A dedicated RGB565 loop avoids format dispatch per pixel and can run from SRAM;
    graphics RGB888 and raw BGR888 retain the generic source path.
*/
class PackedPanelFrame {
public:
    enum class SourceFormat {
        GraphicsRgb565,
        GraphicsRgb888,
        Bgr888
    };

    struct Configuration {
        uint32_t frameWidth = 0;
        uint32_t frameHeight = 0;
        uint32_t outputCount = 1;
        uint32_t bitDepth = 0;
        const uint16_t* gamma = nullptr;
    };

    struct Source {
        const void* pixels = nullptr;
        SourceFormat format = SourceFormat::GraphicsRgb565;
    };

    // Initializes compact row geometry and the borrowed gamma mapping's spread table.
    bool Initialize(const Configuration& configuration);

    // Returns the required number of 32-bit words in one complete scan allocation.
    size_t GetFrameWordCount(void) const;

    // Returns the packed word count for one lane, row pair and brightness plane.
    uint32_t GetRowWordCount(void) const;

    // Writes all output planes into an unpublished, capacity-checked scan allocation.
    bool Prepare(volatile uint32_t* destination, size_t capacityWords, const Source& source) const;

private:
    uint32_t width_ = 0;
    uint32_t rows_ = 0;
    uint32_t columns_ = 0;
    uint32_t outputs_ = 1;
    uint32_t rowWords_ = 0;
    uint32_t depth_ = 0;
    size_t frameWords_ = 0;
    uint64_t gammaBits_[PACKED_PANEL_GAMMA_VALUES] = {};
    uint64_t redBlue565Bits_[PACKED_PANEL_565_RED_BLUE_VALUES] = {};
    uint64_t green565Bits_[PACKED_PANEL_565_GREEN_VALUES] = {};

    // Writes up to five brightness planes using native 32-bit shifts.
    static void WritePlaneGroup(volatile uint32_t* destination, size_t planeStride, uint32_t* pairBits, uint32_t planeCount);

    // Builds all scan planes from validated graphics RGB565 pixels.
    void PrepareRgb565(volatile uint32_t* destination, const uint16_t* pixels) const;

    // Converts one graphics RGB565 value into spaced gamma bits without format dispatch.
    uint64_t ReadRgb565PixelBits(uint16_t pixel) const;

    // Converts one RGB888 source pixel into gamma bits spaced across output planes.
    uint64_t ReadRgb888PixelBits(const Source& source, size_t index) const;

};
