#include "sd/include/memoryStorage.hpp"
#include "sd/include/storageCrc32.hpp"
#include "pico/time.h"

#define SD_DMA_OVERLAP_WORK_BYTES 510
#define JPEG_END_OF_IMAGE_MARKER 0xffd9
#define JPEG_MARKER_BYTES 2
#define JPEG_PIPELINE_PRIME_BYTES 4096

namespace MemoryStorage {
alignas(DMA_BUFFER_ALIGNMENT) uint8_t readBuffer[ChunkBytes];
PlaybackPrefetch playbackPrefetch;
FIL* playbackFile = nullptr;
#if JPEG_PARALLEL_READER
JpegInputPipe jpegPipe;
#endif

/**
    Updates a running storage checksum with the next byte span.

    @param crc Running encoded-frame CRC32 state.
    @param bytes Bytes to include in the checksum.
    @param length Number of bytes to include.

    @return value - Updated CRC32 state before final inversion.
*/
static uint32_t
UpdateImageCrc32 (
    uint32_t crc,
    const uint8_t* bytes,
    size_t length
    )
{
    return StorageCrc32::UpdateCrc32Checksum(crc, bytes, length);
} // UpdateImageCrc32

/**
    Reads a stored 32-bit checksum in little-endian byte order.

    @param bytes Four encoded bytes containing the checksum.

    @return value - Decoded unsigned checksum.
*/
static uint32_t
ReadUnsigned32LittleEndian (
    const uint8_t* bytes
    )
{
    return MediaMetadataCodec::ReadUnsigned32LittleEndian(bytes);
} // ReadUnsigned32LittleEndian

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
    bool outputRGB565
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

    result = {};
    decoderError = 0;
    usedCore1 = false;
    decodeUs = waitUs = 0;
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

} // namespace MemoryStorage
