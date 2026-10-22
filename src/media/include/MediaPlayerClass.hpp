#pragma once
#include "sd/include/MemoryManagerClass.hpp"
#include "network/include/FrameBufferControllerClass.hpp"

/**
    Turns saved photo/video records into verified display frames.

    MemoryManager owns files and streams one frame into the existing CPU buffer.
    This player selects frames from elapsed time, skips stale frames when reads
    run slowly and applies ordinary or ping-pong looping. Callers hold render
    ownership during restoration and updates; the player never scans panels or
    allocates another complete image. Upload completion schedules a reload.
*/
class MediaPlayer {
public:
    // Connects saved-media playback to storage and the shared framebuffer.
    MediaPlayer(MemoryManager& memory, FrameBufferController& upload);

    // Requests restoration of the latest saved generation.
    void RequestReload(void);

    // Reports whether a saved video is currently selected for playback.
    bool IsPlayingVideo(void) const;

    // Returns the selected media descriptor.
    MediaMetadata GetPlaybackMetadata(void) const;

    // Closes playback while preserving the last submitted display frame.
    void Stop(void);

    // Restores the newest usable saved media and verifies its first frame.
    bool Restore(uint32_t nowMs);

    // Reads and publishes the currently due video frame under render ownership.
    bool Update(uint32_t nowMs);

private:
    // Passes one verified-read fragment into the caller-owned framebuffer.
    static bool Consume(void* context, const uint8_t* bytes, size_t count, size_t offset);

    MemoryManager& memory_;
    FrameBufferController& upload_;
    MediaMetadata metadata_;
    uint32_t epochMs_ = 0;
    uint32_t lastFrame_ = 0;
    bool active_ = false;
    bool reload_ = false;
};
