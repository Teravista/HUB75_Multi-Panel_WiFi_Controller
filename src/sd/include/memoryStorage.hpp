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
#include <atomic>
#include "diskio.h"
#include "sd/include/storageCrc32.hpp"
#include "pico/time.h"
#include <cstdio>

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

/**
    Samples a saved image into a small RGB565 preview without touching the CPU framebuffer.

    The reader supplies raw or decoded pixel fragments. Placement maps their logical
    cell centers into the caller-owned preview, whose remaining pixels stay black.
*/
struct PreviewSamplingContext {
    uint8_t* pixels;
    size_t pixelBytes;
    uint16_t width;
    uint16_t height;
    uint16_t left;
    uint16_t top;

    // Samples decoded image fragments into the small RGB565 library preview.
    static bool SamplePreviewPixels(void* opaque, const uint8_t* bytes, size_t count, size_t offset);

};

// Adds managed library files to logical and allocated storage totals.
int MeasureLibraryStorageUsage(MemoryManager::StorageUsage& usage, uint64_t clusterBytes);

inline constexpr size_t HeaderBytes = PHOTO_HEADER_BYTES;

inline constexpr size_t LegacyLastHeaderBytes = SAVED_PHOTO_HEADER_BYTES;

inline constexpr size_t LastHeaderBytes = METADATA_PHOTO_HEADER_BYTES;

inline constexpr size_t MaximumNameLength = STORAGE_MAXIMUM_NAME_LENGTH;

static_assert(ChunkBytes % SD_SECTOR_BYTES == 0 && ChunkBytes % IMAGE_BYTES_PER_PIXEL == 0,
              "Storage chunks must contain complete sectors and RGB pixels");

/**
    Guards the shared filesystem and scratch buffer for one operation.

    Acquisition reports a busy state immediately. Scope exit releases only
    a successful claim, preserving the foreground callback lifetime boundary.
*/
struct StorageGuard {
    bool acquired;

    // Attempts to claim the shared storage state without waiting.
    StorageGuard(void);

    // Releases the storage-operation guard when its scope ends.
    ~StorageGuard(void);

};

struct CompareContext {
    const uint8_t* expected;
    size_t compared = 0;
    bool mismatch = false;
};

// Validates a root filename and builds its SD filesystem path.
bool BuildValidatedImagePath(const char* name, char* path);

// Updates the running CRC32 checksum with another byte span.
uint32_t UpdateImageCrc32(uint32_t crc, const uint8_t* bytes, size_t length);

// Writes a 32-bit unsigned value in little-endian byte order.
void WriteUnsigned32LittleEndian(uint8_t* p, uint32_t value);

// Reads a 32-bit unsigned value in little-endian byte order.
uint32_t ReadUnsigned32LittleEndian(const uint8_t* p);

// Builds a photo storage header with dimensions and payload checksum.
void BuildPhotoStorageHeader(uint8_t* header, uint32_t checksum);

// Compares readback pixels against the source used by the storage self-check.
bool CompareSavedImageBytes(void* context, const uint8_t* bytes, size_t count, size_t offset);

enum class SlotState { Missing, Invalid, Valid, IoError };

struct SavedPhotoSlot {
    SlotState state = SlotState::Missing;
    uint32_t generation = 0;
    MediaMetadata metadata;
    size_t dataOffset = 0;
    uint32_t checksum = 0;
    uint32_t crcSeed = STORAGE_CRC32_INITIAL_U;
    bool frameChecksums = false;
    bool indexedJpeg = false;
    int result = 0;
    const char* error = "invalid saved photo";
};

// Builds a saved-photo header with its generation and checksum.
void BuildSavedPhotoHeader(uint8_t* header, const MediaMetadata& metadata, uint32_t generation, uint32_t checksum);

// Validates a stored photo while streaming provisional pixel data.
SavedPhotoSlot ReadAndValidatePhotoFile(const char* path, MemoryManager::ImageSink sink, void* context);

// Reads and validates one retained photo, optionally delivering its pixels.
SavedPhotoSlot ReadSavedPhotoSlot(unsigned index, MemoryManager::ImageSink sink = nullptr, void* context = nullptr);

// Selects the newest valid generation with wrap-safe ordering.
int FindNewestSavedPhoto(const SavedPhotoSlot* slots);

// Streams a new retained photo into the selected alternate slot.
bool WriteSavedPhotoSlot(unsigned index, uint32_t generation, const MediaMetadata& metadata, MemoryManager::ImageSource source, void* context, int& result);

enum class SavePhase { Idle, Writing, Verifying };

struct SaveSession {
    SavePhase phase = SavePhase::Idle;
    MediaMetadata metadata;
    unsigned target = 0;
    uint32_t generation = 0;
    uint32_t expectedCrc = 0;
    uint64_t total = 0;
    uint64_t received = 0;
    uint64_t verified = 0;
    uint32_t frameBytes = 0;
    uint32_t verifyFrameBytes = 0;
    uint32_t jpegVerifiedFrames = 0;
    uint32_t crc = STORAGE_CRC32_INITIAL_U;
    uint32_t frameCrc = STORAGE_CRC32_INITIAL_U;
    uint32_t verifyCrc = STORAGE_CRC32_INITIAL_U;
    uint32_t verifyFrameCrc = STORAGE_CRC32_INITIAL_U;
    size_t bufferedBytes = 0;
    bool open = false;
    uint32_t libraryId = 0;
    uint32_t videoId = 0;
    uint32_t maxSecondsMillis = 0;
    size_t headerBytes = MediaTransfer::FileHeaderBytes;
    char name[MemoryManager::PhotoNameBytes]{};
    uint8_t thumbnail[MemoryManager::ThumbnailBytes]{};
};

struct PlaybackSession {
    SavedPhotoSlot description;
    int slot = SAVED_PHOTO_NO_SLOT;
    uint32_t libraryVideoId = 0;
    bool open = false;
};

struct SavedFrameResult {
    bool complete = false;
    int result = 0;
    const char* error = "media frame read failed";
};

// Returns the reserved path for one alternating media slot.
const char* GetSavedMediaPath(unsigned index);

// Flushes buffered transaction bytes to the active save file.
bool FlushMediaWrites(int& result);

// Queues video payload and checksum records through the shared buffer.
bool QueueMediaWrite(const uint8_t* bytes, size_t count, int& result);

// Closes playback without acquiring a second storage guard.
void CloseSavedMediaPlayback(bool safe = true);

// Discards an unpublished save without reacquiring storage ownership.
int DiscardIncompleteMediaSave(bool safe);

// Builds the checked raw or indexed-JPEG video header for the current save.
void BuildVideoStorageHeader(uint8_t* header);

// Inspects a photo, raw video or indexed JPEG video in a retained slot.
SavedPhotoSlot InspectSavedMediaSlot(unsigned index, const char* path = nullptr, bool checkFirstFrame = true);

// Streams and checks a raw frame from the inspection or playback handle.
SavedFrameResult ReadAndVerifyRawVideoFrame(const SavedPhotoSlot& slot, uint32_t index, MemoryManager::ImageSink sink, void* context, bool inspecting, MemoryManager::FrameReadTiming* timing = nullptr);

struct JpegIndexEntry {
    uint32_t offset = 0;
    uint32_t length = 0;
};

// Loads and checks a bounded index window for JPEG playback.
bool LoadJpegIndexWindow(const SavedPhotoSlot& slot, uint32_t index, int& result);

// Validates an indexed JPEG record before completing provisional frame output.
SavedFrameResult ReadAndVerifyJpegFrame(const SavedPhotoSlot& slot, uint32_t index, MemoryManager::ImageSink sink, void* context, bool inspecting, bool verifying = false, MemoryManager::FrameReadTiming* timing = nullptr, bool outputRGB565 = false);

inline constexpr size_t LibraryHeaderBytes = 1536;

inline constexpr size_t VideoLibraryHeaderBytes = 512;

inline constexpr const char* GalleryPaths[] = {"0:/GSET_A.H75", "0:/GSET_B.H75"};

struct GalleryRecord {
    bool valid = false;
    uint32_t generation = 0;
    int result = 0;
    MemoryManager::GallerySettings settings;
};

// Builds a managed video filename from its library ID and extension.
void BuildVideoLibraryPath(uint32_t id, char* path, bool temporary = false);

// Builds a managed photo filename from its library ID and extension.
void BuildPhotoLibraryPath(uint32_t id, char* path, bool temporary = false);

// Parses a managed library filename into its media ID and file type.
bool ParseLibraryName(const char* name, uint32_t& id, bool& video, bool& temporary);

// Visits managed library entries without retaining the full directory.
template <class Visitor>
int ScanLibraryDirectory(Visitor visit);

// Chooses the next unused ID for a named library item.
uint32_t AllocateLibraryId(bool video, int& result);

// Selects a bounded ordered page of library IDs after the requested cursor.
template <class Item>
int FindLibraryPage(bool video, Item* items, size_t capacity, size_t& count, uint32_t after, MemoryManager::LibraryPage& page);

// Selects a bounded ordered page of photo IDs after the requested cursor.
int FindPhotoLibraryPage(MemoryManager::LibraryPhoto* items, size_t capacity, size_t& count, uint32_t after, MemoryManager::LibraryPage& page);

// Selects a bounded ordered page of video IDs after the requested cursor.
int FindVideoLibraryPage(MemoryManager::LibraryVideo* items, size_t capacity, size_t& count, uint32_t after, MemoryManager::LibraryPage& page);

// Checks whether a published video is already present in the library.
int FindExistingLibraryVideo(bool& existingVideo);

// Validates a named photo's header and reads its library metadata.
SavedPhotoSlot InspectPhotoLibraryFile(uint32_t id, MemoryManager::LibraryPhoto* photo = nullptr, uint8_t* thumbnail = nullptr);

// Reads and validates one saved gallery settings record.
GalleryRecord ReadAndValidateGalleryRecord(unsigned index);

// Selects the newest valid saved gallery settings record.
int FindNewestValidGalleryRecord(const GalleryRecord* records);

// Validates a named video's header and reads its library metadata.
SavedPhotoSlot InspectLibraryVideo(uint32_t id, MemoryManager::LibraryVideo* video = nullptr);

// Selects the path owned by the current save transaction.
const char* BuildActiveSaveFilePath(char* path);

// Identifies filesystem failures that require SD session recovery.
bool IsStorageIoError(int result);

// Builds the checked retained or named-library header after complete verification.
void BuildLibraryPublicationHeader(uint8_t* header);

/**
    Holds the single storage session's filesystem, file and bounded read buffer.

    Static module-owned storage preserves one SD hardware owner and avoids placing
    FatFs objects or the 24 KiB scratch allocation on the small foreground stack.
*/
struct StorageState {
    FATFS filesystem{};
    FIL imageFile{};
    FIL mediaFile{};
    FIL playbackFile{};
    SaveSession save;
    PlaybackSession playback;
    uint32_t lastPlaybackFrame = 0;
    bool hasPlaybackFrame = false;
    JpegIndexEntry jpegIndex[JPEG_INDEX_CACHE_ENTRIES];
    uint32_t jpegIndexFirst = 0;
    uint32_t jpegIndexCount = 0;
    int rejectedSlot = SAVED_PHOTO_NO_SLOT;
    uint8_t* readBuffer = MemoryStorage::readBuffer;
    std::atomic_flag storageBusy = ATOMIC_FLAG_INIT;
};

extern StorageState managerState;

} // namespace MemoryStorage
