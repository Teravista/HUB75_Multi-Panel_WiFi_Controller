#pragma once
#include <atomic>

/**
    Arbitrates the single Core 1 foreground loop between networking and JPEG decoding.

    Core 0 reserves an advertised idle slot before preparing a parallel JPEG read. Core 1 then
    runs the submitted decode callback instead of starting another network operation. This
    ordering prevents either core from waiting for work that the other core cannot reach. Atomic
    state transitions publish callback data and completion across the cores; the scheduler neither
    launches a core nor owns decoder buffers. A failed reservation allows the caller to decode
    locally on Core 0.
*/
class Core1Scheduler {
public:
    // Enables the shared Core 1 network and decode scheduler.
    bool Enable(void);

    // Claims Core 1 for network work unless a decoder has reserved it.
    bool TryBeginNetwork(void);

    // Publishes whether Core 1 is available for a decode reservation.
    void OfferDecode(bool allowed);

    // Reserves an idle Core 1 for a JPEG decode without waiting.
    bool TryReserveDecode(void);

    // Releases an unused decode reservation back to network work.
    void CancelReservation(void);

    // Queues a decode callback using the current Core 1 reservation.
    bool SubmitDecode(void (*work)(void* context), void* context);

    // Runs a queued decode job and returns Core 1 to network work.
    bool RunDecode(void);

    // Reports whether the submitted decode job has finished.
    bool DecodeFinished(void) const;

private:
    enum class State : unsigned { Off, Network, Available, Reserved, Queued, Decoding };
    std::atomic<State> state_{State::Off};
    std::atomic<bool> completed_{true};
    void (*work_)(void* context) = nullptr;
    void* context_ = nullptr;
};
