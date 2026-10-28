#pragma once
#include <cstdint>

/**
    Measures the frame rates shown in the controller's Summary page.

    It turns cumulative panel-scan and video-presentation counters into a rolling 30-second
    average, including a shorter startup window when less history exists. The two rates remain
    separate so a smoothly scanned panel can still report slow video preparation. Core 0 records
    and reads bounded in-memory snapshots; the class does not change playback timing or query
    hardware, storage or the network.
*/
class FrameRateTracker {
public:
    static constexpr uint32_t WindowMs = 30000;

    struct Average {
        bool available = false;
        uint32_t windowMs = 0;
        uint32_t displayTenths = 0;
        uint32_t videoTenths = 0;
    };

    // Records display and video frame counters for the rolling average.
    void Sample(uint32_t nowMs, uint32_t displayFrames, uint32_t videoFrames);

    // Calculates display and video FPS over the available 30-second window.
    Average Read(void) const;

private:
    struct Point {
        uint32_t ms = 0;
        uint32_t display = 0;
        uint32_t video = 0;
    };

    static constexpr unsigned Capacity = 32;
    Point history_[Capacity]{};
    Point latest_;
    unsigned first_ = 0;
    unsigned count_ = 0;
    // Returns a history sample by its position in the ring buffer.
    const Point& At(unsigned index) const;

    // Removes the oldest frame-rate history sample.
    void DropOldest(void);

};
