#include "sd/include/MemoryManagerClass.hpp"
#include "sd/include/memoryManagerPlatform.hpp"
#include "sd/include/storageCrc32.hpp"
#include "config/defines.h"
#include "ff.h"
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
    if (good) {
        BuildSavedPhotoHeader(header, metadata, generation, ~crc);
        status = f_lseek(&storage_.imageFile, 0);
        if (status == FR_OK) {
            status = f_write(&storage_.imageFile, header, sizeof(header), &count);
        }
        good = status == FR_OK && count == sizeof(header);
    }
    if (status == FR_DISK_ERR || status == FR_NOT_READY) {
        storage_.imageFile = {};
        result = status;
        return false;
    }
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

    StorageGuard guard;
    if (!guard.acquired || !ready_ || !source) {
        return ReportStorageFailure("photo storage busy/unavailable or missing source");
    }
    if (metadata.pixelFormat != PixelFormat::RGB888 ||
        !metadata.IsPhotoFor(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)) {
        return ReportStorageFailure("unsupported photo metadata");
    }
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = ReadSavedPhotoSlot(unsigned(index));
        if (slots[index].state == SlotState::IoError) {
            return ReportStorageFailure(slots[index].error, slots[index].result);
        }
    }
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
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = ReadSavedPhotoSlot(unsigned(index));
        if (slots[index].state == SlotState::IoError) {
            return ReportStorageFailure(slots[index].error, slots[index].result);
        }
    }
    latest = FindNewestSavedPhoto(slots);
    if (latest < 0) {
        return ReportStorageFailure("no valid saved photo");
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
