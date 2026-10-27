#include "media/jpeg/include/JpegInputPipeClass.hpp"

/**
    Resets the input pipe before streaming a new JPEG frame.
*/
void
JpegInputPipe::Reset (
    void
    )
{
    size_t slotIndex;
    JpegInputPipe::Slot* slot = nullptr;

    for (slotIndex = 0; slotIndex < sizeof(slots_) / sizeof(slots_[0]); ++slotIndex) {
        slot = &(slots_[slotIndex]);
        slot->ready.store(false, std::memory_order_relaxed);
    }
    producer_ = consumer_ = used_ = 0;
    consumed_ = 0;
    finished_.store(false, std::memory_order_relaxed);
} // Reset

/**
    Returns the next free input block without waiting for the consumer.

    @return pointer - Writable producer slot, valid until it is published.
    @return nullptr - Both input slots are still owned by the consumer.
*/
uint8_t*
JpegInputPipe::Writable (
    void
    )
{
    if (slots_[producer_].ready.load(std::memory_order_acquire)) {
        return nullptr;
    } else {
        return slots_[producer_].bytes;
    }
} // Writable

/**
    Makes a filled input block available to the JPEG decoder.

    @param bytes Number of bytes to process.
*/
void
JpegInputPipe::Publish (
    size_t bytes
    )
{
    slots_[producer_].count = bytes;
    slots_[producer_].ready.store(true, std::memory_order_release);
    producer_ ^= 1;
} // Publish

/**
    Signals that no more encoded input bytes will be published.
*/
void
JpegInputPipe::Finish (
    void
    )
{
    finished_.store(true, std::memory_order_release);
} // Finish

/**
    Copies available encoded bytes to the decoder and waits for pending input.

    Consumes published slots in order and releases each slot only after its final byte has been
    copied or skipped. A null destination advances input without copying, as required by JPEG skip
    requests. If input is temporarily absent, the consumer yields until the producer publishes
    data or marks completion; after completion it rechecks the slot to avoid losing the final
    publish.

    @param destination Writable destination buffer; the caller must provide sufficient storage.
    @param length Number of bytes to process.

    @return count - Number of encoded bytes copied or skipped, up to length.
    @return 0 - No more encoded input is available.
*/
size_t
JpegInputPipe::Read (
    uint8_t* destination,
    size_t length
    )
{
    size_t done;
    JpegInputPipe::Slot* slot = nullptr;
    size_t part;

    done = 0;
    while (done < length) {
        slot = &(slots_[consumer_]);
        if (!slot->ready.load(std::memory_order_acquire)) {
            if (finished_.load(std::memory_order_acquire)) {
                // Observe a final publish that happened immediately before Finish.
                if (!slot->ready.load(std::memory_order_acquire)) {
                    break;
                }
            } else {
                JpegPipelineYield();
                continue;
            }
        }
        part = std::min(length - done, slot->count - used_);
        if (destination) {
            std::memcpy(destination + done, slot->bytes + used_, part);
        }
        used_ += part;
        done += part;
        consumed_ += part;
        if (used_ == slot->count) {
            used_ = 0;
            slot->ready.store(false, std::memory_order_release);
            consumer_ ^= 1;
        }
    }

    return done;
} // Read

/**
    Reports the number of encoded bytes consumed by the decoder.

    @return value - Number of encoded bytes consumed since Reset, in bytes; inspect only after
                    worker completion.
*/
size_t
JpegInputPipe::Consumed (
    void
    ) const
{
    return consumed_;
} // Consumed
