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
    // Connects saved-media playback to storage and the framebuffer.
    MediaPlayer(MemoryManager& memory, FrameBufferController& upload);

    // Schedules reopening the selected saved media.
    void RequestReload(void);

    // Reports whether a saved video is currently open for playback.
    bool IsPlayingVideo(void) const;

    // Returns metadata for the currently open saved media.
    MediaMetadata GetPlaybackMetadata(void) const;

    // Closes playback while preserving the last submitted display frame.
    void Stop(void);

    // Restores the newest usable saved media and verifies its first frame.
    bool Restore(uint32_t nowMs);

    // Reads and publishes the currently due video frame under render ownership.
    bool Update(uint32_t nowMs);

    // Returns the ID of the currently selected library video.
    uint32_t GetLibraryVideoId(void) const;

    // Returns the configured playback FPS cap.
    uint32_t GetFpsLimit(void) const;

    // Returns the playback FPS numerator after applying the cap.
    uint32_t GetEffectiveFpsNumerator(void) const;

    // Returns the playback FPS denominator after applying the cap.
    uint32_t GetEffectiveFpsDenominator(void) const;

    // Changes the playback FPS cap and reschedules the next frame.
    bool SetFpsLimit(uint32_t fps, uint32_t nowMs);

    // Selects a library video and restores its first checked frame.
    bool ShowLibraryVideo(uint32_t id, uint32_t nowMs);

    // Opens and displays one checked library photo.
    bool ShowLibraryPhoto(uint32_t id);

private:
    // Selects the decoded pixel format used by the CPU framebuffer.
    static PixelFormat DecodedFormat(void);

    // Reads one checked frame after clearing any generated black borders.
    bool ReadFrame(uint32_t frame);

    // Advances the playback index using loop or ping-pong direction.
    uint64_t PlaybackStep(uint32_t nowMs) const;

    uint32_t libraryVideoId_ = 0;
    uint32_t maxFps_ = 0;
    uint64_t stepOffset_ = 0;
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
