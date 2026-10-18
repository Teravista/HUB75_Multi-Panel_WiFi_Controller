#include "sd/include/MemoryManagerClass.hpp"
#include "sd/include/memoryManagerPlatform.hpp"
#include "config/defines.h"
#include "ff.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

/**
    Holds the single storage session's filesystem, file and bounded read buffer.

    Static class-owned storage preserves one SD hardware owner and avoids placing
    FatFs objects or the 6 KiB scratch allocation on the small foreground stack.
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

    Accumulates the reflected IEEE CRC32 state one byte and bit at a time, allowing the same
    state to continue across streamed chunks. The caller supplies the initial state and applies
    final inversion only when the whole payload is complete.

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
    size_t index;
    unsigned bit;

    // Process each byte least-significant bit first using the reflected IEEE CRC32 polynomial.
    for (index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (bit = 0; bit < BITS_PER_BYTE; ++bit) {
            if (crc & 1) {
                crc = (crc >> 1) ^ CRC32_REFLECTED_POLYNOMIAL_U;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
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

    Creates a new file with exclusive-create semantics, builds the fixed photo header and writes
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
    uint8_t header[HeaderBytes];
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
    BuildPhotoStorageHeader(header, ~UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, pixels, bytes));
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
    Streams a named RGB888 photo and validates its dimensions and checksum.

    Validates the exact header and file length before streaming bounded RGB888 fragments into
    the borrowed sink. A running payload CRC is checked after all pixels are consumed, and file-
    close errors also prevent success. Earlier sink data is provisional: the caller must publish
    an image only after the whole operation succeeds. Reentrant storage access is rejected by
    the held guard.

    @param name Filename in the card root; directory separators and invalid names are rejected.
    @param sink Callback that consumes pixel chunks; false cancels the current frame.
    @param context Caller-owned state passed unchanged to the callback.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::ReadImage (
    const char* name,
    ImageSink sink,
    void* context
    )
{
    char path[MaximumNameLength + STORAGE_PATH_PREFIX_AND_TERMINATOR_BYTES];
    FRESULT result;
    uint8_t header[HeaderBytes];
    uint8_t expected[HeaderBytes];
    UINT count;
    bool complete;
    uint32_t expectedCrc;
    const char* failure = nullptr;
    uint32_t crc;
    size_t offset;
    UINT bytes;
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

    if (!sink) {
        return ReportStorageFailure("missing image consumer");
    }

    // Open the saved file before validating its header and reading pixels.
    result = f_open(&storage_.imageFile, path, FA_READ);
    if (result != FR_OK) {
        return ReportStorageFailure("image open failed", result);
    }

    // Validate the file header and declared size before accepting pixel data.
    std::memset(header, 0, sizeof(header));
    std::memset(expected, 0, sizeof(expected));
    count = 0;
    result = f_read(&storage_.imageFile, header, sizeof(header), &count);
    complete = result == FR_OK && count == sizeof(header) &&
                    f_size(&storage_.imageFile) == SOURCE_FRAME_SIZE_BYTES + HeaderBytes;
    expectedCrc = ReadUnsigned32LittleEndian(header + PHOTO_HEADER_CHECKSUM_OFFSET);
    BuildPhotoStorageHeader(expected, expectedCrc);
    complete = complete && !std::memcmp(header, expected, sizeof(header));
    failure = "invalid stored image header or size";
    crc = STORAGE_CRC32_INITIAL_U;

    // Read bounded chunks and verify their checksum; delivered pixels remain provisional.
    for (offset = 0; complete && offset < SOURCE_FRAME_SIZE_BYTES;) {
        bytes = UINT(std::min(ChunkBytes, SOURCE_FRAME_SIZE_BYTES - offset));
        result = f_read(&storage_.imageFile, storage_.readBuffer, bytes, &count);
        complete = result == FR_OK && count == bytes;
        failure = "image data read failed";
        if (complete) {
            crc = UpdateImageCrc32(crc, storage_.readBuffer, bytes);
            complete = sink(context, storage_.readBuffer, bytes, offset);
            if (!complete) {
                failure = "image consumer rejected data";
            }
        }
        offset += count;
    }

    // Accept the payload only when its complete CRC matches the stored checksum.
    if (complete && ~crc != expectedCrc) {
        complete = false;
        failure = "stored image checksum mismatch";
    }

    // Close the file and preserve any transfer or close error before reporting success.
    closeResult = f_close(&storage_.imageFile);
    if (closeResult != FR_OK) {
        result = closeResult;
        failure = "image close failed";
        complete = false;
    }
    if (!complete) {
        return ReportStorageFailure(failure, result);
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
