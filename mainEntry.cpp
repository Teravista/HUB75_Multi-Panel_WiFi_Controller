#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hub75.hpp"
#include "defines/defines.h"
#include "displayHelpers.hpp"
#include "networkUpload.hpp"
#include "wifiNetwork.hpp"

#define WIFI_STATE_MACHINE_REV "wifi-sm-r5"

/*
    Main loop that advances the animation, wifi state machine and renders the new uploaded frame over TCP
    and sends it to the driver to display
*/
static void RenderLoop(DisplayHelpers &displayHelpers, NetworkUpload &networkUpload, WifiNetwork &wifiNetwork, pimoroni::PicoGraphics_PenRGB888 &graphics)
{
    size_t frameIndex = FIRST_FRAME_INDEX;
    bool animationMovingForward = true;
    uint32_t lastFrameCount = 0;
    absolute_time_t lastFpsTime = get_absolute_time();
    uint32_t framesPerSecond = 0;
    uint32_t lastStatusMs = to_ms_since_boot(get_absolute_time());
    uint32_t uptimeMs = 0;
    uint32_t currentFrameCount = 0;
    uint32_t elapsedMs = 0;
    const unsigned char *uploadedFrame = nullptr;

    while (true) {
        uploadedFrame = networkUpload.GetUploadedFrameData();
        if (networkUpload.IsFrameUploadInProgress()) {
            sleep_ms(UPLOAD_POLL_DELAY_1MS);
            continue;
        }

        // If nothing was uploaded, render the next animation frame or the flashed image.
        // TODO: Save uploaded images to flash to support animations over TCP.
        if (uploadedFrame == nullptr)
        {
            displayHelpers.RenderLogicalImageToGraphics(graphics, displayHelpers.GetImageDataForFrame(frameIndex));

            frameIndex = displayHelpers.GetNextAnimationFrameIndex(frameIndex, animationMovingForward);
        }

        // Update the current WiFi connection state.
        wifiNetwork.UpdateNetworkStateMachine();
        // Update the WiFi status icon in the corner.
        displayHelpers.DrawWifiStatusIcon(graphics, wifiNetwork.IsNetworkReady());

        currentFrameCount = hub75_frame_count;
        elapsedMs = to_ms_since_boot(get_absolute_time()) - to_ms_since_boot(lastFpsTime);

        // some FPS calculations for debugging purposes
        if (elapsedMs >= FPS_UPDATE_INTERVAL_1S) {
            framesPerSecond =
                ((currentFrameCount - lastFrameCount) * FPS_UPDATE_INTERVAL_1S) /
                elapsedMs;
            lastFrameCount = currentFrameCount;
            lastFpsTime = get_absolute_time();
        }

        //print status every 10 seconds in serial output
        uptimeMs = to_ms_since_boot(get_absolute_time());
        if (uptimeMs - lastStatusMs >= STATUS_PRINT_INTERVAL_10S)
        {
            printf("[status] t=%us fps=%u net=%s ip=%s\n",
                    uptimeMs / 1000u,
                    framesPerSecond,
                    wifiNetwork.GetNetStateName(),
                    wifiNetwork.GetWifiIpText());
            lastStatusMs = uptimeMs;
        }
        
        //send frame data to the driver to display on the HUB75 panel
        update(&graphics);

        sleep_ms(ANIMATION_FRAME_DELAY_MS);
    }
}

/* 
    some initial setup and initialization functions 
*/
int main()
{
    set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true);
    stdio_init_all();
    sleep_ms(STARTUP_DELAY_5S);

    alignas(4) static unsigned char frameBuffer[FRAME_BUFFER_SIZE_BYTES];
    DisplayHelpers displayHelpers;
    NetworkUpload networkUpload(&displayHelpers, frameBuffer);
    WifiNetwork wifiNetwork(&networkUpload);
    pimoroni::PicoGraphics_PenRGB888 graphics(
        MATRIX_PANEL_WIDTH,
        MATRIX_PANEL_HEIGHT,
        frameBuffer);

    printf("\n[boot] hub75 starting (%s)\n", WIFI_STATE_MACHINE_REV);

    wifiNetwork.InitializeNetworkTiming();
    networkUpload.InitFrameUploadState();

    create_hub75_driver(MATRIX_PANEL_WIDTH, MATRIX_PANEL_HEIGHT, PANEL_TYPE, INVERTED_STB);
    start_hub75_driver();

    setBasisBrightness(DISPLAY_BASE_BRIGHTNESS);
    setIntensity(DISPLAY_INTENSITY);

    printf("[boot] hub75 driver up\n");

    RenderLoop(displayHelpers, networkUpload, wifiNetwork, graphics);
}
    