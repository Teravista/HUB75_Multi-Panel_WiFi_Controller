#include "sd/include/MemoryManagerClass.hpp"
#include "sd/include/memoryManagerPlatform.hpp"
#include "sd/include/storageCrc32.hpp"
#include "config/defines.h"
#include "ff.h"
#include "media/jpeg/include/jpegFrameDecoder.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

/**
    Holds the single storage session's filesystem, file and bounded read buffer.

    Static class-owned storage preserves one SD hardware owner and avoids placing
    FatFs objects or the 24 KiB scratch allocation on the small foreground stack.
*/
struct MemoryManager::StorageState {
    FATFS filesystem{};
    FIL imageFile{};
    FIL mediaFile{};
    FIL playbackFile{};
    SaveSession save;
    PlaybackSession playback;
    JpegIndexEntry jpegIndex[JPEG_INDEX_CACHE_ENTRIES];
    uint32_t jpegIndexFirst = 0;
    uint32_t jpegIndexCount = 0;
    int rejectedSlot = SAVED_PHOTO_NO_SLOT;
    alignas(DMA_BUFFER_ALIGNMENT) uint8_t readBuffer[ChunkBytes];
    std::atomic_flag storageBusy = ATOMIC_FLAG_INIT;
};

MemoryManager::StorageState MemoryManager::storage_{};

/**
    Attempts to claim the shared storage state without waiting.

    Reentrant callbacks or concurrent operations receive a busy result rather
    than waiting on the same filesystem handle and scratch buffer.
*/
MemoryManager::StorageGuard::StorageGuard (
    void
    ) : acquired(!storage_.storageBusy.test_and_set(std::memory_order_acquire))
{
    return;
} // StorageGuard

/**
    Releases the storage-operation guard when its scope ends.
*/
MemoryManager::StorageGuard::~StorageGuard (
    void
    )
{
    if (acquired) {
        storage_.storageBusy.clear(std::memory_order_release);
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
MemoryManager::BuildValidatedImagePath (
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
MemoryManager::UpdateImageCrc32 (
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
MemoryManager::WriteUnsigned32LittleEndian (
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
MemoryManager::ReadUnsigned32LittleEndian (
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
MemoryManager::BuildPhotoStorageHeader (
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
MemoryManager::CompareSavedImageBytes (
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
    Records a storage error and invalidates readiness after failed SD I/O.

    Retains the supplied diagnostic and clears mounted readiness after disk/not-ready failures
    so a later explicit initialization can retry storage. Invalid arguments or content checks
    can report failure without treating the card as disconnected. The baseline API reports
    availability but does not schedule automatic remounts.

    @param message Human-readable result or error message.
    @param filesystemResult FatFs result code; zero means no filesystem error.

    @return false - Always reports failure after storing the diagnostic and invalidating storage
                    when required.
*/
bool
MemoryManager::ReportStorageFailure (
    const char* message,
    int filesystemResult
    )
{
    error_ = message;
    if (filesystemResult == FR_DISK_ERR || filesystemResult == FR_NOT_READY) {
        ready_ = false;
    }
    std::printf("[memory] %s (FatFs=%d)\n", message, filesystemResult);
    return false;
} // ReportStorageFailure

/**
    Mounts the SD card for image storage.

    Claims the shared storage guard without waiting, prepares the dedicated SPI/card resources
    and immediately asks FatFs to mount the existing filesystem. Hardware initialization and
    mounting run synchronously in the foreground. Failure releases partially claimed hardware;
    an already ready manager returns without remounting. The operation never formats the card.

    @return true - Storage initialization succeeded, including an already initialized device.
    @return false - Storage is busy or hardware/filesystem initialization failed.
*/
bool
MemoryManager::Initialize (
    void
    )
{
    FRESULT result;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    StorageGuard guard;

    if (!guard.acquired) {
        return ReportStorageFailure("storage busy");
    }
    if (ready_) {
        error_ = "none";
        return true;
    }
    if (!InitializeMemoryStorageHardware()) {
        ReleaseMemoryStorageHardware();
        return ReportStorageFailure("SD driver initialization failed");
    }

    // Register the existing filesystem; mounting does not format or replace card contents.
    result = f_mount(&storage_.filesystem, "0:", 1);

    if (result != FR_OK) {
        f_mount(nullptr, "0:", 0);
        ReleaseMemoryStorageHardware();
        if (result == FR_NO_FILESYSTEM) {
            return ReportStorageFailure("card has no supported FAT/exFAT filesystem", result);
        }
        return ReportStorageFailure("SD card mount failed", result);
    }
    ready_ = true;

    error_ = "none";
    std::printf("[memory] SD card mounted; SPI clock=%u Hz\n", GetMemoryStorageClockHz());
    return true;
} // Initialize

/**
    Unmounts storage and releases its SPI and DMA resources.

    Claims storage ownership before unregistering the filesystem and releasing only the SD
    reader's SPI/DMA resources. Readiness is cleared even if unmounting reports an error. A busy
    guard returns immediately rather than waiting inside a callback or concurrent storage
    operation.

    @return true - Storage handles and hardware resources were released.
    @return false - Storage is busy and cannot be shut down safely.
*/
bool
MemoryManager::Shutdown (
    void
    )
{
    FRESULT result;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    StorageGuard guard;

    if (!guard.acquired) {
        return ReportStorageFailure("storage busy");
    }

    DiscardIncompleteMediaSave(ready_);
    CloseSavedMediaPlayback();

    // Register the existing filesystem; mounting does not format or replace card contents.
    result = f_mount(nullptr, "0:", 0);
    ready_ = false;
    ReleaseMemoryStorageHardware();

    if (result != FR_OK) {
        return ReportStorageFailure("SD unmount failed", result);
    }

    error_ = "none";
    return true;
} // Shutdown

/**
    Reports whether mounted SD storage is available for use.

    @return true - The card is mounted and storage is available.
    @return false - Storage is not currently ready.
*/
bool
MemoryManager::IsReady (
    void
    ) const
{
    return ready_;
} // IsReady

/**
    Returns the most recent storage operation error.

    @return text - Borrowed null-terminated result text; do not free it.
*/
const char*
MemoryManager::GetLastError (
    void
    ) const
{
    return error_;
} // GetLastError

/**
    Writes a new named photo without replacing an existing file.

    Creates a new file with exclusive-create semantics, builds the descriptor-bearing photo header and writes
    the caller's stable RGB888 image in bounded chunks. The full payload checksum is computed
    before writing, and successful completion includes closing the file. A failed or short write
    triggers cleanup instead of intentionally replacing an existing image. This is a synchronous
    foreground call.

    @param name Filename in the card root; directory separators and invalid names are rejected.
    @param pixels Read-only image pixel data; keep it unchanged until the operation returns.
    @param bytes Number of bytes to process.

    @return true - The new RGB888 file was written and closed successfully.
    @return false - Input is invalid, the name exists, storage is unavailable or file I/O
                    failed; GetLastError() gives the reason.
*/
bool
MemoryManager::SaveImage (
    const char* name,
    const uint8_t* pixels,
    size_t bytes
    )
{
    char path[MaximumNameLength + STORAGE_PATH_PREFIX_AND_TERMINATOR_BYTES];
    FRESULT result;
    uint8_t header[LastHeaderBytes];
    MediaMetadata metadata;
    UINT written;
    bool complete;
    size_t offset;
    UINT count;
    FRESULT closeResult;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    StorageGuard guard;

    if (!guard.acquired) {
        return ReportStorageFailure("storage busy");
    }
    if (storage_.save.phase != SavePhase::Idle) {
        return ReportStorageFailure("media save in progress");
    }
    if (!ready_) {
        return ReportStorageFailure("storage not ready");
    }

    // Validate the root filename and image inputs before opening a file.
    if (!BuildValidatedImagePath(name, path)) {
        return ReportStorageFailure("invalid image filename");
    }
    if (!pixels || bytes != SOURCE_FRAME_SIZE_BYTES) {
        return ReportStorageFailure("invalid image size");
    }

    // Create a new file; an existing image must never be truncated.
    result = f_open(&storage_.imageFile, path, FA_WRITE | FA_CREATE_NEW);
    if (result == FR_EXIST) {
        return ReportStorageFailure("image already exists", result);
    }
    if (result != FR_OK) {
        return ReportStorageFailure("image open for write failed", result);
    }

    // Build the on-card header, including dimensions, payload size and checksum.
    std::memset(header, 0, sizeof(header));
    metadata = MediaMetadata::Photo(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT);
    BuildSavedPhotoHeader(header, metadata, 0,
                          ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, pixels, bytes));
    written = 0;
    result = f_write(&storage_.imageFile, header, sizeof(header), &written);
    complete = result == FR_OK && written == sizeof(header);

    // Write bounded pixel chunks and stop immediately after a short or failed write.
    for (offset = 0; complete && offset < bytes;) {
        count = UINT(std::min(ChunkBytes, bytes - offset));
        result = f_write(&storage_.imageFile, pixels + offset, count, &written);
        complete = result == FR_OK && written == count;
        offset += written;
    }

    // Close the file and preserve any transfer or close error before reporting success.
    closeResult = f_close(&storage_.imageFile);
    if (!complete || closeResult != FR_OK) {
        f_unlink(path);
        if (result == FR_OK) {
            result = closeResult;
        }
        return ReportStorageFailure("image write failed or card full", result);
    }

    error_ = "none";
    return true;
} // SaveImage

/**
    Streams a named RGB888 photo and validates its metadata and checksum.

    Supports legacy named/retained headers and the descriptor-bearing v3 format.
    The shared reader checks the header and exact photo dimensions before pixels
    reach the synchronous sink. Fragments remain provisional until payload CRC and
    close succeed; the optional metadata output is published only after success.

    @param name Valid root filename to read.
    @param sink Synchronous RGB888 pixel consumer.
    @param context Borrowed consumer state for the duration of the read.
    @param metadata Optional descriptor output; unchanged on failure.

    @return true - The complete photo and its metadata passed verification.
    @return false - Input, storage, header, consumer, checksum or close failed.
*/
bool
MemoryManager::ReadImage (
    const char* name,
    ImageSink sink,
    void* context,
    MediaMetadata* metadata
    )
{
    char path[MaximumNameLength + STORAGE_PATH_PREFIX_AND_TERMINATOR_BYTES];
    SavedPhotoSlot loaded;

    StorageGuard guard;

    if (!guard.acquired) {
        return ReportStorageFailure("storage busy");
    }
    if (storage_.save.phase != SavePhase::Idle) {
        return ReportStorageFailure("media save in progress");
    }
    if (!ready_) {
        return ReportStorageFailure("storage not ready");
    }
    if (!BuildValidatedImagePath(name, path)) {
        return ReportStorageFailure("invalid image filename");
    }
    if (!sink) {
        return ReportStorageFailure("missing image consumer");
    }

    // Deliver provisional pixels under the storage guard; publish metadata last.
    loaded = ReadAndValidatePhotoFile(path, sink, context);
    if (loaded.state != SlotState::Valid) {
        return ReportStorageFailure(loaded.error, loaded.result);
    }
    if (metadata) {
        *metadata = loaded.metadata;
    }
    error_ = "none";
    return true;
} // ReadImage

/**
    Saves, rereads, and removes an unused temporary test image.

    Chooses an unused temporary filename, saves the supplied image and rereads every byte
    through a comparing sink. The test file is removed after the read attempt, while a pre-
    existing test filename is left untouched. The original image must remain unchanged during
    save and comparison; no additional full framebuffer is allocated.

    @param pixels Read-only image pixel data; keep it unchanged until the operation returns.
    @param bytes Number of bytes to process.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::RunReadWriteCheck (
    const uint8_t* pixels,
    size_t bytes
    )
{
    char name[SD_SELF_TEST_NAME_BYTES];
    char path[MaximumNameLength + STORAGE_PATH_PREFIX_AND_TERMINATOR_BYTES];
    bool available;
    unsigned candidate;
    FRESULT result;
    CompareContext check;
    bool verified;
    FRESULT cleanupResult;

    if (!ready_) {
        return ReportStorageFailure("storage not ready");
    }

    if (!pixels || bytes != SOURCE_FRAME_SIZE_BYTES) {
        return ReportStorageFailure("invalid self-test image");
    }
    // Choose an unused name; never replace a pre-existing test or user file.
    available = false;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    {
        StorageGuard guard;
        if (!guard.acquired) {
            return ReportStorageFailure("storage busy");
        }
        for (candidate = 0; candidate < SD_SELF_TEST_FILENAME_ATTEMPTS; ++candidate) {
            std::snprintf(name, sizeof(name), "H75T%03u.RGB", candidate);
            BuildValidatedImagePath(name, path);
            result = f_stat(path, nullptr);
            if (result == FR_NO_FILE) {
                available = true;
                break;
            }
            if (result != FR_OK) {
                return ReportStorageFailure("self-test filename lookup failed", result);
            }
        }
    }
    if (!available) {
        return ReportStorageFailure("no unused self-test filename");
    }
    std::printf("[memory] self-test: save/read %u-byte image\n", unsigned(bytes));
    if (!SaveImage(name, pixels, bytes)) {
        return false;
    }
    check = {pixels};
    verified = ReadImage(name, CompareSavedImageBytes, &check);
    {
        StorageGuard guard;
        if (!guard.acquired) {
            return ReportStorageFailure("storage busy during test cleanup");
        }
        cleanupResult = f_unlink(path);
    }
    if (cleanupResult != FR_OK) {
        return ReportStorageFailure("self-test file cleanup failed", cleanupResult);
    }
    if (!verified || check.compared != bytes) {
        if (check.mismatch) {
            return ReportStorageFailure("self-test readback differs from saved image");
        }
        return false;
    }

    error_ = "none";
    std::printf("[memory] self-test PASS: saved and read %u bytes; test file removed\n", unsigned(bytes));
    return true;
} // RunReadWriteCheck

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
MemoryManager::BuildSavedPhotoHeader (
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
MemoryManager::SavedPhotoSlot
MemoryManager::ReadAndValidatePhotoFile (
    const char* path,
    ImageSink sink,
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
    slot.result = f_open(&storage_.imageFile, path, FA_READ);
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
    slot.result = f_read(&storage_.imageFile, header, STORAGE_HEADER_PREFIX_BYTES, &count);
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
        complete = f_size(&storage_.imageFile) == SOURCE_FRAME_SIZE_BYTES + headerBytes;
    }
    if (complete) {
        slot.result = f_read(&storage_.imageFile, header + STORAGE_HEADER_PREFIX_BYTES,
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
        slot.result = f_read(&storage_.imageFile, storage_.readBuffer, bytes, &count);
        slot.error = "image data read failed";
        complete = slot.result == FR_OK && count == bytes;
        if (complete) {
            crc = UpdateImageCrc32(crc, storage_.readBuffer, bytes);
            if (sink && !sink(context, storage_.readBuffer, bytes, offset)) {
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
    closeResult = f_close(&storage_.imageFile);

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
MemoryManager::SavedPhotoSlot
MemoryManager::ReadSavedPhotoSlot (
    unsigned index,
    ImageSink sink,
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
MemoryManager::FindNewestSavedPhoto (
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
MemoryManager::WriteSavedPhotoSlot (
    unsigned index,
    uint32_t generation,
    const MediaMetadata& metadata,
    ImageSource source,
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
    status = f_open(&storage_.imageFile, path, FA_WRITE | FA_CREATE_ALWAYS);
    result = status;
    if (status != FR_OK) {
        return false;
    }

    // Reserve the header and stream stable source chunks while accumulating the payload CRC.
    BuildSavedPhotoHeader(header, metadata, generation, 0);
    crc = STORAGE_CRC32_INITIAL_U;
    count = 0;
    status = f_write(&storage_.imageFile, header, sizeof(header), &count);
    good = status == FR_OK && count == sizeof(header);
    for (offset = 0; good && offset < SOURCE_FRAME_SIZE_BYTES;) {
        bytes = UINT(std::min(ChunkBytes, SOURCE_FRAME_SIZE_BYTES - offset));
        good = source(context, storage_.readBuffer, bytes, offset);
        if (!good) {
            break;
        }
        crc = UpdateImageCrc32(crc, storage_.readBuffer, bytes);
        status = f_write(&storage_.imageFile, storage_.readBuffer, bytes, &count);
        good = status == FR_OK && count == bytes;
        offset += count;
    }

    // Install the final payload checksum only after every source chunk was written.
    if (good) {
        BuildSavedPhotoHeader(header, metadata, generation, ~crc);
        status = f_lseek(&storage_.imageFile, 0);
        if (status == FR_OK) {
            status = f_write(&storage_.imageFile, header, sizeof(header), &count);
        }
        good = status == FR_OK && count == sizeof(header);
    }

    // Avoid additional filesystem cleanup writes after communication with the card has failed.
    if (status == FR_DISK_ERR || status == FR_NOT_READY) {
        storage_.imageFile = {};
        result = status;
        return false;
    }

    // Close to flush the completed file; remove an incomplete target only while storage is usable.
    closed = f_close(&storage_.imageFile);
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
    Saves a retained RGB888 photo with the default full-panel descriptor.

    Existing callers keep the raw-photo API. The metadata-aware overload performs
    all validation, locking, alternate-slot selection and readback verification.

    @param source Stable logical RGB888 pixel producer.
    @param context Borrowed source state valid until the save completes.

    @return true - A new photo generation was saved and verified.
    @return false - Availability, input, I/O or verification prevented the save.
*/
bool
MemoryManager::SaveLastImage (
    ImageSource source,
    void* context
    )
{
    return SaveLastImage(MediaMetadata::Photo(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT),
                         source, context);
} // SaveLastImage

/**
    Saves a new retained photo while keeping the previous valid generation.

    Inspects both retained slots before choosing the alternate destination. Source
    pixels are streamed synchronously and the completed file is reread before the
    save is reported successful. A source callback must not re-enter storage, and
    its image must stay stable throughout the call. No extra full frame is allocated.

    @param metadata Exact-panel RGB888 photo descriptor to retain with the pixels.
    @param source Stable logical RGB888 pixel producer.
    @param context Borrowed source state valid until this call completes.

    @return true - A new generation was written and verified.
    @return false - Storage is unavailable/busy, source data failed or validation failed.
*/
bool
MemoryManager::SaveLastImage (
    const MediaMetadata& metadata,
    ImageSource source,
    void* context
    )
{
    SavedPhotoSlot slots[SAVED_PHOTO_SLOT_COUNT];
    SavedPhotoSlot verified;
    int latest;
    unsigned target;
    uint32_t generation;
    int result;
    size_t index;

    // Claim storage and reject unsupported input before inspecting or changing retained files.
    StorageGuard guard;

    if (!guard.acquired || !ready_ || !source) {
        return ReportStorageFailure("photo storage busy/unavailable or missing source");
    }
    if (storage_.save.phase != SavePhase::Idle) {
        return ReportStorageFailure("media save in progress");
    }
    if (metadata.pixelFormat != PixelFormat::RGB888 ||
        !metadata.IsPhotoFor(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)) {
        return ReportStorageFailure("unsupported photo metadata");
    }

    // Inspect both generations so the destination can preserve the newest usable content.
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = InspectSavedMediaSlot(unsigned(index));
        if (slots[index].state == SlotState::IoError) {
            return ReportStorageFailure(slots[index].error, slots[index].result);
        }
    }

    // Close any old playback handle and select the replacement generation and alternate slot.
    CloseSavedMediaPlayback();
    latest = FindNewestSavedPhoto(slots);
    if (latest == 0) {
        target = 1;
    } else {
        target = 0;
    }
    generation = 1;
    if (latest >= 0) {
        generation = slots[latest].generation + 1;
    }

    // Write the new slot, then independently reread it before reporting a successful save.
    result = FR_OK;
    if (!WriteSavedPhotoSlot(target, generation, metadata, source, context, result)) {
        return ReportStorageFailure("saved photo write failed", result);
    }
    verified = ReadSavedPhotoSlot(target);
    if (verified.state != SlotState::Valid || verified.generation != generation) {
        return ReportStorageFailure("saved photo verification failed", verified.result);
    }
    error_ = "none";
    return true;
} // SaveLastImage

/**
    Restores the newest verified retained photo through a chunk sink.

    Header and payload validation determines the newest usable generation first,
    so a damaged replacement can fall back to the other retained copy. A final
    synchronous read delivers its provisional pixels and verifies them again.
    Callers publish the frame only after this method succeeds.

    @param sink Synchronous consumer of logical RGB888 fragments.
    @param context Borrowed consumer state valid until this call completes.
    @param metadata Optional descriptor output; unchanged unless the read succeeds.

    @return true - The selected retained photo was streamed and verified.
    @return false - No valid copy exists or availability, callback or I/O failed.
*/
bool
MemoryManager::ReadLastImage (
    ImageSink sink,
    void* context,
    MediaMetadata* metadata
    )
{
    SavedPhotoSlot slots[SAVED_PHOTO_SLOT_COUNT];
    SavedPhotoSlot loaded;
    size_t index;
    int latest;

    StorageGuard guard;

    if (!guard.acquired || !ready_ || !sink) {
        return ReportStorageFailure("photo storage busy/unavailable or missing sink");
    }
    if (storage_.save.phase != SavePhase::Idle) {
        return ReportStorageFailure("media save in progress");
    }
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = InspectSavedMediaSlot(unsigned(index));
        if (slots[index].state == SlotState::IoError) {
            return ReportStorageFailure(slots[index].error, slots[index].result);
        }
    }
    latest = FindNewestSavedPhoto(slots);
    if (latest < 0) {
        return ReportStorageFailure("no valid saved photo");
    }
    if (slots[latest].metadata.type != MediaType::Photo) {
        return ReportStorageFailure("newest saved media is video");
    }
    loaded = ReadSavedPhotoSlot(unsigned(latest), sink, context);
    if (loaded.state != SlotState::Valid) {
        return ReportStorageFailure(loaded.error, loaded.result);
    }
    if (metadata) {
        *metadata = loaded.metadata;
    }
    error_ = "none";
    return true;
} // ReadLastImage

/**
    Returns the reserved path for one alternating media slot.

    @param index Alternating slot index, zero or one.

    @return path - Borrowed constant path; callers never free it.
*/
const char*
MemoryManager::GetSavedMediaPath (
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
MemoryManager::FlushMediaWrites (
    int& result
    )
{
    UINT count;

    result = FR_OK;
    if (!storage_.save.bufferedBytes) {
        return true;
    }
    result = f_write(&storage_.mediaFile, storage_.readBuffer, UINT(storage_.save.bufferedBytes), &count);
    if (result != FR_OK || count != storage_.save.bufferedBytes) {
        return false;
    }
    storage_.save.bufferedBytes = 0;
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
MemoryManager::QueueMediaWrite (
    const uint8_t* bytes,
    size_t count,
    int& result
    )
{
    size_t part;

    result = FR_OK;
    while (count) {
        part = std::min(count, ChunkBytes - storage_.save.bufferedBytes);
        std::memcpy(storage_.readBuffer + storage_.save.bufferedBytes, bytes, part);
        storage_.save.bufferedBytes += part;
        bytes += part;
        count -= part;
        if (storage_.save.bufferedBytes == ChunkBytes && !FlushMediaWrites(result)) {
            return false;
        }
    }
    return true;
} // QueueMediaWrite

/**
    Closes playback without acquiring a second storage guard.

    Callers already hold storage ownership. Clearing the description prevents a
    closed file or old generation from being reused by the player.
*/
void
MemoryManager::CloseSavedMediaPlayback (
    void
    )
{
    if (storage_.playback.open) {
        f_close(&storage_.playbackFile);
    }
    storage_.playbackFile = {};
    storage_.playback = {};
    storage_.jpegIndexCount = 0;
} // CloseSavedMediaPlayback

/**
    Discards an unpublished save without reacquiring storage ownership.

    Only an unpublished target is removed. On lost storage, handles are discarded
    without further writes so cleanup cannot modify a disconnected/replaced card.

    @param safe Whether the current SD session remains safe for cleanup writes.
*/
void
MemoryManager::DiscardIncompleteMediaSave (
    bool safe
    )
{
    if (storage_.save.phase == SavePhase::Idle) {
        return;
    }
    if (safe) {
        if (storage_.save.open) {
            f_close(&storage_.mediaFile);
        }
        f_unlink(GetSavedMediaPath(storage_.save.target));
    }
    storage_.mediaFile = {};
    storage_.save = {};
} // DiscardIncompleteMediaSave

/**
    Builds the checked raw or indexed-JPEG video header for the current save.

    The final header includes frame stride, generation, descriptor and expected
    payload checksum. It is installed only after complete SD readback succeeds.

    @param header Output buffer containing FileHeaderBytes writable bytes.
*/
void
MemoryManager::BuildVideoStorageHeader (
    uint8_t* header
    )
{
    std::memset(header, 0, MediaTransfer::FileHeaderBytes);
    std::memcpy(header, "H75L", MEDIA_MAGIC_BYTES);
    header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_RAW_VIDEO;
    if (storage_.save.metadata.pixelFormat == PixelFormat::JPEG) {
        header[STORAGE_HEADER_VERSION_OFFSET] = STORAGE_VERSION_JPEG_VIDEO;
    }
    MediaMetadataCodec::WriteUnsigned16LittleEndian(header + STORAGE_HEADER_LENGTH_OFFSET, MediaTransfer::FileHeaderBytes);
    WriteUnsigned32LittleEndian(header + STORAGE_HEADER_GENERATION_OFFSET, storage_.save.generation);
    MediaMetadataCodec::EncodeMediaDescriptor(storage_.save.metadata, header + STORAGE_HEADER_METADATA_OFFSET,
                                            MediaMetadataCodec::HeaderBytes);
    WriteUnsigned32LittleEndian(header + VIDEO_FRAME_BYTES_OFFSET, uint32_t(storage_.save.metadata.FrameBytes()));
    if (storage_.save.metadata.pixelFormat == PixelFormat::JPEG) {
        WriteUnsigned32LittleEndian(header + VIDEO_FRAME_STRIDE_OFFSET, MediaTransfer::JpegIndexBytes);
    } else {
        WriteUnsigned32LittleEndian(header + VIDEO_FRAME_STRIDE_OFFSET,
                                   uint32_t(storage_.save.metadata.FrameBytes()) + FRAME_CHECKSUM_BYTES);
    }
    WriteUnsigned32LittleEndian(header + VIDEO_DATA_OFFSET_FIELD, MediaTransfer::FileHeaderBytes);
    WriteUnsigned32LittleEndian(header + VIDEO_PAYLOAD_CRC_OFFSET, storage_.save.expectedCrc);
    WriteUnsigned32LittleEndian(header + VIDEO_HEADER_CRC_OFFSET,
                               ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, VIDEO_HEADER_CRC_OFFSET));
} // BuildVideoStorageHeader

/**
    Streams and checks a raw frame from the inspection or playback handle.

    Seeks to the selected record only when needed, then streams whole-pixel chunks.
    A short prefix reaches a sector boundary before bulk reads. Both legacy photo
    CRCs and raw video per-frame CRCs are checked before success; consumers must
    not publish provisional pixels until the complete operation returns.

    @param slot Validated photo/video file description.
    @param index Zero-based frame to read.
    @param sink Optional synchronous consumer of provisional RGB888 fragments.
    @param context Borrowed consumer state.
    @param inspecting Selects the inspection file instead of the active playback file.

    @return result - Completion flag, filesystem status and validation diagnostic.
*/
MemoryManager::FrameResult
MemoryManager::ReadAndVerifyRawVideoFrame (
    const SavedPhotoSlot& slot,
    uint32_t index,
    ImageSink sink,
    void* context,
    bool inspecting
    )
{
    FrameResult read;
    FIL* file = nullptr;
    uint64_t position;
    uint64_t stride;
    uint32_t crc;
    uint32_t expected;
    UINT count;
    UINT bytes;
    size_t offset;
    size_t chunk;
    uint8_t checksum[FRAME_CHECKSUM_BYTES];

    // Choose the owned file handle and the checksum layout used by this saved generation.
    file = &storage_.playbackFile;
    if (inspecting) {
        file = &storage_.imageFile;
    }
    stride = slot.metadata.FrameBytes();
    crc = slot.crcSeed;
    if (slot.frameChecksums) {
        stride += FRAME_CHECKSUM_BYTES;
        crc = STORAGE_CRC32_INITIAL_U;
    }

    // Seek only when the selected frame is not already next in the file stream.
    position = slot.dataOffset + stride * index;
    if (f_tell(file) != position) {
        read.result = f_lseek(file, FSIZE_t(position));
    }

    // Read bounded whole-pixel fragments; use a short prefix to reach a sector boundary.
    for (offset = 0; read.result == FR_OK && offset < slot.metadata.FrameBytes();) {
        chunk = ChunkBytes;
        if (f_tell(file) % SD_SECTOR_BYTES) {
            chunk = SD_SECTOR_BYTES - size_t(f_tell(file) % SD_SECTOR_BYTES);
            while (chunk % slot.metadata.PixelBytes()) {
                chunk += SD_SECTOR_BYTES;
            }
        }
        bytes = UINT(std::min(uint64_t(chunk), slot.metadata.FrameBytes() - offset));
        read.result = f_read(file, storage_.readBuffer, bytes, &count);
        if (read.result != FR_OK || count != bytes) {
            return read;
        }
        crc = UpdateImageCrc32(crc, storage_.readBuffer, bytes);
        if (sink && !sink(context, storage_.readBuffer, bytes, offset)) {
            read.error = "media frame consumer rejected data";
            return read;
        }
        offset += bytes;
    }
    if (read.result != FR_OK) {
        return read;
    }

    // Accept provisional pixels only when the frame checksum matches its stored record.
    expected = slot.checksum;
    if (slot.frameChecksums) {
        read.result = f_read(file, checksum, sizeof(checksum), &count);
        if (read.result != FR_OK || count != sizeof(checksum)) {
            return read;
        }
        expected = ReadUnsigned32LittleEndian(checksum);
    }
    read.error = "media frame checksum mismatch";
    read.complete = ~crc == expected;
    return read;
} // ReadAndVerifyRawVideoFrame

/**
    Inspects a photo, raw video or indexed JPEG video in a retained slot.

    Legacy photos retain complete validation. Video headers must match supported
    dimensions, stride and exact file size; their first frame is checked before
    the slot is eligible. Later frames are checked as they are read, avoiding a
    whole-video scan at boot. A previously failed playback slot is excluded.

    @param index Alternating slot to inspect.

    @return slot - Validity, metadata, generation and any I/O/verification failure.
*/
MemoryManager::SavedPhotoSlot
MemoryManager::InspectSavedMediaSlot (
    unsigned index
    )
{
    SavedPhotoSlot slot;
    uint8_t header[MediaTransfer::FileHeaderBytes];
    UINT count;
    FRESULT closed;
    bool valid;
    FrameResult first;
    bool jpeg;
    uint64_t expectedSize;
    uint32_t expectedStride;

    // Classify missing and inaccessible slots before attempting to parse their headers.
    slot.result = f_open(&storage_.imageFile, GetSavedMediaPath(index), FA_READ);
    if (slot.result == FR_NO_FILE) {
        return slot;
    }
    if (slot.result != FR_OK) {
        slot.state = SlotState::IoError;
        return slot;
    }

    // Recognize older photo containers and delegate their complete validation to the photo reader.
    std::memset(header, 0, sizeof(header));
    slot.result = f_read(&storage_.imageFile, header, STORAGE_HEADER_PREFIX_BYTES, &count);
    if (slot.result == FR_OK && count == STORAGE_HEADER_PREFIX_BYTES &&
        (!std::memcmp(header, "H75I", MEDIA_MAGIC_BYTES) ||
         (!std::memcmp(header, "H75L", MEDIA_MAGIC_BYTES) &&
          header[STORAGE_HEADER_VERSION_OFFSET] != STORAGE_VERSION_RAW_VIDEO &&
          header[STORAGE_HEADER_VERSION_OFFSET] != STORAGE_VERSION_JPEG_VIDEO))) {
        closed = f_close(&storage_.imageFile);
        if (closed != FR_OK) {
            slot.state = SlotState::IoError;
            slot.result = closed;
            return slot;
        }
        return ReadSavedPhotoSlot(index);
    }

    // Validate the video header checksum and descriptor before trusting any offsets or lengths.
    slot.state = SlotState::Invalid;
    valid = slot.result == FR_OK && count == STORAGE_HEADER_PREFIX_BYTES;
    if (valid) {
        slot.result = f_read(&storage_.imageFile, header + STORAGE_HEADER_PREFIX_BYTES,
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
        valid = uint64_t(f_size(&storage_.imageFile)) == expectedSize &&
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
        if (jpeg) {
            first = ReadAndVerifyJpegFrame(slot, 0, nullptr, nullptr, true);
        } else {
            first = ReadAndVerifyRawVideoFrame(slot, 0, nullptr, nullptr, true);
        }
        valid = first.complete;
        slot.result = first.result;
        slot.error = first.error;
    }

    // Publish a usable candidate only after closing succeeds and no playback rejection excludes it.
    closed = f_close(&storage_.imageFile);
    if (slot.result != FR_OK || closed != FR_OK) {
        slot.state = SlotState::IoError;
        if (slot.result == FR_OK) {
            slot.result = closed;
        }
    } else if (valid && int(index) != storage_.rejectedSlot) {
        slot.state = SlotState::Valid;
    }
    return slot;
} // InspectSavedMediaSlot

/**
    Reports whether a video save owns the shared storage buffer.

    @return true - A write or readback-verification transaction is active.
    @return false - No video transaction is active.
*/
bool
MemoryManager::IsMediaSaveActive (
    void
    ) const
{
    return storage_.save.phase != SavePhase::Idle;
} // IsMediaSaveActive

/**
    Starts an unpublished raw or indexed-JPEG video save while retaining the previous valid slot.

    Closes playback, inspects both generations and chooses the alternate target.
    An invalid staging header prevents booting partial content. Incoming bytes are
    written incrementally; only successful readback can install the final header.

    @param metadata Validated raw or indexed-JPEG video description.
    @param payloadChecksum Expected CRC32 of the incoming pixel payload.

    @return true - The unpublished transaction is ready for payload appends.
    @return false - Storage is busy, metadata is unsupported or file creation failed.
*/
bool
MemoryManager::BeginMediaSave (
    const MediaMetadata& metadata,
    uint32_t payloadChecksum
    )
{
    SavedPhotoSlot slots[SAVED_PHOTO_SLOT_COUNT];
    size_t index;
    int latest;
    FRESULT status;
    int result;
    uint8_t staged[MediaTransfer::FileHeaderBytes];
    UINT count;

    StorageGuard guard;

    if (!guard.acquired || !ready_ || storage_.save.phase != SavePhase::Idle) {
        return ReportStorageFailure("video storage busy/unavailable");
    }
    if (!MediaTransfer::IsSupportedVideo(metadata, LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)) {
        return ReportStorageFailure("unsupported video metadata");
    }

    // Release playback and inspect both retained generations before choosing a replacement slot.
    CloseSavedMediaPlayback();
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = InspectSavedMediaSlot(unsigned(index));
        if (slots[index].state == SlotState::IoError) {
            return ReportStorageFailure("saved media lookup failed", slots[index].result);
        }
    }

    // Initialize transaction counters while keeping the preceding generation available.
    latest = FindNewestSavedPhoto(slots);
    storage_.save = {};
    storage_.save.metadata = metadata;
    storage_.save.expectedCrc = payloadChecksum;
    metadata.PayloadBytes(storage_.save.total);
    storage_.save.generation = 1;
    if (latest == 0) {
        storage_.save.target = 1;
    }
    if (latest >= 0) {
        storage_.save.generation = slots[latest].generation + 1;
    }

    // Create an invalid staging header; startup must never restore a partially received clip.
    status = f_open(&storage_.mediaFile, GetSavedMediaPath(storage_.save.target), FA_WRITE | FA_CREATE_ALWAYS);
    if (status != FR_OK) {
        return ReportStorageFailure("video create failed", status);
    }
    storage_.save.phase = SavePhase::Writing;
    storage_.save.open = true;
    std::memset(staged, 0, sizeof(staged));
    std::memcpy(staged, "H75P", MEDIA_MAGIC_BYTES);
    result = f_write(&storage_.mediaFile, staged, sizeof(staged), &count);
    if (result != FR_OK || count != sizeof(staged)) {
        DiscardIncompleteMediaSave(result != FR_DISK_ERR && result != FR_NOT_READY);
        return ReportStorageFailure("video staging failed", result);
    }
    error_ = "none";
    return true;
} // BeginMediaSave

/**
    Adds received video bytes to the bounded storage write buffer.

    Updates whole-payload and per-frame checksums while inserting each completed
    frame CRC into its stored record. Successful appends do not publish the clip;
    callers must finish reception and complete readback verification.

    @param bytes Next ordered pixel payload fragment.
    @param count Number of incoming payload bytes.

    @return true - The fragment was accepted and queued or written.
    @return false - The transaction, range or file write is invalid.
*/
bool
MemoryManager::AppendMediaBytes (
    const uint8_t* bytes,
    size_t count
    )
{
    size_t part;
    int result;
    uint8_t checksum[FRAME_CHECKSUM_BYTES];

    StorageGuard guard;

    if (!guard.acquired || !ready_ || storage_.save.phase != SavePhase::Writing) {
        return ReportStorageFailure("no writable video transaction");
    }
    if (!bytes || !count || count > storage_.save.total - storage_.save.received) {
        return ReportStorageFailure("invalid video chunk");
    }

    // JPEG already carries its index and per-frame CRC records, so store its payload unchanged.
    if (storage_.save.metadata.pixelFormat == PixelFormat::JPEG) {
        if (!QueueMediaWrite(bytes, count, result)) {
            return ReportStorageFailure("JPEG video write failed", result);
        }
        storage_.save.crc = UpdateImageCrc32(storage_.save.crc, bytes, count);
        storage_.save.received += count;
        error_ = "none";
        return true;
    }

    // Split raw payload fragments at frame boundaries so each completed frame gets its own CRC record.
    while (count) {
        part = std::min(count, size_t(storage_.save.metadata.FrameBytes() - storage_.save.frameBytes));
        if (!QueueMediaWrite(bytes, part, result)) {
            return ReportStorageFailure("video write failed", result);
        }
        storage_.save.crc = UpdateImageCrc32(storage_.save.crc, bytes, part);
        storage_.save.frameCrc = UpdateImageCrc32(storage_.save.frameCrc, bytes, part);
        storage_.save.frameBytes += part;
        storage_.save.received += part;
        bytes += part;
        count -= part;

        // Append the raw frame checksum and reset only the per-frame running state.
        if (storage_.save.frameBytes == storage_.save.metadata.FrameBytes()) {
            WriteUnsigned32LittleEndian(checksum, ~storage_.save.frameCrc);
            if (!QueueMediaWrite(checksum, sizeof(checksum), result)) {
                return ReportStorageFailure("video frame CRC write failed", result);
            }
            storage_.save.frameBytes = 0;
            storage_.save.frameCrc = STORAGE_CRC32_INITIAL_U;
        }
    }
    error_ = "none";
    return true;
} // AppendMediaBytes

/**
    Flushes the completed upload and begins incremental readback verification.

    Requires the exact payload and transfer CRC before closing the write handle.
    The same unpublished file is reopened for bounded readback. Verification must
    finish before playback or a SAVED acknowledgement can expose the new clip.

    @return true - The transaction entered readback verification.
    @return false - Reception, CRC, flush, close or reopening failed.
*/
bool
MemoryManager::StartMediaVerification (
    void
    )
{
    int result;

    StorageGuard guard;

    if (!guard.acquired || !ready_ || storage_.save.phase != SavePhase::Writing ||
        storage_.save.received != storage_.save.total || storage_.save.frameBytes) {
        return ReportStorageFailure("incomplete video upload");
    }

    // Require the complete ordered upload and its transfer checksum before starting readback.
    if (~storage_.save.crc != storage_.save.expectedCrc) {
        return ReportStorageFailure("uploaded video checksum mismatch");
    }
    if (!FlushMediaWrites(result)) {
        return ReportStorageFailure("video final flush failed", result);
    }

    // Flush and reopen the unpublished file so verification reads actual saved data.
    result = f_close(&storage_.mediaFile);
    storage_.save.open = false;
    if (result != FR_OK) {
        return ReportStorageFailure("video close failed", result);
    }
    result = f_open(&storage_.mediaFile, GetSavedMediaPath(storage_.save.target), FA_READ | FA_WRITE);
    if (result != FR_OK) {
        return ReportStorageFailure("video verification open failed", result);
    }
    storage_.save.open = true;
    result = f_lseek(&storage_.mediaFile, MediaTransfer::FileHeaderBytes);
    if (result != FR_OK) {
        return ReportStorageFailure("video verification seek failed", result);
    }
    storage_.save.phase = SavePhase::Verifying;
    error_ = "none";
    return true;
} // StartMediaVerification

/**
    Verifies the next saved segment and publishes only after complete success.

    Reads one bounded payload segment and checks frame records as their boundaries
    are reached. The final whole-payload CRC must also match. Only then is the
    checked header installed and the file closed, preserving the older slot until
    the replacement has passed every readback check.

    @param complete Set true only when verification and publication have finished.

    @return true - This verification step succeeded; inspect complete for publication.
    @return false - The transaction, readback, checksum or publication failed.
*/
bool
MemoryManager::VerifyMediaSaveStep (
    bool& complete
    )
{
    UINT bytes;
    UINT count;
    int result;
    uint8_t checksum[FRAME_CHECKSUM_BYTES];
    uint8_t header[MediaTransfer::FileHeaderBytes];
    bool jpegFinished;

    complete = false;
    StorageGuard guard;

    if (!guard.acquired || !ready_ || storage_.save.phase != SavePhase::Verifying) {
        return ReportStorageFailure("no video verification transaction");
    }

    // Advance format-specific readback while preserving the invalid staging header.
    if (storage_.save.metadata.pixelFormat == PixelFormat::JPEG) {
        if (!VerifyJpegSaveStep(jpegFinished)) {
            return false;
        }
        if (!jpegFinished) {
            return true;
        }
    } else {

        // Bound raw reads by the scratch buffer, payload end and next frame-checksum boundary.
        bytes = UINT(std::min({uint64_t(ChunkBytes), storage_.save.total - storage_.save.verified,
                              storage_.save.metadata.FrameBytes() - storage_.save.verifyFrameBytes}));
        result = f_read(&storage_.mediaFile, storage_.readBuffer, bytes, &count);
        if (result != FR_OK || count != bytes) {
            return ReportStorageFailure("video verification read failed", result);
        }
        storage_.save.verifyCrc = UpdateImageCrc32(storage_.save.verifyCrc, storage_.readBuffer, bytes);
        storage_.save.verifyFrameCrc = UpdateImageCrc32(storage_.save.verifyFrameCrc, storage_.readBuffer, bytes);
        storage_.save.verified += bytes;
        storage_.save.verifyFrameBytes += bytes;
        if (storage_.save.verifyFrameBytes == storage_.save.metadata.FrameBytes()) {
            result = f_read(&storage_.mediaFile, checksum, sizeof(checksum), &count);
            if (result != FR_OK || count != sizeof(checksum)) {
                return ReportStorageFailure("video verification CRC read failed", result);
            }
            if (ReadUnsigned32LittleEndian(checksum) != ~storage_.save.verifyFrameCrc) {
                return ReportStorageFailure("stored video frame checksum mismatch");
            }
            storage_.save.verifyFrameBytes = 0;
            storage_.save.verifyFrameCrc = STORAGE_CRC32_INITIAL_U;
        }
    }

    // Publish the checked header only after all payload and frame checks have passed.
    if (storage_.save.verified == storage_.save.total) {
        if (~storage_.save.verifyCrc != storage_.save.expectedCrc) {
            return ReportStorageFailure("stored video checksum mismatch");
        }
        BuildVideoStorageHeader(header);
        result = f_lseek(&storage_.mediaFile, 0);
        if (result == FR_OK) {
            result = f_write(&storage_.mediaFile, header, sizeof(header), &count);
        }
        if (result != FR_OK || count != sizeof(header)) {
            return ReportStorageFailure("video publication failed", result);
        }
        result = f_close(&storage_.mediaFile);
        storage_.save.open = false;
        if (result != FR_OK) {
            return ReportStorageFailure("video publication close failed", result);
        }
        storage_.save = {};
        storage_.rejectedSlot = SAVED_PHOTO_NO_SLOT;
        complete = true;
        std::puts("[memory] video saved and verified");
    }
    error_ = "none";
    return true;
} // VerifyMediaSaveStep

/**
    Cancels an unpublished video save and releases its temporary resources.

    Cleanup runs under storage ownership. It removes only the unfinished target
    when the card remains usable, otherwise discarding handles without more writes.
*/
void
MemoryManager::AbortMediaSave (
    void
    )
{
    StorageGuard guard;

    if (!guard.acquired) {
        ReportStorageFailure("storage busy");
        return;
    }
    DiscardIncompleteMediaSave(ready_);
} // AbortMediaSave

/**
    Opens the newest valid photo or video for checked frame playback.

    Inspects both generations, excluding a slot rejected after a frame error.
    Photos receive full validation; videos validate their checked header and first
    frame here and verify remaining frames during playback. No full clip is loaded.

    @param metadata Receives the selected media description on success.

    @return true - A valid generation was opened for playback.
    @return false - Storage is busy, unavailable or has no usable saved generation.
*/
bool
MemoryManager::OpenLastMedia (
    MediaMetadata& metadata
    )
{
    SavedPhotoSlot slots[SAVED_PHOTO_SLOT_COUNT];
    size_t index;
    int latest;
    int result;

    StorageGuard guard;

    if (!guard.acquired || !ready_ || storage_.save.phase != SavePhase::Idle) {
        return ReportStorageFailure("media playback busy/unavailable");
    }
    CloseSavedMediaPlayback();
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = InspectSavedMediaSlot(unsigned(index));
        if (slots[index].state == SlotState::IoError) {
            return ReportStorageFailure("saved media lookup failed", slots[index].result);
        }
    }
    latest = FindNewestSavedPhoto(slots);
    if (latest < 0) {
        return ReportStorageFailure("no valid saved media");
    }
    result = f_open(&storage_.playbackFile, GetSavedMediaPath(unsigned(latest)), FA_READ);
    if (result != FR_OK) {
        return ReportStorageFailure("media playback open failed", result);
    }
    storage_.playback.open = true;
    storage_.playback.slot = latest;
    storage_.playback.description = slots[latest];
    metadata = slots[latest].metadata;
    error_ = "none";
    return true;
} // OpenLastMedia

/**
    Reads and verifies one frame from the selected saved media.

    A failed read or frame CRC closes playback and rejects that generation for
    the next open, allowing the player to try the previous valid saved content.
    Only the complete verified frame may be published to the display.

    @param frameIndex Zero-based frame within the selected saved clip.
    @param sink Synchronous consumer of provisional frame bytes.
    @param context Borrowed consumer state for this read.

    @return true - The requested frame and its CRC passed verification.
    @return false - The request, I/O, consumer or checksum failed.
*/
bool
MemoryManager::ReadMediaFrame (
    uint32_t frameIndex,
    ImageSink sink,
    void* context
    )
{
    FrameResult read;

    StorageGuard guard;

    if (!guard.acquired || !ready_ || !storage_.playback.open || storage_.save.phase != SavePhase::Idle) {
        return ReportStorageFailure("media playback not open");
    }
    if (!sink || frameIndex >= storage_.playback.description.metadata.frameCount) {
        return ReportStorageFailure("invalid media frame request");
    }
    if (storage_.playback.description.indexedJpeg) {
        read = ReadAndVerifyJpegFrame(storage_.playback.description, frameIndex, sink, context, false);
    } else {
        read = ReadAndVerifyRawVideoFrame(storage_.playback.description, frameIndex, sink, context, false);
    }
    if (!read.complete) {
        storage_.rejectedSlot = storage_.playback.slot;
        CloseSavedMediaPlayback();
        return ReportStorageFailure(read.error, read.result);
    }
    error_ = "none";
    return true;
} // ReadMediaFrame

/**
    Closes the active playback file and clears its runtime state.
*/
void
MemoryManager::CloseMediaPlayback (
    void
    )
{
    StorageGuard guard;

    if (!guard.acquired) {
        ReportStorageFailure("storage busy");
        return;
    }
    CloseSavedMediaPlayback();
} // CloseMediaPlayback

/**
    Supplies bounded compressed bytes and checksums their exact record data.

    The first half of shared storage scratch is a refill buffer. Even skipped
    bytes update the encoded CRC and final-marker history; no callback can read
    outside the selected record. A failed or short read stops the decoder.

    @param context Borrowed bounded file reader for this JPEG record.
    @param bytes Writable destination, or null when the decoder skips input.
    @param count Maximum bytes requested by the decoder.

    @return count - Actual bytes copied or skipped, possibly fewer than requested.
    @return 0 - The record ended or a filesystem read failed.
*/
size_t
MemoryManager::ReadJpegInput (
    void* context,
    uint8_t* bytes,
    size_t count
    )
{
    JpegReadContext* reader = nullptr;
    FIL* file = nullptr;
    size_t copied;
    size_t part;
    UINT actual;

    reader = static_cast<JpegReadContext*>(context);
    file = static_cast<FIL*>(reader->file);
    copied = 0;

    // Refill only within this encoded record; the other half of scratch belongs to the decoder.
    while (copied < count) {
        if (reader->cursor == reader->buffered) {
            if (!reader->remaining || reader->result != FR_OK) {
                break;
            }
            part = std::min(reader->remaining, size_t(JPEG_INPUT_BUFFER_BYTES));
            reader->result = f_read(file, storage_.readBuffer, UINT(part), &actual);
            if (reader->result != FR_OK || actual != part) {
                break;
            }
            reader->remaining -= part;
            reader->buffered = part;
            reader->cursor = 0;
        }

        // Track every consumed or skipped byte so CRC and final-marker checks cover the entire record.
        part = std::min(count - copied, reader->buffered - reader->cursor);
        reader->crc = UpdateImageCrc32(reader->crc, storage_.readBuffer + reader->cursor, part);
        if (part >= JPEG_MARKER_BYTES) {
            reader->previousByte = storage_.readBuffer[reader->cursor + part - JPEG_MARKER_BYTES];
        } else {
            reader->previousByte = reader->lastByte;
        }
        reader->lastByte = storage_.readBuffer[reader->cursor + part - 1];
        if (bytes) {
            std::memcpy(bytes + copied, storage_.readBuffer + reader->cursor, part);
        }
        reader->cursor += part;
        copied += part;
    }
    return copied;
} // ReadJpegInput

/**
    Validates an indexed JPEG record before completing provisional frame output.

    Checks index CRC, payload bounds, record length, and adjacent-record continuity
    before decoding. The same bounded memory handles startup, random/reverse
    playback and incremental save validation. Full decoding plus encoded CRC and
    end-marker checks must all succeed before a caller publishes the frame.

    @param slot Checked JPEG slot metadata and payload offset.
    @param index Zero-based independently seekable frame.
    @param sink Optional synchronous consumer of provisional RGB888 rows.
    @param context Borrowed pixel-consumer state.
    @param inspecting Selects the startup inspection handle.
    @param verifying Selects the active unpublished save handle instead.

    @return result - Completion, filesystem status and a stable failure description.
*/
MemoryManager::FrameResult
MemoryManager::ReadAndVerifyJpegFrame (
    const SavedPhotoSlot& slot,
    uint32_t index,
    ImageSink sink,
    void* context,
    bool inspecting,
    bool verifying
    )
{
    FrameResult read;
    FIL* file = nullptr;
    uint8_t entry[MediaTransfer::JpegIndexBytes];
    uint8_t record[MediaTransfer::JpegRecordBytes];
    UINT count;
    uint32_t offset;
    uint32_t length;
    uint32_t expectedCrc;
    uint32_t nextOffset;
    uint64_t end;
    JpegReadContext input;
    int decoderError;
    bool decoded;

    // Choose inspection, playback or verification ownership without opening another file handle.
    read.error = "invalid indexed JPEG frame";
    file = &storage_.playbackFile;
    if (inspecting) {
        file = &storage_.imageFile;
    }
    if (verifying) {
        file = &storage_.mediaFile;
    }

    // Playback uses a checked index window; inspection and verification read independent index records.
    if (!inspecting && !verifying) {
        if (!LoadJpegIndexWindow(slot, index, read.result)) {
            return read;
        }
        offset = storage_.jpegIndex[index - storage_.jpegIndexFirst].offset;
        length = storage_.jpegIndex[index - storage_.jpegIndexFirst].length;
    } else {
        read.result = f_lseek(file, FSIZE_t(slot.dataOffset + uint64_t(index) * MediaTransfer::JpegIndexBytes));
        if (read.result == FR_OK) {
            read.result = f_read(file, entry, sizeof(entry), &count);
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
            read.result = f_read(file, entry, sizeof(entry), &count);
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
    if (f_tell(file) != slot.dataOffset + offset) {
        read.result = f_lseek(file, FSIZE_t(slot.dataOffset + offset));
    }
    if (read.result == FR_OK) {
        read.result = f_read(file, record, sizeof(record), &count);
    }
    if (read.result != FR_OK || count != sizeof(record) || ReadUnsigned32LittleEndian(record) != length) {
        return read;
    }
    expectedCrc = ReadUnsigned32LittleEndian(record + FRAME_CHECKSUM_BYTES);

    // Decode into provisional pixel callbacks using the shared input and workspace halves.
    input.file = file;
    input.remaining = length;
    decoded = JpegFrameDecoder::Decode(ReadJpegInput, &input, sink, context,
                                      slot.metadata.width, slot.metadata.height,
                                      storage_.readBuffer + JPEG_INPUT_BUFFER_BYTES,
                                      JPEG_WORK_BUFFER_BYTES, decoderError);
    if (!decoded) {
        read.result = input.result;
        read.error = "invalid/unsupported JPEG frame";
        return read;
    }
    // The decoder can stop before consuming trailing markers; drain within this record only.
    while (ReadJpegInput(&input, nullptr, JPEG_INPUT_BUFFER_BYTES)) {
    }
    read.result = input.result;
    read.complete = read.result == FR_OK && !input.remaining && input.cursor == input.buffered &&
                    ~input.crc == expectedCrc && input.previousByte == JPEG_MARKER_PREFIX &&
                    input.lastByte == JPEG_END_MARKER;
    return read;
} // ReadAndVerifyJpegFrame

/**
    Checks one bounded JPEG payload block or one complete encoded frame.

    First streams the complete saved payload to validate the transfer CRC. Later
    calls validate one independently decodable frame at a time, including its index
    and record checksums. This keeps the staging header invalid until every frame
    has passed and leaves the previous retained generation untouched.

    @param finished True only after the payload CRC and every indexed frame pass.

    @return true - This verification step passed; finished reports full completion.
    @return false - Readback, payload checksum, indexing or JPEG decoding failed.
*/
bool
MemoryManager::VerifyJpegSaveStep (
    bool& finished
    )
{
    UINT bytes;
    UINT count;
    int result;
    SavedPhotoSlot slot;
    FrameResult frame;

    finished = false;

    // First verify the exact saved payload in bounded reads without decoding pixels.
    if (storage_.save.verified < storage_.save.total) {
        bytes = UINT(std::min(uint64_t(ChunkBytes), storage_.save.total - storage_.save.verified));
        result = f_read(&storage_.mediaFile, storage_.readBuffer, bytes, &count);
        if (result != FR_OK || count != bytes) {
            return ReportStorageFailure("JPEG payload verification read failed", result);
        }
        storage_.save.verifyCrc = UpdateImageCrc32(storage_.save.verifyCrc, storage_.readBuffer, bytes);
        storage_.save.verified += bytes;
        if (storage_.save.verified == storage_.save.total &&
            ~storage_.save.verifyCrc != storage_.save.expectedCrc) {
            return ReportStorageFailure("stored JPEG payload checksum mismatch");
        }
        return true;
    }

    // After payload CRC succeeds, check one indexed frame per call before allowing publication.
    slot.metadata = storage_.save.metadata;
    slot.dataOffset = MediaTransfer::FileHeaderBytes;
    slot.indexedJpeg = true;
    frame = ReadAndVerifyJpegFrame(slot, storage_.save.jpegVerifiedFrames, nullptr, nullptr, false, true);
    if (!frame.complete) {
        return ReportStorageFailure(frame.error, frame.result);
    }
    ++storage_.save.jpegVerifiedFrames;
    finished = storage_.save.jpegVerifiedFrames == storage_.save.metadata.frameCount;
    return true;
} // VerifyJpegSaveStep


/**
    Loads and checks a bounded index window for JPEG playback.

    Caches offset/length pairs for at most 512 frames in four KiB. Every entry
    CRC, bound and local continuity check passes before the window is published.
    A preceding entry validates the window boundary, and the final entry must end
    exactly at the payload boundary. Cache ownership ends whenever playback closes.

    @param slot Checked JPEG metadata for the active playback handle.
    @param index Frame whose index window is required.
    @param result Receives filesystem status; stays zero for invalid index content.

    @return true - The selected entry belongs to the checked active window.
    @return false - Index readback, CRC, bounds or record continuity failed.
*/
bool
MemoryManager::LoadJpegIndexWindow (
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
    if (storage_.jpegIndexCount && index >= storage_.jpegIndexFirst &&
        index - storage_.jpegIndexFirst < storage_.jpegIndexCount) {
        return true;
    }

    // Invalidate the old window before loading a replacement and include its preceding boundary entry.
    storage_.jpegIndexCount = 0;
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
    result = f_lseek(&storage_.playbackFile, FSIZE_t(slot.dataOffset + uint64_t(readFirst) * MediaTransfer::JpegIndexBytes));
    if (result == FR_OK) {
        result = f_read(&storage_.playbackFile, storage_.readBuffer, bytes, &count);
    }
    if (result != FR_OK || count != bytes) {
        return false;
    }

    // Validate index CRCs, frame bounds and continuity, including the window and payload boundaries.
    previousEnd = 0;
    for (entryIndex = 0; entryIndex < entries + prefix; ++entryIndex) {
        frame = readFirst + entryIndex;
        encoded = storage_.readBuffer + entryIndex * MediaTransfer::JpegIndexBytes;
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
            storage_.jpegIndex[entryIndex - prefix] = {offset, length};
        }
    }

    // Expose the replacement window only after every loaded entry has passed validation.
    storage_.jpegIndexFirst = first;
    storage_.jpegIndexCount = entries;
    return true;
} // LoadJpegIndexWindow
