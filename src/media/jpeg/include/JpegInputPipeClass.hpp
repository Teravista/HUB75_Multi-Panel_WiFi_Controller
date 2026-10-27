#pragma once
#include "media/jpeg/include/jpegWorker.hpp"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "config/defines.h"


/**
    Streams compressed JPEG bytes from SD reads on Core 0 to a decoder on Core 1.

    Its two bounded slots let the storage producer fill one block while the decoder consumes
    another. Atomic publication protects slot contents until the consumer has copied their final
    byte, and an end-of-input flag lets the decoder distinguish pending data from exhaustion.
    Decoder reads may yield while waiting for a block; producer slot requests return immediately
    when the pipe is full. The pipe performs no filesystem access and must be reset only when the
    previous decode has finished.
*/
class JpegInputPipe {
public:
    static constexpr size_t SlotBytes = 8 * 1024;

    // Resets the input pipe before streaming a new JPEG frame.
    void Reset(void);

    // Returns the next free input block without waiting for the consumer.
    uint8_t* Writable(void);

    // Makes a filled input block available to the JPEG decoder.
    void Publish(size_t bytes);

    // Signals that no more encoded input bytes will be published.
    void Finish(void);

    // Copies available encoded bytes to the decoder and waits for pending input.
    size_t Read(uint8_t* destination, size_t length);

    // Reports the number of encoded bytes consumed by the decoder.
    size_t Consumed(void) const;

private:
    struct Slot {
        alignas(DMA_BUFFER_ALIGNMENT) uint8_t bytes[SlotBytes];
        size_t count = 0;
        std::atomic<bool> ready{false};
    } slots_[JPEG_INPUT_PIPE_SLOT_COUNT];

    std::atomic<bool> finished_{false};
    unsigned producer_ = 0;
    unsigned consumer_ = 0;
    size_t used_ = 0;
    size_t consumed_ = 0;
};
