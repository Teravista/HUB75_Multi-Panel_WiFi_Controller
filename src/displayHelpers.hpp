#pragma once

#include <cstddef>

#include "defines/defines.h"
#include "hub75.hpp"

class DisplayHelpers {
public:
    /*
        Returns the image data for the requested frame. Animation builds select a
        frame from the animation array; non-animation builds always use the single
        static image. This keeps conditional compilation out of the render loop.
    */
    const unsigned char *GetImageDataForFrame(
        size_t frameIndex
    );

    /*
        Converts one coordinate from the logical 128x256 image into the physical
        512x64 framebuffer used by the HUB75 driver. The logical image is divided
        into four 64x128 panels. This function finds the logical panel, gets the
        coordinate inside that panel, applies the required orientation correction,
        rotates it into the driver's 128x64 panel layout, and places it according
        to the panel's daisy-chain index.
    */
    void ConvertLogicalToPhysicalCoordinates(
        int logicalX,
        int logicalY,
        int &physicalX,
        int &physicalY
    );

    /*
      Renders one logical image into the physical graphics framebuffer. Each
      source pixel is read from the image data, converted from the stored color
      byte order to RGB, mapped to its physical panel coordinate, and written to
      the graphics buffer for the HUB75 driver.
    */
    void RenderLogicalImageToGraphics(
        pimoroni::PicoGraphics_PenRGB888 &graphics,
        const unsigned char *imageData
    );

    /*
      Draws a small WiFi status icon (three signal bars) over the top-right
      corner of the logical image, on a black backing square so it stays
      readable over any image content. White bars mean connected, red means not
      connected.
    */
    void DrawWifiStatusIcon(
        pimoroni::PicoGraphics_PenRGB888 &graphics,
        bool isConnected
    );

    /*
      Returns the next animation frame while moving between the first and last
      frame. The movingForward flag is updated whenever an endpoint is reached,
      creating a sequence such as 0, 1, 2, 1, 0, 1. For a single-frame image,
      the first frame is always returned.
    */
    size_t GetNextPingPongFrameIndex(
        size_t currentFrameIndex,
        bool &movingForward,
        size_t frameCount
    );

    /*
     Gets the next frame for the configured animation.
     If this is not an animation build, it always returns the first frame.
    */
    size_t GetNextAnimationFrameIndex(
        size_t currentFrameIndex,
        bool &animationMovingForward
    );
};
