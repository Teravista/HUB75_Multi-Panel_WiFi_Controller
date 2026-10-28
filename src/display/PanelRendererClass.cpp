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
#define IP_LABEL_MAX_WIDTH (IP_MAX_ADDRESS_CHARACTERS * IP_DIGIT_ADVANCE - 1 + IP_LABEL_PADDING_PIXELS * 2)
#define IP_LABEL_HEIGHT (WIFI_ICON_BACKING_HEIGHT)
#define IP_LABEL_HORIZONTAL_ADJUSTMENT 2
#define IP_FONT_HEIGHT IP_FONT_ROW_COUNT
#define IP_DIGIT_WIDTH 3
#define IP_DIGIT_ADVANCE 4
#define IP_DOT_ADVANCE 2
#define IP_DOT_ROW 4
#define IP_TEXT_VERTICAL_ADJUSTMENT 1



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
    Draws the Wi-Fi connection indicator over the logical image.

    Maps the icon and IP label through the same logical-to-physical transform as image content. A
    compact bitmap font and text-sized black backing keep the label aligned next to the connection
    indicator.

    The function retains the covered base pixels for repeated overlays on an unchanged frame and
    refreshes that background when a new image is supplied. Restoring the background before
    drawing prevents stale address text or a disconnected label from permanently altering the
    displayed photo.

    @param graphics Destination graphics framebuffer for the panel chain.
    @param isConnected Whether the local network connection is ready.
    @param ipAddress IPv4 address text to show next to the Wi-Fi icon.
    @param freshFrame Whether a new base image was drawn before the status overlay.
*/
void
PanelRenderer::DrawWifiStatusIcon (
    CpuGraphics& graphics,
    bool isConnected,
    const char* ipAddress,
    bool freshFrame
    )
{
    static CpuFrame::Pixel background[IP_LABEL_MAX_WIDTH * IP_LABEL_HEIGHT];
    static CpuFrame::Pixel* savedFrameBuffer = nullptr;

    int physicalX;
    int physicalY;
    int iconLogicalLeft;
    int iconLogicalTop;
    int backingLogicalLeft;
    int backingLogicalTop;
    int y;
    int x;
    int bar;
    int dx;
    int barHeight;
    int barLeft;
    int labelRight;
    int labelLeft;
    int textTop;
    CpuFrame::Pixel* pixels = nullptr;
    bool captureBackground;
    size_t backgroundIndex;
    size_t pixelIndex;
    int length;
    int textWidth;
    bool valid;
    char c;
    int cursor;
    int i;
    char nextC;
    int width;
    int row;
    uint8_t bits;
    int column;

    iconLogicalLeft = LOGICAL_IMAGE_WIDTH - WIFI_ICON_MARGIN - WIFI_ICON_WIDTH - WIFI_ICON_HORIZONTAL_OFFSET;
    iconLogicalTop = WIFI_ICON_MARGIN;
    backingLogicalLeft = iconLogicalLeft - WIFI_ICON_BACKING_SIDE_PADDING;
    backingLogicalTop = iconLogicalTop;

    // A compact 3x5 font keeps even 255.255.255.255 on one line. Drawing each
    // logical pixel through the panel mapper preserves orientation across seams.
    labelRight = backingLogicalLeft - IP_LABEL_HORIZONTAL_ADJUSTMENT;
    labelLeft = labelRight - IP_LABEL_HORIZONTAL_ADJUSTMENT + IP_LABEL_PADDING_PIXELS - IP_LABEL_MAX_WIDTH;
    textTop = iconLogicalTop + (WIFI_ICON_HEIGHT - IP_FONT_HEIGHT) / CENTER_ALIGNMENT_DIVISOR + IP_TEXT_VERTICAL_ADJUSTMENT;

    // Restore or capture only the small backing patch under the IP label.
    pixels = static_cast<CpuFrame::Pixel*>(graphics.frame_buffer) + graphics.layer_offset;
    captureBackground = freshFrame || savedFrameBuffer != pixels;
    backgroundIndex = 0;
    for (y = iconLogicalTop; y < iconLogicalTop + IP_LABEL_HEIGHT; ++y) {
        for (x = labelLeft; x < labelLeft + IP_LABEL_MAX_WIDTH; ++x) {
            ConvertLogicalToPhysicalCoordinates(x, y, physicalX, physicalY);
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
    if (isConnected && ipAddress != nullptr) {
        length = 0;
        textWidth = 0;
        valid = true;
        for (; length < IP_MAX_ADDRESS_CHARACTERS && ipAddress[length]; ++length) {
            c = ipAddress[length];
            if (c != '.' && (c < '0' || c > '9')) {
                valid = false;
                break;
            }
            if (c == '.') {
                textWidth += IP_DOT_ADVANCE;
            } else {
                textWidth += IP_DIGIT_ADVANCE;
            }
        }
        if (valid && length > 0 && ipAddress[length] == '\0') {
            --textWidth; // No trailing character spacing.
            cursor = labelRight - textWidth - IP_DOT_ADVANCE;
            graphics.set_pen(BLACK_COLOR_VALUE, BLACK_COLOR_VALUE, BLACK_COLOR_VALUE);
            for (y = iconLogicalTop; y < iconLogicalTop + IP_LABEL_HEIGHT; ++y) {
                for (x = cursor - IP_LABEL_PADDING_PIXELS; x < cursor + textWidth + IP_LABEL_PADDING_PIXELS; ++x) {
                    ConvertLogicalToPhysicalCoordinates(x, y, physicalX, physicalY);
                    graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
                }
            }
            graphics.set_pen(RGB_CHANNEL_MAX, RGB_CHANNEL_MAX, RGB_CHANNEL_MAX);
            for (i = 0; i < length; ++i) {
                nextC = ipAddress[i];
                if (nextC == '.') {
                    width = 1;
                } else {
                    width = IP_DIGIT_WIDTH;
                }
                for (row = 0; row < IP_FONT_HEIGHT; ++row) {
                    bits = 0;
                    if (nextC == '.') {
                        if (row == IP_DOT_ROW) {
                            bits = 1;
                        }
                    } else {
                        bits = digitRows_[nextC - '0'][row];
                    }
                    for (column = 0; column < width; ++column) {
                        if (!(bits & (1u << (width - 1 - column)))) {
                            continue;
                        }
                        ConvertLogicalToPhysicalCoordinates(cursor + column, textTop + row, physicalX,
                                                            physicalY);
                        graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
                    }
                }
                cursor += width + 1;
            }
        }
    }

    // Black backing square so the icon reads clearly over any image content.
    graphics.set_pen(BLACK_COLOR_VALUE, BLACK_COLOR_VALUE, BLACK_COLOR_VALUE);
    for (y = 0; y < WIFI_ICON_BACKING_HEIGHT; y++) {
        for (x = 0; x < WIFI_ICON_BACKING_WIDTH; x++) {
            ConvertLogicalToPhysicalCoordinates(backingLogicalLeft + x, backingLogicalTop + y, physicalX,
                                                physicalY);
            graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
        }
    }

    if (isConnected) {
        graphics.set_pen(RGB_CHANNEL_MAX, RGB_CHANNEL_MAX, RGB_CHANNEL_MAX); // white bars for connected
    } else {
        graphics.set_pen(0, RGB_CHANNEL_MAX,
                         0); // red bars for not connected (counting in my bit shiftied coloring)
    }
    for (bar = 0; bar < WIFI_ICON_BAR_COUNT; bar++) {
        barHeight = (bar + 1) * WIFI_ICON_BAR_HEIGHT_STEP;
        barLeft = bar * WIFI_ICON_BAR_COLUMN_STEP;

        for (y = WIFI_ICON_HEIGHT - barHeight; y < WIFI_ICON_HEIGHT; y++) {
            for (dx = 0; dx < WIFI_ICON_BAR_WIDTH; dx++) {
                ConvertLogicalToPhysicalCoordinates(iconLogicalLeft + barLeft + dx, iconLogicalTop + y,
                                                    physicalX, physicalY);
                graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
            }
        }
    }
} // DrawWifiStatusIcon

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
