#include "core/include/Core1SchedulerClass.hpp"

/**
    Enables the shared Core 1 network and decode scheduler.

    @return true - The disabled scheduler was enabled for network work.
    @return false - The scheduler was already enabled or owned by another operation.
*/
bool
Core1Scheduler::Enable (
    void
    )
{
    State expected;

    expected = State::Off;
    return state_.compare_exchange_strong(expected, State::Network, std::memory_order_release);
} // Enable

/**
    Claims Core 1 for network work unless a decoder has reserved it.

    Core 1 only. Retract the idle offer before any network/lifecycle operation.

    @return true - Core 1 is available for network work.
    @return false - A decoder reservation or decode job currently owns Core 1.
*/
bool
Core1Scheduler::TryBeginNetwork (
    void
    )
{
    State expected;

    expected = State::Available;
    if (state_.compare_exchange_strong(expected, State::Network, std::memory_order_acquire)) {
        return true;
    }
    return expected == State::Network;
} // TryBeginNetwork

/**
    Publishes whether Core 1 is available for a decode reservation.

    @param allowed Whether Core 1 may currently be offered to the JPEG decoder.
*/
void
Core1Scheduler::OfferDecode (
    bool allowed
    )
{
    State nextState;

    nextState = {};
    if (allowed) {
        nextState = State::Available;
    } else {
        nextState = State::Network;
    }
    state_.store(nextState, std::memory_order_release);
} // OfferDecode

/**
    Reserves an idle Core 1 for a JPEG decode without waiting.

    Core 0 only. Never wait for a busy/unlaunched Core 1; fall back locally.

    @return true - Core 1 was reserved for decoding.
    @return false - Core 1 is busy or unavailable; the caller may decode locally.
*/
bool
Core1Scheduler::TryReserveDecode (
    void
    )
{
    State expected;

    expected = State::Available;
    if (!state_.compare_exchange_strong(expected, State::Reserved, std::memory_order_acquire)) {
        return false;
    }

    // A successful reservation belongs to Core 0 until it queues or cancels the decoder job.
    completed_.store(false, std::memory_order_relaxed);
    return true;
} // TryReserveDecode

/**
    Releases an unused decode reservation back to network work.
*/
void
Core1Scheduler::CancelReservation (
    void
    )
{
    // Reserved cannot transition except through this Core 0 producer.
    if (state_.load(std::memory_order_acquire) != State::Reserved) {
        return;
    }
    completed_.store(true, std::memory_order_release);
    state_.store(State::Network, std::memory_order_release);
} // CancelReservation

/**
    Queues a decode callback using the current Core 1 reservation.

    Publishes a borrowed decode callback and context only while Core 0 owns a valid reservation.
    Release ordering makes them visible before Core 1 can observe the queued state. Submission
    does not run or wait for the callback; its context and workspace must stay alive until
    DecodeFinished reports completion.

    @param work Borrowed callback run once by Core 1 for the reserved decode job.
    @param context Caller-owned state passed unchanged to the callback.

    @return true - The callback was queued on the reserved worker.
    @return false - The callback is missing or no valid decode reservation is held.
*/
bool
Core1Scheduler::SubmitDecode (
    void (*work)(void*),
    void* context
    )
{
    if (!work || state_.load(std::memory_order_acquire) != State::Reserved) {
        return false;
    }

    // Publish the callback and context before Core 1 can observe the queued state.
    work_ = work;
    context_ = context;
    state_.store(State::Queued, std::memory_order_release);
    return true;
} // SubmitDecode

/**
    Runs a queued decode job and returns Core 1 to network work.

    Core 1 only. IRQ-based networking remains enabled while the job runs.

    @return true - A queued decode callback was run.
    @return false - No queued decode callback was available.
*/
bool
Core1Scheduler::RunDecode (
    void
    )
{
    State expected;

    expected = State::Queued;
    if (!state_.compare_exchange_strong(expected, State::Decoding, std::memory_order_acquire)) {
        return false;
    }
    work_(context_);
    // No callback/context/workspace access after this release. The foreground
    // loop can resume networking as soon as the job has completed.
    completed_.store(true, std::memory_order_release);
    state_.store(State::Network, std::memory_order_release);
    return true;
} // RunDecode

/**
    Reports whether the submitted decode job has finished.

    @return true - The submitted worker job has finished.
    @return false - The submitted worker job has not finished.
*/
bool
Core1Scheduler::DecodeFinished (
    void
    ) const
{
    return completed_.load(std::memory_order_acquire);
} // DecodeFinished
