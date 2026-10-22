#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hub75.hpp"
#include "config/defines.h"
#include "display/include/displayHelpers.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include "network/wifiNetwork.hpp"
#include "sd/include/MemoryManagerClass.hpp"
#include "media/include/PhotoPersistenceClass.hpp"
#include "media/include/MediaPlayerClass.hpp"
#include "network/include/TcpVideoUploadClass.hpp"

#define WIFI_STATE_MACHINE_REV "raw-video-storage"

/**
    Prepares and submits display frames while advancing photo and network services.

    Wi-Fi work stays on the existing foreground loop. Rendering claims the CPU
    frame so callbacks cannot replace pixels while persistence reads them. Each
    completed upload is saved once before drawing the Wi-Fi status overlay.
    The scanner continues showing its last submitted buffer during SD work.

    @param displayHelpers Logical-to-physical panel mapping and status overlay.
    @param upload Shared framebuffer and raw photo receiver.
    @param wifiNetwork Existing local-network state controller.
    @param memory SD image-storage boundary.
    @param photos Automatic photo-save and startup-restore bridge.
    @param player Verified saved-media playback scheduler.
    @param video Bounded raw-video upload and verification service.
    @param graphics Shared CPU graphics source buffer.
*/
static void
RenderLoop (
    DisplayHelpers& displayHelpers,
    FrameBufferController& upload,
    WifiNetwork& wifiNetwork,
    MemoryManager& memory,
    PhotoPersistence& photos,
    MediaPlayer& player,
    TcpVideoUpload& video,
    CpuGraphics& graphics
    )
{
    size_t frameIndex;
    bool animationMovingForward;
    bool storageStarted;
    bool reloadVideo;
    uint32_t lastFrameCount;
    absolute_time_t lastFpsTime;
    uint32_t framesPerSecond;
    uint32_t lastStatusMs;
    uint32_t uptimeMs;
    uint32_t currentFrameCount;
    uint32_t elapsedMs;
    const unsigned char* uploadedFrame = nullptr;
    PhotoPersistence::SaveResult saved;
    MediaMetadata metadata;

    frameIndex = FIRST_FRAME_INDEX;
    animationMovingForward = true;
    storageStarted = false;
    lastFrameCount = 0;
    lastFpsTime = get_absolute_time();
    framesPerSecond = 0;
    lastStatusMs = to_ms_since_boot(get_absolute_time());

    for (;;) {
        wifiNetwork.UpdateNetworkStateMachine();
        if (!storageStarted && wifiNetwork.IsRadioInitialized()) {
            storageStarted = true;
            if (memory.Initialize()) {
                player.RequestReload();
            } else {
                printf("[memory] saves unavailable: %s\n", memory.GetLastError());
            }
        }
        video.Update();
        reloadVideo = video.TakeReloadRequest();
        if (reloadVideo) {
            player.RequestReload();
        }
        if (!upload.TryBeginRender()) {
            sleep_ms(UPLOAD_POLL_DELAY_1MS);
            continue;
        }

        // Completed photo uploads replace playback; video transactions request a reload.
        if (!reloadVideo && upload.GetUploadedFrameData() && !upload.IsStoredFrame()) {
            player.Stop();
            saved = photos.SaveUploadedImage();
            if (saved == PhotoPersistence::SaveResult::Saved) {
                metadata = upload.GetFrameMetadata();
                printf("[memory] photo saved format=%s frames=%u fps=%u/%u\n",
                       metadata.FormatName(), unsigned(metadata.frameCount),
                       unsigned(metadata.fpsNumerator), unsigned(metadata.fpsDenominator));
            } else if (saved == PhotoPersistence::SaveResult::Failed) {
                printf("[memory] photo save failed: %s; keeping live photo\n", memory.GetLastError());
            }
        } else {
            player.Update(to_ms_since_boot(get_absolute_time()));
        }
        uploadedFrame = upload.GetUploadedFrameData();
        if (!uploadedFrame) {
            displayHelpers.RenderLogicalImageToGraphics(
                graphics, displayHelpers.GetImageDataForFrame(frameIndex));
            frameIndex = displayHelpers.GetNextAnimationFrameIndex(frameIndex, animationMovingForward);
        }
        displayHelpers.DrawWifiStatusIcon(graphics, wifiNetwork.IsNetworkReady());

        currentFrameCount = hub75_frame_count;
        uptimeMs = to_ms_since_boot(get_absolute_time());
        elapsedMs = uptimeMs - to_ms_since_boot(lastFpsTime);
        if (elapsedMs >= FPS_UPDATE_INTERVAL_1S) {
            framesPerSecond = ((currentFrameCount - lastFrameCount) * FPS_UPDATE_INTERVAL_1S) / elapsedMs;
            lastFrameCount = currentFrameCount;
            lastFpsTime = get_absolute_time();
        }
        if (uptimeMs - lastStatusMs >= STATUS_PRINT_INTERVAL_10S) {
            printf("[status] t=%us fps=%u net=%s ip=%s\n", uptimeMs / MILLISECONDS_PER_SECOND_U,
                   framesPerSecond, wifiNetwork.GetNetStateName(), wifiNetwork.GetWifiIpText());
            lastStatusMs = uptimeMs;
        }
        if (hub75_ready_for_update()) {
            update(&graphics);
        }
        upload.EndRender();
        sleep_ms(ANIMATION_FRAME_DELAY_MS);
    }
} // RenderLoop

/**
    Initializes the controller and restores a saved photo before networking starts.

    The flashed frame is queued first so display scanning has visible content
    during SD probing. A verified saved photo or first video frame replaces it; missing or damaged
    storage leaves the flashed image available. Startup temporarily releases SD
    resources for the initial radio claim and re-enables them from RenderLoop.

    @return value - The controller normally remains in RenderLoop indefinitely.
*/
int
main (
    void
    )
{
    alignas(DMA_BUFFER_ALIGNMENT) static unsigned char frameBuffer[FRAME_BUFFER_SIZE_BYTES];
    DisplayHelpers displayHelpers;
    FrameBufferController upload(&displayHelpers, frameBuffer, true);
    MemoryManager memory;
    static TcpVideoUpload video(memory, upload);
    WifiNetwork wifiNetwork(&upload, &video);
    MediaPlayer player(memory, upload);
    PhotoPersistence photos(memory, upload);
    CpuGraphics graphics(MATRIX_PANEL_WIDTH, MATRIX_PANEL_HEIGHT, frameBuffer);
    bool restored;

    set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true);
    stdio_init_all();
#if HUB75_VERBOSE_LOGS
    sleep_ms(STARTUP_DELAY_5S);
#endif
    printf("\n[boot] hub75 starting (%s)\n", WIFI_STATE_MACHINE_REV);
    wifiNetwork.InitializeNetworkTiming();
    create_hub75_driver(MATRIX_PANEL_WIDTH, MATRIX_PANEL_HEIGHT, PANEL_TYPE, INVERTED_STB);
    start_hub75_driver();
    setBasisBrightness(DISPLAY_BASE_BRIGHTNESS);
    setIntensity(DISPLAY_INTENSITY);
    printf("[boot] hub75 driver up\n");

    displayHelpers.RenderLogicalImageToGraphics(
        graphics, displayHelpers.GetImageDataForFrame(FIRST_FRAME_INDEX));
    displayHelpers.DrawWifiStatusIcon(graphics, false);
    update(&graphics);

    restored = false;
    if (memory.Initialize()) {
        if (upload.TryBeginRender()) {
            restored = player.Restore(to_ms_since_boot(get_absolute_time()));
            upload.EndRender();
        }
        if (restored) {
            printf("[memory] saved photo restored\n");
        } else {
            printf("[memory] using flashed image: %s\n", memory.GetLastError());
        }
        player.Stop();
        memory.Shutdown();
    } else {
        printf("[memory] using flashed image: %s\n", memory.GetLastError());
    }
    if (upload.TryBeginRender()) {
        if (!restored) {
            displayHelpers.RenderLogicalImageToGraphics(
                graphics, displayHelpers.GetImageDataForFrame(FIRST_FRAME_INDEX));
        }
        displayHelpers.DrawWifiStatusIcon(graphics, false);
        if (hub75_ready_for_update()) {
            update(&graphics);
        }
        upload.EndRender();
    }
    RenderLoop(displayHelpers, upload, wifiNetwork, memory, photos, player, video, graphics);
} // main
