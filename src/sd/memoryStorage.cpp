#include "sd/include/memoryStorage.hpp"
#include "sd/include/storageCrc32.hpp"
#include "pico/time.h"

#define SD_DMA_OVERLAP_WORK_BYTES 510
#define JPEG_END_OF_IMAGE_MARKER 0xffd9
#define JPEG_MARKER_BYTES 2
#define JPEG_PIPELINE_PRIME_BYTES 4096

#define LIBRARY_PREVIEW_SAMPLE_CENTER 2
#define LIBRARY_PREVIEW_SAMPLE_STEP 4

namespace MemoryStorage {

StorageState managerState{};

alignas(DMA_BUFFER_ALIGNMENT) uint8_t readBuffer[ChunkBytes];
PlaybackPrefetch playbackPrefetch;
FIL* playbackFile = nullptr;
#if JPEG_PARALLEL_READER
JpegInputPipe jpegPipe;
#endif

/**
    Checks whether a file is the active playback handle using the prefetch cache.

    @param file Open FatFs file used by this operation.

    @return true - The file is the active playback file backed by the prefetch cache.
    @return false - The file must use direct FatFs access.
*/
bool
IsPrefetchedPlaybackFile (
    FIL& file
    )
{
    return &file == playbackFile;
} // IsPrefetchedPlaybackFile

/**
    Returns the logical playback position or the ordinary file position.

    @param file Open FatFs file used by this operation.

    @return value - Logical byte position of the playback cache, or the direct FatFs file
                    position.
*/
uint64_t
GetPlaybackFilePosition (
    FIL& file
    )
{
    if (IsPrefetchedPlaybackFile(file)) {
        return playbackPrefetch.Tell();
    } else {
        return uint64_t(f_tell(&file));
    }
} // GetPlaybackFilePosition

/**
    Moves the logical playback cursor or seeks an uncached file.

    @param file Open FatFs file used by this operation.
    @param offset Byte offset within the source image or file.

    @return FR_OK - The filesystem operation completed successfully.
    @return other - FatFs error code describing the failed filesystem operation.
*/
FRESULT
SeekPlaybackFile (
    FIL& file,
    uint64_t offset
    )
{
    if (!IsPrefetchedPlaybackFile(file)) {
        return f_lseek(&file, FSIZE_t(offset));
    }
    playbackPrefetch.Seek(offset);
    return FR_OK;
} // SeekPlaybackFile

/**
    Reads one playback cache block and records optional SD timing.

    Positions the backend file only when its physical cursor differs from the requested cache-
    block offset, then performs one synchronous exact-length read. A short successful FatFs read
    is converted into an error so a partially filled block cannot become a valid cache entry.
    Optional counters separate seek and read time for the enclosing playback profile.

    @param file Open FatFs file used by this operation.
    @param offset Byte offset within the source image or file.
    @param destination Writable destination buffer; the caller must provide sufficient storage.
    @param bytes Number of bytes to process.
    @param result Output FatFs or frame-validation result, including any failure details.
    @param timing Optional output counters for the work and I/O measured by this operation.

    @return true - The requested block was read from storage successfully.
    @return false - The read or positioning failed; result contains the FatFs code.
*/
bool
ReadPlaybackCacheBlock (
    FIL& file,
    uint64_t offset,
    uint8_t* destination,
    size_t bytes,
    FRESULT& result,
    MemoryManager::FrameReadTiming* timing
    )
{
    uint64_t started;
    uint64_t readStarted;
    UINT count;

    if (timing) {
        started = to_us_since_boot(get_absolute_time());
    } else {
        started = 0;
    }
    if (uint64_t(f_tell(&file)) != offset) {
        result = f_lseek(&file, FSIZE_t(offset));
    }
    if (timing) {
        timing->seekUs += to_us_since_boot(get_absolute_time()) - started;
    }

    if (result != FR_OK) {
        return false;
    }
    if (timing) {
        readStarted = to_us_since_boot(get_absolute_time());
    } else {
        readStarted = 0;
    }
    count = 0;
    result = f_read(&file, destination, UINT(bytes), &count);
    if (timing) {
        timing->readUs += to_us_since_boot(get_absolute_time()) - readStarted;
        ++timing->readCalls;
    }
    if (result == FR_OK && count != bytes) {
        result = FR_INT_ERR;
    }
    return result == FR_OK;
} // ReadPlaybackCacheBlock

/**
    Reads one playback block using the caller's file and error state.

    @param context Borrowed playback read context; valid until the cache call returns.
    @param offset Absolute file offset of the requested block, in bytes.
    @param data Destination buffer for the requested file bytes.
    @param size Number of bytes to read into the destination.

    @return true - The complete block was read.
    @return false - Seeking or reading failed; the context receives the filesystem error.
*/
bool
ReadPrefetchBackendBlock (
    void* context,
    uint64_t offset,
    uint8_t* data,
    size_t size
    )
{
    PlaybackReadContext* read = nullptr;

    read = static_cast<PlaybackReadContext*>(context);
    return ReadPlaybackCacheBlock(*read->file, offset, data, size, *read->result, read->timing);
} // ReadPrefetchBackendBlock

/**
    Reads playback bytes through the cache or directly for non-playback files.

    Uses the logical prefetch cursor only for the active playback handle; unrelated files keep
    normal FatFs positioning and direct reads. Cache misses invoke a borrowed synchronous backend
    that preserves filesystem errors and optional timing. The returned byte count still needs to
    be checked by callers, because reaching EOF can produce fewer bytes without a transport-style
    wait.

    @param file Open FatFs file used by this operation.
    @param destination Writable destination buffer; the caller must provide sufficient storage.
    @param bytes Number of bytes to process.
    @param count Output number of bytes actually transferred.
    @param timing Optional output counters for the work and I/O measured by this operation.

    @return FR_OK - The filesystem operation completed successfully.
    @return other - FatFs error code describing the failed filesystem operation.
*/
FRESULT
ReadPlaybackFileBytes (
    FIL& file,
    uint8_t* destination,
    UINT bytes,
    UINT* count,
    MemoryManager::FrameReadTiming* timing
    )
{
    PlaybackReadContext readContext;
    uint64_t started;
    FRESULT result;
    FRESULT nextResult;
    size_t delivered;

    if (!IsPrefetchedPlaybackFile(file)) {
        if (timing) {
            started = to_us_since_boot(get_absolute_time());
        } else {
            started = 0;
        }
        result = f_read(&file, destination, bytes, count);
        if (timing) {
            timing->readUs += to_us_since_boot(get_absolute_time()) - started;
            ++timing->readCalls;
        }
        return result;
    }
    nextResult = FR_OK;
    delivered = 0;

    // Use the bounded cache only for the active playback file; ordinary file reads keep their own cursor.
    readContext.file = &file;
    readContext.result = &nextResult;
    readContext.timing = timing;
    playbackPrefetch.Read(destination, bytes, delivered, f_size(&file),
                          {&readContext, ReadPrefetchBackendBlock});
    *count = UINT(delivered);
    return nextResult;
} // ReadPlaybackFileBytes

/**
    Speculatively fills the next playback cache block without invalidating current data.

    @param file Open FatFs file used by this operation.
    @param timing Optional output counters for the work and I/O measured by this operation.
*/
[[maybe_unused]] void
PrefetchNextPlaybackBlock (
    FIL& file,
    MemoryManager::FrameReadTiming* timing
    )
{
    PlaybackReadContext readContext;
    FRESULT result;

    if (!IsPrefetchedPlaybackFile(file)) {
        return;
    }
    result = FR_OK;
    // A speculative error cannot invalidate already verified current-frame bytes.
    // The next required read follows the normal disk-error/close/remount policy.
    readContext.file = &file;
    readContext.result = &result;
    readContext.timing = timing;
    playbackPrefetch.Ahead(f_size(&file), {&readContext, ReadPrefetchBackendBlock});
} // PrefetchNextPlaybackBlock

/**
    Verifies and delivers a buffered raw frame fragment to its pixel sink.

    Processes at most the requested portion of a previously read fragment, updating its running
    CRC and invoking the synchronous pixel sink at the matching logical offset. Rejection prevents
    further work on that fragment. During SD DMA overlap, timing records the CPU work hidden
    behind the transfer rather than treating it as an independent wall-time cost.

    @param maximum Maximum number of pending bytes to process in this step.
    @param overlapping Whether this CPU work runs while an SD DMA transfer is active.
*/
void
FrameWork::ProcessPendingFramePixels (
    size_t maximum,
    bool overlapping
    )
{
    size_t part;
    uint64_t start;
    uint64_t stage;
    uint64_t end;
    /**
        Returns elapsed boot time in microseconds for read profiling.

        @return value - Monotonic time since boot in microseconds.
    */
    auto nowUs = [] { return to_us_since_boot(get_absolute_time()); };

    if (!accepted || done == count) {
        return;
    }
    part = std::min(maximum, count - done);

    if (timing) {
        start = nowUs();
    } else {
        start = 0;
    }
    crc = UpdateImageCrc32(crc, bytes + done, part);
    if (timing) {
        stage = nowUs();
    } else {
        stage = 0;
    }
    if (timing) {
        timing->crcUs += stage - start;
    }
    if (sink) {
        accepted = sink(context, bytes + done, part, offset + done);
    }
    if (timing) {
        end = nowUs();
        timing->consumerUs += end - stage;
        if (overlapping) {
            timing->overlapWorkUs += end - start;
            timing->overlapBytes += uint32_t(part);
        }
    }
    done += part;
} // ProcessPendingFramePixels

/**
    Processes pending frame pixels while the next SD DMA transfer runs.

    @param context Caller-owned state passed unchanged to the callback.
*/
void
FrameWork::ProcessPixelsDuringSdDma (
    void* context
    )
{
    static_cast<FrameWork*>(context)->ProcessPendingFramePixels(SD_DMA_OVERLAP_WORK_BYTES, true);
} // ProcessPixelsDuringSdDma

/**
    Registers bounded CPU work to overlap with SD read DMA.

    @param work Pending frame conversion state kept alive for the scope lifetime.
*/
ReadWorkScope::ReadWorkScope (
    FrameWork& work
    )
{
    SetMemoryStorageReadWork(FrameWork::ProcessPixelsDuringSdDma, &work);
} // ReadWorkScope

/**
    Removes the SD DMA work callback when the read scope ends.
*/
ReadWorkScope::~ReadWorkScope (
    void
    )
{
    SetMemoryStorageReadWork(nullptr, nullptr);
} // ~ReadWorkScope

/**
    Starts optional low-level SD read timing collection.

    @param value Optional output accumulator for SD read profile counters.
*/
DriverReadProfileScope::DriverReadProfileScope (
    MemoryManager::FrameReadTiming* value
    ) : timing(value)
{
    if (timing) {
        BeginMemoryStorageReadProfile();
    }
} // DriverReadProfileScope

/**
    Collects low-level SD timings when the frame read scope ends.
*/
DriverReadProfileScope::~DriverReadProfileScope (
    void
    )
{
    sd_spi_read_profile_t profile;

    if (!timing) {
        return;
    }
    profile = EndMemoryStorageReadProfile();
    timing->commandUs = profile.command_us;
    timing->tokenUs = profile.token_us;
    timing->transferUs = profile.transfer_us;
    timing->sectorCrcUs = profile.crc_us;
    timing->transactions = profile.transactions;
    timing->sectors = profile.sectors;
} // ~DriverReadProfileScope

/**
    Reads, verifies, and streams one raw saved-media frame.

    Positions the reader at the chosen frame, then alternates the shared I/O buffer's two halves.
    While one half is being read by SD DMA, bounded checksum/mapping work can consume the
    previously completed half; the pending fragment is drained before its storage can be reused.

    The stored frame checksum is checked after every payload byte has been processed. Sink
    rejection, a short read or a checksum mismatch leaves the frame incomplete, even if earlier
    provisional pixels were delivered. The caller holds storage and frame ownership throughout
    this synchronous operation and publishes only success.

    @param file Open FatFs file used by this operation.
    @param slot Validated stored-media header and location information.
    @param index Zero-based index of the requested entry, slot or frame.
    @param sink Callback that consumes pixel chunks; false cancels the current frame.
    @param context Caller-owned state passed unchanged to the callback.
    @param timing Optional output counters for the work and I/O measured by this operation.

    @return result - Inspection/read result including validity, metadata and any I/O or
                     validation error.
*/
FrameResult
ReadAndVerifyRawVideoFrame (
    FIL& file,
    const Slot& slot,
    uint32_t index,
    MemoryManager::ImageSink sink,
    void* context,
    MemoryManager::FrameReadTiming* timing
    )
{
    FrameResult read;
    uint64_t started;
    size_t frameBytes;
    size_t pixelBytes;
    uint64_t checksumBytes;
    uint64_t stride;
    uint32_t initialChecksum;
    FrameWork pending;
    constexpr size_t PlaybackChunkBytes = ChunkBytes / 2;
    unsigned nextBuffer;
    UINT count;
    size_t offset;
    size_t chunk;
    UINT bytes;
    uint8_t* destination = nullptr;
    uint32_t expected;
    uint8_t checksum[FRAME_CHECKSUM_BYTES];
    /**
        Returns elapsed boot time in microseconds for read profiling.

        @return value - Monotonic time since boot in microseconds.
    */
    auto nowUs = [] { return to_us_since_boot(get_absolute_time()); };

    read = {};
    DriverReadProfileScope driverProfile(timing);

    if (timing) {
        started = nowUs();
    } else {
        started = 0;
    }
    frameBytes = slot.metadata.FrameBytes();
    pixelBytes = slot.metadata.PixelBytes();
    checksumBytes = {};
    if (slot.frameChecksums) {
        checksumBytes = uint64_t(FRAME_CHECKSUM_BYTES);
    } else {
        checksumBytes = 0ull;
    }
    stride = frameBytes + checksumBytes;
    read.result = SeekPlaybackFile(file, slot.dataOffset + stride * index);
    if (timing) {
        timing->seekUs += nowUs() - started;
    }
    initialChecksum = {};
    if (slot.frameChecksums) {
        initialChecksum = STORAGE_CRC32_INITIAL_U;
    } else {
        initialChecksum = slot.crcSeed;
    }
    pending = {nullptr, 0, 0, 0, initialChecksum, sink, context, timing};
    static_assert(PlaybackChunkBytes % SD_SECTOR_BYTES == 0 && PlaybackChunkBytes % RGB888_PIXEL_BYTES == 0);
    nextBuffer = 0;
    count = 0;
    for (offset = 0; read.result == FR_OK && offset < frameBytes;) {
        chunk = PlaybackChunkBytes;
        if (GetPlaybackFilePosition(file) % SD_SECTOR_BYTES) {
            // Start with a short RGB-complete prefix ending on a sector boundary.
            // Later 12 KiB reads are sector aligned, so FatFs can read directly
            // instead of splitting every chunk into cached head/bulk/tail reads.
            chunk = SD_SECTOR_BYTES - size_t(GetPlaybackFilePosition(file) % SD_SECTOR_BYTES);
            while (chunk % pixelBytes) {
                chunk += SD_SECTOR_BYTES;
            }
        }
        bytes = UINT(std::min(chunk, frameBytes - offset));
        destination = readBuffer + nextBuffer * PlaybackChunkBytes;
        {
            ReadWorkScope workScope(pending);
            read.result = ReadPlaybackFileBytes(file, destination, bytes, &count, timing);
        }

        if (read.result != FR_OK || count != bytes) {
            return read;
        }
        // Some reads may be satisfied by the filesystem cache, or have fewer
        // DMA starts than needed. Finish the remainder before reusing a buffer.
        pending.ProcessPendingFramePixels(pending.count - pending.done);
        if (!pending.accepted) {
            read.error = "media frame consumer rejected data";
            return read;
        }
        pending.bytes = destination;
        pending.count = bytes;
        pending.done = 0;
        pending.offset = offset;
        nextBuffer ^= 1;
        offset += bytes;
    }

    if (read.result != FR_OK) {
        return read;
    }
    pending.ProcessPendingFramePixels(pending.count - pending.done);
    if (!pending.accepted) {
        read.error = "media frame consumer rejected data";
        return read;
    }
    expected = slot.checksum;
    if (slot.frameChecksums) {
        read.result = ReadPlaybackFileBytes(file, checksum, FRAME_CHECKSUM_BYTES, &count, timing);
        if (read.result != FR_OK || count != FRAME_CHECKSUM_BYTES) {
            return read;
        }
        expected = ReadUnsigned32LittleEndian(checksum);
    }
    read.error = "media frame checksum mismatch";
    read.complete = ~pending.crc == expected;
    if (timing) {
        timing->totalUs = nowUs() - started;
    }
    return read;
} // ReadAndVerifyRawVideoFrame

/**
    Refills the bounded encoded JPEG input buffer from storage.

    Reads the next bounded portion of the current JPEG record through the playback read path and
    updates its encoded-frame CRC. An optional whole-payload accumulator receives the same bytes
    during save verification. It also retains the last marker word across refills for final end-
    of-image validation; short or failed reads never become a valid input block.

    @return true - More encoded JPEG bytes were loaded into the input buffer.
    @return false - Input is exhausted or the file read failed.
*/
bool
JpegStream::RefillJpegInputBuffer (
    void
    )
{
    UINT bytes;
    UINT count;
    uint64_t started;
    /**
        Returns elapsed boot time in microseconds for read profiling.

        @return value - Monotonic time since boot in microseconds.
    */
    auto nowUs = [] { return to_us_since_boot(get_absolute_time()); };

    if (!remaining) {
        return false;
    }
    bytes = UINT(std::min(remaining, BufferBytes));
    count = 0;

    if (timing) {
        started = nowUs();
    } else {
        started = 0;
    }
    result.result = ReadPlaybackFileBytes(file, readBuffer, bytes, &count, timing);

    if (result.result != FR_OK || count != bytes) {
        return false;
    }
    if (timing) {
        started = nowUs();
    } else {
        started = 0;
    }
    crc = UpdateImageCrc32(crc, readBuffer, count);

    // Include encoded record bytes in whole-payload verification as well as the frame checksum.
    if (payloadCrc) {
        *payloadCrc = UpdateImageCrc32(*payloadCrc, readBuffer, count);
    }
    if (timing) {
        timing->crcUs += nowUs() - started;
    }
    if (count >= JPEG_MARKER_BYTES) {
        lastWord = uint16_t((uint16_t(readBuffer[count - JPEG_MARKER_BYTES]) << BITS_PER_BYTE) |
                            readBuffer[count - 1]);
    } else {
        lastWord = uint16_t((lastWord << BITS_PER_BYTE) | readBuffer[0]);
    }
    remaining -= count;
    buffered = count;
    used = 0;
    return true;
} // RefillJpegInputBuffer

/**
    Supplies encoded JPEG bytes and updates frame verification state.

    Supplies the decoder from the current input block, refilling synchronously from SD when
    exhausted. A null output pointer skips bytes while still reading and checksumming them through
    the refill path. The number returned can be short when the record ends or storage fails; the
    enclosing JPEG validation reports which condition prevented completion.

    @param context Caller-owned state passed unchanged to the callback.
    @param bytes Writable byte buffer filled by this operation.
    @param length Number of bytes to process.

    @return count - Number of encoded bytes supplied or skipped, no more than requested.
    @return 0 - No input bytes could be supplied.
*/
size_t
JpegStream::ReadEncodedJpegInput (
    void* context,
    uint8_t* bytes,
    size_t length
    )
{
    size_t done;
    size_t part;

    JpegStream* stream = &(*static_cast<JpegStream*>(context));
    done = 0;
    while (done < length) {
        if (stream->used == stream->buffered && !stream->RefillJpegInputBuffer()) {
            break;
        }
        part = std::min(length - done, stream->buffered - stream->used);
        if (bytes) {
            std::memcpy(bytes + done, readBuffer + stream->used, part);
        }
        stream->used += part;
        done += part;
    }

    return done;
} // ReadEncodedJpegInput

/**
    Forwards decoded JPEG fragments to the frame's pixel sink.

    Forwards each decoded fragment immediately to the caller's borrowed sink and records the
    sink's acceptance and optional execution time. With no sink, output is accepted without
    framebuffer work for validation-only reads. These callbacks do not publish an image;
    successful final record validation remains required.

    @param context Caller-owned state passed unchanged to the callback.
    @param bytes Read-only input byte buffer.
    @param count Number of bytes to process.
    @param offset Byte offset within the source image or file.

    @return true - The decoded output was accepted, or no output sink was requested.
    @return false - The pixel sink rejected the decoded output.
*/
bool
JpegStream::WriteDecodedJpegPixels (
    void* context,
    const uint8_t* bytes,
    size_t count,
    size_t offset
    )
{
    uint64_t started;

    JpegStream* stream = &(*static_cast<JpegStream*>(context));
    if (!stream->sink) {
        return true;
    }
    if (stream->timing) {
        started = to_us_since_boot(get_absolute_time());
    } else {
        started = 0;
    }
    stream->accepted = stream->sink(stream->context, bytes, count, offset);
    if (stream->timing) {
        stream->timing->consumerUs += to_us_since_boot(get_absolute_time()) - started;
    }
    return stream->accepted;
} // WriteDecodedJpegPixels

#if JPEG_PARALLEL_READER
/**
    Supplies encoded input to the worker through the JPEG input pipe.

    @param state Opaque callback state pointing to the current decoder context.
    @param destination Writable destination buffer; the caller must provide sufficient storage.
    @param bytes Number of bytes to process.

    @return count - Number of encoded bytes supplied or skipped, no more than requested.
    @return 0 - No input bytes could be supplied.
*/
size_t
ParallelDecode::ReadEncodedJpegInput (
    void* state,
    uint8_t* destination,
    size_t bytes
    )
{
    uint64_t started;
    size_t count;

    ParallelDecode* job = &(*static_cast<ParallelDecode*>(state));
    started = to_us_since_boot(get_absolute_time());
    count = jpegPipe.Read(destination, bytes);
    job->waitUs += to_us_since_boot(get_absolute_time()) - started;
    return count;
} // ReadEncodedJpegInput

/**
    Decodes a JPEG frame on the reserved worker and records its result.

    @param state Opaque callback state pointing to the current decoder context.
*/
void
ParallelDecode::DecodeJpegOnWorker (
    void* state
    )
{
    uint64_t started;

    ParallelDecode* job = &(*static_cast<ParallelDecode*>(state));
    started = to_us_since_boot(get_absolute_time());
    job->decoded = JpegFrameDecoder::Decode(ReadEncodedJpegInput, &(*job), JpegStream::WriteDecodedJpegPixels,
                                           &job->stream, job->width, job->height,
                                           readBuffer + JpegStream::BufferBytes, JpegStream::BufferBytes,
                                           job->error, job->stages, job->clock, nullptr, job->rgb565);
    job->elapsedUs = to_us_since_boot(get_absolute_time()) - started;
} // DecodeJpegOnWorker

/**
    Overlaps SD input reads with JPEG decoding on an available Core 1.

    Uses the caller's existing Core 1 reservation to prime the input pipe and start a borrowed
    decode job. Core 0 fills additional encoded blocks and checksums their bytes while Core 1
    consumes them. When the pipe is full, Core 0 yields; after input is buffered it may
    synchronously prefetch the next playback block.

    On read failure it signals end-of-input and waits for the worker to finish before returning,
    so no callback can retain the stack context or shared workspace after cleanup. Remaining
    encoded tail bytes are still read and checksummed even if the decoder reaches an end marker
    early. The local fallback is selected by the caller before this function when a reservation
    cannot be obtained.

    @param stream JPEG input/output state shared with the streaming callbacks.
    @param width Image width in pixels.
    @param height Image height in pixels.
    @param error Output JPEG decoder result code; JDR_OK indicates success.
    @param stages Optional output counters for individual JPEG decoder stages.
    @param clock Optional monotonic profiling clock returning microseconds.
    @param rgb565 Whether decoded output uses RGB565 rather than RGB888 pixels.
    @param decodeUs Output time spent decoding, in microseconds.
    @param waitUs Output time spent waiting for encoded input, in microseconds.
    @param pipelineBytes Output number of encoded bytes handled through the parallel input pipe.

    @return true - The reserved worker decoded the frame and all remaining encoded bytes were read.
    @return false - Input preparation, worker startup, decoding or the remaining read failed.
*/
bool
DecodeJpegParallel (
    JpegStream& stream,
    uint16_t width,
    uint16_t height,
    int& error,
    JpegFrameDecoder::Profile* stages,
    JpegFrameDecoder::Clock clock,
    bool rgb565,
    uint64_t& decodeUs,
    uint64_t& waitUs,
    uint32_t& pipelineBytes
    )
{
    uint8_t* first = nullptr;
    size_t initial;
    bool good;
    bool prefetched;
    uint8_t* target = nullptr;
    size_t bytes;
    uint64_t consumerWorkUs;
    uint64_t measuredConsumerUs;
    ParallelDecode job{stream, width, height, error, stages, clock, rgb565};
    /**
        Reads the next encoded JPEG input chunk into the pipeline buffer.

        Reads an exact bounded part of the encoded record into the chosen pipe slot, then updates
        frame and optional whole-payload checksums before publishing input. It retains the
        trailing marker word and remaining byte count for final validation. The producer calls it
        synchronously on Core 0; Core 1 sees a slot only after the outer pipeline marks the
        completed block ready.

        @param destination Writable destination buffer; the caller must provide sufficient storage.
        @param limit Maximum number of bytes to load into the stream block.

        @return true - The next requested stream block was filled.
        @return false - The stream block could not be completely read.
    */
    auto fill = [&](uint8_t* destination, size_t limit) {
        UINT requested;
        UINT count;
        uint64_t crcStarted;

        requested = UINT(std::min(stream.remaining, limit));
        count = 0;
        stream.result.result =
            ReadPlaybackFileBytes(stream.file, destination, requested, &count, stream.timing);
        if (stream.result.result != FR_OK || count != requested) {
            return false;
        }
        crcStarted = to_us_since_boot(get_absolute_time());
        stream.crc = UpdateImageCrc32(stream.crc, destination, count);
        if (stream.payloadCrc) {
            *stream.payloadCrc = UpdateImageCrc32(*stream.payloadCrc, destination, count);
        }
        if (stream.timing) {
            stream.timing->crcUs += to_us_since_boot(get_absolute_time()) - crcStarted;
        }
        if (count >= JPEG_MARKER_BYTES) {
            stream.lastWord = uint16_t((uint16_t(destination[count - JPEG_MARKER_BYTES]) << BITS_PER_BYTE) |
                                       destination[count - 1]);
        } else if (count) {
            stream.lastWord = uint16_t((stream.lastWord << BITS_PER_BYTE) | destination[0]);
        }
        stream.remaining -= count;
        return true;
    };

    pipelineBytes = 0;
    jpegPipe.Reset();

    // A short first read starts decoding promptly; subsequent reads fill 8 KiB
    // slots while the decoder works on already published data.
    first = jpegPipe.Writable();
    initial = std::min(stream.remaining, size_t(JPEG_PIPELINE_PRIME_BYTES));
    if (!fill(first, initial)) {
        CancelJpegDecodeWorkerReservation();
        return false;
    }
    jpegPipe.Publish(initial);
    if (!stream.remaining) {
        jpegPipe.Finish();
    }

    // Reserve Core 1 before starting the producer so neither core waits on an unavailable decode worker.
    if (!StartJpegDecodeWorker(ParallelDecode::DecodeJpegOnWorker, &job)) {
        CancelJpegDecodeWorkerReservation();
        return false;
    }
    good = true;
    prefetched = false;
    while (!JpegDecodeWorkerFinished()) {
        if (stream.remaining && good) {
            if ((target = jpegPipe.Writable())) {
                bytes = std::min(stream.remaining, JpegInputPipe::SlotBytes);
                if (!fill(target, bytes)) {
                    good = false;
                    jpegPipe.Finish();
                } else {
                    pipelineBytes += uint32_t(bytes);
                    jpegPipe.Publish(bytes);
                    if (!stream.remaining) {
                        jpegPipe.Finish();
                    }
                }
                continue;
            }
        }
        // Once current JPEG input is safely copied into the pipe, use the spare
        // Core 0 time to fetch the next file block while Core 1 still decodes.
        if (!stream.remaining && good && !prefetched) {
            prefetched = true;
            PrefetchNextPlaybackBlock(stream.file, stream.timing);
        }
        JpegPipelineYield();
    }
    // Completion is an acquire barrier: callbacks/workspace are quiescent before
    // return, remount, raw playback, upload verification or buffer reuse.
    consumerWorkUs = {};
    if (stream.timing) {
        consumerWorkUs = stream.timing->consumerUs;
    } else {
        consumerWorkUs = 0;
    }
    if (job.elapsedUs > job.waitUs + consumerWorkUs) {
        measuredConsumerUs = {};
        if (stream.timing) {
            measuredConsumerUs = stream.timing->consumerUs;
        } else {
            measuredConsumerUs = 0;
        }
        decodeUs = job.elapsedUs - job.waitUs - measuredConsumerUs;
    } else {
        decodeUs = 0;
    }
    waitUs = job.waitUs;
    if (!good || !job.decoded) {
        return false;
    }
    // An EOI can end decoding before the producer reaches the record's tail.
    // Read and CRC every remaining byte before accepting a frame.
    while (stream.remaining) {
        if (!fill(readBuffer, JpegStream::BufferBytes)) {
            return false;
        }
    }

    return true;
} // DecodeJpegParallel

#endif

/**
    Decodes and checks a bounded JPEG record already positioned by the storage manager.

    Reserves idle Core 1 before consuming input, then feeds its bounded pipe from Core 0.
    A busy or unlaunched worker selects the synchronous decoder immediately. All producer
    reads stay on Core 0, and a failed read signals input completion before waiting for
    the worker to release its borrowed context. Pixel output remains provisional until
    encoded length, CRC and the final JPEG end marker pass; no complete RGB888 frame is
    allocated.

    @param file Open encoded record positioned just after its checked record header.
    @param metadata Expected decoded image dimensions.
    @param bytes Exact encoded record length.
    @param checksum Expected encoded-frame CRC32.
    @param sink Consumer of provisional decoded pixels, or null for validation.
    @param context Borrowed pixel-consumer context.
    @param timing Optional output counters for this record.
    @param outputRGB565 True selects little-endian RGB565 output; false selects RGB888.
    @param payloadCrc Optional running payload CRC receiving every encoded frame byte.

    @return result - Completion flag, filesystem result and failure description.
*/
FrameResult
DecodeEncodedJpegFrame (
    FIL& file,
    const MediaMetadata& metadata,
    size_t bytes,
    uint32_t checksum,
    MemoryManager::ImageSink sink,
    void* context,
    MemoryManager::FrameReadTiming* timing,
    bool outputRGB565,
    uint32_t* payloadCrc
    )
{
    FrameResult result;
    int decoderError;
    bool decoded;
    bool usedCore1;
    uint64_t started;
    uint64_t decodeStarted;
    uint64_t decodeUs;
    uint64_t waitUs;
    uint32_t pipelineBytes;
    decltype(&JpegStream::WriteDecodedJpegPixels) outputCallback;
    JpegStream stream{file, result, sink, context, timing, bytes};

    stream.payloadCrc = payloadCrc;
    result = {};
    decoderError = 0;
    usedCore1 = false;
    decodeUs = 0;
    waitUs = 0;
    pipelineBytes = 0;
    started = to_us_since_boot(get_absolute_time());
    DriverReadProfileScope profile(timing);

    // Reservation precedes the first pipe read, avoiding circular waits with networking.
    #if JPEG_PARALLEL_READER
    if (sink && ReserveJpegDecodeWorker()) {
        usedCore1 = true;
        decoded = DecodeJpegParallel(stream, metadata.width, metadata.height, decoderError,
                                     nullptr, nullptr, outputRGB565, decodeUs, waitUs, pipelineBytes);
    } else
    #endif
    {
        outputCallback = nullptr;
        if (sink) {
            outputCallback = JpegStream::WriteDecodedJpegPixels;
        }
        decodeStarted = to_us_since_boot(get_absolute_time());
        decoded = JpegFrameDecoder::Decode(JpegStream::ReadEncodedJpegInput, &stream,
                                          outputCallback, &stream, metadata.width, metadata.height,
                                          readBuffer + JpegStream::BufferBytes, JpegStream::BufferBytes,
                                          decoderError, nullptr, nullptr, nullptr, outputRGB565);
        decodeUs = to_us_since_boot(get_absolute_time()) - decodeStarted;
    }

    // Drain trailing record bytes even if the JPEG decoder stopped at an earlier marker.
    if (decoded) {
        while (stream.remaining) {
            if (!stream.RefillJpegInputBuffer()) {
                return result;
            }
        }
    }
    result.error = "invalid/unsupported JPEG frame";
    if (!stream.accepted) {
        result.error = "JPEG pixel consumer rejected data";
    }
    if (decoded && stream.accepted) {
        result.error = "JPEG frame checksum or end marker mismatch";
        result.complete = result.result == FR_OK && !stream.remaining &&
                          ~stream.crc == checksum && stream.lastWord == JPEG_END_OF_IMAGE_MARKER;
    }
    if (timing) {
        timing->totalUs = to_us_since_boot(get_absolute_time()) - started;
        timing->decodeUs = decodeUs;
        timing->encodedBytes = bytes;
        timing->jpegInputWaitUs = waitUs;
        timing->overlapBytes = pipelineBytes;
        if (usedCore1) {
            timing->jpegCore1Frames = 1;
        } else {
            timing->jpegCore0Frames = 1;
        }
    }
    return result;
} // DecodeEncodedJpegFrame

/**
    Attempts to claim the shared storage state without waiting.

    Reentrant callbacks or concurrent operations receive a busy result rather
    than waiting on the same filesystem handle and scratch buffer.
*/
StorageGuard::StorageGuard (
    void
    ) : acquired(!managerState.storageBusy.test_and_set(std::memory_order_acquire))
{
    return;
} // StorageGuard

/**
    Releases the storage-operation guard when its scope ends.
*/
StorageGuard::~StorageGuard (
    void
    )
{
    if (acquired) {
        managerState.storageBusy.clear(std::memory_order_release);
    }
} // ~StorageGuard

/**
    Validates a root filename and builds its SD filesystem path.

    Checks a single bounded root filename and optional extension before adding the drive prefix.
    Directory separators, leading dots and unsupported characters are rejected without touching
    the card. Callers provide the fixed output capacity used by the manager.

    @param name Filename in the card root; directory separators and invalid names are rejected.
    @param path Storage path used by this operation.

    @return true - The filename is valid and the storage path was built.
    @return false - The name is empty, too long or contains disallowed path characters.
*/
bool
BuildValidatedImagePath (
    const char* name,
    char* path
    )
{
    size_t length;
    bool dot;
    char c;

    if (!name || !name[0] || name[0] == '.') {
        return false;
    }
    length = 0;
    dot = false;

    // Allow only one root filename and extension; reject separators and traversal syntax.
    for (; length <= MaximumNameLength && name[length]; ++length) {
        c = name[length];
        if (c == '.') {
            if (dot) {
                return false;
            }
            dot = true;
        } else if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
                   c != '_' && c != '-') {
            return false;
        }
    }

    if (length > MaximumNameLength || name[length - 1] == '.') {
        return false;
    }
    std::snprintf(path, MaximumNameLength + STORAGE_PATH_PREFIX_AND_TERMINATOR_BYTES, "0:/%s", name);
    return true;
} // BuildValidatedImagePath

/**
    Updates the running CRC32 checksum with another byte span.

    Uses flash-resident slicing-by-four tables while retaining the existing
    reflected IEEE CRC32 state, polynomial and final inversion. The same state
    continues across arbitrary fragment boundaries, preserving every stored CRC.

    @param crc Running CRC32 state before processing these bytes.
    @param bytes Read-only input byte buffer.
    @param length Number of bytes to process.

    @return value - Updated running CRC32 state; apply the protocol final inversion when
                    completing the checksum.
*/
uint32_t
UpdateImageCrc32 (
    uint32_t crc,
    const uint8_t* bytes,
    size_t length
    )
{
    return StorageCrc32::UpdateCrc32Checksum(crc, bytes, length);
} // UpdateImageCrc32

/**
    Writes a 32-bit unsigned value in little-endian byte order.

    @param p Writable destination buffer; the caller must provide sufficient storage.
    @param value Unsigned integer value to encode.
*/
void
WriteUnsigned32LittleEndian (
    uint8_t* p,
    uint32_t value
    )
{
    int i;

    for (i = 0; i < UINT32_BYTES; ++i) {
        p[i] = uint8_t(value >> (BITS_PER_BYTE * i));
    }
} // WriteUnsigned32LittleEndian

/**
    Reads a 32-bit unsigned value in little-endian byte order.

    @param p Read-only source bytes in the format required by this operation.

    @return value - Unsigned integer decoded from the source bytes in little-endian order.
*/
uint32_t
ReadUnsigned32LittleEndian (
    const uint8_t* p
    )
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << BITS_PER_BYTE) |
           (uint32_t(p[UINT32_BYTE_TWO_INDEX]) << WORD_BYTE_TWO_SHIFT) |
           (uint32_t(p[UINT32_BYTE_THREE_INDEX]) << WORD_BYTE_THREE_SHIFT);
} // ReadUnsigned32LittleEndian

/**
    Builds a photo storage header with dimensions and payload checksum.

    Encodes the fixed RGB888 photo format, logical panel dimensions and payload size in the
    defined byte order. The supplied checksum covers the complete image payload. Constructing
    this header performs no filesystem operation and does not imply the file has been saved.

    @param header Buffer holding the storage or HTTP header.
    @param checksum Expected CRC32 checksum of the media payload.
*/
void
BuildPhotoStorageHeader (
    uint8_t* header,
    uint32_t checksum
    )
{
    // These field offsets and little-endian values define the persistent binary format.
    std::memcpy(header, "H75I", MEDIA_MAGIC_BYTES);
    header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_PHOTO; // File format version
    header[PHOTO_HEADER_PIXEL_BYTES_OFFSET] = IMAGE_BYTES_PER_PIXEL;
    header[PHOTO_HEADER_WIDTH_LOW_OFFSET] = uint8_t(LOGICAL_IMAGE_WIDTH);
    header[PHOTO_HEADER_WIDTH_HIGH_OFFSET] = uint8_t(LOGICAL_IMAGE_WIDTH >> BITS_PER_BYTE);
    header[PHOTO_HEADER_HEIGHT_LOW_OFFSET] = uint8_t(LOGICAL_IMAGE_HEIGHT);
    header[PHOTO_HEADER_HEIGHT_HIGH_OFFSET] = uint8_t(LOGICAL_IMAGE_HEIGHT >> BITS_PER_BYTE);
    header[PHOTO_HEADER_RESERVED_LOW_OFFSET] = header[PHOTO_HEADER_RESERVED_HIGH_OFFSET] = 0;
    WriteUnsigned32LittleEndian(header + PHOTO_HEADER_PAYLOAD_SIZE_OFFSET, SOURCE_FRAME_SIZE_BYTES);
    WriteUnsigned32LittleEndian(header + PHOTO_HEADER_CHECKSUM_OFFSET, checksum);
} // BuildPhotoStorageHeader

/**
    Compares readback pixels against the source used by the storage self-check.

    Requires contiguous chunk offsets and compares each readback fragment against the original
    caller-owned image. A differing byte or unexpected offset records a mismatch and stops the
    enclosing read. The source image must remain stable until the complete self-check returns.

    @param context Caller-owned state passed unchanged to the callback.
    @param bytes Read-only input byte buffer.
    @param count Number of bytes to process.
    @param offset Byte offset within the source image or file.

    @return true - The saved bytes match the original image in this chunk.
    @return false - At least one saved byte differs from the original image.
*/
bool
CompareSavedImageBytes (
    void* context,
    const uint8_t* bytes,
    size_t count,
    size_t offset
    )
{
    CompareContext* check = nullptr;

    check = &(*static_cast<CompareContext*>(context));
    if (offset != check->compared || std::memcmp(check->expected + offset, bytes, count)) {
        check->mismatch = true;
        return false;
    }
    check->compared += count;
    return true;
} // CompareSavedImageBytes

/**
    Builds a saved-photo header with its generation and checksum.

    Encodes the photo's generation, media descriptor and expected payload CRC into the fixed
    saved-photo header, then checksums that header separately. The caller has already validated
    metadata and supplies enough output storage. Building the header performs no I/O and does not
    make an in-progress file valid by itself.

    @param header Buffer holding the storage or HTTP header.
    @param metadata Media dimensions, pixel format, placement and playback settings.
    @param generation Sequence number used to identify the newest valid stored copy.
    @param checksum Expected CRC32 checksum of the media payload.
*/
void
BuildSavedPhotoHeader (
    uint8_t* header,
    const MediaMetadata& metadata,
    uint32_t generation,
    uint32_t checksum
    )
{
    std::memset(header, 0, LastHeaderBytes);
    std::memcpy(header, "H75L", MEDIA_MAGIC_BYTES);
    header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_SAVED_PHOTO;
    MediaMetadataCodec::WriteUnsigned16LittleEndian(header + STORAGE_HEADER_LENGTH_OFFSET, LastHeaderBytes);
    WriteUnsigned32LittleEndian(header + STORAGE_HEADER_GENERATION_OFFSET, generation);
    MediaMetadataCodec::EncodeMediaDescriptor(metadata, header + STORAGE_HEADER_METADATA_OFFSET,
                                              MediaMetadataCodec::HeaderBytes);
    WriteUnsigned32LittleEndian(header + SAVED_PHOTO_PAYLOAD_CRC_OFFSET, checksum);
    WriteUnsigned32LittleEndian(
        header + SAVED_PHOTO_HEADER_CRC_OFFSET,
        ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, SAVED_PHOTO_HEADER_CRC_OFFSET));
} // BuildSavedPhotoHeader

/**
    Validates a stored photo while streaming provisional pixel data.

    Recognizes supported current and legacy photo headers, checks the declared file size and
    reconstructs compatible metadata where older formats lack a descriptor. It then reads the
    complete RGB888 payload in bounded chunks while accumulating the correct version-specific
    checksum.

    Sink calls run synchronously as chunks arrive, so their pixels remain provisional until the
    final checksum and file close both succeed. The result distinguishes missing, invalid and
    I/O-failed files, allowing callers to fall back on damaged content without hiding an actual
    card communication failure.

    @param path Storage path used by this operation.
    @param sink Callback that consumes pixel chunks; false cancels the current frame.
    @param context Caller-owned state passed unchanged to the callback.

    @return result - Inspection/read result including validity, metadata and any I/O or
                     validation error.
*/
SavedPhotoSlot
ReadAndValidatePhotoFile (
    const char* path,
    MemoryManager::ImageSink sink,
    void* context
    )
{
    SavedPhotoSlot slot;
    uint8_t header[LastHeaderBytes];
    uint8_t expected[LegacyLastHeaderBytes];
    UINT count;
    size_t headerBytes;
    bool complete;
    uint32_t expectedCrc;
    uint32_t crc;
    uint32_t checksumOffset;
    size_t offset;
    UINT bytes;
    FRESULT closeResult;

    slot = {};

    // Open the saved file before validating its header and reading pixels.
    slot.result = f_open(&managerState.imageFile, path, FA_READ);
    slot.error = "image open failed";
    if (slot.result == FR_NO_FILE) {
        return slot;
    }
    if (slot.result != FR_OK) {
        slot.state = SlotState::IoError;
        return slot;
    }
    slot.state = SlotState::Invalid;

    // Validate the file header and declared size before accepting pixel data.
    std::memset(header, 0, sizeof(header));
    std::memset(expected, 0, sizeof(expected));
    count = 0;
    headerBytes = 0;
    slot.result = f_read(&managerState.imageFile, header, STORAGE_HEADER_PREFIX_BYTES, &count);
    complete = slot.result == FR_OK && count == STORAGE_HEADER_PREFIX_BYTES;
    slot.error = "image header read failed";
    if (complete) {
        if (!std::memcmp(header, "H75I", MEDIA_MAGIC_BYTES) &&
            header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_PHOTO) {
            headerBytes = HeaderBytes;
        } else if (!std::memcmp(header, "H75L", MEDIA_MAGIC_BYTES) &&
                   header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_LEGACY_SAVED_PHOTO) {
            headerBytes = LegacyLastHeaderBytes;
        } else if (!std::memcmp(header, "H75L", MEDIA_MAGIC_BYTES) &&
                   header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_SAVED_PHOTO) {
            headerBytes = LastHeaderBytes;
        }
        slot.error = "invalid stored image header";
        complete = headerBytes != 0;
    }
    if (complete) {
        slot.error = "invalid stored image size";
        complete = f_size(&managerState.imageFile) == SOURCE_FRAME_SIZE_BYTES + headerBytes;
    }
    if (complete) {
        slot.result = f_read(&managerState.imageFile, header + STORAGE_HEADER_PREFIX_BYTES,
                             UINT(headerBytes - STORAGE_HEADER_PREFIX_BYTES), &count);
        slot.error = "image header read failed";
        complete = slot.result == FR_OK && count == headerBytes - STORAGE_HEADER_PREFIX_BYTES;
    }
    expectedCrc = 0;
    crc = STORAGE_CRC32_INITIAL_U;
    if (complete) {
        slot.error = "invalid stored image header";
        if (header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_SAVED_PHOTO) {
            slot.generation = ReadUnsigned32LittleEndian(header + STORAGE_HEADER_GENERATION_OFFSET);
            expectedCrc = ReadUnsigned32LittleEndian(header + SAVED_PHOTO_PAYLOAD_CRC_OFFSET);
            complete =
                header[STORAGE_HEADER_RESERVED_OFFSET] == 0 &&
                MediaMetadataCodec::ReadUnsigned16LittleEndian(header + STORAGE_HEADER_LENGTH_OFFSET) ==
                    LastHeaderBytes &&
                ReadUnsigned32LittleEndian(header + SAVED_PHOTO_HEADER_CRC_OFFSET) ==
                    ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, SAVED_PHOTO_HEADER_CRC_OFFSET) &&
                MediaMetadataCodec::DecodeMediaDescriptor(header + STORAGE_HEADER_METADATA_OFFSET,
                                                          MediaMetadataCodec::HeaderBytes, slot.metadata) &&
                slot.metadata.pixelFormat == PixelFormat::RGB888 &&
                slot.metadata.IsPhotoFor(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT);
        } else {
            // Legacy files had no media type/FPS/format. They were
            // single RGB888 panel photos, so synthesize that descriptor.
            slot.metadata = MediaMetadata::Photo(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT);
            BuildPhotoStorageHeader(expected,
                                    ReadUnsigned32LittleEndian(header + PHOTO_HEADER_CHECKSUM_OFFSET));
            if (header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_LEGACY_SAVED_PHOTO) {
                std::memcpy(expected, "H75L", MEDIA_MAGIC_BYTES);
                expected[MEDIA_MAGIC_BYTES] = STORAGE_VERSION_LEGACY_SAVED_PHOTO;
                slot.generation = ReadUnsigned32LittleEndian(header + PHOTO_LEGACY_GENERATION_OFFSET);
                WriteUnsigned32LittleEndian(expected + PHOTO_LEGACY_GENERATION_OFFSET, slot.generation);
                WriteUnsigned32LittleEndian(
                    expected + PHOTO_LEGACY_CHECKSUM_OFFSET,
                    ReadUnsigned32LittleEndian(header + PHOTO_LEGACY_CHECKSUM_OFFSET));
                crc = UpdateImageCrc32(crc, header, PHOTO_LEGACY_CHECKSUM_OFFSET);
            }
            complete = !std::memcmp(header, expected, headerBytes);
            checksumOffset = {};
            if (header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_PHOTO) {
                checksumOffset = PHOTO_HEADER_CHECKSUM_OFFSET;
            } else {
                checksumOffset = PHOTO_LEGACY_CHECKSUM_OFFSET;
            }
            expectedCrc = ReadUnsigned32LittleEndian(header + checksumOffset);
        }
    }

    slot.dataOffset = headerBytes;
    slot.checksum = expectedCrc;
    slot.crcSeed = crc;

    // Read bounded chunks and verify their checksum; delivered pixels remain provisional.
    for (offset = 0; complete && offset < SOURCE_FRAME_SIZE_BYTES;) {
        bytes = UINT(std::min(ChunkBytes, SOURCE_FRAME_SIZE_BYTES - offset));
        slot.result = f_read(&managerState.imageFile, managerState.readBuffer, bytes, &count);
        slot.error = "image data read failed";
        complete = slot.result == FR_OK && count == bytes;
        if (complete) {
            crc = UpdateImageCrc32(crc, managerState.readBuffer, bytes);
            if (sink && !sink(context, managerState.readBuffer, bytes, offset)) {
                slot.error = "image consumer rejected data";
                complete = false;
            }
        }
        offset += count;
    }

    // Accept the payload only when its complete CRC matches the stored checksum.
    if (complete && ~crc != expectedCrc) {
        complete = false;
        slot.error = "stored image checksum mismatch";
    }

    // Close the file and preserve any transfer or close error before reporting success.
    closeResult = f_close(&managerState.imageFile);

    if (slot.result != FR_OK || closeResult != FR_OK) {
        if (slot.result == FR_OK) {
            slot.result = closeResult;
            slot.error = "image close failed";
        }
        slot.state = SlotState::IoError;
    } else if (complete) {
        slot.state = SlotState::Valid;
    }
    return slot;
} // ReadAndValidatePhotoFile

/**
    Reads and validates one retained photo, optionally delivering its pixels.

    Both retained slots use the same version-aware reader as named images.
    Legacy files gain their known RGB888 photo descriptor without being rewritten.

    @param index Retained slot index, zero or one.
    @param sink Optional synchronous consumer; null performs validation only.
    @param context Borrowed consumer state valid until the read returns.

    @return slot - Validity, metadata, generation and any filesystem/error result.
*/
SavedPhotoSlot
ReadSavedPhotoSlot (
    unsigned index,
    MemoryManager::ImageSink sink,
    void* context
    )
{
    const char* path = nullptr;

    if (index == 0) {
        path = "0:/LASTA.H75";
    } else {
        path = "0:/LASTB.H75";
    }
    return ReadAndValidatePhotoFile(path, sink, context);
} // ReadSavedPhotoSlot

/**
    Selects the newest valid generation with wrap-safe ordering.

    Invalid slots are ignored. When both copies validate, signed generation
    distance chooses the newer copy even across the unsigned counter wrap.

    @param slots Two previously inspected retained photo descriptions.

    @return 0 - The first slot is the newest valid copy.
    @return 1 - The second slot is the newest valid copy.
    @return SAVED_PHOTO_NO_SLOT - Neither slot contains a valid photo.
*/
int
FindNewestSavedPhoto (
    const SavedPhotoSlot* slots
    )
{
    if (slots[0].state != SlotState::Valid) {
        if (slots[1].state == SlotState::Valid) {
            return 1;
        }
        return SAVED_PHOTO_NO_SLOT;
    }
    if (slots[1].state == SlotState::Valid && int32_t(slots[1].generation - slots[0].generation) > 0) {
        return 1;
    }
    return 0;
} // FindNewestSavedPhoto

/**
    Streams a new retained photo into the selected alternate slot.

    The source is read in bounded chunks while its payload CRC accumulates.
    Metadata and generation have a separate checksum in the final header. Only after the payload is written is its final header installed
    and the file closed. A failed write leaves the other retained generation intact.
    Actual card I/O failure avoids additional cleanup writes to that session.

    @param index Destination slot index, zero or one.
    @param generation New generation number.
    @param metadata Validated exact-panel RGB888 photo description.
    @param source Synchronous stable RGB888 pixel producer.
    @param context Borrowed source state for this save.
    @param result Output filesystem status for failure reporting.

    @return true - Header and payload were written and closed successfully.
    @return false - The source, write, seek or close failed.
*/
bool
WriteSavedPhotoSlot (
    unsigned index,
    uint32_t generation,
    const MediaMetadata& metadata,
    MemoryManager::ImageSource source,
    void* context,
    int& result
    )
{
    const char* path = nullptr;
    uint8_t header[LastHeaderBytes];
    uint32_t crc;
    UINT count;
    FRESULT status;
    FRESULT closed;
    bool good;
    size_t offset;
    UINT bytes;

    // Open the alternate retained slot; the preceding valid generation remains untouched.
    if (index == 0) {
        path = "0:/LASTA.H75";
    } else {
        path = "0:/LASTB.H75";
    }
    status = f_open(&managerState.imageFile, path, FA_WRITE | FA_CREATE_ALWAYS);
    result = status;
    if (status != FR_OK) {
        return false;
    }

    // Reserve the header and stream stable source chunks while accumulating the payload CRC.
    BuildSavedPhotoHeader(header, metadata, generation, 0);
    crc = STORAGE_CRC32_INITIAL_U;
    count = 0;
    status = f_write(&managerState.imageFile, header, sizeof(header), &count);
    good = status == FR_OK && count == sizeof(header);
    for (offset = 0; good && offset < SOURCE_FRAME_SIZE_BYTES;) {
        bytes = UINT(std::min(ChunkBytes, SOURCE_FRAME_SIZE_BYTES - offset));
        good = source(context, managerState.readBuffer, bytes, offset);
        if (!good) {
            break;
        }
        crc = UpdateImageCrc32(crc, managerState.readBuffer, bytes);
        status = f_write(&managerState.imageFile, managerState.readBuffer, bytes, &count);
        good = status == FR_OK && count == bytes;
        offset += count;
    }

    // Install the final payload checksum only after every source chunk was written.
    if (good) {
        BuildSavedPhotoHeader(header, metadata, generation, ~crc);
        status = f_lseek(&managerState.imageFile, 0);
        if (status == FR_OK) {
            status = f_write(&managerState.imageFile, header, sizeof(header), &count);
        }
        good = status == FR_OK && count == sizeof(header);
    }

    // Avoid additional filesystem cleanup writes after communication with the card has failed.
    if (status == FR_DISK_ERR || status == FR_NOT_READY) {
        managerState.imageFile = {};
        result = status;
        return false;
    }

    // Close to flush the completed file; remove an incomplete target only while storage is usable.
    closed = f_close(&managerState.imageFile);
    if (status == FR_OK) {
        status = closed;
    }
    result = status;
    if (!good || status != FR_OK) {
        if (status != FR_DISK_ERR && status != FR_NOT_READY) {
            f_unlink(path);
        }
        return false;
    }
    return true;
} // WriteSavedPhotoSlot

/**
    Returns the reserved path for one alternating media slot.

    @param index Alternating slot index, zero or one.

    @return path - Borrowed constant path; callers never free it.
*/
const char*
GetSavedMediaPath (
    unsigned index
    )
{
    if (index == 0) {
        return "0:/LASTA.H75";
    }
    return "0:/LASTB.H75";
} // GetSavedMediaPath

/**
    Flushes buffered transaction bytes to the active save file.

    Writes only the occupied part of the single shared scratch buffer. A short
    write keeps the transaction unpublished and must be followed by cancellation.

    @param result Receives the FatFs status of any failed write.

    @return true - All queued bytes were written, or none were pending.
    @return false - The write failed or returned fewer bytes than requested.
*/
bool
FlushMediaWrites (
    int& result
    )
{
    UINT count;

    result = FR_OK;
    if (!managerState.save.bufferedBytes) {
        return true;
    }
    result = f_write(&managerState.mediaFile, managerState.readBuffer, UINT(managerState.save.bufferedBytes), &count);
    if (result != FR_OK || count != managerState.save.bufferedBytes) {
        return false;
    }
    managerState.save.bufferedBytes = 0;
    return true;
} // FlushMediaWrites

/**
    Queues video payload and checksum records through the shared buffer.

    Copies fragments into the bounded buffer and flushes it when full. Video
    ownership prevents photo operations from reusing those queued bytes.

    @param bytes Payload or frame-checksum bytes to queue.
    @param count Number of bytes to append.
    @param result Receives the FatFs status of any failed flush.

    @return true - All supplied bytes were queued or flushed.
    @return false - A buffered write failed or was short.
*/
bool
QueueMediaWrite (
    const uint8_t* bytes,
    size_t count,
    int& result
    )
{
    size_t part;

    result = FR_OK;
    while (count) {
        part = std::min(count, ChunkBytes - managerState.save.bufferedBytes);
        std::memcpy(managerState.readBuffer + managerState.save.bufferedBytes, bytes, part);
        managerState.save.bufferedBytes += part;
        bytes += part;
        count -= part;
        if (managerState.save.bufferedBytes == ChunkBytes && !FlushMediaWrites(result)) {
            return false;
        }
    }
    return true;
} // QueueMediaWrite

/**
    Closes playback without acquiring a second storage guard.

    Lost sessions discard the handle without touching a replacement card.

    @param safe Whether the current playback file may be closed against its card.

    Callers already hold storage ownership. Clearing the description prevents a
    closed file or old generation from being reused by the player.
*/
void
CloseSavedMediaPlayback (
    bool safe
    )
{
    if (safe && managerState.playback.open) {
        f_close(&managerState.playbackFile);
    }
    managerState.playbackFile = {};
    managerState.playback = {};
    managerState.jpegIndexCount = 0;
    managerState.hasPlaybackFrame = false;
    MemoryStorage::playbackPrefetch.Reset();
    MemoryStorage::playbackFile = nullptr;
} // CloseSavedMediaPlayback

/**
    Discards an unpublished save without reacquiring storage ownership.

    Closes and removes only an unpublished target while the card session remains safe.
    A failed close is checked before unlinking so a lost or replaced card receives no
    further writes. Handles are reset in every case; the caller decides whether the
    returned cleanup error invalidates storage.

    @param safe True permits cleanup writes while the current card session is usable.

    @return 0 - No transaction needed cleanup, or cleanup completed successfully.
    @return other - FatFs error encountered while closing or removing the unfinished target.
*/
int
DiscardIncompleteMediaSave (
    bool safe
    )
{
    char path[LIBRARY_PATH_BYTES];
    FRESULT result;
    FRESULT removed;

    result = FR_OK;
    if (managerState.save.phase == SavePhase::Idle) {
        return int(result);
    }
    if (safe && managerState.save.open) {
        result = f_close(&managerState.mediaFile);
        safe = !IsStorageIoError(result);
    }
    if (safe) {
        removed = f_unlink(BuildActiveSaveFilePath(path));
        if (result == FR_OK && removed != FR_NO_FILE) {
            result = removed;
        }
    }
    managerState.mediaFile = {};
    managerState.save = {};
    return int(result);
} // DiscardIncompleteMediaSave

/**
    Builds the checked raw or indexed-JPEG video header for the current save.

    The final header includes frame stride, generation, descriptor and expected
    payload checksum. It is installed only after complete SD readback succeeds.

    @param header Output buffer containing FileHeaderBytes writable bytes.
*/
void
BuildVideoStorageHeader (
    uint8_t* header
    )
{
    std::memset(header, 0, MediaTransfer::FileHeaderBytes);
    std::memcpy(header, "H75L", MEDIA_MAGIC_BYTES);
    header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_RAW_VIDEO;
    if (managerState.save.metadata.pixelFormat == PixelFormat::JPEG) {
        header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_JPEG_VIDEO;
    }
    MediaMetadataCodec::WriteUnsigned16LittleEndian(header + STORAGE_HEADER_LENGTH_OFFSET, MediaTransfer::FileHeaderBytes);
    WriteUnsigned32LittleEndian(header + STORAGE_HEADER_GENERATION_OFFSET, managerState.save.generation);
    MediaMetadataCodec::EncodeMediaDescriptor(managerState.save.metadata, header + STORAGE_HEADER_METADATA_OFFSET,
                                            MediaMetadataCodec::HeaderBytes);
    WriteUnsigned32LittleEndian(header + VIDEO_FRAME_BYTES_OFFSET, uint32_t(managerState.save.metadata.FrameBytes()));
    if (managerState.save.metadata.pixelFormat == PixelFormat::JPEG) {
        WriteUnsigned32LittleEndian(header + VIDEO_FRAME_STRIDE_OFFSET, MediaTransfer::JpegIndexBytes);
    } else {
        WriteUnsigned32LittleEndian(header + VIDEO_FRAME_STRIDE_OFFSET,
                                   uint32_t(managerState.save.metadata.FrameBytes()) + FRAME_CHECKSUM_BYTES);
    }
    WriteUnsigned32LittleEndian(header + VIDEO_DATA_OFFSET_FIELD, MediaTransfer::FileHeaderBytes);
    WriteUnsigned32LittleEndian(header + VIDEO_PAYLOAD_CRC_OFFSET, managerState.save.expectedCrc);
    WriteUnsigned32LittleEndian(header + VIDEO_HEADER_CRC_OFFSET,
                               ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, VIDEO_HEADER_CRC_OFFSET));
} // BuildVideoStorageHeader

/**
    Streams and checks a raw frame from the inspection or playback handle.

    Delegates the bounded read/CRC pipeline while the caller retains storage ownership.
    Playback uses its 64 KiB file cache and alternates the shared scratch halves; inspection
    reads directly. The complete stored checksum must pass before pixels can be published.

    @param slot Checked frame metadata and storage layout.
    @param index Zero-based raw frame index.
    @param sink Optional consumer of provisional pixels.
    @param context Borrowed pixel-consumer state.
    @param inspecting True selects the inspection handle.
    @param timing Optional output read and pipeline measurements.

    @return result - Completion flag, filesystem status and validation diagnostic.
*/
SavedFrameResult
ReadAndVerifyRawVideoFrame (
    const SavedPhotoSlot& slot,
    uint32_t index,
    MemoryManager::ImageSink sink,
    void* context,
    bool inspecting,
    MemoryManager::FrameReadTiming* timing
    )
{
    FIL* file = nullptr;
    MemoryStorage::Slot description;
    MemoryStorage::FrameResult result;

    file = &managerState.playbackFile;
    if (inspecting) {
        file = &managerState.imageFile;
    }
    description = {slot.metadata, slot.dataOffset, slot.checksum, slot.crcSeed, slot.frameChecksums};
    result = MemoryStorage::ReadAndVerifyRawVideoFrame(*file, description, index, sink, context, timing);
    return {result.complete, int(result.result), result.error};
} // ReadAndVerifyRawVideoFrame

/**
    Inspects a photo, raw video or indexed JPEG video in a retained slot.

    Legacy photos retain complete validation. Video headers must match supported
    dimensions, stride and exact file size; their first frame is checked before
    the slot is eligible. Later frames are checked as they are read, avoiding a
    whole-video scan at boot. A previously failed playback slot is excluded.

    @param index Alternating slot to inspect.
    @param path Optional managed path overriding the retained-slot filename.
    @param checkFirstFrame Whether to validate the first frame before accepting the header.

    @return slot - Validity, metadata, generation and any I/O/verification failure.
*/
SavedPhotoSlot
InspectSavedMediaSlot (
    unsigned index,
    const char* path,
    bool checkFirstFrame
    )
{
    SavedPhotoSlot slot;
    uint8_t header[MediaTransfer::FileHeaderBytes];
    UINT count;
    FRESULT closed;
    bool valid;
    SavedFrameResult first;
    bool jpeg;
    uint64_t expectedSize;
    uint32_t expectedStride;

    // Classify missing and inaccessible slots before attempting to parse their headers.
    bool reserved;

    reserved = path == nullptr;
    if (!path) {
        path = GetSavedMediaPath(index);
    }
    slot.result = f_open(&managerState.imageFile, path, FA_READ);
    if (slot.result == FR_NO_FILE) {
        return slot;
    }
    if (slot.result != FR_OK) {
        slot.state = SlotState::IoError;
        return slot;
    }

    // Recognize older photo containers and delegate their complete validation to the photo reader.
    std::memset(header, 0, sizeof(header));
    slot.result = f_read(&managerState.imageFile, header, STORAGE_HEADER_PREFIX_BYTES, &count);
    if (slot.result == FR_OK && count == STORAGE_HEADER_PREFIX_BYTES &&
        (!std::memcmp(header, "H75I", MEDIA_MAGIC_BYTES) ||
         (!std::memcmp(header, "H75L", MEDIA_MAGIC_BYTES) &&
          header[STORAGE_HEADER_VERSION_OFFSET] != STORAGE_VERSION_RAW_VIDEO &&
          header[STORAGE_HEADER_VERSION_OFFSET] != STORAGE_VERSION_JPEG_VIDEO))) {
        closed = f_close(&managerState.imageFile);
        if (closed != FR_OK) {
            slot.state = SlotState::IoError;
            slot.result = closed;
            return slot;
        }
        return ReadAndValidatePhotoFile(path, nullptr, nullptr);
    }

    // Validate the video header checksum and descriptor before trusting any offsets or lengths.
    slot.state = SlotState::Invalid;
    valid = slot.result == FR_OK && count == STORAGE_HEADER_PREFIX_BYTES;
    if (valid) {
        slot.result = f_read(&managerState.imageFile, header + STORAGE_HEADER_PREFIX_BYTES,
                             sizeof(header) - STORAGE_HEADER_PREFIX_BYTES, &count);
        valid = slot.result == FR_OK && count == sizeof(header) - STORAGE_HEADER_PREFIX_BYTES &&
                !std::memcmp(header, "H75L", MEDIA_MAGIC_BYTES) &&
                (header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_RAW_VIDEO ||
                 header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_JPEG_VIDEO) &&
                header[STORAGE_HEADER_RESERVED_OFFSET] == 0 &&
                MediaMetadataCodec::ReadUnsigned16LittleEndian(header + STORAGE_HEADER_LENGTH_OFFSET) == sizeof(header) &&
                ReadUnsigned32LittleEndian(header + VIDEO_HEADER_CRC_OFFSET) ==
                    ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, VIDEO_HEADER_CRC_OFFSET) &&
                MediaMetadataCodec::DecodeMediaDescriptor(header + STORAGE_HEADER_METADATA_OFFSET,
                                                          MediaMetadataCodec::HeaderBytes, slot.metadata) &&
                MediaTransfer::IsSupportedVideo(slot.metadata, LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT) &&
                ReadUnsigned32LittleEndian(header + VIDEO_FRAME_BYTES_OFFSET) == slot.metadata.FrameBytes() &&
                ReadUnsigned32LittleEndian(header + VIDEO_DATA_OFFSET_FIELD) == sizeof(header);
    }
    if (valid) {

        // Apply the exact raw-record or indexed-JPEG file layout and reject extra or missing bytes.
        jpeg = slot.metadata.pixelFormat == PixelFormat::JPEG;
        expectedSize = sizeof(header) + slot.metadata.FrameBytes() * slot.metadata.frameCount +
                       uint64_t(FRAME_CHECKSUM_BYTES) * slot.metadata.frameCount;
        expectedStride = uint32_t(slot.metadata.FrameBytes()) + FRAME_CHECKSUM_BYTES;
        if (jpeg) {
            expectedSize = sizeof(header) + slot.metadata.encodedBytes;
            expectedStride = MediaTransfer::JpegIndexBytes;
        }
        valid = uint64_t(f_size(&managerState.imageFile)) == expectedSize &&
                ReadUnsigned32LittleEndian(header + VIDEO_FRAME_STRIDE_OFFSET) == expectedStride &&
                ((jpeg && header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_JPEG_VIDEO) ||
                 (!jpeg && header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_RAW_VIDEO));
    }
    if (valid) {

        // Check the first frame without loading the whole clip; later frames are checked during playback.
        slot.generation = ReadUnsigned32LittleEndian(header + STORAGE_HEADER_GENERATION_OFFSET);
        slot.dataOffset = sizeof(header);
        slot.frameChecksums = true;
        slot.indexedJpeg = jpeg;
        if (checkFirstFrame) {
            if (jpeg) {
                first = ReadAndVerifyJpegFrame(slot, 0, nullptr, nullptr, true);
            } else {
                first = ReadAndVerifyRawVideoFrame(slot, 0, nullptr, nullptr, true);
            }
            valid = first.complete;
            slot.result = first.result;
            slot.error = first.error;
        }
    }

    // Publish a usable candidate only after closing succeeds and no playback rejection excludes it.
    closed = f_close(&managerState.imageFile);
    if (slot.result != FR_OK || closed != FR_OK) {
        slot.state = SlotState::IoError;
        if (slot.result == FR_OK) {
            slot.result = closed;
        }
    } else if (valid && (!reserved || int(index) != managerState.rejectedSlot)) {
        slot.state = SlotState::Valid;
    }
    return slot;
} // InspectSavedMediaSlot

/**
    Validates an indexed JPEG record before completing provisional frame output.

    Checks index CRC, payload bounds, record length, and adjacent-record continuity
    before decoding. The same bounded memory handles startup, random/reverse
    playback and incremental save validation. Full decoding plus encoded CRC and
    end-marker checks must all succeed before a caller publishes the frame.

    @param slot Checked JPEG slot metadata and payload offset.
    @param index Zero-based independently seekable frame.
    @param sink Optional consumer of provisional RGB888 or RGB565 rows.
    @param context Borrowed pixel-consumer state.
    @param inspecting Selects the startup inspection handle.
    @param verifying Selects the active unpublished save handle instead.
    @param timing Optional output counters for the bounded frame read.
    @param outputRGB565 True selects RGB565 JPEG pixels; false selects RGB888.

    @return result - Completion, filesystem status and a stable failure description.
*/
SavedFrameResult
ReadAndVerifyJpegFrame (
    const SavedPhotoSlot& slot,
    uint32_t index,
    MemoryManager::ImageSink sink,
    void* context,
    bool inspecting,
    bool verifying,
    MemoryManager::FrameReadTiming* timing,
    bool outputRGB565
    )
{
    SavedFrameResult read;
    FIL* file = nullptr;
    uint8_t entry[MediaTransfer::JpegIndexBytes];
    uint8_t record[MediaTransfer::JpegRecordBytes];
    UINT count;
    uint32_t offset;
    uint32_t length;
    uint32_t expectedCrc;
    uint32_t nextOffset;
    uint64_t end;
    MemoryStorage::FrameResult decoded;

    // Choose inspection, playback or verification ownership without opening another file handle.
    read.error = "invalid indexed JPEG frame";
    file = &managerState.playbackFile;
    if (inspecting) {
        file = &managerState.imageFile;
    }
    if (verifying) {
        file = &managerState.mediaFile;
    }

    // Playback uses a checked index window; inspection and verification read independent index records.
    if (!inspecting && !verifying) {
        if (!LoadJpegIndexWindow(*file, slot, index, read.result)) {
            return read;
        }
        offset = managerState.jpegIndex[index - managerState.jpegIndexFirst].offset;
        length = managerState.jpegIndex[index - managerState.jpegIndexFirst].length;
    } else {
        read.result = MemoryStorage::SeekPlaybackFile(*file, slot.dataOffset + uint64_t(index) * MediaTransfer::JpegIndexBytes);
        if (read.result == FR_OK) {
            read.result = MemoryStorage::ReadPlaybackFileBytes(*file, entry, sizeof(entry), &count, timing);
        }
        if (read.result != FR_OK || count != sizeof(entry)) {
            return read;
        }

        // Validate the selected index checksum and encoded bounds before using its record offset.
        offset = ReadUnsigned32LittleEndian(entry + JPEG_ENTRY_OFFSET_FIELD);
        length = ReadUnsigned32LittleEndian(entry + JPEG_ENTRY_LENGTH_FIELD);
        expectedCrc = ReadUnsigned32LittleEndian(entry + JPEG_ENTRY_CHECKSUM_FIELD);
        WriteUnsigned32LittleEndian(entry + JPEG_ENTRY_CHECKSUM_FIELD, index);
        end = uint64_t(offset) + MediaTransfer::JpegRecordBytes + length;
        if (expectedCrc != ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, entry, sizeof(entry)) ||
            length < JPEG_MINIMUM_ENCODED_BYTES || length > MediaTransfer::MaximumJpegBytes ||
            offset < uint64_t(slot.metadata.frameCount) * MediaTransfer::JpegIndexBytes ||
            end > slot.metadata.encodedBytes) {
            return read;
        }
        if (index == 0 && offset != uint64_t(slot.metadata.frameCount) * MediaTransfer::JpegIndexBytes) {
            return read;
        }

        // Require contiguous records and an exact payload end to reject aliases, gaps and trailing data.
        if (index + 1 < slot.metadata.frameCount) {
            read.result = MemoryStorage::ReadPlaybackFileBytes(*file, entry, sizeof(entry), &count, timing);
            if (read.result != FR_OK || count != sizeof(entry)) {
                return read;
            }
            nextOffset = ReadUnsigned32LittleEndian(entry);
            if (end != nextOffset) {
                return read;
            }
        } else if (end != slot.metadata.encodedBytes) {
            return read;
        }
    }

    // Continue adjacent forward records without seeking, then cross-check their declared lengths.
    if (MemoryStorage::GetPlaybackFilePosition(*file) != slot.dataOffset + offset) {
        read.result = MemoryStorage::SeekPlaybackFile(*file, slot.dataOffset + offset);
    }
    if (read.result == FR_OK) {
        read.result = MemoryStorage::ReadPlaybackFileBytes(*file, record, sizeof(record), &count, timing);
    }
    if (read.result != FR_OK || count != sizeof(record) || ReadUnsigned32LittleEndian(record) != length) {
        return read;
    }
    expectedCrc = ReadUnsigned32LittleEndian(record + FRAME_CHECKSUM_BYTES);

    // Decode and checksum encoded bytes while preserving the existing strict index validation.
    decoded = MemoryStorage::DecodeEncodedJpegFrame(*file, slot.metadata, length, expectedCrc,
                                                   sink, context, timing, outputRGB565);
    read = {decoded.complete, int(decoded.result), decoded.error};
    return read;
} // ReadAndVerifyJpegFrame

/**
    Loads and checks a bounded index window for JPEG playback or save verification.

    Caches offset/length pairs for at most 512 frames in four KiB. Every entry
    CRC, bound and local continuity check passes before the window is published.
    A preceding entry validates the window boundary, and the final entry must end
    exactly at the payload boundary. Cache ownership ends whenever playback closes or verification starts.
    Save readback borrows the same cache while playback is suspended; the supplied
    file always belongs to that exclusive operation.

    @param file Open playback or verification file whose index is being checked.
    @param slot Checked JPEG metadata and stored payload offset.
    @param index Frame whose index window is required.
    @param result Receives filesystem status; stays zero for invalid index content.

    @return true - The selected entry belongs to the checked active window.
    @return false - Index readback, CRC, bounds or record continuity failed.
*/
bool
LoadJpegIndexWindow (
    FIL& file,
    const SavedPhotoSlot& slot,
    uint32_t index,
    int& result
    )
{
    uint32_t first;
    uint32_t readFirst;
    uint32_t entries;
    uint32_t prefix;
    uint32_t entryIndex;
    uint32_t frame;
    uint32_t offset;
    uint32_t length;
    uint32_t checksum;
    uint64_t previousEnd;
    uint8_t* encoded = nullptr;
    UINT bytes;
    UINT count;

    // Reuse the active window without touching the index file again.
    result = FR_OK;
    if (managerState.jpegIndexCount && index >= managerState.jpegIndexFirst &&
        index - managerState.jpegIndexFirst < managerState.jpegIndexCount) {
        return true;
    }

    // Invalidate the old window before loading a replacement and include its preceding boundary entry.
    managerState.jpegIndexCount = 0;
    first = index / JPEG_INDEX_CACHE_ENTRIES * JPEG_INDEX_CACHE_ENTRIES;
    readFirst = first;
    prefix = 0;
    if (first) {
        --readFirst;
        prefix = 1;
    }
    entries = std::min(uint32_t(JPEG_INDEX_CACHE_ENTRIES), slot.metadata.frameCount - first);
    bytes = UINT((entries + prefix) * MediaTransfer::JpegIndexBytes);
    static_assert((JPEG_INDEX_CACHE_ENTRIES + 1) * MediaTransfer::JpegIndexBytes <= JPEG_INPUT_BUFFER_BYTES,
                  "JPEG index window exceeds shared input scratch");

    // Read the complete bounded window into shared scratch before publishing any cached entry.
    result = MemoryStorage::SeekPlaybackFile(file, slot.dataOffset + uint64_t(readFirst) * MediaTransfer::JpegIndexBytes);
    if (result == FR_OK) {
        result = MemoryStorage::ReadPlaybackFileBytes(file, managerState.readBuffer, bytes, &count, nullptr);
    }
    if (result != FR_OK || count != bytes) {
        return false;
    }

    // Validate index CRCs, frame bounds and continuity, including the window and payload boundaries.
    previousEnd = 0;
    for (entryIndex = 0; entryIndex < entries + prefix; ++entryIndex) {
        frame = readFirst + entryIndex;
        encoded = managerState.readBuffer + entryIndex * MediaTransfer::JpegIndexBytes;
        offset = ReadUnsigned32LittleEndian(encoded + JPEG_ENTRY_OFFSET_FIELD);
        length = ReadUnsigned32LittleEndian(encoded + JPEG_ENTRY_LENGTH_FIELD);
        checksum = ReadUnsigned32LittleEndian(encoded + JPEG_ENTRY_CHECKSUM_FIELD);
        WriteUnsigned32LittleEndian(encoded + JPEG_ENTRY_CHECKSUM_FIELD, frame);
        if (checksum != ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, encoded, MediaTransfer::JpegIndexBytes) ||
            length < JPEG_MINIMUM_ENCODED_BYTES || length > MediaTransfer::MaximumJpegBytes ||
            offset < uint64_t(slot.metadata.frameCount) * MediaTransfer::JpegIndexBytes ||
            uint64_t(offset) + MediaTransfer::JpegRecordBytes + length > slot.metadata.encodedBytes ||
            (entryIndex && offset != previousEnd) ||
            (!frame && offset != uint64_t(slot.metadata.frameCount) * MediaTransfer::JpegIndexBytes)) {
            return false;
        }
        previousEnd = uint64_t(offset) + MediaTransfer::JpegRecordBytes + length;
        if (frame + 1 == slot.metadata.frameCount && previousEnd != slot.metadata.encodedBytes) {
            return false;
        }
        if (entryIndex >= prefix) {
            managerState.jpegIndex[entryIndex - prefix] = {offset, length};
        }
    }

    // Expose the replacement window only after every loaded entry has passed validation.
    managerState.jpegIndexFirst = first;
    managerState.jpegIndexCount = entries;
    return true;
} // LoadJpegIndexWindow

/**
    Selects the path owned by the current save transaction.

    Library saves use an unpublished TMP file until all readback succeeds. Legacy
    retained slots keep their existing reserved filenames.

    @param path Caller-owned buffer containing LIBRARY_PATH_BYTES bytes.

    @return path - The borrowed path for this transaction; callers never free it.
*/
const char*
BuildActiveSaveFilePath (
    char* path
    )
{
    if (managerState.save.libraryId) {
        BuildPhotoLibraryPath(managerState.save.libraryId, path, true);
        return path;
    }
    if (managerState.save.videoId) {
        BuildVideoLibraryPath(managerState.save.videoId, path, true);
        return path;
    }
    return GetSavedMediaPath(managerState.save.target);
} // BuildActiveSaveFilePath

/**
    Identifies filesystem failures that require SD session recovery.

    @param result Result code returned by the SDK or filesystem operation.

    @return true - The result indicates a disk, readiness, timeout or invalid-session error.
    @return false - The result does not indicate one of those storage I/O failures.
*/
bool
IsStorageIoError (
    int result
    )
{
    return result == FR_DISK_ERR || result == FR_NOT_READY || result == FR_TIMEOUT ||
           result == FR_INVALID_OBJECT;
} // IsStorageIoError

/**
    Builds a managed video filename from its library ID and extension.

    Keeps the original two-digit VIDEO names for legacy IDs and uses the compact hexadecimal 8.3
    namespace for later IDs. The temporary flag selects the unpublished staging extension without
    changing the item's ID. Callers validate ID bounds and provide the fixed path capacity before
    using the generated filename.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param path Storage path used by this operation.
    @param temporary Whether to select the unpublished staging file instead of the saved item.
*/
void
BuildVideoLibraryPath (
    uint32_t id,
    char* path,
    bool temporary
    )
{
    const char* shortNameExtension = nullptr;
    const char* extendedNameExtension = nullptr;

    if (id <= LIBRARY_LEGACY_ID_MAX) {
        shortNameExtension = {};
        if (temporary) {
            shortNameExtension = "TMP";
        } else {
            shortNameExtension = "H75";
        }
        std::snprintf(path, LIBRARY_PATH_BYTES, "0:/VIDEO%02lu.%s", static_cast<unsigned long>(id),
                      shortNameExtension);
    } else {
        extendedNameExtension = {};
        if (temporary) {
            extendedNameExtension = "TMP";
        } else {
            extendedNameExtension = "H75";
        }
        std::snprintf(path, LIBRARY_PATH_BYTES, "0:/V%07lX.%s", static_cast<unsigned long>(id),
                      extendedNameExtension);
    }
} // BuildVideoLibraryPath

/**
    Builds a managed photo filename from its library ID and extension.

    Keeps the original two-digit PHOTO names for legacy IDs and uses the compact hexadecimal 8.3
    namespace for later IDs. Temporary and published files share the same ID with different
    extensions, allowing rename-based publication. This helper does not touch the directory or
    allocate a library slot.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param path Storage path used by this operation.
    @param temporary Whether to select the unpublished staging file instead of the saved item.
*/
void
BuildPhotoLibraryPath (
    uint32_t id,
    char* path,
    bool temporary
    )
{
    const char* shortNameExtension = nullptr;
    const char* extendedNameExtension = nullptr;

    if (id <= LIBRARY_LEGACY_ID_MAX) {
        shortNameExtension = {};
        if (temporary) {
            shortNameExtension = "TMP";
        } else {
            shortNameExtension = "H75";
        }
        std::snprintf(path, LIBRARY_PATH_BYTES, "0:/PHOTO%02lu.%s", static_cast<unsigned long>(id),
                      shortNameExtension);
    } else {
        extendedNameExtension = {};
        if (temporary) {
            extendedNameExtension = "TMP";
        } else {
            extendedNameExtension = "H75";
        }
        std::snprintf(path, LIBRARY_PATH_BYTES, "0:/P%07lX.%s", static_cast<unsigned long>(id),
                      extendedNameExtension);
    }
} // BuildPhotoLibraryPath

/**
    Parses a managed library filename into its media ID and file type.

    Private bounded directory traversal. Only canonical controller filenames are managed;
    unrelated files/directories are left alone. Existing names stay.

    @param name Name associated with the file or saved library item.
    @param id Output identifier assigned to the saved or discovered library item.
    @param video True selects videos; false selects photos.
    @param temporary Whether to select the unpublished staging file instead of the saved item.

    @return true - The filename identifies a valid controller-owned media item.
    @return false - The filename or its numeric ID is not valid for the library.
*/
bool
ParseLibraryName (
    const char* name,
    uint32_t& id,
    bool& video,
    bool& temporary
    )
{
    size_t length;
    const char* extension = nullptr;
    size_t i;
    char c;

    length = std::strlen(name);
    if (length != LIBRARY_LEGACY_FILENAME_BYTES && length != LIBRARY_HEX_FILENAME_BYTES) {
        return false;
    }
    extension = name + length - LIBRARY_EXTENSION_BYTES;
    temporary = !std::strcmp(extension, ".TMP");
    if (!temporary && std::strcmp(extension, ".H75")) {
        return false;
    }
    id = 0;
    if (!std::strncmp(name, "PHOTO", LIBRARY_LEGACY_PREFIX_BYTES) ||
        !std::strncmp(name, "VIDEO", LIBRARY_LEGACY_PREFIX_BYTES)) {
        if (length != LIBRARY_LEGACY_FILENAME_BYTES || name[LIBRARY_LEGACY_PREFIX_BYTES] < '0' ||
            name[LIBRARY_LEGACY_PREFIX_BYTES] > '9' || name[LIBRARY_LEGACY_ID_ONES_OFFSET] < '0' ||
            name[LIBRARY_LEGACY_ID_ONES_OFFSET] > '9') {
            return false;
        }
        video = name[0] == 'V';
        id = uint32_t(name[LIBRARY_LEGACY_PREFIX_BYTES] - '0') * DECIMAL_RADIX +
             uint32_t(name[LIBRARY_LEGACY_ID_ONES_OFFSET] - '0');
    } else {
        if (length != LIBRARY_HEX_FILENAME_BYTES || (name[0] != 'P' && name[0] != 'V')) {
            return false;
        }
        video = name[0] == 'V';
        for (i = 1; i < LIBRARY_HEX_ID_END_OFFSET; ++i) {
            c = name[i];
            if (c >= '0' && c <= '9') {
                id = id * HEXADECIMAL_RADIX + uint32_t(c - '0');
            } else if (c >= 'A' && c <= 'F') {
                id = id * HEXADECIMAL_RADIX + uint32_t(c - 'A' + DECIMAL_RADIX);
            } else {
                return false;
            }
        }
        if (id <= LIBRARY_LEGACY_ID_MAX) {
            return false; // No aliases for the original names.
        }
    }
    return id && id <= MemoryManager::MaximumLibraryId;
} // ParseLibraryName

/**
    Visits managed library entries without retaining the full directory.

    Visits only recognized controller-owned root files, decoding each filename into its media ID,
    type and staging status before invoking the borrowed visitor. The directory is closed after
    normal completion, early visitor stop or read failure. Traversal is synchronous and bounded in
    memory, so it can take longer as the card's directory grows without retaining a full list of
    entries.

    @param visit Callback invoked for each controller-owned library directory entry.

    @return FR_OK - The filesystem operation completed successfully.
    @return other - FatFs error code describing the failed filesystem operation.
*/
template <class Visitor>
int
ScanLibraryDirectory (
    Visitor visit
    )
{
    DIR directory;
    int result;
    FILINFO info;
    uint32_t id;
    bool video;
    bool temporary;
    int closed;

    directory = {};
    result = f_opendir(&directory, "0:/");

    if (result != FR_OK) {
        return result;
    }
    info = {};
    while ((result = f_readdir(&directory, &info)) == FR_OK && info.fname[0]) {
        id = 0;
        video = false;
        temporary = false;
        if (!(info.fattrib & AM_DIR) && ParseLibraryName(info.fname, id, video, temporary) &&
            !visit(id, video, temporary, info)) {
            break;
        }
    }

    closed = f_closedir(&directory);
    if (result == FR_OK) {
        return closed;
    } else {
        return result;
    }
} // ScanLibraryDirectory

/**
    Chooses the next unused ID for a named library item.

    Scans published entries of the selected media type to build a small occupancy bitmap for
    reusable low IDs and track the highest existing ID. It reuses a free low ID when possible,
    otherwise advances beyond the high-water mark within the filename namespace. Directory failure
    or exhaustion returns no ID rather than overwriting an existing published item.

    @param video True selects videos; false selects photos.
    @param result Output FatFs or frame-validation result, including any failure details.

    @return id - A nonzero unused media identifier.
    @return 0 - The directory could not be read or the ID namespace is exhausted; result gives
                the FatFs status.
*/
uint32_t
AllocateLibraryId (
    bool video,
    int& result
    )
{
    uint8_t occupied[LIBRARY_ID_BITMAP_BYTES];
    uint32_t largest;
    uint32_t id;

    // Reuse low deleted IDs with only 64 bytes. If that range is occupied,
    // append beyond the high-water ID rather than scanning every possible ID.
    std::memset(occupied, 0, sizeof(occupied));
    largest = 0;
    result = ScanLibraryDirectory([&](uint32_t id, bool isVideo, bool temporary, [[maybe_unused]] const FILINFO& info) {
        if (isVideo != video || temporary) {
            return true;
        }
        largest = std::max(largest, id);
        if (id <= LIBRARY_REUSABLE_ID_LIMIT) {
            occupied[(id - 1) / BITS_PER_BYTE] |= uint8_t(1u << ((id - 1) % BITS_PER_BYTE));
        }
        return true;
    });

    if (result != FR_OK) {
        return 0;
    }
    for (id = 1; id <= LIBRARY_REUSABLE_ID_LIMIT; ++id) {
        if (!(occupied[(id - 1) / BITS_PER_BYTE] & (1u << ((id - 1) % BITS_PER_BYTE)))) {
            return id;
        }
    }

    if (largest < MemoryManager::MaximumLibraryId) {
        return largest + 1;
    } else {
        return 0;
    }
} // AllocateLibraryId

/**
    Selects a bounded ordered page of library IDs after the requested cursor.

    Scans the directory while retaining only the lowest IDs beyond the exclusive cursor that fit
    in the caller's page. Sorted insertion keeps memory proportional to page size, while separate
    totals determine whether another page exists. Only IDs are selected here; later inspection
    fills each item's metadata and validity without decoding the entire collection.

    @param video True selects videos; false selects photos.
    @param items Output array of library entries for this page.
    @param capacity Maximum number of entries available in the output array.
    @param count Output number of entries or bytes produced by this operation.
    @param after Exclusive library ID cursor; zero starts at the first page.
    @param page Optional output pagination totals and continuation cursor.

    @return FR_OK - The filesystem operation completed successfully.
    @return other - FatFs error code describing the failed filesystem operation.
*/
template <class Item>
int
FindLibraryPage (
    bool video,
    Item* items,
    size_t capacity,
    size_t& count,
    uint32_t after,
    MemoryManager::LibraryPage& page
    )
{
    uint32_t remaining;
    int result;

    count = 0;
    page = {};
    remaining = 0;
    result = ScanLibraryDirectory([&](uint32_t id, bool isVideo, bool temporary, [[maybe_unused]] const FILINFO& info) {
        size_t at;
        size_t i;

        if (isVideo != video || temporary) {
            return true;
        }
        ++page.total;
        if (id <= after) {
            return true;
        }
        ++remaining;
        at = 0;
        while (at < count && items[at].id < id) {
            ++at;
        }
        if (at >= capacity) {
            return true;
        }
        if (count < capacity) {
            ++count;
        }
        for (i = count - 1; i > at; --i) {
            items[i].id = items[i - 1].id;
        }
        items[at].id = id;
        return true;
    });
    page.hasMore = remaining > count;
    if (count) {
        page.nextAfter = items[count - 1].id;
    } else {
        page.nextAfter = after;
    }
    return result;
} // FindLibraryPage

/**
    Selects a bounded ordered page of photo IDs after the requested cursor.

    @param items Output array of library entries for this page.
    @param capacity Maximum number of entries available in the output array.
    @param count Output number of entries selected.
    @param after Exclusive library ID cursor; zero starts at the first page.
    @param page Output pagination totals and continuation cursor.

    @return FR_OK - The page was selected successfully.
    @return other - FatFs error describing the failed directory operation.
*/
int
FindPhotoLibraryPage (
    MemoryManager::LibraryPhoto* items,
    size_t capacity,
    size_t& count,
    uint32_t after,
    MemoryManager::LibraryPage& page
    )
{
    return FindLibraryPage(false, items, capacity, count, after, page);
} // FindPhotoLibraryPage

/**
    Selects a bounded ordered page of video IDs after the requested cursor.

    @param items Output array of library entries for this page.
    @param capacity Maximum number of entries available in the output array.
    @param count Output number of entries selected.
    @param after Exclusive library ID cursor; zero starts at the first page.
    @param page Output pagination totals and continuation cursor.

    @return FR_OK - The page was selected successfully.
    @return other - FatFs error describing the failed directory operation.
*/
int
FindVideoLibraryPage (
    MemoryManager::LibraryVideo* items,
    size_t capacity,
    size_t& count,
    uint32_t after,
    MemoryManager::LibraryPage& page
    )
{
    return FindLibraryPage(true, items, capacity, count, after, page);
} // FindVideoLibraryPage

/**
    Checks whether a published video is already present in the library.

    @param existingVideo Output flag set when a published video is found.

    @return FR_OK - The directory was checked successfully.
    @return other - FatFs error describing the failed directory operation.
*/
int
FindExistingLibraryVideo (
    bool& existingVideo
    )
{
    existingVideo = false;
    return ScanLibraryDirectory([&]([[maybe_unused]] uint32_t id, bool video, bool temporary, [[maybe_unused]] const FILINFO& info) {
        if (video && !temporary) {
            existingVideo = true;
        }
        return !existingVideo;
    });
} // FindExistingLibraryVideo

/**
    Validates a named photo's header and reads its library metadata.

    Reads the bounded library header and verifies both its base and extended checksums, photo
    descriptor, stored name terminator and payload/file-size relationships. Valid metadata and the
    optional persisted thumbnail are copied from that header; this listing path does not decode or
    checksum the full photo payload. Damaged entries can still receive an ID/name placeholder,
    while filesystem errors retain an I/O-failed result for storage recovery.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param photo Output description of the requested saved media item.
    @param thumbnail RGB565 preview pixel buffer for the saved media item.

    @return result - Inspection/read result including validity, metadata and any I/O or
                     validation error.
*/
SavedPhotoSlot
InspectPhotoLibraryFile (
    uint32_t id,
    MemoryManager::LibraryPhoto* photo,
    uint8_t* thumbnail
    )
{
    SavedPhotoSlot slot;
    char path[LIBRARY_PATH_BYTES];
    UINT count;
    const uint8_t* header = nullptr;
    bool valid;
    uint64_t payload;
    bool fileSizeMatches;
    bool frameStrideMatches;
    uint64_t expectedFrameStride;
    uint32_t frameChecksumBytes;
    int closed;

    slot = {};

    // Validate the root filename and image inputs before opening a file.
    BuildPhotoLibraryPath(id, path);

    // Open the saved file before validating its header and reading pixels.
    slot.result = f_open(&managerState.imageFile, path, FA_READ);
    if (slot.result == FR_NO_FILE) {
        return slot;
    }
    if (slot.result != FR_OK) {
        slot.state = SlotState::IoError;
        return slot;
    }
    slot.state = SlotState::Invalid;
    if (photo) {
        *photo = {};
        photo->id = id;
        photo->bytes = f_size(&managerState.imageFile);
        std::snprintf(photo->name, sizeof(photo->name), "Photo %lu (damaged)",
                      static_cast<unsigned long>(id));
    }
    count = 0;
    slot.result = f_read(&managerState.imageFile, managerState.readBuffer, LibraryHeaderBytes, &count);
    header = managerState.readBuffer;
    valid = slot.result == FR_OK && count == LibraryHeaderBytes &&
        !std::memcmp(header, "H75L", MEDIA_MAGIC_BYTES) &&
        (header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_LIBRARY_RAW_PHOTO ||
         header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_LIBRARY_JPEG_PHOTO) &&
        header[STORAGE_HEADER_RESERVED_OFFSET] == 0 &&
        MediaMetadataCodec::ReadUnsigned16LittleEndian(header + STORAGE_HEADER_LENGTH_OFFSET) ==
            LibraryHeaderBytes &&
        ReadUnsigned32LittleEndian(header + STORAGE_HEADER_DATA_OFFSET_FIELD) == LibraryHeaderBytes &&
        ReadUnsigned32LittleEndian(header + STORAGE_HEADER_CRC_OFFSET) ==
            ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, STORAGE_HEADER_CRC_OFFSET) &&
        ReadUnsigned32LittleEndian(header + LibraryHeaderBytes - FRAME_CHECKSUM_BYTES) ==
            ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, LibraryHeaderBytes - FRAME_CHECKSUM_BYTES) &&
        MediaMetadataCodec::DecodeMediaDescriptor(header + STORAGE_HEADER_METADATA_OFFSET,
                                                  MEDIA_DESCRIPTOR_BYTES, slot.metadata) &&
        slot.metadata.IsPhotoWithin(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT) &&
        header[LIBRARY_NAME_TERMINATOR_OFFSET] == 0;
    slot.indexedJpeg = slot.metadata.pixelFormat == PixelFormat::JPEG;
    payload = 0;
    if (valid) {
        fileSizeMatches = {};
        frameStrideMatches = {};
        if (slot.metadata.PayloadBytes(payload) &&
            slot.indexedJpeg == (header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_LIBRARY_JPEG_PHOTO) &&
            ReadUnsigned32LittleEndian(header + STORAGE_HEADER_FRAME_BYTES_OFFSET) ==
                slot.metadata.FrameBytes()) {
            expectedFrameStride = {};
            if (slot.indexedJpeg) {
                expectedFrameStride = MediaTransfer::JpegIndexBytes;
            } else {
                expectedFrameStride = slot.metadata.FrameBytes() + FRAME_CHECKSUM_BYTES;
            }
            frameStrideMatches = bool(
                ReadUnsigned32LittleEndian(header + STORAGE_HEADER_FRAME_STRIDE_OFFSET) == expectedFrameStride);
        } else {
            frameStrideMatches = false;
        }
        if (frameStrideMatches) {
            frameChecksumBytes = {};
            if (slot.indexedJpeg) {
                frameChecksumBytes = 0;
            } else {
                frameChecksumBytes = FRAME_CHECKSUM_BYTES;
            }
            fileSizeMatches =
                bool(uint64_t(f_size(&managerState.imageFile)) == LibraryHeaderBytes + payload + frameChecksumBytes);
        } else {
            fileSizeMatches = false;
        }
        valid = fileSizeMatches;
    }
    if (valid) {
        slot.dataOffset = LibraryHeaderBytes;
        slot.frameChecksums = true;
        slot.checksum = ReadUnsigned32LittleEndian(header + STORAGE_HEADER_PAYLOAD_CRC_OFFSET);
        slot.generation = ReadUnsigned32LittleEndian(header + STORAGE_HEADER_GENERATION_OFFSET);
        if (photo) {
            photo->valid = true;
            photo->checksum = ReadUnsigned32LittleEndian(header + STORAGE_HEADER_PAYLOAD_CRC_OFFSET);
            photo->metadata = slot.metadata;
            photo->bytes = payload;
            std::memcpy(photo->name, header + LIBRARY_NAME_OFFSET, sizeof(photo->name));
        }
        if (thumbnail) {
            std::memcpy(thumbnail, header + LIBRARY_EXTRA_DATA_OFFSET, MemoryManager::ThumbnailBytes);
        }
    }
    closed = f_close(&managerState.imageFile);
    if (slot.result != FR_OK || closed != FR_OK) {
        slot.state = SlotState::IoError;
        if (slot.result == FR_OK) {
            slot.result = closed;
        }
    } else if (valid) {
        slot.state = SlotState::Valid;
    }
    return slot;
} // InspectPhotoLibraryFile

/**
    Reads and validates one saved gallery settings record.

    Reads one retained settings record, accepts supported legacy/current lengths and checks its
    magic, reserved bytes and checksum. Stored selection and display values are validated together
    and older records receive compatible defaults for fields they did not contain. Invalid content
    is distinct from an actual read/close error, allowing the caller to choose the other retained
    generation safely.

    @param index Zero-based index of the requested entry, slot or frame.

    @return result - Inspection/read result including validity, metadata and any I/O or
                     validation error.
*/
GalleryRecord
ReadAndValidateGalleryRecord (
    unsigned index
    )
{
    GalleryRecord result;
    uint8_t header[GALLERY_CONTROLS_RECORD_BYTES];
    UINT count;
    bool legacy;
    bool current;
    bool controls;
    uint32_t video;
    int closed;

    result = {};

    // Open the saved file before validating its header and reading pixels.
    result.result = f_open(&managerState.imageFile, GalleryPaths[index], FA_READ);
    if (result.result == FR_NO_FILE) {
        result.result = FR_OK;
        return result;
    }
    if (result.result != FR_OK) {
        return result;
    }

    // Validate the file header and declared size before accepting pixel data.
    std::memset(header, 0, sizeof(header));
    count = 0;
    result.result = f_read(&managerState.imageFile, header, sizeof(header), &count);
    legacy = count == GALLERY_LEGACY_RECORD_BYTES && !std::memcmp(header, "H75S", MEDIA_MAGIC_BYTES);
    current = count == GALLERY_VIDEO_RECORD_BYTES && !std::memcmp(header, "H75G", MEDIA_MAGIC_BYTES);
    controls = count == GALLERY_CONTROLS_RECORD_BYTES && !std::memcmp(header, "H75C", MEDIA_MAGIC_BYTES);
    if (current || controls) {
        video = ReadUnsigned32LittleEndian(header + GALLERY_VIDEO_ID_OFFSET);
    } else {
        video = 0;
    }
    result.valid =
        result.result == FR_OK && (legacy || current || controls) && f_size(&managerState.imageFile) == count &&
        ReadUnsigned32LittleEndian(header + count - FRAME_CHECKSUM_BYTES) ==
            ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, count - FRAME_CHECKSUM_BYTES) &&
        ReadUnsigned32LittleEndian(header + GALLERY_PHOTO_ID_OFFSET) <= MemoryManager::MaximumLibraryId &&
        ReadUnsigned32LittleEndian(header + GALLERY_INTERVAL_OFFSET) >= 1 &&
        ReadUnsigned32LittleEndian(header + GALLERY_INTERVAL_OFFSET) <= MAX_SLIDESHOW_INTERVAL_SECONDS &&
        header[GALLERY_SLIDESHOW_OFFSET] <= 1 && !header[GALLERY_RESERVED_OFFSET_ONE] &&
        !header[GALLERY_RESERVED_OFFSET_TWO] && !header[GALLERY_RESERVED_OFFSET_THREE] &&
        video <= MemoryManager::MaximumLibraryId &&
        (!video || (!ReadUnsigned32LittleEndian(header + GALLERY_PHOTO_ID_OFFSET) &&
                    !header[GALLERY_SLIDESHOW_OFFSET])) &&
        (!current || (!ReadUnsigned32LittleEndian(header + GALLERY_FPS_OFFSET))) &&
        (!controls ||
         (ReadUnsigned32LittleEndian(header + GALLERY_FPS_OFFSET) <= MAX_VIDEO_FPS &&
          ReadUnsigned32LittleEndian(header + GALLERY_BRIGHTNESS_OFFSET) <= MAX_BRIGHTNESS_PERCENT &&
          !ReadUnsigned32LittleEndian(header + GALLERY_CONTROLS_RESERVED_OFFSET)));
    if (result.valid) {
        result.generation = ReadUnsigned32LittleEndian(header + GALLERY_GENERATION_OFFSET);
        result.settings.selectedPhoto = ReadUnsigned32LittleEndian(header + GALLERY_PHOTO_ID_OFFSET);
        result.settings.intervalSeconds = ReadUnsigned32LittleEndian(header + GALLERY_INTERVAL_OFFSET);
        result.settings.slideshow = header[GALLERY_SLIDESHOW_OFFSET];
        result.settings.selectedVideo = video;
        if (controls) {
            result.settings.maxFps = ReadUnsigned32LittleEndian(header + GALLERY_FPS_OFFSET);
            result.settings.brightness = ReadUnsigned32LittleEndian(header + GALLERY_BRIGHTNESS_OFFSET);
        }
    }
    closed = f_close(&managerState.imageFile);
    if (result.result == FR_OK) {
        result.result = closed;
    }
    return result;
} // ReadAndValidateGalleryRecord

/**
    Selects the newest valid saved gallery settings record.

    Ignores invalid retained records and compares generation numbers with wrap-aware ordering when
    both copies are usable. This identifies the latest fully validated settings without modifying
    either file. With no valid copy, the caller can use default settings or create the first
    generation.

    @param records Inspected gallery settings records to compare.

    @return 0 - The first settings record is the newest valid copy.
    @return 1 - The second settings record is the newest valid copy.
    @return STORAGE_NO_SLOT - Neither settings record is valid.
*/
int
FindNewestValidGalleryRecord (
    const GalleryRecord* records
    )
{
    uint32_t difference;

    if (!records[0].valid) {
        if (records[1].valid) {
            return 1;
        } else {
            return STORAGE_NO_SLOT;
        }
    }
    if (!records[1].valid) {
        return 0;
    }
    difference = records[1].generation - records[0].generation;
    if (difference && difference < STORAGE_GENERATION_HALF_RANGE_U) {
        return 1;
    } else {
        return 0;
    }
} // FindNewestValidGalleryRecord

/**
    Validates a named video's header and reads its library metadata.

    Validates the named video's base/extended header checksums, media descriptor, name, upload
    metadata and expected file layout before returning a library record. Listing inspects metadata
    only, leaving encoded-frame integrity checks to playback and upload verification. A damaged
    file can remain represented as an invalid library item, while actual open/read/close errors
    remain I/O failures.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param video Video upload service or output video description; null is allowed where
                 optional.

    @return result - Inspection/read result including validity, metadata and any I/O or
                     validation error.
*/
SavedPhotoSlot
InspectLibraryVideo (
    uint32_t id,
    MemoryManager::LibraryVideo* video
    )
{
    SavedPhotoSlot slot;
    char path[LIBRARY_PATH_BYTES];
    UINT count;
    const uint8_t* header = nullptr;
    int closed;
    bool valid;
    uint64_t payload;
    bool fileSizeMatches;
    bool frameStrideMatches;
    uint64_t expectedFrameStride;
    uint64_t frameChecksumBytes;
    int closeClosed;

    slot = {};

    // Validate the root filename and image inputs before opening a file.
    BuildVideoLibraryPath(id, path);

    // Open the saved file before validating its header and reading pixels.
    slot.result = f_open(&managerState.imageFile, path, FA_READ);
    if (slot.result == FR_NO_FILE) {
        return slot;
    }
    if (slot.result != FR_OK) {
        slot.state = SlotState::IoError;
        return slot;
    }
    slot.state = SlotState::Invalid;
    if (video) {
        *video = {};
        video->id = id;
        video->bytes = f_size(&managerState.imageFile);
        std::snprintf(video->name, sizeof(video->name), "Video %lu (damaged)",
                      static_cast<unsigned long>(id));
    }
    count = 0;
    slot.result = f_read(&managerState.imageFile, managerState.readBuffer, VideoLibraryHeaderBytes, &count);
    header = managerState.readBuffer;
    if (slot.result == FR_OK && count >= MEDIA_FILE_HEADER_BYTES &&
        !std::memcmp(header, "H75L", MEDIA_MAGIC_BYTES) &&
        (header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_RAW_VIDEO ||
         header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_JPEG_VIDEO)) {
        // Existing r46 media can be adopted without copying or re-encoding it.
        closed = f_close(&managerState.imageFile);
        if (closed != FR_OK) {
            slot.state = SlotState::IoError;
            slot.result = closed;
            return slot;
        }
        slot = InspectSavedMediaSlot(0, path, false);
        if (video && slot.state == SlotState::Valid) {
            video->valid = true;
            video->metadata = slot.metadata;
            video->checksum = slot.checksum;
            slot.metadata.PayloadBytes(video->bytes);
            std::snprintf(video->name, sizeof(video->name), "Restored video %lu",
                          static_cast<unsigned long>(id));
        }
        return slot;
    }
    valid = slot.result == FR_OK && count == VideoLibraryHeaderBytes &&
        !std::memcmp(header, "H75L", MEDIA_MAGIC_BYTES) &&
        (header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_LIBRARY_RAW_VIDEO ||
         header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_LIBRARY_JPEG_VIDEO) &&
        header[STORAGE_HEADER_RESERVED_OFFSET] == 0 &&
        MediaMetadataCodec::ReadUnsigned16LittleEndian(header + STORAGE_HEADER_LENGTH_OFFSET) ==
            VideoLibraryHeaderBytes &&
        ReadUnsigned32LittleEndian(header + STORAGE_HEADER_DATA_OFFSET_FIELD) == VideoLibraryHeaderBytes &&
        ReadUnsigned32LittleEndian(header + STORAGE_HEADER_CRC_OFFSET) ==
            ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, STORAGE_HEADER_CRC_OFFSET) &&
        ReadUnsigned32LittleEndian(header + VideoLibraryHeaderBytes - STORAGE_HEADER_VERSION_OFFSET) ==
            ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header,
                              VideoLibraryHeaderBytes - STORAGE_HEADER_VERSION_OFFSET) &&
        MediaMetadataCodec::DecodeMediaDescriptor(header + STORAGE_HEADER_METADATA_OFFSET,
                                                  MEDIA_DESCRIPTOR_BYTES, slot.metadata) &&
        MediaTransfer::IsSupportedVideo(slot.metadata, LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT) &&
        header[LIBRARY_NAME_TERMINATOR_OFFSET] == 0;
    slot.indexedJpeg = slot.metadata.pixelFormat == PixelFormat::JPEG;
    payload = 0;
    if (valid) {
        fileSizeMatches = {};
        frameStrideMatches = {};
        if (slot.metadata.PayloadBytes(payload) &&
            slot.indexedJpeg == (header[STORAGE_HEADER_VERSION_OFFSET] == STORAGE_VERSION_LIBRARY_JPEG_VIDEO) &&
            ReadUnsigned32LittleEndian(header + STORAGE_HEADER_FRAME_BYTES_OFFSET) ==
                slot.metadata.FrameBytes()) {
            expectedFrameStride = {};
            if (slot.indexedJpeg) {
                expectedFrameStride = MediaTransfer::JpegIndexBytes;
            } else {
                expectedFrameStride = slot.metadata.FrameBytes() + FRAME_CHECKSUM_BYTES;
            }
            frameStrideMatches = bool(
                ReadUnsigned32LittleEndian(header + STORAGE_HEADER_FRAME_STRIDE_OFFSET) == expectedFrameStride);
        } else {
            frameStrideMatches = false;
        }
        if (frameStrideMatches) {
            frameChecksumBytes = {};
            if (slot.indexedJpeg) {
                frameChecksumBytes = 0;
            } else {
                frameChecksumBytes = uint64_t(slot.metadata.frameCount) * FRAME_CHECKSUM_BYTES;
            }
            fileSizeMatches =
                bool(uint64_t(f_size(&managerState.imageFile)) == VideoLibraryHeaderBytes + payload + frameChecksumBytes);
        } else {
            fileSizeMatches = false;
        }
        valid = fileSizeMatches;
    }
    if (valid) {
        slot.dataOffset = VideoLibraryHeaderBytes;
        slot.frameChecksums = true;
        slot.checksum = ReadUnsigned32LittleEndian(header + STORAGE_HEADER_PAYLOAD_CRC_OFFSET);
        if (video) {
            video->valid = true;
            video->metadata = slot.metadata;
            video->bytes = payload;
            video->checksum = slot.checksum;
            video->maxSecondsMillis = ReadUnsigned32LittleEndian(header + LIBRARY_EXTRA_DATA_OFFSET);
            std::memcpy(video->name, header + LIBRARY_NAME_OFFSET, sizeof(video->name));
        }
    }
    closeClosed = f_close(&managerState.imageFile);
    if (slot.result != FR_OK || closeClosed != FR_OK) {
        slot.state = SlotState::IoError;
        if (slot.result == FR_OK) {
            slot.result = closeClosed;
        }
    } else if (valid) {
        slot.state = SlotState::Valid;
    }
    return slot;
} // InspectLibraryVideo

/**
    Builds the checked retained or named-library header after complete verification.

    The common 64-byte media header is extended with an item name, photo thumbnail
    or video duration setting. The full extension has its own CRC and remains
    unpublished until the closed staging file is renamed to its final name.

    @param header Writable shared scratch for the full selected header length.
*/
void
BuildLibraryPublicationHeader (
    uint8_t* header
    )
{
    std::memset(header, 0, managerState.save.headerBytes);
    BuildVideoStorageHeader(header);
    if (!managerState.save.libraryId && !managerState.save.videoId) {
        return;
    }
    if (managerState.save.libraryId) {
        header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_LIBRARY_RAW_PHOTO;
        if (managerState.save.metadata.pixelFormat == PixelFormat::JPEG) {
            header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_LIBRARY_JPEG_PHOTO;
        }
    } else {
        header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_LIBRARY_RAW_VIDEO;
        if (managerState.save.metadata.pixelFormat == PixelFormat::JPEG) {
            header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_LIBRARY_JPEG_VIDEO;
        }
    }
    MediaMetadataCodec::WriteUnsigned16LittleEndian(header + STORAGE_HEADER_LENGTH_OFFSET, managerState.save.headerBytes);
    WriteUnsigned32LittleEndian(header + VIDEO_DATA_OFFSET_FIELD, managerState.save.headerBytes);
    WriteUnsigned32LittleEndian(header + VIDEO_HEADER_CRC_OFFSET,
                               ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, VIDEO_HEADER_CRC_OFFSET));
    std::memcpy(header + LIBRARY_NAME_OFFSET, managerState.save.name, sizeof(managerState.save.name));
    if (managerState.save.libraryId) {
        std::memcpy(header + LIBRARY_EXTRA_DATA_OFFSET, managerState.save.thumbnail, sizeof(managerState.save.thumbnail));
    } else {
        WriteUnsigned32LittleEndian(header + LIBRARY_EXTRA_DATA_OFFSET, managerState.save.maxSecondsMillis);
    }
    WriteUnsigned32LittleEndian(header + managerState.save.headerBytes - FRAME_CHECKSUM_BYTES,
                               ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header,
                                                managerState.save.headerBytes - FRAME_CHECKSUM_BYTES));
} // BuildLibraryPublicationHeader

/**
    Samples decoded image fragments into the small RGB565 library preview.

    Samples the centers of logical preview cells from incoming raw or decoded JPEG fragments and
    writes RGB565 values into the small caller-owned preview. Source offsets and placement
    determine the destination, so out-of-order MCU rows and letterboxed media are handled
    consistently. Pixels outside sampled cells are ignored, and the enclosing operation
    initializes the remaining preview to black.

    @param opaque Caller-owned preview sampling state.
    @param bytes Read-only input byte buffer.
    @param count Number of bytes to process.
    @param offset Byte offset within the source image or file.

    @return true - The pixel chunk was sampled into the preview buffer.
    @return false - The requested pixel format or range is invalid.
*/
bool
PreviewSamplingContext::SamplePreviewPixels (
    void* opaque,
    const uint8_t* bytes,
    size_t count,
    size_t offset
    )
{
    PreviewSamplingContext* preview = nullptr;
    size_t i;
    size_t pixel;
    size_t x;
    size_t y;
    uint16_t rgb;

    preview = &(*static_cast<PreviewSamplingContext*>(opaque));
    if (count % preview->pixelBytes || offset % preview->pixelBytes ||
        offset + count > size_t(preview->width) * preview->height * preview->pixelBytes) {
        return false;
    }
    // Double each dimension of the old preview: sample 4x4 cell centers.
    // Logical offsets also handle JPEG's out-of-order MCU row fragments.
    for (i = 0; i < count; i += preview->pixelBytes) {
        pixel = (offset + i) / preview->pixelBytes;
        x = pixel % preview->width + preview->left;
        y = pixel / preview->width + preview->top;
        if (x % LIBRARY_PREVIEW_SAMPLE_STEP != LIBRARY_PREVIEW_SAMPLE_CENTER ||
            y % LIBRARY_PREVIEW_SAMPLE_STEP != LIBRARY_PREVIEW_SAMPLE_CENTER) {
            continue;
        }
        if (preview->pixelBytes == RGB565_PIXEL_BYTES) {
            rgb = MediaMetadataCodec::ReadUnsigned16LittleEndian(bytes + i);
        } else {
            rgb = uint16_t((bytes[i] >> RGB565_RED_BLUE_REDUCE_SHIFT) << RGB565_RED_SHIFT |
                           (bytes[i + 1] >> RGB565_GREEN_REDUCE_SHIFT) << RGB565_GREEN_SHIFT |
                           bytes[i + RGB_PIXEL_BLUE_INDEX] >> RGB565_RED_BLUE_REDUCE_SHIFT);
        }
        MediaMetadataCodec::WriteUnsigned16LittleEndian(
            preview->pixels +
                ((y / LIBRARY_PREVIEW_SAMPLE_STEP) * MemoryManager::PreviewWidth + x / LIBRARY_PREVIEW_SAMPLE_STEP) *
                    RGB565_PIXEL_BYTES,
            rgb);
    }

    return true;
} // SamplePreviewPixels

/**
    Adds managed library files to logical and allocated storage totals.

    @param usage Running storage totals to update.
    @param clusterBytes Number of bytes allocated for one filesystem cluster.

    @return FR_OK - All managed directory entries were counted.
    @return other - FatFs error describing the failed directory operation.
*/
int
MeasureLibraryStorageUsage (
    MemoryManager::StorageUsage& usage,
    uint64_t clusterBytes
    )
{
    return ScanLibraryDirectory([&]([[maybe_unused]] uint32_t id, [[maybe_unused]] bool video, [[maybe_unused]] bool temporary, const FILINFO& info) {
        usage.fileBytes += info.fsize;
        usage.allocatedBytes += (uint64_t(info.fsize) + clusterBytes - 1) / clusterBytes * clusterBytes;
        return true;
    });
} // MeasureLibraryStorageUsage

} // namespace MemoryStorage
