#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hub75.hpp"
#include "config/defines.h"
#include "display/include/displayHelpers.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include "network/wifiNetwork.hpp"
#include "sd/include/MemoryManagerClass.hpp"
#include "media/include/MediaLibraryClass.hpp"
#include "media/include/MediaPlayerClass.hpp"
#include "network/include/TcpVideoUploadClass.hpp"

#define WIFI_STATE_MACHINE_REV "saved-media-library"

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
    @param library Saved-photo/video selection, slideshow and persistent settings.
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
    MediaLibrary& library,
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
    uint32_t attemptedPhotoRevision;
    uint32_t savedVideoId;

    frameIndex = FIRST_FRAME_INDEX;
    animationMovingForward = true;
    storageStarted = false;
    attemptedPhotoRevision = 0;
    lastFrameCount = 0;
    lastFpsTime = get_absolute_time();
    framesPerSecond = 0;
    lastStatusMs = to_ms_since_boot(get_absolute_time());

    for (;;) {

        // Advance network services before claiming the shared rendering buffer.
        wifiNetwork.UpdateNetworkStateMachine();
        if (!storageStarted && wifiNetwork.IsRadioInitialized()) {
            storageStarted = true;
            if (memory.Initialize()) {
                library.Initialize();
                if (!library.WantsMedia()) {
                    player.RequestReload();
                }
            } else {
                printf("[memory] saves unavailable: %s\n", memory.GetLastError());
            }
        }

        // Finish deferred upload or verification work and schedule any saved-media reload.
        video.Update();
        reloadVideo = video.TakeReloadRequest();

        if (!upload.TryBeginRender()) {
            sleep_ms(UPLOAD_POLL_DELAY_1MS);
            continue;
        }

        // Adopt a completed library upload only after the display transaction releases ownership.
        if (reloadVideo) {
            savedVideoId = video.TakeSavedLibraryId();
            if (savedVideoId) {
                library.NotifyVideoSaved(savedVideoId);
            } else if (library.WantsMedia()) {
                library.RequestReload();
            } else {
                player.RequestReload();
            }
        }

        // Save each raw photo revision once, before connection overlays modify the source pixels.
        if (!reloadVideo && upload.GetUploadedFrameData() && !upload.IsStoredFrame() &&
            upload.GetFrameRevision() != attemptedPhotoRevision) {
            attemptedPhotoRevision = upload.GetFrameRevision();
            player.Stop();
            if (!library.ImportLivePhoto()) {
                printf("[memory] photo save failed: %s; keeping live photo\n", memory.GetLastError());
            }
        }
        if (library.WantsMedia()) {
            library.Update(to_ms_since_boot(get_absolute_time()));
        } else {
            player.Update(to_ms_since_boot(get_absolute_time()));
        }

        // Draw the fallback only when no verified or newly uploaded frame is available.
        uploadedFrame = upload.GetUploadedFrameData();
        if (!uploadedFrame) {
            displayHelpers.RenderLogicalImageToGraphics(
                graphics, displayHelpers.GetImageDataForFrame(frameIndex));
            frameIndex = displayHelpers.GetNextAnimationFrameIndex(frameIndex, animationMovingForward);
        }
        displayHelpers.DrawWifiStatusIcon(graphics, wifiNetwork.IsNetworkReady());

        // Measure completed panel scans independently of saved-video frame selection.
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

        // Submit only when the driver can accept a complete frame, then release render ownership.
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
    static TcpVideoUpload video(memory, upload, true);
    WifiNetwork wifiNetwork(&upload, &video);
    MediaPlayer player(memory, upload);
    static MediaLibrary library(memory, upload, player);
    CpuGraphics graphics(MATRIX_PANEL_WIDTH, MATRIX_PANEL_HEIGHT, frameBuffer);
    bool restored;

    // Start clocks, serial output and panel scanning before performing storage or Wi-Fi work.
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

    // Queue visible fallback content while the saved-media slot is inspected.
    displayHelpers.RenderLogicalImageToGraphics(
        graphics, displayHelpers.GetImageDataForFrame(FIRST_FRAME_INDEX));
    displayHelpers.DrawWifiStatusIcon(graphics, false);
    update(&graphics);

    // Restore a checked saved frame, retaining the fallback when storage or decoding fails.
    restored = false;
    if (memory.Initialize()) {
        if (upload.TryBeginRender()) {
            library.Initialize();
            if (library.WantsMedia()) {
                restored = library.Update(to_ms_since_boot(get_absolute_time()));
            } else {
                restored = player.Restore(to_ms_since_boot(get_absolute_time()));
            }
            upload.EndRender();
        }
        if (restored) {
            printf("[memory] saved photo restored\n");
        } else {
            printf("[memory] using flashed image: %s\n", memory.GetLastError());
        }

        // Release the startup SD session before the initial radio resource claim.
        player.Stop();
        memory.Shutdown();
    } else {
        printf("[memory] using flashed image: %s\n", memory.GetLastError());
    }

    // Submit restored content before entering the foreground service loop.
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
    RenderLoop(displayHelpers, upload, wifiNetwork, memory, library, player, video, graphics);
} // main
