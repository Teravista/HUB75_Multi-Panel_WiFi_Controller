#include "display/include/PanelRendererClass.hpp"
#include "config/defines.h"

#define IP_ROW_RIGHT 0b001
#define IP_ROW_CENTER 0b010
#define IP_ROW_LEFT 0b100
#define IP_ROW_SIDES (IP_ROW_LEFT | IP_ROW_RIGHT)
#define IP_ROW_LEFT_CENTER (IP_ROW_LEFT | IP_ROW_CENTER)
#define IP_ROW_FULL (IP_ROW_LEFT | IP_ROW_CENTER | IP_ROW_RIGHT)

#define IP_MAX_ADDRESS_CHARACTERS 15
#define IP_LABEL_PADDING_PIXELS 2
#define IP_LABEL_PADDING_SIDES 2
#define IP_LABEL_GAP 2
#define NETWORK_INDICATOR_COUNT 2
#define NETWORK_INDICATOR_MIN_GAP 1
#define NETWORK_INDICATOR_FIXED_WIDTH (NETWORK_INDICATOR_COUNT * (WIFI_ICON_MARGIN + WIFI_ICON_BACKING_WIDTH + IP_LABEL_GAP + IP_LABEL_PADDING_SIDES * IP_LABEL_PADDING_PIXELS) + NETWORK_INDICATOR_MIN_GAP)
#define IP_FONT_HEIGHT IP_FONT_ROW_COUNT
#define IP_DIGIT_WIDTH 3
#define IP_DIGIT_ADVANCE 4
#define IP_DOT_WIDTH 1
#define IP_DOT_ADVANCE 2
#define IP_DOT_ROW 4
#define IP_TEXT_VERTICAL_ADJUSTMENT 1
#define HOTSPOT_DOT_HEIGHT 3
#define HOTSPOT_DOT_CENTER_ROW 1
#define HOTSPOT_DOT_FIRST_COLUMN 1
#define HOTSPOT_DOT_GAP 1
#define NETWORK_INDICATOR_TOP (LOGICAL_IMAGE_HEIGHT - WIFI_ICON_MARGIN - WIFI_ICON_BACKING_HEIGHT)
#define HOTSPOT_DOT_TOP (NETWORK_INDICATOR_TOP - HOTSPOT_DOT_GAP - HOTSPOT_DOT_HEIGHT)
#define NETWORK_OVERLAY_TOP HOTSPOT_DOT_TOP
#define HOTSPOT_DOT_WIDTH 1
#define HOTSPOT_DOT_ADVANCE 2
#define HOTSPOT_DOT_STRIP_WIDTH (HOTSPOT_DOT_FIRST_COLUMN + (WIFI_HOTSPOT_MAX_CLIENTS - 1) * HOTSPOT_DOT_ADVANCE + HOTSPOT_DOT_WIDTH)
#define NETWORK_OVERLAY_HEIGHT (NETWORK_INDICATOR_TOP + WIFI_ICON_BACKING_HEIGHT - NETWORK_OVERLAY_TOP)
#define NETWORK_GREEN_SOURCE_BYTE_OFFSET 1

static_assert(WIFI_HOTSPOT_MAX_CLIENTS > 0 && HOTSPOT_DOT_STRIP_WIDTH <= WIFI_ICON_BACKING_WIDTH,
    "Hotspot client dots must fit across the antenna backing");
static_assert(NETWORK_OVERLAY_TOP >= 0 &&
    HOTSPOT_DOT_TOP + HOTSPOT_DOT_HEIGHT <= NETWORK_INDICATOR_TOP &&
    NETWORK_INDICATOR_TOP + WIFI_ICON_BACKING_HEIGHT <= LOGICAL_IMAGE_HEIGHT,
    "Network icons and device strip must fit within separate display rows");

const uint8_t PanelRenderer::houseRows_[WIFI_ICON_HEIGHT] = {
    0b00011000, 0b00111100, 0b01100110, 0b11000011,
    0b01000010, 0b01011010, 0b01011010, 0b01111110};

const uint8_t PanelRenderer::antennaRows_[WIFI_ICON_HEIGHT] = {
    0b00000000, 0b10000001, 0b01011010, 0b00100100,
    0b00011000, 0b00011000, 0b00011000, 0b00111100};

const uint8_t PanelRenderer::digitRows_[][IP_FONT_HEIGHT] = {
    {IP_ROW_FULL, IP_ROW_SIDES, IP_ROW_SIDES, IP_ROW_SIDES, IP_ROW_FULL},
    {IP_ROW_CENTER, IP_ROW_LEFT_CENTER, IP_ROW_CENTER, IP_ROW_CENTER, IP_ROW_FULL},
    {IP_ROW_FULL, IP_ROW_RIGHT, IP_ROW_FULL, IP_ROW_LEFT, IP_ROW_FULL},
    {IP_ROW_FULL, IP_ROW_RIGHT, IP_ROW_FULL, IP_ROW_RIGHT, IP_ROW_FULL},
    {IP_ROW_SIDES, IP_ROW_SIDES, IP_ROW_FULL, IP_ROW_RIGHT, IP_ROW_RIGHT},
    {IP_ROW_FULL, IP_ROW_LEFT, IP_ROW_FULL, IP_ROW_RIGHT, IP_ROW_FULL},
    {IP_ROW_FULL, IP_ROW_LEFT, IP_ROW_FULL, IP_ROW_SIDES, IP_ROW_FULL},
    {IP_ROW_FULL, IP_ROW_RIGHT, IP_ROW_CENTER, IP_ROW_CENTER, IP_ROW_CENTER},
    {IP_ROW_FULL, IP_ROW_SIDES, IP_ROW_FULL, IP_ROW_SIDES, IP_ROW_FULL},
    {IP_ROW_FULL, IP_ROW_SIDES, IP_ROW_FULL, IP_ROW_RIGHT, IP_ROW_FULL}};

/**
    Returns the built-in image data for the requested animation frame.

    @param frameIndex Zero-based index of the requested frame.

    @return pointer - Borrowed built-in frame data; do not free or modify it.
*/
const unsigned char*
PanelRenderer::GetImageDataForFrame (
    size_t frameIndex
    )
{
#if USE_ANIMATION
    return FLASHED_IMAGE_DATA[frameIndex];
#else
    (void)frameIndex;
    return FLASHED_IMAGE_DATA;
#endif
} // GetImageDataForFrame

/**
    Maps a logical image coordinate to its physical panel position.

    Finds the logical panel containing the pixel, applies the configured chain index and reverses
    axes for panels mounted in the opposite orientation. The final rotation places the pixel in
    the driver's horizontal chain buffer. Callers use this same transform for image pixels and
    overlays so panel seams stay consistent.

    @param logicalX Horizontal coordinate in the logical image, in pixels.
    @param logicalY Vertical coordinate in the logical image, in pixels.
    @param physicalX Output horizontal coordinate after panel wiring correction, in pixels.
    @param physicalY Output vertical coordinate after panel wiring correction, in pixels.
*/
void
PanelRenderer::ConvertLogicalToPhysicalCoordinates (
    int logicalX,
    int logicalY,
    int& physicalX,
    int& physicalY
    )
{
    int logicalPanelColumn;
    int logicalPanelRow;
    int localPanelX;
    int localPanelY;
    int daisyChainPanelIndex;

    // Determine which logical panel contains this image coordinate.
    logicalPanelColumn = logicalX / LOGICAL_PANEL_WIDTH;
    logicalPanelRow = logicalY / LOGICAL_PANEL_HEIGHT;

    // Subtract the preceding panels to get the coordinate inside this panel.
    localPanelX = logicalX - (logicalPanelColumn * LOGICAL_PANEL_WIDTH);
    localPanelY = logicalY - (logicalPanelRow * LOGICAL_PANEL_HEIGHT);

    // Convert the logical panel position to its physical daisy-chain index.
    daisyChainPanelIndex = PANEL_INDEX_BY_LOGICAL_PANEL[logicalPanelRow][logicalPanelColumn];

    // Panels 2 and 3 are mounted in the opposite orientation, so flip both
    // local axes before converting to the physical display coordinates.
    if (daisyChainPanelIndex >= FIRST_FLIPPED_PANEL_INDEX) {
        localPanelY = (LOGICAL_PANEL_HEIGHT - 1) - localPanelY;
        localPanelX = (LOGICAL_PANEL_WIDTH - 1) - localPanelX;
    }

    // Rotate the logical panel into the driver's physical panel layout.
    // Place the panel in the horizontal daisy-chain framebuffer.
    physicalX = (daisyChainPanelIndex * PHYSICAL_PANEL_WIDTH) + (PHYSICAL_PANEL_WIDTH - 1 - localPanelY);
    physicalY = localPanelX;
} // ConvertLogicalToPhysicalCoordinates

/**
    Maps a logical image into the graphics buffer for the panel chain.

    Clears the destination, walks the built-in logical RGB image and applies the configured color
    wiring and panel-coordinate transform to each pixel. This is a synchronous full-image
    conversion into the graphics source buffer. It does not submit a HUB75 scan-buffer swap; the
    display pipeline performs that later.

    @param graphics Destination graphics framebuffer for the panel chain.
    @param imageData Read-only logical RGB888 source frame bytes.
*/
void
PanelRenderer::RenderLogicalImageToGraphics (
    CpuGraphics& graphics,
    const unsigned char* imageData
    )
{
    int physicalX;
    int physicalY;
    int logicalX;
    int logicalY;
    int imageByteIndex;
    int redDiodeValue;
    int greenDiodeValue;
    int blueDiodeValue;

    // Clear pixels that are not overwritten by the current logical image.
    graphics.set_pen(BLACK_COLOR_VALUE, BLACK_COLOR_VALUE, BLACK_COLOR_VALUE);
    graphics.clear();

    for (logicalY = 0; logicalY < LOGICAL_IMAGE_HEIGHT; logicalY++) {
        for (logicalX = 0; logicalX < LOGICAL_IMAGE_WIDTH; logicalX++) {
            // Locate the three color bytes for this logical image pixel.
            imageByteIndex = (logicalY * LOGICAL_IMAGE_WIDTH + logicalX) * IMAGE_BYTES_PER_PIXEL;

            // physical color wirieng is broken so the need for shifting
            redDiodeValue = imageData[imageByteIndex + IMAGE_RED_BYTE_OFFSET];
            greenDiodeValue = imageData[imageByteIndex + IMAGE_GREEN_BYTE_OFFSET];
            blueDiodeValue = imageData[imageByteIndex + IMAGE_BLUE_BYTE_OFFSET];

            ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, physicalX, physicalY);

            graphics.set_pen(redDiodeValue, greenDiodeValue, blueDiodeValue);
            graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
        }
    }
} // RenderLogicalImageToGraphics

/**
    Draws independent home Wi-Fi and hotspot indicators at the bottom corners.

    The home address stays at the bottom-right and the hosted address stays at
    the bottom-left. A saved bottom strip restores the image before every redraw,
    so address changes and disconnects cannot leave old text behind. New source
    frames replace that strip before either indicator is painted. Exceptionally
    long address pairs use tighter character spacing to keep the labels apart.

    @param graphics Destination graphics framebuffer for the panel chain.
    @param homeReady Whether home Wi-Fi and its local controller services are ready.
    @param homeIp Home Wi-Fi IPv4 text, never replaced by the hotspot address.
    @param hotspotReady Whether the hosted network and controller services are ready.
    @param hotspotIp Hosted network IPv4 text, independent of home association.
    @param freshFrame Whether a new base image was drawn before these overlays.
    @param hotspotDevices Optional WIFI_HOTSPOT_MAX_CLIENTS device-state entries.
*/
void
PanelRenderer::DrawNetworkStatus (
    CpuGraphics& graphics,
    bool homeReady,
    const char* homeIp,
    bool hotspotReady,
    const char* hotspotIp,
    bool freshFrame,
    const uint8_t* hotspotDevices
    )
{
    static CpuFrame::Pixel background[LOGICAL_IMAGE_WIDTH * NETWORK_OVERLAY_HEIGHT];
    static_assert(sizeof(background) / sizeof(background[0]) ==
        LOGICAL_IMAGE_WIDTH * (NETWORK_OVERLAY_HEIGHT),
        "Network overlay backup must cover every captured row");
    static CpuFrame::Pixel* savedFrameBuffer = nullptr;
    CpuFrame::Pixel* pixels = nullptr;
    bool captureBackground;
    int physicalX;
    int physicalY;
    int logicalX;
    int logicalY;
    int top;
    int digitAdvance;
    int homeWidth;
    int hotspotWidth;
    size_t backgroundIndex;
    size_t pixelIndex;

    top = NETWORK_OVERLAY_TOP;
    pixels = static_cast<CpuFrame::Pixel*>(graphics.frame_buffer) + graphics.layer_offset;
    captureBackground = freshFrame || savedFrameBuffer != pixels;
    backgroundIndex = 0;
    for (logicalY = top; logicalY < top + NETWORK_OVERLAY_HEIGHT; ++logicalY) {
        for (logicalX = 0; logicalX < LOGICAL_IMAGE_WIDTH; ++logicalX) {
            ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, physicalX, physicalY);
            pixelIndex = physicalY * MATRIX_PANEL_WIDTH + physicalX;
            if (captureBackground) {
                background[backgroundIndex] = pixels[pixelIndex];
            } else {
                pixels[pixelIndex] = background[backgroundIndex];
            }
            ++backgroundIndex;
        }
    }
    savedFrameBuffer = pixels;

    // Normal spacing fits the fixed hotspot address and every home IPv4 address.
    // Compact spacing also supports a longer, custom hotspot subnet without overlap.
    digitAdvance = IP_DIGIT_ADVANCE;
    homeWidth = MeasureIpLabel(homeIp, digitAdvance);
    hotspotWidth = MeasureIpLabel(hotspotIp, digitAdvance);
    if (homeWidth + hotspotWidth + NETWORK_INDICATOR_FIXED_WIDTH > LOGICAL_IMAGE_WIDTH) {
        digitAdvance = IP_DIGIT_WIDTH;
    }
    DrawNetworkIndicator(graphics, homeReady, homeIp, true, digitAdvance);
    DrawNetworkIndicator(graphics, hotspotReady, hotspotIp, false, digitAdvance);
    DrawHotspotDeviceDots(graphics, hotspotDevices);
} // DrawNetworkStatus

/**
    Measures a bounded numeric IP label with the selected digit spacing.

    @param ipAddress Null-terminated address text containing only digits and dots.
    @param digitAdvance Horizontal distance between consecutive digit origins.

    @return pixels - Width of the visible glyphs without trailing character spacing.
    @return 0 - The label is absent, empty, too long or contains unsupported characters.
*/
int
PanelRenderer::MeasureIpLabel (
    const char* ipAddress,
    int digitAdvance
    )
{
    int length;
    int width;
    int trailingSpacing;
    char character;

    if (!ipAddress) {
        return 0;
    }
    width = 0;
    trailingSpacing = 0;
    length = 0;
    for (; length < IP_MAX_ADDRESS_CHARACTERS && ipAddress[length]; ++length) {
        character = ipAddress[length];
        if (character == '.') {
            width += IP_DOT_ADVANCE;
            trailingSpacing = IP_DOT_ADVANCE - IP_DOT_WIDTH;
        } else if (character >= '0' && character <= '9') {
            width += digitAdvance;
            trailingSpacing = digitAdvance - IP_DIGIT_WIDTH;
        } else {
            return 0;
        }
    }
    if (!length || ipAddress[length]) {
        return 0;
    }
    return width - trailingSpacing;
} // MeasureIpLabel

/**
    Draws one connection icon and its inward-facing IP label.

    Left indicators place the label to the icon's right; right indicators place
    it to the icon's left. Coordinates are logical image coordinates and pass
    through the existing panel rotation/wiring mapper. The black text backing
    follows the measured glyph width. Disconnected indicators retain a red icon
    without showing an address from another network interface.

    @param graphics Destination graphics framebuffer for the panel chain.
    @param connected Whether this particular network path is ready.
    @param ipAddress IPv4 label belonging to this network path.
    @param alignRight True anchors the icon at the bottom-right; false uses bottom-left.
    @param digitAdvance Horizontal distance between consecutive digit origins.
*/
void
PanelRenderer::DrawNetworkIndicator (
    CpuGraphics& graphics,
    bool connected,
    const char* ipAddress,
    bool alignRight,
    int digitAdvance
    )
{
    int physicalX;
    int physicalY;
    int backingLeft;
    int top;
    int iconLeft;
    int labelLeft;
    int textLeft;
    int textTop;
    int textWidth;
    int x;
    int y;
    int cursor;
    int index;
    int width;
    int advance;
    int row;
    int column;
    const uint8_t* iconRows = nullptr;
    uint8_t bits;
    char character;

    top = NETWORK_INDICATOR_TOP;
    backingLeft = WIFI_ICON_MARGIN;
    if (alignRight) {
        backingLeft = LOGICAL_IMAGE_WIDTH - WIFI_ICON_MARGIN - WIFI_ICON_BACKING_WIDTH;
    }
    iconLeft = backingLeft + WIFI_ICON_BACKING_SIDE_PADDING;
    textTop = top + (WIFI_ICON_HEIGHT - IP_FONT_HEIGHT) / CENTER_ALIGNMENT_DIVISOR + IP_TEXT_VERTICAL_ADJUSTMENT;

    // Only ready paths display their own address. The common saved strip already
    // restored any former label before this function draws the current one.
    textWidth = 0;
    if (connected) {
        textWidth = MeasureIpLabel(ipAddress, digitAdvance);
    }
    if (textWidth > 0) {
        labelLeft = backingLeft + WIFI_ICON_BACKING_WIDTH + IP_LABEL_GAP;
        if (alignRight) {
            labelLeft = backingLeft - IP_LABEL_GAP - textWidth - IP_LABEL_PADDING_PIXELS * IP_LABEL_PADDING_SIDES;
        }
        textLeft = labelLeft + IP_LABEL_PADDING_PIXELS;
        graphics.set_pen(BLACK_COLOR_VALUE, BLACK_COLOR_VALUE, BLACK_COLOR_VALUE);
        for (y = top; y < top + WIFI_ICON_BACKING_HEIGHT; ++y) {
            for (x = labelLeft; x < labelLeft + textWidth + IP_LABEL_PADDING_PIXELS * IP_LABEL_PADDING_SIDES; ++x) {
                ConvertLogicalToPhysicalCoordinates(x, y, physicalX, physicalY);
                graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
            }
        }

        graphics.set_pen(RGB_CHANNEL_MAX, RGB_CHANNEL_MAX, RGB_CHANNEL_MAX);
        cursor = textLeft;
        for (index = 0; ipAddress[index]; ++index) {
            character = ipAddress[index];
            width = IP_DIGIT_WIDTH;
            advance = digitAdvance;
            if (character == '.') {
                width = IP_DOT_WIDTH;
                advance = IP_DOT_ADVANCE;
            }
            for (row = 0; row < IP_FONT_HEIGHT; ++row) {
                bits = 0;
                if (character == '.') {
                    if (row == IP_DOT_ROW) {
                        bits = IP_ROW_RIGHT;
                    }
                } else {
                    bits = digitRows_[character - '0'][row];
                }
                for (column = 0; column < width; ++column) {
                    if (bits & (1u << (width - 1 - column))) {
                        ConvertLogicalToPhysicalCoordinates(cursor + column, textTop + row, physicalX, physicalY);
                        graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
                    }
                }
            }
            cursor += advance;
        }
    }

    graphics.set_pen(BLACK_COLOR_VALUE, BLACK_COLOR_VALUE, BLACK_COLOR_VALUE);
    for (y = top; y < top + WIFI_ICON_BACKING_HEIGHT; ++y) {
        for (x = backingLeft; x < backingLeft + WIFI_ICON_BACKING_WIDTH; ++x) {
            ConvertLogicalToPhysicalCoordinates(x, y, physicalX, physicalY);
            graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
        }
    }
    if (connected) {
        graphics.set_pen(RGB_CHANNEL_MAX, RGB_CHANNEL_MAX, RGB_CHANNEL_MAX);
    } else {
        // These panels route the physical red diode through the green channel.
        graphics.set_pen(BLACK_COLOR_VALUE, RGB_CHANNEL_MAX, BLACK_COLOR_VALUE);
    }
    iconRows = antennaRows_;
    if (alignRight) {
        iconRows = houseRows_;
    }
    for (y = 0; y < WIFI_ICON_HEIGHT; ++y) {
        for (x = 0; x < WIFI_ICON_WIDTH; ++x) {
            if (iconRows[y] & (1u << (WIFI_ICON_WIDTH - 1 - x))) {
                ConvertLogicalToPhysicalCoordinates(iconLeft + x, top + y, physicalX, physicalY);
                graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
            }
        }
    }
} // DrawNetworkIndicator

/**
    Advances one frame and reverses direction at each animation endpoint.

    @param currentFrameIndex Zero-based index of the currently displayed frame.
    @param movingForward In/out playback direction; updated when an endpoint is reached.
    @param frameCount Total number of frames in the animation.

    @return index - Next zero-based frame index, with the direction reversed at endpoints.
    @return 0 - The animation contains at most one frame.
*/
size_t
PanelRenderer::GetNextPingPongFrameIndex (
    size_t currentFrameIndex,
    bool& movingForward,
    size_t frameCount
    )
{
    // A single frame is an image, not an animation.
    if (frameCount <= MINIMUM_FRAME_COUNT) {
        return FIRST_FRAME_INDEX;
    }

    // Reverse direction when the current frame is at either endpoint.
    if (movingForward && (currentFrameIndex + FRAME_INDEX_STEP >= frameCount)) {
        movingForward = false;
    } else if (!movingForward && currentFrameIndex == FIRST_FRAME_INDEX) {
        movingForward = true;
    }

    // Move one frame in the current direction.
    if (movingForward) {
        return currentFrameIndex + FRAME_INDEX_STEP;
    }

    return currentFrameIndex - FRAME_INDEX_STEP;
} // GetNextPingPongFrameIndex

/**
    Selects the next frame according to the built-in animation mode.

    @param currentFrameIndex Zero-based index of the currently displayed frame.
    @param animationMovingForward In/out playback direction; updated when an endpoint is
                                  reached.

    @return index - Next frame according to the compiled animation mode.
    @return 0 - The built-in image is static or the animation has returned to its beginning.
*/
size_t
PanelRenderer::GetNextAnimationFrameIndex (
    size_t currentFrameIndex,
    bool& animationMovingForward
    )
{
#if USE_ANIMATION
    // Use the animation frame count when the build includes an animation.
    return GetNextPingPongFrameIndex(currentFrameIndex, animationMovingForward, ANIMATION_FRAME_COUNT);
#else
    // Non-animation builds always stay on the first frame.
    (void)currentFrameIndex;
    (void)animationMovingForward;
    return FIRST_FRAME_INDEX;
#endif
} // GetNextAnimationFrameIndex

/**
    Draws one stable connection/activity dot for each hotspot device slot.

    The ten-column, three-row backing sits above the antenna with a one-pixel gap.
    Single pixels occupy the middle row at columns two, four, six and eight.
    A connected idle peer is white, accepted meaningful traffic is green, and an
    empty slot is black.
    The shared overlay backup restores the strip before redraw. Logical green
    passes through the same configured channel reordering as ordinary images.

    @param graphics Destination graphics framebuffer for the panel chain.
    @param hotspotDevices Optional WIFI_HOTSPOT_MAX_CLIENTS device-state entries.
*/
void
PanelRenderer::DrawHotspotDeviceDots (
    CpuGraphics& graphics,
    const uint8_t* hotspotDevices
    )
{
    uint8_t activeColor[IMAGE_BYTES_PER_PIXEL]{};
    int top;
    int left;
    int x;
    int y;
    int physicalX;
    int physicalY;
    size_t slot;

    top = HOTSPOT_DOT_TOP;
    left = WIFI_ICON_MARGIN;
    graphics.set_pen(BLACK_COLOR_VALUE, BLACK_COLOR_VALUE, BLACK_COLOR_VALUE);
    for (y = top; y < top + HOTSPOT_DOT_HEIGHT; ++y) {
        for (x = left; x < left + WIFI_ICON_BACKING_WIDTH; ++x) {
            ConvertLogicalToPhysicalCoordinates(x, y, physicalX, physicalY);
            graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
        }
    }
    if (!hotspotDevices) {
        return;
    }
    activeColor[NETWORK_GREEN_SOURCE_BYTE_OFFSET] = RGB_CHANNEL_MAX;
    for (slot = 0; slot < WIFI_HOTSPOT_MAX_CLIENTS; ++slot) {
        if (hotspotDevices[slot] == WIFI_HOTSPOT_CLIENT_OFF) {
            continue;
        }
        graphics.set_pen(RGB_CHANNEL_MAX, RGB_CHANNEL_MAX, RGB_CHANNEL_MAX);
        if (hotspotDevices[slot] == WIFI_HOTSPOT_CLIENT_ACTIVE) {
            graphics.set_pen(activeColor[IMAGE_RED_BYTE_OFFSET], activeColor[IMAGE_GREEN_BYTE_OFFSET],
                activeColor[IMAGE_BLUE_BYTE_OFFSET]);
        }
        x = left + HOTSPOT_DOT_FIRST_COLUMN + int(slot) * HOTSPOT_DOT_ADVANCE;
        ConvertLogicalToPhysicalCoordinates(x, top + HOTSPOT_DOT_CENTER_ROW, physicalX, physicalY);
        graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
    }
} // DrawHotspotDeviceDots
