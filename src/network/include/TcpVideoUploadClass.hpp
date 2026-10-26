#pragma once
#include "sd/include/MemoryManagerClass.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include "media/include/mediaTransfer.hpp"

/**
    Receives raw or indexed-JPEG videos without retaining the clip in RAM.

    TCP callbacks collect at most one acknowledged chunk and handle replies;
    Update performs every filesystem write and verification step in foreground
    code, outside the lwIP lock. Exclusive framebuffer ownership pauses playback
    while a transaction is active. Completion or cancellation releases that
    ownership and requests saved playback restoration. Library mode publishes an
    independent item ID; retained mode preserves the alternating legacy generations.
*/
class TcpVideoUpload {
public:
    // Connects raw TCP video uploads to storage and display ownership.
    TcpVideoUpload(MemoryManager& memory, FrameBufferController& upload, bool libraryUploads = false);

    // Starts the TCP video upload listener.
    bool Start(void);

    // Stops the video listener and cancels its active client.
    void Stop(void);

    // Reports whether the TCP video listener is active.
    bool IsListening(void);

    // Advances foreground storage and pending TCP replies.
    void Update(void);

    // Reports whether the video upload holds exclusive framebuffer access.
    bool OwnsDisplay(void) const;

    // Consumes a request to restore playback after a video upload.
    bool TakeReloadRequest(void);

    // Returns and clears the ID of the last verified library video.
    uint32_t TakeSavedLibraryId(void);

private:
    // Returns elapsed boot time in milliseconds for transport deadlines.
    static uint32_t GetVideoUploadTimeMs(void);

    // Removes TCP callbacks before closing or replacing a connection.
    static void DetachVideoClientCallbacks(tcp_pcb* pcb);

    // Installs callbacks for the active video upload client.
    void Attach(void);

    // Cancels the video client and requests foreground storage cleanup.
    err_t Cancel(void);

    // Queues pending video acknowledgements while TCP has send capacity.
    err_t Pump(void);

    // Prepares a video protocol acknowledgement for transmission.
    void Reply(const char* message);

    // Releases media ownership and returns a terminal upload response.
    void FinishJob(const char* message);

    // Accepts a video upload client when no competing upload owns the display.
    static err_t Accept(void* context, tcp_pcb* pcb, err_t error);

    // Buffers incoming video protocol bytes for foreground processing.
    static err_t Receive(void* context, tcp_pcb* pcb, pbuf* packet, err_t error);

    // Advances the video reply after TCP acknowledges transmitted bytes.
    static err_t Sent(void* context, tcp_pcb* pcb, u16_t bytes);

    // Services video transport progress and inactivity deadlines.
    static err_t Poll(void* context, tcp_pcb* pcb);

    // Releases the video client after a TCP error.
    static void Error(void* context, err_t error);

    enum class Phase { Idle, Header, Opening, Body, Writing, Verifying, Closing, Canceled };
    MemoryManager& memory_;
    FrameBufferController& upload_;
    tcp_pcb* listener_ = nullptr;
    tcp_pcb* client_ = nullptr;
    Phase phase_ = Phase::Idle;
    uint8_t receive_[MediaTransfer::BulkChunkBytes]{};
    size_t chunkBytes_ = MediaTransfer::ChunkBytes;
    size_t received_ = 0;
    size_t needed_ = MediaTransfer::RequestBytes;
    MediaMetadata metadata_;
    uint64_t total_ = 0;
    uint64_t written_ = 0;
    uint32_t progress_ = 0;
    uint64_t appendWorkUs_ = 0;
    uint64_t verificationWorkUs_ = 0;
    const char* reply_ = nullptr;
    size_t replyBytes_ = 0;
    size_t acknowledged_ = 0;
    bool queued_ = false;
    bool external_ = false;
    bool reload_ = false;
    bool enabled_ = false;
    bool libraryUploads_ = false;
    uint32_t transactionId_ = 0;
    uint32_t savedLibraryId_ = 0;
};
