#include "network/include/FrameBufferControllerClass.hpp"
#include "display/include/displayHelpers.hpp"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include <cstdio>

#define RGB888_RED_CHANNEL_INDEX 0
#define RGB888_GREEN_CHANNEL_INDEX 1
#define RGB888_BLUE_CHANNEL_INDEX 2

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
    DisplayHelpers* helpers,
    unsigned char* buffer,
    bool holdCompletedFrame
    ) : m_displayHelpers(helpers), m_frameBuffer(buffer),
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
        m_displayHelpers->ConvertLogicalToPhysicalCoordinates(int(index % LOGICAL_IMAGE_WIDTH),
                                                              int(index / LOGICAL_IMAGE_WIDTH), x, y);
        source = (y * MATRIX_PANEL_WIDTH + x) * CpuFrame::PixelBytes;
        pixel = CpuFrame::Load(m_frameBuffer + source);
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
    m_displayHelpers->ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, physicalX, physicalY);
    destIndex = (physicalY * MATRIX_PANEL_WIDTH + physicalX) * CpuFrame::PixelBytes;
    // Preserve existing physical wiring correction for both transports.
    CpuFrame::Store(m_frameBuffer + destIndex, pixelBytes[IMAGE_RED_BYTE_OFFSET],
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
    std::printf("[upload] TCP listening on port %u\n", port);
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

/**
    Maps a stored raw frame fragment into the CPU framebuffer.

    The caller already holds render ownership. Mapping is calculated once for
    each logical panel-row run; a signed stride then advances through physical
    pixels. CpuFrame stores the configured CPU format without another full buffer.

    @param bytes Read-only logical RGB888 or little-endian RGB565 pixels.
    @param count Number of bytes in this complete-pixel fragment.
    @param offset Byte offset in the logical full-panel frame.
    @param format Source fragment encoding: RGB888 or little-endian RGB565.

    @return true - Every pixel in the bounded fragment was mapped.
    @return false - The pointer, range or RGB alignment is invalid.
*/
bool
FrameBufferController::WriteStoredFramePixels (
    const uint8_t* bytes,
    size_t count,
    size_t offset,
    PixelFormat format
    )
{
    size_t pixel;
    size_t pixelBytes;
    size_t frameBytes;
    uint16_t packed;
    uint32_t color;
    uint8_t unpacked[IMAGE_BYTES_PER_PIXEL];
    const uint8_t* channels = nullptr;
    size_t remaining;
    size_t run;
    size_t index;
    int logicalX;
    int logicalY;
    int x;
    int y;
    int nextX;
    int nextY;
    ptrdiff_t destination;
    ptrdiff_t stride;

    // Resolve the source pixel width before checking offsets and whole-pixel alignment.
    pixelBytes = IMAGE_BYTES_PER_PIXEL;
    if (format == PixelFormat::RGB565) {
        pixelBytes = RGB565_PIXEL_BYTES;
    } else if (format != PixelFormat::RGB888) {
        return false;
    }
    frameBytes = size_t(LOGICAL_IMAGE_WIDTH) * LOGICAL_IMAGE_HEIGHT * pixelBytes;
    if (!bytes || offset > frameBytes || count > frameBytes - offset ||
        offset % pixelBytes || count % pixelBytes) {
        return false;
    }

    // Map one logical panel-row run at a time and advance by its physical byte stride.
    pixel = offset / pixelBytes;
    remaining = count / pixelBytes;
    while (remaining) {
        logicalX = int(pixel % LOGICAL_IMAGE_WIDTH);
        logicalY = int(pixel / LOGICAL_IMAGE_WIDTH);
        run = std::min(remaining, size_t(LOGICAL_PANEL_WIDTH - logicalX % LOGICAL_PANEL_WIDTH));
        x = 0;
        y = 0;
        m_displayHelpers->ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, x, y);
        destination = (ptrdiff_t(y) * MATRIX_PANEL_WIDTH + x) * CpuFrame::PixelBytes;
        stride = 0;
        if (run > 1) {
            nextX = 0;
            nextY = 0;
            m_displayHelpers->ConvertLogicalToPhysicalCoordinates(logicalX + 1, logicalY, nextX, nextY);
            stride = (ptrdiff_t(nextY - y) * MATRIX_PANEL_WIDTH + nextX - x) * ptrdiff_t(CpuFrame::PixelBytes);
        }

        // Expand packed source colors only when needed; the CPU frame keeps its configured format.
        for (index = 0; index < run; ++index) {
            channels = bytes;
            if (format == PixelFormat::RGB565) {
                packed = MediaMetadataCodec::ReadUnsigned16LittleEndian(bytes);
                color = CpuFrame::Expand565(packed);
                unpacked[RGB888_RED_CHANNEL_INDEX] = uint8_t(color >> RGB888_RED_SHIFT);
                unpacked[RGB888_GREEN_CHANNEL_INDEX] = uint8_t(color >> RGB888_GREEN_SHIFT);
                unpacked[RGB888_BLUE_CHANNEL_INDEX] = uint8_t(color);
                channels = unpacked;
            }
            // Keep the same physical channel correction for every stored encoding.
            CpuFrame::Store(m_frameBuffer + destination, channels[IMAGE_RED_BYTE_OFFSET],
                            channels[IMAGE_GREEN_BYTE_OFFSET], channels[IMAGE_BLUE_BYTE_OFFSET]);
            destination += stride;
            bytes += pixelBytes;
        }
        pixel += run;
        remaining -= run;
    }
    return true;
} // WriteStoredFramePixels
