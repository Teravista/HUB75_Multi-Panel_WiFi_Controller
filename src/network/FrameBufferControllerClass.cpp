#include "network/include/FrameBufferControllerClass.hpp"
#include "display/include/displayHelpers.hpp"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
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
    Claims a complete RGB888 photo transfer without waiting for another owner.

    A zero token means rendering or a previous upload still owns the frame. The
    receiver validates the exact payload size and keeps fragmented pixels together.

    @param bytes Complete RGB888 photo payload length.

    @return token - Nonzero ownership token for the accepted transfer.
    @return 0 - The expected frame size or ownership state prevented reception.
*/
FrameBufferController::Token
FrameBufferController::BeginFrame (
    size_t bytes
    )
{
    return m_receiver.Begin(bytes);
} // BeginFrame

/**
    Publishes a completely received photo through its current ownership token.

    The completed revision lets foreground persistence attempt a save only once.
    With pending-frame holding enabled, rendering must claim the frame before a
    new upload can replace the pixels.

    @param owner Ownership token returned by BeginFrame.

    @return true - The exact frame payload was accepted and published.
    @return false - The owner is stale or the payload is incomplete.
*/
bool
FrameBufferController::FinishFrame (
    Token owner
    )
{
    return m_receiver.Finish(owner);
} // FinishFrame
