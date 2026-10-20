#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "config/defines.h"

/**
    Tracks ownership and completion of the shared image frame during uploads.

    It serializes rendering, incoming RGB888 pixel fragments and saved-photo restoration
    through an atomic ownership state. Upload tokens distinguish the current sender from stale
    callbacks, while completed-frame revisions notify foreground code that the displayed content
    has changed. Fragmented input is assembled into complete pixels before calling the supplied
    sink. It owns the receive state and small partial-pixel buffer; sockets, full-frame storage
    and SD persistence belong to higher-level controllers.
*/
class FrameReceiver {
public:
    using Token = uint32_t;
    using PixelSink = void (*)(void* context, const uint8_t* bytes, size_t pixelIndex);

    // Configures the expected frame size and decoded-pixel callback.
    FrameReceiver(size_t bytes, void* context, PixelSink sink, bool holdCompletedFrame = false);

    // Claims framebuffer access when no upload owns it.
    bool TryBeginRender(void);

    // Releases framebuffer ownership after rendering.
    void EndRender(void);

    // Reports whether a frame upload currently owns the receiver.
    bool UploadInProgress(void) const;

    // Reports whether a complete frame is available for display.
    bool HasFrame(void) const;

    // Returns the revision of the most recently completed frame.
    Token FrameRevision(void) const;

    // Starts a frame upload and returns its ownership token.
    Token Begin(size_t bytes);

    // Accepts upload bytes and delivers complete pixels to the sink.
    bool Feed(Token token, const uint8_t* bytes, size_t count);

    // Checks whether the owner has supplied a complete frame.
    bool CanFinish(Token token) const;

    // Publishes a complete uploaded frame and releases its upload ownership.
    bool Finish(Token token);

    // Discards the owning upload and releases its framebuffer lock.
    void Abort(Token token);

private:
    enum class State : uint32_t { Idle, Render, Upload, Pending };

    // Checks whether a token belongs to the current upload.
    bool Owns(Token token) const;

    // Releases the current framebuffer ownership state.
    void Release(State next = State::Idle);

    const size_t expected_;
    void* const context_;
    const PixelSink sink_;
    const bool holdCompletedFrame_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> hasFrame_{false};
    std::atomic<Token> frameRevision_{0};
    Token sequence_ = 0;
    Token owner_ = 0;
    size_t received_ = 0;
    size_t carryLength_ = 0;
    uint8_t carry_[IMAGE_BYTES_PER_PIXEL]{};
};
