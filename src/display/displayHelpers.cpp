#include "display/include/displayHelpers.hpp"

#define RGB_CHANNEL_MAX 255

// Returns the built-in image data for the requested animation frame.
const unsigned char *DisplayHelpers::GetImageDataForFrame([[maybe_unused]] size_t frameIndex)
{
#if USE_ANIMATION
    return FLASHED_IMAGE_DATA[frameIndex];
#else
    return FLASHED_IMAGE_DATA;
#endif
}

// Maps a logical image coordinate to its physical panel position.
void DisplayHelpers::ConvertLogicalToPhysicalCoordinates(
    int logicalX,
    int logicalY,
    int &physicalX,
    int &physicalY)
{
    // Determine which logical panel contains this image coordinate.
    int logicalPanelColumn = logicalX / LOGICAL_PANEL_WIDTH;
    int logicalPanelRow = logicalY / LOGICAL_PANEL_HEIGHT;

    // Subtract the preceding panels to get the coordinate inside this panel.
    int localPanelX = logicalX - (logicalPanelColumn * LOGICAL_PANEL_WIDTH);
    int localPanelY = logicalY - (logicalPanelRow * LOGICAL_PANEL_HEIGHT);

    // Convert the logical panel position to its physical daisy-chain index.
    int daisyChainPanelIndex = PANEL_INDEX_BY_LOGICAL_PANEL[logicalPanelRow][logicalPanelColumn];

    // Panels 2 and 3 are mounted in the opposite orientation, so flip both
    // local axes before converting to the physical display coordinates.
    if (daisyChainPanelIndex >= FIRST_FLIPPED_PANEL_INDEX) {
        localPanelY = (LOGICAL_PANEL_HEIGHT - 1) - localPanelY;
        localPanelX = (LOGICAL_PANEL_WIDTH - 1) - localPanelX;
    }

    // Rotate the logical panel into the driver's physical panel layout.
    // Place the panel in the horizontal daisy-chain framebuffer.
    physicalX = (daisyChainPanelIndex * PHYSICAL_PANEL_WIDTH) +
                (PHYSICAL_PANEL_WIDTH - 1 - localPanelY);
    physicalY = localPanelX;
}

// Maps a logical image into the graphics buffer for the panel chain.
void DisplayHelpers::RenderLogicalImageToGraphics(
    CpuGraphics &graphics,
    const unsigned char *imageData)
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

            //physical color wirieng is broken so the need for shifting
            redDiodeValue = imageData[imageByteIndex + IMAGE_RED_BYTE_OFFSET];
            greenDiodeValue = imageData[imageByteIndex + IMAGE_GREEN_BYTE_OFFSET];
            blueDiodeValue = imageData[imageByteIndex + IMAGE_BLUE_BYTE_OFFSET];

            ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, physicalX, physicalY);

            graphics.set_pen(redDiodeValue, greenDiodeValue, blueDiodeValue);
            graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
        }
    }
}

// Draws the Wi-Fi connection indicator over the logical image.
void DisplayHelpers::DrawWifiStatusIcon(
    CpuGraphics &graphics,
    bool isConnected)
{
    int physicalX;
    int physicalY;
    const int iconLogicalLeft = LOGICAL_IMAGE_WIDTH - WIFI_ICON_MARGIN - WIFI_ICON_WIDTH -
                                WIFI_ICON_HORIZONTAL_OFFSET;
    const int iconLogicalTop = WIFI_ICON_MARGIN;
    const int backingLogicalLeft = iconLogicalLeft - WIFI_ICON_BACKING_SIDE_PADDING;
    const int backingLogicalTop = iconLogicalTop;
    int y;
    int x;
    int bar;
    int dx;
    int barHeight;
    int barLeft;

    // Black backing square so the icon reads clearly over any image content.
    graphics.set_pen(BLACK_COLOR_VALUE, BLACK_COLOR_VALUE, BLACK_COLOR_VALUE);
    for (y = 0; y < WIFI_ICON_BACKING_HEIGHT; y++) {
        for (x = 0; x < WIFI_ICON_BACKING_WIDTH; x++) {
            ConvertLogicalToPhysicalCoordinates(
                backingLogicalLeft + x, backingLogicalTop + y, physicalX, physicalY);
            graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
        }
    }

    if (isConnected) {
        graphics.set_pen(RGB_CHANNEL_MAX, RGB_CHANNEL_MAX, RGB_CHANNEL_MAX); // white bars for connected
    }
    else {
        graphics.set_pen(0, RGB_CHANNEL_MAX,
                         0); // red bars for not connected (counting in my bit shiftied coloring)
    }
    for (bar = 0; bar < WIFI_ICON_BAR_COUNT; bar++) {
        barHeight = (bar + 1) * WIFI_ICON_BAR_HEIGHT_STEP;
        barLeft = bar * WIFI_ICON_BAR_COLUMN_STEP;

        for (y = WIFI_ICON_HEIGHT - barHeight; y < WIFI_ICON_HEIGHT; y++) {
            for (dx = 0; dx < WIFI_ICON_BAR_WIDTH; dx++) {
                ConvertLogicalToPhysicalCoordinates(
                    iconLogicalLeft + barLeft + dx, iconLogicalTop + y, physicalX, physicalY);
                graphics.set_pixel(pimoroni::Point(physicalX, physicalY));
            }
        }
    }
}

// Advances one frame and reverses direction at each animation endpoint.
size_t DisplayHelpers::GetNextPingPongFrameIndex(
    size_t currentFrameIndex,
    bool &movingForward,
    size_t frameCount)
{
    // A single frame is an image, not an animation.
    if (frameCount <= MINIMUM_FRAME_COUNT) {
        return FIRST_FRAME_INDEX;
    }

    // Reverse direction when the current frame is at either endpoint.
    if (movingForward && (currentFrameIndex + FRAME_INDEX_STEP >= frameCount)) {
        movingForward = false;
    }
    else if (!movingForward && currentFrameIndex == FIRST_FRAME_INDEX) {
        movingForward = true;
    }

    // Move one frame in the current direction.
    if (movingForward) {
        return currentFrameIndex + FRAME_INDEX_STEP;
    }

    return currentFrameIndex - FRAME_INDEX_STEP;
}

// Selects the next frame according to the built-in animation mode.
size_t DisplayHelpers::GetNextAnimationFrameIndex(
    [[maybe_unused]] size_t currentFrameIndex,
    [[maybe_unused]] bool &animationMovingForward)
{
#if USE_ANIMATION
    // Use the animation frame count when the build includes an animation.
    return GetNextPingPongFrameIndex(
        currentFrameIndex,
        animationMovingForward,
        ANIMATION_FRAME_COUNT);
#else
    // Non-animation builds always stay on the first frame.
    return FIRST_FRAME_INDEX;
#endif
}
