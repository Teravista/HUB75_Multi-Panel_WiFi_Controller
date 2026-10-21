#include "media/include/PhotoPersistenceClass.hpp"

/**
    Connects single-photo persistence to storage and the live framebuffer.

    @param memory SD storage manager used for saved media operations.
    @param upload Shared framebuffer owner and photo upload controller.
*/
PhotoPersistence::PhotoPersistence (
    MemoryManager& memory,
    FrameBufferController& upload
    ) : memory_(memory), upload_(upload)
{
    return;
} // PhotoPersistence

/**
    Restores the newest verified saved photo to the framebuffer.

    Runs before radio/network callbacks start. The storage reader verifies the
    descriptor and payload before publication. A failed provisional load is
    aborted so normal rendering replaces it with the flashed image.

    @return true - The newest valid stored photo was restored to the display buffer.
    @return false - No valid photo was available or restoration failed.
*/
bool
PhotoPersistence::RestoreLastImage (
    void
    )
{
    uint32_t token;
    MediaMetadata metadata;

    // Restore through the same guarded receive path used by uploads; abandon provisional pixels on
    // failure.
    token = upload_.BeginFrame(SOURCE_FRAME_SIZE_BYTES);
    if (!token) {
        return false;
    }
    RestoreContext context{upload_, token};
    if (!memory_.ReadLastImage(Consume, &context, &metadata) ||
        !metadata.IsPhotoFor(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT) || !upload_.FinishFrame(token)) {
        upload_.AbortFrame(token);
        return false;
    }
    attemptedRevision_ = upload_.GetFrameRevision();
    return true;
} // RestoreLastImage

/**
    Saves a newly uploaded frame while holding render ownership.

    Under render ownership, once per completed revision, BEFORE drawing status. Failed saves
    leave the live photo visible and do not cause a write loop.

    @return SaveResult::Unchanged - No new uploaded photo needs saving.
    @return SaveResult::Saved - The new photo was saved successfully.
    @return SaveResult::Failed - The new photo could not be saved.
*/
PhotoPersistence::SaveResult
PhotoPersistence::SaveUploadedImage (
    void
    )
{
    uint32_t revision;

    // Save each completed upload once, before status overlays are drawn into its pixels.
    revision = upload_.GetFrameRevision();
    if (!upload_.GetUploadedFrameData() || revision == attemptedRevision_) {
        return SaveResult::Unchanged;
    }
    attemptedRevision_ = revision;
    if (!memory_.IsReady() || !memory_.SaveLastImage(upload_.GetFrameMetadata(), Produce, &upload_)) {
        return SaveResult::Failed;
    }
    return SaveResult::Saved;
} // SaveUploadedImage

/**
    Copies a stored photo fragment into the live framebuffer.

    @param context Caller-owned state passed unchanged to the callback.
    @param bytes Read-only input byte buffer.
    @param count Number of bytes to process.
    @param offset Byte offset within the source image or file.

    @return true - The requested pixel chunk was accepted or copied.
    @return false - The pixel range, buffer, format or ownership is invalid.
*/
bool
PhotoPersistence::Consume (
    void* context,
    const uint8_t* bytes,
    size_t count,
    size_t offset
    )
{
    PhotoPersistence::RestoreContext* restore = nullptr;

    restore = &(*static_cast<RestoreContext*>(context));
    if (offset != restore->offset || !restore->upload.FeedFrame(restore->token, bytes, count)) {
        return false;
    }
    restore->offset += count;
    return true;
} // Consume

/**
    Copies a live framebuffer fragment for storage.

    @param context Caller-owned state passed unchanged to the callback.
    @param bytes Writable byte buffer filled by this operation.
    @param count Number of bytes to process.
    @param offset Byte offset within the source image or file.

    @return true - The requested pixel chunk was accepted or copied.
    @return false - The pixel range, buffer, format or ownership is invalid.
*/
bool
PhotoPersistence::Produce (
    void* context,
    uint8_t* bytes,
    size_t count,
    size_t offset
    )
{
    return static_cast<FrameBufferController*>(context)->ReadFramePixels(bytes, count, offset);
} // Produce
