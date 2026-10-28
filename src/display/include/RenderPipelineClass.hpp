#pragma once
#include "network/include/FrameBufferControllerClass.hpp"
#include <cstdint>

/**
    Coordinates safe handoff of a prepared image to the HUB75 display driver.

    Foreground image preparation and the interrupt-driven panel scanner progress independently.
    This class keeps CPU framebuffer ownership across preparation and any subsequent wait, then
    submits pixels only when the driver's writer is ready. A timed-out wait releases that
    ownership so a stalled scanner cannot indefinitely exclude uploads. Preparation and submission
    are borrowed synchronous callbacks; the pipeline owns neither the pixel buffers nor the panel-
    scanning hardware.
*/
class RenderPipeline {
public:
    enum class Result { Idle, Waiting, Submitted, Stalled };
    static constexpr uint64_t MaxWaitUs = 250000;
    // Borrowed synchronous callbacks; their contexts remain valid for the call.
    struct Prepare {
        void* context;
        void (*run)(void* context);
    };

    struct Submit {
        void* context;
        void (*run)(void* context, uint64_t scannerWaitUs);
    };

    // Binds the pipeline to the framebuffer ownership controller.
    explicit RenderPipeline(FrameBufferController& upload);

    // Releases any framebuffer ownership retained by the pipeline.
    ~RenderPipeline(void);

    // Disables copying so resource ownership cannot be duplicated.
    RenderPipeline(const RenderPipeline& other) = delete;

    // Disables copying so framebuffer ownership cannot be duplicated.
    RenderPipeline& operator=(const RenderPipeline& other) = delete;

    // Claims the source buffer and prepares a frame when the pipeline is free.
    bool TryPrepare(bool scannerReady, Prepare prepare);

    // Submits a prepared frame when the scanner is ready or releases a stalled frame.
    Result TrySubmit(bool scannerReady, Submit submit);

    // Releases the source buffer and clears the prepared-frame state.
    void Reset(void);

private:
    // Returns elapsed boot time in microseconds for submission deadlines.
    static uint64_t NowUs(void);

    FrameBufferController& upload_;
    uint64_t preparedUs_ = 0;
    bool prepared_ = false;
    bool blocked_ = false;
};
