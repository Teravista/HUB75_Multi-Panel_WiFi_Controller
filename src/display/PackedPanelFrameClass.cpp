#include <include/PackedPanelFrameClass.hpp>

#include <limits>

/**
    Initializes compact row geometry and the borrowed gamma mapping's spread table.

    Rows contain only physical image columns. Five six-bit values use thirty bits
    per word; unused fields in the final word are discarded without clocking them
    onto the panels. Gamma bits are spread once at
    startup so frame preparation can combine channels without per-plane lookups.

    @param configuration Physical frame dimensions, bit depth and gamma lookup.

    @return true - Geometry and gamma values fit the compact representation.
    @return false - Geometry, depth, gamma mapping or allocation arithmetic is invalid.
*/
bool
PackedPanelFrame::Initialize (
    const Configuration& configuration
    )
{
    uint32_t value;
    uint32_t plane;
    uint32_t expanded;
    uint64_t words;

    frameWords_ = 0;
    if (!configuration.gamma || configuration.frameWidth == 0 || configuration.frameHeight == 0 ||
        configuration.outputCount == 0 || configuration.outputCount > PACKED_PANEL_MAX_OUTPUTS ||
        configuration.frameWidth % configuration.outputCount != 0 ||
        configuration.frameHeight % PACKED_PANEL_ROW_HALVES != 0 ||
        configuration.bitDepth == 0 || configuration.bitDepth > PACKED_PANEL_MAX_DEPTH) {
        return false;
    }

    width_ = configuration.frameWidth;
    rows_ = configuration.frameHeight / PACKED_PANEL_ROW_HALVES;
    outputs_ = configuration.outputCount;
    columns_ = configuration.frameWidth / outputs_;
    depth_ = configuration.bitDepth;
    rowWords_ = uint32_t((uint64_t(columns_) +
        PACKED_PANEL_PIXELS_PER_WORD - 1u) / PACKED_PANEL_PIXELS_PER_WORD);
    words = uint64_t(rowWords_) * rows_ * outputs_;
    if (words > std::numeric_limits<size_t>::max() / sizeof(uint32_t) / depth_) {
        return false;
    }

    for (value = 0; value < PACKED_PANEL_GAMMA_VALUES; ++value) {
        if (configuration.gamma[value] >= (1u << depth_)) {
            return false;
        }
        gammaBits_[value] = 0;
        for (plane = 0; plane < depth_; ++plane) {
            if (configuration.gamma[value] & (1u << plane)) {
                gammaBits_[value] |= uint64_t(1) << (plane * PACKED_PANEL_PIXEL_BITS);
            }
        }
    }
    // Native RGB565 indices avoid expanding every pixel back to eight-bit channels.
    for (value = 0; value < PACKED_PANEL_565_RED_BLUE_VALUES; ++value) {
        expanded = (value << PACKED_PANEL_565_RED_BLUE_EXPAND_SHIFT) |
            (value >> PACKED_PANEL_565_RED_BLUE_REPEAT_SHIFT);
        redBlue565Bits_[value] = gammaBits_[expanded];
    }
    for (value = 0; value < PACKED_PANEL_565_GREEN_VALUES; ++value) {
        expanded = (value << PACKED_PANEL_565_GREEN_EXPAND_SHIFT) |
            (value >> PACKED_PANEL_565_GREEN_REPEAT_SHIFT);
        green565Bits_[value] = gammaBits_[expanded] << PACKED_PANEL_GREEN_BIT;
    }
    frameWords_ = size_t(words) * depth_;
    return true;
} // Initialize

/**
    Returns the required number of 32-bit words in one complete scan allocation.

    @return value - Allocation length in words, or zero before valid initialization.
*/
size_t
PackedPanelFrame::GetFrameWordCount (
    void
    ) const
{
    return frameWords_;
} // GetFrameWordCount

/**
    Returns the packed word count for one lane, row pair and brightness plane.

    @return value - Packed row length in words, including unclocked final-word padding.
*/
uint32_t
PackedPanelFrame::GetRowWordCount (
    void
    ) const
{
    return rowWords_;
} // GetRowWordCount

/**
    Converts one graphics RGB565 value into spaced gamma bits without format dispatch.

    The graphics word has swapped bytes. Its native five/six-bit channel indices
    select the existing gamma tables, retaining the generic path's exact result.
    Inlining this small reader keeps per-pixel work inside the hot packing loop.

    @param pixel One borrowed graphics-format RGB565 word.

    @return value - Gamma bits at six-bit intervals, with R/G/B in the lowest positions.
*/
PACKED_PANEL_ALWAYS_INLINE uint64_t
PackedPanelFrame::ReadRgb565PixelBits (
    uint16_t pixel
    ) const
{
    uint32_t color;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    color = ((uint32_t(pixel) << PACKED_PANEL_BYTE_BITS) | (pixel >> PACKED_PANEL_BYTE_BITS)) &
        PACKED_PANEL_565_PIXEL_MASK;
    red = color >> PACKED_PANEL_565_RED_SHIFT;
    green = (color >> PACKED_PANEL_565_GREEN_SHIFT) & PACKED_PANEL_565_GREEN_MASK;
    blue = color & PACKED_PANEL_565_RED_BLUE_MASK;
    return redBlue565Bits_[red] | green565Bits_[green] |
        (redBlue565Bits_[blue] << PACKED_PANEL_BLUE_BIT);
} // ReadRgb565PixelBits




/**
    Writes up to five brightness planes using native 32-bit shifts.

    Each workspace value contains one column pair's six output bits at consecutive
    plane positions. Extracting those values into a FIFO word uses only 32-bit
    operations. The next plane is separated by the complete row/output stride,
    retaining the scanner's existing memory layout.

    @param destination First plane's word in the inactive scan allocation.
    @param planeStride Word distance between consecutive brightness planes.
    @param pairBits Five mutable column-pair workspaces, advanced after each plane.
    @param planeCount Number of planes to extract, no greater than five.
*/
PACKED_PANEL_ALWAYS_INLINE void
PackedPanelFrame::WritePlaneGroup (
    volatile uint32_t* destination,
    size_t planeStride,
    uint32_t* pairBits,
    uint32_t planeCount
    )
{
    uint32_t plane;
    uint32_t pair;
    uint32_t word;

    for (plane = 0; plane < planeCount; ++plane) {
        word = 0;
        for (pair = 0; pair < PACKED_PANEL_PIXELS_PER_WORD; ++pair) {
            word |= (pairBits[pair] & PACKED_PANEL_PIXEL_MASK) <<
                (pair * PACKED_PANEL_PIXEL_BITS);
            pairBits[pair] >>= PACKED_PANEL_PIXEL_BITS;
        }
        *destination = word;
        if (plane + 1u < planeCount) {
            destination += planeStride;
        }
    }
} // WritePlaneGroup

/**
    Writes all output planes into an unpublished, capacity-checked scan allocation.

    Borrows the physical RGB565 frame and prepares both scan outputs without an
    intermediate image. The existing mounting correction and scan timing remain
    part of the native packing loop.

    @param destination Driver-owned inactive scan allocation.
    @param capacityWords Writable allocation capacity in 32-bit words.
    @param source Borrowed physical RGB565 frame.

    @return true - All planes were prepared for publication.
    @return false - Initialization, pointers or allocation capacity are invalid.
*/
bool
PackedPanelFrame::Prepare (
    volatile uint32_t* destination,
    size_t capacityWords,
    const Source& source
    ) const
{
    if (!frameWords_ || !source.pixels || !destination || capacityWords < frameWords_) {
        return false;
    }

    PrepareRgb565(destination, static_cast<const uint16_t*>(source.pixels));
    return true;
} // Prepare


/**
    Builds all scan planes from validated graphics RGB565 pixels.

    Prepare has already checked the source, allocation capacity and initialized
    geometry. Each output chooses its native row-half order and traversal once.
    Dual-output mounting correction rotates the complete first chain; the second
    chain retains its original order. No additional image buffer is needed.
    The inner loop uses typed row pointers and native gamma lookups.
    Plane extraction is also inlined so the hot loop executes entirely from
    SRAM when enabled. The forty-byte plane workspace, scan-buffer layout and
    final-word zero padding match the compatible generic path.

    @param destination Driver-owned inactive scan allocation with checked capacity.
    @param pixels Borrowed complete physical graphics frame in RGB565 byte order.
*/
void
PACKED_PANEL_HOT_CODE(PackedPanelFrame::PrepareRgb565) (
    volatile uint32_t* destination,
    const uint16_t* pixels
    ) const
{
    uint32_t lowBits[PACKED_PANEL_PIXELS_PER_WORD];
    uint32_t highBits[PACKED_PANEL_PIXELS_PER_WORD];
    uint64_t pairBits;
    uint32_t row;
    uint32_t lane;
    uint32_t group;
    uint32_t pair;
    uint32_t column;
    uint32_t lowPlaneCount;
    uint32_t highPlaneCount;
    size_t sourceOffset;
    size_t halfPixels;
    ptrdiff_t sourceColumnStep;
    ptrdiff_t sourceColumnOffset;
    size_t destinationIndex;
    size_t planeStride;
    const uint16_t* firstRow = nullptr;
    const uint16_t* secondRow = nullptr;

    lowPlaneCount = depth_;
    if (lowPlaneCount > PACKED_PANEL_PLANES_PER_GROUP) {
        lowPlaneCount = PACKED_PANEL_PLANES_PER_GROUP;
    }
    highPlaneCount = depth_ - lowPlaneCount;
    halfPixels = size_t(rows_) * width_;
    planeStride = size_t(rows_) * outputs_ * rowWords_;
    for (row = 0; row < rows_; ++row) {
        for (lane = 0; lane < outputs_; ++lane) {
            // Select row halves and traversal before entering the native pixel loop.
            sourceColumnStep = 1;
            sourceOffset = size_t(row) * width_ + size_t(lane) * columns_;
            if (lane == 0) {
                firstRow = pixels + sourceOffset;
                secondRow = firstRow + halfPixels;
            } else {
                secondRow = pixels + sourceOffset;
                firstRow = secondRow + halfPixels;
            }
#if PACKED_PANEL_DUAL_FIRST_OUTPUT_ROTATE_180
            // Apply the mounting correction after shared mapping, including saved RGB copies.
            if (outputs_ == PACKED_PANEL_MAX_OUTPUTS && lane == 0) {
                sourceOffset = size_t(rows_ - 1u - row) * width_ + columns_ - 1u;
                secondRow = pixels + sourceOffset;
                firstRow = secondRow + halfPixels;
                sourceColumnStep = -1;
            }
#endif
            sourceColumnOffset = 0;

            for (group = 0; group < rowWords_; ++group) {
                // Only the final word may contain unused fields; never read beyond a row.
                for (pair = 0; pair < PACKED_PANEL_PIXELS_PER_WORD; ++pair) {
                    pairBits = 0;
                    column = group * PACKED_PANEL_PIXELS_PER_WORD + pair;
                    if (column < columns_) {
                        pairBits = ReadRgb565PixelBits(firstRow[sourceColumnOffset]) |
                            (ReadRgb565PixelBits(secondRow[sourceColumnOffset]) << PACKED_PANEL_CHANNELS_PER_HALF);
                        sourceColumnOffset += sourceColumnStep;
                    }
                    lowBits[pair] = uint32_t(pairBits);
                    highBits[pair] = uint32_t(pairBits >> PACKED_PANEL_WORD_BITS);
                }

                // The common inlined writer retains exact bit-plane and DMA word ordering.
                destinationIndex = (size_t(row) * outputs_ + lane) * rowWords_ + group;
                WritePlaneGroup(destination + destinationIndex, planeStride, lowBits, lowPlaneCount);
                if (highPlaneCount) {
                    WritePlaneGroup(destination + destinationIndex + lowPlaneCount * planeStride,
                        planeStride, highBits, highPlaneCount);
                }
            }
        }
    }
} // PrepareRgb565
