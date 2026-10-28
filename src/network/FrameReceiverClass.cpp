#include "network/include/FrameReceiverClass.hpp"

/**
    Configures the expected frame size and decoded-pixel callback.

    @param bytes Maximum complete frame size accepted by this receiver, in bytes.
    @param context Caller-owned state passed unchanged to the callback.
    @param sink Callback that consumes pixel chunks; false cancels the current frame.
    @param holdCompletedFrame Whether the completed upload retains ownership until explicitly
                              released.
*/
FrameReceiver::FrameReceiver (
    size_t bytes,
    void* context,
    PixelSink sink,
    bool holdCompletedFrame
    ) : expected_(bytes), context_(context), sink_(sink), holdCompletedFrame_(holdCompletedFrame)
{
    return;
} // FrameReceiver

/**
    Claims framebuffer access when no upload owns it.

    @return true - Rendering owns the framebuffer until EndRender is called.
    @return false - An upload or another render operation currently owns the framebuffer.
*/
bool
FrameReceiver::TryBeginRender (
    void
    )
{
    State expected;

    expected = State::Idle;
    if (state_.compare_exchange_strong(expected, State::Render, std::memory_order_acquire)) {
        return true;
    }
    expected = State::Pending;
    return state_.compare_exchange_strong(expected, State::Render, std::memory_order_acquire);
} // TryBeginRender

/**
    Releases framebuffer ownership after rendering.
*/
void
FrameReceiver::EndRender (
    void
    )
{
    state_.store(State::Idle, std::memory_order_release);
} // EndRender

/**
    Reports whether a frame upload currently owns the receiver.

    @return true - A photo upload is currently receiving pixels.
    @return false - No photo upload is currently receiving pixels.
*/
bool
FrameReceiver::UploadInProgress (
    void
    ) const
{
    return state_.load(std::memory_order_acquire) == State::Upload;
} // UploadInProgress

/**
    Reports whether a complete frame is available for display.

    @return true - A complete frame has been published.
    @return false - No complete frame has been published.
*/
bool
FrameReceiver::HasFrame (
    void
    ) const
{
    return hasFrame_.load(std::memory_order_acquire);
} // HasFrame

/**
    Returns the revision of the most recently completed frame.

    @return value - Revision counter incremented when a new complete frame is published.
*/
FrameReceiver::Token
FrameReceiver::FrameRevision (
    void
    ) const
{
    return frameRevision_.load(std::memory_order_acquire);
} // FrameRevision

/**
    Starts a frame upload and returns its ownership token.

    Atomically claims an idle receiver for one upload and assigns a fresh nonzero token after
    checking the expected frame size. Receive counters and partial-pixel state are reset for the
    new owner. A busy receiver returns immediately so network callbacks never wait for rendering
    or saved-media access to finish.

    @param bytes Number of bytes to process.

    @return token - Nonzero ownership token for the new upload.
    @return 0 - The frame size is invalid or another operation owns the buffer.
*/
FrameReceiver::Token
FrameReceiver::Begin (
    size_t bytes
    )
{
    State expected;

    if (bytes != expected_ || !bytes || bytes % IMAGE_BYTES_PER_PIXEL || !sink_) {
        return 0;
    }
    expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Upload, std::memory_order_acquire)) {
        return 0;
    }
    if (++sequence_ == 0) {
        ++sequence_;
    }
    owner_ = sequence_;
    received_ = carryLength_ = 0;
    return owner_;
} // Begin

/**
    Accepts upload bytes and delivers complete pixels to the sink.

    @param token Ownership token returned when this upload was started.
    @param bytes Read-only input byte buffer.
    @param count Number of bytes to process.

    @return true - The chunk belongs to the current upload and was accepted.
    @return false - The token, byte range or receiver state is invalid.
*/
bool
FrameReceiver::Feed (
    Token token,
    const uint8_t* bytes,
    size_t count
    )
{
    size_t i;

    if (!Owns(token) || (count && !bytes) || count > expected_ - received_) {
        return false;
    }
    for (i = 0; i < count; ++i) {
        carry_[carryLength_++] = bytes[i];
        ++received_;
        if (carryLength_ == IMAGE_BYTES_PER_PIXEL) {
            sink_(context_, carry_, received_ / IMAGE_BYTES_PER_PIXEL - 1);
            carryLength_ = 0;
        }
    }

    return true;
} // Feed

/**
    Checks whether the owner has supplied a complete frame.

    @param token Ownership token returned when this upload was started.

    @return true - The token owns a complete upload with no partial pixel remaining.
    @return false - The token is stale, the upload is incomplete or a partial pixel remains.
*/
bool
FrameReceiver::CanFinish (
    Token token
    ) const
{
    return Owns(token) && received_ == expected_ && !carryLength_;
} // CanFinish

/**
    Publishes a complete uploaded frame and releases its upload ownership.

    Accepts completion only from the current owner after the exact frame size has arrived. It
    publishes frame availability and advances the revision before either releasing ownership or
    retaining a pending frame for explicit foreground handling. Incomplete or stale owners leave
    the current transaction unchanged.

    @param token Ownership token returned when this upload was started.

    @return true - The completed upload was published.
    @return false - The token is invalid or the complete frame is not yet available.
*/
bool
FrameReceiver::Finish (
    Token token
    )
{
    State conditionalValue1;

    if (!CanFinish(token)) {
        return false;
    }
    frameRevision_.store(token, std::memory_order_release);
    hasFrame_.store(true, std::memory_order_release);
    conditionalValue1 = {};
    if (holdCompletedFrame_) {
        conditionalValue1 = State::Pending;
    } else {
        conditionalValue1 = State::Idle;
    }
    Release(conditionalValue1);
    return true;
} // Finish

/**
    Discards the owning upload and releases its framebuffer lock.

    @param token Ownership token returned when this upload was started.
*/
void
FrameReceiver::Abort (
    Token token
    )
{
    if (!Owns(token)) {
        return;
    }
    // Partial writes cannot be rolled back: next render restores flashed content.
    if (received_) {
        hasFrame_.store(false, std::memory_order_release);
    }
    Release();
} // Abort

/**
    Checks whether a token belongs to the current upload.

    @param token Ownership token returned when this upload was started.

    @return true - The nonzero token identifies the current upload owner.
    @return false - The token is zero or no longer owns the upload.
*/
bool
FrameReceiver::Owns (
    Token token
    ) const
{
    return token && token == owner_;
} // Owns

/**
    Releases the current framebuffer ownership state.

    @param next Receiver state to publish after releasing ownership.
*/
void
FrameReceiver::Release (
    State next
    )
{
    owner_ = 0;
    received_ = carryLength_ = 0;
    state_.store(next, std::memory_order_release);
} // Release

/**
    Claims exclusive framebuffer ownership for an incremental media upload.

    @return true - External media owns the framebuffer until explicitly released.
    @return false - Another operation currently owns the framebuffer.
*/
bool
FrameReceiver::TryBeginExternal (
    void
    )
{
    State expected;

    expected = State::Idle;
    return state_.compare_exchange_strong(expected, State::External, std::memory_order_acquire);
} // TryBeginExternal

/**
    Releases exclusive saved-media framebuffer ownership.
*/
void
FrameReceiver::EndExternal (
    void
    )
{
    state_.store(State::Idle, std::memory_order_release);
} // EndExternal

/**
    Marks a verified stored-media frame complete and advances its revision.

    Foreground only while holding Render. Playback does not create an upload transaction, and
    keeps ownership until the driver submission finishes.
*/
void
FrameReceiver::PublishStoredFrame (
    void
    )
{
    if (++sequence_ == 0) {
        ++sequence_;
    }
    frameRevision_.store(sequence_, std::memory_order_release);
    hasFrame_.store(true, std::memory_order_release);
} // PublishStoredFrame

/**
    Clears availability after failed stored-media restoration.
*/
void
FrameReceiver::ClearStoredFrame (
    void
    )
{
    hasFrame_.store(false, std::memory_order_release);
} // ClearStoredFrame
