#pragma once
#include "display/include/graphicsTypes.hpp"

#include <cstddef>

#include "config/defines.h"
#include "hub75.hpp"


/**
    Adapts the project's logical image layout to the physically wired HUB75 panels.

    The rest of the controller works in logical image coordinates, while the graphics buffer
    follows the panel chain, rotations and mounting orientation. This class centralizes that
    mapping for built-in images, stored pixel placement and the Wi-Fi status overlay. It also
    preserves the image beneath the IP label when an existing frame is redrawn. It prepares pixels
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

    // Draws the Wi-Fi connection indicator over the logical image.
    void DrawWifiStatusIcon(CpuGraphics& graphics, bool isConnected, const char* ipAddress, bool freshFrame = false);

    // Advances one frame and reverses direction at each animation endpoint.
    size_t GetNextPingPongFrameIndex(size_t currentFrameIndex, bool& movingForward, size_t frameCount);

    // Selects the next frame according to the built-in animation mode.
    size_t GetNextAnimationFrameIndex(size_t currentFrameIndex, bool& animationMovingForward);

private:
    static const uint8_t digitRows_[][IP_FONT_ROW_COUNT];

};
