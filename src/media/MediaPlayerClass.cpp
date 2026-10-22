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
        if (!memory_.OpenLastMedia(metadata_)) {
            break;
        }
        if (memory_.ReadMediaFrame(0, Consume, this)) {
            upload_.PublishStoredFrame(metadata_);
            active_ = metadata_.type == MediaType::Video;
            epochMs_ = nowMs;
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
    uint64_t elapsed;
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
    elapsed = uint32_t(nowMs - epochMs_);
    step = elapsed * metadata_.fpsNumerator / (uint64_t(MILLISECONDS_PER_SECOND_U) * metadata_.fpsDenominator);
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
