#pragma once
#include "sd/include/MemoryManagerClass.hpp"
#include "sd/include/memoryManagerPlatform.hpp"
#include "sd/include/PlaybackPrefetchClass.hpp"
#include "media/jpeg/include/JpegInputPipeClass.hpp"
#include "media/jpeg/include/jpegFrameDecoder.hpp"
#include "media/include/mediaTransfer.hpp"
#include "ff.h"
#include <algorithm>
#include <cstring>

namespace MemoryStorage {
inline constexpr size_t ChunkBytes = MemoryManager::IoChunkBytes;
inline constexpr size_t JpegStreamBufferBytes = ChunkBytes / 2;
extern uint8_t readBuffer[ChunkBytes];
extern PlaybackPrefetch playbackPrefetch;
extern FIL* playbackFile;
struct PlaybackReadContext {
    FIL* file = nullptr;
    FRESULT* result = nullptr;
    MemoryManager::FrameReadTiming* timing = nullptr;
};

struct Slot {
    MediaMetadata metadata;
    size_t dataOffset = 0;
    uint32_t checksum = 0;
    uint32_t crcSeed = STORAGE_CRC32_INITIAL_U;
    bool frameChecksums = false;
};

struct FrameResult {
    bool complete = false;
    FRESULT result = FR_OK;
    const char* error = "media frame read failed";
};

/**
    Tracks checksum and pixel-consumer work for a buffered raw-frame fragment.

    Its progress cursor allows bounded portions of a completed read to be processed while the next
    SD DMA transfer is active. The borrowed source bytes and sink must remain valid until all
    pending work has drained. Rejection and running CRC state flow back to the enclosing frame
    read before it can publish success.
*/
struct FrameWork {
    const uint8_t* bytes = nullptr;
    size_t count = 0;
    size_t done = 0;
    size_t offset = 0;
    uint32_t crc;
    MemoryManager::ImageSink sink;
    void* context;
    MemoryManager::FrameReadTiming* timing;
    bool accepted = true;

    // Verifies and delivers a buffered raw frame fragment to its pixel sink.
    void ProcessPendingFramePixels(size_t maximum, bool overlapping = false);

    // Processes pending frame pixels while the next SD DMA transfer runs.
    static void ProcessPixelsDuringSdDma(void* context);

};

/**
    Temporarily connects pending raw-frame work to the SD driver's DMA overlap hook.

    It borrows a FrameWork object for the lifetime of one read scope and removes the callback on
    destruction. This keeps later transfers from calling a context whose buffer has been reused or
    whose stack frame has ended. The enclosing storage operation remains responsible for draining
    and validating the pending pixels.
*/
struct ReadWorkScope {
    // Registers bounded CPU work to overlap with SD read DMA.
    explicit ReadWorkScope(FrameWork& work);

    // Removes the SD DMA work callback when the read scope ends.
    ~ReadWorkScope(void);

};

/**
    Collects low-level SD timings for one optional frame-read measurement.

    The scope enables the storage driver's profile collector only when a timing output is
    supplied, then merges the completed counters when leaving the scope. It does not own the SPI
    hardware or issue reads; the surrounding frame operation determines which transactions fall
    within the measurement.
*/
struct DriverReadProfileScope {
    MemoryManager::FrameReadTiming* timing;

    // Starts optional low-level SD read timing collection.
    explicit DriverReadProfileScope(MemoryManager::FrameReadTiming* value);

    // Collects low-level SD timings when the frame read scope ends.
    ~DriverReadProfileScope(void);

};

// Reads, verifies, and streams one raw saved-media frame.
FrameResult ReadAndVerifyRawVideoFrame(FIL& file, const Slot& slot, uint32_t index, MemoryManager::ImageSink sink, void* context, MemoryManager::FrameReadTiming* timing = nullptr);


/**
    Bridges one encoded SD record to synchronous JPEG input and pixel callbacks.

    It retains remaining input, checksum and acceptance state while borrowing the open file,
    caller sink and shared scratch buffer. Refills account for every encoded byte, including skip
    requests and validation-only reads. The enclosing frame read owns its lifetime and decides
    whether the completed decode and checksum permit publication.
*/
struct JpegStream {
    FIL& file;
    FrameResult& result;
    MemoryManager::ImageSink sink;
    void* context;
    MemoryManager::FrameReadTiming* timing;
    size_t remaining;
    size_t buffered = 0;
    size_t used = 0;
    uint32_t crc = STORAGE_CRC32_INITIAL_U;
    uint16_t lastWord = 0;
    bool accepted = true;
    uint32_t* payloadCrc = nullptr;
    static constexpr size_t BufferBytes = JpegStreamBufferBytes;

    // Refills the bounded encoded JPEG input buffer from storage.
    bool RefillJpegInputBuffer(void);

    // Supplies encoded JPEG bytes and updates frame verification state.
    static size_t ReadEncodedJpegInput(void* context, uint8_t* bytes, size_t length);

    // Forwards decoded JPEG fragments to the frame's pixel sink.
    static bool WriteDecodedJpegPixels(void* context, const uint8_t* bytes, size_t count, size_t offset);

};

#if JPEG_PARALLEL_READER
extern JpegInputPipe jpegPipe;

/**
    Borrows one JPEG stream for a reserved Core 1 decoder job.

    It carries the dimensions, output mode and optional profiling destinations needed by the
    worker while Core 0 supplies encoded bytes through JpegInputPipe. The job and all referenced
    state must outlive worker completion. It neither starts a core nor owns filesystem access; the
    surrounding pipeline coordinates that lifetime.
*/
struct ParallelDecode {
    JpegStream& stream;
    uint16_t width;
    uint16_t height;
    int& error;
    JpegFrameDecoder::Profile* stages;
    JpegFrameDecoder::Clock clock;
    bool rgb565;
    bool decoded = false;
    uint64_t elapsedUs = 0;
    uint64_t waitUs = 0;

    // Supplies encoded input to the worker through the JPEG input pipe.
    static size_t ReadEncodedJpegInput(void* state, uint8_t* destination, size_t bytes);

    // Decodes a JPEG frame on the reserved worker and records its result.
    static void DecodeJpegOnWorker(void* state);

};

// Overlaps SD input reads with JPEG decoding on an available Core 1.
bool DecodeJpegParallel(JpegStream& stream, uint16_t width, uint16_t height, int& error, JpegFrameDecoder::Profile* stages, JpegFrameDecoder::Clock clock, bool rgb565, uint64_t& decodeUs, uint64_t& waitUs, uint32_t& pipelineBytes);

#endif

// Decodes and checks a bounded JPEG record already positioned by the storage manager.
FrameResult DecodeEncodedJpegFrame(FIL& file, const MediaMetadata& metadata, size_t bytes, uint32_t checksum, MemoryManager::ImageSink sink, void* context, MemoryManager::FrameReadTiming* timing, bool outputRGB565);

// Checks whether a file is the active playback handle using the prefetch cache.
bool IsPrefetchedPlaybackFile(FIL& file);

// Returns the logical playback position or the ordinary file position.
uint64_t GetPlaybackFilePosition(FIL& file);

// Moves the logical playback cursor or seeks an uncached file.
FRESULT SeekPlaybackFile(FIL& file, uint64_t offset);

// Reads one playback cache block and records optional SD timing.
bool ReadPlaybackCacheBlock(FIL& file, uint64_t offset, uint8_t* destination, size_t bytes, FRESULT& result, MemoryManager::FrameReadTiming* timing);

// Reads one playback block using the caller's file and error state.
bool ReadPrefetchBackendBlock(void* context, uint64_t offset, uint8_t* data, size_t size);

// Reads playback bytes through the cache or directly for non-playback files.
FRESULT ReadPlaybackFileBytes(FIL& file, uint8_t* destination, UINT bytes, UINT* count, MemoryManager::FrameReadTiming* timing);

// Speculatively fills the next playback cache block without invalidating current data.
[[maybe_unused]] void PrefetchNextPlaybackBlock(FIL& file, MemoryManager::FrameReadTiming* timing);

} // namespace MemoryStorage
