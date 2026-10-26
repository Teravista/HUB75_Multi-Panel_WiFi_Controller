#include "media/include/MediaPlayerClass.hpp"
#include <cstdio>

#define PLAYBACK_RESTORE_ATTEMPTS 2
#define PING_PONG_DIRECTION_COUNT_ULL 2ull

/**
    Connects saved-media playback to storage and the framebuffer.

    @param memory SD storage manager used for saved media operations.
    @param upload Shared framebuffer owner and photo upload controller.
*/
MediaPlayer::MediaPlayer (
    MemoryManager& memory,
    FrameBufferController& upload
    ) : memory_(memory), upload_(upload)
{
    return;
} // MediaPlayer

/**
    Schedules reopening the selected saved media.
*/
void
MediaPlayer::RequestReload (
    void
    )
{
    reload_ = true;
} // RequestReload

/**
    Reports whether a saved video is currently open for playback.

    @return true - Saved video playback is currently active.
    @return false - No saved video is currently playing.
*/
bool
MediaPlayer::IsPlayingVideo (
    void
    ) const
{
    return active_;
} // IsPlayingVideo

/**
    Returns metadata for the currently open saved media.

    @return metadata - Copy of the current media dimensions, format, placement and playback
                       settings.
*/
MediaMetadata
MediaPlayer::GetPlaybackMetadata (
    void
    ) const
{
    return metadata_;
} // GetPlaybackMetadata

/**
    Closes playback while preserving the last submitted display frame.
*/
void
MediaPlayer::Stop (
    void
    )
{
    memory_.CloseMediaPlayback();
    active_ = false;
    reload_ = false;
    libraryVideoId_ = 0;
} // Stop

/**
    Restores the newest usable saved media and verifies its first frame.

    Tries the newest slot, then the previous generation when a frame failure
    rejects the first. Pixel publication follows full frame verification. If
    neither slot is usable, availability is cleared so the main loop draws its
    flashed fallback; no partial frame is submitted.

    @param nowMs Current monotonic time in milliseconds.

    @return true - A saved photo or video was restored.
    @return false - No saved generation could be opened and verified.
*/
bool
MediaPlayer::Restore (
    uint32_t nowMs
    )
{
    int attempt;

    reload_ = false;
    for (attempt = 0; attempt < PLAYBACK_RESTORE_ATTEMPTS && memory_.IsReady(); ++attempt) {
        if (libraryVideoId_) {
            if (!memory_.OpenLibraryVideo(libraryVideoId_, metadata_)) {
                break;
            }
        } else if (!memory_.OpenLastMedia(metadata_)) {
            break;
        }
        if (memory_.ReadMediaFrame(0, Consume, this)) {
            upload_.PublishStoredFrame(metadata_);
            active_ = metadata_.type == MediaType::Video;
            epochMs_ = nowMs;
            stepOffset_ = 0;
            lastFrame_ = 0;
            std::printf("[playback] restored frames=%lu fps=%lu/%lu\n",
                        static_cast<unsigned long>(metadata_.frameCount),
                        static_cast<unsigned long>(metadata_.fpsNumerator),
                        static_cast<unsigned long>(metadata_.fpsDenominator));
            return true;
        }
    }
    active_ = false;
    upload_.ClearStoredFrame();
    return false;
} // Restore

/**
    Reads and publishes the currently due video frame under render ownership.

    Selects the frame from elapsed time rather than adding read latency to each
    frame period. Ping-pong reverses at each endpoint without repeating the turning
    frame. A failed read requests the previous usable saved generation.

    @param nowMs Current monotonic time in milliseconds.

    @return true - A new verified frame was published.
    @return false - No frame was due, playback is inactive or restoration failed.
*/
bool
MediaPlayer::Update (
    uint32_t nowMs
    )
{
    uint64_t step;
    uint64_t period;
    uint64_t phase;
    uint32_t frame;

    if (reload_) {
        return memory_.IsReady() && Restore(nowMs);
    }
    if (!active_) {
        return false;
    }
    step = PlaybackStep(nowMs);
    if (metadata_.pingPong && metadata_.frameCount > 1) {
        period = PING_PONG_DIRECTION_COUNT_ULL * (metadata_.frameCount - 1);
        phase = step % period;
        if (phase < metadata_.frameCount) {
            frame = uint32_t(phase);
        } else {
            frame = uint32_t(period - phase);
        }
    } else {
        frame = uint32_t(step % metadata_.frameCount);
    }
    if (frame == lastFrame_) {
        return false;
    }
    if (!memory_.ReadMediaFrame(frame, Consume, this)) {
        return Restore(nowMs);
    }
    upload_.PublishStoredFrame(metadata_);
    lastFrame_ = frame;
    return true;
} // Update

/**
    Passes one verified-read fragment into the caller-owned framebuffer.

    The enclosing frame read owns publication: these pixels stay provisional until
    its CRC succeeds.

    @param context Borrowed MediaPlayer with the active frame format.
    @param bytes Logical raw fragment; JPEG emits decoded RGB888 rows.
    @param count Number of complete-pixel bytes.
    @param offset Byte offset within the logical frame.

    @return true - The fragment was mapped into the CPU buffer.
    @return false - The fragment range or alignment was invalid.
*/
bool
MediaPlayer::Consume (
    void* context,
    const uint8_t* bytes,
    size_t count,
    size_t offset
    )
{
    MediaPlayer* player = nullptr;
    PixelFormat format;

    player = static_cast<MediaPlayer*>(context);
    format = PixelFormat::RGB888;
    if (player->metadata_.pixelFormat == PixelFormat::RGB565) {
        format = PixelFormat::RGB565;
    }
    return player->upload_.WriteStoredFramePixels(bytes, count, offset, format);
} // Consume

/**
    Returns the ID of the currently selected library video.

    @return id - Nonzero identifier of the selected or next usable library item.
    @return 0 - No matching library item is available or selected.
*/
uint32_t
MediaPlayer::GetLibraryVideoId (
    void
    ) const
{
    if (active_) {
        return libraryVideoId_;
    } else {
        return 0;
    }
} // GetLibraryVideoId

/**
    Returns the configured playback FPS cap.

    @return value - Configured video FPS cap.
    @return 0 - No override is applied; use the frame rate stored with the video.
*/
uint32_t
MediaPlayer::GetFpsLimit (
    void
    ) const
{
    return maxFps_;
} // GetFpsLimit

/**
    Returns the playback FPS numerator after applying the cap.

    @return value - Numerator of the effective frames-per-second fraction after applying the FPS
                    cap.
*/
uint32_t
MediaPlayer::GetEffectiveFpsNumerator (
    void
    ) const
{
    if (maxFps_ && uint64_t(maxFps_) * metadata_.fpsDenominator < metadata_.fpsNumerator) {
        return maxFps_;
    } else {
        return metadata_.fpsNumerator;
    }
} // GetEffectiveFpsNumerator

/**
    Returns the playback FPS denominator after applying the cap.

    @return value - Nonzero denominator of the effective frames-per-second fraction.
*/
uint32_t
MediaPlayer::GetEffectiveFpsDenominator (
    void
    ) const
{
    if (maxFps_ && uint64_t(maxFps_) * metadata_.fpsDenominator < metadata_.fpsNumerator) {
        return 1;
    } else {
        return metadata_.fpsDenominator;
    }
} // GetEffectiveFpsDenominator

/**
    Changes the playback FPS cap and reschedules the next frame.

    @param fps Maximum requested video frame rate; zero preserves the uploaded rate.
    @param nowMs Current monotonic time since boot, in milliseconds.

    @return true - The FPS limit was accepted and playback timing updated.
    @return false - The requested FPS limit is outside the supported range.
*/
bool
MediaPlayer::SetFpsLimit (
    uint32_t fps,
    uint32_t nowMs
    )
{
    if (fps > MAX_VIDEO_FPS) {
        return false;
    }
    if (fps == maxFps_) {
        return true;
    }
    // Keep the current timeline position when changing speed; no clip reload.
    if (active_) {
        stepOffset_ = PlaybackStep(nowMs);
        epochMs_ = nowMs;
    }
    maxFps_ = fps;
    return true;
} // SetFpsLimit

/**
    Advances the playback index using loop or ping-pong direction.

    @param nowMs Current monotonic time since boot, in milliseconds.

    @return value - Monotonic logical frame step computed from elapsed playback time and
                    effective FPS.
*/
uint64_t
MediaPlayer::PlaybackStep (
    uint32_t nowMs
    ) const
{
    return stepOffset_ + uint64_t(uint32_t(nowMs - epochMs_)) * GetEffectiveFpsNumerator() /
                             (MILLISECONDS_PER_SECOND_ULL * GetEffectiveFpsDenominator());
} // PlaybackStep

/**
    Selects a library video and restores its first checked frame.

    Selection closes the previous file before opening the chosen ID. The library
    controller owns fallback selection when this video fails validation.

    @param id Nonzero saved-video library ID.
    @param nowMs Current playback clock in milliseconds.

    @return true - The selected video opened and its first frame was verified.
    @return false - The ID, storage or first-frame validation failed.
*/
bool
MediaPlayer::ShowLibraryVideo (
    uint32_t id,
    uint32_t nowMs
    )
{
    Stop();
    libraryVideoId_ = id;
    return id && Restore(nowMs);
} // ShowLibraryVideo


/**
    Opens and displays one checked library photo.

    The caller holds render ownership. Pixel fragments remain provisional until
    the complete stored frame and its checksum succeed; only then is it published.

    @param id Nonzero saved-photo library ID.

    @return true - The requested photo was verified and published.
    @return false - Opening, decoding, mapping or verification failed.
*/
bool
MediaPlayer::ShowLibraryPhoto (
    uint32_t id
    )
{
    Stop();
    if (!memory_.OpenLibraryPhoto(id, metadata_) || !memory_.ReadMediaFrame(0, Consume, this)) {
        return false;
    }
    upload_.PublishStoredFrame(metadata_);
    return true;
} // ShowLibraryPhoto
