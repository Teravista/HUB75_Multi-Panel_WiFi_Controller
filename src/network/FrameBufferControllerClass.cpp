#include "core/include/serialLog.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include <cstring>
#include "display/include/PanelRendererClass.hpp"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "network/include/clientActivity.hpp"
#include <cstdio>

/**
    Returns elapsed boot time in milliseconds for transport deadlines.

    @return value - Monotonic time since boot in milliseconds, wrapping at the counter width.
*/
uint32_t
FrameBufferController::GetPhotoUploadTimeMs (
    void
    )
{
    return to_ms_since_boot(get_absolute_time());
} // GetPhotoUploadTimeMs

/**
    Removes TCP callbacks before closing or replacing a connection.

    @param pcb lwIP TCP connection associated with this callback or operation.
*/
void
FrameBufferController::DetachPhotoClientCallbacks (
    tcp_pcb* pcb
    )
{
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_err(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
} // DetachPhotoClientCallbacks

/**
    Connects frame reception to the CPU framebuffer and display helpers.

    @param helpers Logical-to-physical pixel mapping and display overlay helpers.
    @param buffer Writable destination buffer; the caller must provide sufficient storage.
    @param holdCompletedFrame Whether the completed upload retains ownership until explicitly
                              released.
*/
FrameBufferController::FrameBufferController (
    PanelRenderer* helpers,
    unsigned char* buffer,
    bool holdCompletedFrame
    ) : m_panelRenderer(helpers), m_frameBuffer(buffer),
      m_receiver(SOURCE_FRAME_SIZE_BYTES, this, StorePixel, holdCompletedFrame)
{
    return;
} // FrameBufferController

/**
    Copies framebuffer pixels into RGB888 output for saving.

    Foreground only, under render ownership and before status overlays.

    @param pixels Writable destination buffer; the caller must provide sufficient storage.
    @param bytes Number of bytes to process.
    @param offset Byte offset within the source image or file.

    @return true - The requested pixel chunk was accepted or copied.
    @return false - The pixel range, buffer, format or ownership is invalid.
*/
bool
FrameBufferController::ReadFramePixels (
    uint8_t* pixels,
    size_t bytes,
    size_t offset
    ) const
{
    size_t i;
    size_t index;
    int x;
    int y;
    size_t source;
    uint32_t pixel;

    if (!pixels || !m_receiver.HasFrame() || offset > SOURCE_FRAME_SIZE_BYTES ||
        bytes > SOURCE_FRAME_SIZE_BYTES - offset || offset % IMAGE_BYTES_PER_PIXEL ||
        bytes % IMAGE_BYTES_PER_PIXEL) {
        return false;
    }
    for (i = 0; i < bytes; i += IMAGE_BYTES_PER_PIXEL) {
        index = (offset + i) / IMAGE_BYTES_PER_PIXEL;
        x = 0;
        y = 0;
        m_panelRenderer->ConvertLogicalToPhysicalCoordinates(int(index % LOGICAL_IMAGE_WIDTH),
                                                              int(index / LOGICAL_IMAGE_WIDTH), x, y);
        source = (y * MATRIX_PANEL_WIDTH + x) * CpuFrame::PixelBytes;
        pixel = CpuFrame::LoadRgbPixelAsRgb888(m_frameBuffer + source);
        pixels[i + IMAGE_BLUE_BYTE_OFFSET] = uint8_t(pixel);
        pixels[i + IMAGE_GREEN_BYTE_OFFSET] = uint8_t(pixel >> RGB888_GREEN_SHIFT);
        pixels[i + IMAGE_RED_BYTE_OFFSET] = uint8_t(pixel >> RGB888_RED_SHIFT);
    }

    return true;
} // ReadFramePixels

/**
    Forwards a received pixel callback to its upload controller.

    @param context Caller-owned state passed unchanged to the callback.
    @param bytes Read-only input byte buffer.
    @param index Zero-based index of the requested entry, slot or frame.
*/
void
FrameBufferController::StorePixel (
    void* context,
    const uint8_t* bytes,
    size_t index
    )
{
    static_cast<FrameBufferController*>(context)->DecodeAndStorePixel(bytes, index);
} // StorePixel

/**
    Converts one received RGB888 pixel into the CPU framebuffer format.

    Turns the logical upload index into panel-chain coordinates using the shared renderer, then
    writes one converted pixel into the physical CPU framebuffer. The receiver has already
    assembled a complete RGB888 pixel and validated upload ownership before invoking this sink.

    @param pixelBytes Source RGB888 bytes for one pixel.
    @param pixelIndex Zero-based pixel position in the logical image.
*/
void
FrameBufferController::DecodeAndStorePixel (
    const unsigned char* pixelBytes,
    size_t pixelIndex
    )
{
    int logicalX;
    int logicalY;
    int physicalX;
    int physicalY;
    size_t destIndex;

    logicalX = static_cast<int>(pixelIndex % LOGICAL_IMAGE_WIDTH);
    logicalY = static_cast<int>(pixelIndex / LOGICAL_IMAGE_WIDTH);
    physicalX = 0;
    physicalY = 0;
    m_panelRenderer->ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, physicalX, physicalY);
    destIndex = (physicalY * MATRIX_PANEL_WIDTH + physicalX) * CpuFrame::PixelBytes;
    // Preserve existing physical wiring correction for both transports.
    CpuFrame::StoreRgbPixel(m_frameBuffer + destIndex, pixelBytes[IMAGE_RED_BYTE_OFFSET],
                            pixelBytes[IMAGE_GREEN_BYTE_OFFSET], pixelBytes[IMAGE_BLUE_BYTE_OFFSET]);
} // DecodeAndStorePixel

/**
    Installs callbacks for the active raw photo upload client.
*/
void
FrameBufferController::AttachClient (
    void
    )
{
    tcp_arg(m_clientPcb, this);
    tcp_recv(m_clientPcb, OnTcpRecv);
    tcp_sent(m_clientPcb, OnTcpSent);
    tcp_err(m_clientPcb, OnTcpErr);
    tcp_poll(m_clientPcb, OnTcpPoll, TCP_POLL_INTERVAL_TICKS);
} // AttachClient

/**
    Resets per-client photo upload and acknowledgement state.
*/
void
FrameBufferController::ResetClient (
    void
    )
{
    AbortFrame(m_rawOwner);
    m_rawOwner = 0;
    m_clientPcb = nullptr;
    m_rawBytes = 0;
    m_complete = m_ackQueued = false;
    m_acknowledged = 0;
} // ResetClient

/**
    Aborts the active photo TCP client and its incomplete frame.

    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
FrameBufferController::AbortClient (
    void
    )
{
    if (m_clientPcb) {
        DetachPhotoClientCallbacks(m_clientPcb);
        tcp_abort(m_clientPcb);
    }
    ResetClient();
    return ERR_ABRT;
} // AbortClient

/**
    Queues the photo upload acknowledgement when TCP has send capacity.

    Queues the completed-photo acknowledgement only when TCP has send capacity. Temporary memory
    pressure preserves the current reply state for sent/poll retries; the connection and held
    completed frame remain associated until acknowledgement finishes. Fatal send errors abort the
    client and release its upload ownership.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
FrameBufferController::PumpReply (
    void
    )
{
    err_t result;
    err_t nextResult;
    err_t nextNextResult;

    if (!m_complete) {
        return ERR_OK;
    }

    // Queue the acknowledgement once; temporary send pressure retries on a later callback.
    if (!m_ackQueued) {
        result = tcp_write(m_clientPcb, UPLOAD_ACK_MESSAGE, kAckBytes, TCP_WRITE_FLAG_COPY);
        if (result == ERR_MEM) {
            return ERR_OK;
        }
        if (result != ERR_OK) {
            return AbortClient();
        }
        m_ackQueued = true;
    }

    // Close only after the peer has acknowledged the complete reply.
    if (m_acknowledged == kAckBytes) {
        DetachPhotoClientCallbacks(m_clientPcb);
        nextResult = tcp_close(m_clientPcb);
        if (nextResult == ERR_OK) {
            ResetClient();
            return ERR_OK;
        }
        if (nextResult == ERR_MEM) {
            AttachClient();
            return ERR_OK;
        }
        return AbortClient();
    }

    // Ask lwIP to transmit the queued response bytes.
    nextNextResult = tcp_output(m_clientPcb);
    if (nextNextResult == ERR_OK || nextNextResult == ERR_MEM) {
        return ERR_OK;
    } else {
        return AbortClient();
    }
} // PumpReply

/**
    Accepts TCP photo bytes and completes or rejects the uploaded frame.

    Feeds each received packet-chain segment into the current raw-photo ownership token and
    releases the lwIP buffers after consumption. A complete validated frame is published before
    its acknowledgement is pumped. Transport errors, premature closure or rejected bytes abort the
    client; the callback does not wait for SD persistence, which the foreground performs after
    publication.

    @param arg Instance pointer supplied when registering the lwIP callback.
    @param pcb lwIP TCP connection associated with this callback or operation.
    @param packet Received lwIP packet chain; null signals that the peer closed the connection.
    @param error Error code or diagnostic associated with this operation.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
FrameBufferController::OnTcpRecv (
    void* arg,
    tcp_pcb* pcb,
    pbuf* packet,
    err_t error
    )
{
    FrameBufferController* self = nullptr;
    bool valid;
    pbuf* part = nullptr;

    self = static_cast<FrameBufferController*>(arg);

    // Handle transport failure and peer closure before consuming any packet data.
    if (error != ERR_OK) {
        if (packet) {
            pbuf_free(packet);
        }
        return self->AbortClient();
    }
    if (!packet) {
        if (self->m_complete) {
            return self->PumpReply();
        } else {
            return self->AbortClient();
        }
    }

    // Keep every segment within the current photo transaction and its declared frame size.
    valid = !self->m_complete && packet->tot_len <= SOURCE_FRAME_SIZE_BYTES - self->m_rawBytes;
    if (valid) {
        for (part = packet; part && valid; part = part->next) {
            valid = self->FeedFrame(self->m_rawOwner, static_cast<const uint8_t*>(part->payload), part->len);
        }
        self->m_rawBytes += packet->tot_len;
    }
    if (valid && packet->tot_len) {
        ClientActivity::RecordCommunication(pcb);
    }
    tcp_recved(pcb, packet->tot_len);

    // Return the receive buffer to lwIP after its bytes have been consumed.
    pbuf_free(packet);
    if (!valid) {
        return self->AbortClient();
    }

    // Publish a complete photo before sending its acknowledgement; SD saving happens later.
    self->m_progress = GetPhotoUploadTimeMs();
    if (self->m_rawBytes == SOURCE_FRAME_SIZE_BYTES) {
        if (!self->FinishFrame(self->m_rawOwner)) {
            return self->AbortClient();
        }
        self->m_rawOwner = 0;
        self->m_complete = true;
    }
    return self->PumpReply();
} // OnTcpRecv

/**
    Tracks acknowledged photo reply bytes and closes a completed reply.

    @param arg Instance pointer supplied when registering the lwIP callback.
    @param pcb Unused connection argument required by the TCP callback interface.
    @param length Number of bytes to process.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
FrameBufferController::OnTcpSent (
    void* arg,
    [[maybe_unused]] tcp_pcb* pcb,
    u16_t length
    )
{
    FrameBufferController* self = nullptr;

    self = static_cast<FrameBufferController*>(arg);
    if (!self->m_ackQueued || length > kAckBytes - self->m_acknowledged) {
        return self->AbortClient();
    }
    self->m_acknowledged += length;
    self->m_progress = GetPhotoUploadTimeMs();
    return self->PumpReply();
} // OnTcpSent

/**
    Services a pending photo reply and expires an inactive upload.

    @param arg Instance pointer supplied when registering the lwIP callback.
    @param pcb Unused connection argument required by the TCP callback interface.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
FrameBufferController::OnTcpPoll (
    void* arg,
    [[maybe_unused]] tcp_pcb* pcb
    )
{
    FrameBufferController* self = nullptr;
    uint32_t now;

    self = static_cast<FrameBufferController*>(arg);
    now = GetPhotoUploadTimeMs();
    if (now - self->m_progress >= kIdleMs || now - self->m_started >= kLifetimeMs) {
        return self->AbortClient();
    }
    return self->PumpReply();
} // OnTcpPoll

/**
    Releases photo upload state after a TCP connection error.

    @param arg Instance pointer supplied when registering the lwIP callback.
    @param error Unused error code required by the TCP callback interface.
*/
void
FrameBufferController::OnTcpErr (
    void* arg,
    [[maybe_unused]] err_t error
    )
{
    // PCB was already freed by lwIP.
    static_cast<FrameBufferController*>(arg)->ResetClient();
} // OnTcpErr

/**
    Accepts a TCP photo client and initializes its receive state.

    Accepts a raw photo connection only when the listener and framebuffer can assign it exclusive
    upload ownership. It resets per-client counters and installs callbacks for receive, reply
    progress, polling and failure. A conflicting connection is rejected rather than replacing an
    image transaction already in progress.

    @param arg Instance pointer supplied when registering the lwIP callback.
    @param pcb lwIP TCP connection associated with this callback or operation.
    @param error Error code or diagnostic associated with this operation.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
    @return ERR_VAL - The callback state, incoming connection or stack result is invalid.
*/
err_t
FrameBufferController::OnTcpAccept (
    void* arg,
    tcp_pcb* pcb,
    err_t error
    )
{
    FrameBufferController* self = nullptr;

    self = static_cast<FrameBufferController*>(arg);
    if (!pcb) {
        return ERR_VAL;
    }
    if (!self || error != ERR_OK || self->m_clientPcb ||
        !(self->m_rawOwner = self->BeginFrame(SOURCE_FRAME_SIZE_BYTES))) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    self->m_clientPcb = pcb;
    self->m_started = self->m_progress = GetPhotoUploadTimeMs();
    self->AttachClient();
    return ERR_OK;
} // OnTcpAccept

/**
    Starts the raw TCP photo upload listener.

    Foreground lifecycle API, only while CYW43 is initialized.

    @param port Local TCP port on which to accept photo uploads.

    @return true - The required listener is active, including an already matching listener.
    @return false - The service could not allocate, bind or start its listener.
*/
bool
FrameBufferController::StartFrameUploadServer (
    uint16_t port
    )
{
    bool same;
    tcp_pcb* pcb = nullptr;
    tcp_pcb* listener = nullptr;

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    if (m_listenerPcb) {
        same = m_listenerPort == port;
        cyw43_arch_lwip_end();
        return same;
    }
    pcb = tcp_new();
    if (!pcb) {
        cyw43_arch_lwip_end();
        return false;
    }
    if (tcp_bind(pcb, IP_ADDR_ANY, port) != ERR_OK) {
        tcp_abort(pcb);
        cyw43_arch_lwip_end();
        return false;
    }
    listener = tcp_listen(pcb);
    if (!listener) {
        tcp_abort(pcb);
        cyw43_arch_lwip_end();
        return false;
    }
    m_listenerPcb = listener;
    m_listenerPort = port;
    tcp_arg(listener, this);
    tcp_accept(listener, OnTcpAccept);
    cyw43_arch_lwip_end();
    HUB75_DEBUG("[upload] TCP listening on port %u\n", port);
    return true;
} // StartFrameUploadServer

/**
    Stops the raw TCP photo upload listener and resets receive state.
*/
void
FrameBufferController::StopFrameUploadServer (
    void
    )
{
    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    if (m_clientPcb) {
        AbortClient();
    }
    if (m_listenerPcb) {
        tcp_accept(m_listenerPcb, nullptr);
        tcp_close(m_listenerPcb); // LISTEN close does not allocate a FIN.
        m_listenerPcb = nullptr;
        m_listenerPort = 0;
    }
    cyw43_arch_lwip_end();
} // StopFrameUploadServer

/**
    Reports whether the raw TCP photo listener is active.

    @return true - The TCP listener is active.
    @return false - The TCP listener is not active.
*/
bool
FrameBufferController::IsFrameUploadServerListening (
    void
    )
{
    bool ready;

    cyw43_arch_lwip_begin();
    ready = m_listenerPcb && m_listenerPcb->state == LISTEN;
    cyw43_arch_lwip_end();
    return ready;
} // IsFrameUploadServerListening

/**
    Reports whether a photo upload is currently receiving bytes.

    @return true - A photo upload is currently receiving pixels.
    @return false - No photo upload is currently receiving pixels.
*/
bool
FrameBufferController::IsFrameUploadInProgress (
    void
    ) const
{
    return m_receiver.UploadInProgress();
} // IsFrameUploadInProgress

/**
    Claims framebuffer ownership for rendering.

    @return true - Rendering owns the framebuffer until EndRender is called.
    @return false - An upload currently owns the framebuffer.
*/
bool
FrameBufferController::TryBeginRender (
    void
    )
{
    return m_receiver.TryBeginRender();
} // TryBeginRender

/**
    Releases framebuffer ownership after rendering.
*/
void
FrameBufferController::EndRender (
    void
    )
{
    m_receiver.EndRender();
} // EndRender

/**
    Returns the completed logical frame when one is available.

    @return pointer - Borrowed complete CPU framebuffer; ownership remains with the upload
                      controller.
    @return nullptr - No complete frame is available.
*/
const unsigned char*
FrameBufferController::GetUploadedFrameData (
    void
    ) const
{
    if (m_receiver.HasFrame()) {
        return m_frameBuffer;
    } else {
        return nullptr;
    }
} // GetUploadedFrameData

/**
    Returns the revision of the most recently published frame.

    @return value - Revision counter incremented when a new complete frame is published.
*/
FrameBufferController::Token
FrameBufferController::GetFrameRevision (
    void
    ) const
{
    return m_receiver.FrameRevision();
} // GetFrameRevision

/**
    Feeds bytes to the frame upload identified by its ownership token.

    @param owner Ownership token returned when this upload was started.
    @param bytes Read-only input byte buffer.
    @param count Number of bytes to process.

    @return true - The chunk belongs to the current upload and was accepted.
    @return false - The token, byte range or receiver state is invalid.
*/
bool
FrameBufferController::FeedFrame (
    Token owner,
    const uint8_t* bytes,
    size_t count
    )
{
    return m_receiver.Feed(owner, bytes, count);
} // FeedFrame

/**
    Aborts the frame upload identified by its ownership token.

    @param owner Ownership token returned when this upload was started.
*/
void
FrameBufferController::AbortFrame (
    Token owner
    )
{
    m_receiver.Abort(owner);
} // AbortFrame

/**
    Starts a full-panel RGB888 photo upload with default metadata.

    @param bytes Complete RGB888 photo payload length.

    @return token - Nonzero ownership token for the accepted upload.
    @return 0 - Size or ownership prevents starting this upload.
*/
FrameBufferController::Token
FrameBufferController::BeginFrame (
    size_t bytes
    )
{
    return BeginFrame(bytes, MediaMetadata::Photo(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT));
} // BeginFrame

/**
    Starts a photo upload with the requested frame size and metadata.

    The common descriptor can describe video, but this receiver still accepts
    only one RGB888 photo matching the complete panel. Pending metadata changes
    only after ownership is acquired; a rejected upload leaves the active frame
    and its description unchanged.

    @param bytes Number of bytes to process.
    @param metadata Logical photo type, dimensions, pixel format and timing.

    @return token - Nonzero ownership token for the new photo upload.
    @return 0 - The metadata, frame size or framebuffer ownership prevents starting the upload.
*/
FrameBufferController::Token
FrameBufferController::BeginFrame (
    size_t bytes,
    const MediaMetadata& metadata
    )
{
    uint64_t payload;
    uint32_t token;

    // The descriptor can express videos, but this receiver still owns one photo.
    if (metadata.pixelFormat != PixelFormat::RGB888 ||
        !metadata.IsPhotoFor(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT) || !metadata.PayloadBytes(payload) ||
        payload != bytes) {
        return 0;
    }
    token = m_receiver.Begin(bytes);
    if (token) {
        m_pendingMetadata = metadata;
    }
    return token;
} // BeginFrame

/**
    Publishes a complete upload with its pending photo metadata.

    Validates completion before changing the current descriptor. Metadata is
    installed while the upload still owns the framebuffer, then Finish publishes
    the new revision and releases or holds it for foreground persistence.

    @param owner Ownership token returned when this upload was started.

    @return true - The completed upload was published.
    @return false - The token is invalid or the complete frame is not yet available.
*/
bool
FrameBufferController::FinishFrame (
    Token owner
    )
{
    if (!m_receiver.CanFinish(owner)) {
        return false;
    }
    m_metadata = m_pendingMetadata;
    m_fromStorage = false;
    return m_receiver.Finish(owner);
} // FinishFrame


/**
    Returns metadata for the current completed frame.

    Read under foreground render ownership, or in serialized transport context.

    @return metadata - Copy of the completed frame type, dimensions, format and timing.
*/
MediaMetadata
FrameBufferController::GetFrameMetadata (
    void
    ) const
{
    return m_metadata;
} // GetFrameMetadata

/**
    Claims exclusive framebuffer ownership for saved-media work.

    @return true - External media owns the framebuffer until explicitly released.
    @return false - Another operation currently owns the framebuffer.
*/
bool
FrameBufferController::TryBeginExternalMedia (
    void
    )
{
    return m_receiver.TryBeginExternal();
} // TryBeginExternalMedia

/**
    Releases exclusive saved-media framebuffer ownership.
*/
void
FrameBufferController::EndExternalMedia (
    void
    )
{
    m_receiver.EndExternal();
} // EndExternalMedia

/**
    Reports whether the current frame came from saved media.

    @return true - The current published frame came from SD storage.
    @return false - The current frame did not come from SD storage.
*/
bool
FrameBufferController::IsStoredFrame (
    void
    ) const
{
    return m_fromStorage;
} // IsStoredFrame

/**
    Publishes saved-media metadata and marks the frame complete.

    @param metadata Media dimensions, pixel format, placement and playback settings.
*/
void
FrameBufferController::PublishStoredFrame (
    const MediaMetadata& metadata
    )
{
    m_metadata = metadata;
    m_fromStorage = true;
    m_receiver.PublishStoredFrame();
} // PublishStoredFrame

/**
    Clears the availability of the current saved frame.
*/
void
FrameBufferController::ClearStoredFrame (
    void
    )
{
    m_receiver.ClearStoredFrame();
} // ClearStoredFrame

#if CPU_FRAME_RGB565
/**
    Reorders one little-endian RGB565 pixel into native panel colour and byte order.

    Five-bit channels promoted to green replicate their top bit; reducing green
    drops its lowest bit. This preserves the existing RGB888 expansion/repacking
    result while sharing the same inlined conversion between fragments and tiles.

    @param destination Writable native CPU pixel, including unaligned buffers.
    @param source Borrowed two-byte little-endian RGB565 pixel.
*/
FRAMEBUFFER_ALWAYS_INLINE void
FrameBufferController::StorePanelRgb565Pixel (
    uint8_t* destination,
    const uint8_t* source
    )
{
    uint16_t value;
    uint16_t fiveBitChannels[FRAMEBUFFER_RGB_CHANNEL_COUNT];
    uint16_t sixBitChannels[FRAMEBUFFER_RGB_CHANNEL_COUNT];

    // Reorder native channels without expanding, repacking or calling a pixel writer.
    value = uint16_t(source[0]) | uint16_t(uint16_t(source[1]) << BITS_PER_BYTE);
    fiveBitChannels[FRAMEBUFFER_RGB_RED_CHANNEL] = value >> RGB565_RED_SHIFT;
    sixBitChannels[FRAMEBUFFER_RGB_GREEN_CHANNEL] =
        (value >> RGB565_GREEN_SHIFT) & RGB565_GREEN_MASK;
    fiveBitChannels[FRAMEBUFFER_RGB_BLUE_CHANNEL] = value & RGB565_BLUE_MASK;
    fiveBitChannels[FRAMEBUFFER_RGB_GREEN_CHANNEL] =
        sixBitChannels[FRAMEBUFFER_RGB_GREEN_CHANNEL] >> FRAMEBUFFER_565_CHANNEL_WIDTH_DIFFERENCE;
    sixBitChannels[FRAMEBUFFER_RGB_RED_CHANNEL] =
        (fiveBitChannels[FRAMEBUFFER_RGB_RED_CHANNEL] << FRAMEBUFFER_565_CHANNEL_WIDTH_DIFFERENCE) |
        (fiveBitChannels[FRAMEBUFFER_RGB_RED_CHANNEL] >> FRAMEBUFFER_565_EXTRA_GREEN_BIT_SHIFT);
    sixBitChannels[FRAMEBUFFER_RGB_BLUE_CHANNEL] =
        (fiveBitChannels[FRAMEBUFFER_RGB_BLUE_CHANNEL] << FRAMEBUFFER_565_CHANNEL_WIDTH_DIFFERENCE) |
        (fiveBitChannels[FRAMEBUFFER_RGB_BLUE_CHANNEL] >> FRAMEBUFFER_565_EXTRA_GREEN_BIT_SHIFT);
    value = uint16_t((fiveBitChannels[IMAGE_RED_BYTE_OFFSET] << RGB565_RED_SHIFT) |
        (sixBitChannels[IMAGE_GREEN_BYTE_OFFSET] << RGB565_GREEN_SHIFT) |
        fiveBitChannels[IMAGE_BLUE_BYTE_OFFSET]);
    value = uint16_t((value << BITS_PER_BYTE) | (value >> BITS_PER_BYTE));
    std::memcpy(destination, &value, sizeof(value));
} // StorePanelRgb565Pixel
#endif

/**
    Converts stored pixels and places them in the CPU framebuffer.

    Validates pixel alignment, source bounds and placement before mapping each logical image
    region into the panel-chain framebuffer. Within a panel-row run, a constant signed destination
    stride avoids recalculating coordinates per pixel. With an RGB565 CPU frame,
    RGB565 input is reordered directly at its five/six-bit precision. A five-bit
    channel promoted to green replicates its top bit; green reduced to five bits
    drops its lowest bit. This matches expansion to RGB888 and repacking exactly,
    including alternative channel wiring, without doing that intermediate work.
    Other input/output combinations retain the general conversion path.
    The caller already owns the destination frame; this helper
    neither claims that ownership nor publishes a completed image.

    @param helpers Logical-to-physical pixel mapping and display overlay helpers.
    @param buffer Writable destination buffer; the caller must provide sufficient storage.
    @param bytes Read-only input byte buffer.
    @param count Number of bytes to process.
    @param offset Byte offset within the source image or file.
    @param width Image width in pixels.
    @param height Image height in pixels.
    @param left Horizontal image placement in pixels; AutoOffset selects centering.
    @param top Vertical image placement in pixels; AutoOffset selects centering.

    @return true - The requested pixel chunk was accepted or copied.
    @return false - The pixel range, buffer, format or ownership is invalid.
*/
template <PixelFormat Format>
bool
FrameBufferController::MapStoredPixels (
    PanelRenderer& helpers,
    uint8_t* buffer,
    const uint8_t* bytes,
    size_t count,
    size_t offset,
    uint16_t width,
    uint16_t height,
    uint16_t left,
    uint16_t top
    )
{
    size_t PixelBytes;
    size_t FrameBytes;
    size_t pixel;
    size_t remaining;
    int localX;
    int logicalX;
    int logicalY;
    size_t run;
    int x;
    int y;
    ptrdiff_t dest;
    ptrdiff_t stride;
    int nextX;
    int nextY;
    size_t i;
    uint8_t decoded[IMAGE_BYTES_PER_PIXEL];
    const uint8_t* rgb = nullptr;
    uint16_t value;
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    if (Format == PixelFormat::RGB565) {
        PixelBytes = RGB565_PIXEL_BYTES;
    } else {
        PixelBytes = RGB888_PIXEL_BYTES;
    }
    if (!width || !height || width > LOGICAL_IMAGE_WIDTH || height > LOGICAL_IMAGE_HEIGHT) {
        return false;
    }
    FrameBytes = size_t(width) * height * PixelBytes;
    if (left == MediaMetadata::AutoOffset) {
        left = (LOGICAL_IMAGE_WIDTH - width) / CENTER_ALIGNMENT_DIVISOR;
    }
    if (top == MediaMetadata::AutoOffset) {
        top = (LOGICAL_IMAGE_HEIGHT - height) / CENTER_ALIGNMENT_DIVISOR;
    }
    if (left > LOGICAL_IMAGE_WIDTH - width || top > LOGICAL_IMAGE_HEIGHT - height) {
        return false;
    }
    if (!bytes || offset > FrameBytes || count > FrameBytes - offset || offset % PixelBytes ||
        count % PixelBytes) {
        return false;
    }
    pixel = offset / PixelBytes;
    remaining = count / PixelBytes;
    while (remaining) {
        localX = int(pixel % width);
        logicalX = localX + left;
        logicalY = int(pixel / width) + top;
        // Within one logical panel row, the rotated destination advances by a
        // constant signed stride. Calculate coordinates per run, not per pixel.
        run = std::min({remaining, size_t(width - localX),
                                     size_t(LOGICAL_PANEL_WIDTH - logicalX % LOGICAL_PANEL_WIDTH)});
        x = 0;
        y = 0;
        helpers.ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, x, y);
        dest = (ptrdiff_t(y) * MATRIX_PANEL_WIDTH + x) * CpuFrame::PixelBytes;
        stride = 0;
        if (run > 1) {
            nextX = 0;
            nextY = 0;
            helpers.ConvertLogicalToPhysicalCoordinates(logicalX + 1, logicalY, nextX, nextY);
            stride =
                (ptrdiff_t(nextY - y) * MATRIX_PANEL_WIDTH + nextX - x) * ptrdiff_t(CpuFrame::PixelBytes);
        }
        for (i = 0; i < run; ++i) {
#if CPU_FRAME_RGB565
            if (Format == PixelFormat::RGB565) {
                StorePanelRgb565Pixel(buffer + dest, bytes);
            } else
#endif
            {
                // Retain full RGB888 handling and the wider CPU-frame fallback.
                rgb = bytes;
                if (Format == PixelFormat::RGB565) {
                    value = uint16_t(bytes[0]) | uint16_t(uint16_t(bytes[1]) << BITS_PER_BYTE);
                    red = uint8_t(value >> RGB565_RED_SHIFT);
                    green = uint8_t((value >> RGB565_GREEN_SHIFT) & RGB565_GREEN_MASK);
                    blue = uint8_t(value & RGB565_BLUE_MASK);
                    decoded[0] =
                        uint8_t((red << RGB565_RED_BLUE_EXPAND_SHIFT) | (red >> RGB565_RED_BLUE_REPLICATE_SHIFT));
                    decoded[1] =
                        uint8_t((green << RGB565_GREEN_EXPAND_SHIFT) | (green >> RGB565_GREEN_REPLICATE_SHIFT));
                    decoded[RGB_PIXEL_BLUE_INDEX] = uint8_t((blue << RGB565_RED_BLUE_EXPAND_SHIFT) |
                                                            (blue >> RGB565_RED_BLUE_REPLICATE_SHIFT));
                    rgb = decoded;
                }
                CpuFrame::StoreRgbPixel(buffer + dest, rgb[IMAGE_RED_BYTE_OFFSET], rgb[IMAGE_GREEN_BYTE_OFFSET],
                                        rgb[IMAGE_BLUE_BYTE_OFFSET]);
            }
            dest += stride;
            bytes += PixelBytes;
        }

        pixel += run;
        remaining -= run;
    }

    return true;
} // MapStoredPixels

/**
    Maps a stored pixel fragment into the placed CPU framebuffer region.

    @param bytes Read-only input byte buffer.
    @param count Number of bytes to process.
    @param offset Byte offset within the source image or file.
    @param format Pixel encoding used by the source data.
    @param width Image width in pixels.
    @param height Image height in pixels.
    @param left Horizontal image placement in pixels; AutoOffset selects centering.
    @param top Vertical image placement in pixels; AutoOffset selects centering.

    @return true - The requested pixel chunk was accepted or copied.
    @return false - The pixel range, buffer, format or ownership is invalid.
*/
bool
FrameBufferController::WriteStoredFramePixels (
    const uint8_t* bytes,
    size_t count,
    size_t offset,
    PixelFormat format,
    uint16_t width,
    uint16_t height,
    uint16_t left,
    uint16_t top
    )
{
    if (format == PixelFormat::RGB888) {
        return MapStoredPixels<PixelFormat::RGB888>(*m_panelRenderer, m_frameBuffer, bytes, count, offset,
                                                    width, height, left, top);
    }
    if (format == PixelFormat::RGB565) {
        return MapStoredPixels<PixelFormat::RGB565>(*m_panelRenderer, m_frameBuffer, bytes, count, offset,
                                                    width, height, left, top);
    }
    return false;
} // WriteStoredFramePixels

/**
    Clears the framebuffer before writing a saved image with placement metadata.

    @param metadata Media dimensions, pixel format, placement and playback settings.
*/
void
FrameBufferController::PrepareStoredFrame (
    const MediaMetadata& metadata
    )
{
    // Full-size crop frames overwrite every pixel. Fit frames must also remove
    // previous content/overlays from the generated borders before publication.
    if (metadata.width != LOGICAL_IMAGE_WIDTH || metadata.height != LOGICAL_IMAGE_HEIGHT) {
        std::memset(m_frameBuffer, 0, FRAME_BUFFER_SIZE_BYTES);
    }
} // PrepareStoredFrame

/**
    Copies prepared native rectangles into the CPU framebuffer without pixel conversion.

    The storage reader already checked the layout signature. Each packed rectangle
    is copied as row spans into its physical framebuffer position, with no channel
    conversion, per-pixel coordinate mapping or additional complete-frame buffer.
    The caller retains render ownership and publishes only after the frame CRC passes.

    @param bytes Provisional pixels in the exact configured CPU representation.
    @param count Number of complete-pixel bytes in this fragment.
    @param offset Byte position within the packed prepared frame.
    @param layout Validated native rectangles and pixel representation.

    @return true - All fragment bytes were copied to valid framebuffer positions.
    @return false - The fragment, layout, alignment or destination bounds are invalid.
*/
bool
FrameBufferController::WritePreparedFramePixels (
    const uint8_t* bytes,
    size_t count,
    size_t offset,
    const PreparedFrameLayout& layout
    )
{
    size_t index;
    size_t regionBytes;
    size_t rowBytes;
    size_t span;
    size_t localPixel;
    size_t destination;
    const PreparedFrameRegion* region = nullptr;

    if (!bytes || !layout.compatible || layout.pixelBytes != CpuFrame::PixelBytes ||
        !layout.regionCount || layout.regionCount > PREPARED_FRAME_MAX_REGIONS ||
        offset > layout.frameBytes || count > layout.frameBytes - offset ||
        offset % CpuFrame::PixelBytes || count % CpuFrame::PixelBytes) {
        return false;
    }

    for (index = 0; index < layout.regionCount && count; ++index) {
        region = &layout.regions[index];
        if (!region->width || !region->height ||
            uint32_t(region->x) + region->width > MATRIX_PANEL_WIDTH ||
            uint32_t(region->y) + region->height > MATRIX_PANEL_HEIGHT) {
            return false;
        }
        regionBytes = size_t(region->width) * region->height * CpuFrame::PixelBytes;
        if (offset >= regionBytes) {
            offset -= regionBytes;
            continue;
        }

        // Native row spans preserve gaps between panels and generated black borders.
        rowBytes = size_t(region->width) * CpuFrame::PixelBytes;
        while (offset < regionBytes && count) {
            localPixel = offset / CpuFrame::PixelBytes;
            span = std::min(count, rowBytes - offset % rowBytes);
            destination = ((size_t(region->y) + localPixel / region->width) * MATRIX_PANEL_WIDTH +
                region->x + localPixel % region->width) * CpuFrame::PixelBytes;
            std::memcpy(m_frameBuffer + destination, bytes, span);
            bytes += span;
            count -= span;
            offset += span;
        }
        offset = 0;
    }

    return count == 0;
} // WritePreparedFramePixels

/**
    Maps a complete decoded JPEG tile after checking its format, bounds and placement once.

    RGB565 CPU output splits the tile only where it crosses a physical panel's
    logical boundary. Each rectangle calculates one origin and two signed pixel
    strides, then reuses them for every row. Rotation and flipped panels therefore
    retain the fragment path's exact coordinates without per-row divisions or
    validation. Other pixel combinations use the compatible fragment mapper.
    The caller retains render ownership and publishes only after frame verification.

    @param tile Borrowed, tightly packed rectangle inside the logical source image.
    @param width Complete source image width in pixels.
    @param height Complete source image height in pixels.
    @param left Horizontal source-image placement; AutoOffset selects centering.
    @param top Vertical source-image placement; AutoOffset selects centering.

    @return true - Every tile pixel was mapped into the CPU framebuffer.
    @return false - The tile, image geometry, format or placement was invalid.
*/
bool
FrameBufferController::WriteStoredFrameTile (
    const ImageTile& tile,
    uint16_t width,
    uint16_t height,
    uint16_t left,
    uint16_t top
    )
{
    size_t rowBytes;
    size_t offset;
    uint32_t row;
    PixelFormat format;
#if CPU_FRAME_RGB565
    uint32_t tileX;
    uint32_t tileY;
    uint32_t column;
    uint32_t blockWidth;
    uint32_t blockHeight;
    int logicalX;
    int logicalY;
    int physicalX;
    int physicalY;
    int adjacentX;
    int adjacentY;
    ptrdiff_t origin;
    ptrdiff_t destination;
    ptrdiff_t columnStride;
    ptrdiff_t rowStride;
    const uint8_t* source = nullptr;
#endif

    // Check the whole borrowed rectangle before touching any destination pixels.
    if (!m_panelRenderer || !m_frameBuffer || !tile.pixels || !width || !height ||
        width > LOGICAL_IMAGE_WIDTH || height > LOGICAL_IMAGE_HEIGHT ||
        !tile.width || !tile.height ||
        (tile.pixelBytes != RGB565_PIXEL_BYTES && tile.pixelBytes != RGB888_PIXEL_BYTES) ||
        tile.left > width || tile.width > width - tile.left ||
        tile.top > height || tile.height > height - tile.top ||
        tile.bytes != size_t(tile.width) * tile.height * tile.pixelBytes) {
        return false;
    }
    if (left == MediaMetadata::AutoOffset) {
        left = (LOGICAL_IMAGE_WIDTH - width) / CENTER_ALIGNMENT_DIVISOR;
    }
    if (top == MediaMetadata::AutoOffset) {
        top = (LOGICAL_IMAGE_HEIGHT - height) / CENTER_ALIGNMENT_DIVISOR;
    }
    if (left > LOGICAL_IMAGE_WIDTH - width || top > LOGICAL_IMAGE_HEIGHT - height) {
        return false;
    }

#if CPU_FRAME_RGB565
    if (tile.pixelBytes == RGB565_PIXEL_BYTES) {
        // Each block stays inside one panel, where both coordinate strides are constant.
        tileY = 0;
        while (tileY < tile.height) {
            logicalY = top + tile.top + tileY;
            blockHeight = std::min(uint32_t(tile.height) - tileY,
                uint32_t(LOGICAL_PANEL_HEIGHT - logicalY % LOGICAL_PANEL_HEIGHT));
            tileX = 0;
            while (tileX < tile.width) {
                logicalX = left + tile.left + tileX;
                blockWidth = std::min(uint32_t(tile.width) - tileX,
                    uint32_t(LOGICAL_PANEL_WIDTH - logicalX % LOGICAL_PANEL_WIDTH));
                m_panelRenderer->ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, physicalX, physicalY);
                origin = (ptrdiff_t(physicalY) * MATRIX_PANEL_WIDTH + physicalX) * CpuFrame::PixelBytes;
                columnStride = 0;
                rowStride = 0;
                if (blockWidth > 1) {
                    m_panelRenderer->ConvertLogicalToPhysicalCoordinates(logicalX + 1, logicalY, adjacentX, adjacentY);
                    columnStride = (ptrdiff_t(adjacentY - physicalY) * MATRIX_PANEL_WIDTH + adjacentX - physicalX) *
                        ptrdiff_t(CpuFrame::PixelBytes);
                }
                if (blockHeight > 1) {
                    m_panelRenderer->ConvertLogicalToPhysicalCoordinates(logicalX, logicalY + 1, adjacentX, adjacentY);
                    rowStride = (ptrdiff_t(adjacentY - physicalY) * MATRIX_PANEL_WIDTH + adjacentX - physicalX) *
                        ptrdiff_t(CpuFrame::PixelBytes);
                }

                // Convert directly from the decoder's tile without copying a row scratch buffer.
                for (row = 0; row < blockHeight; ++row) {
                    source = tile.pixels + ((size_t(tileY + row) * tile.width) + tileX) * RGB565_PIXEL_BYTES;
                    destination = origin + ptrdiff_t(row) * rowStride;
                    for (column = 0; column < blockWidth; ++column) {
                        StorePanelRgb565Pixel(m_frameBuffer + destination, source);
                        destination += columnStride;
                        source += RGB565_PIXEL_BYTES;
                    }
                }
                tileX += blockWidth;
            }
            tileY += blockHeight;
        }
        return true;
    }
#endif

    // Wider output/input combinations keep the established mapper and byte-order handling.
    format = PixelFormat::RGB888;
    if (tile.pixelBytes == RGB565_PIXEL_BYTES) {
        format = PixelFormat::RGB565;
    }
    rowBytes = size_t(tile.width) * tile.pixelBytes;
    for (row = 0; row < tile.height; ++row) {
        offset = ((size_t(tile.top) + row) * width + tile.left) * tile.pixelBytes;
        if (!WriteStoredFramePixels(tile.pixels + row * rowBytes, rowBytes, offset,
            format, width, height, left, top)) {
            return false;
        }
    }
    return true;
} // WriteStoredFrameTile
