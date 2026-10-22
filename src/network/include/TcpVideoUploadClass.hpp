#pragma once
#include "sd/include/MemoryManagerClass.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include "media/include/mediaTransfer.hpp"

/**
    Receives long raw videos without retaining the clip in RAM.

    TCP callbacks collect at most one acknowledged chunk and handle replies;
    Update performs every filesystem write and verification step in foreground
    code, outside the lwIP lock. Exclusive framebuffer ownership pauses playback
    while a transaction is active. Completion or cancellation releases that
    ownership and requests restoration of the newest usable saved generation.
*/
class TcpVideoUpload {
public:
    // Connects raw TCP video uploads to storage and display ownership.
    TcpVideoUpload(MemoryManager& memory, FrameBufferController& upload);

    // Starts the TCP video listener.
    bool Start(void);

    // Stops the listener and schedules active-transaction cleanup.
    void Stop(void);

    // Reports whether the TCP video listener is active.
    bool IsListening(void);

    // Advances foreground storage and pending TCP replies.
    void Update(void);

    // Reports whether the upload holds exclusive framebuffer ownership.
    bool OwnsDisplay(void) const;

    // Consumes a playback-reload request after a transaction ends.
    bool TakeReloadRequest(void);

private:
    // Returns monotonic time for transport inactivity checks.
    static uint32_t GetVideoUploadTimeMs(void);

    // Detaches TCP callbacks before closing or aborting a client.
    static void DetachVideoClientCallbacks(tcp_pcb* pcb);

    // Installs callbacks for the active upload client.
    void Attach(void);

    // Aborts transport and defers storage cleanup to Update.
    err_t Cancel(void);

    // Queues or advances a pending protocol acknowledgement.
    err_t Pump(void);

    // Resets response state for the next acknowledgement.
    void Reply(const char* message);

    // Releases media ownership and returns a terminal upload response.
    void FinishJob(const char* message);

    // Accepts one client while this video transport is idle.
    static err_t Accept(void* context, tcp_pcb* pcb, err_t error);

    // Copies bounded incoming bytes without accessing the filesystem.
    static err_t Receive(void* context, tcp_pcb* pcb, pbuf* packet, err_t error);

    // Advances a reply after TCP acknowledges its bytes.
    static err_t Sent(void* context, tcp_pcb* pcb, u16_t bytes);

    // Services replies and cancels stalled clients.
    static err_t Poll(void* context, tcp_pcb* pcb);

    // Marks a failed client for deferred foreground cleanup.
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
};
