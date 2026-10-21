#pragma once
#include "sd/include/MemoryManagerClass.hpp"
#include "network/include/FrameBufferControllerClass.hpp"

/**
    Persists newly received live photos and restores the last saved photo at startup.

    This is the foreground bridge between raw photo reception in FrameBufferController and chunked
    save/read operations in MemoryManager. Frame revisions identify new uploads so the same image
    is not repeatedly written to the card. Borrowed callbacks transfer fragments through the
    existing framebuffer rather than allocating a second full image. A failed save leaves the live
    photo available, while restoration publishes pixels only after the stored image has passed
    validation.
*/
class PhotoPersistence {
public:
    enum class SaveResult { Unchanged, Saved, Failed };

    // Connects single-photo persistence to storage and the live framebuffer.
    PhotoPersistence(MemoryManager& memory, FrameBufferController& upload);

    // Restores the newest verified saved photo to the framebuffer.
    bool RestoreLastImage(void);

    // Saves a newly uploaded frame while holding render ownership.
    SaveResult SaveUploadedImage(void);

private:
    struct RestoreContext {
        FrameBufferController& upload;
        FrameBufferController::Token token;
        size_t offset = 0;
    };

    // Copies a stored photo fragment into the live framebuffer.
    static bool Consume(void* context, const uint8_t* bytes, size_t count, size_t offset);

    // Copies a live framebuffer fragment for storage.
    static bool Produce(void* context, uint8_t* bytes, size_t count, size_t offset);

    MemoryManager& memory_;
    FrameBufferController& upload_;
    FrameBufferController::Token attemptedRevision_ = 0;
};
