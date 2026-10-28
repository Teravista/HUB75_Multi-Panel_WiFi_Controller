#include "core/include/serialLog.hpp"
#include <cstdint>
#include "display/include/RenderPipelineClass.hpp"
#include "http/include/webServer.hpp"
#include "http/include/HttpMediaUploadClass.hpp"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hub75.hpp"
#include "config/defines.h"
#include "display/include/PanelRendererClass.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include "network/include/WifiConnectionClass.hpp"
#include "network/include/networkCore.hpp"
#include "sd/include/MemoryManagerClass.hpp"
#include "media/include/MediaLibraryClass.hpp"
#include "media/include/MediaPlayerClass.hpp"
#include "network/include/TcpVideoUploadClass.hpp"

#define SERIAL_STATUS_LINE_BYTES 320

#define WIFI_STATE_MACHINE_REV "dual-wifi-client-dots"


struct NetworkPauseContext {
    bool* servicesPaused;
    RenderPipeline* pipeline;
};

/**
    Borrows foreground state for synchronous preparation and display submission.

    The render loop owns every referenced object and keeps this record alive while a prepared
    frame waits for the scanner. Callbacks do not own storage, buffers or network state, and do
    not outlive the loop iteration that invokes them.
*/
struct RenderContext {
    PanelRenderer* renderer;
    FrameBufferController* upload;
    MemoryManager* memory;
    MediaLibrary* library;
    MediaPlayer* player;
    TcpVideoUpload* video;
    CpuGraphics* graphics;
    NetworkServices::Status* networkStatus;
    size_t* frameIndex;
    bool* animationMovingForward;
    bool* reloadVideo;
    bool* reloadHttp;
    uint32_t* attemptedPhotoRevision;
    const unsigned char** uploadedFrame;
    FrameBufferController::Token* displayedRevision;
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
    context->pipeline->Reset();
    if (command == NetworkServices::Command::PauseServices) {
        *context->servicesPaused = true;
    } else if (command == NetworkServices::Command::ResumeServices) {
        *context->servicesPaused = false;
    }
} // HandleNetworkPauseRequest

/**
    Prepares the next saved or built-in frame under exclusive CPU-frame ownership.

    Adopts completed uploads and persists a new live photo before drawing overlays. Saved-media
    selection and frame timing advance through the existing library/player APIs. If no usable
    frame exists, the built-in image is mapped into the same caller-owned CPU buffer. Preparation
    is synchronous; RenderPipeline retains ownership until submission or cancellation.

    @param opaque Borrowed foreground rendering context.
*/
static void
PrepareDisplayFrame (
    void* opaque
    )
{
    RenderContext* render = nullptr;
    uint32_t savedVideoId;

    render = static_cast<RenderContext*>(opaque);

    // Adopt a completed library upload only after the display transaction releases ownership.
    if ((*render->reloadVideo) || (*render->reloadHttp)) {
        savedVideoId = render->video->TakeSavedLibraryId();
        if (savedVideoId) {
            render->library->NotifyVideoSaved(savedVideoId);
        } else if (render->library->WantsMedia()) {
            render->library->RequestReload();
        } else {
            render->player->RequestReload();
        }
    }

    // Save each raw photo revision once, before connection overlays modify the source pixels.
    if (!(*render->reloadVideo) && !(*render->reloadHttp) && render->upload->GetUploadedFrameData() && !render->upload->IsStoredFrame() &&
        render->upload->GetFrameRevision() != (*render->attemptedPhotoRevision)) {
        (*render->attemptedPhotoRevision) = render->upload->GetFrameRevision();
        render->player->Stop();
        if (!render->library->ImportLivePhoto()) {
            printf("[memory] photo save failed: %s; keeping live photo\n", render->memory->GetLastError());
        }
    }
    if (render->library->WantsMedia()) {
        render->library->Update(to_ms_since_boot(get_absolute_time()));
    } else {
        render->player->Update(to_ms_since_boot(get_absolute_time()));
    }

    (*render->reloadVideo) = false;
    (*render->reloadHttp) = false;

    // Draw the fallback only when no verified or newly uploaded frame is available.
    (*render->uploadedFrame) = render->upload->GetUploadedFrameData();
    if (!(*render->uploadedFrame)) {
        render->renderer->RenderLogicalImageToGraphics(
            *render->graphics, render->renderer->GetImageDataForFrame((*render->frameIndex)));
        (*render->frameIndex) = render->renderer->GetNextAnimationFrameIndex((*render->frameIndex), (*render->animationMovingForward));
    }
} // PrepareDisplayFrame

/**
    Draws the status overlay and submits a prepared frame to the display driver.

    Runs only after the scanner accepts another frame. A changed source revision refreshes the
    small saved background beneath both network labels; overlay-only redraws restore that background.
    The completed submission advances the distinct video-frame counter before the pipeline
    releases CPU-frame ownership.

    @param opaque Borrowed foreground rendering context.
    @param scannerWaitUs Elapsed scanner wait in microseconds, supplied by the pipeline.
*/
static void
SubmitDisplayFrame (
    void* opaque,
    uint64_t scannerWaitUs
    )
{
    RenderContext* render = nullptr;
    bool freshFrame;

    (void)scannerWaitUs;
    render = static_cast<RenderContext*>(opaque);
    freshFrame = *render->uploadedFrame == nullptr ||
        render->upload->GetFrameRevision() != *render->displayedRevision;
    render->renderer->DrawNetworkStatus(*render->graphics,
        render->networkStatus->homeReady, render->networkStatus->homeIp,
        render->networkStatus->hotspotReady, render->networkStatus->hotspotIp, freshFrame,
        render->networkStatus->hotspotDevices);
    update(render->graphics);
    *render->displayedRevision = render->upload->GetFrameRevision();
    render->player->RecordFramePresented();
} // SubmitDisplayFrame

/**
    Prepares display frames and advances foreground storage and media services.

    Core 1 handles Wi-Fi and transport work. This loop acknowledges its lifecycle
    requests before accessing storage. Rendering claims the CPU frame so callbacks
    cannot replace pixels while persistence reads them. Each
    completed upload is saved once before drawing the Wi-Fi status overlay.
    The scanner continues showing its last submitted buffer during SD work.
    Prepared CPU pixels remain owned across scanner waits; a bounded timeout releases the frame
    without resetting display hardware or preventing foreground maintenance.

    @param renderer Logical-to-physical panel mapping and status overlay.
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
    PanelRenderer& renderer,
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
    size_t hotspotDeviceIndex;
    SerialLog::Status status;
    char statusLine[SERIAL_STATUS_LINE_BYTES];
    const unsigned char* uploadedFrame = nullptr;
    uint32_t attemptedPhotoRevision;
    FrameBufferController::Token displayedRevision;
    RenderPipeline::Result submission;
    RenderContext render;
    RenderPipeline pipeline(upload);

    frameIndex = FIRST_FRAME_INDEX;
    animationMovingForward = true;
    servicesPaused = false;
    storageLost = false;
    storageRestored = false;
    reloadVideo = false;
    reloadHttp = false;
    context = {&servicesPaused, &pipeline};
    handler = {&context, HandleNetworkPauseRequest};
    attemptedPhotoRevision = 0;
    displayedRevision = 0;
    render = {&renderer, &upload, &memory, &library, &player, &video, &graphics,
              &networkStatus, &frameIndex, &animationMovingForward, &reloadVideo, &reloadHttp,
              &attemptedPhotoRevision, &uploadedFrame, &displayedRevision};
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
            status.homeNetwork = networkStatus.homeState;
            status.homeIp = networkStatus.homeIp;
            status.hotspotNetwork = networkStatus.hotspotState;
            status.hotspotIp = networkStatus.hotspotIp;

            // Count idle and active associations from the same snapshot as the panel dots.
            for (hotspotDeviceIndex = 0; hotspotDeviceIndex < WIFI_HOTSPOT_MAX_CLIENTS; ++hotspotDeviceIndex) {
                if (networkStatus.hotspotDevices[hotspotDeviceIndex] != WIFI_HOTSPOT_CLIENT_OFF) {
                    ++status.hotspotClients;
                }
            }
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
        if (storageEvent != MemoryManager::Event::None) {
            pipeline.Reset();
        }
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

        // Prepare once, retaining ownership until the independent scanner can accept the frame.
        pipeline.TryPrepare(hub75_ready_for_update(), {&render, PrepareDisplayFrame});
        submission = pipeline.TrySubmit(hub75_ready_for_update(), {&render, SubmitDisplayFrame});
        if (submission == RenderPipeline::Result::Stalled) {
            std::puts("[display] scanner wait timed out; frame ownership released");
        }
        if (submission != RenderPipeline::Result::Submitted) {
            sleep_ms(UPLOAD_POLL_DELAY_1MS);
            continue;
        }
        sleep_ms(ANIMATION_FRAME_DELAY_MS);
    }
} // RenderLoop

/**
    Constructs the controller and starts display, storage and network services.

    Clocks and USB are initialized by main before any controller constructor runs.
    The flashed frame is queued first so display scanning has visible content
    during SD probing. A verified saved photo or first video frame replaces it; missing or damaged
    storage leaves the flashed image available. The mounted SD session remains
    independent of Core 1 radio initialization and connection retries.

*/
static void __attribute__((noinline))
RunController (
    void
    )
{
    alignas(DMA_BUFFER_ALIGNMENT) static unsigned char frameBuffer[FRAME_BUFFER_SIZE_BYTES];
    PanelRenderer renderer;
    FrameBufferController upload(&renderer, frameBuffer, true);
    MemoryManager memory;
    static TcpVideoUpload video(memory, upload, true);
    // Keep the long-lived DHCP workspace off the foreground render stack.
    static WifiConnection wifiNetwork(&upload, &video);
    NetworkServices network(wifiNetwork, YieldNetworkWorker, &video);
    MediaPlayer player(memory, upload);
    static MediaLibrary library(memory, upload, player);
    static HttpMediaUpload http(memory, upload);
    CpuGraphics graphics(MATRIX_PANEL_WIDTH, MATRIX_PANEL_HEIGHT, frameBuffer);
    bool restored;

    http.SetMediaLibrary(&library);
    WebServer::SetMediaUpload(&http);

    HUB75_DEBUG("[boot] controllers constructed\n");

    // Start scanning only after USB startup and controller construction complete.
    create_hub75_driver(MATRIX_PANEL_WIDTH, MATRIX_PANEL_HEIGHT, PANEL_TYPE, INVERTED_STB);
    start_hub75_driver();
    setBasisBrightness(DISPLAY_BASE_BRIGHTNESS);
    setIntensity(DISPLAY_INTENSITY);
    HUB75_DEBUG("[boot] hub75 driver up\n");

    // Queue visible fallback content while the saved-media slot is inspected.
    renderer.RenderLogicalImageToGraphics(
        graphics, renderer.GetImageDataForFrame(FIRST_FRAME_INDEX));
    HUB75_DEBUG("[boot] drawing initial network indicators\n");
    renderer.DrawNetworkStatus(graphics, false, nullptr, false, nullptr, true);
    update(&graphics);
    HUB75_DEBUG("[boot] initial frame submitted\n");

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
            renderer.RenderLogicalImageToGraphics(
                graphics, renderer.GetImageDataForFrame(FIRST_FRAME_INDEX));
        }
        renderer.DrawNetworkStatus(graphics, false, nullptr, false, nullptr, true);
        if (hub75_ready_for_update()) {
            update(&graphics);
            player.RecordFramePresented();
        }
        upload.EndRender();
    }
    HUB75_DEBUG("[boot] starting network worker\n");
    StartNetworkCore(network);
    RenderLoop(renderer, upload, network, memory, library, player, video, http, graphics);
} // RunController

/**
    Initializes clocks and USB before constructing controller services.

    USB starts independently of controller constructors, display allocation and
    SD restoration, so a later startup fault does not prevent the initial serial
    connection. Verbose builds retain the bounded serial attachment delay.

    @return 0 - The controller loop returned to the firmware entry point.
*/
int
main (
    void
    )
{
    set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true);
    stdio_init_all();
#if HUB75_VERBOSE_LOGS
    sleep_ms(STARTUP_DELAY_5S);
#endif
    printf("\n[boot] hub75 starting (%s)\n", WIFI_STATE_MACHINE_REV);
    HUB75_DEBUG("[boot] constructing controllers\n");
    RunController();
    return 0;
} // main
