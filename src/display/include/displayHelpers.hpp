#pragma once

#include <cstddef>

#include "config/defines.h"
#include "hub75.hpp"

#if CPU_FRAME_RGB565
using CpuGraphics = pimoroni::PicoGraphics_PenRGB565;
#else
using CpuGraphics = pimoroni::PicoGraphics_PenRGB888;
#endif

class DisplayHelpers {
public:
    // Returns the built-in image data for the requested animation frame.
    const unsigned char *GetImageDataForFrame(
        size_t frameIndex
    );

    // Maps a logical image coordinate to its physical panel position.
    void ConvertLogicalToPhysicalCoordinates(
        int logicalX,
        int logicalY,
        int &physicalX,
        int &physicalY
    );

    // Maps a logical image into the graphics buffer for the panel chain.
    void RenderLogicalImageToGraphics(
        CpuGraphics &graphics,
        const unsigned char *imageData
    );

    // Draws the Wi-Fi connection indicator over the logical image.
    void DrawWifiStatusIcon(
        CpuGraphics &graphics,
        bool isConnected
    );

    // Advances one frame and reverses direction at each animation endpoint.
    size_t GetNextPingPongFrameIndex(
        size_t currentFrameIndex,
        bool &movingForward,
        size_t frameCount
    );

    // Selects the next frame according to the built-in animation mode.
    size_t GetNextAnimationFrameIndex(
        size_t currentFrameIndex,
        bool &animationMovingForward
    );

};
