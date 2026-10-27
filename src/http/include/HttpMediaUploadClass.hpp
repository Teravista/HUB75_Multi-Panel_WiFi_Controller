#pragma once
#include "sd/include/MemoryManagerClass.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include "media/include/mediaTransfer.hpp"
#include "media/include/MediaLibraryClass.hpp"

/**
    Coordinates browser video transfer, verified SD publication and playback ownership.

    The HTTP transport owns sockets and fills one bounded request buffer. Foreground
    updates perform all storage operations, retain the upload session and confirmed
    byte offset, and report reception separately from verification. Successful library
    saves are selected through MediaLibrary. Cancellation, socket loss or inactivity
    releases the shared framebuffer so saved playback can resume.
*/
class HttpMediaUpload {
public:
    static constexpr size_t EnvelopeBytes = 9;
    static constexpr size_t Capacity = EnvelopeBytes + MediaTransfer::BulkChunkBytes;
    static constexpr uint32_t ReceiveIdleMs = MediaTransfer::ReceiveIdleMs;

    // Binds browser video uploads to storage and framebuffer ownership.
    HttpMediaUpload(MemoryManager& memory, FrameBufferController& upload);

    // Connects completed browser videos to the saved-media library.
    void SetMediaLibrary(MediaLibrary* library);

    // Advances queued browser video requests and incremental SD verification.
    void Update(bool networkActive = true);

    // Claims the bounded HTTP request buffer when it is available.
    bool Reserve(void);

    // Releases the HTTP request buffer for another request.
    void Release(void);

    // Returns the bounded buffer used to receive an HTTP upload request.
    uint8_t* Input(void);

    // Queues a received HTTP upload request for foreground processing.
    void Submit(size_t bytes);

    // Reports whether the queued request has a response ready.
    bool ResponseReady(void) const;

    // Returns the prepared upload response.
    const char* Response(void) const;

    // Requests cancellation of the active upload session.
    void Disconnect(void);

    // Reports whether the upload holds exclusive framebuffer access.
    bool OwnsDisplay(void) const;

    // Consumes a request to restore playback after an upload.
    bool TakeReloadRequest(void);

private:
    enum class State { Idle, Receiving, Verifying, Saved, Error, Canceled };

    // Returns elapsed boot time in milliseconds for upload deadlines.
    static uint32_t GetUploadTimeMs(void);

    // Ends a browser upload and releases storage and framebuffer ownership.
    void Finish(State state, const char* error = "");

    // Formats the upload session state as a bounded JSON response.
    void Respond(const char* requestError = "");

    MemoryManager& memory_;
    FrameBufferController& upload_;
    MediaLibrary* library_ = nullptr;
    uint8_t input_[Capacity]{};
    char response_[HTTP_UPLOAD_RESPONSE_BYTES]{};
    size_t bytes_ = 0;
    uint32_t session_ = 0;
    uint32_t progress_ = 0;
    uint32_t videoId_ = 0;
    uint64_t offset_ = 0;
    uint64_t total_ = 0;
    State state_ = State::Idle;
    const char* error_ = "";
    const char* warning_ = "";
    bool external_ = false;
    bool reload_ = false;
    bool networkActive_ = false;
    bool reserved_ = false;
    bool pending_ = false;
    bool responseReady_ = false;
    bool cancel_ = false;
};
