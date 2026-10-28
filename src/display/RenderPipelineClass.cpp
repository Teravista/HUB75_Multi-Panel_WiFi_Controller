#include "display/include/RenderPipelineClass.hpp"

#include "pico/time.h"

/**
    Releases any framebuffer ownership retained by the pipeline.
*/
RenderPipeline::~RenderPipeline (
    void
    )
{
    Reset();
} // ~RenderPipeline

/**
    Releases the source buffer and clears the prepared-frame state.
*/
void
RenderPipeline::Reset (
    void
    )
{
    if (prepared_) {
        upload_.EndRender();
    }
    prepared_ = false;
    blocked_ = false;
} // Reset

/**
    Returns elapsed boot time in microseconds for submission deadlines.

    @return value - Monotonic profiling clock time in microseconds.
*/
uint64_t
RenderPipeline::NowUs (
    void
    )
{
    return to_us_since_boot(get_absolute_time());
} // NowUs

/**
    Binds the pipeline to the framebuffer ownership controller.

    @param upload Shared framebuffer owner and photo upload controller.
*/
RenderPipeline::RenderPipeline (
    FrameBufferController& upload
    ) : upload_(upload)
{
    return;
} // RenderPipeline

/**
    Claims the source buffer and prepares a frame when the pipeline is free.

    Acquires CPU-frame ownership before invoking the borrowed preparation callback. Preparation
    itself is synchronous and may include SD reads or decoding. Once it returns, the prepared
    frame remains protected until TrySubmit succeeds or Reset releases it; an unavailable owner is
    reported immediately rather than waited on.

    @param scannerReady Whether the display scanner can currently accept another frame.
    @param prepare Callback that prepares pixels after source-buffer ownership is acquired.

    @return true - The source buffer was acquired and the frame prepared.
    @return false - The scanner, pipeline or source buffer is not ready.
*/
bool
RenderPipeline::TryPrepare (
    bool scannerReady,
    Prepare prepare
    )
{
    if (prepared_ || (blocked_ && !scannerReady)) {
        return false;
    }
    blocked_ = false;
    if (!upload_.TryBeginRender()) {
        return false;
    }
    prepared_ = true;
    prepare.run(prepare.context);
    preparedUs_ = NowUs();
    return true;
} // TryPrepare

/**
    Submits a prepared frame when the scanner is ready or releases a stalled frame.

    Polls scanner readiness rather than waiting in a loop. While a prepared frame is pending, CPU-
    frame ownership stays held so uploads cannot overwrite its pixels. A ready scanner triggers
    the synchronous submission callback and releases ownership; a deadline expiry releases the
    frame and delays new preparation until recovery.

    @param scannerReady Whether the display scanner can currently accept another frame.
    @param submit Callback that submits the prepared frame to the display driver.

    @return Result::Idle - No prepared frame is waiting.
    @return Result::Waiting - A frame is prepared but the scanner is not ready.
    @return Result::Stalled - The wait timed out and framebuffer ownership was released.
    @return Result::Submitted - The prepared frame was submitted to the display.
*/
RenderPipeline::Result
RenderPipeline::TrySubmit (
    bool scannerReady,
    Submit submit
    )
{
    uint64_t waitUs;

    if (!prepared_) {
        return Result::Idle;
    }
    waitUs = NowUs() - preparedUs_;
    if (!scannerReady) {
        if (waitUs < MaxWaitUs) {
            return Result::Waiting;
        }
        // A stopped scanner must not hold off uploads indefinitely. Wait
        // for recovery before preparing more frames; keep the DMA untouched.
        Reset();
        blocked_ = true;
        return Result::Stalled;
    }
    submit.run(submit.context, waitUs);
    Reset();
    return Result::Submitted;
} // TrySubmit
