#include "sd/include/MemoryManagerClass.hpp"
#include "sd/include/memoryStorage.hpp"
#include "pico/time.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#define FAT_RESERVED_CLUSTER_COUNT 2
#define SD_SECTOR_BYTES_U 512u

/**
    Records a storage error and requests recovery for failed SD I/O.

    Preserve the diagnostic for API callers, and invalidate the session only for serious
    filesystem I/O failures. Expected busy/unavailable conditions do not print repeatedly;
    changed errors are reported immediately and identical errors have a 15-second quiet period.

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
    if (MemoryStorage::IsStorageIoError(filesystemResult)) {
        ready_ = false;
        storageError_ = message;
        if (state_ == State::Ready) {
            state_ = State::Recovering;
        }
    }
    if (!SerialLog::RoutineStorageError(message, filesystemResult) &&
        errorLog_.Allow(message, filesystemResult, to_ms_since_boot(get_absolute_time()))) {
        std::printf("[sd] error: %s (code=%d)\n", message, filesystemResult);
    }
    return false;
} // ReportStorageFailure

/**
    Enables storage monitoring and attempts to mount the SD card.

    Claims the non-waiting storage guard, enables monitoring and reuses an already ready mount
    when possible. A recovering session is discarded before SD hardware initialization and an
    immediate FatFs mount attempt. These driver/filesystem calls are synchronous and can take
    time; successful mounting starts a new generation for cache invalidation and health checks.

    Any initialization or mount failure releases partial resources, records the cause and
    schedules the unavailable-state retry deadline. Mounting uses existing card contents and never
    formats the card.

    @return true - Storage initialization succeeded, including an already initialized device.
    @return false - Storage is busy or hardware/filesystem initialization failed.
*/
bool
MemoryManager::Initialize (
    void
    )
{
    FRESULT result;
    const char* mountError = nullptr;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired) {
        return ReportStorageFailure("storage busy");
    }
    suspended_ = false;
    if (ready_) {
        error_ = "none";
        return true;
    }
    if (state_ == State::Recovering) {
        DiscardSession();
    }
    state_ = State::Mounting;
    std::puts("[sd] -> mounting");
    if (!InitializeMemoryStorageHardware()) {
        ReleaseMemoryStorageHardware();
        state_ = State::Unavailable;
        lastAttemptMs_ = to_ms_since_boot(get_absolute_time());
        storageError_ = "SD driver initialization failed";
        return ReportStorageFailure("SD driver initialization failed");
    }

    // Register the existing filesystem; mounting does not format or replace card contents.
    HUB75_DEBUG("[sd] startup: mounting FAT filesystem\n");
    result = f_mount(&MemoryStorage::managerState.filesystem, "0:", 1);
    HUB75_DEBUG("[sd] startup: mount result=%u\n", unsigned(result));

    if (result != FR_OK) {
        f_mount(nullptr, "0:", 0);
        ReleaseMemoryStorageHardware();
        state_ = State::Unavailable;
        lastAttemptMs_ = to_ms_since_boot(get_absolute_time());
        if (result == FR_NO_FILESYSTEM) {
            storageError_ = "unsupported filesystem";
        } else {
            storageError_ = "SD card mount failed";
        }
        mountError = {};
        if (result == FR_NO_FILESYSTEM) {
            mountError = "card has no supported FAT/exFAT filesystem";
        } else {
            mountError = "SD card mount failed";
        }
        return ReportStorageFailure(mountError, result);
    }
    ready_ = true;
    state_ = State::Ready;
    storageError_ = "";
    lastHealthMs_ = to_ms_since_boot(get_absolute_time());
    ++generation_;
    SetMemoryStorageMounted();

    error_ = "none";
    std::printf("[sd] -> ready clock=%u Hz\n", GetMemoryStorageClockHz());
    return true;
} // Initialize

/**
    Closes storage resources and suspends automatic mount retries.

    A healthy session may close and remove its unfinished transaction before releasing
    resources. Failed sessions discard handles without writeback. Automatic retry stays
    suspended until Initialize is explicitly called again. A busy storage guard returns
    immediately rather than waiting inside another operation.

    @return true - Storage handles and hardware resources were released.
    @return false - Storage is busy and cannot be shut down safely.
*/
bool
MemoryManager::Shutdown (
    void
    )
{
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired) {
        return ReportStorageFailure("storage busy");
    }
    if (ready_) {
        MemoryStorage::DiscardIncompleteMediaSave(true);
        MemoryStorage::CloseSavedMediaPlayback();
    }
    DiscardSession();
    suspended_ = true;
    state_ = State::Unavailable;
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
    char path[MemoryStorage::MaximumNameLength + STORAGE_PATH_PREFIX_AND_TERMINATOR_BYTES];
    FRESULT result;
    uint8_t header[MemoryStorage::LastHeaderBytes];
    MediaMetadata metadata;
    UINT written;
    bool complete;
    size_t offset;
    UINT count;
    FRESULT closeResult;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;

    if (!guard.acquired) {
        return ReportStorageFailure("storage busy");
    }
    if (MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("media save in progress");
    }
    if (!ready_) {
        return ReportStorageFailure("storage not ready");
    }

    // Validate the root filename and image inputs before opening a file.
    if (!MemoryStorage::BuildValidatedImagePath(name, path)) {
        return ReportStorageFailure("invalid image filename");
    }
    if (!pixels || bytes != SOURCE_FRAME_SIZE_BYTES) {
        return ReportStorageFailure("invalid image size");
    }

    // Create a new file; an existing image must never be truncated.
    result = f_open(&MemoryStorage::managerState.imageFile, path, FA_WRITE | FA_CREATE_NEW);
    if (result == FR_EXIST) {
        return ReportStorageFailure("image already exists", result);
    }
    if (result != FR_OK) {
        return ReportStorageFailure("image open for write failed", result);
    }

    // Build the on-card header, including dimensions, payload size and checksum.
    std::memset(header, 0, sizeof(header));
    metadata = MediaMetadata::Photo(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT);
    MemoryStorage::BuildSavedPhotoHeader(header, metadata, 0,
                          ~MemoryStorage::UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, pixels, bytes));
    written = 0;
    result = f_write(&MemoryStorage::managerState.imageFile, header, sizeof(header), &written);
    complete = result == FR_OK && written == sizeof(header);

    // Write bounded pixel chunks and stop immediately after a short or failed write.
    for (offset = 0; complete && offset < bytes;) {
        count = UINT(std::min(MemoryStorage::ChunkBytes, bytes - offset));
        result = f_write(&MemoryStorage::managerState.imageFile, pixels + offset, count, &written);
        complete = result == FR_OK && written == count;
        offset += written;
    }

    // Close the file and preserve any transfer or close error before reporting success.
    closeResult = f_close(&MemoryStorage::managerState.imageFile);
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
    char path[MemoryStorage::MaximumNameLength + STORAGE_PATH_PREFIX_AND_TERMINATOR_BYTES];
    MemoryStorage::SavedPhotoSlot loaded;

    MemoryStorage::StorageGuard guard;

    if (!guard.acquired) {
        return ReportStorageFailure("storage busy");
    }
    if (MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("media save in progress");
    }
    if (!ready_) {
        return ReportStorageFailure("storage not ready");
    }
    if (!MemoryStorage::BuildValidatedImagePath(name, path)) {
        return ReportStorageFailure("invalid image filename");
    }
    if (!sink) {
        return ReportStorageFailure("missing image consumer");
    }

    // Deliver provisional pixels under the storage guard; publish metadata last.
    loaded = MemoryStorage::ReadAndValidatePhotoFile(path, sink, context);
    if (loaded.state != MemoryStorage::SlotState::Valid) {
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
    char path[MemoryStorage::MaximumNameLength + STORAGE_PATH_PREFIX_AND_TERMINATOR_BYTES];
    bool available;
    unsigned candidate;
    FRESULT result;
    MemoryStorage::CompareContext check;
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
        MemoryStorage::StorageGuard guard;
        if (!guard.acquired) {
            return ReportStorageFailure("storage busy");
        }
        for (candidate = 0; candidate < SD_SELF_TEST_FILENAME_ATTEMPTS; ++candidate) {
            std::snprintf(name, sizeof(name), "H75T%03u.RGB", candidate);
            MemoryStorage::BuildValidatedImagePath(name, path);
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
    HUB75_DEBUG("[memory] self-test: save/read %u-byte image\n", unsigned(bytes));
    if (!SaveImage(name, pixels, bytes)) {
        return false;
    }
    check = {pixels};
    verified = ReadImage(name, MemoryStorage::CompareSavedImageBytes, &check);
    {
        MemoryStorage::StorageGuard guard;
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
    HUB75_DEBUG("[memory] self-test PASS: saved and read %u bytes; test file removed\n", unsigned(bytes));
    return true;
} // RunReadWriteCheck

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
    MemoryStorage::SavedPhotoSlot slots[SAVED_PHOTO_SLOT_COUNT];
    MemoryStorage::SavedPhotoSlot verified;
    int latest;
    unsigned target;
    uint32_t generation;
    int result;
    size_t index;

    // Claim storage and reject unsupported input before inspecting or changing retained files.
    MemoryStorage::StorageGuard guard;

    if (!guard.acquired || !ready_ || !source) {
        return ReportStorageFailure("photo storage busy/unavailable or missing source");
    }
    if (MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("media save in progress");
    }
    if (metadata.pixelFormat != PixelFormat::RGB888 ||
        !metadata.IsPhotoFor(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)) {
        return ReportStorageFailure("unsupported photo metadata");
    }

    // Inspect both generations so the destination can preserve the newest usable content.
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = MemoryStorage::InspectSavedMediaSlot(unsigned(index));
        if (slots[index].state == MemoryStorage::SlotState::IoError) {
            return ReportStorageFailure(slots[index].error, slots[index].result);
        }
    }

    // Close any old playback handle and select the replacement generation and alternate slot.
    MemoryStorage::CloseSavedMediaPlayback();
    latest = MemoryStorage::FindNewestSavedPhoto(slots);
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
    if (!MemoryStorage::WriteSavedPhotoSlot(target, generation, metadata, source, context, result)) {
        return ReportStorageFailure("saved photo write failed", result);
    }
    verified = MemoryStorage::ReadSavedPhotoSlot(target);
    if (verified.state != MemoryStorage::SlotState::Valid || verified.generation != generation) {
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
    MemoryStorage::SavedPhotoSlot slots[SAVED_PHOTO_SLOT_COUNT];
    MemoryStorage::SavedPhotoSlot loaded;
    size_t index;
    int latest;

    MemoryStorage::StorageGuard guard;

    if (!guard.acquired || !ready_ || !sink) {
        return ReportStorageFailure("photo storage busy/unavailable or missing sink");
    }
    if (MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("media save in progress");
    }
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = MemoryStorage::InspectSavedMediaSlot(unsigned(index));
        if (slots[index].state == MemoryStorage::SlotState::IoError) {
            return ReportStorageFailure(slots[index].error, slots[index].result);
        }
    }
    latest = MemoryStorage::FindNewestSavedPhoto(slots);
    if (latest < 0) {
        return ReportStorageFailure("no valid saved photo");
    }
    if (slots[latest].metadata.type != MediaType::Photo) {
        return ReportStorageFailure("newest saved media is video");
    }
    loaded = MemoryStorage::ReadSavedPhotoSlot(unsigned(latest), sink, context);
    if (loaded.state != MemoryStorage::SlotState::Valid) {
        return ReportStorageFailure(loaded.error, loaded.result);
    }
    if (metadata) {
        *metadata = loaded.metadata;
    }
    error_ = "none";
    return true;
} // ReadLastImage

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
    return MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle;
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
    MemoryStorage::SavedPhotoSlot slots[SAVED_PHOTO_SLOT_COUNT];
    size_t index;
    int latest;
    FRESULT status;
    int result;
    uint8_t staged[MediaTransfer::FileHeaderBytes];
    UINT count;

    MemoryStorage::StorageGuard guard;

    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("video storage busy/unavailable");
    }
    if (!MediaTransfer::IsSupportedVideo(metadata, LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)) {
        return ReportStorageFailure("unsupported video metadata");
    }

    // Release playback and inspect both retained generations before choosing a replacement slot.
    MemoryStorage::CloseSavedMediaPlayback();
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = MemoryStorage::InspectSavedMediaSlot(unsigned(index));
        if (slots[index].state == MemoryStorage::SlotState::IoError) {
            return ReportStorageFailure("saved media lookup failed", slots[index].result);
        }
    }

    // Initialize transaction counters while keeping the preceding generation available.
    latest = MemoryStorage::FindNewestSavedPhoto(slots);
    MemoryStorage::managerState.save = {};
    MemoryStorage::managerState.save.metadata = metadata;
    MemoryStorage::managerState.save.expectedCrc = payloadChecksum;
    metadata.PayloadBytes(MemoryStorage::managerState.save.total);
    MemoryStorage::managerState.save.generation = 1;
    if (latest == 0) {
        MemoryStorage::managerState.save.target = 1;
    }
    if (latest >= 0) {
        MemoryStorage::managerState.save.generation = slots[latest].generation + 1;
    }

    // Create an invalid staging header; startup must never restore a partially received clip.
    status = f_open(&MemoryStorage::managerState.mediaFile, MemoryStorage::GetSavedMediaPath(MemoryStorage::managerState.save.target), FA_WRITE | FA_CREATE_ALWAYS);
    if (status != FR_OK) {
        return ReportStorageFailure("video create failed", status);
    }
    MemoryStorage::managerState.save.phase = MemoryStorage::SavePhase::Writing;
    MemoryStorage::managerState.save.open = true;
    std::memset(staged, 0, sizeof(staged));
    std::memcpy(staged, "H75P", MEDIA_MAGIC_BYTES);
    result = f_write(&MemoryStorage::managerState.mediaFile, staged, sizeof(staged), &count);
    if (result != FR_OK || count != sizeof(staged)) {
        MemoryStorage::DiscardIncompleteMediaSave(!MemoryStorage::IsStorageIoError(result));
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

    MemoryStorage::StorageGuard guard;

    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Writing) {
        return ReportStorageFailure("no writable video transaction");
    }
    if (!bytes || !count || count > MemoryStorage::managerState.save.total - MemoryStorage::managerState.save.received) {
        return ReportStorageFailure("invalid video chunk");
    }

    // JPEG already carries its index and per-frame CRC records, so store its payload unchanged.
    if (MemoryStorage::managerState.save.metadata.pixelFormat == PixelFormat::JPEG) {
        if (!MemoryStorage::QueueMediaWrite(bytes, count, result)) {
            return ReportStorageFailure("JPEG video write failed", result);
        }
        MemoryStorage::managerState.save.crc = MemoryStorage::UpdateImageCrc32(MemoryStorage::managerState.save.crc, bytes, count);
        MemoryStorage::managerState.save.received += count;
        error_ = "none";
        return true;
    }

    // Split raw payload fragments at frame boundaries so each completed frame gets its own CRC record.
    while (count) {
        part = std::min(count, size_t(MemoryStorage::managerState.save.metadata.FrameBytes() - MemoryStorage::managerState.save.frameBytes));
        if (!MemoryStorage::QueueMediaWrite(bytes, part, result)) {
            return ReportStorageFailure("video write failed", result);
        }
        MemoryStorage::managerState.save.crc = MemoryStorage::UpdateImageCrc32(MemoryStorage::managerState.save.crc, bytes, part);
        MemoryStorage::managerState.save.frameCrc = MemoryStorage::UpdateImageCrc32(MemoryStorage::managerState.save.frameCrc, bytes, part);
        MemoryStorage::managerState.save.frameBytes += part;
        MemoryStorage::managerState.save.received += part;
        bytes += part;
        count -= part;

        // Append the raw frame checksum and reset only the per-frame running state.
        if (MemoryStorage::managerState.save.frameBytes == MemoryStorage::managerState.save.metadata.FrameBytes()) {
            MemoryStorage::WriteUnsigned32LittleEndian(checksum, ~MemoryStorage::managerState.save.frameCrc);
            if (!MemoryStorage::QueueMediaWrite(checksum, sizeof(checksum), result)) {
                return ReportStorageFailure("video frame CRC write failed", result);
            }
            MemoryStorage::managerState.save.frameBytes = 0;
            MemoryStorage::managerState.save.frameCrc = STORAGE_CRC32_INITIAL_U;
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
    char path[LIBRARY_PATH_BYTES];

    MemoryStorage::StorageGuard guard;

    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Writing ||
        MemoryStorage::managerState.save.received != MemoryStorage::managerState.save.total || MemoryStorage::managerState.save.frameBytes) {
        return ReportStorageFailure("incomplete video upload");
    }

    // Require the complete ordered upload and its transfer checksum before starting readback.
    if (~MemoryStorage::managerState.save.crc != MemoryStorage::managerState.save.expectedCrc) {
        return ReportStorageFailure("uploaded video checksum mismatch");
    }
    if (!MemoryStorage::FlushMediaWrites(result)) {
        return ReportStorageFailure("video final flush failed", result);
    }

    // Flush and reopen the unpublished file so verification reads actual saved data.
    result = f_close(&MemoryStorage::managerState.mediaFile);
    MemoryStorage::managerState.save.open = false;
    if (result != FR_OK) {
        return ReportStorageFailure("video close failed", result);
    }
    result = f_open(&MemoryStorage::managerState.mediaFile, MemoryStorage::BuildActiveSaveFilePath(path), FA_READ | FA_WRITE);
    if (result != FR_OK) {
        return ReportStorageFailure("video verification open failed", result);
    }
    MemoryStorage::managerState.save.open = true;
    result = f_lseek(&MemoryStorage::managerState.mediaFile, MemoryStorage::managerState.save.headerBytes);
    if (result != FR_OK) {
        return ReportStorageFailure("video verification seek failed", result);
    }
    MemoryStorage::managerState.jpegIndexCount = 0;
    MemoryStorage::managerState.save.phase = MemoryStorage::SavePhase::Verifying;
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
    uint8_t* header = nullptr;
    char stagingPath[LIBRARY_PATH_BYTES];
    char finalPath[LIBRARY_PATH_BYTES];
    bool jpegFinished;
    const char* savedType = nullptr;
    uint32_t savedId;

    complete = false;
    MemoryStorage::StorageGuard guard;

    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Verifying) {
        return ReportStorageFailure("no video verification transaction");
    }

    // Advance format-specific readback while preserving the invalid staging header.
    if (MemoryStorage::managerState.save.metadata.pixelFormat == PixelFormat::JPEG) {
        if (!VerifyJpegSaveStep(jpegFinished)) {
            return false;
        }
        if (!jpegFinished) {
            return true;
        }
    } else {

        // Bound raw reads by the scratch buffer, payload end and next frame-checksum boundary.
        bytes = UINT(std::min({uint64_t(MemoryStorage::ChunkBytes), MemoryStorage::managerState.save.total - MemoryStorage::managerState.save.verified,
                              MemoryStorage::managerState.save.metadata.FrameBytes() - MemoryStorage::managerState.save.verifyFrameBytes}));
        result = f_read(&MemoryStorage::managerState.mediaFile, MemoryStorage::managerState.readBuffer, bytes, &count);
        if (result != FR_OK || count != bytes) {
            return ReportStorageFailure("video verification read failed", result);
        }
        MemoryStorage::managerState.save.verifyCrc = MemoryStorage::UpdateImageCrc32(MemoryStorage::managerState.save.verifyCrc, MemoryStorage::managerState.readBuffer, bytes);
        MemoryStorage::managerState.save.verifyFrameCrc = MemoryStorage::UpdateImageCrc32(MemoryStorage::managerState.save.verifyFrameCrc, MemoryStorage::managerState.readBuffer, bytes);
        MemoryStorage::managerState.save.verified += bytes;
        MemoryStorage::managerState.save.verifyFrameBytes += bytes;
        if (MemoryStorage::managerState.save.verifyFrameBytes == MemoryStorage::managerState.save.metadata.FrameBytes()) {
            result = f_read(&MemoryStorage::managerState.mediaFile, checksum, sizeof(checksum), &count);
            if (result != FR_OK || count != sizeof(checksum)) {
                return ReportStorageFailure("video verification CRC read failed", result);
            }
            if (MemoryStorage::ReadUnsigned32LittleEndian(checksum) != ~MemoryStorage::managerState.save.verifyFrameCrc) {
                return ReportStorageFailure("stored video frame checksum mismatch");
            }
            MemoryStorage::managerState.save.verifyFrameBytes = 0;
            MemoryStorage::managerState.save.verifyFrameCrc = STORAGE_CRC32_INITIAL_U;
        }
    }

    // Publish the checked header only after all payload and frame checks have passed.
    if (MemoryStorage::managerState.save.verified == MemoryStorage::managerState.save.total) {
        if (~MemoryStorage::managerState.save.verifyCrc != MemoryStorage::managerState.save.expectedCrc) {
            return ReportStorageFailure("stored video checksum mismatch");
        }
        header = MemoryStorage::managerState.readBuffer;
        MemoryStorage::BuildLibraryPublicationHeader(header);
        result = f_lseek(&MemoryStorage::managerState.mediaFile, 0);
        if (result == FR_OK) {
            result = f_write(&MemoryStorage::managerState.mediaFile, header, MemoryStorage::managerState.save.headerBytes, &count);
        }
        if (result != FR_OK || count != MemoryStorage::managerState.save.headerBytes) {
            return ReportStorageFailure("video publication failed", result);
        }
        result = f_close(&MemoryStorage::managerState.mediaFile);
        MemoryStorage::managerState.save.open = false;
        if (result != FR_OK) {
            return ReportStorageFailure("video publication close failed", result);
        }
        if (MemoryStorage::managerState.save.libraryId || MemoryStorage::managerState.save.videoId) {
            MemoryStorage::BuildActiveSaveFilePath(stagingPath);
            if (MemoryStorage::managerState.save.libraryId) {
                MemoryStorage::BuildPhotoLibraryPath(MemoryStorage::managerState.save.libraryId, finalPath);
            } else {
                MemoryStorage::BuildVideoLibraryPath(MemoryStorage::managerState.save.videoId, finalPath);
            }
            result = f_rename(stagingPath, finalPath);
            if (result != FR_OK) {
                return ReportStorageFailure("library publication rename failed", result);
            }
        }
        savedType = "video";
        savedId = MemoryStorage::managerState.save.videoId;
        if (MemoryStorage::managerState.save.metadata.type == MediaType::Photo) {
            savedType = "photo";
            savedId = MemoryStorage::managerState.save.libraryId;
        }
        std::printf("[save] %s id=%lu format=%s frames=%lu verified\n", savedType,
                    static_cast<unsigned long>(savedId), MemoryStorage::managerState.save.metadata.FormatName(),
                    static_cast<unsigned long>(MemoryStorage::managerState.save.metadata.frameCount));
        MemoryStorage::managerState.save = {};
        MemoryStorage::managerState.rejectedSlot = SAVED_PHOTO_NO_SLOT;
        complete = true;
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
    int result;
    MemoryStorage::StorageGuard guard;

    if (!guard.acquired) {
        ReportStorageFailure("storage busy");
        return;
    }
    result = MemoryStorage::DiscardIncompleteMediaSave(ready_);
    if (result != FR_OK) {
        ReportStorageFailure("SD save cleanup failed", result);
    }
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
    MemoryStorage::SavedPhotoSlot slots[SAVED_PHOTO_SLOT_COUNT];
    size_t index;
    int latest;
    int result;

    MemoryStorage::StorageGuard guard;

    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("media playback busy/unavailable");
    }
    MemoryStorage::CloseSavedMediaPlayback();
    for (index = 0; index < SAVED_PHOTO_SLOT_COUNT; ++index) {
        slots[index] = MemoryStorage::InspectSavedMediaSlot(unsigned(index));
        if (slots[index].state == MemoryStorage::SlotState::IoError) {
            return ReportStorageFailure("saved media lookup failed", slots[index].result);
        }
    }
    latest = MemoryStorage::FindNewestSavedPhoto(slots);
    if (latest < 0) {
        return ReportStorageFailure("no valid saved media");
    }
    result = f_open(&MemoryStorage::managerState.playbackFile, MemoryStorage::GetSavedMediaPath(unsigned(latest)), FA_READ);
    if (result != FR_OK) {
        return ReportStorageFailure("media playback open failed", result);
    }
    MemoryStorage::managerState.playback.open = true;
    MemoryStorage::playbackFile = &MemoryStorage::managerState.playbackFile;
    MemoryStorage::playbackPrefetch.Reset();
    MemoryStorage::managerState.playback.slot = latest;
    MemoryStorage::managerState.playback.description = slots[latest];
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
    @param timing Optional output counters for this frame read.
    @param jpegOutput Requested JPEG pixel format; raw media retains its stored format.

    @return true - The requested frame and its CRC passed verification.
    @return false - The request, I/O, consumer or checksum failed.
*/
bool
MemoryManager::ReadMediaFrame (
    uint32_t frameIndex,
    ImageSink sink,
    void* context,
    FrameReadTiming* timing,
    PixelFormat jpegOutput
    )
{
    MemoryStorage::SavedFrameResult read;
    uint64_t started;
    PlaybackPrefetch::Stats before;
    PlaybackPrefetch::Stats after;

    if (timing) {
        *timing = {};
    }
    started = to_us_since_boot(get_absolute_time());
    MemoryStorage::StorageGuard guard;

    if (!guard.acquired || !ready_ || !MemoryStorage::managerState.playback.open || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("media playback not open");
    }
    if (!sink || (jpegOutput != PixelFormat::RGB888 && jpegOutput != PixelFormat::RGB565) || frameIndex >= MemoryStorage::managerState.playback.description.metadata.frameCount) {
        return ReportStorageFailure("invalid media frame request");
    }
    // Direction guides read-ahead without changing the selected frame or validation.
    before = MemoryStorage::playbackPrefetch.GetStats();
    MemoryStorage::playbackPrefetch.SetBackwards(MemoryStorage::managerState.hasPlaybackFrame && frameIndex < MemoryStorage::managerState.lastPlaybackFrame);
    if (MemoryStorage::managerState.playback.description.indexedJpeg) {
        read = MemoryStorage::ReadAndVerifyJpegFrame(MemoryStorage::managerState.playback.description, frameIndex, sink, context, false, false, timing, jpegOutput == PixelFormat::RGB565);
    } else {
        read = MemoryStorage::ReadAndVerifyRawVideoFrame(MemoryStorage::managerState.playback.description, frameIndex, sink, context, false, timing);
    }
    if (!read.complete) {
        MemoryStorage::managerState.rejectedSlot = MemoryStorage::managerState.playback.slot;
        MemoryStorage::CloseSavedMediaPlayback(!MemoryStorage::IsStorageIoError(read.result));
        return ReportStorageFailure(read.error, read.result);
    }
    if (timing) {
        after = MemoryStorage::playbackPrefetch.GetStats();
        timing->prefetchHitBytes = after.hitBytes - before.hitBytes;
        timing->prefetchReadBytes = after.readBytes - before.readBytes;
        timing->prefetchLoads = after.loads - before.loads;
        timing->prefetchAheadLoads = after.aheadLoads - before.aheadLoads;
        timing->prefetchFailures = after.failures - before.failures;
        timing->totalUs = to_us_since_boot(get_absolute_time()) - started;
    }
    MemoryStorage::managerState.lastPlaybackFrame = frameIndex;
    MemoryStorage::managerState.hasPlaybackFrame = true;
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
    MemoryStorage::StorageGuard guard;

    if (!guard.acquired) {
        ReportStorageFailure("storage busy");
        return;
    }
    MemoryStorage::CloseSavedMediaPlayback(ready_);
} // CloseMediaPlayback

/**
    Checks one bounded JPEG payload block or one complete encoded frame.

    Checksums the index first, then reuses the existing checked index window while
    reading record headers and encoded bytes in order. Each frame's entropy stream,
    dimensions, CRC and end marker are validated without reconstructing pixels.
    Those same bytes advance the whole-payload CRC, avoiding a second payload scan.
    Playback is suspended while verification borrows its index cache. The caller
    compares the final payload CRC before publishing the saved file.

    @param finished Set true after the final indexed frame covers the exact payload.

    @return true - One bounded index block or complete JPEG frame passed verification.
    @return false - File I/O, index bounds, continuity or JPEG validation failed.
*/
bool
MemoryManager::VerifyJpegSaveStep (
    bool& finished
    )
{
    uint64_t indexBytes;
    UINT bytes;
    UINT count;
    int result;
    MemoryStorage::SavedPhotoSlot slot;
    MemoryStorage::JpegIndexEntry entry;
    MemoryStorage::FrameResult frame;
    uint8_t record[MediaTransfer::JpegRecordBytes];
    uint32_t checksum;

    finished = false;
    indexBytes = uint64_t(MemoryStorage::managerState.save.metadata.frameCount) * MediaTransfer::JpegIndexBytes;

    // Include every stored index byte in the ordered payload checksum once.
    if (MemoryStorage::managerState.save.verified < indexBytes) {
        bytes = UINT(std::min(uint64_t(MemoryStorage::ChunkBytes), indexBytes - MemoryStorage::managerState.save.verified));
        result = f_read(&MemoryStorage::managerState.mediaFile, MemoryStorage::managerState.readBuffer, bytes, &count);
        if (result != FR_OK || count != bytes) {
            return ReportStorageFailure("JPEG index verification read failed", result);
        }
        MemoryStorage::managerState.save.verifyCrc = MemoryStorage::UpdateImageCrc32(
            MemoryStorage::managerState.save.verifyCrc, MemoryStorage::managerState.readBuffer, bytes);
        MemoryStorage::managerState.save.verified += bytes;
        return true;
    }

    // Load a checked window from the save handle; no playback or inspection file is borrowed.
    slot.metadata = MemoryStorage::managerState.save.metadata;
    slot.dataOffset = MemoryStorage::managerState.save.headerBytes;
    slot.indexedJpeg = true;
    if (!MemoryStorage::LoadJpegIndexWindow(MemoryStorage::managerState.mediaFile, slot,
                                          MemoryStorage::managerState.save.jpegVerifiedFrames, result)) {
        return ReportStorageFailure("stored JPEG frame/index verification failed", result);
    }
    entry = MemoryStorage::managerState.jpegIndex[
        MemoryStorage::managerState.save.jpegVerifiedFrames - MemoryStorage::managerState.jpegIndexFirst];
    if (entry.offset != MemoryStorage::managerState.save.verified) {
        return ReportStorageFailure("stored JPEG frame order mismatch");
    }

    // Cross-check the contiguous record header before feeding its bytes to either checksum.
    result = FR_OK;
    if (uint64_t(f_tell(&MemoryStorage::managerState.mediaFile)) != uint64_t(slot.dataOffset) + entry.offset) {
        result = f_lseek(&MemoryStorage::managerState.mediaFile, slot.dataOffset + entry.offset);
    }
    if (result == FR_OK) {
        result = f_read(&MemoryStorage::managerState.mediaFile, record, sizeof(record), &count);
    }
    if (result != FR_OK || count != sizeof(record)) {
        return ReportStorageFailure("JPEG record verification read failed", result);
    }
    if (MemoryStorage::ReadUnsigned32LittleEndian(record) != entry.length) {
        return ReportStorageFailure("stored JPEG record length mismatch");
    }
    checksum = MemoryStorage::ReadUnsigned32LittleEndian(record + FRAME_CHECKSUM_BYTES);
    MemoryStorage::managerState.save.verifyCrc = MemoryStorage::UpdateImageCrc32(
        MemoryStorage::managerState.save.verifyCrc, record, sizeof(record));

    // Validate encoded bytes and update both CRCs through the same bounded input stream.
    frame = MemoryStorage::DecodeEncodedJpegFrame(
        MemoryStorage::managerState.mediaFile, slot.metadata, entry.length, checksum, nullptr, nullptr,
        nullptr, false, &MemoryStorage::managerState.save.verifyCrc);
    if (!frame.complete) {
        return ReportStorageFailure(frame.error, frame.result);
    }
    MemoryStorage::managerState.save.verified += sizeof(record) + entry.length;
    ++MemoryStorage::managerState.save.jpegVerifiedFrames;
    finished = MemoryStorage::managerState.save.jpegVerifiedFrames == MemoryStorage::managerState.save.metadata.frameCount;
    if (finished && MemoryStorage::managerState.save.verified != MemoryStorage::managerState.save.total) {
        return ReportStorageFailure("JPEG payload has trailing or missing frames");
    }
    return true;
} // VerifyJpegSaveStep

/**
    Starts a named photo save with its own metadata and thumbnail.

    Allocates an unused photo ID and opens its staging file through the shared media save
    lifecycle. The caller supplies validated per-image metadata, an expected payload checksum, a
    name and an already prepared thumbnail. These are retained for final header publication, while
    payload bytes arrive through later append calls. The photo becomes visible under its final
    filename only after successful SD readback verification.

    @param metadata Media dimensions, pixel format, placement and playback settings.
    @param checksum Expected CRC32 checksum of the media payload.
    @param name Name associated with the file or saved library item.
    @param thumbnail RGB565 preview pixel buffer for the saved media item.
    @param id Output identifier assigned to the saved or discovered library item.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::BeginLibraryPhoto (
    const MediaMetadata& metadata,
    uint32_t checksum,
    const char* name,
    const uint8_t* thumbnail,
    uint32_t& id
    )
{
    char path[LIBRARY_PATH_BYTES];
    int lookup;
    uint32_t target;
    size_t i;
    int result;
    UINT count;

    id = 0;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("photo storage busy or unavailable");
    }
    if (!metadata.IsPhotoWithin(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT) || !name || !thumbnail ||
        (metadata.pixelFormat == PixelFormat::JPEG &&
         metadata.encodedBytes > MediaTransfer::MaximumJpegBytes + JPEG_SINGLE_PHOTO_OVERHEAD_BYTES)) {
        return ReportStorageFailure("invalid photo metadata");
    }

    // Validate the root filename and image inputs before opening a file.
    lookup = FR_OK;
    target = MemoryStorage::AllocateLibraryId(false, lookup);
    if (lookup != FR_OK) {
        return ReportStorageFailure("photo library lookup failed", lookup);
    }
    if (!target) {
        return ReportStorageFailure("media filename namespace exhausted");
    }
    MemoryStorage::CloseSavedMediaPlayback();
    MemoryStorage::managerState.save = MemoryStorage::SaveSession{};
    MemoryStorage::managerState.save.libraryId = target;
    MemoryStorage::managerState.save.headerBytes = MemoryStorage::LibraryHeaderBytes;
    MemoryStorage::managerState.save.metadata = metadata;
    MemoryStorage::managerState.save.expectedCrc = checksum;
    metadata.PayloadBytes(MemoryStorage::managerState.save.total);
    for (i = 0; i + 1 < sizeof(MemoryStorage::managerState.save.name) && name[i]; ++i) {
        if (static_cast<unsigned char>(name[i]) >= ASCII_SPACE &&
            static_cast<unsigned char>(name[i]) < ASCII_DELETE) {
            MemoryStorage::managerState.save.name[i] = name[i];
        } else {
            MemoryStorage::managerState.save.name[i] = '_';
        }
    }

    std::memcpy(MemoryStorage::managerState.save.thumbnail, thumbnail, sizeof(MemoryStorage::managerState.save.thumbnail));
    MemoryStorage::BuildPhotoLibraryPath(target, path, true);

    // Open the chosen destination before writing its header and pixel payload.
    result = f_open(&MemoryStorage::managerState.mediaFile, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (result != FR_OK) {
        MemoryStorage::managerState.save = {};
        return ReportStorageFailure("photo create failed", result);
    }
    MemoryStorage::managerState.save.open = true;
    MemoryStorage::managerState.save.phase = MemoryStorage::SavePhase::Writing;
    std::memset(MemoryStorage::managerState.readBuffer, 0, MemoryStorage::LibraryHeaderBytes);
    std::memcpy(MemoryStorage::managerState.readBuffer, "H75P", MEDIA_MAGIC_BYTES);
    count = 0;
    result = f_write(&MemoryStorage::managerState.mediaFile, MemoryStorage::managerState.readBuffer, MemoryStorage::LibraryHeaderBytes, &count);
    if (result != FR_OK || count != MemoryStorage::LibraryHeaderBytes) {
        MemoryStorage::DiscardIncompleteMediaSave(!MemoryStorage::IsStorageIoError(result));
        return ReportStorageFailure("photo staging failed", result);
    }
    id = target;

    error_ = "none";
    return true;
} // BeginLibraryPhoto

/**
    Lists one bounded page of saved photos and their metadata.

    Selects a bounded cursor-ordered set of photo IDs, then inspects each selected file's stored
    metadata and validity. Directory totals and continuation information are returned separately
    from the number of entries in this page. The guard is held across synchronous listing/header
    reads, and an active save or unavailable card prevents the page from being treated as
    complete.

    @param photos Output array of saved photo descriptions.
    @param capacity Maximum number of entries available in the output array.
    @param count Output number of entries or bytes produced by this operation.
    @param after Exclusive library ID cursor; zero starts at the first page.
    @param page Optional output pagination totals and continuation cursor.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::ListLibraryPhotos (
    LibraryPhoto* photos,
    size_t capacity,
    size_t& count,
    uint32_t after,
    LibraryPage* page
    )
{
    LibraryPage measured;
    int result;
    size_t i;
    uint32_t id;
    MemoryStorage::SavedPhotoSlot slot;

    count = 0;
    if (page) {
        *page = {};
    }

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("photo storage busy or unavailable");
    }
    if (!photos || !capacity || after > MaximumLibraryId) {
        return ReportStorageFailure("invalid photo page request");
    }
    measured = {};
    result = MemoryStorage::FindPhotoLibraryPage(photos, capacity, count, after, measured);

    if (result != FR_OK) {
        return ReportStorageFailure("photo directory read failed", result);
    }
    for (i = 0; i < count; ++i) {
        id = photos[i].id;
        slot = MemoryStorage::InspectPhotoLibraryFile(id, &photos[i]);
        if (slot.state == MemoryStorage::SlotState::IoError) {
            return ReportStorageFailure("photo listing failed", slot.result);
        }
    }

    if (page) {
        *page = measured;
    }

    error_ = "none";
    return true;
} // ListLibraryPhotos

/**
    Reads and validates metadata for one saved photo ID.

    Checks availability and transaction exclusivity before inspecting the requested managed photo
    file. Only a valid inspected record is copied into the caller's output; missing, corrupt and
    I/O-failed records retain their error distinction. This reads metadata rather than streaming
    the full image into the display.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param item Output description of the requested saved media item.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::GetLibraryPhoto (
    uint32_t id,
    LibraryPhoto& item
    )
{
    MemoryStorage::SavedPhotoSlot slot;

    item = {};

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("photo storage busy or unavailable");
    }
    if (!id || id > MaximumLibraryId) {
        return ReportStorageFailure("invalid photo id");
    }
    slot = MemoryStorage::InspectPhotoLibraryFile(id, &item);
    if (slot.state == MemoryStorage::SlotState::IoError || slot.state == MemoryStorage::SlotState::Missing) {
        return ReportStorageFailure("photo unavailable", slot.result);
    }

    error_ = "none";
    return true;
} // GetLibraryPhoto

/**
    Reads the stored thumbnail belonging to one photo ID.

    Inspects the named photo under storage ownership and copies its persisted small RGB565
    thumbnail through the file-validation helper. This avoids decoding the full image for the
    older thumbnail representation. A concurrent save, bad ID or invalid file returns failure
    without opening the active playback stream.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param thumbnail RGB565 preview pixel buffer for the saved media item.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::ReadLibraryThumbnail (
    uint32_t id,
    uint8_t* thumbnail
    )
{
    MemoryStorage::SavedPhotoSlot slot;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("photo storage busy or unavailable");
    }
    if (!id || id > MaximumLibraryId || !thumbnail) {
        return ReportStorageFailure("invalid photo id");
    }
    slot = MemoryStorage::InspectPhotoLibraryFile(id, nullptr, thumbnail);
    if (slot.state != MemoryStorage::SlotState::Valid) {
        return ReportStorageFailure("photo thumbnail unavailable", slot.result);
    }

    error_ = "none";
    return true;
} // ReadLibraryThumbnail

/**
    Opens a named saved photo for framebuffer restoration.

    Closes the prior playback reader, validates the named photo's stored header and opens its file
    as the active playback source. The returned metadata describes that reader, but no pixels are
    streamed or published here. A subsequent frame read performs payload validation and delivers
    fragments under the caller's render ownership.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param metadata Output media descriptor populated by this operation.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::OpenLibraryPhoto (
    uint32_t id,
    MediaMetadata& metadata
    )
{
    MemoryStorage::SavedPhotoSlot slot;
    char path[LIBRARY_PATH_BYTES];
    int result;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("photo storage busy or unavailable");
    }
    if (!id || id > MaximumLibraryId) {
        return ReportStorageFailure("invalid photo id");
    }
    MemoryStorage::CloseSavedMediaPlayback();
    slot = MemoryStorage::InspectPhotoLibraryFile(id);
    if (slot.state != MemoryStorage::SlotState::Valid) {
        return ReportStorageFailure("saved photo unavailable", slot.result);
    }

    // Validate the root filename and image inputs before opening a file.
    MemoryStorage::BuildPhotoLibraryPath(id, path);

    // Open the saved file before validating its header and reading pixels.
    result = f_open(&MemoryStorage::managerState.playbackFile, path, FA_READ);
    if (result != FR_OK) {
        return ReportStorageFailure("photo playback open failed", result);
    }
    MemoryStorage::managerState.playback.open = true;
    MemoryStorage::playbackFile = &MemoryStorage::managerState.playbackFile;
    MemoryStorage::playbackPrefetch.Reset();
    MemoryStorage::managerState.playback.slot = STORAGE_LIBRARY_PHOTO_SLOT;
    MemoryStorage::managerState.playback.description = slot;
    metadata = slot.metadata;

    error_ = "none";
    return true;
} // OpenLibraryPhoto

/**
    Deletes the files belonging to one saved photo ID.

    Validates the managed ID and requires idle, ready storage before removing its photo files.
    Deletion is limited to controller-generated library paths and records any filesystem failure
    for recovery handling. Selection repair belongs to MediaLibrary and must be coordinated before
    deleting a selected item.

    @param id Saved media library identifier; valid IDs are nonzero.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::DeleteLibraryPhoto (
    uint32_t id
    )
{
    char path[LIBRARY_PATH_BYTES];
    int result;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("photo storage busy or unavailable");
    }
    if (!id || id > MaximumLibraryId) {
        return ReportStorageFailure("invalid photo id");
    }
    if (MemoryStorage::managerState.playback.open && MemoryStorage::managerState.playback.slot == STORAGE_LIBRARY_PHOTO_SLOT) {
        MemoryStorage::CloseSavedMediaPlayback();
    }

    // Validate the root filename and image inputs before opening a file.
    MemoryStorage::BuildPhotoLibraryPath(id, path);
    result = f_unlink(path);

    if (result != FR_OK) {
        return ReportStorageFailure("photo delete failed", result);
    }

    error_ = "none";
    return true;
} // DeleteLibraryPhoto

/**
    Reads the newest valid library selection and display settings.

    Reads both retained settings records and selects the newest valid generation. Legacy records
    are normalized by the decoder, while an absent/invalid saved configuration uses the supported
    defaults. Actual filesystem errors are reported instead of being mistaken for a user who has
    never saved settings.

    @param settings Output gallery selection, slideshow and display settings.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::ReadGallerySettings (
    GallerySettings& settings
    )
{
    MemoryStorage::GalleryRecord records[GALLERY_SETTINGS_RECORD_COUNT];
    size_t recordIndex;
    const MemoryStorage::GalleryRecord* record = nullptr;
    int latest;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("photo storage busy or unavailable");
    }
    records[0] = MemoryStorage::ReadAndValidateGalleryRecord(0);
    records[1] = MemoryStorage::ReadAndValidateGalleryRecord(1);
    for (recordIndex = 0; recordIndex < sizeof(records) / sizeof(records[0]); ++recordIndex) {
        record = &(records[recordIndex]);
        if (record->result != FR_OK) {
            return ReportStorageFailure("slideshow settings read failed", record->result);
        }
    }

    latest = MemoryStorage::FindNewestValidGalleryRecord(records);
    if (latest < 0) {
        settings = GallerySettings{};
    } else {
        settings = records[latest].settings;
    }

    error_ = "none";
    return true;
} // ReadGallerySettings

/**
    Saves library selection and display settings transactionally.

    Validates the complete combination of selection, slideshow, FPS and brightness before choosing
    the alternate retained settings record. The new generation is written with a checksum, closed
    and reread to verify both contents and generation. The previously valid record is retained
    while this synchronous write/readback sequence runs, providing a fallback after interrupted
    updates.

    @param settings Saved gallery selection, slideshow and display settings.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::SaveGallerySettings (
    const GallerySettings& settings
    )
{
    MemoryStorage::GalleryRecord records[GALLERY_SETTINGS_RECORD_COUNT];
    size_t recordIndex;
    const MemoryStorage::GalleryRecord* record = nullptr;
    int latest;
    unsigned target;
    uint8_t header[GALLERY_CONTROLS_RECORD_BYTES];
    size_t headerBytes;
    uint32_t nextGeneration;
    int result;
    UINT count;
    int closed;
    uint32_t writeError;
    MemoryStorage::GalleryRecord verified;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("photo storage busy or unavailable");
    }
    if (settings.selectedPhoto > MaximumLibraryId || !settings.intervalSeconds ||
        settings.intervalSeconds > MAX_SLIDESHOW_INTERVAL_SECONDS ||
        (settings.slideshow && !settings.selectedPhoto) || settings.selectedVideo > MaximumLibraryId ||
        (settings.selectedVideo && (settings.selectedPhoto || settings.slideshow)) ||
        settings.maxFps > MAX_VIDEO_FPS || settings.brightness > MAX_BRIGHTNESS_PERCENT) {
        return ReportStorageFailure("invalid gallery/display settings");
    }
    records[0] = MemoryStorage::ReadAndValidateGalleryRecord(0);
    records[1] = MemoryStorage::ReadAndValidateGalleryRecord(1);
    for (recordIndex = 0; recordIndex < sizeof(records) / sizeof(records[0]); ++recordIndex) {
        record = &(records[recordIndex]);
        if (record->result != FR_OK) {
            return ReportStorageFailure("slideshow settings read failed", record->result);
        }
    }

    latest = MemoryStorage::FindNewestValidGalleryRecord(records);
    if (latest == 0) {
        target = 1;
    } else {
        target = 0;
    }

    // Build the on-card header, including dimensions, payload size and checksum.
    std::memset(header, 0, sizeof(header));
    headerBytes = sizeof(header);
    std::memcpy(header, "H75C", MEDIA_MAGIC_BYTES);
    nextGeneration = {};
    if (latest < 0) {
        nextGeneration = 1;
    } else {
        nextGeneration = records[latest].generation + 1;
    }
    MemoryStorage::WriteUnsigned32LittleEndian(header + GALLERY_GENERATION_OFFSET, nextGeneration);
    MemoryStorage::WriteUnsigned32LittleEndian(header + GALLERY_PHOTO_ID_OFFSET, settings.selectedPhoto);
    MemoryStorage::WriteUnsigned32LittleEndian(header + GALLERY_INTERVAL_OFFSET, settings.intervalSeconds);
    header[GALLERY_SLIDESHOW_OFFSET] = settings.slideshow;
    MemoryStorage::WriteUnsigned32LittleEndian(header + GALLERY_VIDEO_ID_OFFSET, settings.selectedVideo);
    MemoryStorage::WriteUnsigned32LittleEndian(header + GALLERY_FPS_OFFSET, settings.maxFps);
    MemoryStorage::WriteUnsigned32LittleEndian(header + GALLERY_BRIGHTNESS_OFFSET, settings.brightness);
    MemoryStorage::WriteUnsigned32LittleEndian(
        header + headerBytes - FRAME_CHECKSUM_BYTES,
        ~MemoryStorage::UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, header, headerBytes - FRAME_CHECKSUM_BYTES));

    // Open the chosen destination before writing its header and pixel payload.
    result = f_open(&MemoryStorage::managerState.imageFile, MemoryStorage::GalleryPaths[target], FA_WRITE | FA_CREATE_ALWAYS);
    if (result != FR_OK) {
        return ReportStorageFailure("slideshow settings create failed", result);
    }
    count = 0;
    result = f_write(&MemoryStorage::managerState.imageFile, header, UINT(headerBytes), &count);
    closed = f_close(&MemoryStorage::managerState.imageFile);
    if (result != FR_OK || closed != FR_OK || count != headerBytes) {
        writeError = {};
        if (result == FR_OK) {
            writeError = closed;
        } else {
            writeError = result;
        }
        return ReportStorageFailure("slideshow settings save failed", writeError);
    }
    verified = MemoryStorage::ReadAndValidateGalleryRecord(target);
    if (!verified.valid || verified.result != FR_OK ||
        verified.generation != MemoryStorage::ReadUnsigned32LittleEndian(header + GALLERY_GENERATION_OFFSET)) {
        return ReportStorageFailure("slideshow settings verification failed", verified.result);
    }

    error_ = "none";
    return true;
} // SaveGallerySettings

/**
    Saves the current framebuffer as a new named library photo.

    First reads the stable framebuffer source to compute its payload checksum and small RGB565
    thumbnail. It then opens a new library-photo transaction, reads the source again to append its
    pixels, and completes readback verification before returning success and an ID.

    This convenience path performs the complete save synchronously, so it can delay foreground
    frame preparation. The caller must keep the source image unchanged through every pass,
    including while the manager releases and reacquires its internal guard between transaction
    operations.

    @param source Callback that fills requested source pixel chunks; false cancels the save.
    @param context Caller-owned state passed unchanged to the callback.
    @param id Output identifier assigned to the saved or discovered library item.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::SaveLibraryImage (
    ImageSource source,
    void* context,
    uint32_t& id
    )
{
    static uint8_t thumbnail[ThumbnailBytes];
    uint32_t crc;
    size_t offset;
    size_t count;
    size_t i;
    size_t pixel;
    size_t x;
    size_t y;
    uint16_t word;
    size_t chunkOffset;
    size_t chunkCount;
    bool complete;

    if (!source || !ready_) {
        return ReportStorageFailure("missing image source or storage");
    }
    // A thumbnail samples each 8x8 cell's center, before panel wiring/status overlays.
    crc = STORAGE_CRC32_INITIAL_U;
    {
        MemoryStorage::StorageGuard guard;
        if (!guard.acquired || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
            return ReportStorageFailure("photo storage busy");
        }
        std::memset(thumbnail, 0, sizeof(thumbnail));
        for (offset = 0; offset < SOURCE_FRAME_SIZE_BYTES; offset += MemoryStorage::ChunkBytes) {
            count = std::min(MemoryStorage::ChunkBytes, SOURCE_FRAME_SIZE_BYTES - offset);
            if (!source(context, MemoryStorage::managerState.readBuffer, count, offset)) {
                return ReportStorageFailure("image source failed");
            }
            crc = MemoryStorage::UpdateImageCrc32(crc, MemoryStorage::managerState.readBuffer, count);
            for (i = 0; i < count; i += RGB888_PIXEL_BYTES) {
                pixel = (offset + i) / RGB888_PIXEL_BYTES;
                x = pixel % LOGICAL_IMAGE_WIDTH;
                y = pixel / LOGICAL_IMAGE_WIDTH;
                if (x % LIBRARY_THUMBNAIL_SAMPLE_STEP != LIBRARY_THUMBNAIL_SAMPLE_CENTER ||
                    y % LIBRARY_THUMBNAIL_SAMPLE_STEP != LIBRARY_THUMBNAIL_SAMPLE_CENTER) {
                    continue;
                }
                word = uint16_t((MemoryStorage::managerState.readBuffer[i] >> RGB565_RED_BLUE_REDUCE_SHIFT) << RGB565_RED_SHIFT |
                             (MemoryStorage::managerState.readBuffer[i + 1] >> RGB565_GREEN_REDUCE_SHIFT) << RGB565_GREEN_SHIFT |
                             MemoryStorage::managerState.readBuffer[i + RGB_PIXEL_BLUE_INDEX] >> RGB565_RED_BLUE_REDUCE_SHIFT);
                MediaMetadataCodec::WriteUnsigned16LittleEndian(
                    thumbnail + ((y / LIBRARY_THUMBNAIL_SAMPLE_STEP) * LIBRARY_THUMBNAIL_WIDTH +
                                 x / LIBRARY_THUMBNAIL_SAMPLE_STEP) *
                                    RGB565_PIXEL_BYTES,
                    word);
            }
        }
    }
    if (!BeginLibraryPhoto(MediaMetadata::Photo(LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT), ~crc,
                           "Uploaded photo", thumbnail, id)) {
        return false;
    }
    for (chunkOffset = 0; chunkOffset < SOURCE_FRAME_SIZE_BYTES; chunkOffset += MemoryStorage::ChunkBytes) {
        chunkCount = std::min(MemoryStorage::ChunkBytes, SOURCE_FRAME_SIZE_BYTES - chunkOffset);
        if (!source(context, MemoryStorage::managerState.readBuffer, chunkCount, chunkOffset) || !AppendMediaBytes(MemoryStorage::managerState.readBuffer, chunkCount)) {
            AbortMediaSave();
            return false;
        }
    }

    if (!StartMediaVerification()) {
        AbortMediaSave();
        return false;
    }
    complete = false;
    while (!complete) {
        if (!VerifyMediaSaveStep(complete)) {
            AbortMediaSave();
            return false;
        }
    }

    return true;
} // SaveLibraryImage

/**
    Starts a named video save using a newly allocated library ID.

    Allocates a new managed video ID and creates a staging save without loading the whole clip
    into memory. It retains the video's name and selected upload-duration setting alongside
    validated timing, loop and color metadata. Subsequent append and verification calls share the
    bounded transaction buffer, and final rename publishes the item only after the saved payload
    is accepted.

    @param metadata Media dimensions, pixel format, placement and playback settings.
    @param checksum Expected CRC32 checksum of the media payload.
    @param name Name associated with the file or saved library item.
    @param maxSecondsMillis Upload duration limit selected by the sender, in milliseconds.
    @param id Output identifier assigned to the saved or discovered library item.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::BeginLibraryVideo (
    const MediaMetadata& metadata,
    uint32_t checksum,
    const char* name,
    uint32_t maxSecondsMillis,
    uint32_t& id
    )
{
    char path[LIBRARY_PATH_BYTES];
    int lookup;
    uint32_t target;
    size_t i;
    int result;
    UINT count;

    id = 0;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("video storage busy or unavailable");
    }
    if (!name || !MediaTransfer::IsSupportedVideo(metadata, LOGICAL_IMAGE_WIDTH, LOGICAL_IMAGE_HEIGHT)) {
        return ReportStorageFailure("invalid video metadata");
    }

    // Validate the root filename and image inputs before opening a file.
    lookup = FR_OK;
    target = MemoryStorage::AllocateLibraryId(true, lookup);
    if (lookup != FR_OK) {
        return ReportStorageFailure("video library lookup failed", lookup);
    }
    if (!target) {
        return ReportStorageFailure("media filename namespace exhausted");
    }
    MemoryStorage::CloseSavedMediaPlayback();
    MemoryStorage::managerState.save = MemoryStorage::SaveSession{};
    MemoryStorage::managerState.save.videoId = target;
    MemoryStorage::managerState.save.headerBytes = MemoryStorage::VideoLibraryHeaderBytes;
    MemoryStorage::managerState.save.metadata = metadata;
    MemoryStorage::managerState.save.expectedCrc = checksum;
    MemoryStorage::managerState.save.maxSecondsMillis = maxSecondsMillis;
    metadata.PayloadBytes(MemoryStorage::managerState.save.total);
    for (i = 0; i + 1 < sizeof(MemoryStorage::managerState.save.name) && name[i]; ++i) {
        if (static_cast<unsigned char>(name[i]) >= ASCII_SPACE &&
            static_cast<unsigned char>(name[i]) < ASCII_DELETE) {
            MemoryStorage::managerState.save.name[i] = name[i];
        } else {
            MemoryStorage::managerState.save.name[i] = '_';
        }
    }

    if (!MemoryStorage::managerState.save.name[0]) {
        std::snprintf(MemoryStorage::managerState.save.name, sizeof(MemoryStorage::managerState.save.name), "Video %lu", static_cast<unsigned long>(target));
    }
    MemoryStorage::BuildVideoLibraryPath(target, path, true);

    // Open the chosen destination before writing its header and pixel payload.
    result = f_open(&MemoryStorage::managerState.mediaFile, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (result != FR_OK) {
        MemoryStorage::managerState.save = {};
        return ReportStorageFailure("video library create failed", result);
    }
    MemoryStorage::managerState.save.open = true;
    MemoryStorage::managerState.save.phase = MemoryStorage::SavePhase::Writing;
    std::memset(MemoryStorage::managerState.readBuffer, 0, MemoryStorage::VideoLibraryHeaderBytes);
    std::memcpy(MemoryStorage::managerState.readBuffer, "H75P", MEDIA_MAGIC_BYTES);
    count = 0;
    result = f_write(&MemoryStorage::managerState.mediaFile, MemoryStorage::managerState.readBuffer, MemoryStorage::VideoLibraryHeaderBytes, &count);
    if (result != FR_OK || count != MemoryStorage::VideoLibraryHeaderBytes) {
        MemoryStorage::DiscardIncompleteMediaSave(!MemoryStorage::IsStorageIoError(result));
        return ReportStorageFailure("video library staging failed", result);
    }
    id = target;

    error_ = "none";
    return true;
} // BeginLibraryVideo

/**
    Lists one bounded page of saved videos and their metadata.

    Selects a bounded ordered page of video IDs and inspects their headers for format, timing,
    saved name and validity. The page reports directory totals and a continuation cursor without
    retaining the whole collection. Reads are synchronous under the storage guard; busy or
    unavailable storage fails the request rather than returning a partial successful page.

    @param videos Output array of saved video descriptions.
    @param capacity Maximum number of entries available in the output array.
    @param count Output number of entries or bytes produced by this operation.
    @param after Exclusive library ID cursor; zero starts at the first page.
    @param page Optional output pagination totals and continuation cursor.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::ListLibraryVideos (
    LibraryVideo* videos,
    size_t capacity,
    size_t& count,
    uint32_t after,
    LibraryPage* page
    )
{
    LibraryPage measured;
    int result;
    size_t i;
    uint32_t id;
    MemoryStorage::SavedPhotoSlot slot;

    count = 0;
    if (page) {
        *page = {};
    }

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("video storage busy or unavailable");
    }
    if (!videos || !capacity || after > MaximumLibraryId) {
        return ReportStorageFailure("invalid video page request");
    }
    measured = {};
    result = MemoryStorage::FindVideoLibraryPage(videos, capacity, count, after, measured);

    if (result != FR_OK) {
        return ReportStorageFailure("video directory read failed", result);
    }
    for (i = 0; i < count; ++i) {
        id = videos[i].id;
        slot = MemoryStorage::InspectLibraryVideo(id, &videos[i]);
        if (slot.state == MemoryStorage::SlotState::IoError) {
            return ReportStorageFailure("video listing failed", slot.result);
        }
    }

    if (page) {
        *page = measured;
    }

    error_ = "none";
    return true;
} // ListLibraryVideos

/**
    Reads and validates metadata for one saved video ID.

    Inspects one managed video under the storage guard and returns its metadata, checksum, name
    and recorded upload limit only when the stored header is valid. It does not open the clip for
    playback or validate every frame. Missing or corrupt entries remain distinguishable from
    filesystem failures for library handling.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param item Output description of the requested saved media item.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::GetLibraryVideo (
    uint32_t id,
    LibraryVideo& item
    )
{
    MemoryStorage::SavedPhotoSlot slot;

    item = {};

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("video storage busy or unavailable");
    }
    if (!id || id > MaximumLibraryId) {
        return ReportStorageFailure("invalid video id");
    }
    slot = MemoryStorage::InspectLibraryVideo(id, &item);
    if (slot.state == MemoryStorage::SlotState::IoError || slot.state == MemoryStorage::SlotState::Missing) {
        return ReportStorageFailure("video unavailable", slot.result);
    }

    error_ = "none";
    return true;
} // GetLibraryVideo

/**
    Opens a named saved video for frame playback.

    Validates the selected video's stored description before replacing the active playback reader.
    It opens the managed file, resets its prefetch/index state and returns the metadata used by
    MediaPlayer's timeline. Frame pixels and checksums are validated later by individual frame
    reads, so opening alone does not publish an image or guarantee that every frame is intact.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param metadata Output media descriptor populated by this operation.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::OpenLibraryVideo (
    uint32_t id,
    MediaMetadata& metadata
    )
{
    MemoryStorage::SavedPhotoSlot slot;
    char path[LIBRARY_PATH_BYTES];
    int result;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("video storage busy or unavailable");
    }
    if (!id || id > MaximumLibraryId) {
        return ReportStorageFailure("invalid video id");
    }
    MemoryStorage::CloseSavedMediaPlayback();
    slot = MemoryStorage::InspectLibraryVideo(id);
    if (slot.state != MemoryStorage::SlotState::Valid) {
        return ReportStorageFailure("saved video unavailable", slot.result);
    }

    // Validate the root filename and image inputs before opening a file.
    MemoryStorage::BuildVideoLibraryPath(id, path);

    // Open the saved file before validating its header and reading pixels.
    result = f_open(&MemoryStorage::managerState.playbackFile, path, FA_READ);
    if (result != FR_OK) {
        return ReportStorageFailure("video library playback open failed", result);
    }
    MemoryStorage::managerState.playback.open = true;
    MemoryStorage::playbackFile = &MemoryStorage::managerState.playbackFile;
    MemoryStorage::playbackPrefetch.Reset();
    MemoryStorage::managerState.playback.slot = STORAGE_LIBRARY_VIDEO_SLOT;
    MemoryStorage::managerState.playback.libraryVideoId = id;
    MemoryStorage::managerState.playback.description = slot;
    metadata = slot.metadata;

    error_ = "none";
    return true;
} // OpenLibraryVideo

/**
    Deletes the files belonging to one saved video ID.

    Checks the managed ID and closes its active playback reader when necessary before unlinking
    the video's files. The operation is serialized with saves and other storage access; filesystem
    errors enter the usual failure path. MediaLibrary is responsible for persisting a replacement
    selection before calling deletion.

    @param id Saved media library identifier; valid IDs are nonzero.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::DeleteLibraryVideo (
    uint32_t id
    )
{
    char path[LIBRARY_PATH_BYTES];
    int result;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("video storage busy or unavailable");
    }
    if (!id || id > MaximumLibraryId) {
        return ReportStorageFailure("invalid video id");
    }
    if (MemoryStorage::managerState.playback.open && MemoryStorage::managerState.playback.libraryVideoId == id) {
        MemoryStorage::CloseSavedMediaPlayback();
    }

    // Validate the root filename and image inputs before opening a file.
    MemoryStorage::BuildVideoLibraryPath(id, path);
    result = f_unlink(path);

    if (result != FR_OK) {
        return ReportStorageFailure("video delete failed", result);
    }

    error_ = "none";
    return true;
} // DeleteLibraryVideo

/**
    Adopts the previous firmware's saved video without copying its payload.

    Checks a persistent migration marker and existing video entries before examining legacy
    retained media. A newest usable legacy video can be adopted by renaming its payload into the
    library rather than copying the clip. The marker prevents a deliberately deleted imported
    video from reappearing on every boot. A successful lookup with no video to adopt returns id
    zero; filesystem errors remain failures.

    @param id Output identifier assigned to the saved or discovered library item.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::ImportLastVideo (
    uint32_t& id
    )
{
    FILINFO markerInfo;
    int exists;
    bool existingVideo;
    int scanned;
    MemoryStorage::SavedPhotoSlot slots[SAVED_MEDIA_SLOT_COUNT];
    size_t slotIndex;
    const MemoryStorage::SavedPhotoSlot* slot = nullptr;
    int latest;
    char path[LIBRARY_PATH_BYTES];
    int lookup;
    uint32_t candidate;
    int renamed;
    /**
        Writes a marker so legacy saved-video import is not repeated.

        Writes and closes the persistent migration marker only after legacy-video adoption or
        confirmation that an existing library already takes precedence. Later boots can then leave
        a deliberately deleted imported clip deleted. Short writes and close failures are reported
        so a failed marker is not treated as a completed migration.

        @return true - The legacy video migration marker was written and closed.
        @return false - Creating, writing or closing the marker file failed.
    */
    auto markImported = [&]() {
        uint8_t marker[STORAGE_HEADER_METADATA_OFFSET];
        int result;
        UINT written;
        int closed;
        uint32_t writeError;

        std::memset(marker, 0, sizeof(marker));
        std::memcpy(marker, "H75V", MEDIA_MAGIC_BYTES);
        MemoryStorage::WriteUnsigned32LittleEndian(marker + MEDIA_MAGIC_BYTES, 1);
        MemoryStorage::WriteUnsigned32LittleEndian(
            marker + STORAGE_HEADER_GENERATION_OFFSET,
            ~MemoryStorage::UpdateImageCrc32(STORAGE_CRC32_INITIAL_U, marker, STORAGE_HEADER_GENERATION_OFFSET));
        result = f_open(&MemoryStorage::managerState.imageFile, "0:/VLIB.H75", FA_WRITE | FA_CREATE_ALWAYS);
        written = 0;
        if (result != FR_OK) {
            return ReportStorageFailure("video migration state create failed", result);
        }
        result = f_write(&MemoryStorage::managerState.imageFile, marker, sizeof(marker), &written);
        closed = f_close(&MemoryStorage::managerState.imageFile);
        if (result != FR_OK || written != sizeof(marker) || closed != FR_OK) {
            writeError = {};
            if (result == FR_OK) {
                writeError = closed;
            } else {
                writeError = result;
            }
            return ReportStorageFailure("video migration state write failed", writeError);
        }
        return true;
    };

    id = 0;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("video storage busy or unavailable");
    }
    // A persistent marker prevents deleted videos from reappearing from an older
    // legacy backup on the next boot. Interrupted migration never deletes payload.
    markerInfo = {};
    exists = f_stat("0:/VLIB.H75", &markerInfo);
    if (exists == FR_OK) {
        return true;
    }
    if (exists != FR_NO_FILE) {
        return ReportStorageFailure("video migration state lookup failed", exists);
    }

    existingVideo = false;
    scanned = MemoryStorage::FindExistingLibraryVideo(existingVideo);
    if (scanned != FR_OK) {
        return ReportStorageFailure("video import lookup failed", scanned);
    }
    if (existingVideo) {
        return markImported();
    }
    slots[0] = MemoryStorage::InspectSavedMediaSlot(0);
    slots[1] = MemoryStorage::InspectSavedMediaSlot(1);
    for (slotIndex = 0; slotIndex < sizeof(slots) / sizeof(slots[0]); ++slotIndex) {
        slot = &(slots[slotIndex]);
        if (slot->state == MemoryStorage::SlotState::IoError) {
            return ReportStorageFailure("legacy video lookup failed", slot->result);
        }
    }

    latest = MemoryStorage::FindNewestSavedPhoto(slots);
    if (latest < 0 || slots[latest].metadata.type != MediaType::Video) {
        return markImported();
    }

    // Validate the root filename and image inputs before opening a file.
    lookup = FR_OK;
    candidate = MemoryStorage::AllocateLibraryId(true, lookup);
    if (lookup != FR_OK || !candidate) {
        return ReportStorageFailure("video import allocation failed", lookup);
    }
    MemoryStorage::BuildVideoLibraryPath(candidate, path);
    MemoryStorage::CloseSavedMediaPlayback();
    renamed = f_rename(MemoryStorage::GetSavedMediaPath(unsigned(latest)), path);
    if (renamed != FR_OK) {
        return ReportStorageFailure("legacy video import failed", renamed);
    }
    id = candidate;
    MemoryStorage::managerState.rejectedSlot = STORAGE_NO_SLOT;

    error_ = "none";
    return markImported();
} // ImportLastVideo

/**
    Builds a verified first-frame preview without replacing active playback.

    Inspects the selected photo/video using its own metadata, then reads and verifies its first
    frame through a temporary handle and a small sampling sink. The preview is initialized to
    black before content pixels are sampled in their saved placement. It keeps the current
    playback handle, index cache and CPU framebuffer untouched, but synchronous reading or JPEG
    decoding can still delay foreground work.

    @param id Saved media library identifier; valid IDs are nonzero.
    @param video True selects videos; false selects photos.
    @param pixels Writable destination buffer; the caller must provide sufficient storage.
    @param capacity Number of elements available in the destination buffer, including any text
                    terminator.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::ReadLibraryPreview (
    uint32_t id,
    bool video,
    uint8_t* pixels,
    size_t capacity
    )
{
    MemoryStorage::PreviewSamplingContext preview;
    MemoryStorage::SavedPhotoSlot slot;
    char path[LIBRARY_PATH_BYTES];
    int opened;
    uint32_t pixelBytes;
    MemoryStorage::SavedFrameResult read;
    int closed;
    const char* failure = nullptr;
    uint32_t failureCode;

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
        return ReportStorageFailure("preview storage busy or unavailable");
    }
    if (!id || id > MaximumLibraryId || !pixels || capacity < PreviewBytes) {
        return ReportStorageFailure("invalid preview request");
    }
    slot = {};
    if (video) {
        slot = MemoryStorage::InspectLibraryVideo(id);
    } else {
        slot = MemoryStorage::InspectPhotoLibraryFile(id);
    }
    if (slot.state != MemoryStorage::SlotState::Valid) {
        return ReportStorageFailure("saved preview unavailable", slot.result);
    }

    // Validate the root filename and image inputs before opening a file.
    if (video) {
        MemoryStorage::BuildVideoLibraryPath(id, path);
    } else {
        MemoryStorage::BuildPhotoLibraryPath(id, path);
    }

    // Open the saved file before validating its header and reading pixels.
    opened = f_open(&MemoryStorage::managerState.imageFile, path, FA_READ);
    if (opened != FR_OK) {
        return ReportStorageFailure("preview open failed", opened);
    }
    pixelBytes = {};
    if (slot.indexedJpeg) {
        pixelBytes = RGB888_PIXEL_BYTES;
    } else {
        pixelBytes = slot.metadata.PixelBytes();
    }

    preview = {pixels,
              pixelBytes,
              slot.metadata.width,
              slot.metadata.height,
              slot.metadata.Left(LOGICAL_IMAGE_WIDTH),
              slot.metadata.Top(LOGICAL_IMAGE_HEIGHT)};

    std::memset(pixels, 0, PreviewBytes);
    // Use a separate file handle and no playback JPEG index cache. Existing
    // playback position, selection, profiling and displayed pixels stay intact.
    read = {};
    if (slot.indexedJpeg) {
        read = MemoryStorage::ReadAndVerifyJpegFrame(slot, 0, MemoryStorage::PreviewSamplingContext::SamplePreviewPixels, &preview, true);
    } else {
        read = MemoryStorage::ReadAndVerifyRawVideoFrame(slot, 0, MemoryStorage::PreviewSamplingContext::SamplePreviewPixels, &preview, true);
    }
    closed = f_close(&MemoryStorage::managerState.imageFile);
    if (!read.complete || closed != FR_OK) {
        std::memset(pixels, 0, PreviewBytes);
        failure = {};
        if (read.complete) {
            failure = "preview close failed";
        } else {
            failure = read.error;
        }
        failureCode = {};
        if (read.result == FR_OK) {
            failureCode = closed;
        } else {
            failureCode = read.result;
        }
        return ReportStorageFailure(failure, failureCode);
    }

    error_ = "none";
    return true;
} // ReadLibraryPreview

/**
    Totals logical and allocated bytes in the controller's managed SD files.

    Totals only controller-managed media, settings and retained files, reporting logical lengths
    separately from cluster-rounded allocation. A bounded-memory directory traversal is
    synchronous, so this is heavier than a cached status read. The storage guard prevents
    concurrent file operations, and unavailable/busy storage returns failure rather than a
    misleading complete usage total.

    @param usage Output totals for the controller-managed SD files.

    @return true - The storage operation completed successfully.
    @return false - Input, storage availability, file I/O or verification prevented completion;
                    GetLastError() gives the reason.
*/
bool
MemoryManager::GetStorageUsage (
    StorageUsage& usage
    )
{
    StorageUsage measured;
    uint64_t clusterBytes;
    size_t nextPathIndex;
    const char* path = nullptr;
    size_t pathIndex;
    const char* currentPath = nullptr;
    int result;
    /**
        Adds one managed file to logical and allocated storage usage totals.

        Looks up one reserved controller file and adds its logical length and cluster-rounded
        allocation to the running usage snapshot. Missing files contribute nothing, but lookup
        errors or a directory in place of a managed file fail the overall measurement. The
        enclosing storage guard keeps this synchronous lookup serialized.

        @param path Storage path used by this operation.

        @return true - The managed file was counted, or it does not exist.
        @return false - The path names a directory or its metadata could not be read.
    */
    auto include = [&](const char* path) {
        FILINFO info;
        int result;

        info = {};
        result = f_stat(path, &info);
        if (result == FR_NO_FILE) {
            return true;
        }
        if (result != FR_OK) {
            return ReportStorageFailure("storage usage lookup failed", result);
        }
        if (info.fattrib & AM_DIR) {
            return false;
        }
        measured.fileBytes += info.fsize;
        measured.allocatedBytes += (uint64_t(info.fsize) + clusterBytes - 1) / clusterBytes * clusterBytes;
        return true;
    };

    usage = {};

    // Claim the shared storage buffer before filesystem access or user callbacks.
    MemoryStorage::StorageGuard guard;
    if (!guard.acquired || !ready_ || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle || !MemoryStorage::managerState.filesystem.csize ||
        MemoryStorage::managerState.filesystem.n_fatent < FAT_RESERVED_CLUSTER_COUNT) {
        return false;
    }
    measured = {};
    clusterBytes = uint64_t(MemoryStorage::managerState.filesystem.csize) * SD_SECTOR_BYTES_U;
    measured.totalBytes = uint64_t(MemoryStorage::managerState.filesystem.n_fatent - FAT_RESERVED_CLUSTER_COUNT) * clusterBytes;

    for (nextPathIndex = 0; nextPathIndex < SAVED_MEDIA_SLOT_COUNT; ++nextPathIndex) {
        path = MemoryStorage::GetSavedMediaPath(unsigned(nextPathIndex));
        if (!include(path)) {
            return false;
        }
    }

    for (pathIndex = 0; pathIndex < sizeof(MemoryStorage::GalleryPaths) / sizeof(MemoryStorage::GalleryPaths[0]); ++pathIndex) {
        currentPath = MemoryStorage::GalleryPaths[pathIndex];
        if (!include(currentPath)) {
            return false;
        }
    }

    if (!include("0:/VLIB.H75")) {
        return false;
    }
    result = MemoryStorage::MeasureLibraryStorageUsage(measured, clusterBytes);

    if (result != FR_OK) {
        return ReportStorageFailure("storage usage directory failed", result);
    }
    usage = measured;
    return true;
} // GetStorageUsage

/**
    Returns the readable SD lifecycle state name.

    @return "mounting" - A mount attempt is in progress.
    @return "ready" - Mounted SD storage is available for operations.
    @return "recovering" - The failed SD session is being invalidated.
    @return "unavailable" - No usable card is mounted.
*/
const char*
MemoryManager::GetStateName (
    void
    ) const
{
    switch (state_) {
    case State::Mounting:
        return "mounting";
    case State::Ready:
        return "ready";
    case State::Recovering:
        return "recovering";
    default:
        return "unavailable";
    }
} // GetStateName

/**
    Returns the remaining delay before the next SD mount attempt.

    @param nowMs Current monotonic time since boot, in milliseconds.

    @return value - Milliseconds remaining before another automatic mount attempt.
    @return 0 - No retry is pending or the retry deadline has arrived.
*/
uint32_t
MemoryManager::GetRetryInMs (
    uint32_t nowMs
    ) const
{
    uint32_t elapsed;

    if (suspended_ || state_ != State::Unavailable) {
        return 0;
    }
    elapsed = nowMs - lastAttemptMs_;
    if (elapsed >= RetryIntervalMs) {
        return 0;
    } else {
        return RetryIntervalMs - elapsed;
    }
} // GetRetryInMs

/**
    Advances SD health checks, failure recovery, and periodic mount retries.

    Foreground only. Shutdown explicitly suspends storage retries; Initialize resumes monitoring
    and makes one immediate mount attempt.

    @param nowMs Current monotonic time since boot, in milliseconds.

    @return Event::None - No availability transition occurred.
    @return Event::Unavailable - The current storage session failed and retries were scheduled.
    @return Event::Ready - A card was mounted and storage became available.
*/
MemoryManager::Event
MemoryManager::Update (
    uint32_t nowMs
    )
{
    if (suspended_) {
        return Event::None;
    }

    // Claim the shared storage buffer before filesystem access or user callbacks.
    if (state_ == State::Recovering) {
        MemoryStorage::StorageGuard guard;
        if (!guard.acquired) {
            return Event::None;
        }
        DiscardSession();
        state_ = State::Unavailable;
        lastAttemptMs_ = nowMs;
        std::printf("[sd] -> unavailable retry=%lu ms\n", static_cast<unsigned long>(RetryIntervalMs));
        return Event::Unavailable;
    }
    if (state_ == State::Unavailable) {
        if (GetRetryInMs(nowMs)) {
            return Event::None;
        }
        if (Initialize()) {
            return Event::Ready;
        } else {
            return Event::None;
        }
    }
    if (state_ == State::Ready && nowMs - lastHealthMs_ >= HealthIntervalMs) {
        MemoryStorage::StorageGuard guard;
        if (!guard.acquired || MemoryStorage::managerState.save.phase != MemoryStorage::SavePhase::Idle) {
            return Event::None;
        }
        // Bypass FatFs/prefetch caches: one read-only physical sector confirms
        // idle cards are still responding, even when a static photo is in RAM.
        lastHealthMs_ = nowMs;
        if (disk_read(0, MemoryStorage::readBuffer, 0, 1) != RES_OK) {
            ReportStorageFailure("SD health read failed", FR_DISK_ERR);
        }
    }
    return Event::None;
} // Update

/**
    Returns the current SD lifecycle state.

    @return State::Unavailable - No usable mounted card is available.
    @return State::Mounting - A mount attempt is in progress.
    @return State::Ready - The mounted card is available.
    @return State::Recovering - The failed session is being invalidated before retrying.
*/
MemoryManager::State
MemoryManager::GetState (
    void
    ) const
{
    return state_;
} // GetState

/**
    Returns the most recent SD availability error.

    @return text - Borrowed null-terminated result text; do not free it.
*/
const char*
MemoryManager::GetStorageError (
    void
    ) const
{
    return storageError_;
} // GetStorageError

/**
    Returns the generation identifying the current mounted storage session.

    @return value - Generation identifying the current mount; a changed value invalidates cached
                    handles.
*/
uint32_t
MemoryManager::GetStorageGeneration (
    void
    ) const
{
    return generation_;
} // GetStorageGeneration

/**
    Releases failed-session resources before a fresh SD mount attempt.

    Drops save and playback handles without closing or flushing them. A reinserted card
    must never receive dirty writes from the previous session. Unmounting invalidates
    FatFs objects, clears frame/index/prefetch state and releases only the SD hardware.
    The next mount starts a fresh storage generation.
*/
void
MemoryManager::DiscardSession (
    void
    )
{
    MemoryStorage::managerState.save = {};
    MemoryStorage::managerState.playback = {};
    MemoryStorage::managerState.mediaFile = {};
    MemoryStorage::managerState.playbackFile = {};
    MemoryStorage::managerState.imageFile = {};
    MemoryStorage::managerState.jpegIndexCount = 0;
    MemoryStorage::managerState.hasPlaybackFrame = false;
    MemoryStorage::managerState.rejectedSlot = SAVED_PHOTO_NO_SLOT;
    MemoryStorage::playbackFile = nullptr;
    MemoryStorage::playbackPrefetch.Reset();
    f_mount(nullptr, "0:", 0);
    MemoryStorage::managerState.filesystem = {};
    ReleaseMemoryStorageHardware();
    ready_ = false;
} // DiscardSession

/**
    Returns current controller RAM capacity and usage figures.

    Read-only reference figures. RAM includes live heap allocations and stack reservations.

    @return usage - RAM capacity and allocation counters in bytes; available is false when
                    runtime figures cannot be obtained.
*/
MemoryManager::RamUsage
MemoryManager::GetRamUsage (
    void
    ) const
{
    return GetMemoryRuntimeRamUsage();
} // GetRamUsage

/**
    Returns total RAM used by static data, heap, and reserved stacks.

    @return value - RAM occupied by static data, heap and reserved stacks, in bytes.
*/
uint64_t
MemoryManager::RamUsage::UsedBytes (
    void
    ) const
{
    return staticBytes + heapBytes + stackReservedBytes;
} // UsedBytes

/**
    Returns RAM remaining after accounted allocations and stack reservations.

    @return value - RAM remaining after the accounted allocations and stack reservations, in
                    bytes.
*/
uint64_t
MemoryManager::RamUsage::FreeBytes (
    void
    ) const
{
    return totalBytes - UsedBytes();
} // FreeBytes

/**
    Returns the readable name of the current storage activity.

    @return "receiving" - A media upload is receiving payload bytes.
    @return "verifying" - Saved payload bytes are being checked.
    @return "idle" - No media save is in progress.
*/
const char*
MemoryManager::GetActivityName (
    void
    ) const
{
    if (MemoryStorage::managerState.save.phase == MemoryStorage::SavePhase::Writing) {
        return "receiving";
    } else {
        if (MemoryStorage::managerState.save.phase == MemoryStorage::SavePhase::Verifying) {
            return "verifying";
        } else {
            return "idle";
        }
    }
} // GetActivityName
