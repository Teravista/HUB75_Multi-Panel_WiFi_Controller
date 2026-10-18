#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hub75.hpp"
#include "config/defines.h"
#include "display/include/displayHelpers.hpp"
#include "network/networkUpload.hpp"
#include "network/wifiNetwork.hpp"

#define WIFI_STATE_MACHINE_REV "wifi-sm-r5"

// Prepares and submits display frames while advancing controller services.
static void RenderLoop(DisplayHelpers &displayHelpers, NetworkUpload &networkUpload, WifiNetwork &wifiNetwork, CpuGraphics &graphics)
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
            printf("[status] t=%us fps=%u net=%s ip=%s\n", uptimeMs / MILLISECONDS_PER_SECOND_U,
                   framesPerSecond, wifiNetwork.GetNetStateName(), wifiNetwork.GetWifiIpText());
            lastStatusMs = uptimeMs;
        }
        
        // Reuse the driver writer only after its previous swap was consumed.
        if (hub75_ready_for_update()) update(&graphics);

        sleep_ms(ANIMATION_FRAME_DELAY_MS);
    }
}

// Initializes the controller and starts the display and network loops.
int main()
{
    set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true);
    stdio_init_all();
    sleep_ms(STARTUP_DELAY_5S);

    alignas(DMA_BUFFER_ALIGNMENT) static unsigned char frameBuffer[FRAME_BUFFER_SIZE_BYTES];
    DisplayHelpers displayHelpers;
    NetworkUpload networkUpload(&displayHelpers, frameBuffer);
    WifiNetwork wifiNetwork(&networkUpload);
    CpuGraphics graphics(
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

    // Queue visible content before the first blocking Wi-Fi connection.
    displayHelpers.RenderLogicalImageToGraphics(
        graphics, displayHelpers.GetImageDataForFrame(FIRST_FRAME_INDEX));
    displayHelpers.DrawWifiStatusIcon(graphics, false);
    update(&graphics);

    RenderLoop(displayHelpers, networkUpload, wifiNetwork, graphics);
}
    