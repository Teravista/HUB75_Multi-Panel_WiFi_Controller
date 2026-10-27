#include "http/include/HttpMediaUploadClass.hpp"
#include "media/include/MediaLibraryClass.hpp"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include <cstdio>
#include <cstring>

/**
    Binds browser video uploads to storage and framebuffer ownership.

    @param memory SD storage manager used for verified video saves.
    @param upload Shared framebuffer controller used to exclude competing uploads.
*/
HttpMediaUpload::HttpMediaUpload (
    MemoryManager& memory,
    FrameBufferController& upload
    ) : memory_(memory), upload_(upload)
{
    return;
} // HttpMediaUpload

/**
    Returns elapsed boot time in milliseconds for upload deadlines.

    @return value - Monotonic boot time in milliseconds, wrapping at the counter width.
*/
uint32_t
HttpMediaUpload::GetUploadTimeMs (
    void
    )
{
    return to_ms_since_boot(get_absolute_time());
} // GetUploadTimeMs

/**
    Connects completed browser videos to the saved-media library.

    @param library Saved-media controller; null selects retained-generation compatibility.
*/
void
HttpMediaUpload::SetMediaLibrary (
    MediaLibrary* library
    )
{
    library_ = library;
} // SetMediaLibrary

/**
    Ends a browser upload and releases storage and framebuffer ownership.

    A completely verified library save is selected before playback is released.
    Failure or cancellation discards only the unpublished transaction. Selection
    settings can fail independently of a durable save, so that outcome is a warning.
    This runs in foreground code, outside the lwIP lock.

    @param state Terminal upload state to publish.
    @param error Static error explanation; an empty string reports no failure.
*/
void
HttpMediaUpload::Finish (
    State state,
    const char* error
    )
{
    if (state == State::Saved && library_ && videoId_) {
        if (!library_->NotifyVideoSaved(videoId_)) {
            warning_ = "Video saved; selection settings could not be saved.";
        }
    }
    if (external_) {
        memory_.AbortMediaSave();
        upload_.EndExternalMedia();
        external_ = false;
        reload_ = true;
    }
    state_ = state;
    error_ = error;
} // Finish

/**
    Formats the upload session state as a bounded JSON response.

    The response records confirmed payload progress and the verified library ID.
    Publishing response readiness is serialized with callbacks; the transport borrows
    the buffer until its connection closes. No filesystem work occurs under that lock.

    @param requestError Static per-request error; empty uses the current transaction error.
*/
void
HttpMediaUpload::Respond (
    const char* requestError
    )
{
    const char* names[] = {"idle", "receiving", "verifying", "saved", "error", "canceled"};
    const char* message = nullptr;

    message = error_;
    if (*requestError) {
        message = requestError;
    }
    std::snprintf(response_, sizeof(response_),
                  "{\"id\":%lu,\"state\":\"%s\",\"offset\":%llu,\"total\":%llu,\"videoId\":%lu,\"error\":\"%s\",\"warning\":\"%s\"}",
                  static_cast<unsigned long>(session_), names[unsigned(state_)],
                  static_cast<unsigned long long>(offset_), static_cast<unsigned long long>(total_),
                  static_cast<unsigned long>(videoId_), message, warning_);
    if (networkActive_) {
        cyw43_arch_lwip_begin();
    }
    responseReady_ = true;
    if (networkActive_) {
        cyw43_arch_lwip_end();
    }
} // Respond

/**
    Advances queued browser video requests and incremental SD verification.

    Callbacks fill one bounded message and retain its buffer. The foreground claims
    that message under the lwIP lock, then validates session IDs and confirmed byte
    offsets before accessing storage. An upload owns the framebuffer until verified
    publication, cancellation, inactivity or an SD error releases it.

    Each verification update processes one bounded storage step. Socket loss requests
    cancellation rather than retaining a resumable job. Startup and radio shutdown
    can call this with networkActive false to complete deferred cleanup safely.

    @param networkActive Whether an initialized radio permits lwIP locking.
*/
void
HttpMediaUpload::Update (
    bool networkActive
    )
{
    bool cancel;
    bool pending;
    uint8_t operation;
    uint32_t id;
    uint32_t requestedOffset;
    size_t count;
    size_t expectedHeader;
    bool named;
    bool opened;
    MediaMetadata metadata;
    uint32_t checksum;
    uint32_t maxMillis;
    const char* name = nullptr;
    bool complete;

    // Borrow the completed request without holding lwIP while touching the SD card.
    networkActive_ = networkActive;
    if (networkActive_) {
        cyw43_arch_lwip_begin();
    }
    cancel = cancel_;
    cancel_ = false;
    pending = pending_;
    pending_ = false;
    if (networkActive_) {
        cyw43_arch_lwip_end();
    }
    if (cancel) {
        Finish(State::Canceled);
        if (networkActive_) {
            cyw43_arch_lwip_begin();
        }
        reserved_ = false;
        responseReady_ = false;
        if (networkActive_) {
            cyw43_arch_lwip_end();
        }
        return;
    }
    if (external_ && !memory_.IsReady()) {
        Finish(State::Error, "SD unavailable");
    }
    if (external_ && state_ == State::Receiving && GetUploadTimeMs() - progress_ >= ReceiveIdleMs) {
        Finish(State::Error, "upload timed out");
    }

    if (pending) {
        if (bytes_ < EnvelopeBytes || bytes_ > Capacity) {
            Respond("invalid request");
            return;
        }
        operation = input_[HTTP_ENVELOPE_OPERATION_OFFSET];
        id = MediaMetadataCodec::ReadUnsigned32LittleEndian(input_ + HTTP_ENVELOPE_SESSION_OFFSET);
        requestedOffset = MediaMetadataCodec::ReadUnsigned32LittleEndian(input_ + HTTP_ENVELOPE_OFFSET_FIELD);
        count = bytes_ - EnvelopeBytes;

        // Start only after both descriptor validation and exclusive display admission succeed.
        if (operation == HTTP_OP_UPLOAD_VIDEO_BEGIN || operation == HTTP_OP_UPLOAD_NAMED_VIDEO_BEGIN) {
            named = operation == HTTP_OP_UPLOAD_NAMED_VIDEO_BEGIN;
            expectedHeader = MediaTransfer::RequestBytes;
            if (named) {
                expectedHeader += MemoryManager::PhotoNameBytes + sizeof(uint32_t);
            }
            maxMillis = 0;
            if (named && count == expectedHeader) {
                maxMillis = MediaMetadataCodec::ReadUnsigned32LittleEndian(
                    input_ + EnvelopeBytes + MediaTransfer::RequestBytes + MemoryManager::PhotoNameBytes);
            }
            if (id || requestedOffset || count != expectedHeader ||
                (named && (!library_ || maxMillis < MIN_VIDEO_DURATION_MS || maxMillis > MAX_VIDEO_DURATION_MS)) ||
                !MediaMetadataCodec::DecodeMediaDescriptor(input_ + EnvelopeBytes, MediaMetadataCodec::HeaderBytes, metadata) ||
                !MediaTransfer::IsSupportedVideo(metadata, LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)) {
                Respond("invalid metadata");
                return;
            }
            if (external_ || memory_.IsMediaSaveActive() || !upload_.TryBeginExternalMedia()) {
                Respond("display busy");
                return;
            }
            external_ = true;
            ++session_;
            if (!session_) {
                ++session_;
            }
            videoId_ = 0;
            offset_ = 0;
            metadata.PayloadBytes(total_);
            error_ = "";
            warning_ = "";
            checksum = MediaMetadataCodec::ReadUnsigned32LittleEndian(
                input_ + EnvelopeBytes + MediaMetadataCodec::HeaderBytes);
            name = "Video upload";
            if (named) {
                name = reinterpret_cast<const char*>(input_ + EnvelopeBytes + MediaTransfer::RequestBytes);
            }
            if (library_) {
                opened = memory_.BeginLibraryVideo(metadata, checksum, name, maxMillis, videoId_);
            } else {
                opened = memory_.BeginMediaSave(metadata, checksum);
            }
            if (!opened) {
                Finish(State::Error, memory_.GetLastError());
            } else {
                state_ = State::Receiving;
                progress_ = GetUploadTimeMs();
            }
        } else if (!session_ || id != session_) {
            Respond("wrong upload session");
            return;
        } else if (operation == HTTP_OP_UPLOAD_CHUNK) {
            // Exact offsets prevent accidental duplication inside a live request sequence.
            if (state_ != State::Receiving || requestedOffset != offset_ || !count ||
                count > MediaTransfer::BulkChunkBytes || count > total_ - offset_) {
                Respond("unexpected chunk");
                return;
            }
            if (!memory_.AppendMediaBytes(input_ + EnvelopeBytes, count)) {
                Finish(State::Error, "SD write failed");
            } else {
                offset_ += count;
                progress_ = GetUploadTimeMs();
                if (offset_ == total_) {
                    if (!memory_.StartMediaVerification()) {
                        Finish(State::Error, "checksum failed");
                    } else {
                        state_ = State::Verifying;
                    }
                }
            }
        } else if ((operation == HTTP_OP_UPLOAD_STATUS || operation == HTTP_OP_UPLOAD_CANCEL) &&
                   !count && !requestedOffset) {
            if (operation == HTTP_OP_UPLOAD_CANCEL && external_) {
                Finish(State::Canceled);
            } else if (state_ == State::Receiving) {
                progress_ = GetUploadTimeMs();
            }
        } else {
            Respond("invalid request");
            return;
        }
        Respond();
    } else if (state_ == State::Verifying) {
        // Publication remains the storage manager's decision after a complete checked reread.
        complete = false;
        if (!memory_.VerifyMediaSaveStep(complete)) {
            Finish(State::Error, "SD verification failed");
        } else if (complete) {
            Finish(State::Saved);
        }
    }
} // Update

/**
    Claims the bounded HTTP request buffer when it is available.

    @return true - The input buffer is reserved for this HTTP request.
    @return false - Another request or cancellation still owns the buffer.
*/
bool
HttpMediaUpload::Reserve (
    void
    )
{
    if (reserved_ || cancel_) {
        return false;
    }
    reserved_ = true;
    return true;
} // Reserve

/**
    Releases the HTTP request buffer for another request.
*/
void
HttpMediaUpload::Release (
    void
    )
{
    reserved_ = false;
} // Release

/**
    Returns the bounded buffer used to receive an HTTP upload request.

    @return pointer - Borrowed input buffer; write at most Capacity bytes while reserved.
*/
uint8_t*
HttpMediaUpload::Input (
    void
    )
{
    return input_;
} // Input

/**
    Queues a received HTTP upload request for foreground processing.

    @param bytes Completed request length, including the envelope.
*/
void
HttpMediaUpload::Submit (
    size_t bytes
    )
{
    bytes_ = bytes;
    pending_ = true;
    responseReady_ = false;
} // Submit

/**
    Reports whether the queued request has a response ready.

    @return true - The transport can borrow the completed response.
    @return false - The queued operation has not produced a response yet.
*/
bool
HttpMediaUpload::ResponseReady (
    void
    ) const
{
    return responseReady_;
} // ResponseReady

/**
    Returns the prepared upload response.

    @return text - Borrowed JSON text, valid until the next reserved request.
*/
const char*
HttpMediaUpload::Response (
    void
    ) const
{
    return response_;
} // Response

/**
    Requests cancellation of the active upload session.
*/
void
HttpMediaUpload::Disconnect (
    void
    )
{
    cancel_ = true;
} // Disconnect

/**
    Reports whether the upload holds exclusive framebuffer access.

    @return true - The upload currently owns the shared framebuffer.
    @return false - No browser upload owns the framebuffer.
*/
bool
HttpMediaUpload::OwnsDisplay (
    void
    ) const
{
    return external_;
} // OwnsDisplay

/**
    Consumes a request to restore playback after an upload.

    @return true - Playback should reload after save, failure or cancellation.
    @return false - No reload was pending.
*/
bool
HttpMediaUpload::TakeReloadRequest (
    void
    )
{
    bool requested;

    requested = reload_;
    reload_ = false;
    return requested;
} // TakeReloadRequest
