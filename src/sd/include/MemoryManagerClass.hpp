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

    It coordinates SPI and private storage helpers holding the filesystem, file handles,
    shared 24 KiB scratch and a 32 KiB
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

    // Checks one bounded JPEG payload block or one complete encoded frame.
    bool VerifyJpegSaveStep(bool& finished);

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
