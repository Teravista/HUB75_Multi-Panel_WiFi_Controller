#include "network/include/TcpVideoUploadClass.hpp"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

/**
    Connects raw TCP video uploads to storage and display ownership.

    @param memory SD storage manager used for saved media operations.
    @param upload Shared framebuffer owner and photo upload controller.
    @param libraryUploads True saves new named library videos; false uses retained generations.
*/
TcpVideoUpload::TcpVideoUpload (
    MemoryManager& memory,
    FrameBufferController& upload,
    bool libraryUploads
    ) : memory_(memory), upload_(upload), libraryUploads_(libraryUploads)
{
    return;
} // TcpVideoUpload

/**
    Returns elapsed boot time in milliseconds for transport deadlines.

    @return value - Monotonic time since boot in milliseconds, wrapping at the counter width.
*/
uint32_t
TcpVideoUpload::GetVideoUploadTimeMs (
    void
    )
{
    return to_ms_since_boot(get_absolute_time());
} // GetVideoUploadTimeMs

/**
    Removes TCP callbacks before closing or replacing a connection.

    @param pcb lwIP TCP connection associated with this callback or operation.
*/
void
TcpVideoUpload::DetachVideoClientCallbacks (
    tcp_pcb* pcb
    )
{
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
    tcp_err(pcb, nullptr);
} // DetachVideoClientCallbacks

/**
    Installs callbacks for the active video upload client.
*/
void
TcpVideoUpload::Attach (
    void
    )
{
    tcp_arg(client_, this);
    tcp_recv(client_, Receive);
    tcp_sent(client_, Sent);
    tcp_poll(client_, Poll, TCP_POLL_INTERVAL_TICKS);
    tcp_err(client_, Error);
} // Attach

/**
    Cancels the video client and requests foreground storage cleanup.

    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
TcpVideoUpload::Cancel (
    void
    )
{
    if (client_) {
        DetachVideoClientCallbacks(client_);
        tcp_abort(client_);
        client_ = nullptr;
    }
    phase_ = Phase::Canceled;
    return ERR_ABRT;
} // Cancel

/**
    Prepares a video protocol acknowledgement for transmission.

    @param message Human-readable result or error message.
*/
void
TcpVideoUpload::Reply (
    const char* message
    )
{
    reply_ = message;
    replyBytes_ = std::strlen(message);
    acknowledged_ = 0;
    queued_ = false;
} // Reply

/**
    Queues pending video acknowledgements while TCP has send capacity.

    Queues the small pending protocol reply and tracks acknowledgement before advancing or closing
    the connection. Send-buffer pressure and a temporary close failure retain state for a later
    callback/update. It never waits for the peer; fatal transport errors cancel the client, with
    storage cleanup deferred to the foreground side.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
TcpVideoUpload::Pump (
    void
    )
{
    err_t result;
    err_t nextResult;
    err_t nextNextResult;

    if (!client_ || phase_ == Phase::Canceled) {
        return ERR_OK;
    }
    if (reply_ && !queued_) {
        result = tcp_write(client_, reply_, u16_t(replyBytes_), TCP_WRITE_FLAG_COPY);
        if (result == ERR_MEM) {
            return ERR_OK;
        }
        if (result != ERR_OK) {
            return Cancel();
        }
        queued_ = true;
    }
    if (reply_ && queued_ && acknowledged_ == replyBytes_) {
        reply_ = nullptr;
    }
    if (!reply_ && phase_ == Phase::Closing) {
        DetachVideoClientCallbacks(client_);
        nextResult = tcp_close(client_);
        if (nextResult == ERR_MEM) {
            Attach();
            return ERR_OK;
        }
        if (nextResult != ERR_OK) {
            return Cancel();
        }
        client_ = nullptr;
        phase_ = Phase::Idle;
        return ERR_OK;
    }

    // Ask lwIP to transmit the queued response bytes.
    nextNextResult = tcp_output(client_);
    if (nextNextResult == ERR_OK || nextNextResult == ERR_MEM) {
        return ERR_OK;
    } else {
        return Cancel();
    }
} // Pump

/**
    Accepts a video upload client when no competing upload owns the display.

    Accepts a client only while the video transport is idle, initializes descriptor reception and
    installs its callbacks. Replies bypass Nagle coalescing because stop-and-wait progress depends
    on prompt small acknowledgements. Actual storage and framebuffer ownership are claimed later,
    after the complete descriptor have been validated.

    @param context Caller-owned state passed unchanged to the callback.
    @param pcb lwIP TCP connection associated with this callback or operation.
    @param error Error code or diagnostic associated with this operation.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
    @return ERR_VAL - The callback state, incoming connection or stack result is invalid.
*/
err_t
TcpVideoUpload::Accept (
    void* context,
    tcp_pcb* pcb,
    err_t error
    )
{
    TcpVideoUpload* server = nullptr;

    server = &(*static_cast<TcpVideoUpload*>(context));
    if (!pcb) {
        return ERR_VAL;
    }
    if (error != ERR_OK || server->phase_ != Phase::Idle || server->client_) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    server->client_ = pcb;
    server->phase_ = Phase::Header;
    server->received_ = 0;
    server->needed_ = MediaTransfer::RequestBytes;
    server->chunkBytes_ = MediaTransfer::ChunkBytes;
    server->written_ = 0;
    server->reply_ = nullptr;
    // Replies are only a few bytes. Do not hold them for Nagle coalescing.
    tcp_nagle_disable(pcb);
    server->appendWorkUs_ = server->verificationWorkUs_ = 0;
    server->progress_ = GetVideoUploadTimeMs();
    server->Attach();
    return ERR_OK;
} // Accept

/**
    Buffers incoming video protocol bytes for foreground processing.

    Copies one bounded protocol section from a possibly chained TCP packet into the receive
    buffer. Bytes are accepted only in header, name or body phases and must fit the currently
    requested section. The callback acknowledges/frees packet storage and updates activity time;
    SD writes and verification wait for the next foreground storage step.

    @param context Caller-owned state passed unchanged to the callback.
    @param pcb lwIP TCP connection associated with this callback or operation.
    @param packet Received lwIP packet chain; null signals that the peer closed the connection.
    @param error Error code or diagnostic associated with this operation.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
TcpVideoUpload::Receive (
    void* context,
    tcp_pcb* pcb,
    pbuf* packet,
    err_t error
    )
{
    TcpVideoUpload* server = nullptr;
    bool accepting;
    pbuf* part = nullptr;

    server = &(*static_cast<TcpVideoUpload*>(context));
    if (error != ERR_OK) {
        if (packet) {
            pbuf_free(packet);
        }
        return server->Cancel();
    }
    if (!packet) {
        if (server->phase_ == Phase::Verifying || server->phase_ == Phase::Closing) {
            return server->Pump();
        }
        return server->Cancel();
    }
    accepting = server->phase_ == Phase::Header || server->phase_ == Phase::Body;
    if (!accepting || packet->tot_len > server->needed_ - server->received_) {
        pbuf_free(packet);
        return server->Cancel();
    }
    for (part = packet; part; part = part->next) {
        std::memcpy(server->receive_ + server->received_, part->payload, part->len);
        server->received_ += part->len;
    }

    tcp_recved(pcb, packet->tot_len);

    // Return the receive buffer to lwIP after its bytes have been consumed.
    pbuf_free(packet);
    server->progress_ = GetVideoUploadTimeMs();
    return ERR_OK;
} // Receive

/**
    Advances the video reply after TCP acknowledges transmitted bytes.

    @param context Caller-owned state passed unchanged to the callback.
    @param pcb Unused connection required by the callback interface.
    @param bytes Number of bytes to process.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
TcpVideoUpload::Sent (
    void* context,
    [[maybe_unused]] tcp_pcb* pcb,
    u16_t bytes
    )
{
    TcpVideoUpload* server = nullptr;

    server = &(*static_cast<TcpVideoUpload*>(context));
    if (!server->reply_ || !server->queued_ || bytes > server->replyBytes_ - server->acknowledged_) {
        return server->Cancel();
    }
    server->acknowledged_ += bytes;
    server->progress_ = GetVideoUploadTimeMs();
    return server->Pump();
} // Sent

/**
    Services video transport progress and inactivity deadlines.

    @param context Caller-owned state passed unchanged to the callback.
    @param pcb Unused connection required by the callback interface.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
TcpVideoUpload::Poll (
    void* context,
    [[maybe_unused]] tcp_pcb* pcb
    )
{
    TcpVideoUpload* server = nullptr;

    server = &(*static_cast<TcpVideoUpload*>(context));
    if (GetVideoUploadTimeMs() - server->progress_ >= MediaTransfer::ReceiveIdleMs) {
        return server->Cancel();
    }
    return server->Pump();
} // Poll

/**
    Releases the video client after a TCP error.

    @param context Caller-owned state passed unchanged to the callback.
    @param error Unused error status required by the callback interface.
*/
void
TcpVideoUpload::Error (
    void* context,
    [[maybe_unused]] err_t error
    )
{
    TcpVideoUpload* server = nullptr;

    server = &(*static_cast<TcpVideoUpload*>(context));
    server->client_ = nullptr;
    server->phase_ = Phase::Canceled;
} // Error

/**
    Starts the TCP video upload listener.

    Creates a fresh TCP video listener under the lwIP lock and installs the accept callback only
    after allocation, bind and listen succeed. Partial startup failures release the PCB so
    network-state retries can attempt again. An already active listener is reused rather than
    opening a second endpoint.

    @return true - The required listener is active, including an already matching listener.
    @return false - The service could not allocate, bind or start its listener.
*/
bool
TcpVideoUpload::Start (
    void
    )
{
    tcp_pcb* pcb = nullptr;

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    enabled_ = true;
    if (listener_) {
        cyw43_arch_lwip_end();
        return true;
    }
    pcb = tcp_new();
    if (!pcb) {
        cyw43_arch_lwip_end();
        return false;
    }
    if (tcp_bind(pcb, IP_ADDR_ANY, MediaTransfer::Port) != ERR_OK) {
        tcp_abort(pcb);
        cyw43_arch_lwip_end();
        return false;
    }
    listener_ = tcp_listen(pcb);
    if (!listener_) {
        tcp_abort(pcb);
        cyw43_arch_lwip_end();
        return false;
    }
    tcp_arg(listener_, this);
    tcp_accept(listener_, Accept);
    cyw43_arch_lwip_end();
    std::printf("[video] TCP listening on port %u\n", MediaTransfer::Port);
    return true;
} // Start

/**
    Stops the video listener and cancels its active client.
*/
void
TcpVideoUpload::Stop (
    void
    )
{
    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    if (client_) {
        Cancel();
    }
    if (listener_) {
        tcp_accept(listener_, nullptr);
        tcp_close(listener_);
        listener_ = nullptr;
    }
    enabled_ = false;
    cyw43_arch_lwip_end();
} // Stop

/**
    Reports whether the TCP video listener is active.

    @return true - The TCP listener is active.
    @return false - The TCP listener is not active.
*/
bool
TcpVideoUpload::IsListening (
    void
    )
{
    bool listening;

    cyw43_arch_lwip_begin();
    listening = listener_ && listener_->state == LISTEN;
    cyw43_arch_lwip_end();
    return listening;
} // IsListening

/**
    Reports whether the video upload holds exclusive framebuffer access.

    @return true - The upload currently owns the shared display framebuffer.
    @return false - The upload does not own the display framebuffer.
*/
bool
TcpVideoUpload::OwnsDisplay (
    void
    ) const
{
    return external_;
} // OwnsDisplay

/**
    Consumes a request to restore playback after a video upload.

    @return true - A playback reload was requested; this call consumes that request.
    @return false - No playback reload request was pending.
*/
bool
TcpVideoUpload::TakeReloadRequest (
    void
    )
{
    bool result;

    result = reload_;
    reload_ = false;
    return result;
} // TakeReloadRequest

/**
    Releases media ownership and returns a terminal upload response.

    Filesystem cleanup runs outside the lwIP lock. Cancellation preserves the
    previous published generation, then the main loop reloads saved playback.

    @param message Static terminal protocol response.
*/
void
TcpVideoUpload::FinishJob (
    const char* message
    )
{
    if (external_) {
        memory_.AbortMediaSave();
        upload_.EndExternalMedia();
        external_ = false;
        reload_ = true;
    }
    cyw43_arch_lwip_begin();
    if (client_ && phase_ != Phase::Canceled) {
        phase_ = Phase::Closing;
        Reply(message);
    }
    cyw43_arch_lwip_end();
} // FinishJob

/**
    Advances foreground storage and pending TCP replies.

    Snapshots transport state under the lwIP lock and performs bounded filesystem
    work outside it. The stop-and-wait protocol applies backpressure: a chunk is
    acknowledged only after its append succeeds. VERIFY precedes incremental SD
    readback; SAVED is sent only after the final checked header is published.
    Network shutdown defers cancellation cleanup here without entering a dead radio.
*/
void
TcpVideoUpload::Update (
    void
    )
{
    Phase phase;
    bool full;
    bool pending;
    bool bulk;
    bool complete;
    bool opened;
    uint32_t checksum;

    if (!enabled_) {
        if (external_) {
            memory_.AbortMediaSave();
            upload_.EndExternalMedia();
            external_ = false;
            reload_ = true;
        }
        phase_ = Phase::Idle;
        received_ = 0;
        reply_ = nullptr;
        return;
    }
    cyw43_arch_lwip_begin();
    phase = phase_;
    full = received_ == needed_;
    pending = reply_ != nullptr;
    if (phase == Phase::Header && full) {
        phase_ = Phase::Opening;
    }
    if (phase == Phase::Body && full && !pending) {
        phase_ = Phase::Writing;
    }
    cyw43_arch_lwip_end();

    // Cancellation releases storage outside callbacks, even after the radio stops.
    if (phase == Phase::Canceled) {
        if (external_) {
            memory_.AbortMediaSave();
            upload_.EndExternalMedia();
            external_ = false;
            reload_ = true;
        }
        cyw43_arch_lwip_begin();
        phase_ = Phase::Idle;
        received_ = 0;
        reply_ = nullptr;
        cyw43_arch_lwip_end();
        return;
    }
    if (external_ && !memory_.IsReady()) {
        FinishJob("ERR STORAGE\n");
        return;
    }
    if (phase == Phase::Header && full) {
        bulk = std::memcmp(receive_, MediaTransfer::BulkRequestMagic, MEDIA_MAGIC_BYTES) == 0;
        if (bulk) {
            std::memcpy(receive_, "H75M", MEDIA_MAGIC_BYTES);
            chunkBytes_ = MediaTransfer::BulkChunkBytes;
        } else {
            chunkBytes_ = MediaTransfer::ChunkBytes;
        }
        if (!MediaMetadataCodec::DecodeMediaDescriptor(receive_, MediaMetadataCodec::HeaderBytes, metadata_) ||
            !MediaTransfer::IsSupportedVideo(metadata_, LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)) {
            FinishJob("ERR METADATA\n");
            return;
        }
        if (memory_.IsMediaSaveActive() || !upload_.TryBeginExternalMedia()) {
            FinishJob("ERR BUSY\n");
            return;
        }
        external_ = true;
        checksum = MediaMetadataCodec::ReadUnsigned32LittleEndian(receive_ + MediaMetadataCodec::HeaderBytes);
        transactionId_ = 0;
        if (libraryUploads_) {
            opened = memory_.BeginLibraryVideo(metadata_, checksum, "Uploaded video", 0, transactionId_);
        } else {
            opened = memory_.BeginMediaSave(metadata_, checksum);
        }
        if (!opened) {
            FinishJob("ERR STORAGE\n");
            return;
        }
        metadata_.PayloadBytes(total_);
        cyw43_arch_lwip_begin();
        if (phase_ != Phase::Canceled) {
            phase_ = Phase::Body;
            received_ = 0;
            needed_ = size_t(std::min(uint64_t(chunkBytes_), total_));
            progress_ = GetVideoUploadTimeMs();
            if (bulk) {
                Reply("READY 6144\n");
            } else {
                Reply("READY\n");
            }
        }
        cyw43_arch_lwip_end();
    } else if (phase == Phase::Body && full && !pending) {
        if (!memory_.AppendMediaBytes(receive_, needed_)) {
            FinishJob("ERR WRITE\n");
            return;
        }
        written_ += needed_;
        if (written_ == total_ && !memory_.StartMediaVerification()) {
            FinishJob("ERR CHECKSUM\n");
            return;
        }
        cyw43_arch_lwip_begin();
        if (phase_ != Phase::Canceled) {
            received_ = 0;
            progress_ = GetVideoUploadTimeMs();
            if (written_ == total_) {
                phase_ = Phase::Verifying;
                Reply("VERIFY\n");
            } else {
                phase_ = Phase::Body;
                needed_ = size_t(std::min(uint64_t(chunkBytes_), total_ - written_));
                Reply("NEXT\n");
            }
        }
        cyw43_arch_lwip_end();
    } else if (phase == Phase::Verifying && !pending) {
        complete = false;
        if (!memory_.VerifyMediaSaveStep(complete)) {
            FinishJob("ERR VERIFY\n");
            return;
        }
        cyw43_arch_lwip_begin();
        progress_ = GetVideoUploadTimeMs();
        cyw43_arch_lwip_end();
        if (complete) {
            savedLibraryId_ = transactionId_;
            FinishJob("SAVED\n");
        }
    }
    cyw43_arch_lwip_begin();
    Pump();
    cyw43_arch_lwip_end();
} // Update

/**
    Returns and clears the ID of the last verified library video.

    Only complete SD publication sets this ID. Canceled or rejected transactions
    never request adoption of an unfinished library item.

    @return id - Verified saved-video ID, consumed by this call.
    @return 0 - No new library video is awaiting selection.
*/
uint32_t
TcpVideoUpload::TakeSavedLibraryId (
    void
    )
{
    uint32_t id;
    id = savedLibraryId_;
    savedLibraryId_ = 0;
    return id;
} // TakeSavedLibraryId
