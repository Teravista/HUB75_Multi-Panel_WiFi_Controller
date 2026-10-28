#pragma once
#include "display/include/graphicsTypes.hpp"

#include <cstddef>

#include "config/defines.h"
#include "hub75.hpp"


/**
    Adapts the project's logical image layout to the physically wired HUB75 panels.

    The rest of the controller works in logical image coordinates, while the graphics buffer
    follows the panel chain, rotations and mounting orientation. This class centralizes that
    mapping for built-in images, stored pixel placement and both network overlays. It also
    preserves the image beneath the bottom-corner IP labels when a held frame is redrawn. It prepares pixels
    through the graphics interface; the HUB75 driver remains responsible for scan buffers, DMA and
    continuous panel refresh.
*/
class PanelRenderer {
public:
    // Returns the built-in image data for the requested animation frame.
    const unsigned char* GetImageDataForFrame(size_t frameIndex);

    // Maps a logical image coordinate to its physical panel position.
    void ConvertLogicalToPhysicalCoordinates(int logicalX, int logicalY, int& physicalX, int& physicalY);

    // Maps a logical image into the graphics buffer for the panel chain.
    void RenderLogicalImageToGraphics(CpuGraphics& graphics, const unsigned char* imageData);

    // Draws independent home Wi-Fi and hotspot indicators at the bottom corners.
    void DrawNetworkStatus(CpuGraphics& graphics, bool homeReady, const char* homeIp, bool hotspotReady, const char* hotspotIp, bool freshFrame = false, const uint8_t* hotspotDevices = nullptr);

    // Advances one frame and reverses direction at each animation endpoint.
    size_t GetNextPingPongFrameIndex(size_t currentFrameIndex, bool& movingForward, size_t frameCount);

    // Selects the next frame according to the built-in animation mode.
    size_t GetNextAnimationFrameIndex(size_t currentFrameIndex, bool& animationMovingForward);

private:
    static const uint8_t digitRows_[][IP_FONT_ROW_COUNT];
    static const uint8_t houseRows_[WIFI_ICON_HEIGHT];
    static const uint8_t antennaRows_[WIFI_ICON_HEIGHT];

    // Draws one stable connection/activity dot for each hotspot device slot.
    void DrawHotspotDeviceDots(CpuGraphics& graphics, const uint8_t* hotspotDevices);

    // Measures a bounded numeric IP label with the selected digit spacing.
    static int MeasureIpLabel(const char* ipAddress, int digitAdvance);

    // Draws one connection icon and its inward-facing IP label.
    void DrawNetworkIndicator(CpuGraphics& graphics, bool connected, const char* ipAddress, bool alignRight, int digitAdvance);

};
