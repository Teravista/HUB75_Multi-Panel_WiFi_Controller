#include "core/include/serialLog.hpp"
#include <cstdint>
#include "http/include/webServer.hpp"
#include "http/include/HttpMediaUploadClass.hpp"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hub75.hpp"
#include "config/defines.h"
#include "display/include/displayHelpers.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include "network/include/WifiConnectionClass.hpp"
#include "network/include/networkCore.hpp"
#include "sd/include/MemoryManagerClass.hpp"
#include "media/include/MediaLibraryClass.hpp"
#include "media/include/MediaPlayerClass.hpp"
#include "network/include/TcpVideoUploadClass.hpp"

#define SERIAL_STATUS_LINE_BYTES 256

#define WIFI_STATE_MACHINE_REV "runtime-status"

struct NetworkPauseContext {
    bool* servicesPaused;
};

/**
    Yields the network worker while waiting for a foreground acknowledgement.
*/
static void
YieldNetworkWorker (
    void
    )
{
    sleep_ms(UPLOAD_POLL_DELAY_1MS);
} // YieldNetworkWorker

/**
    Handles a network lifecycle request at a safe foreground boundary.

    Pauses foreground transport users only while Core 1 changes listeners. Radio restart
    never releases SD resources or changes saved playback. These acknowledgements occur
    outside storage/render operations, avoiding concurrent transport teardown.

    @param opaque Foreground-owned transport pause flag.
    @param command Requested listener lifecycle boundary.
*/
static void
HandleNetworkPauseRequest (
    void* opaque,
    NetworkServices::Command command
    )
{
    NetworkPauseContext* context = nullptr;
    context = static_cast<NetworkPauseContext*>(opaque);
    if (command == NetworkServices::Command::PauseServices) {
        *context->servicesPaused = true;
    } else if (command == NetworkServices::Command::ResumeServices) {
        *context->servicesPaused = false;
    }
} // HandleNetworkPauseRequest

/**
    Prepares display frames and advances foreground storage and media services.

    Core 1 handles Wi-Fi and transport work. This loop acknowledges its lifecycle
    requests before accessing storage. Rendering claims the CPU frame so callbacks
    cannot replace pixels while persistence reads them. Each
    completed upload is saved once before drawing the Wi-Fi status overlay.
    The scanner continues showing its last submitted buffer during SD work.

    @param displayHelpers Logical-to-physical panel mapping and status overlay.
    @param upload Shared framebuffer and raw photo receiver.
    @param network Cross-core network status and lifecycle command mailbox.
    @param memory SD image-storage boundary.
    @param library Saved-photo/video selection, slideshow and persistent settings.
    @param player Verified saved-media playback scheduler.
    @param video Bounded raw-video upload and verification service.
    @param http Foreground browser video upload coordinator.
    @param graphics Shared CPU graphics source buffer.
*/
static void
RenderLoop (
    DisplayHelpers& displayHelpers,
    FrameBufferController& upload,
    NetworkServices& network,
    MemoryManager& memory,
    MediaLibrary& library,
    MediaPlayer& player,
    TcpVideoUpload& video,
    HttpMediaUpload& http,
    CpuGraphics& graphics
    )
{
    size_t frameIndex;
    bool animationMovingForward;
    bool servicesPaused;
    bool storageLost;
    bool storageRestored;
    MemoryManager::Event storageEvent;
    NetworkServices::Status networkStatus;
    NetworkServices::Handler handler;
    NetworkPauseContext context;
    bool reloadVideo;
    bool reloadHttp;
    uint32_t lastFrameCount;
    uint32_t lastStatusMs;
    uint32_t uptimeMs;
    uint32_t currentFrameCount;
    uint32_t lastVideoFrameCount;
    uint32_t videoFrameCount;
    SerialLog::Status status;
    char statusLine[SERIAL_STATUS_LINE_BYTES];
    const unsigned char* uploadedFrame = nullptr;
    uint32_t attemptedPhotoRevision;
    uint32_t savedVideoId;

    frameIndex = FIRST_FRAME_INDEX;
    animationMovingForward = true;
    servicesPaused = false;
    storageLost = false;
    storageRestored = false;
    reloadVideo = false;
    reloadHttp = false;
    context = {&servicesPaused};
    handler = {&context, HandleNetworkPauseRequest};
    attemptedPhotoRevision = 0;
    lastFrameCount = uint32_t(hub75_frame_count);
    lastVideoFrameCount = player.GetVideoFramesPresented();
    lastStatusMs = to_ms_since_boot(get_absolute_time());

    for (;;) {

        // Sample and report even while upload ownership or listener handoffs pause rendering.
        uptimeMs = to_ms_since_boot(get_absolute_time());
        currentFrameCount = uint32_t(hub75_frame_count);
        library.SampleFrameRates(uptimeMs, currentFrameCount);
        network.ProcessRequests(handler);
        networkStatus = network.ReadStatus();
        if (uint32_t(uptimeMs - lastStatusMs) >= STATUS_PRINT_INTERVAL_10S) {
            videoFrameCount = player.GetVideoFramesPresented();
            status = {};
            status.uptimeSeconds = uptimeMs / MILLISECONDS_PER_SECOND_U;
            status.displayTenths = uint32_t(uint64_t(uint32_t(currentFrameCount - lastFrameCount)) *
                FPS_TENTHS_PER_MILLISECOND_SCALE / uint32_t(uptimeMs - lastStatusMs));
            status.videoTenths = uint32_t(uint64_t(uint32_t(videoFrameCount - lastVideoFrameCount)) *
                FPS_TENTHS_PER_MILLISECOND_SCALE / uint32_t(uptimeMs - lastStatusMs));
            status.video = player.IsPlayingVideo();
            status.targetNumerator = player.GetEffectiveFpsNumerator();
            status.targetDenominator = player.GetEffectiveFpsDenominator();
            if (memory.IsMediaSaveActive()) {
                status.mode = "upload";
            } else {
                status.mode = library.ActiveMode();
            }
            status.network = networkStatus.state;
            status.ip = networkStatus.ip;
            status.storage = memory.GetStateName();
            status.sdRetryMs = memory.GetRetryInMs(uptimeMs);
            status.job = memory.GetActivityName();
            if (SerialLog::FormatStatus(statusLine, sizeof(statusLine), status)) {
                std::puts(statusLine);
            }
            lastFrameCount = currentFrameCount;
            lastVideoFrameCount = videoFrameCount;
            lastStatusMs = uptimeMs;
        }
        if (servicesPaused) {
            sleep_ms(UPLOAD_POLL_DELAY_1MS);
            continue;
        }

        // Recover storage independently, and discard old-card selections before accepting new jobs.
        storageEvent = memory.Update(to_ms_since_boot(get_absolute_time()));
        if (storageEvent == MemoryManager::Event::Unavailable) {
            storageLost = true;
            storageRestored = false;
        } else if (storageEvent == MemoryManager::Event::Ready) {
            storageRestored = true;
        }
        if ((storageLost || storageRestored) && upload.TryBeginRender()) {
            if (storageLost) {
                library.StorageUnavailable();
                storageLost = false;
            }
            if (storageRestored) {
                storageRestored = false;
                if (library.Initialize()) {
                    if (library.WantsMedia()) {
                        library.RequestReload();
                    } else {
                        player.RequestReload();
                    }
                }
            }
            upload.EndRender();
        }

        // Storage runs only here; transport callbacks and reply pumping run on Core 1.
        video.UpdateStorage();
        WebServer::UpdateStorage();
        reloadVideo = video.TakeReloadRequest() || reloadVideo;
        reloadHttp = http.TakeReloadRequest() || reloadHttp;

        if (!upload.TryBeginRender()) {
            sleep_ms(UPLOAD_POLL_DELAY_1MS);
            continue;
        }

        // Adopt a completed library upload only after the display transaction releases ownership.
        if (reloadVideo || reloadHttp) {
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
        if (!reloadVideo && !reloadHttp && upload.GetUploadedFrameData() && !upload.IsStoredFrame() &&
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

        reloadVideo = false;
        reloadHttp = false;

        // Draw the fallback only when no verified or newly uploaded frame is available.
        uploadedFrame = upload.GetUploadedFrameData();
        if (!uploadedFrame) {
            displayHelpers.RenderLogicalImageToGraphics(
                graphics, displayHelpers.GetImageDataForFrame(frameIndex));
            frameIndex = displayHelpers.GetNextAnimationFrameIndex(frameIndex, animationMovingForward);
        }
        displayHelpers.DrawWifiStatusIcon(graphics, networkStatus.ready);

        // Submit only when the driver can accept a complete frame, then release render ownership.
        if (hub75_ready_for_update()) {
            update(&graphics);
            player.RecordFramePresented();
        }
        upload.EndRender();
        sleep_ms(ANIMATION_FRAME_DELAY_MS);
    }
} // RenderLoop

/**
    Initializes the controller and restores a saved photo before networking starts.

    The flashed frame is queued first so display scanning has visible content
    during SD probing. A verified saved photo or first video frame replaces it; missing or damaged
    storage leaves the flashed image available. The mounted SD session remains
    independent of Core 1 radio initialization and connection retries.

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
    WifiConnection wifiNetwork(&upload, &video);
    NetworkServices network(wifiNetwork, YieldNetworkWorker, &video);
    MediaPlayer player(memory, upload);
    static MediaLibrary library(memory, upload, player);
    static HttpMediaUpload http(memory, upload);
    CpuGraphics graphics(MATRIX_PANEL_WIDTH, MATRIX_PANEL_HEIGHT, frameBuffer);
    bool restored;

    http.SetMediaLibrary(&library);
    WebServer::SetMediaUpload(&http);

    // Start clocks, serial output and panel scanning before performing storage or Wi-Fi work.
    set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true);
    stdio_init_all();
#if HUB75_VERBOSE_LOGS
    sleep_ms(STARTUP_DELAY_5S);
#endif
    printf("\n[boot] hub75 starting (%s)\n", WIFI_STATE_MACHINE_REV);
    create_hub75_driver(MATRIX_PANEL_WIDTH, MATRIX_PANEL_HEIGHT, PANEL_TYPE, INVERTED_STB);
    start_hub75_driver();
    setBasisBrightness(DISPLAY_BASE_BRIGHTNESS);
    setIntensity(DISPLAY_INTENSITY);
    HUB75_DEBUG("[boot] hub75 driver up\n");

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
        if (!restored) {
            std::puts("[playback] built-in (saved media unavailable)");
        }

        // SD retains its SPI/DMA resources while Core 1 initializes or reconnects the radio.
    } else {
        std::puts("[playback] built-in (SD unavailable)");
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
            player.RecordFramePresented();
        }
        upload.EndRender();
    }
    StartNetworkCore(network);
    RenderLoop(displayHelpers, upload, network, memory, library, player, video, http, graphics);
} // main
