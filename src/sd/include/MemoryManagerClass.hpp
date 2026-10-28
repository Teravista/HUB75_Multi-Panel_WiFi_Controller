#pragma once

#include <cstddef>
#include <cstdint>
#include "config/include/storageConfig.hpp"
#include "config/defines.h"
#include "core/include/serialLog.hpp"
#include "media/include/mediaMetadata.hpp"
#include "media/include/mediaTransfer.hpp"

/**
    Provides the foreground SD storage boundary for saved photos and raw/indexed-JPEG videos.

    It owns SPI, the filesystem, file handles, shared 24 KiB scratch and a 64 KiB
    active-playback cache. Mount retries and read-only health checks recover
    unavailable cards without restarting Wi-Fi. JPEG reads can feed an available Core 1 decoder through
    bounded input slots; all filesystem operations remain on Core 0.
    Independent library items and alternating retained generations use checked
    metadata and CRCs. Bounded directory pages, thumbnails and persistent selection
    settings share this storage boundary. Video transactions verify segments without keeping
    the clip in RAM. Playback streams one verified frame at a time. A non-waiting
    guard protects files and scratch storage from reentrant access, while the
    previous valid generation remains available during replacement. Consumers
    publish pixel fragments only after their complete read succeeds.
*/
class MemoryManager {
public:
    /**
        Carries RAM accounting used by controller status and web memory reports.

        Capacity, static allocations, heap use and reserved stacks are kept separately so
        consumers can derive consistent used/free totals from one snapshot. The availability flag
        distinguishes a valid measurement from unsupported runtime accounting; the record does not
        inspect or own the allocations it describes.
    */
    struct RamUsage {
        bool available = false;
        uint64_t totalBytes = 0;
        uint64_t staticBytes = 0;
        uint64_t heapBytes = 0;
        uint64_t stackReservedBytes = 0;

        // Returns total RAM used by static data, heap, and reserved stacks.
        uint64_t UsedBytes(void) const;

        // Returns RAM remaining after accounted allocations and stack reservations.
        uint64_t FreeBytes(void) const;

    };

    enum class State { Unavailable, Mounting, Ready, Recovering };
    enum class Event { None, Unavailable, Ready };
    static constexpr uint32_t RetryIntervalMs = 15000;
    static constexpr uint32_t HealthIntervalMs = 15000;

    // Returns current controller RAM capacity and usage figures.
    RamUsage GetRamUsage(void) const;

    // Returns the readable name of the current storage activity.
    const char* GetActivityName(void) const;

    // Advances SD health checks, failure recovery, and periodic mount retries.
    Event Update(uint32_t nowMs);

    // Returns the current SD lifecycle state.
    State GetState(void) const;

    // Returns the readable SD lifecycle state name.
    const char* GetStateName(void) const;

    // Returns the most recent SD availability error.
    const char* GetStorageError(void) const;

    // Returns the remaining delay before the next SD mount attempt.
    uint32_t GetRetryInMs(uint32_t nowMs) const;

    // Returns the generation identifying the current mounted storage session.
    uint32_t GetStorageGeneration(void) const;

    static constexpr size_t IoChunkBytes = 24 * 1024;
    static constexpr size_t PrefetchBytes = 64 * 1024;
    // Pixel fragments are provisional until the complete read succeeds. JPEG
    // sinks may run on the reserved worker and must not perform storage I/O.
    using ImageSink = bool (*)(void* context, const uint8_t* pixels, size_t bytes, size_t offset);

    using ImageSource = bool (*)(void* context, uint8_t* pixels, size_t bytes, size_t offset);

    // Saves a retained RGB888 photo with the default full-panel descriptor.
    bool SaveLastImage(ImageSource source, void* context);

    // Saves a new retained photo while keeping the previous valid generation.
    bool SaveLastImage(const MediaMetadata& metadata, ImageSource source, void* context);

    // Restores the newest verified retained photo through a chunk sink.
    bool ReadLastImage(ImageSink sink, void* context, MediaMetadata* metadata = nullptr);

    // Enables storage monitoring and attempts to mount the SD card.
    bool Initialize(void);

    // Closes storage resources and suspends automatic mount retries.
    bool Shutdown(void);

    // Reports whether mounted SD storage is available for use.
    bool IsReady(void) const;

    // Returns the most recent storage operation error.
    const char* GetLastError(void) const;

    // Writes a new named photo without replacing an existing file.
    bool SaveImage(const char* name, const uint8_t* pixels, size_t bytes);

    // Streams a named RGB888 photo and validates its metadata and checksum.
    bool ReadImage(const char* name, ImageSink sink, void* context, MediaMetadata* metadata = nullptr);

    // Saves, rereads, and removes an unused temporary test image.
    bool RunReadWriteCheck(const uint8_t* pixels, size_t bytes);

    // Starts an unpublished raw or indexed-JPEG video save while retaining the previous valid slot.
    bool BeginMediaSave(const MediaMetadata& metadata, uint32_t payloadChecksum);

    // Adds received video bytes to the bounded storage write buffer.
    bool AppendMediaBytes(const uint8_t* bytes, size_t count);

    // Flushes the completed upload and begins incremental readback verification.
    bool StartMediaVerification(void);

    // Verifies the next saved segment and publishes only after complete success.
    bool VerifyMediaSaveStep(bool& complete);

    // Cancels an unpublished video save and releases its temporary resources.
    void AbortMediaSave(void);

    // Reports whether a video save owns the shared storage buffer.
    bool IsMediaSaveActive(void) const;

    // Opens the newest valid photo or video for checked frame playback.
    bool OpenLastMedia(MediaMetadata& metadata);

    struct FrameReadTiming {
        uint64_t seekUs = 0;
        uint64_t readUs = 0;
        uint64_t crcUs = 0;
        uint64_t consumerUs = 0;
        uint64_t totalUs = 0;
        uint64_t overlapWorkUs = 0;
        uint64_t decodeUs = 0;
        uint64_t encodedBytes = 0;
        uint64_t jpegInputWaitUs = 0;
        uint32_t jpegCore1Frames = 0;
        uint32_t jpegCore0Frames = 0;
        uint32_t indexCacheHits = 0;
        uint32_t indexCacheLoads = 0;
        uint32_t sequentialReads = 0;
        // Detailed decoder CPU stages sample one in 16 timed JPEG reads.
        uint32_t jpegSamples = 0;
        uint32_t jpegMcus = 0;
        uint32_t jpegIdctBlocks = 0;
        uint32_t jpegDcBlocks = 0;
        uint32_t jpegClockReads = 0;
        uint64_t sampledDecodeUs = 0;
        uint64_t jpegPrepareUs = 0;
        uint64_t jpegEntropyUs = 0;
        uint64_t jpegTransformUs = 0;
        uint64_t jpegColorUs = 0;
        uint64_t jpegOtherUs = 0;
        uint32_t overlapBytes = 0;
        uint64_t commandUs = 0;
        uint64_t tokenUs = 0;
        uint64_t transferUs = 0;
        uint64_t sectorCrcUs = 0;
        uint32_t transactions = 0;
        uint32_t sectors = 0;
        uint32_t readCalls = 0;
        uint64_t prefetchHitBytes = 0;
        uint64_t prefetchReadBytes = 0;
        uint32_t prefetchLoads = 0;
        uint32_t prefetchAheadLoads = 0;
        uint32_t prefetchFailures = 0;
    };

    // Reads and verifies one frame from the selected saved media.
    bool ReadMediaFrame(uint32_t frameIndex, ImageSink sink, void* context, FrameReadTiming* timing = nullptr, PixelFormat jpegOutput = PixelFormat::RGB888);

    // Closes the active playback file and clears its runtime state.
    void CloseMediaPlayback(void);

    static constexpr size_t LibraryPageSize = 10;
    // 8.3 filenames provide a 28-bit ID namespace; card space/directory capacity
    // is reached long before this representation bound, not an item-slot limit.
    static constexpr uint32_t MaximumLibraryId = 0x0fffffff;

    struct LibraryPage {
        uint32_t total = 0;
        uint32_t nextAfter = 0;
        bool hasMore = false;
    };

    static constexpr size_t PhotoNameBytes = 64;
    static constexpr size_t ThumbnailBytes = 16 * 32 * 2;

    struct LibraryPhoto {
        uint32_t id = 0;
        uint32_t checksum = 0;
        bool valid = false;
        MediaMetadata metadata;
        uint64_t bytes = 0;
        char name[PhotoNameBytes]{};
    };

    struct LibraryVideo {
        uint32_t id = 0;
        uint32_t checksum = 0;
        uint32_t maxSecondsMillis = 0;
        bool valid = false;
        MediaMetadata metadata;
        uint64_t bytes = 0;
        char name[PhotoNameBytes]{};
    };

    static constexpr uint32_t DefaultBrightness = 80;

    struct GallerySettings {
        uint32_t selectedPhoto = 0;
        uint32_t intervalSeconds = DEFAULT_SLIDESHOW_INTERVAL_SECONDS;
        bool slideshow = false;
        uint32_t selectedVideo = 0;
        uint32_t maxFps = 0;
        uint32_t brightness = DefaultBrightness;
    };


    // Starts a named photo save with its own metadata and thumbnail.
    bool BeginLibraryPhoto(const MediaMetadata& metadata, uint32_t checksum, const char* name, const uint8_t* thumbnail, uint32_t& id);

    // Lists one bounded page of saved photos and their metadata.
    bool ListLibraryPhotos(LibraryPhoto* photos, size_t capacity, size_t& count, uint32_t after = 0, LibraryPage* page = nullptr);

    // Reads and validates metadata for one saved photo ID.
    bool GetLibraryPhoto(uint32_t id, LibraryPhoto& photo);

    // Reads the stored thumbnail belonging to one photo ID.
    bool ReadLibraryThumbnail(uint32_t id, uint8_t* thumbnail);

    // Opens a named saved photo for framebuffer restoration.
    bool OpenLibraryPhoto(uint32_t id, MediaMetadata& metadata);

    // Deletes the files belonging to one saved photo ID.
    bool DeleteLibraryPhoto(uint32_t id);

    // Reads the newest valid library selection and display settings.
    bool ReadGallerySettings(GallerySettings& settings);

    // Saves library selection and display settings transactionally.
    bool SaveGallerySettings(const GallerySettings& settings);

    // Saves the current framebuffer as a new named library photo.
    bool SaveLibraryImage(ImageSource source, void* context, uint32_t& id);

    // Starts a named video save using a newly allocated library ID.
    bool BeginLibraryVideo(const MediaMetadata& metadata, uint32_t checksum, const char* name, uint32_t maxSecondsMillis, uint32_t& id);

    // Lists one bounded page of saved videos and their metadata.
    bool ListLibraryVideos(LibraryVideo* videos, size_t capacity, size_t& count, uint32_t after = 0, LibraryPage* page = nullptr);

    // Reads and validates metadata for one saved video ID.
    bool GetLibraryVideo(uint32_t id, LibraryVideo& video);

    // Opens a named saved video for frame playback.
    bool OpenLibraryVideo(uint32_t id, MediaMetadata& metadata);

    // Deletes the files belonging to one saved video ID.
    bool DeleteLibraryVideo(uint32_t id);

    // Adopts the previous firmware's saved video without copying its payload.
    bool ImportLastVideo(uint32_t& id);

    struct StorageUsage {
        uint64_t totalBytes = 0;
        uint64_t fileBytes = 0;
        uint64_t allocatedBytes = 0;
    };

    // Totals logical and allocated bytes in the controller's managed SD files.
    bool GetStorageUsage(StorageUsage& usage);

    static constexpr size_t PreviewWidth = 32;
    static constexpr size_t PreviewHeight = 64;
    static constexpr size_t PreviewBytes = PreviewWidth * PreviewHeight * RGB565_PIXEL_BYTES;

    // Builds a verified first-frame preview without replacing active playback.
    bool ReadLibraryPreview(uint32_t id, bool video, uint8_t* pixels, size_t capacity);

private:
    SerialLog::RepeatGate errorLog_;

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
    static int MeasureLibraryStorageUsage(StorageUsage& usage, uint64_t clusterBytes);

    static constexpr size_t HeaderBytes = PHOTO_HEADER_BYTES;
    static constexpr size_t LegacyLastHeaderBytes = SAVED_PHOTO_HEADER_BYTES;
    static constexpr size_t LastHeaderBytes = METADATA_PHOTO_HEADER_BYTES;
    static constexpr size_t ChunkBytes = IoChunkBytes;
    static constexpr size_t MaximumNameLength = STORAGE_MAXIMUM_NAME_LENGTH;
    static_assert(ChunkBytes % SD_SECTOR_BYTES == 0 && ChunkBytes % IMAGE_BYTES_PER_PIXEL == 0,
                  "Storage chunks must contain complete sectors and RGB pixels");

    // Private static state keeps FatFs objects and the read buffer off the stack.
    struct StorageState;
    static StorageState storage_;

    /**
        Guards the class-owned filesystem and scratch buffer for one operation.

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
    static bool BuildValidatedImagePath(const char* name, char* path);

    // Updates the running CRC32 checksum with another byte span.
    static uint32_t UpdateImageCrc32(uint32_t crc, const uint8_t* bytes, size_t length);

    // Writes a 32-bit unsigned value in little-endian byte order.
    static void WriteUnsigned32LittleEndian(uint8_t* p, uint32_t value);

    // Reads a 32-bit unsigned value in little-endian byte order.
    static uint32_t ReadUnsigned32LittleEndian(const uint8_t* p);

    // Builds a photo storage header with dimensions and payload checksum.
    static void BuildPhotoStorageHeader(uint8_t* header, uint32_t checksum);

    // Compares readback pixels against the source used by the storage self-check.
    static bool CompareSavedImageBytes(void* context, const uint8_t* bytes, size_t count, size_t offset);

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
    static void BuildSavedPhotoHeader(uint8_t* header, const MediaMetadata& metadata, uint32_t generation, uint32_t checksum);

    // Validates a stored photo while streaming provisional pixel data.
    static SavedPhotoSlot ReadAndValidatePhotoFile(const char* path, ImageSink sink, void* context);

    // Reads and validates one retained photo, optionally delivering its pixels.
    static SavedPhotoSlot ReadSavedPhotoSlot(unsigned index, ImageSink sink = nullptr, void* context = nullptr);

    // Selects the newest valid generation with wrap-safe ordering.
    static int FindNewestSavedPhoto(const SavedPhotoSlot* slots);

    // Streams a new retained photo into the selected alternate slot.
    static bool WriteSavedPhotoSlot(unsigned index, uint32_t generation, const MediaMetadata& metadata, ImageSource source, void* context, int& result);

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
        char name[PhotoNameBytes]{};
        uint8_t thumbnail[ThumbnailBytes]{};
    };

    struct PlaybackSession {
        SavedPhotoSlot description;
        int slot = SAVED_PHOTO_NO_SLOT;
        uint32_t libraryVideoId = 0;
        bool open = false;
    };

    struct FrameResult {
        bool complete = false;
        int result = 0;
        const char* error = "media frame read failed";
    };

    // Returns the reserved path for one alternating media slot.
    static const char* GetSavedMediaPath(unsigned index);

    // Flushes buffered transaction bytes to the active save file.
    static bool FlushMediaWrites(int& result);

    // Queues video payload and checksum records through the shared buffer.
    static bool QueueMediaWrite(const uint8_t* bytes, size_t count, int& result);

    // Closes playback without acquiring a second storage guard.
    static void CloseSavedMediaPlayback(bool safe = true);

    // Discards an unpublished save without reacquiring storage ownership.
    static int DiscardIncompleteMediaSave(bool safe);

    // Builds the checked raw or indexed-JPEG video header for the current save.
    static void BuildVideoStorageHeader(uint8_t* header);

    // Inspects a photo, raw video or indexed JPEG video in a retained slot.
    static SavedPhotoSlot InspectSavedMediaSlot(unsigned index, const char* path = nullptr, bool checkFirstFrame = true);

    // Streams and checks a raw frame from the inspection or playback handle.
    static FrameResult ReadAndVerifyRawVideoFrame(const SavedPhotoSlot& slot, uint32_t index, ImageSink sink, void* context, bool inspecting, FrameReadTiming* timing = nullptr);

    struct JpegIndexEntry {
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    // Loads and checks a bounded index window for JPEG playback.
    static bool LoadJpegIndexWindow(const SavedPhotoSlot& slot, uint32_t index, int& result);

    // Validates an indexed JPEG record before completing provisional frame output.
    static FrameResult ReadAndVerifyJpegFrame(const SavedPhotoSlot& slot, uint32_t index, ImageSink sink, void* context, bool inspecting, bool verifying = false, FrameReadTiming* timing = nullptr, bool outputRGB565 = false);

    // Checks one bounded JPEG payload block or one complete encoded frame.
    bool VerifyJpegSaveStep(bool& finished);

    static constexpr size_t LibraryHeaderBytes = 1536;
    static constexpr size_t VideoLibraryHeaderBytes = 512;
    static constexpr const char* GalleryPaths[] = {"0:/GSET_A.H75", "0:/GSET_B.H75"};

    struct GalleryRecord {
        bool valid = false;
        uint32_t generation = 0;
        int result = 0;
        GallerySettings settings;
    };

    // Builds a managed video filename from its library ID and extension.
    static void BuildVideoLibraryPath(uint32_t id, char* path, bool temporary = false);

    // Builds a managed photo filename from its library ID and extension.
    static void BuildPhotoLibraryPath(uint32_t id, char* path, bool temporary = false);

    // Parses a managed library filename into its media ID and file type.
    static bool ParseLibraryName(const char* name, uint32_t& id, bool& video, bool& temporary);

    // Visits managed library entries without retaining the full directory.
    template <class Visitor>
    static int ScanLibraryDirectory(Visitor visit);

    // Chooses the next unused ID for a named library item.
    static uint32_t AllocateLibraryId(bool video, int& result);

    // Selects a bounded ordered page of library IDs after the requested cursor.
    template <class Item>
    static int FindLibraryPage(bool video, Item* items, size_t capacity, size_t& count, uint32_t after, MemoryManager::LibraryPage& page);

    // Selects a bounded ordered page of photo IDs after the requested cursor.
    static int FindPhotoLibraryPage(MemoryManager::LibraryPhoto* items, size_t capacity, size_t& count, uint32_t after, MemoryManager::LibraryPage& page);

    // Selects a bounded ordered page of video IDs after the requested cursor.
    static int FindVideoLibraryPage(MemoryManager::LibraryVideo* items, size_t capacity, size_t& count, uint32_t after, MemoryManager::LibraryPage& page);

    // Checks whether a published video is already present in the library.
    static int FindExistingLibraryVideo(bool& existingVideo);

    // Validates a named photo's header and reads its library metadata.
    static SavedPhotoSlot InspectPhotoLibraryFile(uint32_t id, MemoryManager::LibraryPhoto* photo = nullptr, uint8_t* thumbnail = nullptr);

    // Reads and validates one saved gallery settings record.
    static GalleryRecord ReadAndValidateGalleryRecord(unsigned index);

    // Selects the newest valid saved gallery settings record.
    static int FindNewestValidGalleryRecord(const GalleryRecord* records);

    // Validates a named video's header and reads its library metadata.
    static SavedPhotoSlot InspectLibraryVideo(uint32_t id, MemoryManager::LibraryVideo* video = nullptr);

    // Selects the path owned by the current save transaction.
    static const char* BuildActiveSaveFilePath(char* path);

    // Identifies filesystem failures that require SD session recovery.
    static bool IsStorageIoError(int result);

    // Builds the checked retained or named-library header after complete verification.
    static void BuildLibraryPublicationHeader(uint8_t* header);

    // Releases failed-session resources before a fresh SD mount attempt.
    void DiscardSession(void);

    State state_ = State::Unavailable;
    bool suspended_ = false;
    uint32_t lastAttemptMs_ = 0;
    uint32_t lastHealthMs_ = 0;
    uint32_t generation_ = 0;
    const char* storageError_ = "not initialized";
    bool ready_ = false;
    const char* error_ = "storage not initialized";

    // Records a storage error and requests recovery for failed SD I/O.
    bool ReportStorageFailure(const char* message, int filesystemResult = 0);

};
