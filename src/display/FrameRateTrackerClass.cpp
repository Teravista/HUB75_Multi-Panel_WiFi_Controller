#include "display/include/FrameRateTrackerClass.hpp"
#include "config/defines.h"

/**
    Records display and video frame counters for the rolling average.

    Maintains a bounded one-second history while retaining the latest counters on every call.
    The oldest point straddles the averaging boundary so Read can interpolate it without
    allocating memory or waiting for another sample.

    @param nowMs Current monotonic time since boot, in milliseconds.
    @param displayFrames Cumulative number of completed display scans.
    @param videoFrames Cumulative number of presented video frames.
*/
void
FrameRateTracker::Sample (
    uint32_t nowMs,
    uint32_t displayFrames,
    uint32_t videoFrames
    )
{
    latest_ = {nowMs, displayFrames, videoFrames};
    // Keep the snapshot straddling the 30-second boundary for interpolation.
    while (count_ > 1 && uint32_t(nowMs - At(1).ms) >= WindowMs) {
        DropOldest();
    }

    if (!count_ || uint32_t(nowMs - At(count_ - 1).ms) >= MILLISECONDS_PER_SECOND_U) {
        if (count_ == Capacity) {
            DropOldest();
        }
        history_[(first_ + count_) % Capacity] = latest_;
        ++count_;
    }
} // Sample

/**
    Calculates display and video FPS over the available 30-second window.

    Uses the oldest retained sample and latest counters to measure a startup window or the most
    recent 30 seconds. When the window boundary falls between samples, it interpolates the
    excluded fraction instead of counting an entire older interval. Unsigned counter differences
    tolerate clock and frame-counter wrap independently. The calculation uses cached history and
    never waits for another frame.

    @return Average - Rolling display/video rates in tenths of FPS and windowMs in milliseconds;
                      available is false until enough samples exist.
*/
FrameRateTracker::Average
FrameRateTracker::Read (
    void
    ) const
{
    const FrameRateTracker::Point* start = nullptr;
    uint32_t elapsed;
    uint32_t duration;
    const Point* next = nullptr;
    uint32_t cut;
    uint32_t span;
    /**
        Calculates a wrap-safe frame rate with an interpolated window boundary.

        @param current Cumulative frame counter at the latest sample.
        @param from Cumulative frame counter at the earlier sample.
        @param after Cumulative frame counter at the following history sample.

        @return value - Interpolated frame rate in tenths of frames per second.
    */
    auto rate = [&](uint32_t current, uint32_t from, uint32_t after) {
        uint64_t milliFrames;
        uint64_t part;

        // Frame counters and the millisecond clock may independently wrap.
        milliFrames = uint64_t(uint32_t(current - from)) * MILLISECONDS_PER_SECOND_U;
        if (cut && span) {
            part = uint64_t(uint32_t(after - from)) * MILLISECONDS_PER_SECOND_U;
            milliFrames -= part / span * cut + part % span * cut / span;
        }
        return uint32_t(milliFrames * FPS_DECIMAL_SCALE / duration);
    };

    if (!count_) {
        return {};
    }
    start = &(At(0));
    elapsed = latest_.ms - start->ms;
    if (!elapsed) {
        return {};
    }
    if (elapsed < WindowMs) {
        duration = elapsed;
    } else {
        duration = WindowMs;
    }
    if (count_ > 1) {
        next = &At(1);
    } else {
        next = &latest_;
    }
    cut = elapsed - duration;
    span = next->ms - start->ms;

    return {true, duration, rate(latest_.display, start->display, next->display),
            rate(latest_.video, start->video, next->video)};
} // Read

/**
    Returns a history sample by its position in the ring buffer.

    @param index Zero-based index of the requested entry, slot or frame.

    @return reference - Borrowed history point at the requested logical ring-buffer position.
*/
const FrameRateTracker::Point&
FrameRateTracker::At (
    unsigned index
    ) const
{
    return history_[(first_ + index) % Capacity];
} // At

/**
    Removes the oldest frame-rate history sample.
*/
void
FrameRateTracker::DropOldest (
    void
    )
{
    first_ = (first_ + 1) % Capacity;
    --count_;
} // DropOldest
