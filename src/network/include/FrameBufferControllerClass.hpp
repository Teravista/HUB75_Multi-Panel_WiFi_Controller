#pragma once
#include "display/include/displayHelpers.hpp"
#include <cstddef>
#include <cstdint>
#include "lwip/tcp.h"
#include "config/defines.h"
#include "network/include/FrameReceiverClass.hpp"
#include "media/include/mediaMetadata.hpp"

/**
    Connects photo reception and saved-media playback to the shared CPU framebuffer.

    FrameReceiver serializes uploads and rendering so foreground SD saves can read
    stable pixels before status overlays are drawn. RGB888 network input is mapped
    into panel order and the configured CPU pixel format; readback reverses that
    mapping for persistence. The controller owns TCP receive state and references
    a caller-owned framebuffer rather than allocating another full image. Exclusive
    media ownership pauses rendering during video uploads, and verified SD frames
    are mapped and published without being mistaken for new photo uploads.
*/
class FrameBufferController {
public:
    // Connects frame reception to the CPU framebuffer and display helpers.
    explicit FrameBufferController(DisplayHelpers* helpers, unsigned char* buffer, bool holdCompletedFrame = false);

    // Copies framebuffer pixels into RGB888 output for saving.
    bool ReadFramePixels(uint8_t* pixels, size_t bytes, size_t offset) const;

    // Reports whether a photo upload is currently receiving bytes.
    bool IsFrameUploadInProgress(void) const;

    // Claims framebuffer ownership for rendering.
    bool TryBeginRender(void);

    // Releases framebuffer ownership after rendering.
    void EndRender(void);

    // Returns the completed logical frame when one is available.
    const unsigned char* GetUploadedFrameData(void) const;

    // Ownership tokens identify one complete logical RGB888 photo.
    using Token = FrameReceiver::Token;

    // Returns the revision of the most recently published frame.
    Token GetFrameRevision(void) const;

    // Starts a full-panel RGB888 photo upload with default metadata.
    Token BeginFrame(size_t bytes);

    // Starts a photo upload with the requested frame size and metadata.
    Token BeginFrame(size_t bytes, const MediaMetadata& metadata);

    // Returns metadata for the current completed frame.
    MediaMetadata GetFrameMetadata(void) const;

    // Feeds bytes to the frame upload identified by its ownership token.
    bool FeedFrame(Token owner, const uint8_t* bytes, size_t count);

    // Publishes a complete upload with its pending photo metadata.
    bool FinishFrame(Token owner);

    // Aborts the frame upload identified by its ownership token.
    void AbortFrame(Token owner);

    // Starts the raw TCP photo upload listener.
    bool StartFrameUploadServer(uint16_t port);

    // Stops the raw TCP photo upload listener and resets receive state.
    void StopFrameUploadServer(void);

    // Reports whether the raw TCP photo listener is active.
    bool IsFrameUploadServerListening(void);

    // Claims exclusive framebuffer ownership for saved-media work.
    bool TryBeginExternalMedia(void);

    // Releases exclusive saved-media framebuffer ownership.
    void EndExternalMedia(void);

    // Reports whether the current frame came from saved media.
    bool IsStoredFrame(void) const;

    // Publishes saved-media metadata and marks the frame complete.
    void PublishStoredFrame(const MediaMetadata& metadata);

    // Clears the availability of the current saved frame.
    void ClearStoredFrame(void);

    // Maps a stored pixel fragment into the placed CPU framebuffer region.
    bool WriteStoredFramePixels(const uint8_t* bytes, size_t count, size_t offset, PixelFormat format = PixelFormat::RGB888, uint16_t width = LOGICAL_IMAGE_WIDTH, uint16_t height = LOGICAL_IMAGE_HEIGHT, uint16_t left = MediaMetadata::AutoOffset, uint16_t top = MediaMetadata::AutoOffset);

    // Clears the framebuffer before writing a saved image with placement metadata.
    void PrepareStoredFrame(const MediaMetadata& metadata);

private:
    // Converts stored pixels and places them in the CPU framebuffer.
    template <PixelFormat Format> static bool MapStoredPixels(DisplayHelpers& helpers, uint8_t* buffer, const uint8_t* bytes, size_t count, size_t offset, uint16_t width, uint16_t height, uint16_t left, uint16_t top);

    // Returns elapsed boot time in milliseconds for transport deadlines.
    static uint32_t GetPhotoUploadTimeMs(void);

    // Removes TCP callbacks before closing or replacing a connection.
    static void DetachPhotoClientCallbacks(tcp_pcb* pcb);

    static constexpr uint32_t kIdleMs = 10000;

    static constexpr uint32_t kLifetimeMs = 30000;

    static constexpr size_t kAckBytes = sizeof(UPLOAD_ACK_MESSAGE) - 1;

    DisplayHelpers* m_displayHelpers;
    unsigned char* m_frameBuffer;
    FrameReceiver m_receiver;
    MediaMetadata m_metadata = MediaMetadata::Photo(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT);
    MediaMetadata m_pendingMetadata;
    bool m_fromStorage = false;
    tcp_pcb* m_listenerPcb = nullptr;
    uint16_t m_listenerPort = 0;
    tcp_pcb* m_clientPcb = nullptr;
    Token m_rawOwner = 0;
    size_t m_rawBytes = 0;
    uint32_t m_started = 0;
    uint32_t m_progress = 0;
    bool m_complete = false;
    bool m_ackQueued = false;
    size_t m_acknowledged = 0;
    // Converts one received RGB888 pixel into the CPU framebuffer format.
    void DecodeAndStorePixel(const unsigned char* bytes, size_t index);

    // Forwards a received pixel callback to its upload controller.
    static void StorePixel(void* context, const uint8_t* bytes, size_t index);

    // Installs callbacks for the active raw photo upload client.
    void AttachClient(void);

    // Resets per-client photo upload and acknowledgement state.
    void ResetClient(void);

    // Aborts the active photo TCP client and its incomplete frame.
    err_t AbortClient(void);

    // Queues the photo upload acknowledgement when TCP has send capacity.
    err_t PumpReply(void);

    // Accepts TCP photo bytes and completes or rejects the uploaded frame.
    static err_t OnTcpRecv(void* arg, tcp_pcb* pcb, pbuf* packet, err_t error);

    // Accepts a TCP photo client and initializes its receive state.
    static err_t OnTcpAccept(void* arg, tcp_pcb* pcb, err_t error);

    // Tracks acknowledged photo reply bytes and closes a completed reply.
    static err_t OnTcpSent(void* arg, tcp_pcb* pcb, u16_t length);

    // Services a pending photo reply and expires an inactive upload.
    static err_t OnTcpPoll(void* arg, tcp_pcb* pcb);

    // Releases photo upload state after a TCP connection error.
    static void OnTcpErr(void* arg, err_t error);

};
