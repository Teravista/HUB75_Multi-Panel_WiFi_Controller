#include "http/include/HttpMediaUploadClass.hpp"
#include "media/include/MediaLibraryClass.hpp"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "sd/include/storageCrc32.hpp"
#include <cstdio>
#include <cstring>
#define HTTP_RESUME_TOKEN_BYTES 4
#define HTTP_UPLOAD_HEADER_CRC_INITIAL_U 0xffffffffu


/**
    Binds browser media uploads to storage and framebuffer ownership.

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
    Connects completed browser uploads to the saved-media library.

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
    if (state == State::Saved && library_) {
        if (photoUpload_) {
            if (!library_->NotifyPhotoSaved(photoId_)) {
                warning_ = "Photo saved; selection settings could not be saved.";
            }
        } else if (videoId_ && !library_->NotifyVideoSaved(videoId_)) {
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
                  "{\"id\":%lu,\"state\":\"%s\",\"offset\":%llu,\"total\":%llu,\"videoId\":%lu,\"photoId\":%lu,\"resumeToken\":%lu,\"error\":\"%s\",\"warning\":\"%s\"}",
                  static_cast<unsigned long>(session_), names[unsigned(state_)],
                  static_cast<unsigned long long>(offset_), static_cast<unsigned long long>(total_),
                  static_cast<unsigned long>(videoId_), static_cast<unsigned long>(photoId_), static_cast<unsigned long>(resumeToken_), message, warning_);
    PublishResponse();
} // Respond

/**
    Advances browser media commands and incremental SD verification.

    Callbacks fill one bounded message and retain its buffer. The foreground claims
    that message under the lwIP lock, then validates session IDs and confirmed byte
    offsets before accessing storage. An upload owns the framebuffer until verified
    publication, cancellation, inactivity or an SD error releases it.

    Each verification update processes one bounded storage step. Socket loss abandons
    only the current HTTP request, preserving confirmed upload bytes and independent
    verification for retry or resume. Startup and radio shutdown can call this with
    networkActive false to complete deferred cleanup safely.

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
    bool photo;
    bool opened;
    MediaMetadata metadata;
    uint32_t checksum;
    uint32_t maxMillis;
    const char* name = nullptr;
    bool complete;
    bool resumable;
    bool validHeaderLength;
    uint32_t requestedToken;
    uint32_t headerChecksum;

    // Borrow the completed request without holding lwIP while touching the SD card.
    networkActive_ = networkActive;
    if (networkActive_) {
        cyw43_arch_lwip_begin();
    }
    cancel = cancel_;
    cancel_ = false;
    if (abandoned_) {
        abandoned_ = false;
        pending_ = false;
        responseReady_ = false;
        reserved_ = false;
    }
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

        // A nonce resolves a lost start reply without allocating a second saved item.
        if (operation == HTTP_OP_UPLOAD_RESUME) {
            if (id || requestedOffset || count != HTTP_RESUME_TOKEN_BYTES) {
                Respond("invalid resume request");
                return;
            }
            requestedToken = MediaMetadataCodec::ReadUnsigned32LittleEndian(input_ + EnvelopeBytes);
            if (!session_ || !requestedToken || requestedToken != resumeToken_) {
                Respond("wrong upload session");
            } else {
                if (state_ == State::Receiving) {
                    progress_ = GetUploadTimeMs();
                }
                Respond();
            }
            return;
        }

        // Stateless controls share the mailbox but never become an upload session.
        if (HandleLibraryRequest(operation, id, requestedOffset, count)) {
            return;
        }

        // Start only after both descriptor validation and exclusive display admission succeed.
        if (operation == HTTP_OP_UPLOAD_VIDEO_BEGIN || operation == HTTP_OP_UPLOAD_NAMED_VIDEO_BEGIN ||
            operation == HTTP_OP_UPLOAD_PHOTO_BEGIN) {
            named = operation == HTTP_OP_UPLOAD_NAMED_VIDEO_BEGIN;
            photo = operation == HTTP_OP_UPLOAD_PHOTO_BEGIN;
            expectedHeader = MediaTransfer::RequestBytes;
            if (named) {
                expectedHeader += MemoryManager::PhotoNameBytes + sizeof(uint32_t);
            } else if (photo) {
                expectedHeader += MemoryManager::PhotoNameBytes + MemoryManager::ThumbnailBytes;
            }
            resumable = (named || photo) && count == expectedHeader + HTTP_RESUME_TOKEN_BYTES;
            validHeaderLength = count == expectedHeader || resumable;
            requestedToken = 0;
            if (resumable) {
                requestedToken = MediaMetadataCodec::ReadUnsigned32LittleEndian(input_ + EnvelopeBytes + expectedHeader);
            }
            maxMillis = 0;
            if (named && validHeaderLength) {
                maxMillis = MediaMetadataCodec::ReadUnsigned32LittleEndian(
                    input_ + EnvelopeBytes + MediaTransfer::RequestBytes + MemoryManager::PhotoNameBytes);
            }
            if (id || requestedOffset || !validHeaderLength || (resumable && !requestedToken) ||
                (named && (!library_ || maxMillis < MIN_VIDEO_DURATION_MS || maxMillis > MAX_VIDEO_DURATION_MS)) ||
                !MediaMetadataCodec::DecodeMediaDescriptor(input_ + EnvelopeBytes, MediaMetadataCodec::HeaderBytes, metadata) ||
                (!photo && !MediaTransfer::IsSupportedVideo(metadata, LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)) ||
                (photo && (!library_ || !metadata.IsPhotoWithin(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)))) {
                Respond("invalid metadata");
                return;
            }
            // Match the entire original header, including its name and legacy thumbnail.
            headerChecksum = ~StorageCrc32::UpdateCrc32Checksum(
                HTTP_UPLOAD_HEADER_CRC_INITIAL_U, input_ + EnvelopeBytes, expectedHeader);
            if (resumable && requestedToken == resumeToken_ && session_) {
                if (photo != photoUpload_ || headerChecksum != beginChecksum_) {
                    Respond("resume metadata mismatch");
                    return;
                }
                if (state_ == State::Receiving) {
                    progress_ = GetUploadTimeMs();
                }
                Respond();
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
            photoId_ = 0;
            photoUpload_ = photo;
            resumeToken_ = requestedToken;
            beginChecksum_ = headerChecksum;
            offset_ = 0;
            metadata.PayloadBytes(total_);
            error_ = "";
            warning_ = "";
            checksum = MediaMetadataCodec::ReadUnsigned32LittleEndian(
                input_ + EnvelopeBytes + MediaMetadataCodec::HeaderBytes);
            name = "Video upload";
            if (named || photo) {
                name = reinterpret_cast<const char*>(input_ + EnvelopeBytes + MediaTransfer::RequestBytes);
            }
            if (photo) {
                opened = memory_.BeginLibraryPhoto(metadata, checksum, name,
                    input_ + EnvelopeBytes + MediaTransfer::RequestBytes + MemoryManager::PhotoNameBytes, photoId_);
            } else if (library_) {
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
    if (reserved_ || cancel_ || abandoned_) {
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

/**
    Handles foreground library queries and saved display controls.

    Read-only display status uses cached state. Directory queries, previews and control
    changes run only while no upload owns storage. Successful response builders borrow
    the retained output buffer until the transport closes; errors publish a protocol
    response instead of leaving the request waiting indefinitely.

    @param operation Binary mailbox operation code.
    @param id Library ID or first control value.
    @param offset Page cursor, slideshow interval or second control value.
    @param count Number of body bytes after the envelope.

    @return true - The operation is a recognized library command and a response was prepared.
    @return false - The operation belongs to the upload protocol or is unsupported here.
*/
bool
HttpMediaUpload::HandleLibraryRequest (
    uint8_t operation,
    uint32_t id,
    uint32_t offset,
    size_t count
    )
{
    bool handled;
    bool success;

    handled = (operation >= HTTP_OP_PHOTOS_LIST && operation <= HTTP_OP_VIDEO_PREVIEW) ||
              operation == HTTP_OP_DISPLAY_SETTINGS_SET || operation == HTTP_OP_MEMORY_STATUS ||
              operation == HTTP_OP_FPS_LIMIT_SET || operation == HTTP_OP_BRIGHTNESS_SET ||
              operation == HTTP_OP_SLIDESHOW_STOP;
    if (!handled) {
        return false;
    }
    if (!library_ || count) {
        Respond("library request unavailable");
        return true;
    }
    if (operation == HTTP_OP_PLAYBACK_STATUS && !id && !offset) {
        if (library_->PlaybackStatus(response_, sizeof(response_))) {
            PublishResponse();
        } else {
            Respond("display status unavailable");
        }
        return true;
    }
    if (external_ || memory_.IsMediaSaveActive()) {
        Respond("library storage busy");
        return true;
    }

    // Selection changes defer pixel publication to the normal render-owned library update.
    success = false;
    switch (operation) {
    case HTTP_OP_PHOTOS_LIST:
        success = !id && library_->Overview(response_, sizeof(response_), offset);
        break;
    case HTTP_OP_PHOTO_SHOW:
        success = !offset && library_->Select(id) && library_->Overview(response_, sizeof(response_));
        break;
    case HTTP_OP_SLIDESHOW_SET:
        success = id <= 1 && library_->SetSlideshow(id != 0, offset) && library_->Overview(response_, sizeof(response_));
        break;
    case HTTP_OP_PHOTO_DELETE:
        success = !offset && library_->Remove(id) && library_->Overview(response_, sizeof(response_));
        break;
    case HTTP_OP_PHOTO_PREVIEW:
        success = !offset && library_->Thumbnail(id, response_, sizeof(response_), false);
        break;
    case HTTP_OP_VIDEOS_LIST:
        success = !id && library_->VideoOverview(response_, sizeof(response_), offset);
        break;
    case HTTP_OP_VIDEO_SHOW:
        success = !offset && library_->SelectVideo(id) && library_->VideoOverview(response_, sizeof(response_));
        break;
    case HTTP_OP_VIDEO_DELETE:
        success = !offset && library_->RemoveVideo(id) && library_->VideoOverview(response_, sizeof(response_));
        break;
    case HTTP_OP_VIDEO_PREVIEW:
        success = !offset && library_->Thumbnail(id, response_, sizeof(response_), true);
        break;
    case HTTP_OP_DISPLAY_SETTINGS_SET:
        success = library_->SetDisplaySettings(id, offset, GetUploadTimeMs()) &&
                  library_->PlaybackStatus(response_, sizeof(response_));
        break;
    case HTTP_OP_FPS_LIMIT_SET:
        success = !offset && library_->SetDisplaySettings(id, library_->GetBrightness(), GetUploadTimeMs()) &&
                  library_->PlaybackStatus(response_, sizeof(response_));
        break;
    case HTTP_OP_BRIGHTNESS_SET:
        success = !offset && library_->SetDisplaySettings(library_->GetMaxFps(), id, GetUploadTimeMs()) &&
                  library_->PlaybackStatus(response_, sizeof(response_));
        break;
    case HTTP_OP_SLIDESHOW_STOP:
        success = !id && !offset && library_->SetSlideshow(false, library_->GetSlideshowInterval()) &&
                  library_->PlaybackStatus(response_, sizeof(response_));
        break;
    case HTTP_OP_MEMORY_STATUS:
        success = !id && !offset && library_->MemoryStatus(response_, sizeof(response_));
        break;
    default:
        break;
    }
    if (success) {
        PublishResponse();
    } else {
        Respond("library operation failed or storage unavailable");
    }
    return true;
} // HandleLibraryRequest


/**
    Publishes a completed bounded response to the HTTP transport.

    The caller finishes formatting outside lwIP; only the readiness flag changes
    under the network lock so response bytes remain stable for later transmission.
*/
void
HttpMediaUpload::PublishResponse (
    void
    )
{
    if (networkActive_) {
        cyw43_arch_lwip_begin();
    }
    responseReady_ = true;
    if (networkActive_) {
        cyw43_arch_lwip_end();
    }
} // PublishResponse

/**
    Drops a lost HTTP request while retaining its storage transaction.

    Called from transport callbacks. The foreground consumes this flag before
    releasing the mailbox, so no request can overwrite bytes still queued for
    processing. Confirmed upload progress and verification remain available.
*/
void
HttpMediaUpload::AbandonRequest (
    void
    )
{
    abandoned_ = true;
} // AbandonRequest
